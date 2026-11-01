/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app.h"

#include <errno.h>

#include <modem/lte_lc.h>
#include <nrf_modem_gnss.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(gnss, CONFIG_APP_LOG_LEVEL);

/* Filled in the GNSS event handler (interrupt context) when a valid fix arrives. */
static struct nrf_modem_gnss_pvt_data_frame fix_pvt;
static uint8_t searching_sats;

K_SEM_DEFINE(fix_sem, 0, 1);

static uint8_t sats_count(const struct nrf_modem_gnss_pvt_data_frame *pvt, bool used_only)
{
	uint8_t count = 0;

	for (size_t i = 0; i < ARRAY_SIZE(pvt->sv); i++)
	{
		if (pvt->sv[i].sv == 0)
		{
			continue;
		}
		if (!used_only || (pvt->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX))
		{
			count++;
		}
	}

	return count;
}

static void gnss_event_handler(int event)
{
	struct nrf_modem_gnss_pvt_data_frame pvt;

	if (event != NRF_MODEM_GNSS_EVT_PVT)
	{
		return;
	}

	if (nrf_modem_gnss_read(&pvt, sizeof(pvt), NRF_MODEM_GNSS_DATA_PVT) != 0)
	{
		return;
	}

	if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID)
	{
		fix_pvt = pvt;
		k_sem_give(&fix_sem);
	}
	else
	{
		searching_sats = sats_count(&pvt, false);
	}
}

static int gnss_start(int timeout_s)
{
	int err;

	/* The antenna only switches with the radio off (CFUN=0). */
	err = modem_offline();
	if (!err)
	{
		err = antenna_select(ANTENNA_PATH_GNSS);
	}
	if (!err)
	{
		err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	}
	if (!err)
	{
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	}
	if (!err)
	{
		err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
	}
	if (!err)
	{
		/* Single fix: the receiver stops by itself after the fix or the timeout. */
		err = nrf_modem_gnss_fix_interval_set(0);
	}
	if (!err)
	{
		err = nrf_modem_gnss_fix_retry_set(timeout_s);
	}
	if (!err)
	{
		k_sem_reset(&fix_sem);
		searching_sats = 0;
		err = nrf_modem_gnss_start();
	}

	if (err)
	{
		LOG_ERR("GNSS start failed: %d", err);
	}
	return err;
}

static void gnss_stop(void)
{
	(void)nrf_modem_gnss_stop();
	(void)lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_GNSS);
	(void)modem_offline();
}

int gnss_acquire(struct gnss_fix *fix, int timeout_s)
{
	int64_t start = k_uptime_get();
	const struct nrf_modem_gnss_datetime *dt = &fix_pvt.datetime;
	int err;

	LOG_INF("GNSS: searching (timeout %d s)", timeout_s);
	err = gnss_start(timeout_s);
	if (err)
	{
		gnss_stop();
		return err;
	}

	err = k_sem_take(&fix_sem, K_SECONDS(timeout_s + 5));
	gnss_stop();
	if (err)
	{
		LOG_WRN("GNSS: no fix within %d s (%u satellites tracked)", timeout_s, searching_sats);
		return -ETIMEDOUT;
	}

	fix->latitude = fix_pvt.latitude;
	fix->longitude = fix_pvt.longitude;
	fix->altitude = fix_pvt.altitude;
	fix->accuracy = fix_pvt.accuracy;
	fix->satellites = sats_count(&fix_pvt, true);
	fix->ttff_s = (uint32_t)((k_uptime_get() - start) / 1000);
	snprintk(fix->utc, sizeof(fix->utc), "%04u-%02u-%02uT%02u:%02u:%02uZ", dt->year, dt->month,
			 dt->day, dt->hour, dt->minute, dt->seconds);

	return 0;
}
