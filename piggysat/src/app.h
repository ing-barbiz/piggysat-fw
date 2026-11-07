/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_H_
#define APP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/* antenna.c: J3 antenna selection switch */
enum antenna_path
{
	ANTENNA_PATH_LTE,
	ANTENNA_PATH_GNSS,
};

int antenna_init(void);
int antenna_select(enum antenna_path path);

/* leds.c */
enum led_id
{
	LED_GNSS, /* green */
	LED_LTE,  /* orange */
	LED_SENT, /* red */
};

int leds_init(void);
void led_blink(enum led_id led);
void led_on_for(enum led_id led, k_timeout_t duration);
void led_off(enum led_id led);

/* magnet.c: magnet sensor; a present magnet puts the app in standby */
int magnet_init(void);
bool magnet_present(void);
void magnet_wait(k_timeout_t timeout);

/* gnss.c */
struct gnss_fix
{
	double latitude;
	double longitude;
	float altitude;
	float accuracy;
	uint8_t satellites;
	uint32_t ttff_s;
	char utc[32]; /* "YYYY-MM-DDTHH:MM:SSZ" */
};

int gnss_acquire(struct gnss_fix *fix, int timeout_s);

/* modem.c */
int modem_init(void);
int modem_offline(void);
void modem_set_location(double latitude, double longitude, float altitude);
int lte_configure(void);
int lte_connect_start(void);
int lte_wait_for_link(int timeout_s);
void lte_info_log(void);

/* transport.c */
int transport_init(void);
int transport_send(const char *payload, size_t len, int *coap_class, int *coap_detail);

#endif
