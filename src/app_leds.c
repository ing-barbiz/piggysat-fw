/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_internal.h"

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app_leds, CONFIG_APP_LOG_LEVEL);

#define LED_STEP_MS 500

static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
};

static const char *const led_names[] = {"green", "orange", "red"};

int leds_init(void)
{
	int err;

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

static void leds_set_all(int value)
{
	for (size_t i = 0; i < ARRAY_SIZE(leds); i++)
	{
		(void)gpio_pin_set_dt(&leds[i], value);
	}
}

/* Sleeps in small steps so a key press stops the test quickly. */
static bool led_wait_or_stop(void)
{
	for (int t = 0; t < LED_STEP_MS; t += 50)
	{
		if (console_key_pressed())
		{
			return true;
		}
		k_msleep(50);
	}

	return false;
}

void test_leds(void)
{
	printk("LED test: green, orange, red one at a time, then all on. q + Enter to stop.\n");

	for (;;)
	{
		for (size_t i = 0; i < ARRAY_SIZE(leds); i++)
		{
			leds_set_all(0);
			(void)gpio_pin_set_dt(&leds[i], 1);
			printk("  %s\n", led_names[i]);
			if (led_wait_or_stop())
			{
				goto done;
			}
		}

		leds_set_all(1);
		printk("  all\n");
		if (led_wait_or_stop())
		{
			goto done;
		}
	}

done:
	leds_set_all(0);
	printk("LED test stopped\n");
}
