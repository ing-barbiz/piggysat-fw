/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app.h"

#include <zephyr/drivers/hwinfo.h>
#include <app_version.h>
#include <zephyr/logging/log.h>

#include <modem/modem_info.h>
#include <modem/nrf_modem_lib.h>
#include <modem/ntn.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

static void log_reset_cause_flags(const char *label, uint32_t flags)
{
	if (flags & RESET_PIN)
	{
		LOG_INF("%s: pin", label);
	}
	if (flags & RESET_SOFTWARE)
	{
		LOG_INF("%s: software", label);
	}
	if (flags & RESET_BROWNOUT)
	{
		LOG_INF("%s: brownout", label);
	}
	if (flags & RESET_POR)
	{
		LOG_INF("%s: power-on", label);
	}
	if (flags & RESET_WATCHDOG)
	{
		LOG_INF("%s: watchdog", label);
	}
	if (flags & RESET_DEBUG)
	{
		LOG_INF("%s: debug", label);
	}
	if (flags & RESET_SECURITY)
	{
		LOG_INF("%s: security", label);
	}
	if (flags & RESET_LOW_POWER_WAKE)
	{
		LOG_INF("%s: low-power-wake", label);
	}
	if (flags & RESET_CPU_LOCKUP)
	{
		LOG_INF("%s: cpu-lockup", label);
	}
	if (flags & RESET_PARITY)
	{
		LOG_INF("%s: parity", label);
	}
	if (flags & RESET_PLL)
	{
		LOG_INF("%s: pll", label);
	}
	if (flags & RESET_CLOCK)
	{
		LOG_INF("%s: clock", label);
	}
	if (flags & RESET_HARDWARE)
	{
		LOG_INF("%s: hardware", label);
	}
	if (flags & RESET_USER)
	{
		LOG_INF("%s: user", label);
	}
	if (flags & RESET_TEMPERATURE)
	{
		LOG_INF("%s: temperature", label);
	}
	if (flags & RESET_BOOTLOADER)
	{
		LOG_INF("%s: bootloader", label);
	}
	if (flags & RESET_FLASH)
	{
		LOG_INF("%s: flash", label);
	}
}

static void log_reset_cause(void)
{
	uint32_t cause = 0U;
	uint32_t supported = 0U;
	int err;

	err = hwinfo_get_supported_reset_cause(&supported);
	if (err)
	{
		LOG_WRN("Reset cause support query failed: %d", err);
		return;
	}

	err = hwinfo_get_reset_cause(&cause);
	if (err)
	{
		LOG_WRN("Reset cause query failed: %d", err);
		return;
	}

	LOG_INF("Reset cause flags: 0x%08x (supported=0x%08x)",
			(unsigned int)cause, (unsigned int)supported);
	log_reset_cause_flags("Reset cause", cause & supported);

	err = hwinfo_clear_reset_cause();
	if (err)
	{
		LOG_WRN("Reset cause clear failed: %d", err);
	}
}

static bool modem_init(void)
{
	int err;

	ntn_register_handler(ntn_handler);

	err = nrf_modem_lib_init();
	if (err)
	{
		LOG_ERR("Failed to initialize modem library: %d", err);
		return false;
	}

	err = verify_modem_firmware();
	if (err)
	{
		return false;
	}

	err = modem_info_init();
	if (err)
	{
		LOG_ERR("Failed to initialize modem info: %d", err);
	}

	log_modem_identity();

	err = coap_client_start();
	if (err)
	{
		return false;
	}

	return true;
}

/* Board and modem init only; the tests are shell commands (see shell.c). */
int main(void)
{
	LOG_INF("PiggySat test tool v%s started", APP_VERSION_STRING);
	log_reset_cause();

	(void)antenna_init();
	(void)leds_init();
	(void)button_init();
	lte_message_init();

	if (modem_init())
	{
		modem_tests_enable();
		LOG_INF("Ready. Commands: led, button, cw, gnss, lte, sim (type 'help')");
	}
	else
	{
		LOG_ERR("Modem not available: only 'led' and 'button' can run");
	}

	return 0;
}
