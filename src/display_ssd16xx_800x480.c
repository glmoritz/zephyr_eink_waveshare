#define DT_DRV_COMPAT custom_ssd16xx_800x480

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(custom_ssd16xx_800x480, CONFIG_DISPLAY_LOG_LEVEL);

#define CUSTOM_SSD16XX_BUSY_TIMEOUT_MS    2000U
#define CUSTOM_SSD16XX_REFRESH_TIMEOUT_MS 15000U

/* SSD1677 / SSD1681-family command register addresses */
#define SSD16XX_CMD_SOFT_RESET            0x12
#define SSD16XX_CMD_AUTO_WRITE_RED_RAM    0x46
#define SSD16XX_CMD_AUTO_WRITE_BW_RAM     0x47
#define SSD16XX_CMD_DRIVER_OUTPUT_CTRL    0x01
#define SSD16XX_CMD_GATE_VOLTAGE          0x03
#define SSD16XX_CMD_SOURCE_VOLTAGE        0x04
#define SSD16XX_CMD_BOOSTER_SOFT_START    0x0C
#define SSD16XX_CMD_DATA_ENTRY_MODE       0x11
#define SSD16XX_CMD_BORDER_WAVEFORM       0x3C
#define SSD16XX_CMD_TEMP_SENSOR_CTRL      0x18
#define SSD16XX_CMD_VCOM_VOLTAGE          0x2C
#define SSD16XX_CMD_DISPLAY_OPTION        0x37
#define SSD16XX_CMD_WRITE_LUT             0x32
#define SSD16XX_CMD_RAM_X_WINDOW          0x44
#define SSD16XX_CMD_RAM_Y_WINDOW          0x45
#define SSD16XX_CMD_RAM_X_COUNTER         0x4E
#define SSD16XX_CMD_RAM_Y_COUNTER         0x4F
#define SSD16XX_CMD_WRITE_BW_RAM          0x24
#define SSD16XX_CMD_WRITE_RED_RAM         0x26
#define SSD16XX_CMD_DISP_UPDATE_CTRL2     0x22
#define SSD16XX_CMD_MASTER_ACTIVATION     0x20
#define SSD16XX_CMD_DEEP_SLEEP            0x10

/*
 * 4-gray waveform LUT from the Waveshare 3.7" SSD16xx-family panel.
 * Same controller family as our 3.97" panel. Send via SSD16XX_CMD_WRITE_LUT (105 bytes).
 * After loading, use SSD16XX_CMD_DISP_UPDATE_CTRL2 = 0xCF (custom LUT, 4-gray mode 2),
 * NOT 0xF7 (OTP LUT, which only uses the BW RAM and ignores the RED RAM).
 */
static const uint8_t lut_4gray_gc[105] = {
	0x2A, 0x06, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 1 */
	0x28, 0x06, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 2 */
	0x20, 0x06, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 3 */
	0x14, 0x06, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 4 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5 */
	0x00, 0x02, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x08, 0x08, 0x02, /* 6 */
	0x00, 0x02, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 7 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 8 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 9 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 10 */
	0x22, 0x22, 0x22, 0x22, 0x22,                               /* gate */
};

/* Fixed geometry for this 800x480 driver */
#define CUSTOM_SSD16XX_COLS      (800U / 8U)  /* 100 bytes per row */
#define CUSTOM_SSD16XX_ROWS      480U
#define CUSTOM_SSD16XX_BUF_BYTES (CUSTOM_SSD16XX_COLS * CUSTOM_SSD16XX_ROWS) /* 48000 */

struct custom_ssd16xx_config {
	const struct device *mipi_dev;
	struct mipi_dbi_config dbi_config;
	struct gpio_dt_spec busy_gpio;
	uint16_t width;
	uint16_t height;
};

struct custom_ssd16xx_data {
	bool blanking_on;
	/* 4-gray (2bpp): bit1 goes to BW RAM (0x24), bit0 goes to RED RAM (0x26)
	 * Actual buffers live in SPIRAM (.ext_ram.bss), declared per-instance below.
	 */
	uint8_t *bw_plane;
	uint8_t *red_plane;
};

static int custom_ssd16xx_busy_wait(const struct device *dev, uint32_t timeout_ms)
{
	const struct custom_ssd16xx_config *config = dev->config;
	int pin = gpio_pin_get_dt(&config->busy_gpio);
	uint32_t elapsed = 0;

	if (pin < 0) {
		LOG_ERR("Failed to read BUSY GPIO (%d)", pin);
		return pin;
	}

	while (pin > 0) {
		if (elapsed >= timeout_ms) {
			LOG_ERR("EPD BUSY timeout after %u ms", timeout_ms);
			return -ETIMEDOUT;
		}

		k_msleep(5);
		elapsed += 5;
		pin = gpio_pin_get_dt(&config->busy_gpio);
		if (pin < 0) {
			LOG_ERR("Failed to read BUSY GPIO (%d)", pin);
			return pin;
		}
	}

	return 0;
}

/* Wait for BUSY, then send command + data */
static int custom_ssd16xx_write_cmd(const struct device *dev, uint8_t cmd,
					const uint8_t *data, size_t len)
{
	const struct custom_ssd16xx_config *config = dev->config;
	int err;

	err = custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_BUSY_TIMEOUT_MS);
	if (err < 0) {
		return err;
	}

	err = mipi_dbi_command_write(config->mipi_dev, &config->dbi_config, cmd, data, len);
	mipi_dbi_release(config->mipi_dev, &config->dbi_config);
	return err;
}

/* Send command + data WITHOUT a busy-wait (for bulk pixel streaming) */
static int custom_ssd16xx_write_raw(const struct device *dev, uint8_t cmd,
					const uint8_t *data, size_t len)
{
	const struct custom_ssd16xx_config *config = dev->config;
	int err = mipi_dbi_command_write(config->mipi_dev, &config->dbi_config, cmd, data, len);

	mipi_dbi_release(config->mipi_dev, &config->dbi_config);
	return err;
}

static int custom_ssd16xx_panel_init(const struct device *dev)
{
	const struct custom_ssd16xx_config *config = dev->config;
	uint8_t data[8];
	int err;

	err = mipi_dbi_reset(config->mipi_dev, 10);
	if (err < 0) {
		return err;
	}

	k_msleep(10);
	err = custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_BUSY_TIMEOUT_MS);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_SOFT_RESET, NULL, 0);
	if (err < 0) {
		return err;
	}

	data[0] = 0x80;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_TEMP_SENSOR_CTRL, data, 1);
	if (err < 0) {
		return err;
	}

	data[0] = 0xAE;
	data[1] = 0xC7;
	data[2] = 0xC3;
	data[3] = 0xC0;
	data[4] = 0x80;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_BOOSTER_SOFT_START, data, 5);
	if (err < 0) {
		return err;
	}

	data[0] = (config->height - 1) & 0xFF;
	data[1] = ((config->height - 1) >> 8) & 0xFF;
	data[2] = 0x02;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DRIVER_OUTPUT_CTRL, data, 3);
	if (err < 0) {
		return err;
	}

	data[0] = 0x01;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_BORDER_WAVEFORM, data, 1);
	if (err < 0) {
		return err;
	}

	data[0] = 0x01;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DATA_ENTRY_MODE, data, 1);
	if (err < 0) {
		return err;
	}

	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = (config->width - 1) & 0xFF;
	data[3] = ((config->width - 1) >> 8) & 0xFF;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_X_WINDOW, data, 4);
	if (err < 0) {
		return err;
	}

	data[0] = (config->height - 1) & 0xFF;
	data[1] = ((config->height - 1) >> 8) & 0xFF;
	data[2] = 0x00;
	data[3] = 0x00;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_Y_WINDOW, data, 4);
	if (err < 0) {
		return err;
	}

	data[0] = 0x00;
	data[1] = 0x00;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_X_COUNTER, data, 2);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_Y_COUNTER, data, 2);
	if (err < 0) {
		return err;
	}

	return custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_BUSY_TIMEOUT_MS);
}

static int custom_ssd16xx_blanking_on(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;

	data->blanking_on = true;
	return 0;
}

/* Flush both planes to panel and trigger a full 4-gray refresh */
static int custom_ssd16xx_blanking_off(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint8_t tmp[2];
	int err;

	data->blanking_on = false;

	/* Reset RAM X pointer to 0 */
	tmp[0] = 0;
	tmp[1] = 0;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_X_COUNTER, tmp, 2);
	if (err < 0) {
		return err;
	}

	/* Reset RAM Y pointer to height-1 (Y decrements from top to bottom) */
	tmp[0] = (CUSTOM_SSD16XX_ROWS - 1) & 0xFF;
	tmp[1] = ((CUSTOM_SSD16XX_ROWS - 1) >> 8) & 0x01;
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_RAM_Y_COUNTER, tmp, 2);
	if (err < 0) {
		return err;
	}

	/* Write MSB plane (bit1 of 2bpp) into BW RAM */
	LOG_DBG("Writing BW plane to EPD");
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_BW_RAM, data->bw_plane, CUSTOM_SSD16XX_BUF_BYTES);
	if (err < 0) {
		return err;
	}

	/* Reset RAM pointer for RED plane write */
	tmp[0] = 0;
	tmp[1] = 0;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_X_COUNTER, tmp, 2);
	if (err < 0) {
		return err;
	}
	tmp[0] = (CUSTOM_SSD16XX_ROWS - 1) & 0xFF;
	tmp[1] = ((CUSTOM_SSD16XX_ROWS - 1) >> 8) & 0x01;
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_RAM_Y_COUNTER, tmp, 2);
	if (err < 0) {
		return err;
	}

	/* Write LSB plane (bit0 of 2bpp) into RED RAM */
	LOG_DBG("Writing RED plane to EPD");
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_RED_RAM, data->red_plane, CUSTOM_SSD16XX_BUF_BYTES);
	if (err < 0) {
		return err;
	}

	/* Load 4-gray waveform LUT into controller registers */
	LOG_DBG("Loading 4-gray LUT");
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_WRITE_LUT, lut_4gray_gc, sizeof(lut_4gray_gc));
	if (err < 0) {
		return err;
	}

	/* Display Update Control 2: 0xCF = clock on, load LUT, 4-gray mode 2 */
	tmp[0] = 0xCF;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DISP_UPDATE_CTRL2, tmp, 1);
	if (err < 0) {
		return err;
	}

	/* Master Activation */
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_MASTER_ACTIVATION, NULL, 0);
	if (err < 0) {
		return err;
	}

	LOG_DBG("Waiting for EPD refresh...");
	err = custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_REFRESH_TIMEOUT_MS);
	if (err < 0) {
		LOG_ERR("EPD refresh timed out");
		return err;
	}
	LOG_DBG("EPD refresh complete");
	return 0;
}

/*
 * Convert an L_8 (8-bit grayscale) region into the 2-plane framebuffer.
 *
 * Quantisation:
 *   L=0..63   (black)      -> BW bit=0, RED bit=0
 *   L=64..127 (dark gray)  -> BW bit=0, RED bit=1
 *   L=128..191 (light gray)-> BW bit=1, RED bit=0
 *   L=192..255 (white)     -> BW bit=1, RED bit=1
 *
 * Bit packing: MSB = leftmost pixel (bit 7 = column 0 within a byte).
 */
static int custom_ssd16xx_write(const struct device *dev, const uint16_t x,
				const uint16_t y,
				const struct display_buffer_descriptor *desc,
				const void *buf)
{
	struct custom_ssd16xx_data *data = dev->data;
	const uint8_t *src = buf;

	for (uint16_t row = 0; row < desc->height; row++) {
		for (uint16_t col = 0; col < desc->width; col++) {
			uint8_t luma = src[(size_t)row * desc->pitch + col];
			uint8_t g2 = luma >> 6; /* 0-3 */
			uint16_t byte_idx =
				(y + row) * CUSTOM_SSD16XX_COLS + (x + col) / 8u;
			uint8_t bit_pos = 7u - ((x + col) % 8u);

			if (g2 & 0x01u) {
				data->bw_plane[byte_idx] |= BIT(bit_pos);
			} else {
				data->bw_plane[byte_idx] &= ~BIT(bit_pos);
			}
			if (g2 & 0x02u) {
				data->red_plane[byte_idx] |= BIT(bit_pos);
			} else {
				data->red_plane[byte_idx] &= ~BIT(bit_pos);
			}
		}
	}
	return 0;
}

static void custom_ssd16xx_get_capabilities(const struct device *dev,
					    struct display_capabilities *caps)
{
	const struct custom_ssd16xx_config *config = dev->config;

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = config->width;
	caps->y_resolution = config->height;
	caps->supported_pixel_formats = PIXEL_FORMAT_L_8;
	caps->current_pixel_format = PIXEL_FORMAT_L_8;
	caps->screen_info = SCREEN_INFO_EPD;
}

static int custom_ssd16xx_set_pixel_format(const struct device *dev,
					   const enum display_pixel_format pf)
{
	ARG_UNUSED(dev);
	return pf == PIXEL_FORMAT_L_8 ? 0 : -ENOTSUP;
}

static int custom_ssd16xx_init(const struct device *dev)
{
	const struct custom_ssd16xx_config *config = dev->config;

	if (!device_is_ready(config->mipi_dev)) {
		LOG_ERR("MIPI DBI transport not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&config->busy_gpio)) {
		LOG_ERR("Busy GPIO not ready");
		return -ENODEV;
	}

	if (gpio_pin_configure_dt(&config->busy_gpio, GPIO_INPUT) < 0) {
		LOG_ERR("Failed to configure busy GPIO");
		return -EIO;
	}

	return custom_ssd16xx_panel_init(dev);
}

static DEVICE_API(display, custom_ssd16xx_api) = {
	.blanking_on = custom_ssd16xx_blanking_on,
	.blanking_off = custom_ssd16xx_blanking_off,
	.write = custom_ssd16xx_write,
	.get_capabilities = custom_ssd16xx_get_capabilities,
	.set_pixel_format = custom_ssd16xx_set_pixel_format,
};

#define CUSTOM_SSD16XX_DEFINE(inst) \
	/* 48 KB each — placed in SPIRAM to keep DRAM free */ \
	static uint8_t custom_ssd16xx_bw_##inst[CUSTOM_SSD16XX_BUF_BYTES] \
		__attribute__((section(".ext_ram.bss"))); \
	static uint8_t custom_ssd16xx_red_##inst[CUSTOM_SSD16XX_BUF_BYTES] \
		__attribute__((section(".ext_ram.bss"))); \
	static const struct custom_ssd16xx_config custom_ssd16xx_cfg_##inst = { \
		.mipi_dev = DEVICE_DT_GET(DT_PARENT(DT_DRV_INST(inst))), \
		.dbi_config = { \
			.mode = MIPI_DBI_MODE_SPI_4WIRE, \
			.config = MIPI_DBI_SPI_CONFIG_DT(DT_DRV_INST(inst), \
				SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_HOLD_ON_CS | SPI_LOCK_ON, 0), \
		}, \
		.busy_gpio = GPIO_DT_SPEC_GET(DT_DRV_INST(inst), busy_gpios), \
		.width = DT_PROP(DT_DRV_INST(inst), width), \
		.height = DT_PROP(DT_DRV_INST(inst), height), \
	}; \
	static struct custom_ssd16xx_data custom_ssd16xx_data_##inst = { \
		.bw_plane  = custom_ssd16xx_bw_##inst, \
		.red_plane = custom_ssd16xx_red_##inst, \
	}; \
	DEVICE_DT_INST_DEFINE(inst, custom_ssd16xx_init, NULL, \
		&custom_ssd16xx_data_##inst, &custom_ssd16xx_cfg_##inst, \
		POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &custom_ssd16xx_api);

DT_INST_FOREACH_STATUS_OKAY(CUSTOM_SSD16XX_DEFINE)