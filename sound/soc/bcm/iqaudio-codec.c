/*
 * ASoC Driver for IQaudIO Raspberry Pi Codec board
 *
 * Author:	Gordon Garrity <gordon@iqaudio.com>
 *		(C) Copyright IQaudio Limited, 2017-2019
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>

#include <linux/acpi.h>
#include <linux/slab.h>
#include "../codecs/da7213.h"

static int samplerate=44100;

static int snd_rpi_iqaudio_post_dapm_event(struct snd_soc_dapm_widget *w,
                              struct snd_kcontrol *kcontrol,
                              int event)
{
     switch (event) {
     case SND_SOC_DAPM_POST_PMU:
           /* Delay for mic bias ramp */
           msleep(200);
           break;
     default:
           break;
     }

     return 0;
}


static const struct snd_kcontrol_new controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Mic"),
	SOC_DAPM_PIN_SWITCH("Aux In"),
};

static const struct snd_soc_dapm_widget dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Mic", NULL),
	SND_SOC_DAPM_LINE("Aux In", NULL),
//	SND_SOC_DAPM_PRE("Pre Power Up Event", snd_rpi_iqaudio_post_dapm_event),
	SND_SOC_DAPM_POST("Post Power Up Event", snd_rpi_iqaudio_post_dapm_event),
};

static const struct snd_soc_dapm_route audio_map[] = {
	{"Headphone Jack", NULL, "HPL"},
	{"Headphone Jack", NULL, "HPR"},

	{"AUXL", NULL, "Aux In"},
	{"AUXR", NULL, "Aux In"},

	/* Assume Mic1 is linked to Headset and Mic2 to on-board mic */
	{"MIC1", NULL, "Headset Mic"},
	{"MIC2", NULL, "Mic"},

};

/* machine stream operations */

static int snd_rpi_iqaudio_codec_hw_params(struct snd_pcm_substream *substream,
			  struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_codec *codec = rtd->codec;		// Added for error message support
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;		// Added to support bclk_ratio

	int ret;

// Set clock frequency, onboard 11.2896MHz clock
	ret = snd_soc_dai_set_sysclk(codec_dai, DA7213_CLKSRC_MCLK,
				     11289600, SND_SOC_CLOCK_OUT);

	if (ret < 0)
		dev_err(codec_dai->dev, "can't set codec sysclk configuration\n");

	samplerate = params_rate(params);

// MASTER mode
// for 48 etc 		DA7213_PLL_FREQ_OUT_98304000
// for 44.1 etc 	DA7213_PLL_FREQ_OUT_90316800

	switch (samplerate) {
		case  8000:
		case 16000:
		case 32000:
		case 48000:
		case 96000:
		case 192000:
			ret = snd_soc_dai_set_pll(codec_dai, 0,
				DA7213_SYSCLK_PLL, 0, DA7213_PLL_FREQ_OUT_98304000);
			break;
		case 44100:
		case 88200:
		case 176400:
			ret = snd_soc_dai_set_pll(codec_dai, 0,
				DA7213_SYSCLK_PLL, 0, DA7213_PLL_FREQ_OUT_90316800);
			break;
		default:
			dev_err(codec->dev,"Failed to set DA7213 SYSCLK, unsupported samplerate %d\n", samplerate);
	}

	if (ret < 0) {
		dev_err(codec_dai->dev, "failed to start PLL: %d\n", ret);
		return -EIO;
	}

	// Set bclk ratio
	return snd_soc_dai_set_bclk_ratio(cpu_dai,64);
}

static const struct snd_soc_ops snd_rpi_iqaudio_codec_ops = {
	.hw_params = snd_rpi_iqaudio_codec_hw_params,

// Do not close down the PLL by default.
// Doing so would stop record if playback stopped
// or playback ir record is stopped.
//	.hw_free = snd_rpi_iqaudio_codec_hw_free,
};


static struct snd_soc_dai_link snd_rpi_iqaudio_codec_dai[] = {
{
	.cpu_dai_name 		= "bcm2708-i2s.0",
	.codec_dai_name 	= "da7213-hifi",
	.platform_name 		= "bmc2708-i2s.0",
	.codec_name 		= "da7213.1-001a",				// I2C addess 0x01a on Pi
	.dai_fmt 		= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
					| SND_SOC_DAIFMT_CBM_CFM,		// MASTER clock and frame
	.ops			= &snd_rpi_iqaudio_codec_ops,
    	.symmetric_rates	= 1,						// Added as per AT 15/5/17
    	.symmetric_channels	= 1,
    	.symmetric_samplebits	= 1,
},
};

/* audio machine driver */
static struct snd_soc_card snd_rpi_iqaudio_codec = {
	.owner			= THIS_MODULE,
	.dai_link		= snd_rpi_iqaudio_codec_dai,
	.num_links		= ARRAY_SIZE(snd_rpi_iqaudio_codec_dai),
	.dapm_widgets		= dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(dapm_widgets),
};

static int snd_rpi_iqaudio_codec_probe(struct platform_device *pdev)
{
	int ret = 0;

	snd_rpi_iqaudio_codec.dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_card *card = &snd_rpi_iqaudio_codec;
		struct snd_soc_dai_link *dai = &snd_rpi_iqaudio_codec_dai[0];

		i2s_node = of_parse_phandle(pdev->dev.of_node,
					    "i2s-controller", 0);
		if (i2s_node) {
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = i2s_node;
			dai->platform_name = NULL;
			dai->platform_of_node = i2s_node;
		}

		if (of_property_read_string(pdev->dev.of_node, "card_name",
					    &card->name))
			card->name = "IQaudIOCODEC";

		if (of_property_read_string(pdev->dev.of_node, "dai_name",
					    &dai->name))
			dai->name = "IQaudIO CODEC";

		if (of_property_read_string(pdev->dev.of_node,
					"dai_stream_name", &dai->stream_name))
			dai->stream_name = "IQaudIO CODEC HiFi v1";

	}

	ret = snd_soc_register_card(&snd_rpi_iqaudio_codec);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"snd_soc_register_card() failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int snd_rpi_iqaudio_codec_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_rpi_iqaudio_codec);
}

static const struct of_device_id iqaudio_of_match[] = {
	{ .compatible = "iqaudio,iqaudio-codec", },
	{},
};

MODULE_DEVICE_TABLE(of, iqaudio_of_match);

static struct platform_driver snd_rpi_iqaudio_codec_driver = {
	.driver = {
		.name   = "snd-rpi-iqaudio-codec",
		.owner  = THIS_MODULE,
		.of_match_table = iqaudio_of_match,
	},
	.probe          = snd_rpi_iqaudio_codec_probe,
	.remove         = snd_rpi_iqaudio_codec_remove,
};



module_platform_driver(snd_rpi_iqaudio_codec_driver);

MODULE_AUTHOR("Gordon Garrity <gordon@iqaudio.com>");
MODULE_DESCRIPTION("ASoC Driver for IQaudIO CODEC");
MODULE_LICENSE("GPL v2");
