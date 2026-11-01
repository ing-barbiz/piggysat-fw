/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app.h"

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(leds, CONFIG_APP_LOG_LEVEL);

enum led_mode
{
	LED_MODE_OFF,
	LED_MODE_BLINK,
	LED_MODE_ON_TIMED,
};

struct led
{
	struct gpio_dt_spec gpio;
	struct k_work_delayable work;
	enum led_mode mode;
	bool lit;
};

static struct led leds[] = {
	[LED_GNSS] = {.gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(gnss_led), gpios)},
	[LED_LTE] = {.gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(lte_led), gpios)},
	[LED_SENT] = {.gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(sent_led), gpios)},
};

static void led_set(struct led *led, bool on)
{
	led->lit = on;
	(void)gpio_pin_set_dt(&led->gpio, on ? 1 : 0);
}

/* Blink: toggle and reschedule. Timed on: the work item turns the LED off. */
static void led_work_fn(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct led *led = CONTAINER_OF(dwork, struct led, work);

	if (led->mode == LED_MODE_BLINK)
	{
		led_set(led, !led->lit);
		k_work_schedule(&led->work, K_MSEC(CONFIG_APP_LED_BLINK_MS));
	}
	else
	{
		led->mode = LED_MODE_OFF;
		led_set(led, false);
	}
}

int leds_init(void)
{
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++)
	{
		if (!gpio_is_ready_dt(&leds[i].gpio))
		{
			LOG_ERR("LED %u GPIO not ready", (unsigned int)i);
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(&leds[i].gpio, GPIO_OUTPUT_INACTIVE);
		if (err)
		{
			LOG_ERR("Failed to configure LED %u: %d", (unsigned int)i, err);
			return err;
		}

		k_work_init_delayable(&leds[i].work, led_work_fn);
	}

	return 0;
}

static void led_stop(struct led *led)
{
	struct k_work_sync sync;

	led->mode = LED_MODE_OFF;
	(void)k_work_cancel_delayable_sync(&led->work, &sync);
}

void led_blink(enum led_id id)
{
	struct led *led = &leds[id];

	led_stop(led);
	led->mode = LED_MODE_BLINK;
	led_set(led, true);
	k_work_schedule(&led->work, K_MSEC(CONFIG_APP_LED_BLINK_MS));
}

void led_on_for(enum led_id id, k_timeout_t duration)
{
	struct led *led = &leds[id];

	led_stop(led);
	led->mode = LED_MODE_ON_TIMED;
	led_set(led, true);
	k_work_schedule(&led->work, duration);
}

void led_off(enum led_id id)
{
	struct led *led = &leds[id];

	led_stop(led);
	led_set(led, false);
}
