// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimal driver for ADAU1452 SigmaDSP
 * Only performs firmware loading via sigmadsp helper for I2C
 *
 * Copyright 2026
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <sound/soc.h>

#include <linux/slab.h>
#include <linux/device.h>

#include "sigmadsp.h"

#define ADAU1452_FIRMWARE "adau1452.bin"

struct adau1452 {
	struct sigmadsp *sigmadsp;
};

/* Minimal DAI ops: no real hardware config, accept common formats/rates */
static int adau1452_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct i2c_client *client = to_i2c_client(dai->dev);
	struct adau1452 *chip = i2c_get_clientdata(client);
	unsigned int rate = params_rate(params);
	int ret;

	if (!chip || !chip->sigmadsp)
		return -ENODEV;

	dev_info(&client->dev, "adau1452 hw_params: rate=%u\n", rate);

	/* Ensure firmware/program memory is written to the DSP for this rate */
	ret = sigmadsp_setup(chip->sigmadsp, rate);
	if (ret) {
		dev_err(&client->dev, "sigmadsp_setup failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int adau1452_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	/* No special format handling for this minimal driver */
	return 0;
}

static const struct snd_soc_dai_ops adau1452_dai_ops = {
	.set_fmt = adau1452_set_fmt,
	.hw_params = adau1452_hw_params,
};

static struct snd_soc_dai_driver adau1452_dai = {
	.name = "adau1452-hw",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_192000,
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = 0,
		.formats = 0,
	},
	.ops = &adau1452_dai_ops,
};

static int adau1452_component_probe(struct snd_soc_component *component);

static const struct snd_soc_component_driver adau1452_component_driver = {
	.name = "adau1452",
	.probe = adau1452_component_probe,
};

static int adau1452_component_probe(struct snd_soc_component *component)
{
	struct i2c_client *client = to_i2c_client(component->dev);
	struct adau1452 *chip = i2c_get_clientdata(client);
	int ret;

	if (!chip || !chip->sigmadsp)
		return -ENODEV;

	/* attach the parsed firmware to this component so controls are created */
	ret = sigmadsp_attach(chip->sigmadsp, component);
	if (ret)
		dev_err(component->dev, "sigmadsp_attach failed: %d\n", ret);

	/* store chip pointer for later retrieval via snd_soc_component_get_drvdata */
	snd_soc_component_set_drvdata(component, chip);

	return ret;
}

/* Sysfs: echo "addr len" > read_mem to dump dsp memory via sigmadsp read */
static ssize_t read_mem_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct adau1452 *chip = i2c_get_clientdata(client);
	unsigned int addr = 0;
	unsigned int len = 0;
	uint8_t *bufmem;
	int ret, i;

	if (!chip || !chip->sigmadsp)
		return -ENODEV;

	if (sscanf(buf, "%x %u", &addr, &len) < 1)
		return -EINVAL;

	if (len == 0 || len > 1024)
		return -EINVAL;

	if (!chip->sigmadsp->read) {
		dev_err(dev, "sigmadsp read callback not available\n");
		return -ENOSYS;
	}

	bufmem = kmalloc(len, GFP_KERNEL);
	if (!bufmem)
		return -ENOMEM;

	ret = chip->sigmadsp->read(chip->sigmadsp->control_data, addr, bufmem, len);
	if (ret) {
		dev_err(dev, "sigmadsp read failed: %d\n", ret);
		kfree(bufmem);
		return ret;
	}

	dev_info(dev, "read_mem addr=0x%x len=%u\n", addr, len);
	for (i = 0; i < (int)len; i += 16) {
		int j, chunk = ((len - i) < 16) ? (len - i) : 16;
		char line[128];
		char *p = line;
		p += sprintf(p, "%04x: ", addr + i);
		for (j = 0; j < chunk; j++)
			p += sprintf(p, "%02x ", bufmem[i + j]);
		dev_info(dev, "%s", line);
	}

	kfree(bufmem);
	return count;
}

static DEVICE_ATTR_WO(read_mem);

static int adau1452_probe(struct i2c_client *client,
                          const struct i2c_device_id *id)
{
	struct adau1452 *chip;
	int ret;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	/* Initialize sigmadsp instance for this device, which loads firmware */
	chip->sigmadsp = devm_sigmadsp_init_i2c(client, NULL, ADAU1452_FIRMWARE);
	if (IS_ERR(chip->sigmadsp)) {
		ret = PTR_ERR(chip->sigmadsp);
		dev_err(&client->dev, "failed to init sigmadsp: %d\n", ret);
		return ret;
	}

	/* store pointer for future use */
	i2c_set_clientdata(client, chip);

	dev_info(&client->dev, "ADAU1452 firmware loader initialized\n");

	/* register a minimal component/DAI so simple-audio-card can use it */
	ret = devm_snd_soc_register_component(&client->dev,
			&adau1452_component_driver, &adau1452_dai, 1);
	if (ret) {
		dev_err(&client->dev, "failed to register component: %d\n", ret);
		return ret;
	}

	/* create debug sysfs file to read DSP memory */
	ret = device_create_file(&client->dev, &dev_attr_read_mem);
	if (ret)
		dev_warn(&client->dev, "failed to create read_mem sysfs: %d\n", ret);

	return 0;
}

static int adau1452_remove(struct i2c_client *client)
{
	/* sigmadsp resources are managed by devres; nothing to do */
	device_remove_file(&client->dev, &dev_attr_read_mem);
	return 0;
}


static const struct i2c_device_id adau1452_id[] = {
	{ "adau1452", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, adau1452_id);

#ifdef CONFIG_OF
static const struct of_device_id adau1452_of_match[] = {
	{ .compatible = "adi,adau1452" },
	{ }
};
MODULE_DEVICE_TABLE(of, adau1452_of_match);
#endif

static struct i2c_driver adau1452_i2c_driver = {
	.driver = {
		.name = "adau1452",
		.of_match_table = of_match_ptr(adau1452_of_match),
	},
	.probe = adau1452_probe,
	.remove = adau1452_remove,
	.id_table = adau1452_id,
};

module_i2c_driver(adau1452_i2c_driver);

MODULE_DESCRIPTION("Minimal ADAU1452 SigmaDSP firmware loader");
MODULE_AUTHOR("Generated");
MODULE_LICENSE("GPL");
