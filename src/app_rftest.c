#include "app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nrf_modem_at.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(app_rftest, CONFIG_APP_LOG_LEVEL);

/* AT%XRFTEST TX parameters (nRF91x1 production test AT commands, TX test):
 * <test>=1 (TX), <operation>=1 (ON), band, frequency (100 kHz), power (dBm),
 * system mode 0 (NB-IoT), modulation 4 (CW). For CW, RB/tone count, start and
 * subcarrier spacing are don't-care (0); bandwidth NB-IoT (0), NB index 0,
 * burst mode off (0).
 */
#define XRFTEST_SYSTEM_MODE_NBIOT 0
#define XRFTEST_MODULATION_CW 4

static void print_at_error(const char *what, int err)
{
	if (err > 0)
	{
		printk("%s failed: modem error type %d, code %d\n", what,
			   nrf_modem_at_err_type(err), nrf_modem_at_err(err));
	}
	else
	{
		printk("%s failed: %d\n", what, err);
	}
}

void test_lte_cw(void)
{
	char response[64];
	char measured[16];
	const char *value;
	int q4_dbm;
	int err;

	printk("LTE CW TX test: band %d, %d.%d MHz, %d dBm requested.\n"
		   "WARNING: transmits RF. Use a cabled setup or a shielded room.\n",
		   CONFIG_APP_CW_BAND, CONFIG_APP_CW_FREQUENCY_100KHZ / 10,
		   CONFIG_APP_CW_FREQUENCY_100KHZ % 10, CONFIG_APP_CW_POWER_DBM);

	/* %XRFTEST needs the radio off, and the NTN system mode for bands 255/256. */
	err = modem_enter_min_func();
	if (err)
	{
		return;
	}

	err = antenna_select(ANTENNA_PATH_LTE);
	if (err)
	{
		return;
	}

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err)
	{
		printk("Failed to set NTN system mode: %d\n", err);
		return;
	}

	err = nrf_modem_at_cmd(response, sizeof(response),
						   "AT%%XRFTEST=1,1,%d,%d,%d,%d,%d,0,0,0,0,0,0",
						   CONFIG_APP_CW_BAND, CONFIG_APP_CW_FREQUENCY_100KHZ,
						   CONFIG_APP_CW_POWER_DBM, XRFTEST_SYSTEM_MODE_NBIOT,
						   XRFTEST_MODULATION_CW);
	if (err)
	{
		print_at_error("AT%XRFTEST TX on", err);
		(void)nrf_modem_at_printf("AT%%XRFTEST=1,0");
		return;
	}

	/* Response: "%XRFTEST: <antenna_power>", in q4 dBm (divide by 16). */
	value = strstr(response, "%XRFTEST:");
	if (value != NULL)
	{
		q4_dbm = atoi(value + sizeof("%XRFTEST:") - 1);
		snprintf(measured, sizeof(measured), "%.1f", (double)q4_dbm / 16.0);
		printk("Transmitting. Measured power at the antenna port: %s dBm "
			   "(raw %d q4 dBm, requested %d dBm)\n",
			   measured, q4_dbm, CONFIG_APP_CW_POWER_DBM);
	}
	else
	{
		printk("Transmitting. Unexpected response: %s\n", response);
	}

	printk("q + Enter to stop.\n");
	while (!console_key_pressed())
	{
		k_msleep(100);
	}

	err = nrf_modem_at_printf("AT%%XRFTEST=1,0");
	if (err)
	{
		print_at_error("AT%XRFTEST TX off", err);
	}

	(void)modem_enter_min_func();
	printk("LTE CW TX test stopped\n");
}
