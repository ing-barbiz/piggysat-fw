#ifndef APP_INTERNAL_H_
#define APP_INTERNAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>

#include <zephyr/kernel.h>

#include <modem/lte_lc.h>
#include <modem/ntn.h>

#define APP_ENDPOINT_STRING_SIZE 64
#define APP_TIMESTAMP_STRING_SIZE 32
#define APP_MESSAGE_STRING_SIZE 192
#define APP_RAW_PAYLOAD_STRING_SIZE \
	((APP_MESSAGE_STRING_SIZE - 1) * 4 + 4)
#define APP_MODEM_FW_VERSION_SIZE 128
#define APP_MODEM_ID_STRING_SIZE 128

enum antenna_path
{
	ANTENNA_PATH_LTE,
	ANTENNA_PATH_GNSS,
};

/* RTT console input (main.c) */
bool console_key_pressed(void);
void console_flush_input(void);

int antenna_init(void);
int antenna_select(enum antenna_path path);

int leds_init(void);
void test_leds(void);

void test_gnss_nmea(void);

void test_lte_cw(void);

int verify_modem_firmware(void);
void log_modem_identity(void);
int modem_enter_min_func(void);
int lte_modem_configure(void);
int lte_attach_start(void);
bool lte_link_ready(void);
void ntn_handler(const struct ntn_evt *const evt);

int coap_client_start(void);
void test_lte_message(void);

void format_timestamp(char *buffer, size_t buffer_len);

#endif
