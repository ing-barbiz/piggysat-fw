/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_client.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(transport, CONFIG_APP_LOG_LEVEL);

/* Extra wait on top of the CoAP retransmission sequence before giving up. */
#define COAP_RESPONSE_SLACK_MS 10000U

static struct coap_client coap_client_ctx;
static int16_t coap_response_code;

K_SEM_DEFINE(coap_response_sem, 0, 1);

static struct coap_transmission_parameters coap_tx_params = {
	.ack_timeout = CONFIG_APP_COAP_ACK_TIMEOUT_MS,
#if defined(CONFIG_COAP_RANDOMIZE_ACK_TIMEOUT)
	.ack_random_percent = CONFIG_COAP_ACK_RANDOM_PERCENT,
#endif
	.coap_backoff_percent = CONFIG_COAP_BACKOFF_PERCENT,
	.max_retransmission = CONFIG_APP_COAP_MAX_RETRANSMIT,
};

/* Worst-case time the CoAP client needs for the request and all its retransmissions. */
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

static void coap_response_cb(const struct coap_client_response_data *data, void *user_data)
{
	ARG_UNUSED(user_data);

	if (data->result_code >= 0 && !data->last_block)
	{
		return;
	}

	coap_response_code = data->result_code;
	k_sem_give(&coap_response_sem);
}

/* Resolved on every send: the NTN link may come up with a different DNS. */
static int resolve_server(struct sockaddr_storage *addr, socklen_t *addr_len)
{
	const char *host = strlen(CONFIG_APP_THINGSBOARD_IPV4) ? CONFIG_APP_THINGSBOARD_IPV4
														   : CONFIG_APP_THINGSBOARD_HOST;
	struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
	struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
	struct addrinfo *result = NULL;
	char port[8];
	int err;

	memset(addr, 0, sizeof(*addr));
	if (inet_pton(AF_INET, host, &addr4->sin_addr) == 1)
	{
		addr4->sin_family = AF_INET;
		addr4->sin_port = htons(CONFIG_APP_THINGSBOARD_PORT);
		*addr_len = sizeof(*addr4);
		return 0;
	}

	snprintk(port, sizeof(port), "%d", CONFIG_APP_THINGSBOARD_PORT);
	err = getaddrinfo(host, port, &hints, &result);
	if (err || result == NULL)
	{
		LOG_ERR("Cannot resolve '%s': %d", host, err);
		return -EHOSTUNREACH;
	}

	memcpy(addr, result->ai_addr, result->ai_addrlen);
	*addr_len = result->ai_addrlen;
	freeaddrinfo(result);
	return 0;
}

int transport_init(void)
{
	int err = coap_client_init(&coap_client_ctx, "thingsboard");

	if (err)
	{
		LOG_ERR("CoAP client init failed: %d", err);
	}
	return err;
}

int transport_send(const char *payload, size_t len, int *coap_class, int *coap_detail)
{
	struct sockaddr_storage addr;
	socklen_t addr_len;
	struct coap_client_request req = {
		.method = COAP_METHOD_POST,
		.confirmable = true,
		.fmt = COAP_CONTENT_FORMAT_APP_JSON,
		.payload = (const uint8_t *)payload,
		.len = len,
		.cb = coap_response_cb,
	};
	int sock;
	int err;

	err = snprintk(req.path, sizeof(req.path), "api/v1/%s/telemetry",
				   CONFIG_APP_THINGSBOARD_ACCESS_TOKEN);
	if (err < 0 || (size_t)err >= sizeof(req.path) || strlen(CONFIG_APP_THINGSBOARD_ACCESS_TOKEN) == 0)
	{
		LOG_ERR("ThingsBoard access token missing or too long");
		return -EINVAL;
	}

	err = resolve_server(&addr, &addr_len);
	if (err)
	{
		return err;
	}

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0)
	{
		err = -errno;
		LOG_ERR("socket() failed: %d", err);
		return err;
	}

	if (connect(sock, (struct sockaddr *)&addr, addr_len) < 0)
	{
		err = -errno;
		LOG_ERR("connect() failed: %d", err);
		goto out;
	}

	LOG_INF("CoAP POST (%u bytes) to api/v1/<token>/telemetry: %s", (unsigned int)len, payload);
	k_sem_reset(&coap_response_sem);
	coap_response_code = -ETIMEDOUT;
	err = coap_client_req(&coap_client_ctx, sock, NULL, &req, &coap_tx_params);
	if (err < 0)
	{
		LOG_ERR("CoAP request failed: %d", err);
		goto out;
	}

	if (k_sem_take(&coap_response_sem, K_MSEC(coap_response_timeout_ms())) != 0)
	{
		LOG_ERR("No CoAP response");
		err = -ETIMEDOUT;
		goto out;
	}

	if (coap_response_code < 0)
	{
		err = coap_response_code;
		goto out;
	}

	*coap_class = (uint16_t)coap_response_code >> 5;
	*coap_detail = (uint16_t)coap_response_code & 0x1f;
	err = *coap_class == 2 ? 0 : -EBADMSG;

out:
	coap_client_cancel_requests(&coap_client_ctx);
	(void)close(sock);
	return err;
}
