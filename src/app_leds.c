/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_internal.h"

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app_leds, CONFIG_APP_LOG_LEVEL);

#define LED_STEP_MS 500

static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
};

static const char *const led_names[] = {"green", "orange", "red"};

static struct k_work_delayable led_test_work;
static size_t led_test_step;
static bool led_test_active;

static void leds_set_all(int value)
{
	for (size_t i = 0; i < ARRAY_SIZE(leds); i++)
	{
		(void)gpio_pin_set_dt(&leds[i], value);
	}
}

/* Steps through green, orange, red one at a time, then all on. */
static void led_test_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (led_test_step < ARRAY_SIZE(leds))
	{
		leds_set_all(0);
		(void)gpio_pin_set_dt(&leds[led_test_step], 1);
	}
	else
	{
		leds_set_all(1);
	}

	led_test_step = (led_test_step + 1) % (ARRAY_SIZE(leds) + 1);
	k_work_schedule(&led_test_work, K_MSEC(LED_STEP_MS));
}

int leds_init(void)
{
	int err;

	k_work_init_delayable(&led_test_work, led_test_work_fn);

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++)
	{
		if (!gpio_is_ready_dt(&leds[i]))
		{
			LOG_ERR("LED %s GPIO not ready", led_names[i]);
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
		if (err)
		{
			LOG_ERR("Failed to configure LED %s: %d", led_names[i], err);
			return err;
		}
	}

	return 0;
}

int leds_test_start(void)
{
	if (led_test_active)
	{
		return -EALREADY;
	}

	led_test_active = true;
	led_test_step = 0;
	k_work_schedule(&led_test_work, K_NO_WAIT);
	return 0;
}

int leds_test_stop(void)
{
	struct k_work_sync sync;

	if (!led_test_active)
	{
		return -EALREADY;
	}

	led_test_active = false;
	(void)k_work_cancel_delayable_sync(&led_test_work, &sync);
	leds_set_all(0);
	return 0;
}
