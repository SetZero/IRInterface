/**
 * @file
 * @brief Central meter state shared between the threads.
 *
 * Holds the latest reading, a short power history and the most recent raw
 * bytes. Written by the link thread; read by the web server, the WebSocket
 * broadcaster and MQTT.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <zephyr/kernel.h>

#include "meter/meter.hpp"

namespace s0 {

class Hub {
public:
	/** @brief Number of points kept in the power history. */
	static constexpr std::size_t history_points = 360;
	/** @brief Time covered by one history point; 360 points make one hour. */
	static constexpr std::int64_t history_interval_ms = 10'000;
	/** @brief Number of raw bytes kept for inspection. */
	static constexpr std::size_t raw_capacity = 1024;

	/** @brief Result of raw_copy(). */
	struct RawSnapshot {
		std::size_t size;    /**< bytes written to the caller's buffer */
		std::uint32_t total; /**< bytes received since boot */
	};

	/** @brief The one hub of this device. */
	[[nodiscard]] static Hub &instance() noexcept;

	Hub(const Hub &) = delete;
	Hub &operator=(const Hub &) = delete;

	/**
	 * @brief Store a freshly decoded telegram and wake everybody waiting for one.
	 *
	 * The hub stamps its copy with the sequence number and the time.
	 *
	 * @param reading The telegram as produced by a decoder.
	 */
	void publish(const Reading &reading) noexcept;

	/**
	 * @brief The most recent telegram.
	 * @return The reading, or nothing if none was decoded since boot.
	 */
	[[nodiscard]] std::optional<Reading> latest() noexcept;

	/** @brief Number of telegrams published since boot. */
	[[nodiscard]] std::uint32_t generation() noexcept;

	/**
	 * @brief Wait for a telegram newer than one already seen.
	 *
	 * @param seen    The generation the caller has already handled.
	 * @param timeout How long to block at most.
	 * @return The current generation if it differs from @p seen, or
	 *         nothing if the timeout expired first.
	 */
	[[nodiscard]] std::optional<std::uint32_t> wait_new(std::uint32_t seen,
							    k_timeout_t timeout) noexcept;

	/**
	 * @brief Copy the power history, oldest first.
	 *
	 * Each point is the average over one interval, or nothing for an
	 * interval in which the meter reported no power.
	 *
	 * @param out Destination for up to history_points entries.
	 * @return The number of points written.
	 */
	std::size_t history(std::span<std::optional<units::Watts>> out) noexcept;

	/**
	 * @brief Record bytes exactly as received from the optical interface.
	 * @param data The bytes, in the order they arrived.
	 */
	void raw_append(std::span<const std::uint8_t> data) noexcept;

	/**
	 * @brief Copy the most recent raw bytes, oldest first.
	 * @param out Destination for up to raw_capacity bytes.
	 * @return How many bytes were written and how many arrived in total.
	 */
	RawSnapshot raw_copy(std::span<std::uint8_t> out) noexcept;

	/**
	 * @brief Advance the history.
	 *
	 * Has to be called regularly, also while no telegrams arrive, so that
	 * the time axis stays continuous.
	 *
	 * @param now_ms Current uptime in milliseconds.
	 */
	void tick(std::int64_t now_ms) noexcept;

private:
	Hub() noexcept;

	void history_push(std::optional<units::Watts> sample) noexcept;

	k_mutex mutex_{};
	k_condvar condvar_{};

	Reading latest_{};
	bool have_reading_{};
	std::uint32_t generation_{};

	std::array<std::optional<units::Watts>, history_points> history_{};
	std::size_t history_head_{};
	std::size_t history_count_{};
	std::int64_t history_next_ms_{history_interval_ms};
	units::Watts power_sum_{}; /**< sum of the readings in the current interval */
	unsigned power_samples_{};

	std::array<std::uint8_t, raw_capacity> raw_{};
	std::size_t raw_head_{};
	std::uint32_t raw_total_{};
};

} /* namespace s0 */
