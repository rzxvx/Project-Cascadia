// SPDX-License-Identifier: GPL-2.0-only
/*
 * The iPad 2's cameras (K93): the camera ISP of the Apple A5 (S5L8940X),
 * "H4", as two V4L2 capture devices, the rear camera and the front one.
 *
 * The ISP is a computer of its own -- an ARM core at 205 MHz with the
 * sensors' I2C, a MIPI receiver a camera and the image pipeline -- that runs
 * Apple's firmware (iOS 8.4.1's, out of H4ISPServices) behind an IOMMU, the
 * "dart-rt".  The firmware finds the sensors, runs their exposure and white
 * balance and writes finished YUYV frames into the host's buffers.  The host
 * powers it, maps its memory, starts its CPU and then talks to it through
 * rings of 64-byte entries in shared memory, all as iOS's AppleH4CamIn and
 * AppleS5L8930XDART do it: docs/research/k93-camera.md has the protocol,
 * tools/camera/k93isp.c runs the same steps from user space.
 *
 * What the ISP sees through its IOMMU (all four streams the same, 256 MB):
 *
 *   0x0000000  the firmware's main memory, its image at 0       2 MB
 *   0x0200000  the shared region: the rings, their descriptor,
 *              the command packet                               64 KB
 *   0x0400000  the heap the firmware asks for                   up to 4 MB
 *   elsewhere  allocated as needed: the firmware's SHAREDMALLOC
 *              requests, the set-files, the buffer messages, and
 *              every frame buffer (vb2 scatter-gather pages, mapped
 *              page by page)
 *
 * All of it but the frame buffers is coherent DMA memory, which the host
 * reads and writes uncached, as the firmware expects it.
 *
 * The ISP is off until a camera streams: streaming boots it -- power, reset,
 * the sensors' supply, clocks and enables, the IOMMU, the firmware and its
 * handshake, the set-files -- and stopping powers it all down again and frees
 * its memory.  One camera streams at a time.
 *
 * Its firmware is Apple's, so it is not in the kernel: apple/h4isp-firmware.bin
 * (H4ISPServices' H4ISPFIRMWARE section) and, a sensor, its set-file
 * apple/h4isp-setfile-<the sensor's ID in hex>.bin (from the H4ISPSETFILE
 * section) -- ./cascadia k93 firmware takes them out of the IPSW.
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/genalloc.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-sg.h>
#include <media/videobuf2-v4l2.h>

#define H4ISP_FIRMWARE		"apple/h4isp-firmware.bin"

/* the ISP's registers */
#define ISP_ID			0x40000		/* 0x00010000 once powered */
#define ISP_RUN			0x40004		/* 1: its CPU runs */
#define ISP_BOOT		0x40008		/* its firmware's address | BIT(31) */
#define ISP_IRQ_STATUS		0x41000
#define ISP_IRQ_ENABLE		0x41004
#define ISP_DOORBELL		0x41020		/* BIT(a ring's source + 4) */
#define ISP_IRQ_CLEAR		0x41024
#define ISP_RX_ERROR(n)		(0x42000 + 8 * (n))	/* receiver n's error flags */
#define ISP_MIPI(n)		(0xc0000 + 4 * (n))
#define ISP_MBOX(n)		(0xc3000 + 4 * (n))	/* eight words, both ways */

#define ISP_IRQ_RINGS		0xf0	/* the firmware posted on source 0-3 */
#define ISP_IRQ_RX		0x08	/* a receiver's error */
#define ISP_MIPI_CONFIG		0x1201	/* iOS's, for both sensors */
#define ISP_READY		0x8042006

/* its IOMMU's (AppleS5L8930XDART's names) */
#define DART_TLB_OP		0x00
#define  DART_TLB_BUSY		BIT(3)
#define  DART_TLB_FLUSH_ALL	0xfffff02
#define  DART_STE_WRITE(s, i)	((s) << 8 | (i) << 22 | 5)
#define DART_STE		0x08
#define DART_CONFIG		0x0c
#define  DART_CONFIG_ISP	0xbb808080	/* iOS's: streams 0-2 translated, 3 no-fault */
#define DART_ERROR		0x10
#define  DART_ERROR_FAULT	BIT(31)
#define  DART_ERROR_WRITE	BIT(9)
#define  DART_ERROR_CLEAR	0x3f
#define DART_ERROR_ADDR		0x14
#define DART_DIAG		0x18	/* where an invalid translation goes */
#define DART_PERF		0x1000
#define DART_TLB_MISS		0x1004
#define DART_TLB_WAIT		0x1008
#define DART_TLB_HIT		0x100c
#define DART_STREAMS		4
#define DART_TABLES		64	/* a 4 KB table of 4 KB pages each: 4 MB */
#define DART_PTE_VALID		BIT(0)

/* the ISP's address space */
#define VA_FW			0x0
#define FW_SIZE			SZ_2M
#define VA_SHARED		FW_SIZE
#define SHARED_SIZE		SZ_64K
#define SHARED_CMD		0xc000		/* the command packet in it */
#define VA_HEAP			SZ_4M
#define HEAP_MAX		SZ_4M
#define VA_END			(DART_TABLES << 22)	/* the firmware's window: 256 MB */
#define VA_ANY			(~0u)

/* the rings */
#define RING_ENTRY		0x40
#define RING_HOST		0	/* the host posts, the firmware hands back */
#define RING_FW			1	/* the firmware posts, the host answers */
#define RING_TERMINAL		2	/* the firmware posts, the host releases */
#define MAX_RINGS		16
#define MSG_SIZE		0x180	/* a buffer message */
#define MSG_BUFS		9	/* buffers a message at most */
#define MSG_MAGIC		0x30000000	/* iOS's third word for one */

/* commands (CISP_CMD_*, the firmware's own names) and their packets */
#define CMD_START				0x0000
#define CMD_STOP				0x0001
#define CMD_PRINT_ENABLE			0x0004
#define CMD_CH_START				0x0100
#define CMD_CH_STOP				0x0101
#define CMD_CH_BUFFER_RETURN			0x0104
#define CMD_CH_CAMERA_CONFIG_GET		0x0106
#define CMD_CH_CAMERA_CONFIG_SELECT		0x0107
#define CMD_CH_INFO_GET				0x010d
#define CMD_CH_BUFFER_RECYCLE_MODE_SET		0x010e
#define CMD_CH_BUFFER_RECYCLE_START		0x010f
#define CMD_CH_SET_FILE_LOAD			0x0111
#define CMD_CH_AE_FRAME_RATE_MIN_GET		0x0209
#define CMD_CH_AE_FRAME_RATE_MIN_SET		0x020a
#define CMD_CH_CROP_SET				0x0801
#define CMD_CH_OUTPUT_CONFIG_SET		0x0b01
#define CMD_WORDS		(0xa8 / 4)	/* the longest packet sent */

#define NOTE_FRAME_TIMEOUT	0xe00
#define NOTE_FOCUS_FAILED	0xe01

#define H4ISP_CAMS		2
#define H4ISP_MAX_BUFS		VB2_MAX_FRAME	/* vb2's least: buffer indexes */
#define H4ISP_BUFS		8	/* what a REQBUFS gets at most */
#define H4ISP_TAG(cam, idx)	(0x4000 | (cam) << 8 | (idx))

static bool fw_log;
module_param(fw_log, bool, 0644);
MODULE_PARM_DESC(fw_log, "print the ISP firmware's own log (it is sent to the debug log otherwise)");

/* a size a camera offers: a centred cut of one of its sensor's modes */
struct h4isp_size {
	u32 width;
	u32 height;
	u8 mode;
	u16 fps256;		/* the mode's rate, as the firmware gives it: fps * 256 */
};

struct h4isp_cam_desc {
	const char *name;
	const char *card;
	u32 orientation;
	const struct h4isp_size *sizes;
	unsigned int nsizes;
};

/* the modes are the firmware's (CH_CAMERA_CONFIG_GET), for the K93's sensors */
static const struct h4isp_size h4isp_rear_sizes[] = {
	{ 1280, 720, 0, 0x1df8 },	/* of 1292x728, 29.97 fps */
	{ 1280, 800, 1, 0x1b8f },	/* of 1292x808, 27.56 fps */
	{ 640, 360, 2, 0x1df8 },	/* 640x360, 29.97 fps */
};

static const struct h4isp_size h4isp_front_sizes[] = {
	{ 640, 480, 0, 0x1df5 },	/* of 656x488, 29.96 fps */
};

static const struct h4isp_cam_desc h4isp_cam_descs[H4ISP_CAMS] = {
	{ "rear", "Rear camera", V4L2_CAMERA_ORIENTATION_BACK,
	  h4isp_rear_sizes, ARRAY_SIZE(h4isp_rear_sizes) },
	{ "front", "Front camera", V4L2_CAMERA_ORIENTATION_FRONT,
	  h4isp_front_sizes, ARRAY_SIZE(h4isp_front_sizes) },
};

/* memory the ISP sees: coherent, at one address of its own */
struct h4isp_mem {
	struct list_head list;
	void *cpu;
	dma_addr_t dma;
	u32 va;
	u32 size;
	bool firmware;		/* a SHAREDMALLOC answer, the firmware's to free */
};

struct h4isp_ring {
	char name[17];
	u32 type;
	u32 source;
	u32 n;
	u32 *q;
	u32 idx;		/* the next entry to read (theirs) or to post (ours) */
};

struct h4isp_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	u32 va;			/* where the ISP sees it */
	u32 size;
};

struct h4isp;

struct h4isp_cam {
	struct h4isp *isp;
	unsigned int ch;	/* the firmware's channel */
	const struct h4isp_cam_desc *desc;
	struct video_device vdev;
	struct vb2_queue queue;
	struct v4l2_ctrl_handler ctrls;
	struct mutex lock;	/* the node's and its queue's */
	struct v4l2_pix_format fmt;
	const struct h4isp_size *size;

	/* under the ISP's ring_lock */
	struct list_head pending;	/* queued, not yet with the firmware */
	struct h4isp_buf *at_fw[H4ISP_MAX_BUFS];
	unsigned int nat_fw;
	bool running;		/* buffers go to the firmware as they come */
	u32 pool;		/* the picture pool the firmware fills */
	u32 sequence;

	int ae_priority;	/* V4L2_CID_EXPOSURE_AUTO_PRIORITY */
	u32 ae_min;		/* the firmware's own floor for the frame rate, fps * 256 */
};

struct h4isp {
	struct device *dev;
	void __iomem *regs;
	void __iomem *dart;
	void __iomem *ps;
	int irq;
	int dart_irq;
	struct clk *clk;
	struct regulator *vdd;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_active;
	struct pinctrl_state *pins_idle;
	struct gpio_desc *enable[H4ISP_CAMS];
	u32 sensor_type;

	/* the IOMMU: its page tables, one after the other, and its dummy page */
	u32 *pt;
	dma_addr_t pt_dma;
	dma_addr_t dummy_dma;
	void *dummy;
	struct gen_pool *va;
	spinlock_t dart_lock;
	bool on;		/* powered, the IOMMU programmed; under dart_lock */

	struct mutex lock;	/* powering up and down, which camera streams */
	struct h4isp_cam *streaming;
	bool irq_enabled;
	bool dart_irq_enabled;

	struct mutex mem_lock;
	struct list_head mems;
	struct h4isp_mem *fw;
	struct h4isp_mem *shared;
	struct h4isp_mem *heap;
	struct h4isp_mem *msgs;

	/* the rings; the firmware's are read under pump_lock */
	struct mutex pump_lock;
	bool up;		/* the firmware took the handshake */
	struct h4isp_ring rings[MAX_RINGS];
	unsigned int nrings;
	struct h4isp_ring *io;
	struct h4isp_ring *buf_h2t;
	struct h4isp_ring *buf_t2h;
	struct h4isp_ring *sharedmalloc;
	struct h4isp_ring *io_t2h;
	spinlock_t ring_lock;	/* posting on the host's rings, the buffers */
	struct mutex cmd_lock;
	wait_queue_head_t wait;
	struct delayed_work poll;
	bool irq_works;
	u32 rx_errors[H4ISP_CAMS];

	u32 sensor_id[H4ISP_CAMS];
	u32 nmodes[H4ISP_CAMS];

	struct dentry *debugfs;
	u32 dbg_answer[CMD_WORDS];
	u32 dbg_len;
	int dbg_ret;

	struct v4l2_device v4l2;
	struct h4isp_cam cams[H4ISP_CAMS];
};

/* --- the IOMMU ------------------------------------------------------------ */

static int h4isp_dart_wait(struct h4isp *isp)
{
	u32 v;

	return readl_poll_timeout_atomic(isp->dart + DART_TLB_OP, v, !(v & DART_TLB_BUSY),
					 1, 10000);
}

/* every TLB, every stream: what iOS does after any change */
static void h4isp_dart_flush(struct h4isp *isp)
{
	unsigned long flags;

	spin_lock_irqsave(&isp->dart_lock, flags);
	if (isp->on) {
		if (!h4isp_dart_wait(isp))
			writel(DART_TLB_FLUSH_ALL, isp->dart + DART_TLB_OP);
		if (h4isp_dart_wait(isp))
			dev_err_ratelimited(isp->dev, "IOMMU stays busy: %#x\n",
					    readl(isp->dart + DART_TLB_OP));
	}
	spin_unlock_irqrestore(&isp->dart_lock, flags);
}

static void h4isp_dart_map(struct h4isp *isp, u32 va, dma_addr_t pa, u32 size)
{
	for (; size; va += SZ_4K, pa += SZ_4K, size -= SZ_4K)
		WRITE_ONCE(isp->pt[va >> 12], lower_32_bits(pa) | DART_PTE_VALID);
}

static void h4isp_dart_unmap(struct h4isp *isp, u32 va, u32 size)
{
	for (; size; va += SZ_4K, size -= SZ_4K)
		WRITE_ONCE(isp->pt[va >> 12], 0);
}

/*
 * The power state's clock gate only stops the IOMMU's clock: what it had
 * before -- iOS's own tables, for one -- is still in it.  So from scratch:
 * every stream's 64 STEs onto our tables, an invalid translation onto the
 * dummy page, iOS's configuration, every TLB flushed.
 */
static int h4isp_dart_init(struct h4isp *isp)
{
	int s, i, ret;

	writel(DART_ERROR_CLEAR, isp->dart + DART_ERROR);
	writel(lower_32_bits(isp->dummy_dma), isp->dart + DART_DIAG);
	for (s = 0; s < DART_STREAMS; s++)
		for (i = 0; i < DART_TABLES; i++) {
			ret = h4isp_dart_wait(isp);
			if (ret)
				goto busy;
			writel(lower_32_bits(isp->pt_dma + i * SZ_4K) | 1, isp->dart + DART_STE);
			writel(DART_STE_WRITE(s, i), isp->dart + DART_TLB_OP);
		}
	ret = h4isp_dart_wait(isp);
	if (ret)
		goto busy;
	writel(DART_CONFIG_ISP, isp->dart + DART_CONFIG);
	writel(0x7fffffff, isp->dart + DART_PERF);
	writel(0, isp->dart + DART_TLB_MISS);
	writel(0, isp->dart + DART_TLB_WAIT);
	writel(0, isp->dart + DART_TLB_HIT);
	writel(DART_TLB_FLUSH_ALL, isp->dart + DART_TLB_OP);
	ret = h4isp_dart_wait(isp);
	if (!ret)
		return 0;
busy:
	dev_err(isp->dev, "IOMMU does not answer: %#x\n", readl(isp->dart + DART_TLB_OP));
	return ret;
}

static irqreturn_t h4isp_dart_irq(int irq, void *data)
{
	struct h4isp *isp = data;
	u32 st = readl(isp->dart + DART_ERROR);

	if (st & DART_ERROR_FAULT) {
		u32 va = readl(isp->dart + DART_ERROR_ADDR);

		dev_err_ratelimited(isp->dev,
				    "IOMMU fault: %s of %#x, status %#x (STE %u, PTE %u: %#x)\n",
				    st & DART_ERROR_WRITE ? "write" : "read", va, st,
				    (va >> 22) & 63, (va >> 12) & 1023,
				    va < VA_END ? READ_ONCE(isp->pt[va >> 12]) : 0);
		writel(DART_ERROR_CLEAR, isp->dart + DART_ERROR);
	} else {
		/* a counter's overflow: iOS clears it by writing it back */
		writel(readl(isp->dart + DART_PERF), isp->dart + DART_PERF);
	}
	return IRQ_HANDLED;
}

/* --- memory -------------------------------------------------------------- */

static struct h4isp_mem *h4isp_mem_alloc(struct h4isp *isp, u32 size, u32 va)
{
	struct h4isp_mem *m;

	size = ALIGN(size, SZ_4K);
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return NULL;
	m->cpu = dma_alloc_coherent(isp->dev, size, &m->dma, GFP_KERNEL);
	if (!m->cpu)
		goto err;
	m->size = size;
	m->va = va;
	if (va == VA_ANY) {
		m->va = gen_pool_alloc(isp->va, size);
		if (!m->va)
			goto err_free;
	}
	h4isp_dart_map(isp, m->va, m->dma, size);
	mutex_lock(&isp->mem_lock);
	list_add_tail(&m->list, &isp->mems);
	mutex_unlock(&isp->mem_lock);
	h4isp_dart_flush(isp);
	return m;

err_free:
	dma_free_coherent(isp->dev, size, m->cpu, m->dma);
err:
	kfree(m);
	dev_err(isp->dev, "no memory for %u bytes of the ISP's\n", size);
	return NULL;
}

static void h4isp_mem_free(struct h4isp *isp, struct h4isp_mem *m)
{
	if (!m)
		return;
	mutex_lock(&isp->mem_lock);
	list_del(&m->list);
	mutex_unlock(&isp->mem_lock);
	h4isp_dart_unmap(isp, m->va, m->size);
	h4isp_dart_flush(isp);
	if (gen_pool_has_addr(isp->va, m->va, m->size))
		gen_pool_free(isp->va, m->va, m->size);
	dma_free_coherent(isp->dev, m->size, m->cpu, m->dma);
	kfree(m);
}

static void h4isp_mem_free_all(struct h4isp *isp)
{
	struct h4isp_mem *m, *tmp;

	list_for_each_entry_safe(m, tmp, &isp->mems, list)
		h4isp_mem_free(isp, m);
	isp->fw = isp->shared = isp->heap = isp->msgs = NULL;
}

/* an address of the firmware's, as ours, and how much of it there is */
static void *h4isp_ptr(struct h4isp *isp, u32 va, u32 len, u32 *avail)
{
	struct h4isp_mem *m;
	void *p = NULL;

	mutex_lock(&isp->mem_lock);
	list_for_each_entry(m, &isp->mems, list)
		if (va >= m->va && va - m->va < m->size && len <= m->size - (va - m->va)) {
			p = m->cpu + (va - m->va);
			if (avail)
				*avail = m->size - (va - m->va);
			break;
		}
	mutex_unlock(&isp->mem_lock);
	return p;
}

/* --- the rings ------------------------------------------------------------- */

static u32 *h4isp_entry(struct h4isp_ring *r, u32 i)
{
	return r->q + i * (RING_ENTRY / 4);
}

static void h4isp_doorbell(struct h4isp *isp, struct h4isp_ring *r)
{
	writel(BIT(r->source + 4), isp->regs + ISP_DOORBELL);
}

/* a host ring's next entry, if it is ours; under ring_lock */
static u32 *h4isp_next_free(struct h4isp_ring *r)
{
	u32 *e = h4isp_entry(r, r->idx);

	if (!(READ_ONCE(e[0]) & 1))
		return NULL;
	dma_rmb();
	return e;
}

/* post on a host ring: the arguments, then the address, then the doorbell */
static void h4isp_post(struct h4isp *isp, struct h4isp_ring *r, u32 *e, u32 va, u32 a1, u32 a2)
{
	WRITE_ONCE(e[1], a1);
	WRITE_ONCE(e[2], a2);
	dma_wmb();
	WRITE_ONCE(e[0], va);
	r->idx = (r->idx + 1) % r->n;
	h4isp_doorbell(isp, r);
}

static void h4isp_terminal(struct h4isp *isp, u32 va)
{
	char s[200];
	const char *p;
	u32 avail = 0, i;

	p = h4isp_ptr(isp, va, 1, &avail);
	if (!p)
		return;
	for (i = 0; i < sizeof(s) - 1 && i < avail && p[i]; i++)
		s[i] = p[i];
	while (i && (s[i - 1] == '\n' || s[i - 1] == '\r'))
		i--;
	s[i] = 0;
	if (fw_log)
		dev_info(isp->dev, "ISPCPU: %s\n", s);
	else
		dev_dbg(isp->dev, "ISPCPU: %s\n", s);
}

/* SHAREDMALLOC: no address and a size, memory; an address, a free */
static u32 h4isp_sharedmalloc(struct h4isp *isp, u32 va, u32 size, u32 tag)
{
	char name[5] = { tag, tag >> 8, tag >> 16, tag >> 24, 0 };
	struct h4isp_mem *m, *found = NULL;

	if (va) {
		mutex_lock(&isp->mem_lock);
		list_for_each_entry(m, &isp->mems, list)
			if (m->firmware && m->va == va) {
				found = m;
				break;
			}
		mutex_unlock(&isp->mem_lock);
		if (found)
			h4isp_mem_free(isp, found);
		else
			dev_warn(isp->dev, "the firmware frees %#x, which it was never given\n", va);
		return 0;
	}
	m = h4isp_mem_alloc(isp, max_t(u32, size, SZ_4K), VA_ANY);
	if (!m)
		return 0;
	m->firmware = true;
	dev_dbg(isp->dev, "the firmware's %s: %u bytes at %#x\n", name, size, m->va);
	return m->va;
}

static void h4isp_flush_pending(struct h4isp *isp, struct h4isp_cam *cam);

/* a buffer back from the firmware: a frame (type 2) or unused (type 1) */
static void h4isp_buffer_back(struct h4isp *isp, u32 tag, u32 type)
{
	unsigned int c = (tag >> 8) & 0xf, idx = tag & 0xff;
	struct h4isp_cam *cam;
	struct h4isp_buf *buf = NULL;
	unsigned long flags;
	bool running = false;

	if ((tag & ~0xfffu) != H4ISP_TAG(0, 0) || c >= H4ISP_CAMS || idx >= H4ISP_MAX_BUFS) {
		dev_warn_ratelimited(isp->dev, "a buffer back with tag %#x, not ours\n", tag);
		return;
	}
	cam = &isp->cams[c];
	spin_lock_irqsave(&isp->ring_lock, flags);
	buf = cam->at_fw[idx];
	if (buf) {
		cam->at_fw[idx] = NULL;
		cam->nat_fw--;
	}
	running = cam->running;
	spin_unlock_irqrestore(&isp->ring_lock, flags);
	if (!buf) {
		dev_warn_ratelimited(isp->dev, "buffer %#x back twice\n", tag);
		return;
	}

	if (type == 2) {
		buf->vb.vb2_buf.timestamp = ktime_get_ns();
		buf->vb.sequence = cam->sequence++;
		buf->vb.field = V4L2_FIELD_NONE;
		vb2_set_plane_payload(&buf->vb.vb2_buf, 0, cam->fmt.sizeimage);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	} else if (running) {
		/* handed back unfilled while streaming: to the firmware again */
		spin_lock_irqsave(&isp->ring_lock, flags);
		list_add(&buf->list, &cam->pending);
		spin_unlock_irqrestore(&isp->ring_lock, flags);
	} else {
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	h4isp_flush_pending(isp, cam);
}

/*
 * BUF_T2H: type, count, then 10 words a buffer -- planes 0-2, 0 x3, the
 * plane count, the pool, our tag, 0
 */
static void h4isp_buffers_back(struct h4isp *isp, u32 va)
{
	u32 *m, type, cnt, k;

	m = h4isp_ptr(isp, va, 8, NULL);
	if (!m)
		return;
	type = READ_ONCE(m[0]);
	cnt = READ_ONCE(m[1]);
	if (cnt > MSG_BUFS || !h4isp_ptr(isp, va, 8 + cnt * 40, NULL)) {
		dev_warn_ratelimited(isp->dev, "a buffer message of %u buffers at %#x\n", cnt, va);
		return;
	}
	for (k = 0; k < cnt; k++)
		h4isp_buffer_back(isp, READ_ONCE(m[2 + k * 10 + 8]), type);
}

static void h4isp_notification(struct h4isp *isp, u32 va)
{
	u32 *p = h4isp_ptr(isp, va, 8, NULL);
	u32 op = p ? READ_ONCE(p[1]) & 0xffff : 0;

	if (op == NOTE_FRAME_TIMEOUT)
		dev_warn_ratelimited(isp->dev, "the firmware waited for a frame in vain\n");
	else if (op != NOTE_FOCUS_FAILED)	/* these cameras have no focus */
		dev_dbg(isp->dev, "firmware notification %#x\n", op);
}

/* everything the firmware has posted, answered */
static void h4isp_pump(struct h4isp *isp)
{
	unsigned int i;

	mutex_lock(&isp->pump_lock);
	if (!isp->up)
		goto out;
	for (i = 0; i < isp->nrings; i++) {
		struct h4isp_ring *r = &isp->rings[i];

		if (r->type == RING_HOST)
			continue;
		while (!(READ_ONCE(h4isp_entry(r, r->idx)[0]) & 1)) {
			u32 *e = h4isp_entry(r, r->idx), w0, a1, a2, ans;

			dma_rmb();
			w0 = READ_ONCE(e[0]);
			a1 = READ_ONCE(e[1]);
			a2 = READ_ONCE(e[2]);
			ans = w0 & ~3u;
			r->idx = (r->idx + 1) % r->n;
			if (r->type == RING_TERMINAL) {
				h4isp_terminal(isp, ans);
				WRITE_ONCE(e[0], 1);
				continue;
			}
			if (r == isp->sharedmalloc) {
				ans = h4isp_sharedmalloc(isp, ans, a1, a2);
				a1 = a2 = 0;
			} else if (r == isp->buf_t2h) {
				h4isp_buffers_back(isp, ans);
			} else if (r == isp->io_t2h) {
				h4isp_notification(isp, ans);
			} else {
				dev_dbg(isp->dev, "%s: %#x %#x %#x\n", r->name, w0, a1, a2);
			}
			WRITE_ONCE(e[1], a1);
			WRITE_ONCE(e[2], a2);
			dma_wmb();
			WRITE_ONCE(e[0], ans | 1);
			h4isp_doorbell(isp, r);
		}
	}
out:
	mutex_unlock(&isp->pump_lock);
	wake_up_all(&isp->wait);
}

static irqreturn_t h4isp_irq(int irq, void *data)
{
	struct h4isp *isp = data;
	u32 st = readl(isp->regs + ISP_IRQ_STATUS);
	int i;

	if (!(st & (ISP_IRQ_RINGS | ISP_IRQ_RX)))
		return IRQ_NONE;
	writel(st, isp->regs + ISP_IRQ_CLEAR);
	if (st & ISP_IRQ_RX)
		for (i = 0; i < H4ISP_CAMS; i++) {
			u32 v = readl(isp->regs + ISP_RX_ERROR(i));

			if (v & BIT(1)) {
				isp->rx_errors[i]++;
				writel(v, isp->regs + ISP_RX_ERROR(i));
			}
		}
	WRITE_ONCE(isp->irq_works, true);
	return st & ISP_IRQ_RINGS ? IRQ_WAKE_THREAD : IRQ_HANDLED;
}

static irqreturn_t h4isp_irq_thread(int irq, void *data)
{
	h4isp_pump(data);
	return IRQ_HANDLED;
}

/* without its interrupt the ISP would still work, polled */
static void h4isp_poll(struct work_struct *work)
{
	struct h4isp *isp = container_of(work, struct h4isp, poll.work);

	h4isp_pump(isp);
	if (READ_ONCE(isp->up))
		schedule_delayed_work(&isp->poll, msecs_to_jiffies(5));
}

/* until an entry of ours is handed back, answering the firmware meanwhile */
static int h4isp_wait_entry(struct h4isp *isp, u32 *e, unsigned int timeout_ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);

	do {
		if (wait_event_timeout(isp->wait, READ_ONCE(e[0]) & 1, msecs_to_jiffies(10)))
			return 0;
		h4isp_pump(isp);
	} while (time_before(jiffies, end));
	return READ_ONCE(e[0]) & 1 ? 0 : -ETIMEDOUT;
}

/*
 * A command (iOS's ispCmdSend): the packet -- u16 opcode at +4, u16 result
 * at +6, parameters from +8 -- copied into the shared region, its address
 * and length posted on IO; the firmware answers in the packet.
 */
static int h4isp_cmd(struct h4isp *isp, u16 op, u32 *p, u32 len, unsigned int timeout_ms)
{
	u32 *cmd, *e, res;
	unsigned long flags;
	int ret;

	if (!isp->io)
		return -ENODEV;
	p[0] = 0;
	p[1] = op;
	mutex_lock(&isp->cmd_lock);
	cmd = isp->shared->cpu + SHARED_CMD;
	spin_lock_irqsave(&isp->ring_lock, flags);
	e = h4isp_next_free(isp->io);
	if (e) {
		memcpy(cmd, p, len);
		h4isp_post(isp, isp->io, e, VA_SHARED + SHARED_CMD, len, len);
	}
	spin_unlock_irqrestore(&isp->ring_lock, flags);
	if (!e) {
		ret = -EBUSY;
		dev_err(isp->dev, "command %#x: the firmware still has the last one\n", op);
		goto out;
	}
	ret = h4isp_wait_entry(isp, e, timeout_ms);
	if (ret) {
		dev_err(isp->dev, "command %#x: no answer in %u ms\n", op, timeout_ms);
		goto out;
	}
	dma_rmb();
	memcpy(p, cmd, len);
	res = p[1] >> 16;
	if (res) {
		dev_err(isp->dev, "command %#x: error %#x\n", op, res);
		ret = -EIO;
	}
out:
	mutex_unlock(&isp->cmd_lock);
	return ret;
}

static int h4isp_cmd_ch(struct h4isp *isp, u16 op, u32 ch, u32 len)
{
	u32 p[CMD_WORDS] = { };

	p[2] = ch;
	return h4isp_cmd(isp, op, p, len, 1000);
}

/*
 * Buffers to the firmware (iOS's ISP_SendBuffers): a 0x180-byte message --
 * type 1, the count, then 10 words a buffer: its planes' addresses, 0 x3,
 * the plane count, the pool, our tag, 0 -- posted on BUF_H2T as (address,
 * 0x180, 0x30000000).  One buffer a message, a message slot a ring entry.
 * Under ring_lock.
 */
static int h4isp_give(struct h4isp *isp, struct h4isp_cam *cam, struct h4isp_buf *buf)
{
	struct h4isp_ring *r = isp->buf_h2t;
	unsigned int idx = buf->vb.vb2_buf.index;
	u32 *e, *m, slot;

	e = h4isp_next_free(r);
	if (!e)
		return -EBUSY;
	slot = r->idx;
	m = isp->msgs->cpu + slot * MSG_SIZE;
	memset(m, 0, MSG_SIZE);
	m[0] = 1;
	m[1] = 1;
	m[2] = buf->va;
	m[2 + 6] = 1;
	m[2 + 7] = cam->pool;
	m[2 + 8] = H4ISP_TAG(cam->ch, idx);
	cam->at_fw[idx] = buf;
	cam->nat_fw++;
	h4isp_post(isp, r, e, isp->msgs->va + slot * MSG_SIZE, MSG_SIZE, MSG_MAGIC);
	return 0;
}

static void h4isp_flush_pending(struct h4isp *isp, struct h4isp_cam *cam)
{
	struct h4isp_buf *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&isp->ring_lock, flags);
	if (cam->running)
		list_for_each_entry_safe(buf, tmp, &cam->pending, list) {
			if (h4isp_give(isp, cam, buf))
				break;
			list_del(&buf->list);
		}
	spin_unlock_irqrestore(&isp->ring_lock, flags);
}

/* --- power and the firmware's boot ------------------------------------------ */

/*
 * The sensors (iOS's EnableSensor): their clock pins to the peripheral
 * function, 1 ms, each sensor's enable, 5 ms.  Off, the reverse, the clock
 * pins driven low as iBoot leaves them.
 */
static void h4isp_sensors(struct h4isp *isp, bool on)
{
	int i;

	if (on) {
		if (isp->pins_active)
			pinctrl_select_state(isp->pinctrl, isp->pins_active);
		usleep_range(1000, 2000);
		for (i = 0; i < H4ISP_CAMS; i++)
			gpiod_set_value_cansleep(isp->enable[i], 1);
		usleep_range(5000, 6000);
	} else {
		for (i = 0; i < H4ISP_CAMS; i++)
			gpiod_set_value_cansleep(isp->enable[i], 0);
		if (isp->pins_idle)
			pinctrl_select_state(isp->pinctrl, isp->pins_idle);
	}
}

/* the ISP's reset: a pulse on bit 31 of its power state's register */
static void h4isp_reset(struct h4isp *isp)
{
	u32 v = readl(isp->ps) & ~BIT(31);

	writel(v | BIT(31), isp->ps);
	usleep_range(1000, 1500);
	writel(v, isp->ps);
	usleep_range(1000, 1500);
}

static int h4isp_wait_irq_status(struct h4isp *isp, const char *what)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout(isp->regs + ISP_IRQ_STATUS, v, v & ISP_IRQ_RINGS, 1000, 1000000);
	if (ret)
		dev_err(isp->dev, "no %s from the ISP's CPU (status %#x, mailbox %#x %#x)\n", what,
			v, readl(isp->regs + ISP_MBOX(0)), readl(isp->regs + ISP_MBOX(1)));
	writel(~0u, isp->regs + ISP_IRQ_CLEAR);
	return ret;
}

/* the channel table the firmware has set up: its rings, ours made ready */
static int h4isp_rings(struct h4isp *isp, u32 nrings)
{
	u8 *tbl = h4isp_ptr(isp, readl(isp->regs + ISP_MBOX(0)), nrings * 0x100, NULL);
	u32 i, k;

	if (!tbl) {
		dev_err(isp->dev, "the channel table is at %#x, outside the ISP's memory\n",
			readl(isp->regs + ISP_MBOX(0)));
		return -EINVAL;
	}
	for (i = 0; i < nrings; i++) {
		struct h4isp_ring *r = &isp->rings[i];
		u32 *w = (u32 *)(tbl + i * 0x100);

		memcpy(r->name, tbl + i * 0x100, sizeof(r->name) - 1);
		r->name[sizeof(r->name) - 1] = 0;
		r->type = READ_ONCE(w[0x40 / 4]);
		r->source = READ_ONCE(w[0x44 / 4]) & 3;
		r->n = READ_ONCE(w[0x48 / 4]);
		r->idx = 0;
		r->q = h4isp_ptr(isp, READ_ONCE(w[0x4c / 4]), r->n * RING_ENTRY, NULL);
		if (!r->q || !r->n || r->type > RING_TERMINAL) {
			dev_err(isp->dev, "channel %u (%s): %u entries at %#x, type %u\n", i,
				r->name, r->n, READ_ONCE(w[0x4c / 4]), r->type);
			return -EINVAL;
		}
		dev_dbg(isp->dev, "channel %u: %-12s type %u source %u, %u entries\n", i,
			r->name, r->type, r->source, r->n);
		if (!strcmp(r->name, "IO"))
			isp->io = r;
		else if (!strcmp(r->name, "BUF_H2T"))
			isp->buf_h2t = r;
		else if (!strcmp(r->name, "BUF_T2H"))
			isp->buf_t2h = r;
		else if (!strcmp(r->name, "SHAREDMALLOC"))
			isp->sharedmalloc = r;
		else if (!strcmp(r->name, "IO_T2H"))
			isp->io_t2h = r;
		/*
		 * the host's rings are the host's to set up (iOS 0x807e4e14):
		 * every entry ours, the arguments 0; without that the firmware
		 * never takes the ready magic
		 */
		if (r->type == RING_HOST)
			for (k = 0; k < r->n; k++) {
				u32 *e = h4isp_entry(r, k);

				e[0] = 1;
				e[1] = 0;
				e[2] = 0;
			}
	}
	isp->nrings = nrings;
	if (!isp->io || !isp->buf_h2t || !isp->buf_t2h || !isp->sharedmalloc) {
		dev_err(isp->dev, "the firmware has no IO, BUF_H2T, BUF_T2H or SHAREDMALLOC\n");
		return -EINVAL;
	}
	return 0;
}

/*
 * iOS's ISP_Init: the CPU started at the firmware, it says how many rings it
 * has, how much queue memory and how much heap it wants; the host answers
 * with where everything is, and the firmware lays out its channel table;
 * then the ready magic, and the firmware runs.
 */
static int h4isp_handshake(struct h4isp *isp)
{
	u32 nrings, qsize, heap, d_off, *d, v;
	int i, ret;

	for (i = 0; i < 8; i++)
		writel(0, isp->regs + ISP_MBOX(i));
	writel(~0u, isp->regs + ISP_IRQ_CLEAR);
	writel(ISP_MIPI_CONFIG, isp->regs + ISP_MIPI(0));
	writel(ISP_MIPI_CONFIG, isp->regs + ISP_MIPI(1));
	writel(VA_FW | BIT(31), isp->regs + ISP_BOOT);
	writel(1, isp->regs + ISP_RUN);

	ret = h4isp_wait_irq_status(isp, "wake signal");
	if (ret)
		return ret;
	nrings = readl(isp->regs + ISP_MBOX(0));
	qsize = readl(isp->regs + ISP_MBOX(1)) + 1;
	heap = readl(isp->regs + ISP_MBOX(3));
	d_off = ALIGN(qsize, 0x100);
	if (!nrings || nrings > MAX_RINGS || d_off + 0x18 > SHARED_CMD || heap > HEAP_MAX) {
		dev_err(isp->dev, "the firmware wants %u rings, %u bytes for them, %u of heap\n",
			nrings, qsize, heap);
		return -EINVAL;
	}
	if (heap) {
		isp->heap = h4isp_mem_alloc(isp, max_t(u32, heap, SZ_4K), VA_HEAP);
		if (!isp->heap)
			return -ENOMEM;
	}
	writel(VA_FW, isp->regs + ISP_MBOX(0));
	writel(VA_SHARED, isp->regs + ISP_MBOX(1));
	writel(FW_SIZE, isp->regs + ISP_MBOX(2));
	writel(VA_END - FW_SIZE, isp->regs + ISP_MBOX(3));
	writel(heap ? VA_HEAP : 0, isp->regs + ISP_MBOX(4));
	writel(heap ? max_t(u32, heap, SZ_4K) : 0, isp->regs + ISP_MBOX(5));
	writel(isp->sensor_type, isp->regs + ISP_MBOX(6));
	d = isp->shared->cpu + d_off;		/* boot argument, flags, no log buffers */
	d[0] = 5;
	for (i = 1; i < 6; i++)
		d[i] = 0;
	writel(VA_SHARED + d_off, isp->regs + ISP_MBOX(7));
	writel(0x10, isp->regs + ISP_DOORBELL);

	ret = h4isp_wait_irq_status(isp, "channel table");
	if (ret)
		return ret;
	ret = h4isp_rings(isp, nrings);
	if (ret)
		return ret;
	isp->msgs = h4isp_mem_alloc(isp, isp->buf_h2t->n * MSG_SIZE, VA_ANY);
	if (!isp->msgs)
		return -ENOMEM;

	mutex_lock(&isp->pump_lock);
	isp->up = true;
	mutex_unlock(&isp->pump_lock);
	writel(ISP_READY, isp->regs + ISP_MBOX(3));
	ret = readl_poll_timeout(isp->regs + ISP_MBOX(3), v, !v, 1000, 1000000);
	if (ret) {
		dev_err(isp->dev, "the firmware did not take the ready magic\n");
		return ret;
	}
	return 0;
}

static void h4isp_power_down(struct h4isp *isp);

/* a sensor's set-file, by its ID; without one the firmware's defaults */
static void h4isp_setfile(struct h4isp *isp, unsigned int ch)
{
	const struct firmware *fw;
	struct h4isp_mem *m;
	u32 p[CMD_WORDS] = { };
	char name[40];

	snprintf(name, sizeof(name), "apple/h4isp-setfile-%04x.bin", isp->sensor_id[ch]);
	if (request_firmware(&fw, name, isp->dev)) {
		dev_warn(isp->dev, "no %s: camera %u keeps the firmware's defaults\n", name, ch);
		return;
	}
	m = h4isp_mem_alloc(isp, fw->size, VA_ANY);
	if (m) {
		memcpy(m->cpu, fw->data, fw->size);
		p[2] = ch;
		p[3] = m->va;
		p[4] = fw->size;
		h4isp_cmd(isp, CMD_CH_SET_FILE_LOAD, p, 0x14, 1000);
	}
	release_firmware(fw);
}

static int h4isp_power_up(struct h4isp *isp)
{
	const struct firmware *fw;
	u32 p[CMD_WORDS];
	unsigned int ch;
	int ret;

	lockdep_assert_held(&isp->lock);
	if (isp->up)
		return 0;
	ret = request_firmware(&fw, H4ISP_FIRMWARE, isp->dev);
	if (ret) {
		dev_err(isp->dev, "no %s (./cascadia k93 firmware takes it out of the IPSW)\n",
			H4ISP_FIRMWARE);
		return ret;
	}
	if (fw->size < 0x1044 || fw->size > FW_SIZE || memcmp(fw->data + 0x1040, "adc", 3)) {
		dev_err(isp->dev, "%s is not the H4 ISP's firmware\n", H4ISP_FIRMWARE);
		release_firmware(fw);
		return -EINVAL;
	}

	ret = regulator_enable(isp->vdd);
	if (ret)
		goto err_fw;
	usleep_range(5000, 6000);
	ret = clk_prepare_enable(isp->clk);
	if (ret)
		goto err_vdd;
	h4isp_reset(isp);
	h4isp_sensors(isp, true);
	writel(0, isp->regs + ISP_RUN);
	spin_lock_irq(&isp->dart_lock);
	isp->on = true;
	spin_unlock_irq(&isp->dart_lock);
	ret = h4isp_dart_init(isp);
	if (ret)
		goto err;
	if (isp->dart_irq > 0) {
		enable_irq(isp->dart_irq);
		isp->dart_irq_enabled = true;
	}

	ret = -ENOMEM;
	isp->fw = h4isp_mem_alloc(isp, FW_SIZE, VA_FW);
	isp->shared = h4isp_mem_alloc(isp, SHARED_SIZE, VA_SHARED);
	if (!isp->fw || !isp->shared)
		goto err;
	memcpy(isp->fw->cpu, fw->data, fw->size);
	release_firmware(fw);
	fw = NULL;

	ret = h4isp_handshake(isp);
	if (ret)
		goto err;
	writel(ISP_IRQ_RINGS | ISP_IRQ_RX, isp->regs + ISP_IRQ_ENABLE);
	enable_irq(isp->irq);
	isp->irq_enabled = true;

	ret = h4isp_cmd_ch(isp, CMD_START, 0, 8);
	if (ret)
		goto err;
	if (fw_log) {
		memset(p, 0, sizeof(p));
		p[2] = 1;
		h4isp_cmd(isp, CMD_PRINT_ENABLE, p, 0xc, 1000);
	}
	if (!READ_ONCE(isp->irq_works)) {
		dev_warn(isp->dev, "no interrupt from the ISP: polling it\n");
		schedule_delayed_work(&isp->poll, 1);
	}
	/* the sensors (iOS: no modes, no sensor), and their set-files */
	for (ch = 0; ch < H4ISP_CAMS; ch++) {
		memset(p, 0, sizeof(p));
		p[2] = ch;
		if (h4isp_cmd(isp, CMD_CH_INFO_GET, p, 0xa8, 1000))
			continue;
		isp->sensor_id[ch] = p[0x20 / 4];
		isp->nmodes[ch] = p[0x60 / 4];
		dev_dbg(isp->dev, "camera %u: sensor %#x, %u modes, modules %u %u\n", ch,
			isp->sensor_id[ch], isp->nmodes[ch], p[0x14 / 4], p[0x18 / 4]);
		if (isp->nmodes[ch])
			h4isp_setfile(isp, ch);
	}
	return 0;

err:
	if (fw)
		release_firmware(fw);
	h4isp_power_down(isp);
	return ret;
err_vdd:
	regulator_disable(isp->vdd);
err_fw:
	release_firmware(fw);
	return ret;
}

static void h4isp_power_down(struct h4isp *isp)
{
	u32 p[CMD_WORDS] = { };

	if (isp->up && isp->io)
		h4isp_cmd(isp, CMD_STOP, p, 8, 200);
	if (isp->irq_enabled) {
		writel(0, isp->regs + ISP_IRQ_ENABLE);
		disable_irq(isp->irq);
		isp->irq_enabled = false;
	}
	mutex_lock(&isp->pump_lock);
	isp->up = false;
	mutex_unlock(&isp->pump_lock);
	cancel_delayed_work_sync(&isp->poll);
	if (isp->on)
		writel(0, isp->regs + ISP_RUN);
	if (isp->dart_irq_enabled) {
		disable_irq(isp->dart_irq);
		isp->dart_irq_enabled = false;
	}
	h4isp_sensors(isp, false);
	spin_lock_irq(&isp->dart_lock);
	if (isp->on) {
		isp->on = false;
		spin_unlock_irq(&isp->dart_lock);
		clk_disable_unprepare(isp->clk);
		regulator_disable(isp->vdd);
	} else {
		spin_unlock_irq(&isp->dart_lock);
	}
	h4isp_mem_free_all(isp);
	isp->nrings = 0;
	isp->io = isp->buf_h2t = isp->buf_t2h = isp->sharedmalloc = isp->io_t2h = NULL;
}

/* --- a camera's channel ----------------------------------------------------- */

/*
 * How low the firmware's exposure may take the frame rate: as iOS lets it, to
 * its own floor (half the mode's rate) when the light is low -- exposure has
 * priority -- or not below the mode's rate at all.
 */
static int h4isp_ae_floor(struct h4isp_cam *cam)
{
	u32 p[CMD_WORDS] = { };

	p[2] = cam->ch;
	p[3] = READ_ONCE(cam->ae_priority) ? cam->ae_min : cam->size->fps256;
	return h4isp_cmd(cam->isp, CMD_CH_AE_FRAME_RATE_MIN_SET, p, 0x10, 1000);
}

/*
 * iOS's StartCamera: the mode, a centred crop of it, the output (format 1:
 * one plane, YUYV), the firmware recycling buffers; buffers; then CH_START.
 */
static int h4isp_ch_config(struct h4isp_cam *cam)
{
	struct h4isp *isp = cam->isp;
	const struct h4isp_size *s = cam->size;
	u32 p[CMD_WORDS] = { }, sw, sh;
	int ret;

	if (!isp->nmodes[cam->ch] || s->mode >= isp->nmodes[cam->ch]) {
		dev_err(isp->dev, "no %s camera (sensor %#x, %u modes)\n", cam->desc->name,
			isp->sensor_id[cam->ch], isp->nmodes[cam->ch]);
		return -ENODEV;
	}
	p[2] = cam->ch;
	p[3] = s->mode;
	ret = h4isp_cmd(isp, CMD_CH_CAMERA_CONFIG_GET, p, 0x60, 1000);
	if (ret)
		return ret;
	sw = p[4] & 0xffff;
	sh = p[4] >> 16;
	cam->pool = p[8];
	if (sw < s->width || sh < s->height) {
		dev_err(isp->dev, "%s camera's mode %u is %ux%u, not %ux%u\n", cam->desc->name,
			s->mode, sw, sh, s->width, s->height);
		return -EINVAL;
	}

	memset(p, 0, sizeof(p));
	p[2] = cam->ch;
	p[3] = s->mode;
	ret = h4isp_cmd(isp, CMD_CH_CAMERA_CONFIG_SELECT, p, 0x10, 1000);
	if (ret)
		return ret;
	/* the firmware's floor for the frame rate in this mode (half its rate) */
	memset(p, 0, sizeof(p));
	p[2] = cam->ch;
	if (!h4isp_cmd(isp, CMD_CH_AE_FRAME_RATE_MIN_GET, p, 0x10, 1000) && p[3] &&
	    p[3] <= s->fps256)
		cam->ae_min = p[3];
	else
		cam->ae_min = s->fps256 / 2;
	ret = h4isp_ae_floor(cam);
	if (ret)
		return ret;
	memset(p, 0, sizeof(p));
	p[2] = cam->ch;
	p[3] = (sw - s->width) / 2;
	p[4] = (sh - s->height) / 2;
	p[5] = s->width;
	p[6] = s->height;
	ret = h4isp_cmd(isp, CMD_CH_CROP_SET, p, 0x1c, 1000);
	if (ret)
		return ret;
	memset(p, 0, sizeof(p));
	p[2] = cam->ch;
	p[3] = s->width;
	p[4] = s->height;
	p[5] = 1;		/* the range: iOS's, the negation of a flag it leaves 0 */
	p[6] = 1;		/* format 1: one plane, 2 bytes a pixel */
	p[7] = cam->fmt.bytesperline;
	ret = h4isp_cmd(isp, CMD_CH_OUTPUT_CONFIG_SET, p, 0x28, 1000);
	if (ret)
		return ret;
	memset(p, 0, sizeof(p));
	p[2] = cam->ch;
	p[3] = 1;
	ret = h4isp_cmd(isp, CMD_CH_BUFFER_RECYCLE_MODE_SET, p, 0x10, 1000);
	if (ret)
		return ret;
	return h4isp_cmd_ch(isp, CMD_CH_BUFFER_RECYCLE_START, cam->ch, 0xc);
}

/* every buffer the camera holds back to vb2 */
static void h4isp_return_all(struct h4isp_cam *cam, enum vb2_buffer_state state)
{
	struct h4isp *isp = cam->isp;
	struct h4isp_buf *buf, *tmp;
	unsigned long flags;
	LIST_HEAD(list);
	unsigned int i;

	spin_lock_irqsave(&isp->ring_lock, flags);
	cam->running = false;
	list_splice_init(&cam->pending, &list);
	for (i = 0; i < H4ISP_MAX_BUFS; i++)
		if (cam->at_fw[i]) {
			list_add_tail(&cam->at_fw[i]->list, &list);
			cam->at_fw[i] = NULL;
		}
	cam->nat_fw = 0;
	spin_unlock_irqrestore(&isp->ring_lock, flags);
	list_for_each_entry_safe(buf, tmp, &list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

static bool h4isp_all_back(struct h4isp_cam *cam)
{
	unsigned long flags;
	bool done;

	spin_lock_irqsave(&cam->isp->ring_lock, flags);
	done = !cam->nat_fw;
	spin_unlock_irqrestore(&cam->isp->ring_lock, flags);
	return done;
}

/* --- vb2 -------------------------------------------------------------------- */

/*
 * Eight buffers are plenty at 30 fps; ffmpeg asks for 256, which vb2 would
 * cut to 32 -- 59 MB at 720p.  CREATE_BUFS still adds what it is asked for.
 */
static int h4isp_queue_setup(struct vb2_queue *q, unsigned int *nbufs, unsigned int *nplanes,
			     unsigned int sizes[], struct device *alloc_devs[])
{
	struct h4isp_cam *cam = vb2_get_drv_priv(q);

	if (*nplanes)
		return *nplanes != 1 || sizes[0] < cam->fmt.sizeimage ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = cam->fmt.sizeimage;
	if (!vb2_get_num_buffers(q))
		*nbufs = min_t(unsigned int, *nbufs, H4ISP_BUFS);
	return 0;
}

/* a buffer into the ISP's address space, page by page */
static int h4isp_buf_init(struct vb2_buffer *vb)
{
	struct h4isp_cam *cam = vb2_get_drv_priv(vb->vb2_queue);
	struct h4isp_buf *buf = container_of(to_vb2_v4l2_buffer(vb), struct h4isp_buf, vb);
	struct h4isp *isp = cam->isp;
	struct sg_table *sgt = vb2_dma_sg_plane_desc(vb, 0);
	struct scatterlist *sg;
	u32 size = ALIGN(vb2_plane_size(vb, 0), SZ_4K), va, off = 0;
	unsigned int i;

	if (vb->index >= H4ISP_MAX_BUFS || !sgt)
		return -EINVAL;
	va = gen_pool_alloc(isp->va, size);
	if (!va)
		return -ENOMEM;
	for_each_sgtable_dma_sg(sgt, sg, i) {
		dma_addr_t a = sg_dma_address(sg);
		u32 len = min_t(u32, ALIGN(sg_dma_len(sg), SZ_4K), size - off);

		if (!IS_ALIGNED(a, SZ_4K) || (off + len < size && !IS_ALIGNED(sg_dma_len(sg), SZ_4K)))
			break;
		h4isp_dart_map(isp, va + off, a, len);
		off += len;
		if (off == size)
			break;
	}
	if (off < size) {
		dev_err(isp->dev, "buffer %u is not in whole pages\n", vb->index);
		h4isp_dart_unmap(isp, va, off);
		gen_pool_free(isp->va, va, size);
		return -EINVAL;
	}
	h4isp_dart_flush(isp);
	buf->va = va;
	buf->size = size;
	return 0;
}

static void h4isp_buf_cleanup(struct vb2_buffer *vb)
{
	struct h4isp_cam *cam = vb2_get_drv_priv(vb->vb2_queue);
	struct h4isp_buf *buf = container_of(to_vb2_v4l2_buffer(vb), struct h4isp_buf, vb);
	struct h4isp *isp = cam->isp;

	if (!buf->size)
		return;
	h4isp_dart_unmap(isp, buf->va, buf->size);
	h4isp_dart_flush(isp);
	gen_pool_free(isp->va, buf->va, buf->size);
	buf->size = 0;
}

static int h4isp_buf_prepare(struct vb2_buffer *vb)
{
	struct h4isp_cam *cam = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < cam->fmt.sizeimage)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, cam->fmt.sizeimage);
	return 0;
}

static void h4isp_buf_queue(struct vb2_buffer *vb)
{
	struct h4isp_cam *cam = vb2_get_drv_priv(vb->vb2_queue);
	struct h4isp_buf *buf = container_of(to_vb2_v4l2_buffer(vb), struct h4isp_buf, vb);
	struct h4isp *isp = cam->isp;
	unsigned long flags;

	spin_lock_irqsave(&isp->ring_lock, flags);
	list_add_tail(&buf->list, &cam->pending);
	spin_unlock_irqrestore(&isp->ring_lock, flags);
	h4isp_flush_pending(isp, cam);
}

static int h4isp_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct h4isp_cam *cam = vb2_get_drv_priv(q);
	struct h4isp *isp = cam->isp;
	unsigned long flags;
	int ret;

	mutex_lock(&isp->lock);
	if (isp->streaming) {
		dev_info(isp->dev, "the %s camera streams: one camera at a time\n",
			 isp->streaming->desc->name);
		ret = -EBUSY;
		goto err;
	}
	ret = h4isp_power_up(isp);
	if (ret)
		goto err;
	ret = h4isp_ch_config(cam);
	if (ret)
		goto err_off;
	spin_lock_irqsave(&isp->ring_lock, flags);
	cam->sequence = 0;
	cam->running = true;
	spin_unlock_irqrestore(&isp->ring_lock, flags);
	h4isp_flush_pending(isp, cam);
	ret = h4isp_cmd_ch(isp, CMD_CH_START, cam->ch, 0xc);
	if (ret)
		goto err_off;
	isp->streaming = cam;
	mutex_unlock(&isp->lock);
	dev_dbg(isp->dev, "%s camera: %ux%u\n", cam->desc->name, cam->fmt.width, cam->fmt.height);
	return 0;

err_off:
	spin_lock_irqsave(&isp->ring_lock, flags);
	cam->running = false;
	spin_unlock_irqrestore(&isp->ring_lock, flags);
	h4isp_power_down(isp);
err:
	h4isp_return_all(cam, VB2_BUF_STATE_QUEUED);
	mutex_unlock(&isp->lock);
	return ret;
}

static void h4isp_stop_streaming(struct vb2_queue *q)
{
	struct h4isp_cam *cam = vb2_get_drv_priv(q);
	struct h4isp *isp = cam->isp;
	unsigned long flags, end;

	mutex_lock(&isp->lock);
	if (isp->streaming == cam) {
		spin_lock_irqsave(&isp->ring_lock, flags);
		cam->running = false;
		spin_unlock_irqrestore(&isp->ring_lock, flags);
		/* iOS's StopCamera: the channel stopped, its buffers handed back */
		h4isp_cmd_ch(isp, CMD_CH_STOP, cam->ch, 0xc);
		h4isp_cmd_ch(isp, CMD_CH_BUFFER_RETURN, cam->ch, 0xc);
		end = jiffies + msecs_to_jiffies(500);
		while (!h4isp_all_back(cam) && time_before(jiffies, end))
			if (!wait_event_timeout(isp->wait, h4isp_all_back(cam), msecs_to_jiffies(10)))
				h4isp_pump(isp);
		isp->streaming = NULL;
		h4isp_power_down(isp);
	}
	h4isp_return_all(cam, VB2_BUF_STATE_ERROR);
	mutex_unlock(&isp->lock);
}

static const struct vb2_ops h4isp_vb2_ops = {
	.queue_setup = h4isp_queue_setup,
	.buf_init = h4isp_buf_init,
	.buf_cleanup = h4isp_buf_cleanup,
	.buf_prepare = h4isp_buf_prepare,
	.buf_queue = h4isp_buf_queue,
	.start_streaming = h4isp_start_streaming,
	.stop_streaming = h4isp_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

/* --- V4L2 ------------------------------------------------------------------- */

static void h4isp_fill_fmt(const struct h4isp_size *s, struct v4l2_pix_format *f)
{
	memset(f, 0, sizeof(*f));
	f->width = s->width;
	f->height = s->height;
	f->pixelformat = V4L2_PIX_FMT_YUYV;
	f->field = V4L2_FIELD_NONE;
	f->bytesperline = ALIGN(s->width * 2, 16);
	f->sizeimage = f->bytesperline * s->height;
	f->colorspace = V4L2_COLORSPACE_SRGB;
	f->ycbcr_enc = V4L2_YCBCR_ENC_601;
	/* the frames reach below 16 and above 235 */
	f->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	f->xfer_func = V4L2_XFER_FUNC_SRGB;
}

static const struct h4isp_size *h4isp_find_size(struct h4isp_cam *cam, u32 w, u32 h)
{
	return v4l2_find_nearest_size(cam->desc->sizes, cam->desc->nsizes, width, height, w, h);
}

static int h4isp_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct h4isp_cam *cam = video_drvdata(file);

	strscpy(cap->driver, "apple-h4isp", sizeof(cap->driver));
	strscpy(cap->card, cam->desc->card, sizeof(cap->card));
	return 0;
}

static int h4isp_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int h4isp_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct h4isp_cam *cam = video_drvdata(file);

	f->fmt.pix = cam->fmt;
	return 0;
}

static int h4isp_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct h4isp_cam *cam = video_drvdata(file);

	h4isp_fill_fmt(h4isp_find_size(cam, f->fmt.pix.width, f->fmt.pix.height), &f->fmt.pix);
	return 0;
}

static int h4isp_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct h4isp_cam *cam = video_drvdata(file);
	const struct h4isp_size *s;

	if (vb2_is_busy(&cam->queue))
		return -EBUSY;
	s = h4isp_find_size(cam, f->fmt.pix.width, f->fmt.pix.height);
	h4isp_fill_fmt(s, &f->fmt.pix);
	cam->fmt = f->fmt.pix;
	cam->size = s;
	return 0;
}

static int h4isp_enum_framesizes(struct file *file, void *fh, struct v4l2_frmsizeenum *fs)
{
	struct h4isp_cam *cam = video_drvdata(file);

	if (fs->pixel_format != V4L2_PIX_FMT_YUYV || fs->index >= cam->desc->nsizes)
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fs->discrete.width = cam->desc->sizes[fs->index].width;
	fs->discrete.height = cam->desc->sizes[fs->index].height;
	return 0;
}

static int h4isp_enum_frameintervals(struct file *file, void *fh, struct v4l2_frmivalenum *fi)
{
	struct h4isp_cam *cam = video_drvdata(file);
	unsigned int i;

	if (fi->pixel_format != V4L2_PIX_FMT_YUYV || fi->index)
		return -EINVAL;
	for (i = 0; i < cam->desc->nsizes; i++) {
		const struct h4isp_size *s = &cam->desc->sizes[i];

		if (s->width == fi->width && s->height == fi->height) {
			fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
			fi->discrete.numerator = 256;
			fi->discrete.denominator = s->fps256;
			return 0;
		}
	}
	return -EINVAL;
}

static int h4isp_g_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct h4isp_cam *cam = video_drvdata(file);

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	memset(&a->parm.capture, 0, sizeof(a->parm.capture));
	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe.numerator = 256;
	a->parm.capture.timeperframe.denominator = cam->size->fps256;
	a->parm.capture.readbuffers = 2;
	return 0;
}

static int h4isp_enum_input(struct file *file, void *fh, struct v4l2_input *in)
{
	struct h4isp_cam *cam = video_drvdata(file);

	if (in->index)
		return -EINVAL;
	strscpy(in->name, cam->desc->card, sizeof(in->name));
	in->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int h4isp_g_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int h4isp_s_input(struct file *file, void *fh, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops h4isp_ioctl_ops = {
	.vidioc_querycap = h4isp_querycap,
	.vidioc_enum_fmt_vid_cap = h4isp_enum_fmt,
	.vidioc_g_fmt_vid_cap = h4isp_g_fmt,
	.vidioc_try_fmt_vid_cap = h4isp_try_fmt,
	.vidioc_s_fmt_vid_cap = h4isp_s_fmt,
	.vidioc_enum_framesizes = h4isp_enum_framesizes,
	.vidioc_enum_frameintervals = h4isp_enum_frameintervals,
	.vidioc_g_parm = h4isp_g_parm,
	.vidioc_s_parm = h4isp_g_parm,
	.vidioc_enum_input = h4isp_enum_input,
	.vidioc_g_input = h4isp_g_input,
	.vidioc_s_input = h4isp_s_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations h4isp_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static int h4isp_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct h4isp_cam *cam = container_of(ctrl->handler, struct h4isp_cam, ctrls);
	struct h4isp *isp = cam->isp;
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE_AUTO_PRIORITY:
		WRITE_ONCE(cam->ae_priority, ctrl->val);
		mutex_lock(&isp->lock);
		if (isp->streaming == cam)
			ret = h4isp_ae_floor(cam);
		mutex_unlock(&isp->lock);
		return ret;
	}
	return -EINVAL;
}

static const struct v4l2_ctrl_ops h4isp_ctrl_ops = {
	.s_ctrl = h4isp_s_ctrl,
};

static int h4isp_cam_register(struct h4isp *isp, unsigned int ch)
{
	struct h4isp_cam *cam = &isp->cams[ch];
	struct video_device *vdev = &cam->vdev;
	struct vb2_queue *q = &cam->queue;
	int ret;

	cam->isp = isp;
	cam->ch = ch;
	cam->desc = &h4isp_cam_descs[ch];
	cam->size = &cam->desc->sizes[0];
	h4isp_fill_fmt(cam->size, &cam->fmt);
	mutex_init(&cam->lock);
	INIT_LIST_HEAD(&cam->pending);

	cam->ae_priority = 1;
	v4l2_ctrl_handler_init(&cam->ctrls, 3);
	v4l2_ctrl_new_std_menu(&cam->ctrls, NULL, V4L2_CID_CAMERA_ORIENTATION,
			       V4L2_CAMERA_ORIENTATION_EXTERNAL, 0, cam->desc->orientation);
	/*
	 * both pictures come out upright with the screen in its landscape
	 * scanout, the front one unmirrored (seen on the iPad, 2026-11-10)
	 */
	v4l2_ctrl_new_std(&cam->ctrls, NULL, V4L2_CID_CAMERA_SENSOR_ROTATION, 0, 0, 1, 0);
	v4l2_ctrl_new_std(&cam->ctrls, &h4isp_ctrl_ops, V4L2_CID_EXPOSURE_AUTO_PRIORITY,
			  0, 1, 1, 1);
	if (cam->ctrls.error) {
		ret = cam->ctrls.error;
		goto err_ctrls;
	}

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;
	q->drv_priv = cam;
	q->buf_struct_size = sizeof(struct h4isp_buf);
	q->ops = &h4isp_vb2_ops;
	q->mem_ops = &vb2_dma_sg_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &cam->lock;
	q->dev = isp->dev;
	q->min_queued_buffers = 2;
	q->max_num_buffers = H4ISP_MAX_BUFS;
	ret = vb2_queue_init(q);
	if (ret)
		goto err_ctrls;

	strscpy(vdev->name, cam->desc->card, sizeof(vdev->name));
	vdev->fops = &h4isp_fops;
	vdev->ioctl_ops = &h4isp_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->v4l2_dev = &isp->v4l2;
	vdev->queue = q;
	vdev->lock = &cam->lock;
	vdev->ctrl_handler = &cam->ctrls;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	video_set_drvdata(vdev, cam);
	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_ctrls;
	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&cam->ctrls);
	return ret;
}

static void h4isp_cam_unregister(struct h4isp_cam *cam)
{
	if (!video_is_registered(&cam->vdev))
		return;
	video_unregister_device(&cam->vdev);
	v4l2_ctrl_handler_free(&cam->ctrls);
}

/* --- debugfs: a firmware command by hand, while a camera streams ------------ */

#ifdef CONFIG_DEBUG_FS
/* write "OPCODE LEN [WORD2 WORD3 ...]" in hex (words from +8), read the answer */
static ssize_t h4isp_dbg_cmd_write(struct file *file, const char __user *ubuf, size_t len,
				   loff_t *ppos)
{
	struct h4isp *isp = file->private_data;
	u32 p[CMD_WORDS] = { }, op, plen, i;
	char buf[256], *s = buf, *tok;
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	tok = strsep(&s, " \t\n");
	if (!tok || kstrtou32(tok, 16, &op))
		return -EINVAL;
	tok = strsep(&s, " \t\n");
	if (!tok || kstrtou32(tok, 16, &plen) || plen < 8 || plen > sizeof(p) || plen & 3)
		return -EINVAL;
	for (i = 2; i < plen / 4 && (tok = strsep(&s, " \t\n")) && *tok; i++)
		if (kstrtou32(tok, 16, &p[i]))
			return -EINVAL;
	mutex_lock(&isp->lock);
	ret = isp->up ? h4isp_cmd(isp, op, p, plen, 1000) : -ENODEV;
	memcpy(isp->dbg_answer, p, sizeof(p));
	isp->dbg_len = plen;
	isp->dbg_ret = ret;
	mutex_unlock(&isp->lock);
	return ret && ret != -EIO ? ret : len;
}

static ssize_t h4isp_dbg_cmd_read(struct file *file, char __user *ubuf, size_t len,
				  loff_t *ppos)
{
	struct h4isp *isp = file->private_data;
	char buf[CMD_WORDS * 9 + 64];
	int n, i;

	mutex_lock(&isp->lock);
	n = scnprintf(buf, sizeof(buf), "opcode %#x result %#x (%d)\n",
		      isp->dbg_answer[1] & 0xffff, isp->dbg_answer[1] >> 16, isp->dbg_ret);
	for (i = 0; i < isp->dbg_len / 4; i++)
		n += scnprintf(buf + n, sizeof(buf) - n, "%08x%c", isp->dbg_answer[i],
			       i % 4 == 3 || i == isp->dbg_len / 4 - 1 ? '\n' : ' ');
	mutex_unlock(&isp->lock);
	return simple_read_from_buffer(ubuf, len, ppos, buf, n);
}

/* the ISP's memory the driver holds: nothing while no camera streams */
static int h4isp_dbg_memory_show(struct seq_file *sf, void *unused)
{
	struct h4isp *isp = sf->private;
	struct h4isp_mem *m;
	u32 total = 0, n = 0;

	mutex_lock(&isp->mem_lock);
	list_for_each_entry(m, &isp->mems, list) {
		seq_printf(sf, "%08x %8u %s\n", m->va, m->size, m->firmware ? "firmware's" : "");
		total += m->size;
		n++;
	}
	mutex_unlock(&isp->mem_lock);
	seq_printf(sf, "%u blocks, %u KB; address space free: %zu KB\n", n, total >> 10,
		   gen_pool_avail(isp->va) >> 10);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(h4isp_dbg_memory);

static const struct file_operations h4isp_dbg_cmd_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = h4isp_dbg_cmd_write,
	.read = h4isp_dbg_cmd_read,
	.llseek = default_llseek,
};

static void h4isp_debugfs_init(struct h4isp *isp)
{
	isp->debugfs = debugfs_create_dir("h4isp", NULL);
	debugfs_create_file("cmd", 0600, isp->debugfs, isp, &h4isp_dbg_cmd_fops);
	debugfs_create_file("memory", 0400, isp->debugfs, isp, &h4isp_dbg_memory_fops);
	debugfs_create_u32("rx_errors_rear", 0400, isp->debugfs, &isp->rx_errors[0]);
	debugfs_create_u32("rx_errors_front", 0400, isp->debugfs, &isp->rx_errors[1]);
}
#else
static void h4isp_debugfs_init(struct h4isp *isp) { }
#endif

/* --- the platform device ---------------------------------------------------- */

static int h4isp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct h4isp *isp;
	struct resource *res;
	unsigned int i;
	int ret;

	isp = devm_kzalloc(dev, sizeof(*isp), GFP_KERNEL);
	if (!isp)
		return -ENOMEM;
	isp->dev = dev;
	platform_set_drvdata(pdev, isp);

	isp->regs = devm_platform_ioremap_resource_byname(pdev, "isp");
	if (IS_ERR(isp->regs))
		return PTR_ERR(isp->regs);
	isp->dart = devm_platform_ioremap_resource_byname(pdev, "dart");
	if (IS_ERR(isp->dart))
		return PTR_ERR(isp->dart);
	/* the power state's register, the clock gate's too: not requested */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ps");
	if (!res)
		return dev_err_probe(dev, -EINVAL, "no \"ps\" register\n");
	isp->ps = devm_ioremap(dev, res->start, resource_size(res));
	if (!isp->ps)
		return -ENOMEM;
	isp->irq = platform_get_irq_byname(pdev, "isp");
	if (isp->irq < 0)
		return isp->irq;
	isp->dart_irq = platform_get_irq_byname_optional(pdev, "dart");

	isp->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(isp->clk))
		return dev_err_probe(dev, PTR_ERR(isp->clk), "no power state\n");
	isp->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(isp->vdd))
		return dev_err_probe(dev, PTR_ERR(isp->vdd), "no sensor supply\n");
	isp->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(isp->pinctrl)) {
		if (PTR_ERR(isp->pinctrl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		isp->pinctrl = NULL;
	} else {
		isp->pins_active = pinctrl_lookup_state(isp->pinctrl, "active");
		isp->pins_idle = pinctrl_lookup_state(isp->pinctrl, "idle");
		if (IS_ERR(isp->pins_active) || IS_ERR(isp->pins_idle))
			isp->pins_active = isp->pins_idle = NULL;
	}
	isp->enable[0] = devm_gpiod_get_optional(dev, "rear-enable", GPIOD_OUT_LOW);
	if (IS_ERR(isp->enable[0]))
		return dev_err_probe(dev, PTR_ERR(isp->enable[0]), "no rear enable\n");
	isp->enable[1] = devm_gpiod_get_optional(dev, "front-enable", GPIOD_OUT_LOW);
	if (IS_ERR(isp->enable[1]))
		return dev_err_probe(dev, PTR_ERR(isp->enable[1]), "no front enable\n");
	isp->sensor_type = 6;
	of_property_read_u32(dev->of_node, "apple,sensor-type", &isp->sensor_type);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	isp->pt = dmam_alloc_coherent(dev, DART_TABLES * SZ_4K, &isp->pt_dma, GFP_KERNEL);
	isp->dummy = dmam_alloc_coherent(dev, SZ_4K, &isp->dummy_dma, GFP_KERNEL);
	if (!isp->pt || !isp->dummy)
		return -ENOMEM;
	isp->va = devm_gen_pool_create(dev, 12, NUMA_NO_NODE, "h4isp");
	if (IS_ERR(isp->va))
		return PTR_ERR(isp->va);
	ret = gen_pool_add(isp->va, VA_SHARED + SHARED_SIZE, VA_HEAP - VA_SHARED - SHARED_SIZE,
			   NUMA_NO_NODE);
	ret = ret ?: gen_pool_add(isp->va, VA_HEAP + HEAP_MAX, VA_END - VA_HEAP - HEAP_MAX,
				  NUMA_NO_NODE);
	if (ret)
		return ret;

	spin_lock_init(&isp->dart_lock);
	spin_lock_init(&isp->ring_lock);
	mutex_init(&isp->lock);
	mutex_init(&isp->mem_lock);
	mutex_init(&isp->pump_lock);
	mutex_init(&isp->cmd_lock);
	INIT_LIST_HEAD(&isp->mems);
	init_waitqueue_head(&isp->wait);
	INIT_DELAYED_WORK(&isp->poll, h4isp_poll);

	ret = devm_request_threaded_irq(dev, isp->irq, h4isp_irq, h4isp_irq_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, "h4isp", isp);
	if (ret)
		return ret;
	if (isp->dart_irq > 0) {
		ret = devm_request_irq(dev, isp->dart_irq, h4isp_dart_irq, IRQF_NO_AUTOEN,
				       "h4isp-dart", isp);
		if (ret)
			return ret;
	}

	ret = v4l2_device_register(dev, &isp->v4l2);
	if (ret)
		return ret;
	for (i = 0; i < H4ISP_CAMS; i++) {
		ret = h4isp_cam_register(isp, i);
		if (ret)
			goto err;
	}
	h4isp_debugfs_init(isp);
	dev_info(dev, "%s camera %s, %s camera %s\n",
		 isp->cams[0].desc->name, video_device_node_name(&isp->cams[0].vdev),
		 isp->cams[1].desc->name, video_device_node_name(&isp->cams[1].vdev));
	return 0;

err:
	for (i = 0; i < H4ISP_CAMS; i++)
		h4isp_cam_unregister(&isp->cams[i]);
	v4l2_device_unregister(&isp->v4l2);
	return ret;
}

static void h4isp_remove(struct platform_device *pdev)
{
	struct h4isp *isp = platform_get_drvdata(pdev);
	unsigned int i;

	debugfs_remove_recursive(isp->debugfs);
	for (i = 0; i < H4ISP_CAMS; i++)
		h4isp_cam_unregister(&isp->cams[i]);
	mutex_lock(&isp->lock);
	if (isp->on || isp->up)
		h4isp_power_down(isp);
	mutex_unlock(&isp->lock);
	v4l2_device_unregister(&isp->v4l2);
}

static const struct of_device_id h4isp_of_match[] = {
	{ .compatible = "apple,s5l8940x-isp" },
	{ }
};
MODULE_DEVICE_TABLE(of, h4isp_of_match);

static struct platform_driver h4isp_driver = {
	.probe = h4isp_probe,
	.remove = h4isp_remove,
	.driver = {
		.name = "apple-h4isp",
		.of_match_table = h4isp_of_match,
	},
};
module_platform_driver(h4isp_driver);

MODULE_DESCRIPTION("Apple A5 (S5L8940X) H4 camera ISP: the iPad 2's cameras");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(H4ISP_FIRMWARE);
