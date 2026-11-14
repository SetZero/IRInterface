/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "web/web.hpp"

#include <algorithm>
#include <cstring>

#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/websocket.h>

#include "network/captive_portal.hpp"
#include "system/config.hpp"
#include "core/lock.hpp"

LOG_MODULE_REGISTER(s0_web, LOG_LEVEL_INF);

namespace s0 {

namespace {

constexpr std::int64_t status_period_ms = 2000;
/* a client that cannot take a message for this long is dropped */
constexpr std::int32_t send_timeout_ms = 250;
constexpr k_timeout_t reading_wait = K_MSEC(500);

const http_header json_headers[] = {
	{"Content-Type", "application/json"},
	{"Cache-Control", "no-store"},
	{"Access-Control-Allow-Origin", "*"},
};

const http_header redirect_headers[] = {
	{"Location", CaptivePortal::setup_url},
	{"Cache-Control", "no-store"},
};

K_KERNEL_STACK_DEFINE(broadcast_stack, 8192);

} /* namespace */

WebServer::WebServer() noexcept
{
	k_mutex_init(&websocket_mutex_);
}

WebServer &WebServer::instance() noexcept
{
	static WebServer server;

	return server;
}

std::expected<void, WebServer::StartError> WebServer::start() noexcept
{
	k_thread_create(
		&broadcast_thread_, broadcast_stack, K_KERNEL_STACK_SIZEOF(broadcast_stack),
		[](void *self, void *, void *) {
			static_cast<WebServer *>(self)->broadcast_loop();
		},
		this, nullptr, nullptr, K_PRIO_PREEMPT(9), 0, K_NO_WAIT);
	k_thread_name_set(&broadcast_thread_, "s0_websocket");

	if (const int result = http_server_start(); result < 0) {
		LOG_ERR("cannot start the HTTP server (%d)", result);
		return std::unexpected{StartError::HttpServer};
	}
	return {};
}

/* --- REST ----------------------------------------------------------------- */

void WebServer::release_request() noexcept
{
	request_owner_ = nullptr;
	request_length_ = 0;
	request_too_large_ = false;
}

void WebServer::reply(http_response_ctx &response, int status, std::size_t length) noexcept
{
	response.status = static_cast<http_status>(status);
	response.headers = json_headers;
	response.header_count = std::size(json_headers);
	response.body = reinterpret_cast<const std::uint8_t *>(response_.data());
	response.body_len = length;
	response.final_chunk = true;
}

std::size_t WebServer::error_body(std::string_view message) noexcept
{
	json::Writer writer{response_};

	writer << json::begin_object << json::key("error") << message << json::end_object;
	return writer.size();
}

int WebServer::handle_request(const http_client_ctx &client, http_transaction_status status,
			      const http_request_ctx &request, http_response_ctx &response,
			      s0_endpoint endpoint) noexcept
{
	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		if (request_owner_ == &client) {
			release_request();
		}
		return 0;
	}

	if (request_owner_ != nullptr && request_owner_ != &client) {
		/* another request body is still being received */
		reply(response, HTTP_503_SERVICE_UNAVAILABLE, 0);
		return 0;
	}
	request_owner_ = &client;

	if (request.data_len > 0) {
		if (request_length_ + request.data_len > request_.size()) {
			request_too_large_ = true;
		} else {
			std::memcpy(request_.data() + request_length_, request.data,
				    request.data_len);
			request_length_ += request.data_len;
		}
	}

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0; /* more of the body follows */
	}

	if (request_too_large_) {
		reply(response, HTTP_413_PAYLOAD_TOO_LARGE, error_body("request body too large"));
	} else {
		const auto result =
			api_.call(endpoint, client.method == HTTP_POST,
				  std::string_view{request_.data(), request_length_}, response_);

		reply(response, result.http_status, result.length);
	}

	release_request();
	return 0;
}

int WebServer::handle_unknown(const http_client_ctx &client, http_transaction_status status,
			      http_response_ctx &response) noexcept
{
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0; /* nothing to say until the request is complete */
	}

	if (CaptivePortal::is_portal_client(client.fd)) {
		response.status = HTTP_302_FOUND;
		response.headers = redirect_headers;
		response.header_count = std::size(redirect_headers);
		response.body = nullptr;
		response.body_len = 0;
		response.final_chunk = true;
	} else {
		reply(response, HTTP_404_NOT_FOUND, error_body("no such resource"));
	}
	return 0;
}

/* --- WebSocket ------------------------------------------------------------ */

std::expected<void, int> WebServer::accept_websocket(int socket) noexcept
{
	const Lock lock{websocket_mutex_};
	const auto slot = std::ranges::find_if_not(websocket_clients_, &WebSocketClient::connected);

	if (slot == websocket_clients_.end()) {
		LOG_WRN("too many websocket clients");
		return std::unexpected{ENOMEM};
	}

	*slot = {.socket = socket, .greeted = false};
	LOG_INF("websocket client connected");
	return {};
}

void WebServer::drop(WebSocketClient &client) noexcept
{
	if (client.connected()) {
		websocket_unregister(client.socket);
		client = {};
	}
}

void WebServer::send(WebSocketClient &client, std::string_view text) noexcept
{
	const int result = websocket_send_msg(
		client.socket, reinterpret_cast<const std::uint8_t *>(text.data()), text.size(),
		WEBSOCKET_OPCODE_DATA_TEXT, false, true, send_timeout_ms);

	if (result < 0) {
		LOG_INF("websocket client gone (%d)", result);
		drop(client);
	}
}

/* Read and discard whatever the client sent; notice when it has left. */
void WebServer::service(WebSocketClient &client) noexcept
{
	zsock_pollfd poll_entry{.fd = client.socket, .events = ZSOCK_POLLIN, .revents = 0};

	if (zsock_poll(&poll_entry, 1, 0) <= 0) {
		return;
	}
	if (poll_entry.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
		drop(client);
		return;
	}
	if (poll_entry.revents & ZSOCK_POLLIN) {
		std::array<std::uint8_t, 128> discard;
		std::uint32_t message_type = 0;
		std::uint64_t remaining = 0;
		const int result = websocket_recv_msg(client.socket, discard.data(), discard.size(),
						      &message_type, &remaining, 0);

		if ((result < 0 && result != -EAGAIN) || (message_type & WEBSOCKET_FLAG_CLOSE)) {
			drop(client);
		}
	}
}

/* {"type":"status","data":{...}} */
std::string_view WebServer::status_message() noexcept
{
	json::Writer writer{websocket_message_};

	writer << json::begin_object << json::key("type") << "status" << json::key("data");
	api_.write_status(writer);
	writer << json::end_object;
	return writer.ok() ? writer.view() : std::string_view{};
}

/* {"type":"reading","data":{...}} */
std::string_view WebServer::reading_message() noexcept
{
	json::Writer writer{websocket_message_};

	writer << json::begin_object << json::key("type") << "reading" << json::key("data");
	api_.write_reading(writer, Hub::instance().latest());
	writer << json::end_object;
	return writer.ok() ? writer.view() : std::string_view{};
}

void WebServer::broadcast_loop() noexcept
{
	std::uint32_t seen = Hub::instance().generation();
	std::int64_t next_status_ms = 0;

	while (true) {
		const auto fresh = Hub::instance().wait_new(seen, reading_wait);

		if (fresh) {
			seen = *fresh;
		}

		const std::int64_t now = k_uptime_get();
		const bool status_due = now >= next_status_ms;

		/*
		 * Work on a copy of the client table. Sending waits out its
		 * timeout when a client has gone away without closing, as a
		 * phone does when it leaves the network, and the HTTP server
		 * thread must not queue up behind that when it brings a new
		 * client: it serves every other request as well.
		 */
		std::array<WebSocketClient, max_websocket_clients> clients;

		{
			const Lock lock{websocket_mutex_};

			clients = websocket_clients_;
		}

		const auto taken = clients; /* to recognise the same clients when writing back */

		for (auto &client : clients) {
			if (client.connected()) {
				service(client);
			}
		}

		/* a client that has just connected gets both documents at once */
		const bool newcomer = std::ranges::any_of(clients, [](const auto &client) {
			return client.connected() && !client.greeted;
		});

		if (status_due || newcomer) {
			const auto text = status_message();

			for (auto &client : clients) {
				if (client.connected() && (status_due || !client.greeted)) {
					send(client, text);
				}
			}
		}
		if (status_due) {
			next_status_ms = now + status_period_ms;
		}

		if (fresh || newcomer) {
			const auto text = reading_message();

			for (auto &client : clients) {
				if (client.connected() && (fresh || !client.greeted)) {
					send(client, text);
				}
			}
		}

		const Lock lock{websocket_mutex_};

		for (std::size_t index = 0; index < clients.size(); index++) {
			auto &slot = websocket_clients_[index];

			/* a slot that was free may have been given to a new client meanwhile */
			if (slot.socket == taken[index].socket) {
				slot = clients[index];
				slot.greeted = slot.connected();
			}
		}
	}
}

} /* namespace s0 */

/* --- entry points for the declarations in glue.c ---------------------------- */

extern "C" int s0_http_handler(struct http_client_ctx *client, enum http_transaction_status status,
			       const struct http_request_ctx *request_ctx,
			       struct http_response_ctx *response_ctx, void *user_data)
{
	const auto endpoint = static_cast<s0_endpoint>(POINTER_TO_INT(user_data));

	return s0::WebServer::instance().handle_request(*client, status, *request_ctx,
							*response_ctx, endpoint);
}

extern "C" int s0_http_fallback(struct http_client_ctx *client, enum http_transaction_status status,
				const struct http_request_ctx *,
				struct http_response_ctx *response_ctx, void *)
{
	return s0::WebServer::instance().handle_unknown(*client, status, *response_ctx);
}

extern "C" int s0_ws_setup(int ws_socket, struct http_request_ctx *, void *)
{
	const auto accepted = s0::WebServer::instance().accept_websocket(ws_socket);

	return accepted ? 0 : -accepted.error();
}

extern "C" size_t s0_status_json(char *buffer, size_t capacity)
{
	s0::json::Writer writer{std::span{buffer, capacity}};

	s0::WebServer::instance().api().write_status(writer);
	return writer.size();
}

extern "C" int s0_set_wifi(const char *ssid, const char *psk)
{
	auto &settings = s0::Settings::instance();
	s0::Config config = settings.get();

	config.wifi_ssid.assign(ssid);
	config.wifi_psk.assign(psk != nullptr ? psk : "");
	return settings.set(config) ? 0 : -EIO;
}

extern "C" int s0_factory_reset(void)
{
	return s0::Settings::instance().factory_reset() ? 0 : -EIO;
}
