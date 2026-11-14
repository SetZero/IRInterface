/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "network/mqtt.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>

#include "web/api.hpp"
#include "system/config.hpp"
#include "meter/hub.hpp"
#include "core/json.hpp"
#include "network/wifi.hpp"

LOG_MODULE_REGISTER(s0_mqtt, LOG_LEVEL_INF);

namespace s0 {

namespace {

constexpr int connack_timeout_ms = 5000;
constexpr int reconnect_delay_s = 10;

mqtt_utf8 utf8(std::string_view text)
{
	return {reinterpret_cast<const std::uint8_t *>(text.data()),
		static_cast<std::uint32_t>(text.size())};
}

struct SensorClass {
	std::string_view unit;
	std::string_view device_class;
	std::string_view state_class;
};

/* how Home Assistant should treat each unit */
constexpr SensorClass sensor_classes[] = {
	{units::Watts::symbol, "power", "measurement"},
	{units::WattHours::symbol, "energy", "total_increasing"},
	{units::Volts::symbol, "voltage", "measurement"},
	{units::Amperes::symbol, "current", "measurement"},
	{units::Hertz::symbol, "frequency", "measurement"},
	{units::Vars::symbol, "reactive_power", "measurement"},
	{units::VoltAmperes::symbol, "apparent_power", "measurement"},
};

K_KERNEL_STACK_DEFINE(mqtt_stack, 6144);

class MqttTask {
public:
	void start()
	{
		k_thread_create(
			&thread_, mqtt_stack, K_KERNEL_STACK_SIZEOF(mqtt_stack),
			[](void *self, void *, void *) { static_cast<MqttTask *>(self)->run(); },
			this, nullptr, nullptr, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
		k_thread_name_set(&thread_, "s0_mqtt");
	}

	MqttInfo info() const { return {enabled_.load(), connected_.load(), published_.load()}; }

private:
	static void on_event(mqtt_client *client, const mqtt_evt *event)
	{
		auto &self = *static_cast<MqttTask *>(client->user_data);

		switch (event->type) {
		case MQTT_EVT_CONNACK:
			if (event->result == 0) {
				self.connected_.store(true);
			} else {
				LOG_WRN("broker refused the connection (%d)", event->result);
			}
			break;
		case MQTT_EVT_DISCONNECT:
			self.connected_.store(false);
			break;
		default:
			break;
		}
	}

	[[noreturn]] void run()
	{
		while (true) {
			config_ = Settings::instance().get();
			config_generation_ = Settings::instance().generation();

			const bool enabled = config_.mqtt_enabled && !config_.mqtt_host.empty();

			enabled_.store(enabled);
			if (!enabled || !Wifi::instance().online()) {
				k_sleep(K_SECONDS(1));
				continue;
			}

			if (connect()) {
				LOG_INF("connected to %s:%u", config_.mqtt_host.c_str(),
					config_.mqtt_port);
				session();
				LOG_INF("disconnected");
			}

			if (connected_.exchange(false)) {
				mqtt_disconnect(&client_, nullptr);
			} else {
				mqtt_abort(&client_);
			}

			/* wait before reconnecting, but react to new settings at once */
			for (int second = 0; second < reconnect_delay_s && !config_changed();
			     second++) {
				k_sleep(K_SECONDS(1));
			}
		}
	}

	bool config_changed() const
	{
		return config_generation_ != Settings::instance().generation();
	}

	bool resolve()
	{
		char port[8];
		zsock_addrinfo hints{};
		zsock_addrinfo *addresses = nullptr;

		std::snprintf(port, sizeof(port), "%u", config_.mqtt_port);
		hints.ai_family = NET_AF_INET;
		hints.ai_socktype = NET_SOCK_STREAM;

		const int result =
			zsock_getaddrinfo(config_.mqtt_host.c_str(), port, &hints, &addresses);

		if (result != 0 || addresses == nullptr) {
			LOG_WRN("cannot resolve %s (%d)", config_.mqtt_host.c_str(), result);
			return false;
		}

		std::memcpy(&broker_, addresses->ai_addr,
			    std::min<std::size_t>(addresses->ai_addrlen, sizeof(broker_)));
		zsock_freeaddrinfo(addresses);
		return true;
	}

	bool connect()
	{
		if (!resolve()) {
			return false;
		}

		const auto id = Wifi::instance().device_id();

		client_id_.assign("s0master-");
		for (const char character : id.view()) {
			client_id_.push_back(character);
		}
		node_id_.assign("s0master_");
		for (const char character : id.view()) {
			node_id_.push_back(character);
		}

		availability_topic_ = topic("availability");
		will_topic_ = {utf8(availability_topic_), MQTT_QOS_0_AT_MOST_ONCE};
		will_message_ = utf8("offline");
		user_ = utf8(config_.mqtt_user);
		password_ = utf8(config_.mqtt_password);

		mqtt_client_init(&client_);
		client_.broker = &broker_;
		client_.evt_cb = on_event;
		client_.user_data = this;
		client_.client_id = utf8(client_id_);
		client_.user_name = config_.mqtt_user.empty() ? nullptr : &user_;
		client_.password = config_.mqtt_password.empty() ? nullptr : &password_;
		client_.protocol_version = MQTT_VERSION_3_1_1;
		client_.transport.type = MQTT_TRANSPORT_NON_SECURE;
		client_.rx_buf = rx_buf_.data();
		client_.rx_buf_size = rx_buf_.size();
		client_.tx_buf = tx_buf_.data();
		client_.tx_buf_size = tx_buf_.size();
		client_.will_topic = &will_topic_;
		client_.will_message = &will_message_;
		client_.will_retain = 1;

		connected_.store(false);

		const int result = mqtt_connect(&client_);

		if (result != 0) {
			LOG_WRN("cannot connect to %s:%u (%d)", config_.mqtt_host.c_str(),
				config_.mqtt_port, result);
			return false;
		}

		/* the session is usable once the broker acknowledged it */
		const std::int64_t deadline = k_uptime_get() + connack_timeout_ms;

		while (!connected_.load() && k_uptime_get() < deadline) {
			if (!pump(200)) {
				return false;
			}
		}
		if (!connected_.load()) {
			LOG_WRN("no answer from the broker");
		}
		return connected_.load();
	}

	/* Process incoming packets for up to `timeout_ms`; false on a dead link. */
	bool pump(int timeout_ms)
	{
		zsock_pollfd poll_entry{
			.fd = client_.transport.tcp.sock, .events = ZSOCK_POLLIN, .revents = 0};
		const int ready = zsock_poll(&poll_entry, 1, timeout_ms);

		if (ready < 0 ||
		    (poll_entry.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL))) {
			return false;
		}
		if (ready > 0 && (poll_entry.revents & ZSOCK_POLLIN)) {
			const int result = mqtt_input(&client_);

			if (result != 0 && result != -EAGAIN) {
				return false;
			}
		}

		const int result = mqtt_live(&client_);

		return result == 0 || result == -EAGAIN;
	}

	void session()
	{
		std::uint32_t seen = 0;
		bool discovery_sent = false;

		if (!publish(availability_topic_, "online", true)) {
			return;
		}

		while (connected_.load() && Wifi::instance().online() && !config_changed()) {
			const auto fresh = Hub::instance().wait_new(seen, K_MSEC(500));

			if (!pump(0)) {
				return;
			}
			if (!fresh) {
				continue;
			}
			seen = *fresh;

			const auto latest = Hub::instance().latest();

			if (!latest) {
				continue;
			}
			reading_ = *latest;

			if (config_.mqtt_discovery && !discovery_sent) {
				if (!publish_discovery()) {
					return;
				}
				discovery_sent = true;
			}
			if (!publish_state()) {
				return;
			}
			published_.fetch_add(1);
		}
	}

	StaticString<96> topic(std::string_view leaf) const
	{
		StaticString<96> out{config_.mqtt_topic};

		out.push_back('/');
		for (const char character : leaf) {
			out.push_back(character);
		}
		return out;
	}

	bool publish(std::string_view topic_name, std::string_view payload, bool retain)
	{
		mqtt_publish_param param{};

		param.message.topic.topic = utf8(topic_name);
		param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
		param.message.payload.data = const_cast<std::uint8_t *>(
			reinterpret_cast<const std::uint8_t *>(payload.data()));
		param.message.payload.len = static_cast<std::uint32_t>(payload.size());
		param.retain_flag = retain ? 1 : 0;

		const int result = mqtt_publish(&client_, &param);

		if (result != 0) {
			LOG_WRN("publish to %.*s failed (%d)", static_cast<int>(topic_name.size()),
				topic_name.data(), result);
		}
		return result == 0;
	}

	/* One flat JSON object with every known quantity of the telegram. */
	bool publish_state()
	{
		json::Writer out{payload_};
		bool have_power = false;

		out.begin_object();
		for (const auto &value : reading_.values) {
			const auto *known = known_quantity(value.obis);

			if (known == nullptr || value.is_text()) {
				continue;
			}
			have_power = have_power || known->key == "power";
			out.member(known->key, value.number(), 3);
		}
		if (const auto power = reading_.power(); power && !have_power) {
			out.member("power", power->count(), 3);
		}
		out.member("meter_id", reading_.meter_id);
		out.member("protocol", protocol_name(reading_.protocol));
		out.end_object();

		return out.ok() && publish(topic("state"), out.view(), false);
	}

	bool publish_sensor(std::string_view key, std::string_view label, std::string_view unit)
	{
		const SensorClass *sensor = nullptr;

		for (const auto &candidate : sensor_classes) {
			if (candidate.unit == unit) {
				sensor = &candidate;
			}
		}
		if (sensor == nullptr) {
			return true; /* nothing Home Assistant has a class for */
		}

		char unique[64];
		char value_template[64];
		char config_topic[128];

		std::snprintf(unique, sizeof(unique), "%s_%.*s", node_id_.c_str(),
			      static_cast<int>(key.size()), key.data());
		std::snprintf(value_template, sizeof(value_template), "{{ value_json.%.*s }}",
			      static_cast<int>(key.size()), key.data());
		std::snprintf(config_topic, sizeof(config_topic),
			      "homeassistant/sensor/%s/%.*s/config", node_id_.c_str(),
			      static_cast<int>(key.size()), key.data());

		json::Writer out{payload_};

		out.begin_object();
		out.member("name", label);
		out.member("unique_id", unique);
		out.member("state_topic", topic("state"));
		out.member("value_template", value_template);
		out.member("unit_of_measurement", unit);
		out.member("device_class", sensor->device_class);
		out.member("state_class", sensor->state_class);
		out.member("availability_topic", availability_topic_);
		out.key("device").begin_object();
		out << json::key("identifiers") << json::begin_array << node_id_ << json::end_array;
		out.member("name", "S0 Master");
		out.member("manufacturer", "SetZero");
		out.member("model", "S0 Master 0.1");
		out.member("sw_version", firmware_version);
		out.end_object();
		out.end_object();

		return out.ok() && publish(config_topic, out.view(), true);
	}

	/* Announce one sensor per quantity the meter actually reports. */
	bool publish_discovery()
	{
		bool have_power = false;

		for (const auto &value : reading_.values) {
			const auto *known = known_quantity(value.obis);

			if (known == nullptr || value.is_text()) {
				continue;
			}
			have_power = have_power || known->key == "power";
			if (!publish_sensor(known->key, known->label, value.unit())) {
				return false;
			}
		}
		if (reading_.power() && !have_power) {
			return publish_sensor("power", "Power", units::Watts::symbol);
		}
		return true;
	}

	Config config_{};
	std::uint32_t config_generation_{};

	mqtt_client client_{};
	net_sockaddr_storage broker_{};
	StaticString<24> client_id_{};
	StaticString<24> node_id_{};
	StaticString<96> availability_topic_{};
	mqtt_topic will_topic_{};
	mqtt_utf8 will_message_{};
	mqtt_utf8 user_{};
	mqtt_utf8 password_{};
	std::array<std::uint8_t, 512> rx_buf_{};
	std::array<std::uint8_t, 1024> tx_buf_{};
	std::array<char, 1024> payload_{};
	Reading reading_{};

	std::atomic<bool> enabled_{};
	std::atomic<bool> connected_{};
	std::atomic<std::uint32_t> published_{};

	k_thread thread_{};
};

constinit MqttTask task;

} /* namespace */

Mqtt &Mqtt::instance() noexcept
{
	static constinit Mqtt mqtt;

	return mqtt;
}

void Mqtt::start() noexcept
{
	task.start();
}

MqttInfo Mqtt::info() const noexcept
{
	return task.info();
}

} /* namespace s0 */
