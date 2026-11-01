/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app.h"

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(button, CONFIG_APP_LOG_LEVEL);

/* Touch button: TTP223 push-pull output, active high (sw0 in the board devicetree). */
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static struct gpio_callback button_cb;
static const struct shell *button_sh;
static bool button_test_active;
static int button_last_state;
static unsigned int button_presses;

/* The GPIO callback runs in interrupt context; print from a work item. */
static void button_work_fn(struct k_work *work)
{
	int state;

	ARG_UNUSED(work);

	if (!button_test_active)
	{
		return;
	}

	state = gpio_pin_get_dt(&button);
	if (state < 0 || state == button_last_state)
	{
		return;
	}

	button_last_state = state;
	if (state)
	{
		button_presses++;
		shell_print(button_sh, "button: pressed (#%u)", button_presses);
	}
	else
	{
		shell_print(button_sh, "button: released");
	}
}

K_WORK_DEFINE(button_work, button_work_fn);

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_submit(&button_work);
}

int button_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&button))
	{
		LOG_ERR("Button GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (err)
	{
		LOG_ERR("Failed to configure button GPIO: %d", err);
		return err;
	}

	gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
	err = gpio_add_callback_dt(&button, &button_cb);
	if (err)
	{
		LOG_ERR("Failed to add button callback: %d", err);
	}

	return err;
}

int button_test_start(const struct shell *sh)
{
	int err;

	if (button_test_active)
	{
		return -EALREADY;
	}

	button_sh = sh;
	button_presses = 0;
	button_last_state = gpio_pin_get_dt(&button);
	button_test_active = true;

	err = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	if (err)
	{
		button_test_active = false;
		shell_error(sh, "button: failed to enable the interrupt: %d", err);
		return err;
	}

	shell_print(sh, "button: currently %s. Touch the button; 'button stop' ends the test.",
				button_last_state > 0 ? "pressed" : "released");
	return 0;
}

int button_test_stop(const struct shell *sh)
{
	if (!button_test_active)
	{
		return -EALREADY;
	}

	(void)gpio_pin_interrupt_configure_dt(&button, GPIO_INT_DISABLE);
	button_test_active = false;
	shell_print(sh, "button: stopped (%u presses)", button_presses);
	return 0;
}
