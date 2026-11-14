/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "network/captive_portal.hpp"

#include <array>
#include <cstring>

#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>

LOG_MODULE_REGISTER(s0_portal, LOG_LEVEL_INF);

namespace s0 {

namespace {

constexpr std::uint16_t dns_port = 53;
constexpr k_timeout_t bind_retry = K_SECONDS(2);

/* The DHCP server must hand out the address this name server answers on. */
static_assert(std::string_view{CONFIG_NET_DHCPV4_SERVER_OPTION_DNS_ADDRESS} == access_point_address,
	      "CONFIG_NET_DHCPV4_SERVER_OPTION_DNS_ADDRESS must be the access point's address");

K_KERNEL_STACK_DEFINE(portal_stack, 3072);

net_in_addr access_point_in_addr() noexcept
{
	net_in_addr address{};

	std::memcpy(&address, access_point_ip.data(), access_point_ip.size());
	return address;
}

/*
 * A UDP socket on the access point's address. Fails until the Wi-Fi task
 * has given the interface that address.
 */
int open_socket() noexcept
{
	const int socket = zsock_socket(NET_AF_INET, NET_SOCK_DGRAM, NET_IPPROTO_UDP);

	if (socket < 0) {
		return socket;
	}

	net_sockaddr_in local{};

	local.sin_family = NET_AF_INET;
	local.sin_port = net_htons(dns_port);
	local.sin_addr = access_point_in_addr();

	if (zsock_bind(socket, reinterpret_cast<const net_sockaddr *>(&local), sizeof(local)) < 0) {
		zsock_close(socket);
		return -1;
	}
	return socket;
}

} /* namespace */

CaptivePortal &CaptivePortal::instance() noexcept
{
	static constinit CaptivePortal portal;

	return portal;
}

void CaptivePortal::start() noexcept
{
	k_thread_create(
		&thread_, portal_stack, K_KERNEL_STACK_SIZEOF(portal_stack),
		[](void *self, void *, void *) { static_cast<CaptivePortal *>(self)->run(); }, this,
		nullptr, nullptr, K_PRIO_PREEMPT(11), 0, K_NO_WAIT);
	k_thread_name_set(&thread_, "s0_portal");
}

bool CaptivePortal::is_portal_client(int socket) noexcept
{
	net_sockaddr_in local{};
	net_socklen_t length = sizeof(local);

	if (zsock_getsockname(socket, reinterpret_cast<net_sockaddr *>(&local), &length) < 0 ||
	    local.sin_family != NET_AF_INET) {
		return false;
	}
	return std::memcmp(&local.sin_addr, access_point_ip.data(), access_point_ip.size()) == 0;
}

void CaptivePortal::run() noexcept
{
	while (true) {
		const int socket = open_socket();

		if (socket < 0) {
			k_sleep(bind_retry);
			continue;
		}

		LOG_INF("name server on %s", access_point_address);
		serve(socket);
		zsock_close(socket);
	}
}

/* Answer queries until the socket fails. */
void CaptivePortal::serve(int socket) noexcept
{
	std::array<std::uint8_t, dns::max_message_size> query;
	std::array<std::uint8_t, dns::max_message_size> reply;

	while (true) {
		net_sockaddr_storage client{};
		net_socklen_t client_length = sizeof(client);
		const auto received =
			zsock_recvfrom(socket, query.data(), query.size(), 0,
				       reinterpret_cast<net_sockaddr *>(&client), &client_length);

		if (received < 0) {
			LOG_WRN("name server socket failed (%d)", errno);
			return;
		}

		const auto reply_length =
			dns::build_reply(std::span{query}.first(static_cast<std::size_t>(received)),
					 access_point_ip, reply);

		if (!reply_length) {
			continue; /* not a query we answer; stay silent */
		}

		(void)zsock_sendto(socket, reply.data(), *reply_length, 0,
				   reinterpret_cast<const net_sockaddr *>(&client), client_length);
		answered_++;
	}
}

} /* namespace s0 */
