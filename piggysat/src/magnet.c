/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app.h"

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(magnet, CONFIG_APP_LOG_LEVEL);

#define MAGNET_EVT_CHANGED BIT(0)

/* Magnet sensor: active (low) = magnet present. */
static const struct gpio_dt_spec magnet = GPIO_DT_SPEC_GET(DT_NODELABEL(magnet_sensor), gpios);
static struct gpio_callback magnet_cb;

K_EVENT_DEFINE(magnet_events);

static void magnet_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_event_post(&magnet_events, MAGNET_EVT_CHANGED);
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
	if (!err)
	{
		gpio_init_callback(&magnet_cb, magnet_isr, BIT(magnet.pin));
		err = gpio_add_callback_dt(&magnet, &magnet_cb);
	}
	if (!err)
	{
		err = gpio_pin_interrupt_configure_dt(&magnet, GPIO_INT_EDGE_BOTH);
	}
	if (err)
	{
		LOG_ERR("Magnet sensor setup failed: %d", err);
	}

	return err;
}

bool magnet_present(void)
{
	return gpio_pin_get_dt(&magnet) > 0;
}

/* Waits up to the timeout, waking early when the sensor changes. Callers read
 * magnet_present() afterwards, so a change that slips between two waits is
 * still seen at the next check.
 */
void magnet_wait(k_timeout_t timeout)
{
	(void)k_event_wait(&magnet_events, MAGNET_EVT_CHANGED, true, timeout);
}
