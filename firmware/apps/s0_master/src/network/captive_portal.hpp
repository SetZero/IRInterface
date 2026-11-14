/**
 * @file
 * @brief Captive portal on the setup access point.
 *
 * A phone that joins the access point checks whether it has internet
 * access by fetching a well-known URL. The portal makes that check land on
 * this device and answers it with a redirect, so the phone offers to open
 * the setup page by itself. It consists of three parts:
 *
 *  - the DHCP server names this device as DNS server (prj.conf),
 *  - a name server here resolves every name to this device,
 *  - the web server redirects every unknown URL to the setup page, for
 *    clients that came in through the access point.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string_view>

#include <zephyr/kernel.h>

#include "network/dns_redirect.hpp"

namespace s0 {

/** @brief Address of this device on its own access point. */
inline constexpr dns::Ipv4Address access_point_ip{192, 168, 4, 1};
/** @brief access_point_ip as text. */
inline constexpr const char *access_point_address = "192.168.4.1";
/** @brief Network mask of the access point's subnet. */
inline constexpr const char *access_point_netmask = "255.255.255.0";

class CaptivePortal {
public:
	/** @brief Where clients of the access point are sent. */
	static constexpr const char *setup_url = "http://192.168.4.1/";

	/** @brief The one captive portal of this device. */
	[[nodiscard]] static CaptivePortal &instance() noexcept;

	CaptivePortal(const CaptivePortal &) = delete;
	CaptivePortal &operator=(const CaptivePortal &) = delete;

	/**
	 * @brief Start the name server. Call once at boot.
	 *
	 * It answers on the access point's address only, and so stays silent
	 * towards the home network.
	 */
	void start() noexcept;

	/**
	 * @brief Whether a connection came in through the setup access point.
	 * @param socket The connection's socket.
	 * @return true if the socket's local address is the access point's.
	 */
	[[nodiscard]] static bool is_portal_client(int socket) noexcept;

	/** @brief Number of DNS queries answered since boot. */
	[[nodiscard]] std::uint32_t queries_answered() const noexcept { return answered_; }

private:
	constexpr CaptivePortal() noexcept = default;

	[[noreturn]] void run() noexcept;
	void serve(int socket) noexcept;

	k_thread thread_{};
	std::uint32_t answered_{};
};

} /* namespace s0 */
