/**
 * @file
 * @brief REST API: the JSON documents and the operations behind each endpoint.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "system/glue.h"
#include "meter/hub.hpp"
#include "core/json.hpp"
#include "meter/meter.hpp"

namespace s0 {

/** @brief Version reported by the API and to Home Assistant. */
inline constexpr std::string_view firmware_version = "1.0.0";

/**
 * @brief Implements the REST endpoints.
 *
 * Knows nothing about HTTP beyond status codes: it turns an endpoint, a
 * method and a request body into a JSON document. The same documents are
 * pushed over the WebSocket.
 */
class RestApi {
public:
	/** @brief What call() produced. */
	struct Response {
		int http_status{200};
		std::size_t length{}; /**< bytes of JSON written to the output buffer */
	};

	/**
	 * @brief Run one API call.
	 *
	 * @param endpoint Which resource was requested.
	 * @param post     True for POST, false for GET.
	 * @param body     The request body; empty for GET.
	 * @param out      Buffer that receives the JSON response.
	 * @return The HTTP status and the length of the response.
	 */
	Response call(s0_endpoint endpoint, bool post, std::string_view body, std::span<char> out);

	/**
	 * @brief Write the device status document.
	 * @param out The writer to append one JSON object to.
	 */
	void write_status(json::Writer &out) const;

	/**
	 * @brief Write a reading document.
	 * @param out     The writer to append one JSON object to.
	 * @param reading The telegram, or nothing if none was decoded yet.
	 */
	void write_reading(json::Writer &out, const std::optional<Reading> &reading) const;

private:
	/** Nothing on success, otherwise what is wrong with the request. */
	using Outcome = std::expected<void, std::string_view>;

	void write_history(json::Writer &out);
	void write_raw(json::Writer &out);
	void write_config(json::Writer &out) const;
	void write_scan(json::Writer &out) const;

	Outcome update_config(std::string_view body) const;
	Outcome update_wifi(std::string_view body) const;
	Outcome enter_pin(std::string_view body) const;
	Outcome send_pulses(std::string_view body) const;
	std::size_t inject_hex(std::string_view body) const;

	/* scratch space for the two large documents; too big for a thread stack */
	std::array<std::optional<units::Watts>, Hub::history_points> history_{};
	std::array<std::uint8_t, Hub::raw_capacity> raw_{};
	std::array<char, 2 * Hub::raw_capacity> raw_hex_{};
};

} /* namespace s0 */
