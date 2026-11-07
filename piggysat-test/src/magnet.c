/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app.h"

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(magnet, CONFIG_APP_LOG_LEVEL);

/* Magnet sensor: active (low) = magnet present (magnet_sensor / sw0 in the devicetree). */
static const struct gpio_dt_spec magnet = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static struct gpio_callback magnet_cb;
static const struct shell *magnet_sh;
static bool magnet_test_active;
static int magnet_last_state;
static unsigned int magnet_detections;

/* The GPIO callback runs in interrupt context; print from a work item. */
static void magnet_work_fn(struct k_work *work)
{
	int state;

	ARG_UNUSED(work);

	if (!magnet_test_active)
	{
		return;
	}

	state = gpio_pin_get_dt(&magnet);
	if (state < 0 || state == magnet_last_state)
	{
		return;
	}

	magnet_last_state = state;
	if (state)
	{
		magnet_detections++;
		shell_print(magnet_sh, "magnet: detected (#%u)", magnet_detections);
	}
	else
	{
		shell_print(magnet_sh, "magnet: removed");
	}
}

K_WORK_DEFINE(magnet_work, magnet_work_fn);

static void magnet_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_submit(&magnet_work);
}

int magnet_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&magnet))
	{
		LOG_ERR("Magnet sensor GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&magnet, GPIO_INPUT);
	if (err)
	{
		LOG_ERR("Failed to configure magnet sensor GPIO: %d", err);
		return err;
	}

	gpio_init_callback(&magnet_cb, magnet_isr, BIT(magnet.pin));
	err = gpio_add_callback_dt(&magnet, &magnet_cb);
	if (err)
	{
		LOG_ERR("Failed to add magnet sensor callback: %d", err);
	}

	return err;
}

int magnet_test_start(const struct shell *sh)
{
	int err;

	if (magnet_test_active)
	{
		return -EALREADY;
	}

	magnet_sh = sh;
	magnet_detections = 0;
	magnet_last_state = gpio_pin_get_dt(&magnet);
	magnet_test_active = true;

	err = gpio_pin_interrupt_configure_dt(&magnet, GPIO_INT_EDGE_BOTH);
	if (err)
	{
		magnet_test_active = false;
		shell_error(sh, "magnet: failed to enable the interrupt: %d", err);
		return err;
	}

	shell_print(sh, "magnet: currently %s. Bring a magnet close; 'magnet stop' ends the test.",
				magnet_last_state > 0 ? "detected" : "not detected");
	return 0;
}

int magnet_test_stop(const struct shell *sh)
{
	if (!magnet_test_active)
	{
		return -EALREADY;
	}

	(void)gpio_pin_interrupt_configure_dt(&magnet, GPIO_INT_DISABLE);
	magnet_test_active = false;
	shell_print(sh, "magnet: stopped (%u detections)", magnet_detections);
	return 0;
}
