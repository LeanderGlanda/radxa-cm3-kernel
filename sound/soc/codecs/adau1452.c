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
	unsigned int boot_samplerate;
};

/* forward declarations for register helpers */
static int adau1452_write_reg16(struct adau1452 *chip, unsigned int reg, unsigned int value);

/* forward declare regmap callbacks */
static int adau1452_reg_write(void *context, unsigned int reg, unsigned int value);
static int adau1452_reg_read(void *context, unsigned int reg, unsigned int *value);

static const struct regmap_config adau1452_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.max_register = 0xffff,
	.cache_type = REGCACHE_NONE,
	.reg_write = adau1452_reg_write,
	.reg_read = adau1452_reg_read,
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
/* Hibernate register */
#define ADAU1452_HIBERNATE   0xF400

/* Minimal DAI ops: no real hardware config, accept common formats/rates */
static int adau1452_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	/* Firmware upload is performed in component probe / attach
	 * to ensure sigmadsp->component is set before sigmadsp_setup()
	 */
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

static int adau1452_component_probe(struct snd_soc_component *component)
{
	struct i2c_client *client = to_i2c_client(component->dev);
	struct adau1452 *chip = i2c_get_clientdata(client);
	int ret = 0;

	if (!chip || !chip->sigmadsp)
		return -ENODEV;

	/* attach the parsed firmware to this component so controls are created */
	ret = sigmadsp_attach(chip->sigmadsp, component);
	if (ret) {
		dev_err(component->dev, "sigmadsp_attach failed: %d\n", ret);
		return ret;
	}

	dev_info(component->dev, "adau1452: sigmadsp attached\n");

	/* store chip pointer for later retrieval via snd_soc_component_get_drvdata */
	snd_soc_component_set_drvdata(component, chip);

	/* If boot_samplerate was provided, upload program/param RAM now that
	 * the component is attached and sigmadsp->component is valid. This
	 * prevents sigmadsp_activate_ctrl from dereferencing NULL.
	 */
	if (chip->boot_samplerate) {
		ret = sigmadsp_setup(chip->sigmadsp, chip->boot_samplerate);
		if (ret)
			dev_warn(component->dev, "sigmadsp_setup failed in component_probe: %d\n", ret);
	}

	/* If requested via DT, start the DSP core now (START sequence) */
	if (chip->start_core_on_probe) {
		if (adau1452_write_reg16(chip, ADAU1452_START_ADDR, 0xC000))
			dev_warn(component->dev, "failed to write START_ADDR\n");
		if (adau1452_write_reg16(chip, ADAU1452_START_PULSE, 0x0002))
			dev_warn(component->dev, "failed to write START_PULSE\n");
		if (adau1452_write_reg16(chip, ADAU1452_KILL_CORE, 0x0000))
			dev_warn(component->dev, "failed to clear KILL_CORE\n");
		if (adau1452_write_reg16(chip, ADAU1452_START_CORE, 0x0000))
			dev_warn(component->dev, "failed to write START_CORE=0\n");
		if (adau1452_write_reg16(chip, ADAU1452_START_CORE, 0x0001))
			dev_warn(component->dev, "failed to write START_CORE=1\n");
		usleep_range(50, 100);
		if (adau1452_write_reg16(chip, ADAU1452_HIBERNATE, 0x0000))
			dev_warn(component->dev, "failed to clear HIBERNATE after start\n");
		dev_info(component->dev, "adau1452: core started via DT auto-start\n");
	}

	return ret;
}


static const struct snd_soc_component_driver adau1452_component_driver = {
	.name = "adau1452",
	.probe = adau1452_component_probe,
};

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

	if (!chip->regmap)
		return -ENODEV;

	/* write 16-bit control register via regmap API */
	ret = regmap_write(chip->regmap, reg, (u16)value);
	if (ret)
		dev_err(chip->sigmadsp ? chip->sigmadsp->dev : NULL,
				"regmap write 0x%04x failed: %d\n", reg, ret);

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

	if (!chip || !chip->regmap)
		return -ENODEV;

	/* We will write in batches of up to 5 words (slots 0..4) */
	while (written < words) {
		unsigned int batch = min(words - written, 5u);
		unsigned int i;

		for (i = 0; i < batch; i++) {
			uint16_t w = 0;

			if ((written + i) * 2 + 1 < len) {
				w = (bytes[(written + i) * 2] << 8) |
					bytes[(written + i) * 2 + 1];
			} else if ((written + i) * 2 < len) {
				w = (bytes[(written + i) * 2] << 8);
			}

			ret = regmap_write(chip->regmap, ADAU1452_SAFELOAD_DATA(i), w);
			if (ret)
				return ret;
		}

		/* write address and num via regmap */
		ret = regmap_write(chip->regmap, ADAU1452_SAFELOAD_ADDR, (u16)(addr + written));
		if (ret)
			return ret;
		ret = regmap_write(chip->regmap, ADAU1452_SAFELOAD_NUM, (u16)batch);
		if (ret)
			return ret;

		written += batch;
	}

	return 0;
}

static const struct sigmadsp_ops adau1452_sigmadsp_ops = {
	.safeload = adau1452_safeload,
};

/* regmap callbacks: 16-bit BE register address, 16-bit BE value */
static int adau1452_reg_write(void *context, unsigned int reg, unsigned int value)
{
	struct i2c_client *client = context;
	uint8_t buf[4];
	int ret;

	/* pack register addr (be16) and value (be16) */
	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	buf[2] = (value >> 8) & 0xff;
	buf[3] = value & 0xff;

	ret = i2c_master_send(client, buf, 4);
	if (ret < 0)
		return ret;
	else if (ret != 4)
		return -EIO;

	return 0;
}

static int adau1452_reg_read(void *context, unsigned int reg, unsigned int *value)
{
	struct i2c_client *client = context;
	uint8_t send_buf[2];
	uint8_t recv_buf[2];
	struct i2c_msg msgs[2];
	int ret;

	send_buf[0] = reg >> 8;
	send_buf[1] = reg & 0xff;

	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = sizeof(send_buf);
	msgs[0].buf = send_buf;

	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = sizeof(recv_buf);
	msgs[1].buf = recv_buf;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	else if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*value = (recv_buf[0] << 8) | recv_buf[1];
	return 0;
}

/* Apply basic board configuration (pll, mclk out, start core) read from DT */
static void adau1452_apply_board_setup(struct i2c_client *client,
		struct adau1452 *chip)
{
	int ret;
	struct device *dev = &client->dev;
	/* Follow datasheet recommended sequence for safe program/param loading */
	/* 1) Kill core / Hibernate sequence to stop DSP while programming large RAM */
	if (adau1452_write_reg16(chip, ADAU1452_HIBERNATE, 0x0000))
		dev_warn(dev, "failed to clear HIBERNATE before set\n");
	if (adau1452_write_reg16(chip, ADAU1452_HIBERNATE, 0x0001))
		dev_warn(dev, "failed to set HIBERNATE\n");

	/* stop core */
	if (adau1452_write_reg16(chip, ADAU1452_KILL_CORE, 0x0001))
		dev_warn(dev, "failed to set KILL_CORE\n");

	/* 2) Program PLL and MCLK settings (write MCLK_OUT before enabling PLL) */
	if (chip->pll_feedback) {
		if (adau1452_write_reg16(chip, ADAU1452_PLL_CTRL0, chip->pll_feedback))
			dev_warn(dev, "failed to write PLL_CTRL0\n");
	} else {
		if (adau1452_write_reg16(chip, ADAU1452_PLL_CTRL0, 0x0060))
			dev_warn(dev, "failed to write default PLL_CTRL0\n");
	}

	/* user requested value for PLL_CTRL1 (divide by 8 -> 0x0003) */
	if (adau1452_write_reg16(chip, ADAU1452_PLL_CTRL1, chip->pll_prescale ? chip->pll_prescale : 0x0003))
		dev_warn(dev, "failed to write PLL_CTRL1\n");

	/* set PLL clock source to PLL (0x0001) */
	if (adau1452_write_reg16(chip, ADAU1452_PLL_CLK_SRC, 0x0001))
		dev_warn(dev, "failed to write PLL_CLK_SRC\n");

	/* MCLK_OUT: write before enabling PLL. Use user-provided mclk_out or default 0x0007 */
	if (adau1452_write_reg16(chip, ADAU1452_MCLK_OUT, chip->mclk_out ? chip->mclk_out : 0x0007))
		dev_warn(dev, "failed to write MCLK_OUT\n");

	/* 3) Enable PLL */
	if (adau1452_write_reg16(chip, ADAU1452_PLL_ENABLE, 0x0001))
		dev_warn(dev, "failed to write PLL_ENABLE\n");

	/* 4) Wait for PLL lock (max ~11ms per datasheet); poll PLL_LOCK */
	{
		int i;
		unsigned int val = 0;
		for (i = 0; i < 200; i++) {
			ret = regmap_read(chip->regmap, ADAU1452_PLL_LOCK, &val);
			if (!ret) {
				if (val) /* non-zero lock */
					break;
			}
			usleep_range(100, 200); /* wait a bit */
		}
		if (ret)
			dev_warn(dev, "adau1452: failed to read PLL_LOCK: %d\n", ret);
		else if (i == 200)
			dev_warn(dev, "adau1452: PLL_LOCK timeout\n");
	}

	/* 5) Power enable registers (optional) - leave defaults for safety */

	/* 6) Program/parameter RAM upload and core START are performed in
	 * component_probe after sigmadsp_attach to ensure controls are active.
	 */
	dev_info(dev, "adau1452: PLL configuration applied; deferring RAM upload/start\n");
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
		of_property_read_u32(client->dev.of_node, "adi,boot-samplerate", &chip->boot_samplerate);
	}

	/* initialize regmap over i2c access; regmap is required */
	chip->regmap = devm_regmap_init(&client->dev, NULL, client, &adau1452_regmap_config);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(&client->dev, "failed to init regmap: %d\n", ret);
		return ret;
	}

	/* Use the i2c-based sigmadsp initializer to avoid a hard dependency
	 * on the optional regmap helper symbol which may not be present in
	 * all kernel builds. The regmap callbacks implemented above are
	 * still used for regmap-based register access.
	 */
	chip->sigmadsp = devm_sigmadsp_init_i2c(client, &adau1452_sigmadsp_ops,
			ADAU1452_FIRMWARE);
	if (IS_ERR(chip->sigmadsp)) {
		ret = PTR_ERR(chip->sigmadsp);
		dev_err(&client->dev, "failed to init sigmadsp: %d\n", ret);
		return ret;
	}

    /* store pointer for future use */
    i2c_set_clientdata(client, chip);

    dev_info(&client->dev, "ADAU1452 firmware loader initialized: pll_feedback=0x%x pll_prescale=0x%x mclk_out=0x%x boot_samplerate=%u start_core_on_probe=%d\n",
	    chip->pll_feedback, chip->pll_prescale, chip->mclk_out, chip->boot_samplerate, chip->start_core_on_probe);

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
