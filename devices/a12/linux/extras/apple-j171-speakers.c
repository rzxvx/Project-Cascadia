// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J171a internal-speaker playback. MCA register programming follows Asahi Linux mca.c;
 * J171a resources/routing and SIO words come from 21B91 ADT/native driver.
 * Fixed 48kHz, four-channel S16_LE. No microphone/headphone support.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

static bool test_enabled;
module_param(test_enabled, bool, 0600);
MODULE_PARM_DESC(test_enabled, "Arm the local speaker PCM test after boot (default off)");

/* Diagnostic attenuation, not a replacement for speaker protection. */
static unsigned int attenuation = 4;
static int j171_set_attenuation(const char *val, const struct kernel_param *kp)
{
 unsigned int divisor;
 int ret = kstrtouint(val, 0, &divisor);
 if (ret)
  return ret;
 if (divisor != 1 && divisor != 2 && divisor != 4)
  return -EINVAL;
 WRITE_ONCE(*(unsigned int *)kp->arg, divisor);
 return 0;
}
static const struct kernel_param_ops j171_attenuation_ops = {
 .set = j171_set_attenuation,
 .get = param_get_uint,
};
module_param_cb(attenuation, &j171_attenuation_ops, &attenuation, 0600);
MODULE_PARM_DESC(attenuation, "PCM divisor 1, 2 or 4; default 4; root-only live adjustment");

struct j171_audio {
 struct device *dev;
 void __iomem *mca, *adapter;
 struct clk *clock;
 struct gpio_desc *enable;
 bool prepared, running;
 struct snd_dmaengine_dai_dma_data dma;
 struct snd_soc_card card;
 struct snd_soc_dai_link link;
 struct snd_soc_dai_link_component cpu, codec, platform;
 struct snd_dmaengine_pcm_config pcm;
};
static void update(struct j171_audio *a, u32 off, u32 mask, u32 val)
{
 writel((readl(a->mca+off)&~mask)|(val&mask),a->mca+off);
}
static void j171_stop(struct j171_audio *a)
{
 gpiod_set_value(a->enable,0);
 if (a->prepared) update(a,0x300,1,0);
 a->running=false;
}
static void j171_release(struct j171_audio *a)
{
 if (!a->prepared) return;
 j171_stop(a);
 update(a,0x600,0xe,0);
 update(a,0x100,1,0);
 update(a,0x0,1,0);
 a->prepared=false;
 pm_runtime_put_sync(a->dev);
 clk_disable_unprepare(a->clock);
}
static int j171_prepare(struct snd_pcm_substream *s, struct snd_soc_dai *dai)
{
 struct j171_audio *a=snd_soc_dai_get_drvdata(dai);
 int ret;
 if (!READ_ONCE(test_enabled)) return -EACCES;
 if (a->prepared) return 0;
 /* 8 TDM slots of 16 bits: 6.144MHz BCLK at 48kHz. */
 ret=clk_set_rate(a->clock,6144000);
 if (ret) return ret;
 ret=clk_prepare_enable(a->clock);
 if (ret) return ret;
 ret=pm_runtime_resume_and_get(a->dev);
 if (ret<0) { clk_disable_unprepare(a->clock); return ret; }
 a->prepared=true;
 gpiod_set_value(a->enable,0);
 /* DSP_A: one-bit delay, 8 slots, first four enabled, native MCA1. */
 update(a,0x300,3,0);
 writel(0x100,a->mca+4);
 writel(0,a->mca+0x108);       /* frame sync asserted for one BCLK */
 writel(126,a->mca+0x10c);     /* rest of 128-bit frame */
 writel(2,a->mca+0x104);       /* NCO1 -> sync generator 1 */
 writel(0x27047,a->mca+0x304); /* sync=2, TX flags, width16, slots8 */
 writel(1,a->mca+0x308);
 writel(~0U,a->mca+0x30c);
 writel(~0xfU,a->mca+0x310);
 writel(~0U,a->mca+0x314);
 writel(~0xfU,a->mca+0x318);
 /* Asahi DMA adapter: four channels, 16-bit padding on each lane. */
 writel(0x405050,a->adapter);
 writel(0x200,a->mca+0x604);
 writel(4,a->mca+0x608);
 update(a,0x100,1,1);
 update(a,0,1,1);
 update(a,0x600,0xe,0xe);
 /* The native reset path holds LOW 10ms before release; stay muted here. */
 msleep(10);
 dev_dbg(a->dev,"speaker prepared: NCO=%lu ctrl=%#x sync=%#x tx=%#x port=%#x adapter=%#x; amp muted\n",
  clk_get_rate(a->clock),readl(a->mca),readl(a->mca+0x100),
  readl(a->mca+0x304),readl(a->mca+0x600),readl(a->adapter));
 return 0;
}
static int j171_trigger(struct snd_pcm_substream *s,int cmd,struct snd_soc_dai *dai)
{
 struct j171_audio *a=snd_soc_dai_get_drvdata(dai);
 switch(cmd) {
 case SNDRV_PCM_TRIGGER_START:
 case SNDRV_PCM_TRIGGER_RESUME:
 case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
  if (!a->prepared) return -EIO;
  update(a,0x300,3,2); udelay(25);
  update(a,0x300,3,0);
  update(a,0x304,0x70000,0);
  update(a,0x304,0x70000,0x20000);
  udelay(100);
  update(a,0x300,3,1);
  gpiod_set_value(a->enable,1);
  a->running=true;
  break;
 case SNDRV_PCM_TRIGGER_STOP:
 case SNDRV_PCM_TRIGGER_SUSPEND:
 case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
  j171_stop(a); break;
 default: return -EINVAL;
 }
 return 0;
}
static int j171_hw_free(struct snd_pcm_substream *s,struct snd_soc_dai *dai)
{ j171_release(snd_soc_dai_get_drvdata(dai)); return 0; }
static void j171_shutdown(struct snd_pcm_substream *s,struct snd_soc_dai *dai)
{ j171_release(snd_soc_dai_get_drvdata(dai)); }
static int j171_dai_probe(struct snd_soc_dai *dai)
{
 struct snd_soc_card *card=dev_get_drvdata(dai->dev);
 struct j171_audio *a=snd_soc_card_get_drvdata(card);
 snd_soc_dai_set_drvdata(dai,a);
 snd_soc_dai_init_dma_data(dai,&a->dma,NULL);
 return 0;
}
static int j171_startup(struct snd_pcm_substream *s,struct snd_soc_dai *dai)
{ return READ_ONCE(test_enabled) ? 0 : -EACCES; }
static const struct snd_soc_dai_ops j171_ops={
 .probe=j171_dai_probe,.startup=j171_startup,.prepare=j171_prepare,.trigger=j171_trigger,.hw_free=j171_hw_free,
 .shutdown=j171_shutdown,
};
static struct snd_soc_dai_driver j171_dai={
 .name="j171-speaker-cpu",.ops=&j171_ops,
 .playback={.stream_name="Speakers",.channels_min=4,.channels_max=4,
 .rates=SNDRV_PCM_RATE_48000,.formats=SNDRV_PCM_FMTBIT_S16_LE},
};
static const struct snd_soc_component_driver j171_component={.name="j171-speaker-cpu"};
static const struct snd_pcm_hardware j171_pcm_hw={
 .info=SNDRV_PCM_INFO_INTERLEAVED|SNDRV_PCM_INFO_BLOCK_TRANSFER,
 .formats=SNDRV_PCM_FMTBIT_S16_LE,.rates=SNDRV_PCM_RATE_48000,
 .rate_min=48000,.rate_max=48000,.channels_min=4,.channels_max=4,
 .buffer_bytes_max=128*1024,.period_bytes_min=1024,.period_bytes_max=32768,
 .periods_min=2,.periods_max=16,
};
/* No mmap: every user sample goes through the bounded bring-up attenuator. */
static int j171_attenuate(struct snd_pcm_substream *s,int channel,
 unsigned long off,unsigned long bytes)
{
 s16 *p=(s16 *)(s->runtime->dma_area+off);
 unsigned long i;
 int divisor = (int)READ_ONCE(attenuation);
 for(i=0;i<bytes/2;i++) p[i]/=divisor;
 return 0;
}
static int j171_probe(struct platform_device *pdev)
{
 struct device *dev=&pdev->dev;
 struct j171_audio *a;
 int ret;
 if (!of_machine_is_compatible("apple,j171a")) return -ENODEV;
 a=devm_kzalloc(dev,sizeof(*a),GFP_KERNEL); if (!a) return -ENOMEM;
 a->dev=dev; platform_set_drvdata(pdev,a);
 a->mca=devm_platform_ioremap_resource(pdev,0);
 if(IS_ERR(a->mca)) return PTR_ERR(a->mca);
 a->adapter=devm_platform_ioremap_resource(pdev,1);
 if(IS_ERR(a->adapter)) return PTR_ERR(a->adapter);
 a->clock=devm_clk_get(dev,NULL);
 if(IS_ERR(a->clock)) return dev_err_probe(dev,PTR_ERR(a->clock),"speaker clock\n");
 a->enable=devm_gpiod_get(dev,"enable",GPIOD_OUT_LOW);
 if(IS_ERR(a->enable)) return dev_err_probe(dev,PTR_ERR(a->enable),"speaker enable\n");
 if(gpiod_cansleep(a->enable)) return -EINVAL;
 ret=devm_pm_runtime_enable(dev); if(ret) return ret;
 a->dma.addr_width=DMA_SLAVE_BUSWIDTH_2_BYTES;
 a->dma.maxburst=192; a->dma.fifo_size=384;
 a->pcm.prepare_slave_config=snd_dmaengine_pcm_prepare_slave_config;
 a->pcm.pcm_hardware=&j171_pcm_hw;
 a->pcm.prealloc_buffer_size=128*1024;
 a->pcm.process=j171_attenuate;
 ret=devm_snd_soc_register_component(dev,&j171_component,&j171_dai,1);
 if(ret) return ret;
 ret=devm_snd_dmaengine_pcm_register(dev,&a->pcm,0); if(ret) return ret;
 a->cpu.of_node=dev->of_node; a->cpu.dai_name="j171-speaker-cpu";
 a->platform.of_node=dev->of_node;
 a->codec.name="snd-soc-dummy"; a->codec.dai_name="snd-soc-dummy-dai";
 a->link.name="J171 Speaker PCM"; a->link.stream_name="Speakers";
 a->link.cpus=&a->cpu; a->link.num_cpus=1;
 a->link.platforms=&a->platform; a->link.num_platforms=1;
 a->link.codecs=&a->codec; a->link.num_codecs=1;
 a->card.name="J171 Speakers"; a->card.owner=THIS_MODULE;
 a->card.dev=dev; a->card.dai_link=&a->link; a->card.num_links=1;
 snd_soc_card_set_drvdata(&a->card,a);
 ret=devm_snd_soc_register_card(dev,&a->card);
 if(!ret) dev_info(dev,"J171 speaker card: 48k/4ch/S16, runtime attenuation default 1/4; idle amp LOW\n");
 return ret;
}
static void j171_remove(struct platform_device *pdev)
{
 struct snd_soc_card *card=platform_get_drvdata(pdev);
 j171_release(snd_soc_card_get_drvdata(card));
}
static const struct of_device_id j171_match[]={
 {.compatible="cascadia,j171a-speakers"},
 {.compatible="cascadia,j171a-speaker-test"},{}
};
MODULE_DEVICE_TABLE(of,j171_match);
static struct platform_driver j171_driver={.probe=j171_probe,.remove=j171_remove,
 .driver={.name="j171-speakers",.of_match_table=j171_match}};
module_platform_driver(j171_driver);
MODULE_DESCRIPTION("J171a fixed-format speaker bring-up");
MODULE_LICENSE("Dual MIT/GPL");
