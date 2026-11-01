/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app.h"

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(antenna, CONFIG_APP_LOG_LEVEL);

/* J3 antenna selection switch: low = LTE, high = GNSS. */
static const struct gpio_dt_spec antenna_switch =
	GPIO_DT_SPEC_GET(DT_NODELABEL(antenna_switch), gpios);

int antenna_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&antenna_switch))
	{
		LOG_ERR("Antenna switch GPIO not ready");
		return -ENODEV;
	}

	/* Start on LTE so the switch control line never floats. */
	err = gpio_pin_configure_dt(&antenna_switch, GPIO_OUTPUT_INACTIVE);
	if (err)
	{
		LOG_ERR("Failed to configure antenna switch GPIO: %d", err);
		return err;
	}

	LOG_INF("Antenna switch initialized: LTE");
	return 0;
}

/* Callers switch only while the modem radio is off (CFUN=0). */
int antenna_select(enum antenna_path path)
{
	int err = gpio_pin_set_dt(&antenna_switch, path == ANTENNA_PATH_GNSS ? 1 : 0);

	if (err)
	{
		LOG_ERR("Failed to set antenna switch: %d", err);
		return err;
	}

	LOG_INF("Antenna switched to %s", path == ANTENNA_PATH_GNSS ? "GNSS" : "LTE");
	return 0;
}
