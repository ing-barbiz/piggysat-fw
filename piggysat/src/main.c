/*
 * SPDX-License-Identifier: Apache-2.0
 */

/* PiggySat: every CONFIG_APP_REPORT_INTERVAL_SECONDS
 *   1. acquire a GNSS fix                  (GNSS LED blinks); no fix: abort the cycle
 *   2. attach to NTN and send the position (LTE LED blinks)
 *   3. on success, sent LED on for CONFIG_APP_SENT_LED_ON_SECONDS
 *   4. detach and sleep with the modem off
 */

#include "app.h"

#include <stdio.h>
#include <string.h>

#include <app_version.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define PAYLOAD_SIZE 256

/* Copies src into dst escaping '"' and '\' for a JSON string. */
static void json_escape(const char *src, char *dst, size_t size)
{
	size_t out = 0;

	for (; *src != '\0' && out + 2 < size; src++)
	{
		if (*src == '"' || *src == '\\')
		{
			dst[out++] = '\\';
		}
		dst[out++] = *src;
	}
	dst[out] = '\0';
}

static int build_payload(char *buf, size_t size, const struct gnss_fix *fix)
{
	char message[64];

	json_escape(CONFIG_APP_TELEMETRY_MESSAGE, message, sizeof(message));
	return snprintf(buf, size,
					"{\"message\":\"%s\",\"latitude\":%.6f,\"longitude\":%.6f,"
					"\"altitude\":%.1f,\"accuracy\":%.1f,\"satellites\":%u,\"ttff\":%u,"
					"\"gnss_time\":\"%s\"}",
					message, fix->latitude, fix->longitude, (double)fix->altitude,
					(double)fix->accuracy, fix->satellites, (unsigned int)fix->ttff_s, fix->utc);
}

/* Attach, send, detach. Returns 0 when the server accepted the message. */
static int send_report(const char *payload, size_t len)
{
	int coap_class = 0;
	int coap_detail = 0;
	int err;

	led_blink(LED_LTE);

	err = lte_configure();
	if (!err)
	{
		err = lte_connect_start();
	}
	if (!err)
	{
		err = lte_wait_for_link(CONFIG_APP_LTE_ATTACH_TIMEOUT_SECONDS);
		if (err)
		{
			LOG_WRN("LTE: no network within %d s", CONFIG_APP_LTE_ATTACH_TIMEOUT_SECONDS);
		}
	}
	if (!err)
	{
		LOG_INF("LTE: attached, sending");
		err = transport_send(payload, len, &coap_class, &coap_detail);
	}

	led_off(LED_LTE);

	if (!err)
	{
		LOG_INF("Message accepted (CoAP %d.%02d)", coap_class, coap_detail);
		led_on_for(LED_SENT, K_SECONDS(CONFIG_APP_SENT_LED_ON_SECONDS));
	}
	else
	{
		LOG_ERR("Message not delivered: %d (CoAP %d.%02d)", err, coap_class, coap_detail);
	}

	/* CFUN=0 detaches first; over NTN this takes tens of seconds. */
	LOG_INF("LTE: detaching");
	(void)modem_offline();
	return err;
}

static void run_cycle(unsigned int cycle)
{
	struct gnss_fix fix;
	char payload[PAYLOAD_SIZE];
	int len;

	LOG_INF("=== Cycle %u ===", cycle);

	led_blink(LED_GNSS);
	if (gnss_acquire(&fix, CONFIG_APP_GNSS_FIX_TIMEOUT_SECONDS) != 0)
	{
		led_off(LED_GNSS);
		LOG_WRN("No GNSS fix: cycle aborted, nothing sent");
		return;
	}
	led_off(LED_GNSS);

	LOG_INF("GNSS fix: %.6f, %.6f, alt %.1f m, accuracy %.1f m, %u sats, %u s",
			fix.latitude, fix.longitude, (double)fix.altitude, (double)fix.accuracy,
			fix.satellites, (unsigned int)fix.ttff_s);

	/* NTN attach uses the position of this cycle's fix. */
	modem_set_location(fix.latitude, fix.longitude, fix.altitude);

	len = build_payload(payload, sizeof(payload), &fix);
	if (len < 0 || (size_t)len >= sizeof(payload))
	{
		LOG_ERR("Payload too long");
		return;
	}

	(void)send_report(payload, len);
}

int main(void)
{
	unsigned int cycle = 0;
	int err;

	LOG_INF("PiggySat v%s started: report every %d s", APP_VERSION_STRING,
			CONFIG_APP_REPORT_INTERVAL_SECONDS);

	err = antenna_init();
	if (!err)
	{
		err = leds_init();
	}
	if (!err)
	{
		err = modem_init();
	}
	if (!err)
	{
		err = transport_init();
	}
	if (err)
	{
		LOG_ERR("Init failed (%d), stopping", err);
		return 0;
	}

	for (;;)
	{
		int64_t next = k_uptime_get() + CONFIG_APP_REPORT_INTERVAL_SECONDS * 1000LL;

		run_cycle(++cycle);

		/* Fixed period from the start of each cycle; a long cycle starts the next at once. */
		LOG_INF("Sleeping until the next cycle");
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}

	return 0;
}
