/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_internal.h"

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include <SEGGER_RTT.h>

#include <modem/modem_info.h>
#include <modem/nrf_modem_lib.h>
#include <modem/ntn.h>

LOG_MODULE_REGISTER(app_main, CONFIG_APP_LOG_LEVEL);

/* RTT down-buffer used for menu input; the same channel carries the console. */
#define CONSOLE_RTT_CHANNEL 0

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

static bool console_is_blank(char c)
{
	return c == '\r' || c == '\n' || c == ' ';
}

bool console_key_pressed(void)
{
	char c;

	while (SEGGER_RTT_Read(CONSOLE_RTT_CHANNEL, &c, 1) == 1)
	{
		if (!console_is_blank(c))
		{
			return true;
		}
	}

	return false;
}

void console_flush_input(void)
{
	char c;

	/* Let the rest of the line (e.g. the Enter after the choice) arrive. */
	k_msleep(100);
	while (SEGGER_RTT_Read(CONSOLE_RTT_CHANNEL, &c, 1) == 1)
	{
	}
}

static char console_wait_choice(void)
{
	char c;

	for (;;)
	{
		if (SEGGER_RTT_Read(CONSOLE_RTT_CHANNEL, &c, 1) == 1 && !console_is_blank(c))
		{
			console_flush_input();
			return c;
		}
		k_msleep(50);
	}
}

static void print_menu(bool modem_ready)
{
	printk("\n"
		   "==================== PiggySat test tool ====================\n"
		   " 1 - LED test\n"
		   " 2 - LTE CW TX test (band %d, %d.%d MHz, %d dBm)\n"
		   " 3 - GNSS mode (continuous reception, NMEA output)\n"
		   " 4 - LTE message send (position 0,0)\n"
		   "%s"
		   " Type a number and press Enter. During a test, type q + Enter to stop.\n"
		   "============================================================\n"
		   "> ",
		   CONFIG_APP_CW_BAND, CONFIG_APP_CW_FREQUENCY_100KHZ / 10,
		   CONFIG_APP_CW_FREQUENCY_100KHZ % 10, CONFIG_APP_CW_POWER_DBM,
		   modem_ready ? "" : " (modem not available: only test 1 can run)\n");
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

int main(void)
{
	bool modem_ready;
	char choice;

	LOG_INF("PiggySat test tool started");
	log_reset_cause();

	(void)antenna_init();
	(void)leds_init();
	modem_ready = modem_init();

	for (;;)
	{
		/* Give deferred logs a moment so they don't land inside the menu. */
		k_msleep(200);
		print_menu(modem_ready);
		choice = console_wait_choice();
		printk("%c\n", choice);

		if (!modem_ready && choice >= '2' && choice <= '4')
		{
			printk("Modem not available, see the boot log\n");
			continue;
		}

		switch (choice)
		{
		case '1':
			test_leds();
			break;
		case '2':
			test_lte_cw();
			break;
		case '3':
			test_gnss_nmea();
			break;
		case '4':
			test_lte_message();
			break;
		default:
			printk("Unknown choice '%c'\n", choice);
			break;
		}
	}

	return 0;
}
