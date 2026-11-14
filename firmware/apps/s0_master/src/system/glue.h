/**
 * @file
 * @brief Boundary between the C++ application and the parts of Zephyr that are
 * declared through C-only macros (HTTP resources, shell commands) or need
 * SoC headers (reset into the ROM bootloader).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef S0_GLUE_H_
#define S0_GLUE_H_

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/net/http/server.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This header is shared with C, where enumerators are not scoped: they keep
 * the prefixed upper-case form C code uses instead of the CamelCase of the
 * C++ sources.
 */
/* NOLINTBEGIN(readability-identifier-naming) */
enum s0_endpoint {
	S0_EP_STATUS,
	S0_EP_READING,
	S0_EP_HISTORY,
	S0_EP_RAW,
	S0_EP_CONFIG,
	S0_EP_WIFI,
	S0_EP_WIFI_SCAN,
	S0_EP_INJECT,
	S0_EP_REBOOT,
	S0_EP_BOOTLOADER,
	S0_EP_FACTORY_RESET,
	S0_EP_METER_PIN,
	S0_EP_METER_PULSES,
	S0_EP_METER_CANCEL,
};
/* NOLINTEND(readability-identifier-naming) */

/* --- implemented in C++ (web.cpp, api.cpp) ------------------------------ */

/* Handler behind every REST resource; user_data carries the s0_endpoint. */
int s0_http_handler(struct http_client_ctx *client, enum http_transaction_status status,
		    const struct http_request_ctx *request_ctx,
		    struct http_response_ctx *response_ctx, void *user_data);

/*
 * Handler for every path no resource matches: a redirect to the setup page
 * for clients of the setup access point, "not found" for everybody else.
 */
int s0_http_fallback(struct http_client_ctx *client, enum http_transaction_status status,
		     const struct http_request_ctx *request_ctx,
		     struct http_response_ctx *response_ctx, void *user_data);

/* Called when a client upgrades to a WebSocket on /ws. */
int s0_ws_setup(int ws_socket, struct http_request_ctx *request_ctx, void *user_data);

/* Write the status document as JSON text; returns its length. */
size_t s0_status_json(char *buffer, size_t capacity);

/* Store new Wi-Fi credentials; psk may be empty for an open network. */
int s0_set_wifi(const char *ssid, const char *psk);

int s0_factory_reset(void);

/* --- implemented in C (glue.c) ------------------------------------------ */

/*
 * Why the device last restarted: "power-on", "reset button", "brown-out",
 * "watchdog", "firmware crash" or "restart command". Never NULL.
 */
const char *s0_restart_reason(void);

/*
 * Restart after `delay_ms`. With `bootloader` set the chip comes up in the
 * ROM serial download mode, so it can be flashed without pressing BOOT.
 */
void s0_schedule_reboot(bool bootloader, int delay_ms);

#ifdef __cplusplus
}
#endif

#endif /* S0_GLUE_H_ */
