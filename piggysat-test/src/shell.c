/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app.h"

#include <errno.h>
#include <string.h>

#include <zephyr/shell/shell.h>

/* Tests that reconfigure the modem (CW, GNSS, LTE, SIM) run one at a time. */
static bool modem_available;
static atomic_t modem_busy;
static const char *modem_owner = "none";

void modem_tests_enable(void)
{
	modem_available = true;
}

static bool modem_test_claim(const struct shell *sh, const char *name)
{
	if (!modem_available)
	{
		shell_error(sh, "Modem not available, see the boot log");
		return false;
	}

	if (!atomic_cas(&modem_busy, 0, 1))
	{
		shell_error(sh, "Modem busy: '%s' test is running, stop it first", modem_owner);
		return false;
	}

	modem_owner = name;
	return true;
}

void modem_test_release(void)
{
	modem_owner = "none";
	atomic_set(&modem_busy, 0);
}

static bool modem_test_is(const char *name)
{
	return atomic_get(&modem_busy) && strcmp(modem_owner, name) == 0;
}

/* led */

static int cmd_led_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (leds_test_start() == -EALREADY)
	{
		shell_warn(sh, "led: already running");
		return 0;
	}

	shell_print(sh, "led: cycling gnss (green), lte (orange), sent (red), then all on. 'led stop' ends the test.");
	return 0;
}

static int cmd_led_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (leds_test_stop() == -EALREADY)
	{
		shell_warn(sh, "led: not running");
		return 0;
	}

	shell_print(sh, "led: stopped");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_led,
	SHELL_CMD(start, NULL, "Cycle the LEDs every 0.5 s.", cmd_led_start),
	SHELL_CMD(stop, NULL, "Stop the LED test and turn the LEDs off.", cmd_led_stop),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(led, &sub_led, "LED test", NULL);

/* button */

static int cmd_button_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (button_test_start(sh) == -EALREADY)
	{
		shell_warn(sh, "button: already running");
	}

	return 0;
}

static int cmd_button_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (button_test_stop(sh) == -EALREADY)
	{
		shell_warn(sh, "button: not running");
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_button,
	SHELL_CMD(start, NULL, "Report touch button presses and releases.", cmd_button_start),
	SHELL_CMD(stop, NULL, "Stop the button test.", cmd_button_stop),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(button, &sub_button, "Touch button test", NULL);

/* cw */

static int cmd_cw_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!modem_test_claim(sh, "cw"))
	{
		return -EBUSY;
	}

	if (cw_start(sh) != 0)
	{
		modem_test_release();
		return -EIO;
	}

	return 0;
}

static int cmd_cw_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!modem_test_is("cw") || cw_stop(sh) == -EALREADY)
	{
		shell_warn(sh, "cw: not running");
		return 0;
	}

	modem_test_release();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cw,
	SHELL_CMD(start, NULL, "Start the CW transmission (AT%XRFTEST) and print the measured power.",
			  cmd_cw_start),
	SHELL_CMD(stop, NULL, "Stop the CW transmission.", cmd_cw_stop),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(cw, &sub_cw, "LTE CW TX test (band, frequency and power from Kconfig)", NULL);

/* gnss */

static int cmd_gnss_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!modem_test_claim(sh, "gnss"))
	{
		return -EBUSY;
	}

	if (gnss_test_start(sh) != 0)
	{
		modem_test_release();
		return -EIO;
	}

	return 0;
}

static int cmd_gnss_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!modem_test_is("gnss") || gnss_test_stop(sh) == -EALREADY)
	{
		shell_warn(sh, "gnss: not running");
		return 0;
	}

	modem_test_release();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_gnss,
	SHELL_CMD(start, NULL, "Start continuous GNSS reception with NMEA output.", cmd_gnss_start),
	SHELL_CMD(stop, NULL, "Stop GNSS reception.", cmd_gnss_stop),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(gnss, &sub_gnss, "GNSS test (positions are not stored)", NULL);

/* lte */

static int cmd_lte_send(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!modem_test_claim(sh, "lte"))
	{
		return -EBUSY;
	}

	/* Runs in the background; the job releases the modem when it ends. */
	lte_message_start(sh);
	return 0;
}

static int cmd_lte_abort(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (lte_message_abort() == -EALREADY)
	{
		shell_warn(sh, "lte: not running");
		return 0;
	}

	shell_print(sh, "lte: aborting...");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_lte,
	SHELL_CMD(send, NULL, "NTN attach and send one CoAP message with the configured location.", cmd_lte_send),
	SHELL_CMD(abort, NULL, "Abort a running 'lte send' while it waits for the network.",
			  cmd_lte_abort),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(lte, &sub_lte, "LTE message test", NULL);

/* sim */

static int cmd_sim(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!modem_test_claim(sh, "sim"))
	{
		return -EBUSY;
	}

	err = sim_test(sh);
	modem_test_release();
	return err;
}

SHELL_CMD_REGISTER(sim, NULL, "Check that a SIM card is present (ICCID, IMSI)", cmd_sim);
