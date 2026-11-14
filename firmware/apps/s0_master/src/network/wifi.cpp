/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "network/wifi.hpp"

#include <algorithm>
#include <atomic>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include "network/captive_portal.hpp"
#include "system/config.hpp"
#include "core/lock.hpp"

LOG_MODULE_REGISTER(s0_wifi, LOG_LEVEL_INF);

namespace s0 {

namespace {

constexpr std::int64_t retry_delay_ms = 10'000;
constexpr std::int64_t connect_timeout_ms = 30'000;
constexpr unsigned failures_before_access_point = 2;
/* long enough for the setup page, still open on the access point, to show the new address */
constexpr std::int64_t access_point_linger_ms = 30'000;
constexpr std::int64_t scan_max_age_ms = 15'000;

enum class Event : std::uint8_t {
	Connected,
	ConnectFailed,
	Disconnected,
	GotIp,
	ApEnabled,
	ApDisabled,
};

K_MSGQ_DEFINE(events, sizeof(Event), 16, 1);

K_KERNEL_STACK_DEFINE(wifi_stack, 6144);

class WifiTask {
public:
	void start()
	{
		k_mutex_init(&mutex_);

		k_thread_create(
			&thread_, wifi_stack, K_KERNEL_STACK_SIZEOF(wifi_stack),
			[](void *self, void *, void *) { static_cast<WifiTask *>(self)->run(); },
			this, nullptr, nullptr, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
		k_thread_name_set(&thread_, "s0_wifi");
	}

	WifiInfo info()
	{
		WifiInfo out;

		{
			const Lock lock{mutex_};

			out = info_;
		}

		if (out.station == StationState::Connected && station_ != nullptr) {
			wifi_iface_status status{};

			if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, station_, &status,
				     sizeof(status)) == 0) {
				out.rssi = status.rssi;
			}
		}
		return out;
	}

	bool online() { return online_.load(); }

	WifiScan scan()
	{
		const Lock lock{mutex_};
		const std::int64_t now = k_uptime_get();

		if (!scanning_ && station_ != nullptr &&
		    (scan_ms_ < 0 || now - scan_ms_ > scan_max_age_ms)) {
			pending_.clear();
			scanning_ = net_mgmt(NET_REQUEST_WIFI_SCAN, station_, nullptr, 0) == 0;
			scan_ms_ = now;
		}

		return {scanning_, networks_};
	}

	StaticString<6> device_id()
	{
		StaticString<6> id;
		const auto *iface = net_if_get_wifi_sta();
		const auto *link = iface != nullptr
					   ? net_if_get_link_addr(const_cast<net_if *>(iface))
					   : nullptr;

		if (link != nullptr && link->len >= 6) {
			for (int index = 3; index < 6; index++) {
				id.push_back(hex_digit(link->addr[index] >> 4));
				id.push_back(hex_digit(link->addr[index]));
			}
		} else {
			id.assign("000000");
		}
		return id;
	}

private:
	static WifiTask &instance();

	static void post(Event event) { k_msgq_put(&events, &event, K_NO_WAIT); }

	static void on_wifi_event(net_mgmt_event_callback *callback, std::uint64_t event, net_if *)
	{
		auto &self = instance();

		switch (event) {
		case NET_EVENT_WIFI_CONNECT_RESULT: {
			const auto *status = static_cast<const wifi_status *>(callback->info);

			post(status != nullptr && status->status == 0 ? Event::Connected
								      : Event::ConnectFailed);
			break;
		}
		case NET_EVENT_WIFI_DISCONNECT_RESULT:
			post(Event::Disconnected);
			break;
		case NET_EVENT_WIFI_AP_ENABLE_RESULT:
			post(Event::ApEnabled);
			break;
		case NET_EVENT_WIFI_AP_DISABLE_RESULT:
			post(Event::ApDisabled);
			break;
		case NET_EVENT_WIFI_SCAN_RESULT:
			self.on_scan_result(static_cast<const wifi_scan_result *>(callback->info));
			break;
		case NET_EVENT_WIFI_SCAN_DONE:
			self.on_scan_done();
			break;
		default:
			break;
		}
	}

	static void on_ip_event(net_mgmt_event_callback *, std::uint64_t event, net_if *)
	{
		if (event == NET_EVENT_IPV4_DHCP_BOUND) {
			post(Event::GotIp);
		}
	}

	void on_scan_result(const wifi_scan_result *result)
	{
		if (result == nullptr || result->ssid_length == 0) {
			return;
		}

		const std::string_view ssid{reinterpret_cast<const char *>(result->ssid),
					    result->ssid_length};
		const Lock lock{mutex_};

		for (auto &known : pending_) {
			if (known.ssid == ssid) {
				known.rssi = std::max(known.rssi, result->rssi);
				return;
			}
		}

		WifiNetwork network;

		network.ssid.assign(ssid);
		network.rssi = result->rssi;
		network.secure = result->security != WIFI_SECURITY_TYPE_NONE;
		pending_.push_back(network);
	}

	void on_scan_done()
	{
		const Lock lock{mutex_};

		std::ranges::sort(pending_, std::ranges::greater{}, &WifiNetwork::rssi);
		networks_ = pending_;
		scanning_ = false;
		scan_ms_ = k_uptime_get();
	}

	[[noreturn]] void run()
	{
		station_ = net_if_get_wifi_sta();
		access_point_ = net_if_get_wifi_sap();

		net_mgmt_init_event_callback(
			&wifi_events_, on_wifi_event,
			NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |
				NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT |
				NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE);
		net_mgmt_add_event_callback(&wifi_events_);
		net_mgmt_init_event_callback(&address_events_, on_ip_event,
					     NET_EVENT_IPV4_DHCP_BOUND);
		net_mgmt_add_event_callback(&address_events_);

		{
			const Lock lock{mutex_};
			const auto id = device_id();

			info_.ap_ssid.assign("S0-Master-");
			for (const char character : id.view().substr(2)) {
				info_.ap_ssid.push_back(character);
			}
			info_.ap_ip.assign(access_point_address);

			if (station_ != nullptr) {
				const auto *link = net_if_get_link_addr(station_);

				for (int index = 0; link != nullptr && index < link->len; index++) {
					if (index > 0) {
						info_.mac.push_back(':');
					}
					info_.mac.push_back(hex_digit(link->addr[index] >> 4));
					info_.mac.push_back(hex_digit(link->addr[index]));
				}
			}
		}

		/* let the Wi-Fi driver finish starting up */
		k_sleep(K_SECONDS(1));
		apply_config(k_uptime_get());

		while (true) {
			Event event;
			const bool have_event = k_msgq_get(&events, &event, K_SECONDS(1)) == 0;
			const std::int64_t now = k_uptime_get();

			if (have_event) {
				handle(event, now);
			}
			if (config_generation_ != Settings::instance().generation()) {
				apply_config(now);
			}
			periodic(now);
		}
	}

	void apply_config(std::int64_t now)
	{
		const Config config = Settings::instance().get();

		config_generation_ = Settings::instance().generation();

		if (config.wifi_ssid == ssid_.view() && config.wifi_psk == psk_.view() &&
		    started_) {
			return; /* nothing Wi-Fi related changed */
		}

		started_ = true;
		ssid_ = config.wifi_ssid;
		psk_ = config.wifi_psk;
		failures_ = 0;

		{
			const Lock lock{mutex_};

			info_.station = idle_state();
			info_.ssid = ssid_;
		}

		if (joined_ || connecting_) {
			net_mgmt(NET_REQUEST_WIFI_DISCONNECT, station_, nullptr, 0);
			set_joined(false);
			connecting_ = false;
		}

		if (ssid_.empty()) {
			LOG_INF("no network configured");
			start_access_point();
		} else {
			connect(now);
		}
	}

	void connect(std::int64_t now)
	{
		if (station_ == nullptr || ssid_.empty()) {
			return;
		}

		wifi_connect_req_params params{};

		params.ssid = reinterpret_cast<const std::uint8_t *>(ssid_.c_str());
		params.ssid_length = static_cast<std::uint8_t>(ssid_.size());
		params.psk = reinterpret_cast<const std::uint8_t *>(psk_.c_str());
		params.psk_length = static_cast<std::uint8_t>(psk_.size());
		params.security = psk_.empty() ? WIFI_SECURITY_TYPE_NONE : WIFI_SECURITY_TYPE_PSK;
		params.channel = WIFI_CHANNEL_ANY;
		params.band = WIFI_FREQ_BAND_2_4_GHZ;
		params.mfp = WIFI_MFP_OPTIONAL;

		LOG_INF("connecting to \"%s\"", ssid_.c_str());

		const int result =
			net_mgmt(NET_REQUEST_WIFI_CONNECT, station_, &params, sizeof(params));

		connecting_ = result == 0;
		connect_ms_ = now;
		if (result != 0) {
			LOG_WRN("connect request failed (%d)", result);
			failed(now);
		}
	}

	void failed(std::int64_t now)
	{
		connecting_ = false;
		set_joined(false);
		retry_ms_ = now + retry_delay_ms;
		failures_++;

		if (failures_ >= failures_before_access_point && !access_point_wanted_) {
			LOG_WRN("network unreachable, opening the setup access point");
			start_access_point();
		}
	}

	void handle(Event event, std::int64_t now)
	{
		switch (event) {
		case Event::Connected:
			LOG_INF("joined \"%s\"", ssid_.c_str());
			connecting_ = false;
			joined_ = true;
			failures_ = 0;
			disable_power_save();
			break;

		case Event::ConnectFailed:
			LOG_WRN("could not join \"%s\"", ssid_.c_str());
			failed(now);
			break;

		case Event::Disconnected:
			if (joined_ || connecting_) {
				LOG_WRN("disconnected");
				failed(now);
			}
			break;

		case Event::GotIp: {
			char text[NET_IPV4_ADDR_LEN] = "";

			net_addr_ntop(NET_AF_INET, &station_->config.dhcpv4.requested_ip, text,
				      sizeof(text));
			LOG_INF("address %s", text);
			{
				const Lock lock{mutex_};

				info_.station = StationState::Connected;
				info_.ip.assign(text);
			}
			online_.store(true);
			if (access_point_wanted_) {
				access_point_stop_ms_ = now + access_point_linger_ms;
			}
			break;
		}

		case Event::ApEnabled: {
			const Lock lock{mutex_};

			info_.access_point = true;
			break;
		}

		case Event::ApDisabled: {
			const Lock lock{mutex_};

			info_.access_point = false;
			break;
		}
		}
	}

	void periodic(std::int64_t now)
	{
		if (connecting_ && now - connect_ms_ > connect_timeout_ms) {
			LOG_WRN("connection attempt timed out");
			net_mgmt(NET_REQUEST_WIFI_DISCONNECT, station_, nullptr, 0);
			failed(now);
		}

		if (!ssid_.empty() && !joined_ && !connecting_ && now >= retry_ms_) {
			connect(now);
		}

		if (access_point_wanted_ && access_point_stop_ms_ > 0 &&
		    now >= access_point_stop_ms_) {
			stop_access_point();
		}
	}

	/*
	 * The radio's modem sleep is on by default. It saves power this
	 * device does not need to save, and it made the device answer some
	 * requests, address lookups among them, only after more than a
	 * second, long enough for a phone to report it unreachable.
	 */
	void disable_power_save()
	{
		wifi_ps_params params{};

		params.enabled = WIFI_PS_DISABLED;
		params.type = WIFI_PS_PARAM_STATE;

		if (const int result =
			    net_mgmt(NET_REQUEST_WIFI_PS, station_, &params, sizeof(params));
		    result != 0) {
			LOG_WRN("cannot switch Wi-Fi power saving off (%d)", result);
		}
	}

	/* The station state while it holds no address. */
	StationState idle_state() const
	{
		return ssid_.empty() ? StationState::Unconfigured : StationState::Connecting;
	}

	void set_joined(bool joined)
	{
		joined_ = joined;
		if (!joined) {
			online_.store(false);

			const Lock lock{mutex_};

			info_.station = idle_state();
			info_.ip.clear();
		}
	}

	void start_access_point()
	{
		if (access_point_ == nullptr) {
			LOG_ERR("no access point interface");
			return;
		}

		access_point_stop_ms_ = 0;
		if (access_point_wanted_) {
			return;
		}

		static net_in_addr address;
		static net_in_addr netmask;

		net_addr_pton(NET_AF_INET, access_point_address, &address);
		net_addr_pton(NET_AF_INET, access_point_netmask, &netmask);

		if (!access_point_addressed_) {
			net_if_ipv4_set_gw(access_point_, &address);
			net_if_ipv4_addr_add(access_point_, &address, NET_ADDR_MANUAL, 0);
			net_if_ipv4_set_netmask_by_addr(access_point_, &address, &netmask);
			access_point_addressed_ = true;
		}

		net_in_addr pool = address;

		pool.s4_addr[3] += 10;
		if (const int result = net_dhcpv4_server_start(access_point_, &pool);
		    result != 0 && result != -EALREADY) {
			LOG_WRN("DHCP server not started (%d)", result);
		}

		StaticString<32> name;

		{
			const Lock lock{mutex_};

			name = info_.ap_ssid;
		}

		wifi_connect_req_params params{};

		params.ssid = reinterpret_cast<const std::uint8_t *>(name.c_str());
		params.ssid_length = static_cast<std::uint8_t>(name.size());
		params.security = WIFI_SECURITY_TYPE_NONE;
		params.channel = WIFI_CHANNEL_ANY;
		params.band = WIFI_FREQ_BAND_2_4_GHZ;

		const int result = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, access_point_, &params,
					    sizeof(params));

		if (result != 0) {
			LOG_ERR("cannot start the access point (%d)", result);
			return;
		}
		access_point_wanted_ = true;
		LOG_INF("setup access point \"%s\" at %s", name.c_str(), access_point_address);
	}

	void stop_access_point()
	{
		LOG_INF("closing the setup access point");
		net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, access_point_, nullptr, 0);
		net_dhcpv4_server_stop(access_point_);
		access_point_wanted_ = false;
		access_point_stop_ms_ = 0;
	}

	net_if *station_{};
	net_if *access_point_{};
	net_mgmt_event_callback wifi_events_{};
	net_mgmt_event_callback address_events_{};

	k_mutex mutex_{}; /* guards info_ and the scan lists */
	WifiInfo info_{};
	std::atomic<bool> online_{};

	decltype(WifiScan::networks) networks_{};
	decltype(WifiScan::networks) pending_{};
	bool scanning_{};
	std::int64_t scan_ms_{-1};

	/* state owned by the Wi-Fi thread */
	std::uint32_t config_generation_{};
	bool started_{};
	StaticString<32> ssid_{};
	StaticString<64> psk_{};
	bool connecting_{};
	bool joined_{};
	unsigned failures_{};
	std::int64_t connect_ms_{};
	std::int64_t retry_ms_{};
	bool access_point_wanted_{};
	bool access_point_addressed_{};
	std::int64_t access_point_stop_ms_{};

	k_thread thread_{};
};

constinit WifiTask task;

WifiTask &WifiTask::instance()
{
	return task;
}

} /* namespace */

Wifi &Wifi::instance() noexcept
{
	static constinit Wifi wifi;

	return wifi;
}

void Wifi::start() noexcept
{
	task.start();
}

WifiInfo Wifi::info() noexcept
{
	return task.info();
}

bool Wifi::online() noexcept
{
	return task.online();
}

WifiScan Wifi::scan() noexcept
{
	return task.scan();
}

StaticString<6> Wifi::device_id() noexcept
{
	return task.device_id();
}

} /* namespace s0 */
