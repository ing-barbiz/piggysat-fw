/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <nrf_modem_at.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(simtest, CONFIG_APP_LOG_LEVEL);

#define SIM_INIT_TIMEOUT_MS 5000
#define SIM_POLL_MS 250
#define SIM_RESPONSE_SIZE 64

/* Cuts an AT response at the end of its first line. */
static char *first_line(char *resp)
{
	char *end = strpbrk(resp, "\r\n");

	if (end != NULL)
	{
		*end = '\0';
	}

	return resp;
}

static const char *cme_error_text(int code)
{
	switch (code)
	{
	case 10:
		return "SIM not inserted";
	case 13:
		return "SIM failure";
	case 14:
		return "SIM busy";
	case 15:
		return "wrong SIM";
	default:
		return "see the AT command reference";
	}
}

/* "%XSIM: <state>" is 1 when the UICC is initialised and ready. */
static int sim_state(void)
{
	char resp[SIM_RESPONSE_SIZE];
	const char *value;

	if (nrf_modem_at_cmd(resp, sizeof(resp), "AT%%XSIM?") != 0)
	{
		return -1;
	}

	value = strstr(resp, "%XSIM:");
	return value != NULL ? atoi(value + sizeof("%XSIM:") - 1) : -1;
}

static bool sim_wait_ready(void)
{
	for (int t = 0; t < SIM_INIT_TIMEOUT_MS; t += SIM_POLL_MS)
	{
		if (sim_state() == 1)
		{
			return true;
		}
		k_msleep(SIM_POLL_MS);
	}

	return false;
}

static void sim_print(const struct shell *sh, const char *label, const char *cmd)
{
	char resp[SIM_RESPONSE_SIZE];
	int err = nrf_modem_at_cmd(resp, sizeof(resp), "%s", cmd);

	if (err > 0 && nrf_modem_at_err_type(err) == NRF_MODEM_AT_CME_ERROR)
	{
		shell_print(sh, "  %-6s error (CME %d: %s)", label, nrf_modem_at_err(err),
					cme_error_text(nrf_modem_at_err(err)));
	}
	else if (err > 0)
	{
		shell_print(sh, "  %-6s ERROR", label);
	}
	else if (err < 0)
	{
		shell_print(sh, "  %-6s error (%d)", label, err);
	}
	else
	{
		shell_print(sh, "  %-6s %s", label, first_line(resp));
	}
}

int sim_test(const struct shell *sh)
{
	int err;

	shell_print(sh, "sim: powering the SIM interface only (no radio)");

	err = modem_enter_min_func();
	if (err)
	{
		return err;
	}

	/* The UICC can only be activated in an LTE system mode; a previous GNSS test
	 * leaves the modem in GPS-only mode.
	 */
	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err)
	{
		shell_error(sh, "sim: failed to set NTN system mode: %d", err);
		goto out;
	}

	/* Numeric CME errors, so a missing SIM is reported as CME 10. */
	(void)nrf_modem_at_printf("AT+CMEE=1");

	/* CFUN=41 activates the UICC without turning on the radio. */
	err = nrf_modem_at_printf("AT+CFUN=41");
	if (err)
	{
		shell_error(sh, "sim: AT+CFUN=41 failed (error type %d, code %d)",
					nrf_modem_at_err_type(err), nrf_modem_at_err(err));
		goto out;
	}

	if (sim_wait_ready())
	{
		shell_print(sh, "sim: SIM PRESENT");
		sim_print(sh, "PIN:", "AT+CPIN?");
		sim_print(sh, "ICCID:", "AT%XICCID");
		sim_print(sh, "IMSI:", "AT+CIMI");
	}
	else
	{
		shell_error(sh, "sim: SIM NOT DETECTED (not ready after %d s)",
					SIM_INIT_TIMEOUT_MS / 1000);
		sim_print(sh, "XSIM:", "AT%XSIM?");
		sim_print(sh, "PIN:", "AT+CPIN?");
		err = -ENODEV;
	}

out:
	(void)modem_enter_min_func();
	shell_print(sh, "sim: finished, modem offline");
	return err;
}
