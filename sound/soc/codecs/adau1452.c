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
#include <linux/delay.h>
#include <asm/unaligned.h>
#include <linux/regmap.h>

#include "sigmadsp.h"

#define ADAU1452_FIRMWARE "adau1452.bin"

struct adau1452 {
	struct sigmadsp *sigmadsp;
	struct regmap *regmap;
	/* board configuration read from DT */
	bool use_pll;
	u32 pll_feedback;
	u32 pll_prescale;
	bool start_core_on_probe;
	u32 mclk_out;
};

static const struct regmap_config adau1452_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.max_register = 0xffff,
	.cache_type = REGCACHE_NONE,
};

/* ADAU1452 safeload register addresses */
#define ADAU1452_SAFELOAD_DATA(i) (0x6000 + (i))
#define ADAU1452_SAFELOAD_ADDR   0x6005
#define ADAU1452_SAFELOAD_NUM    0x6006

/* Control register addresses used for basic bring-up */
#define ADAU1452_PLL_CTRL0   0xF000
#define ADAU1452_PLL_CTRL1   0xF001
#define ADAU1452_PLL_CLK_SRC 0xF002
#define ADAU1452_PLL_ENABLE  0xF003
#define ADAU1452_PLL_LOCK    0xF004
#define ADAU1452_MCLK_OUT    0xF005
#define ADAU1452_START_PULSE 0xF401
#define ADAU1452_START_CORE  0xF402
#define ADAU1452_KILL_CORE   0xF403
#define ADAU1452_START_ADDR  0xF404

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

/* Write a 16-bit control register (big-endian) via sigmadsp write */
static int adau1452_write_reg16(struct adau1452 *chip, unsigned int reg,
					unsigned int value)
{
	int ret = 0;

	if (!chip)
		return -ENODEV;

	if (chip->regmap) {
		/* regmap expects reg and 32-bit val; convert to 16-bit aligned write */
		ret = regmap_raw_write(chip->regmap, reg, (const void *)&value, 2);
		if (ret)
			dev_err(chip->sigmadsp->dev, "regmap write 0x%04x failed: %d\n", reg, ret);
		return ret;
	}

	/* fallback: use sigmadsp write callback */
	if (!chip->sigmadsp || !chip->sigmadsp->write)
		return -ENODEV;

	{
		uint8_t buf[2];
		buf[0] = (value >> 8) & 0xff;
		buf[1] = value & 0xff;
		ret = chip->sigmadsp->write(chip->sigmadsp->control_data, reg, buf, 2);
		if (ret)
			dev_err(chip->sigmadsp->dev, "write_reg16 0x%04x = 0x%04x failed: %d\n",
					reg, value, ret);
	}

	return ret;
}

/* Sysfs: echo "addr value" > write_reg to write a 16-bit control reg */
static ssize_t write_reg_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct adau1452 *chip = i2c_get_clientdata(client);
	unsigned int addr = 0;
	unsigned int val = 0;

	if (!chip || !chip->sigmadsp)
		return -ENODEV;

	if (sscanf(buf, "%x %x", &addr, &val) < 2)
		return -EINVAL;

	if (addr < 0xF000 || addr > 0xFFFF) {
		dev_warn(dev, "write_reg: addr 0x%x outside control region\n", addr);
	}

	if (val > 0xffff)
		return -EINVAL;

	if (adau1452_write_reg16(chip, addr, val))
		return -EIO;

	dev_info(dev, "wrote reg 0x%04x = 0x%04x\n", addr, val);

	return count;
}

static DEVICE_ATTR_WO(write_reg);

/* Safeload implementation for ADAU1452: write up to 5 words per batch */
static int adau1452_safeload(struct sigmadsp *sigmadsp, unsigned int addr,
		const uint8_t *bytes, size_t len)
{
	struct i2c_client *client = to_i2c_client(sigmadsp->dev);
	struct adau1452 *chip = i2c_get_clientdata(client);
	unsigned int words = (len + 1) / 2; /* number of 16-bit words */
	unsigned int written = 0;
	int ret = 0;

	/* We will write in batches of up to 5 words (slots 0..4) */
	while (written < words) {
		unsigned int batch = min(words - written, 5u);
		unsigned int i;

		if (chip && chip->regmap) {
			/* Use regmap raw writes for safeload slots */
			for (i = 0; i < batch; i++) {
				uint16_t w = 0;
				uint8_t data[2];

				if ((written + i) * 2 + 1 < len) {
					w = (bytes[(written + i) * 2] << 8) |
						bytes[(written + i) * 2 + 1];
				} else if ((written + i) * 2 < len) {
					w = (bytes[(written + i) * 2] << 8);
				}
				data[0] = (w >> 8) & 0xff;
				data[1] = w & 0xff;
				ret = regmap_raw_write(chip->regmap, ADAU1452_SAFELOAD_DATA(i), data, 2);
				if (ret)
					return ret;
			}

			/* write address and num via regmap */
			{
				uint8_t a[2];
				put_unaligned_be16((u16)(addr + written), a);
				ret = regmap_raw_write(chip->regmap, ADAU1452_SAFELOAD_ADDR, a, 2);
				if (ret)
					return ret;
			}
			{
				uint8_t n[2];
				put_unaligned_be16((u16)batch, n);
				ret = regmap_raw_write(chip->regmap, ADAU1452_SAFELOAD_NUM, n, 2);
				if (ret)
					return ret;
			}

		} else {
			uint8_t buf[4];

			/* fallback to raw i2c transfers (existing behaviour) */
			for (i = 0; i < batch; i++) {
				unsigned int slot_addr = ADAU1452_SAFELOAD_DATA(i);
				uint16_t w = 0;

				if ((written + i) * 2 + 1 < len) {
					w = (bytes[(written + i) * 2] << 8) |
						bytes[(written + i) * 2 + 1];
				} else if ((written + i) * 2 < len) {
					w = (bytes[(written + i) * 2] << 8);
				} else {
					w = 0;
				}

				put_unaligned_be16(slot_addr, buf);
				buf[2] = (w >> 8) & 0xff;
				buf[3] = w & 0xff;
				ret = i2c_master_send(client, buf, 4);
				if (ret < 0)
					return ret;
				else if (ret != 4)
					return -EIO;
			}

			/* write the target address for this batch */
			{
				uint8_t a[4];
				put_unaligned_be16(ADAU1452_SAFELOAD_ADDR, a);
				put_unaligned_be16((u16)(addr + written), a + 2);
				ret = i2c_master_send(client, a, 4);
				if (ret < 0)
					return ret;
				else if (ret != 4)
					return -EIO;
			}

			/* write number of words to load (and trigger) */
			{
				uint8_t n[4];
				put_unaligned_be16(ADAU1452_SAFELOAD_NUM, n);
				put_unaligned_be16((u16)batch, n + 2);
				ret = i2c_master_send(client, n, 4);
				if (ret < 0)
					return ret;
				else if (ret != 4)
					return -EIO;
			}
		}

		written += batch;
	}

	return 0;
}

static const struct sigmadsp_ops adau1452_sigmadsp_ops = {
	.safeload = adau1452_safeload,
};

/* Apply basic board configuration (pll, mclk out, start core) read from DT */
static void adau1452_apply_board_setup(struct i2c_client *client,
		struct adau1452 *chip)
{
	int ret;
	struct device *dev = &client->dev;

	if (chip->use_pll) {
		/* program PLL dividers if provided */
		dev_info(dev, "adau1452: configuring PLL fb=%u prescale=%u\n",
				chip->pll_feedback, chip->pll_prescale);
		if (chip->pll_feedback)
			adau1452_write_reg16(chip, ADAU1452_PLL_CTRL0, chip->pll_feedback);
		if (chip->pll_prescale)
			adau1452_write_reg16(chip, ADAU1452_PLL_CTRL1, chip->pll_prescale);

		/* enable PLL */
		adau1452_write_reg16(chip, ADAU1452_PLL_ENABLE, 0x0001);

		/* wait for PLL lock */
		{
			int i;
			for (i = 0; i < 100; i++) {
				uint8_t buf[2];
				if (chip->regmap)
					ret = regmap_raw_read(chip->regmap, ADAU1452_PLL_LOCK, buf, 2);
				else if (chip->sigmadsp && chip->sigmadsp->read)
					ret = chip->sigmadsp->read(chip->sigmadsp->control_data,
							ADAU1452_PLL_LOCK, buf, 2);
				else
					ret = -ENOSYS;

				if (!ret) {
					if (buf[1] || buf[0]) /* non-zero lock */
						break;
				}
				msleep(1);
			}
			if (ret)
				dev_warn(dev, "adau1452: failed to read PLL_LOCK: %d\n", ret);
			else if (i == 100)
				dev_warn(dev, "adau1452: PLL_LOCK timeout\n");
		}
	}

	if (chip->mclk_out)
		adau1452_write_reg16(chip, ADAU1452_MCLK_OUT, chip->mclk_out);

	if (chip->start_core_on_probe) {
		/* set START_ADDRESS to a common program address if not zero */
		/* Common SigmaStudio export used 0xC000 in your bin; write that */
		adau1452_write_reg16(chip, ADAU1452_START_ADDR, 0xC000);
		/* trigger START_CORE */
		adau1452_write_reg16(chip, ADAU1452_START_CORE, 0x0001);
		dev_info(dev, "adau1452: started core (START_CORE)");
	}
}

static int adau1452_probe(struct i2c_client *client,
                          const struct i2c_device_id *id)
{
	struct adau1452 *chip;
	int ret;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	/* parse optional DT properties for basic board setup */
	if (client->dev.of_node) {
		of_property_read_bool(client->dev.of_node, "adi,use-pll");
		chip->use_pll = of_property_read_bool(client->dev.of_node, "adi,use-pll");
		of_property_read_u32(client->dev.of_node, "adi,pll-feedback", &chip->pll_feedback);
		of_property_read_u32(client->dev.of_node, "adi,pll-prescale", &chip->pll_prescale);
		chip->start_core_on_probe = of_property_read_bool(client->dev.of_node, "adi,start-core-on-probe");
		of_property_read_u32(client->dev.of_node, "adi,mclk-out", &chip->mclk_out);
	}

	/* initialize regmap over raw i2c access so we can do regmap writes */
	chip->regmap = devm_regmap_init(&client->dev, NULL, client, &adau1452_regmap_config);
	if (IS_ERR(chip->regmap))
		chip->regmap = NULL;

	/* Initialize sigmadsp instance for this device, which loads firmware */
#ifdef CONFIG_SND_SOC_SIGMADSP_REGMAP
	if (chip->regmap)
		chip->sigmadsp = devm_sigmadsp_init_regmap(&client->dev, chip->regmap,
				&adau1452_sigmadsp_ops, ADAU1452_FIRMWARE);
	else
#endif
		chip->sigmadsp = devm_sigmadsp_init_i2c(client, &adau1452_sigmadsp_ops, ADAU1452_FIRMWARE);
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

	/* Apply board configuration (pll, mclk, start core) if present */
	adau1452_apply_board_setup(client, chip);

	/* create debug sysfs file to read DSP memory */
	ret = device_create_file(&client->dev, &dev_attr_read_mem);
	if (ret)
		dev_warn(&client->dev, "failed to create read_mem sysfs: %d\n", ret);

	/* create write_reg for bringing up control registers */
	ret = device_create_file(&client->dev, &dev_attr_write_reg);
	if (ret)
		dev_warn(&client->dev, "failed to create write_reg sysfs: %d\n", ret);

	return 0;
}

static int adau1452_remove(struct i2c_client *client)
{
	/* sigmadsp resources are managed by devres; nothing to do */
	device_remove_file(&client->dev, &dev_attr_read_mem);
	device_remove_file(&client->dev, &dev_attr_write_reg);
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
