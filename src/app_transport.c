/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_client.h>

LOG_MODULE_REGISTER(app_transport, CONFIG_APP_LOG_LEVEL);

/* Extra wait on top of the CoAP retransmission sequence before the app gives up. */
#define COAP_RESPONSE_SLACK_MS 10000U

static int client_fd = -1;
static struct sockaddr_storage host_addr;
static socklen_t host_addr_len;
static char resolved_server_ip[APP_ENDPOINT_STRING_SIZE];
static bool host_addr_resolved;
static struct coap_client coap_client_ctx;
static int16_t coap_response_code;

enum coap_cancel_reason
{
	COAP_CANCEL_REASON_NONE,
	COAP_CANCEL_REASON_TIMEOUT,
	COAP_CANCEL_REASON_SOCKET_DISCONNECT,
};

static enum coap_cancel_reason coap_cancel_reason;

static struct coap_transmission_parameters coap_tx_params = {
	.ack_timeout = CONFIG_APP_COAP_ACK_TIMEOUT_MS,
#if defined(CONFIG_COAP_RANDOMIZE_ACK_TIMEOUT)
	.ack_random_percent = CONFIG_COAP_ACK_RANDOM_PERCENT,
#endif
	.coap_backoff_percent = CONFIG_COAP_BACKOFF_PERCENT,
	.max_retransmission = CONFIG_APP_COAP_MAX_RETRANSMIT,
};

K_SEM_DEFINE(coap_response_sem, 0, 1);

static void format_json_string(const char *src, char *dst, size_t dst_len)
{
	size_t out = 0;

	if (dst_len == 0U)
	{
		return;
	}

	while (src != NULL && *src != '\0' && out + 1 < dst_len)
	{
		unsigned char c = (unsigned char)*src++;

		switch (c)
		{
		case '"':
		case '\\':
			if (out + 2 >= dst_len)
			{
				goto done;
			}
			dst[out++] = '\\';
			dst[out++] = c;
			break;
		case '\b':
		case '\f':
		case '\n':
		case '\r':
		case '\t':
			if (out + 2 >= dst_len)
			{
				goto done;
			}
			dst[out++] = '\\';
			dst[out++] = (c == '\b') ? 'b' : (c == '\f') ? 'f'
										 : (c == '\n')	 ? 'n'
										 : (c == '\r')	 ? 'r'
														 : 't';
			break;
		default:
			if (c < 32U)
			{
				if (out + 6 >= dst_len)
				{
					goto done;
				}
				out += snprintk(&dst[out], dst_len - out, "\\u%04x", c);
			}
			else
			{
				dst[out++] = c;
			}
			break;
		}
	}

done:
	dst[out] = '\0';
}

static int build_thingsboard_resource_path(char *buffer, size_t buffer_len)
{
	int ret;

	if (strlen(CONFIG_APP_THINGSBOARD_ACCESS_TOKEN) == 0U)
	{
		LOG_ERR("CONFIG_APP_THINGSBOARD_ACCESS_TOKEN must be set before starting uploads");
		return -EINVAL;
	}

	ret = snprintk(buffer, buffer_len, "api/v1/%s/telemetry",
				   CONFIG_APP_THINGSBOARD_ACCESS_TOKEN);
	if (ret < 0 || (size_t)ret >= buffer_len)
	{
		LOG_ERR("ThingsBoard CoAP resource path is too long for the current CoAP "
				"client configuration");
		return -E2BIG;
	}

	return 0;
}

static void format_endpoint(const struct sockaddr *addr, char *buffer, size_t buffer_len)
{
	char ip[INET6_ADDRSTRLEN];
	uint16_t port;

	switch (addr->sa_family)
	{
	case AF_INET:
	{
		const struct sockaddr_in *addr4 = (const struct sockaddr_in *)addr;

		inet_ntop(AF_INET, &addr4->sin_addr, ip, sizeof(ip));
		port = ntohs(addr4->sin_port);
		break;
	}
	case AF_INET6:
	{
		const struct sockaddr_in6 *addr6 = (const struct sockaddr_in6 *)addr;

		inet_ntop(AF_INET6, &addr6->sin6_addr, ip, sizeof(ip));
		port = ntohs(addr6->sin6_port);
		break;
	}
	default:
		snprintk(buffer, buffer_len, "unknown");
		return;
	}

	snprintk(buffer, buffer_len, "%s:%u", ip, port);
}

static void format_ip_address(const struct sockaddr *addr, char *buffer, size_t buffer_len)
{
	switch (addr->sa_family)
	{
	case AF_INET:
	{
		const struct sockaddr_in *addr4 = (const struct sockaddr_in *)addr;

		inet_ntop(AF_INET, &addr4->sin_addr, buffer, buffer_len);
		break;
	}
	case AF_INET6:
	{
		const struct sockaddr_in6 *addr6 = (const struct sockaddr_in6 *)addr;

		inet_ntop(AF_INET6, &addr6->sin6_addr, buffer, buffer_len);
		break;
	}
	default:
		snprintk(buffer, buffer_len, "unknown");
		break;
	}
}

static int resolve_server_address(void)
{
	char port_str[8];
	struct addrinfo hints = {0};
	struct addrinfo *result = NULL;
	struct sockaddr_in addr4 = {0};
	char resolved_ip[APP_ENDPOINT_STRING_SIZE];
	const char *server_host = CONFIG_APP_THINGSBOARD_HOST;
	int err;

	if (host_addr_resolved)
	{
		return 0;
	}

	if (strlen(CONFIG_APP_THINGSBOARD_IPV4) != 0U)
	{
		server_host = CONFIG_APP_THINGSBOARD_IPV4;
	}

	if (strlen(server_host) == 0U)
	{
		LOG_ERR("A ThingsBoard host or IPv4 address must be configured before starting uploads");
		return -EINVAL;
	}

	snprintk(port_str, sizeof(port_str), "%d", CONFIG_APP_THINGSBOARD_PORT);

	if (inet_pton(AF_INET, server_host, &addr4.sin_addr) == 1)
	{
		addr4.sin_family = AF_INET;
		addr4.sin_port = htons(CONFIG_APP_THINGSBOARD_PORT);

		memcpy(&host_addr, &addr4, sizeof(addr4));
		host_addr_len = sizeof(addr4);
		snprintk(resolved_server_ip, sizeof(resolved_server_ip), "%s",
				 server_host);
		host_addr_resolved = true;

		LOG_INF("Using configured ThingsBoard IPv4 address %s:%d",
				resolved_server_ip, CONFIG_APP_THINGSBOARD_PORT);
		return 0;
	}

	LOG_INF("Resolving ThingsBoard host '%s' over IPv4",
			server_host);

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	err = getaddrinfo(server_host, port_str, &hints, &result);
	if (err || result == NULL)
	{
		LOG_ERR("Failed to resolve ThingsBoard host '%s': %d",
				server_host, err);
		return -EHOSTUNREACH;
	}

	if (result->ai_addrlen > sizeof(host_addr))
	{
		freeaddrinfo(result);
		return -EOVERFLOW;
	}

	format_ip_address(result->ai_addr, resolved_ip, sizeof(resolved_ip));
	memcpy(&host_addr, result->ai_addr, result->ai_addrlen);
	host_addr_len = (socklen_t)result->ai_addrlen;
	freeaddrinfo(result);
	snprintk(resolved_server_ip, sizeof(resolved_server_ip), "%s", resolved_ip);
	host_addr_resolved = true;

	LOG_INF("Resolved ThingsBoard host '%s' to %s",
			server_host, resolved_server_ip);

	return 0;
}

static void format_telemetry_payload(char *buffer, size_t buffer_len)
{
	static const char json_prefix[] = "{\"message\":\"";
	char json_suffix[96];
	char escaped_message[APP_MESSAGE_STRING_SIZE];
	size_t max_message_len;
	int ret;

	/* GNSS positions are not stored, so the message carries the configured location. */
	ret = snprintf(json_suffix, sizeof(json_suffix),
				   "\",\"latitude\":%.6f,\"longitude\":%.6f,\"altitude\":%.1f}",
				   strtod(CONFIG_APP_NTN_LATITUDE, NULL), strtod(CONFIG_APP_NTN_LONGITUDE, NULL),
				   strtod(CONFIG_APP_NTN_ALTITUDE_METERS, NULL));
	if (ret < 0 || (size_t)ret >= sizeof(json_suffix) ||
		buffer_len < sizeof(json_prefix) + (size_t)ret + 1U)
	{
		buffer[0] = '\0';
		return;
	}

	format_json_string(CONFIG_APP_TELEMETRY_MESSAGE, escaped_message, sizeof(escaped_message));
	max_message_len = buffer_len - sizeof(json_prefix) - (size_t)ret - 1U;
	snprintk(buffer, buffer_len, "%s%.*s%s",
			 json_prefix, (int)max_message_len, escaped_message, json_suffix);
}

static void format_raw_payload(const char *payload, size_t payload_len, char *buffer,
							   size_t buffer_len)
{
	size_t out = 0;

	if (buffer_len < 3)
	{
		return;
	}

	buffer[out++] = 'b';
	buffer[out++] = '\'';

	for (size_t i = 0; i < payload_len && out + 1 < buffer_len; i++)
	{
		unsigned char c = payload[i];

		if ((c == '\\') || (c == '\''))
		{
			if (out + 2 >= buffer_len)
			{
				break;
			}

			buffer[out++] = '\\';
			buffer[out++] = c;
			continue;
		}

		if ((c >= 32U) && (c <= 126U))
		{
			buffer[out++] = c;
			continue;
		}

		if (out + 4 >= buffer_len)
		{
			break;
		}

		out += snprintk(&buffer[out], buffer_len - out, "\\x%02x", c);
	}

	if (out + 2 <= buffer_len)
	{
		buffer[out++] = '\'';
	}

	buffer[out] = '\0';
}

/* Worst-case time the CoAP client needs to send a request and all its
 * retransmissions and wait for the last ACK, using the same parameters it is
 * given in coap_client_req(). The client reports -ETIMEDOUT by itself at that
 * point; the slack only covers the case where that callback never arrives.
 */
static uint32_t coap_response_timeout_ms(void)
{
	uint32_t timeout = coap_tx_params.ack_timeout;
	uint32_t total = 0U;

#if defined(CONFIG_COAP_RANDOMIZE_ACK_TIMEOUT)
	timeout = timeout * coap_tx_params.ack_random_percent / 100U;
#endif

	for (uint8_t i = 0U; i <= coap_tx_params.max_retransmission; i++)
	{
		total += timeout;
		timeout = timeout * coap_tx_params.coap_backoff_percent / 100U;
	}

	return total + COAP_RESPONSE_SLACK_MS;
}

static void log_coap_telemetry_request(const char *payload, size_t payload_len)
{
	char timestamp[APP_TIMESTAMP_STRING_SIZE];
	char from[APP_ENDPOINT_STRING_SIZE];
	char to[APP_ENDPOINT_STRING_SIZE];
	char raw[APP_RAW_PAYLOAD_STRING_SIZE];
	struct sockaddr_storage local_addr;
	socklen_t local_addr_len = sizeof(local_addr);

	format_timestamp(timestamp, sizeof(timestamp));
	format_endpoint((const struct sockaddr *)&host_addr, to, sizeof(to));
	format_raw_payload(payload, payload_len, raw, sizeof(raw));

	if (getsockname(client_fd, (struct sockaddr *)&local_addr, &local_addr_len) == 0)
	{
		format_endpoint((const struct sockaddr *)&local_addr, from, sizeof(from));
	}
	else
	{
		snprintk(from, sizeof(from), "unknown");
	}

	LOG_INF("Telemetry request ts=%s from=%s to=%s bytes=%u",
			timestamp, from, to, (unsigned int)payload_len);
	LOG_INF("Telemetry raw: %s", raw);
	LOG_INF("Telemetry json: %.*s", (int)payload_len, payload);
}

static void coap_response_cb(const struct coap_client_response_data *data, void *user_data)
{
	ARG_UNUSED(user_data);

	if (data->result_code < 0)
	{
		if (data->result_code == -ECANCELED)
		{
			switch (coap_cancel_reason)
			{
			case COAP_CANCEL_REASON_TIMEOUT:
				LOG_WRN("CoAP request canceled locally after %u ms response timeout",
						coap_response_timeout_ms());
				break;

			case COAP_CANCEL_REASON_SOCKET_DISCONNECT:
				LOG_WRN("CoAP request canceled locally because the socket was closed");
				break;

			default:
				LOG_WRN("CoAP request canceled locally");
				break;
			}
		}
		else
		{
			LOG_ERR("CoAP response error: %d", data->result_code);
		}

		coap_response_code = data->result_code;
		k_sem_give(&coap_response_sem);
		return;
	}

	if (data->payload != NULL && data->payload_len > 0U)
	{
		LOG_INF("CoAP response payload: %.*s",
				(int)data->payload_len, (const char *)data->payload);
	}

	if (!data->last_block)
	{
		return;
	}

	LOG_INF("CoAP response code: 0x%x", data->result_code);
	coap_response_code = data->result_code;
	k_sem_give(&coap_response_sem);
}

static void socket_disconnect(void)
{
	coap_cancel_reason = COAP_CANCEL_REASON_SOCKET_DISCONNECT;
	coap_client_cancel_requests(&coap_client_ctx);

	if (client_fd >= 0)
	{
		(void)close(client_fd);
		client_fd = -1;
	}
}

int coap_client_start(void)
{
	int err = coap_client_init(&coap_client_ctx, "thingsboard");

	if (err)
	{
		LOG_ERR("Failed to initialize CoAP client: %d", err);
		return err;
	}

	return 0;
}

static int socket_connect(void)
{
	int err;

	err = resolve_server_address();
	if (err)
	{
		return err;
	}

	LOG_INF("Opening CoAP socket to %s:%d",
			resolved_server_ip, CONFIG_APP_THINGSBOARD_PORT);
	client_fd = socket(((struct sockaddr *)&host_addr)->sa_family, SOCK_DGRAM, IPPROTO_UDP);
	if (client_fd < 0)
	{
		err = -errno;
		LOG_ERR("Failed to create CoAP socket: %d", err);
		return err;
	}

	err = connect(client_fd, (struct sockaddr *)&host_addr, host_addr_len);
	if (err < 0)
	{
		err = -errno;
		LOG_ERR("Failed to connect socket: %d", err);
		socket_disconnect();
		return err;
	}

	LOG_INF("CoAP socket connected");

	return 0;
}

/* The LTE message test can wait minutes for the network, so it runs on its own
 * work queue instead of blocking the shell. Output goes to the shell that
 * started it.
 */
#define LTE_TEST_STACK_SIZE 4096
#define LTE_TEST_PRIORITY 5

K_THREAD_STACK_DEFINE(lte_test_stack, LTE_TEST_STACK_SIZE);
static struct k_work_q lte_test_workq;
static struct k_work lte_test_work;
static const struct shell *lte_test_sh;
static atomic_t lte_test_abort;
static atomic_t lte_test_running;

/* Waits for registration + PDN, or until timeout or abort. */
static int lte_wait_for_link(void)
{
	int64_t deadline = k_uptime_get() + CONFIG_APP_LTE_ATTACH_TIMEOUT_SECONDS * 1000LL;

	while (!lte_link_ready())
	{
		if (atomic_get(&lte_test_abort))
		{
			return -ECANCELED;
		}
		if (k_uptime_get() > deadline)
		{
			return -ETIMEDOUT;
		}
		k_msleep(200);
	}

	return 0;
}

static int coap_post_telemetry(void)
{
	int err;
	char payload[APP_MESSAGE_STRING_SIZE];
	size_t payload_len;
	struct coap_client_request req = {
		.method = COAP_METHOD_POST,
		.confirmable = true,
		.fmt = COAP_CONTENT_FORMAT_APP_JSON,
		.cb = coap_response_cb,
	};

	format_telemetry_payload(payload, sizeof(payload));
	payload_len = strlen(payload);
	err = build_thingsboard_resource_path(req.path, sizeof(req.path));
	if (err)
	{
		return err;
	}

	req.payload = (const uint8_t *)payload;
	req.len = payload_len;

	LOG_INF("Posting CoAP telemetry JSON (%u bytes) to coap://%s:%d/api/v1/<token>/telemetry",
			(unsigned int)payload_len, resolved_server_ip, CONFIG_APP_THINGSBOARD_PORT);
	log_coap_telemetry_request(payload, payload_len);

	k_sem_reset(&coap_response_sem);
	coap_response_code = -ETIMEDOUT;
	coap_cancel_reason = COAP_CANCEL_REASON_NONE;
	err = coap_client_req(&coap_client_ctx, client_fd, NULL, &req, &coap_tx_params);
	if (err < 0)
	{
		LOG_ERR("Failed to start CoAP telemetry upload: %d", err);
		return err;
	}

	err = k_sem_take(&coap_response_sem, K_MSEC(coap_response_timeout_ms()));
	if (err)
	{
		coap_cancel_reason = COAP_CANCEL_REASON_TIMEOUT;
		coap_client_cancel_requests(&coap_client_ctx);
		LOG_ERR("Timed out waiting for CoAP response: %d", err);
		return -ETIMEDOUT;
	}

	if (coap_response_code < 0)
	{
		return coap_response_code;
	}

	return 0;
}

static void lte_test_work_fn(struct k_work *work)
{
	const struct shell *sh = lte_test_sh;
	unsigned int code_class;
	unsigned int code_detail;
	int err;

	ARG_UNUSED(work);

	/* Resolve the host again on every run. */
	host_addr_resolved = false;

	err = lte_modem_configure();
	if (!err)
	{
		err = lte_attach_start();
	}
	if (err)
	{
		shell_error(sh, "lte: modem setup failed: %d", err);
		goto out;
	}

	err = lte_wait_for_link();
	if (err == -ECANCELED)
	{
		shell_warn(sh, "lte: aborted while waiting for the network");
		goto out;
	}
	if (err)
	{
		shell_error(sh, "lte: no network after %d s", CONFIG_APP_LTE_ATTACH_TIMEOUT_SECONDS);
		goto out;
	}

	shell_print(sh, "lte: network attached, sending message");
	err = socket_connect();
	if (err)
	{
		shell_error(sh, "lte: socket setup failed: %d", err);
		goto out;
	}

	err = coap_post_telemetry();
	if (err)
	{
		shell_error(sh, "lte: message send failed: %d", err);
		goto out;
	}

	code_class = (uint16_t)coap_response_code >> 5;
	code_detail = (uint16_t)coap_response_code & 0x1f;
	if (code_class == 2U)
	{
		shell_print(sh, "lte: message accepted by the server (CoAP %u.%02u)",
					code_class, code_detail);
	}
	else
	{
		shell_error(sh, "lte: message rejected by the server (CoAP %u.%02u)",
					code_class, code_detail);
	}

out:
	socket_disconnect();
	(void)modem_enter_min_func();
	shell_print(sh, "lte: finished, modem offline");
	atomic_set(&lte_test_running, 0);
	modem_test_release();
}

void lte_message_init(void)
{
	k_work_queue_init(&lte_test_workq);
	k_work_queue_start(&lte_test_workq, lte_test_stack, K_THREAD_STACK_SIZEOF(lte_test_stack),
					   LTE_TEST_PRIORITY, NULL);
	k_work_init(&lte_test_work, lte_test_work_fn);
}

void lte_message_start(const struct shell *sh)
{
	lte_test_sh = sh;
	atomic_set(&lte_test_abort, 0);
	atomic_set(&lte_test_running, 1);

	shell_print(sh, "lte: NTN attach, one CoAP POST with the configured location, then detach. "
				"Attach can take minutes (timeout %d s); 'lte abort' cancels.",
				CONFIG_APP_LTE_ATTACH_TIMEOUT_SECONDS);
	k_work_submit_to_queue(&lte_test_workq, &lte_test_work);
}

int lte_message_abort(void)
{
	if (!atomic_get(&lte_test_running))
	{
		return -EALREADY;
	}

	atomic_set(&lte_test_abort, 1);
	return 0;
}
