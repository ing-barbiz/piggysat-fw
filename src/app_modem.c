/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <modem/at_parser.h>
#include <modem/modem_info.h>
#include <nrf_modem_at.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(app_modem, CONFIG_APP_LOG_LEVEL);

static bool lte_connected;
static bool lte_pdn_active;
static bool lte_link_up;
static struct k_spinlock lte_link_lock;

static void lte_handler(const struct lte_lc_evt *const evt);

static void lte_registered_set(bool registered)
{
	k_spinlock_key_t key = k_spin_lock(&lte_link_lock);

	lte_connected = registered;
	if (!registered)
	{
		lte_pdn_active = false;
	}
	k_spin_unlock(&lte_link_lock, key);
}

static void lte_pdn_active_set(bool active)
{
	k_spinlock_key_t key = k_spin_lock(&lte_link_lock);

	lte_pdn_active = active;
	k_spin_unlock(&lte_link_lock, key);
}

static void lte_link_update(void)
{
	bool ready;
	bool came_up;
	bool went_down;
	k_spinlock_key_t key = k_spin_lock(&lte_link_lock);

	ready = lte_connected && lte_pdn_active;
	came_up = ready && !lte_link_up;
	went_down = !ready && lte_link_up;
	lte_link_up = ready;
	k_spin_unlock(&lte_link_lock, key);

	if (came_up)
	{
		LOG_INF("LTE link up");
	}
	else if (went_down)
	{
		LOG_WRN("LTE link down, waiting for the modem to reattach");
	}
}

bool lte_link_ready(void)
{
	bool ready;
	k_spinlock_key_t key = k_spin_lock(&lte_link_lock);

	ready = lte_link_up;
	k_spin_unlock(&lte_link_lock, key);

	return ready;
}

/* GNSS positions are not stored, so NTN gets the fixed location from Kconfig. */
static int ntn_static_location_set(void)
{
	double latitude = strtod(CONFIG_APP_NTN_LATITUDE, NULL);
	double longitude = strtod(CONFIG_APP_NTN_LONGITUDE, NULL);
	double altitude = strtod(CONFIG_APP_NTN_ALTITUDE_METERS, NULL);
	int err;

	err = ntn_location_set(latitude, longitude, (float)altitude,
						   CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS);
	if (err)
	{
		LOG_ERR("Failed to set NTN location: %d", err);
		return err;
	}

	LOG_INF("NTN location set: lat=%.6f lon=%.6f alt=%.1f validity=%d",
			latitude, longitude, altitude, CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS);

	return 0;
}

void ntn_handler(const struct ntn_evt *const evt)
{
	if (evt->type != NTN_EVT_LOCATION_REQUEST)
	{
		return;
	}

	LOG_INF("NTN location request: requested=%d accuracy=%u m",
			evt->location_request.requested,
			evt->location_request.accuracy);

	if (evt->location_request.requested)
	{
		(void)ntn_static_location_set();
	}
}

static char *modem_fw_version_trim(char *version)
{
	char *line_end;

	while (*version == '\r' || *version == '\n' || *version == ' ' || *version == '\t')
	{
		version++;
	}

	line_end = strpbrk(version, "\r\n");
	if (line_end != NULL)
	{
		*line_end = '\0';
	}

	return version;
}

static int modem_ident_string_get(const char *at_cmd, char *buf, size_t buf_len)
{
	int err;

	if (buf == NULL || buf_len == 0U)
	{
		return -EINVAL;
	}

	err = nrf_modem_at_cmd(buf, buf_len, "%s", at_cmd);
	if (err)
	{
		buf[0] = '\0';
		return err;
	}

	(void)modem_fw_version_trim(buf);
	return 0;
}

void log_modem_identity(void)
{
	char modem_model[APP_MODEM_ID_STRING_SIZE];
	char modem_hw[APP_MODEM_ID_STRING_SIZE];
	int err;

	LOG_INF("NTN requires nRF9151 LACA A1 hardware with mfw_nrf9151-ntn firmware");

	err = modem_ident_string_get("AT+CGMM", modem_model, sizeof(modem_model));
	if (!err && modem_model[0] != '\0')
	{
		LOG_INF("Detected modem model: %s", modem_model);
	}
	else
	{
		LOG_WRN("Detected modem model unavailable: %d", err);
	}

	err = modem_info_get_hw_version(modem_hw, sizeof(modem_hw));
	if (!err && modem_hw[0] != '\0')
	{
		LOG_INF("Detected modem HW version: %s", modem_hw);
	}
	else
	{
		LOG_WRN("Detected modem HW version unavailable: %d", err);
	}
}

static void log_ntn_system_mode_failure_diagnostics(void)
{
	char response[APP_MODEM_ID_STRING_SIZE];
	int err;

	(void)nrf_modem_at_printf("AT+CMEE=1");
	err = nrf_modem_at_cmd(response, sizeof(response), "AT%%XSYSTEMMODE=0,0,0,0,1");

	if (err > 0)
	{
		LOG_ERR("Raw AT%%XSYSTEMMODE failure: ret=%d type=%d code=%d resp='%s'",
				err, nrf_modem_at_err_type(err), nrf_modem_at_err(err),
				modem_fw_version_trim(response));
	}
	else if (err < 0)
	{
		LOG_ERR("Raw AT%%XSYSTEMMODE transport failure: %d", err);
	}
	else
	{
		LOG_WRN("Raw AT%%XSYSTEMMODE unexpectedly succeeded during diagnostics");
	}

	LOG_WRN("If NTN mode keeps failing, verify the module marking is nRF9151 LACA A1 "
			"and the flashed modem firmware is mfw_nrf9151-ntn");
}

int verify_modem_firmware(void)
{
	char modem_fw[APP_MODEM_FW_VERSION_SIZE];
	char *version;
	int err;

	if (strlen(CONFIG_APP_MODEM_FW_EXPECTED) == 0U)
	{
		LOG_WRN("Modem firmware boot check disabled");
		return 0;
	}

	err = nrf_modem_at_cmd(modem_fw, sizeof(modem_fw), "AT+CGMR");
	if (err)
	{
		LOG_ERR("Failed to read modem firmware version with AT+CGMR: %d", err);
		return err;
	}

	version = modem_fw_version_trim(modem_fw);
	if (version[0] == '\0')
	{
		LOG_ERR("Modem firmware version response was empty");
		return -EIO;
	}

	LOG_INF("Detected modem firmware: %s", version);
	if (strstr(version, CONFIG_APP_MODEM_FW_EXPECTED) == NULL)
	{
		LOG_ERR("Unsupported modem firmware. Expected token '%s' but got '%s'",
				CONFIG_APP_MODEM_FW_EXPECTED, version);
		return -EINVAL;
	}

	LOG_INF("Modem firmware check passed: %s", CONFIG_APP_MODEM_FW_EXPECTED);

	return 0;
}

int modem_enter_min_func(void)
{
	int err = nrf_modem_at_printf("AT+CFUN=0");

	if (err)
	{
		LOG_ERR("Failed to put modem in minimum functionality mode: %d", err);
	}

	return err;
}

int lte_modem_configure(void)
{
	int err;

	LOG_INF("Configuring modem for NTN");
	if (strlen(CONFIG_APP_APN) == 0U)
	{
		LOG_ERR("CONFIG_APP_APN must be set before starting the connection attempt");
		return -EINVAL;
	}

	err = modem_enter_min_func();
	if (err)
	{
		return err;
	}

	err = antenna_select(ANTENNA_PATH_LTE);
	if (err)
	{
		return err;
	}

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT,
								 LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err)
	{
		LOG_ERR("Failed to set modem to NTN-only mode: %d", err);
		log_ntn_system_mode_failure_diagnostics();
		return err;
	}
	LOG_INF("System mode set: NTN_NBIOT");

	err = nrf_modem_at_printf("AT%%XBANDLOCK=2,\"\",\"%s\"", CONFIG_APP_BAND_LOCK_LIST);
	if (err)
	{
		LOG_ERR("Failed to lock NTN bands %s: %d", CONFIG_APP_BAND_LOCK_LIST, err);
		LOG_WRN("Check that the configured band list contains NTN-only bands for "
				"LTE_LC_SYSTEM_MODE_NTN_NBIOT");
		return err;
	}
	LOG_INF("NTN band lock configured: %s", CONFIG_APP_BAND_LOCK_LIST);

	err = nrf_modem_at_printf("AT+CGDCONT=0,\"IP\",\"%s\"", CONFIG_APP_APN);
	if (err)
	{
		LOG_ERR("Failed to set APN for PDP context 0: %d", err);
		return err;
	}
	LOG_INF("APN configured on PDP context 0: %s", CONFIG_APP_APN);

	err = ntn_static_location_set();
	if (err)
	{
		return err;
	}

	LOG_INF("Modem configured for band lock %s, NTN mode, and APN '%s'",
			CONFIG_APP_BAND_LOCK_LIST, CONFIG_APP_APN);

	return 0;
}

int lte_attach_start(void)
{
	int err;

	lte_registered_set(false);
	lte_link_update();

	LOG_INF("Starting LTE attach");
	err = lte_lc_connect_async(lte_handler);
	if (err)
	{
		LOG_ERR("Failed to connect to LTE network: %d", err);
		return err;
	}

	return 0;
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type)
	{
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
			(evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING))
		{
			lte_registered_set(true);
			LOG_INF("Network registration status: %s",
					evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ? "Connected - home" : "Connected - roaming");
			lte_link_update();
		}
		else
		{
			lte_registered_set(false);
			lte_link_update();
		}
		break;
	case LTE_LC_EVT_PDN:
		switch (evt->pdn.type)
		{
		case LTE_LC_EVT_PDN_ACTIVATED:
			lte_pdn_active_set(true);
			LOG_INF("PDN activated: cid=%u", evt->pdn.cid);
			lte_link_update();
			break;
		case LTE_LC_EVT_PDN_DEACTIVATED:
			lte_pdn_active_set(false);
			LOG_WRN("PDN deactivated: cid=%u", evt->pdn.cid);
			lte_link_update();
			break;
		case LTE_LC_EVT_PDN_NETWORK_DETACH:
			lte_pdn_active_set(false);
			LOG_WRN("PDN network detach: cid=%u", evt->pdn.cid);
			lte_link_update();
			break;
		case LTE_LC_EVT_PDN_ESM_ERROR:
			lte_pdn_active_set(false);
			LOG_ERR("PDN ESM error: cid=%u esm=%d", evt->pdn.cid, evt->pdn.esm_err);
			lte_link_update();
			break;
		case LTE_LC_EVT_PDN_SUSPENDED:
			lte_pdn_active_set(false);
			LOG_WRN("PDN suspended: cid=%u", evt->pdn.cid);
			lte_link_update();
			break;
		case LTE_LC_EVT_PDN_RESUMED:
			lte_pdn_active_set(true);
			LOG_INF("PDN resumed: cid=%u", evt->pdn.cid);
			lte_link_update();
			break;
		case LTE_LC_EVT_PDN_IPV6_UP:
			LOG_INF("PDN IPv6 up: cid=%u", evt->pdn.cid);
			break;
		case LTE_LC_EVT_PDN_IPV6_DOWN:
			LOG_WRN("PDN IPv6 down: cid=%u", evt->pdn.cid);
			break;
		case LTE_LC_EVT_PDN_APN_RATE_CONTROL_ON:
			LOG_WRN("PDN APN rate control on: cid=%u", evt->pdn.cid);
			break;
		case LTE_LC_EVT_PDN_APN_RATE_CONTROL_OFF:
			LOG_INF("PDN APN rate control off: cid=%u", evt->pdn.cid);
			break;
		case LTE_LC_EVT_PDN_CTX_DESTROYED:
			lte_pdn_active_set(false);
			LOG_WRN("PDN context destroyed: cid=%u", evt->pdn.cid);
			lte_link_update();
			break;
		default:
			break;
		}
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		LOG_INF("PSM parameter update: TAU=%d s active_time=%d s",
				evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE:
		LOG_INF("eDRX parameter update: eDRX=%.2f s PTW=%.2f s",
				(double)evt->edrx_cfg.edrx, (double)evt->edrx_cfg.ptw);
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("RRC mode: %s",
				evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "Connected" : "Idle");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		LOG_INF("LTE cell changed: cell_id=%d tracking_area=%d",
				evt->cell.id, evt->cell.tac);
		break;
	default:
		break;
	}
}

/* Network details printed by 'lte send' once attached, from AT%XMONITOR
 *
 * %XMONITOR: <reg_status>[,<full_name>,<short_name>,<plmn>,<tac>,<AcT>,<band>,<cell_id>,
 *            <phys_cell_id>,<EARFCN>,<rsrp>,<snr>,<NW-provided_eDRX_value>,<Active-Time>,
 *            <Periodic-TAU-ext>,<Periodic-TAU>]
 */
#define XMONITOR_RESPONSE_SIZE 256
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

static void xmonitor_string(struct at_parser *parser, size_t index, char *buf, size_t size)
{
	size_t len = size;

	if (at_parser_string_get(parser, index, buf, &len) != 0 || buf[0] == '\0')
	{
		snprintk(buf, size, "-");
	}
}

int lte_info_print(const struct shell *sh)
{
	char response[XMONITOR_RESPONSE_SIZE];
	char full_name[32];
	char short_name[32];
	char plmn[8];
	char tac[12];
	char cell_id[12];
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
		shell_error(sh, "lte info: AT%%XMONITOR failed: %d", err);
		return -EIO;
	}

	err = at_parser_init(&parser, response);
	if (!err)
	{
		err = at_parser_num_get(&parser, 1, &reg_status);
	}
	if (err)
	{
		shell_error(sh, "lte info: cannot parse: %s", response);
		return -EBADMSG;
	}

	shell_print(sh, "Registration   %s (%d)", reg_status_text(reg_status), reg_status);
	if (reg_status != LTE_LC_NW_REG_REGISTERED_HOME &&
		reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)
	{
		shell_print(sh, "No network details (not registered)");
		return 0;
	}

	xmonitor_string(&parser, 2, full_name, sizeof(full_name));
	xmonitor_string(&parser, 3, short_name, sizeof(short_name));
	xmonitor_string(&parser, 4, plmn, sizeof(plmn));
	xmonitor_string(&parser, 5, tac, sizeof(tac));
	(void)at_parser_num_get(&parser, 6, &act);
	(void)at_parser_num_get(&parser, 7, &band);
	xmonitor_string(&parser, 8, cell_id, sizeof(cell_id));
	(void)at_parser_num_get(&parser, 9, &pci);
	(void)at_parser_num_get(&parser, 10, &earfcn);
	(void)at_parser_num_get(&parser, 11, &rsrp);
	(void)at_parser_num_get(&parser, 12, &snr);

	shell_print(sh, "Operator       %s / %s (full / short name)", full_name, short_name);
	/* PLMN = MCC (3 digits) + MNC (2 or 3 digits) */
	if (strlen(plmn) >= 5)
	{
		shell_print(sh, "MCC / MNC      %.3s / %s  (PLMN %s)", plmn, plmn + 3, plmn);
	}
	else
	{
		shell_print(sh, "MCC / MNC      - (PLMN %s)", plmn);
	}
	shell_print(sh, "Technology     %s (AcT %d)", act_text(act), act);
	shell_print(sh, "Band           %d", band);
	shell_print(sh, "EARFCN         %d", earfcn);
	shell_print(sh, "Cell ID        %s (%lu)", cell_id, strtoul(cell_id, NULL, 16));
	shell_print(sh, "Tracking area  %s (%lu)", tac, strtoul(tac, NULL, 16));
	shell_print(sh, "PCI            %d", pci);
	if (rsrp == XMONITOR_RSRP_UNKNOWN)
	{
		shell_print(sh, "RSRP           -");
	}
	else
	{
		shell_print(sh, "RSRP           %d dBm", RSRP_IDX_TO_DBM(rsrp));
	}
	if (snr == XMONITOR_SNR_UNKNOWN)
	{
		shell_print(sh, "SNR            -");
	}
	else
	{
		shell_print(sh, "SNR            %d dB", SNR_IDX_TO_DB(snr));
	}

	return 0;
}
