/**
 * @file
 * @brief The web front end: REST requests and the WebSocket live stream.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>

#include "web/api.hpp"
#include "system/glue.h"

namespace s0 {

/**
 * @brief Serves the dashboard, the REST API and the WebSocket stream.
 *
 * Zephyr's HTTP server owns the listening socket and calls into this class
 * through the resources declared in glue.c. The class collects request
 * bodies, hands complete requests to the RestApi, and pushes status and
 * readings to every connected WebSocket client from its own thread.
 */
class WebServer {
public:
	/** @brief Why the server could not be started. */
	enum class StartError : std::uint8_t {
		HttpServer, /**< Zephyr's HTTP server refused to start */
	};

	/** @brief The one web server of this device. */
	[[nodiscard]] static WebServer &instance() noexcept;

	WebServer(const WebServer &) = delete;
	WebServer &operator=(const WebServer &) = delete;

	/** @brief Start listening and start the WebSocket broadcaster. Call once at boot. */
	std::expected<void, StartError> start() noexcept;

	/**
	 * @brief Process one callback of Zephyr's HTTP server for a REST resource.
	 *
	 * A request arrives as one or more callbacks carrying parts of the
	 * body; the response is produced with the final one.
	 *
	 * @param client   The connection the callback belongs to.
	 * @param status   Which stage of the transaction this callback reports.
	 * @param request  The part of the request body delivered now.
	 * @param response Filled in once the request is complete.
	 * @param endpoint The REST resource that was addressed.
	 * @return 0, as the HTTP server expects from a handler that coped.
	 */
	int handle_request(const http_client_ctx &client, http_transaction_status status,
			   const http_request_ctx &request, http_response_ctx &response,
			   s0_endpoint endpoint) noexcept;

	/**
	 * @brief Answer a request for a path that no resource serves.
	 *
	 * Clients of the setup access point are redirected to the setup page:
	 * that is what turns the access point into a captive portal, because
	 * a phone's connectivity check asks for exactly such a path. Everybody
	 * else gets "not found".
	 *
	 * @param client   The connection the callback belongs to.
	 * @param status   Which stage of the transaction this callback reports.
	 * @param response Filled in once the request is complete.
	 * @return 0, as the HTTP server expects from a handler that coped.
	 */
	int handle_unknown(const http_client_ctx &client, http_transaction_status status,
			   http_response_ctx &response) noexcept;

	/**
	 * @brief Take over a connection that was upgraded to a WebSocket.
	 *
	 * @param socket The WebSocket handle created by the HTTP server.
	 * @return Nothing if the client was accepted; an errno value if every
	 *         client slot is taken, in which case the server closes it.
	 */
	std::expected<void, int> accept_websocket(int socket) noexcept;

	/** @brief The REST implementation, for callers other than HTTP. */
	[[nodiscard]] RestApi &api() noexcept { return api_; }

private:
	static constexpr std::size_t response_size = 12 * 1024;
	static constexpr std::size_t request_size = 4 * 1024;
	static constexpr std::size_t max_websocket_clients = 4;
	static_assert(max_websocket_clients <= CONFIG_WEBSOCKET_MAX_CONTEXTS,
		      "Zephyr refuses upgrades beyond CONFIG_WEBSOCKET_MAX_CONTEXTS");

	struct WebSocketClient {
		int socket{-1};
		bool greeted{}; /**< has received the initial status and reading */

		[[nodiscard]] bool connected() const noexcept { return socket >= 0; }
	};

	WebServer() noexcept;

	void release_request() noexcept;
	void reply(http_response_ctx &response, int status, std::size_t length) noexcept;
	std::size_t error_body(std::string_view message) noexcept;

	[[noreturn]] void broadcast_loop() noexcept;
	void drop(WebSocketClient &client) noexcept;
	void send(WebSocketClient &client, std::string_view text) noexcept;
	void service(WebSocketClient &client) noexcept;
	std::string_view status_message() noexcept;
	std::string_view reading_message() noexcept;

	RestApi api_{};

	/*
	 * Zephyr's HTTP server handles all clients from one thread, so one
	 * request and one response buffer are enough. `request_owner_` catches
	 * the rare case of two uploads interleaving.
	 */
	std::array<char, request_size> request_{};
	std::size_t request_length_{};
	bool request_too_large_{};
	const http_client_ctx *request_owner_{};
	std::array<char, response_size> response_{};

	k_mutex websocket_mutex_{}; /**< guards websocket_clients_ */
	std::array<WebSocketClient, max_websocket_clients> websocket_clients_{};
	std::array<char, response_size> websocket_message_{};
	k_thread broadcast_thread_{};
};

} /* namespace s0 */
