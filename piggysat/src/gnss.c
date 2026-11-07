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

/* Satellite info is logged every SATS_LOG_PERIOD_S seconds while searching. */
#define SATS_LOG_PERIOD_S 3

/* Filled in the GNSS event handler (interrupt context) when a valid fix arrives. */
static struct nrf_modem_gnss_pvt_data_frame fix_pvt;

/* Latest PVT frame, with or without a fix, for the satellite log. */
static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static bool last_pvt_valid;
static struct k_spinlock pvt_lock;

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

static const char *sv_system(uint8_t signal)
{
	switch (signal)
	{
	case NRF_MODEM_GNSS_SIGNAL_GPS_L1_CA:
		return "GPS";
	case NRF_MODEM_GNSS_SIGNAL_QZSS_L1_CA:
		return "QZSS";
	case NRF_MODEM_GNSS_SIGNAL_GAL_E1_OS:
		return "GAL";
	default:
		return "?";
	}
}

/* Logs the satellites of the latest PVT frame (called from the waiting thread). */
static void sats_log(int64_t start)
{
	struct nrf_modem_gnss_pvt_data_frame pvt;
	k_spinlock_key_t key = k_spin_lock(&pvt_lock);
	bool valid = last_pvt_valid;

	if (valid)
	{
		pvt = last_pvt;
	}
	k_spin_unlock(&pvt_lock, key);

	if (!valid)
	{
		LOG_INF("GNSS: %d s, no satellite data yet", (int)((k_uptime_get() - start) / 1000));
		return;
	}

	LOG_INF("GNSS: %d s, %u satellites tracked, %u used in fix%s%s",
			(int)((k_uptime_get() - start) / 1000), sats_count(&pvt, false), sats_count(&pvt, true),
			(pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED) ? ", deadline missed" : "",
			(pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME)
				? ", not enough window time"
				: "");

	for (size_t i = 0; i < ARRAY_SIZE(pvt.sv); i++)
	{
		const struct nrf_modem_gnss_sv *sv = &pvt.sv[i];

		if (sv->sv == 0)
		{
			continue;
		}
		LOG_INF("  %-4s %3u  C/N0 %2u.%u dB-Hz  elev %3d deg  azim %3d deg%s%s",
				sv_system(sv->signal), sv->sv, sv->cn0 / 10, sv->cn0 % 10, sv->elevation,
				sv->azimuth, (sv->flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) ? "  used" : "",
				(sv->flags & NRF_MODEM_GNSS_SV_FLAG_UNHEALTHY) ? "  unhealthy" : "");
	}
}

static void gnss_event_handler(int event)
{
	struct nrf_modem_gnss_pvt_data_frame pvt;
	k_spinlock_key_t key;

	if (event != NRF_MODEM_GNSS_EVT_PVT)
	{
		return;
	}

	if (nrf_modem_gnss_read(&pvt, sizeof(pvt), NRF_MODEM_GNSS_DATA_PVT) != 0)
	{
		return;
	}

	key = k_spin_lock(&pvt_lock);
	last_pvt = pvt;
	last_pvt_valid = true;
	k_spin_unlock(&pvt_lock, key);

	if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID)
	{
		fix_pvt = pvt;
		k_sem_give(&fix_sem);
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
		k_spinlock_key_t key = k_spin_lock(&pvt_lock);

		last_pvt_valid = false;
		k_spin_unlock(&pvt_lock, key);
		k_sem_reset(&fix_sem);
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

	/* Wait in 1 s steps so that a magnet stops the search promptly. */
	err = -ETIMEDOUT;
	for (int t = 0; t < timeout_s + 5; t++)
	{
		if (k_sem_take(&fix_sem, K_SECONDS(1)) == 0)
		{
			err = 0;
			break;
		}
		if (magnet_present())
		{
			err = -ECANCELED;
			break;
		}
		if ((t + 1) % SATS_LOG_PERIOD_S == 0)
		{
			sats_log(start);
		}
	}
	gnss_stop();
	if (err == -ECANCELED)
	{
		LOG_INF("GNSS: search stopped (magnet)");
		return err;
	}
	if (err)
	{
		LOG_WRN("GNSS: no fix within %d s", timeout_s);
		sats_log(start);
		return err;
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
