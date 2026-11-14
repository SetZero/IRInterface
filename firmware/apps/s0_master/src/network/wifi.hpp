/**
 * @file
 * @brief Wi-Fi management.
 *
 * Joins the configured network and falls back to a setup access point when
 * there is none or it cannot be reached.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/util.hpp"

namespace s0 {

/** @brief State of the connection to the configured network. */
enum class StationState : std::uint8_t {
	Unconfigured, /**< no network name is stored */
	Connecting,   /**< a network is stored but not joined, or has no address yet */
	Connected,    /**< joined and holding an IPv4 address */
};

/** @brief The name of a station state as used in the API. */
[[nodiscard]] constexpr std::string_view station_state_name(StationState state) noexcept
{
	switch (state) {
	case StationState::Unconfigured:
		return "unconfigured";
	case StationState::Connecting:
		return "connecting";
	case StationState::Connected:
		return "connected";
	}
	return "";
}

/** @brief Snapshot of both Wi-Fi interfaces. */
struct WifiInfo {
	StationState station{StationState::Unconfigured};
	/**
	 * The setup access point is up. Independent of the station state: it
	 * stays up for a while after the station has connected.
	 */
	bool access_point{};
	StaticString<32> ssid{};
	StaticString<16> ip{};
	int rssi{};
	StaticString<32> ap_ssid{};
	StaticString<16> ap_ip{};
	StaticString<17> mac{};
};

/** @brief One network found by a scan. */
struct WifiNetwork {
	StaticString<32> ssid{};
	std::int8_t rssi{};
	bool secure{};
};

/** @brief Result of a network scan. */
struct WifiScan {
	bool scanning{}; /**< a scan is running; the list is from the previous one */
	StaticVector<WifiNetwork, 16> networks{}; /**< strongest first */
};

/** @brief The Wi-Fi radio with its station and access point interfaces. */
class Wifi {
public:
	/** @brief The one Wi-Fi manager of this device. */
	[[nodiscard]] static Wifi &instance() noexcept;

	Wifi(const Wifi &) = delete;
	Wifi &operator=(const Wifi &) = delete;

	/** @brief Start the Wi-Fi thread. Call once at boot. */
	void start() noexcept;

	/** @brief The current state of both interfaces. */
	[[nodiscard]] WifiInfo info() noexcept;

	/** @brief True once the station interface has an address. */
	[[nodiscard]] bool online() noexcept;

	/**
	 * @brief The networks in range.
	 *
	 * Returns the list of the last scan and starts a new one in the
	 * background if that list is stale.
	 */
	[[nodiscard]] WifiScan scan() noexcept;

	/**
	 * @brief A short identifier unique to this device.
	 * @return The last three bytes of the MAC address as hex, e.g. "4393e0".
	 */
	[[nodiscard]] StaticString<6> device_id() noexcept;

private:
	constexpr Wifi() noexcept = default;
};

} /* namespace s0 */
