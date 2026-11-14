/**
 * @file
 * @brief Persistent device configuration (Wi-Fi, MQTT, meter interface).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <expected>

#include <zephyr/kernel.h>

#include "core/util.hpp"

namespace s0 {

/** @brief How the meter is read. */
enum class MeterMode : std::uint8_t {
	Automatic, /**< try the passive settings in turn, then an active request */
	Passive,   /**< listen with fixed UART settings */
	Active,    /**< IEC 62056-21 request/response read-out */
};

/** @brief Everything the device remembers across restarts. */
struct Config {
	StaticString<32> wifi_ssid{};
	StaticString<64> wifi_psk{};

	bool mqtt_enabled{};
	StaticString<64> mqtt_host{};
	std::uint16_t mqtt_port{1883};
	StaticString<32> mqtt_user{};
	StaticString<64> mqtt_password{};
	StaticString<48> mqtt_topic{"s0master"};
	bool mqtt_discovery{true}; /**< publish Home Assistant discovery messages */

	MeterMode meter_mode{MeterMode::Automatic};
	std::uint32_t ir_baud{9600};
	bool ir_7e1{};                     /**< 7 data bits with even parity instead of 8N1 */
	std::uint16_t poll_interval_s{60}; /**< period of the active read-out */
};

/** @brief Why the configuration could not be read or written. */
enum class StorageError : std::uint8_t {
	Unavailable, /**< the settings partition could not be opened */
	ReadFailed,
	WriteFailed,
};

/**
 * @brief Owner of the configuration and its copy in flash.
 *
 * Threads take a snapshot with get() and watch generation() to notice
 * changes without polling the contents.
 */
class Settings {
public:
	/** @brief The one settings store of this device. */
	[[nodiscard]] static Settings &instance() noexcept;

	Settings(const Settings &) = delete;
	Settings &operator=(const Settings &) = delete;

	/**
	 * @brief Open the settings storage and read the stored configuration.
	 *
	 * On failure the defaults stay in effect.
	 */
	std::expected<void, StorageError> load() noexcept;

	/** @brief A snapshot of the current configuration. */
	[[nodiscard]] Config get() noexcept;

	/**
	 * @brief Replace the configuration and write it to flash.
	 *
	 * The new values take effect even if writing fails; they are then
	 * lost at the next restart.
	 *
	 * @param config The complete new configuration.
	 */
	std::expected<void, StorageError> set(const Config &config) noexcept;

	/** @brief Return to the defaults, in memory and in flash. */
	std::expected<void, StorageError> factory_reset() noexcept;

	/** @brief A counter that changes with every set(). */
	[[nodiscard]] std::uint32_t generation() const noexcept { return generation_.load(); }

private:
	Settings() noexcept;

	k_mutex mutex_{};
	Config current_{};
	std::atomic<std::uint32_t> generation_{};
};

} /* namespace s0 */
