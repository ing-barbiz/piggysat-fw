/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <modem/at_parser.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <modem/nrf_modem_lib.h>
#include <modem/ntn.h>
#include <nrf_modem_at.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modem, CONFIG_APP_LOG_LEVEL);

#define MODEM_FW_VERSION_SIZE 128

/* Location given to the modem for NTN: the GNSS fix of the current cycle. */
static double ntn_latitude;
static double ntn_longitude;
static float ntn_altitude;
static bool ntn_location_valid;

static bool lte_registered;
static bool lte_pdn_active;
static struct k_spinlock lte_lock;

K_SEM_DEFINE(lte_link_sem, 0, 1);

static char *first_line(char *text)
{
	char *end;

	while (*text == '\r' || *text == '\n' || *text == ' ')
	{
		text++;
	}
	end = strpbrk(text, "\r\n");
	if (end != NULL)
	{
		*end = '\0';
	}
	return text;
}

static int verify_modem_firmware(void)
{
	char version[MODEM_FW_VERSION_SIZE];
	char *trimmed;
	int err;

	err = nrf_modem_at_cmd(version, sizeof(version), "AT+CGMR");
	if (err)
	{
		LOG_ERR("AT+CGMR failed: %d", err);
		return err;
	}

	trimmed = first_line(version);
	LOG_INF("Modem firmware: %s", trimmed);
	if (strlen(CONFIG_APP_MODEM_FW_EXPECTED) > 0 &&
		strstr(trimmed, CONFIG_APP_MODEM_FW_EXPECTED) == NULL)
	{
		LOG_ERR("Unsupported modem firmware, expected '%s'", CONFIG_APP_MODEM_FW_EXPECTED);
		return -EINVAL;
	}

	return 0;
}

static int ntn_location_update(void)
{
	int err;

	if (!ntn_location_valid)
	{
		LOG_ERR("No GNSS location for NTN");
		return -ENODATA;
	}

	err = ntn_location_set(ntn_latitude, ntn_longitude, ntn_altitude,
							   CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS);

	if (err)
	{
		LOG_ERR("Failed to set NTN location: %d", err);
	}
	return err;
}

static void ntn_handler(const struct ntn_evt *const evt)
{
	if (evt->type == NTN_EVT_LOCATION_REQUEST && evt->location_request.requested)
	{
		(void)ntn_location_update();
	}
}

static void lte_link_update(void)
{
	bool ready;
	k_spinlock_key_t key = k_spin_lock(&lte_lock);

	ready = lte_registered && lte_pdn_active;
	k_spin_unlock(&lte_lock, key);

	if (ready)
	{
		k_sem_give(&lte_link_sem);
	}
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	k_spinlock_key_t key;

	switch (evt->type)
	{
	case LTE_LC_EVT_NW_REG_STATUS:
		key = k_spin_lock(&lte_lock);
		lte_registered = evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
						 evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING;
		if (!lte_registered)
		{
			lte_pdn_active = false;
		}
		k_spin_unlock(&lte_lock, key);
		LOG_INF("Network registration status: %d", evt->nw_reg_status);
		lte_link_update();
		break;
	case LTE_LC_EVT_PDN:
		key = k_spin_lock(&lte_lock);
		if (evt->pdn.type == LTE_LC_EVT_PDN_ACTIVATED || evt->pdn.type == LTE_LC_EVT_PDN_RESUMED)
		{
			lte_pdn_active = true;
		}
		else if (evt->pdn.type != LTE_LC_EVT_PDN_IPV6_UP &&
				 evt->pdn.type != LTE_LC_EVT_PDN_IPV6_DOWN &&
				 evt->pdn.type != LTE_LC_EVT_PDN_APN_RATE_CONTROL_ON &&
				 evt->pdn.type != LTE_LC_EVT_PDN_APN_RATE_CONTROL_OFF)
		{
			lte_pdn_active = false;
		}
		k_spin_unlock(&lte_lock, key);
		LOG_INF("PDN event %d (cid %u)", evt->pdn.type, evt->pdn.cid);
		lte_link_update();
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		LOG_INF("Cell: id=%d tac=%d", evt->cell.id, evt->cell.tac);
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("RRC %s", evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "connected" : "idle");
		break;
	default:
		break;
	}
}

int modem_init(void)
{
	int err;

	ntn_register_handler(ntn_handler);

	err = nrf_modem_lib_init();
	if (err)
	{
		LOG_ERR("Modem library init failed: %d", err);
		return err;
	}

	return verify_modem_firmware();
}

int modem_offline(void)
{
	int err = nrf_modem_at_printf("AT+CFUN=0");

	if (err)
	{
		LOG_ERR("AT+CFUN=0 failed: %d", err);
	}
	return err;
}

void modem_set_location(double latitude, double longitude, float altitude)
{
	ntn_latitude = latitude;
	ntn_longitude = longitude;
	ntn_altitude = altitude;
	ntn_location_valid = true;
}

int lte_configure(void)
{
	int err;

	/* The antenna only switches with the radio off (CFUN=0). */
	err = modem_offline();
	if (!err)
	{
		err = antenna_select(ANTENNA_PATH_LTE);
	}
	if (!err)
	{
		err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	}
	if (!err)
	{
		err = nrf_modem_at_printf("AT%%XBANDLOCK=2,\"\",\"%s\"", CONFIG_APP_BAND_LOCK_LIST);
	}
	if (!err)
	{
		err = nrf_modem_at_printf("AT+CGDCONT=0,\"IP\",\"%s\"", CONFIG_APP_APN);
	}
	if (!err)
	{
		err = ntn_location_update();
	}

	if (err)
	{
		LOG_ERR("NTN configuration failed: %d", err);
	}
	return err;
}

int lte_connect_start(void)
{
	k_spinlock_key_t key = k_spin_lock(&lte_lock);

	lte_registered = false;
	lte_pdn_active = false;
	k_spin_unlock(&lte_lock, key);
	k_sem_reset(&lte_link_sem);

	LOG_INF("LTE: attaching (NTN)");
	return lte_lc_connect_async(lte_handler);
}

/* Waits in 1 s steps so that a magnet stops the attach promptly. */
int lte_wait_for_link(int timeout_s)
{
	for (int t = 0; t < timeout_s; t++)
	{
		if (k_sem_take(&lte_link_sem, K_SECONDS(1)) == 0)
		{
			return 0;
		}
		if (magnet_present())
		{
			return -ECANCELED;
		}
	}

	return -ETIMEDOUT;
}

/* Network details logged once attached, from AT%XMONITOR and AT+CGPADDR.
 *
 * %XMONITOR: <reg_status>[,<full_name>,<short_name>,<plmn>,<tac>,<AcT>,<band>,<cell_id>,
 *            <phys_cell_id>,<EARFCN>,<rsrp>,<snr>,<NW-provided_eDRX_value>,<Active-Time>,
 *            <Periodic-TAU-ext>,<Periodic-TAU>]
 */
#define XMONITOR_RESPONSE_SIZE 384
#define XMONITOR_RSRP_UNKNOWN 255
#define XMONITOR_SNR_UNKNOWN 127

static const char *reg_status_text(int status)
{
	switch (status)
	{
	case LTE_LC_NW_REG_NOT_REGISTERED:
		return "not registered";
	case LTE_LC_NW_REG_REGISTERED_HOME:
		return "registered, home";
	case LTE_LC_NW_REG_SEARCHING:
		return "searching";
	case LTE_LC_NW_REG_REGISTRATION_DENIED:
		return "registration denied";
	case LTE_LC_NW_REG_UNKNOWN:
		return "unknown";
	case LTE_LC_NW_REG_REGISTERED_ROAMING:
		return "registered, roaming";
	case LTE_LC_NW_REG_UICC_FAIL:
		return "UICC failure";
	default:
		return "?";
	}
}

static const char *act_text(int act)
{
	switch (act)
	{
	case LTE_LC_LTE_MODE_LTEM:
		return "LTE-M";
	case LTE_LC_LTE_MODE_NBIOT:
		return "NB-IoT";
	case LTE_LC_LTE_MODE_NTN_NBIOT:
		return "NB-IoT NTN";
	default:
		return "?";
	}
}

/* Copies string field 'index' into buf, or "-" when missing or empty. */
static void at_string(struct at_parser *parser, size_t index, char *buf, size_t size)
{
	size_t len = size;

	if (at_parser_string_get(parser, index, buf, &len) != 0 || buf[0] == '\0')
	{
		snprintk(buf, size, "-");
	}
}

static void ip_address_log(void)
{
	char response[128];
	char ip[64];
	struct at_parser parser;

	/* +CGPADDR: <cid>,"<IPv4>"[,"<IPv6>"] */
	if (nrf_modem_at_cmd(response, sizeof(response), "AT+CGPADDR=0") != 0 ||
		at_parser_init(&parser, response) != 0)
	{
		LOG_INF("  IP address     -");
		return;
	}
	at_string(&parser, 2, ip, sizeof(ip));
	LOG_INF("  IP address     %s", ip);
}

void lte_info_log(void)
{
	char response[XMONITOR_RESPONSE_SIZE];
	char full_name[32];
	char short_name[32];
	char plmn[8];
	char tac[12];
	char cell_id[12];
	char edrx[12];
	char active_time[12];
	char tau_ext[12];
	char tau[12];
	struct at_parser parser;
	int reg_status = -1;
	int act = -1;
	int band = -1;
	int pci = -1;
	int earfcn = -1;
	int rsrp = XMONITOR_RSRP_UNKNOWN;
	int snr = XMONITOR_SNR_UNKNOWN;
	int err;

	err = nrf_modem_at_cmd(response, sizeof(response), "AT%%XMONITOR");
	if (err)
	{
		LOG_WRN("Network info: AT%%XMONITOR failed: %d", err);
		return;
	}

	err = at_parser_init(&parser, response);
	if (!err)
	{
		err = at_parser_num_get(&parser, 1, &reg_status);
	}
	if (err)
	{
		LOG_WRN("Network info: cannot parse: %s", first_line(response));
		return;
	}

	LOG_INF("Network info:");
	LOG_INF("  Registration   %s (%d)", reg_status_text(reg_status), reg_status);
	if (reg_status != LTE_LC_NW_REG_REGISTERED_HOME &&
		reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)
	{
		return;
	}

	at_string(&parser, 2, full_name, sizeof(full_name));
	at_string(&parser, 3, short_name, sizeof(short_name));
	at_string(&parser, 4, plmn, sizeof(plmn));
	at_string(&parser, 5, tac, sizeof(tac));
	(void)at_parser_num_get(&parser, 6, &act);
	(void)at_parser_num_get(&parser, 7, &band);
	at_string(&parser, 8, cell_id, sizeof(cell_id));
	(void)at_parser_num_get(&parser, 9, &pci);
	(void)at_parser_num_get(&parser, 10, &earfcn);
	(void)at_parser_num_get(&parser, 11, &rsrp);
	(void)at_parser_num_get(&parser, 12, &snr);
	at_string(&parser, 13, edrx, sizeof(edrx));
	at_string(&parser, 14, active_time, sizeof(active_time));
	at_string(&parser, 15, tau_ext, sizeof(tau_ext));
	at_string(&parser, 16, tau, sizeof(tau));

	LOG_INF("  Operator       %s / %s", full_name, short_name);
	/* PLMN = MCC (3 digits) + MNC (2 or 3 digits) */
	if (strlen(plmn) >= 5)
	{
		LOG_INF("  MCC / MNC      %.3s / %s (PLMN %s)", plmn, plmn + 3, plmn);
	}
	else
	{
		LOG_INF("  MCC / MNC      - (PLMN %s)", plmn);
	}
	LOG_INF("  Technology     %s (AcT %d)", act_text(act), act);
	LOG_INF("  Band           %d", band);
	LOG_INF("  EARFCN         %d", earfcn);
	LOG_INF("  Cell ID        %s (%lu)", cell_id, strtoul(cell_id, NULL, 16));
	LOG_INF("  Tracking area  %s (%lu)", tac, strtoul(tac, NULL, 16));
	LOG_INF("  PCI            %d", pci);
	if (rsrp == XMONITOR_RSRP_UNKNOWN)
	{
		LOG_INF("  RSRP           -");
	}
	else
	{
		LOG_INF("  RSRP           %d dBm", RSRP_IDX_TO_DBM(rsrp));
	}
	if (snr == XMONITOR_SNR_UNKNOWN)
	{
		LOG_INF("  SNR            -");
	}
	else
	{
		LOG_INF("  SNR            %d dB", SNR_IDX_TO_DB(snr));
	}
	LOG_INF("  eDRX           %s", edrx);
	LOG_INF("  Active time    %s", active_time);
	LOG_INF("  Periodic TAU   %s (ext %s)", tau, tau_ext);
	ip_address_log();
}
