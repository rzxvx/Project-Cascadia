// SPDX-License-Identifier: GPL-2.0
/*
 * Apple dockchannel UART earlycon - iPhone / A-series Linux bring-up.
 *
 * TX-only, for the earliest boot diagnostics over the DCSD serial cable, which
 * on A-series carries the dockchannel UART rather than the s5l UART. No device
 * init is done: iBoot/m1n1 bring the channel up before handing off.
 *
 * The earlycon address must point at the dockchannel DATA block, i.e. the ADT
 * "dockchannel-uart" reg base + 0x4000 (on t8030: 0x23d128000 + 0x4000 =
 * 0x23d12c000). The two FIFO registers then sit at +0x04/+0x14, inside the one
 * page that the earlycon framework maps (SZ_4K). Protocol and offsets come from
 * m1n1 src/dockchannel_uart.c (DATA_TX8 0x4004, DATA_TX_FREE 0x4014, relative
 * to the uart base = 0x04/0x14 relative to the data block).
 *
 * Use with: earlycon=dockchannel,mmio32,0x23d12c000
 */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/serial_core.h>

#define DOCKCHANNEL_TX8		0x04	/* write a byte to transmit */
#define DOCKCHANNEL_TX_FREE	0x14	/* read != 0 => TX FIFO has space */
#define DOCKCHANNEL_TX_RETRIES	10000	/* bound the wait like the real driver */

static void dockchannel_early_putc(struct uart_port *port, unsigned char c)
{
	int retries = DOCKCHANNEL_TX_RETRIES;

	/* If no debug host is draining the FIFO, TX_FREE stays 0 forever; a
	 * bounded wait drops the byte instead of hanging the whole boot. */
	while (readl(port->membase + DOCKCHANNEL_TX_FREE) == 0 && --retries)
		cpu_relax();
	if (!retries)
		return;

	writel(c, port->membase + DOCKCHANNEL_TX8);
}

static void dockchannel_early_write(struct console *con, const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;

	/* uart_console_write already expands '\n' to "\r\n". */
	uart_console_write(&dev->port, s, n, dockchannel_early_putc);
}

static int __init dockchannel_early_console_setup(struct earlycon_device *device,
						  const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = dockchannel_early_write;
	return 0;
}

EARLYCON_DECLARE(dockchannel, dockchannel_early_console_setup);
OF_EARLYCON_DECLARE(dockchannel, "apple,dockchannel-uart", dockchannel_early_console_setup);
