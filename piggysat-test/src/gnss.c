/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include <nrf_modem_gnss.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(gnss, CONFIG_APP_LOG_LEVEL);

#define GNSS_NMEA_QUEUE_LEN 16

/* The GNSS event handler runs in interrupt context, so NMEA sentences are
 * queued there and printed to the shell from a work item.
 */
K_MSGQ_DEFINE(gnss_nmea_q, sizeof(struct nrf_modem_gnss_nmea_data_frame),
			  GNSS_NMEA_QUEUE_LEN, 4);

static atomic_t gnss_nmea_dropped;
static const struct shell *gnss_sh;
static bool gnss_active;

void format_timestamp(char *buffer, size_t buffer_len)
{
	time_t now = time(NULL);
	struct tm tm_utc;

	if ((now == (time_t)-1) || (gmtime_r(&now, &tm_utc) == NULL))
	{
		snprintk(buffer, buffer_len, "unavailable");
		return;
	}

	strftime(buffer, buffer_len, "%Y-%m-%dT%H:%M:%S", &tm_utc);
}

static void gnss_nmea_work_fn(struct k_work *work)
{
	struct nrf_modem_gnss_nmea_data_frame nmea;
	char *end;

	ARG_UNUSED(work);

	while (k_msgq_get(&gnss_nmea_q, &nmea, K_NO_WAIT) == 0)
	{
		/* NMEA sentences end with CRLF; the shell adds its own line ending. */
		end = strpbrk(nmea.nmea_str, "\r\n");
		if (end != NULL)
		{
			*end = '\0';
		}

		if (gnss_active && gnss_sh != NULL)
		{
			shell_print(gnss_sh, "%s", nmea.nmea_str);
		}
	}
}

K_WORK_DEFINE(gnss_nmea_work, gnss_nmea_work_fn);

static void gnss_event_handler(int event)
{
	struct nrf_modem_gnss_nmea_data_frame nmea;

	switch (event)
	{
	case NRF_MODEM_GNSS_EVT_NMEA:
		if (nrf_modem_gnss_read(&nmea, sizeof(nmea), NRF_MODEM_GNSS_DATA_NMEA) != 0)
		{
			break;
		}
		if (k_msgq_put(&gnss_nmea_q, &nmea, K_NO_WAIT) != 0)
		{
			atomic_inc(&gnss_nmea_dropped);
		}
		k_work_submit(&gnss_nmea_work);
		break;
	default:
		break;
	}
}

static int gnss_start_continuous(void)
{
	int err;

	err = modem_enter_min_func();
	if (err)
	{
		return err;
	}

	err = antenna_select(ANTENNA_PATH_GNSS);
	if (err)
	{
		return err;
	}

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err)
	{
		LOG_ERR("Failed to set modem to GNSS-only mode: %d", err);
		return err;
	}

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err)
	{
		LOG_ERR("Failed to activate GNSS functional mode: %d", err);
		return err;
	}

	err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
	if (err)
	{
		LOG_ERR("Failed to set GNSS event handler: %d", err);
		return err;
	}

	err = nrf_modem_gnss_nmea_mask_set(NRF_MODEM_GNSS_NMEA_GGA_MASK |
									   NRF_MODEM_GNSS_NMEA_GLL_MASK |
									   NRF_MODEM_GNSS_NMEA_GSA_MASK |
									   NRF_MODEM_GNSS_NMEA_GSV_MASK |
									   NRF_MODEM_GNSS_NMEA_RMC_MASK);
	if (err)
	{
		LOG_ERR("Failed to set NMEA mask: %d", err);
		return err;
	}

	/* Continuous tracking (1 s fix interval), no fix retry timeout. */
	err = nrf_modem_gnss_fix_retry_set(0);
	if (err)
	{
		LOG_ERR("Failed to set GNSS fix retry: %d", err);
		return err;
	}

	err = nrf_modem_gnss_fix_interval_set(1);
	if (err)
	{
		LOG_ERR("Failed to set GNSS fix interval: %d", err);
		return err;
	}

	k_msgq_purge(&gnss_nmea_q);
	atomic_set(&gnss_nmea_dropped, 0);

	err = nrf_modem_gnss_start();
	if (err)
	{
		LOG_ERR("Failed to start GNSS: %d", err);
		return err;
	}

	return 0;
}

static void gnss_shutdown(void)
{
	int err = nrf_modem_gnss_stop();

	if (err)
	{
		LOG_WRN("Failed to stop GNSS: %d", err);
	}

	(void)lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_GNSS);
	(void)modem_enter_min_func();
}

int gnss_test_start(const struct shell *sh)
{
	int err;

	gnss_sh = sh;
	gnss_active = true;
	shell_print(sh, "gnss: continuous reception, NMEA output. Positions are not stored. "
				"'gnss stop' ends the test.");

	err = gnss_start_continuous();
	if (err)
	{
		gnss_active = false;
		shell_error(sh, "gnss: start failed: %d", err);
		gnss_shutdown();
		return err;
	}

	return 0;
}

int gnss_test_stop(const struct shell *sh)
{
	if (!gnss_active)
	{
		return -EALREADY;
	}

	gnss_active = false;
	gnss_shutdown();
	shell_print(sh, "gnss: stopped (%d NMEA sentences dropped), modem offline",
				(int)atomic_get(&gnss_nmea_dropped));
	return 0;
}
