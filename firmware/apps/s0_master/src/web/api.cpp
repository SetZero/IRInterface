/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "web/api.hpp"

#include <concepts>
#include <cstdio>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "system/config.hpp"
#include "link/link.hpp"
#include "network/mqtt.hpp"
#include "network/wifi.hpp"

LOG_MODULE_REGISTER(s0_api, LOG_LEVEL_INF);

namespace s0 {

namespace {

constexpr int http_ok = 200;
constexpr int http_bad_request = 400;
constexpr int http_internal_error = 500;
constexpr int reboot_delay_ms = 500; /* long enough for the response to leave */

using Outcome = std::expected<void, std::string_view>;

constexpr std::string_view format_name(bool is_7e1) noexcept
{
	return is_7e1 ? "7E1" : "8N1";
}

constexpr std::string_view format_name(irlink::IrFormat format) noexcept
{
	return format_name(format == irlink::IrFormat::F7E1);
}

constexpr std::string_view mode_name(MeterMode mode) noexcept
{
	switch (mode) {
	case MeterMode::Passive:
		return "passive";
	case MeterMode::Active:
		return "active";
	case MeterMode::Automatic:
		break;
	}
	return "auto";
}

/* "1-0:16.7.0*255" */
StaticString<24> obis_text(const Obis &code)
{
	char text[25];

	std::snprintf(text, sizeof(text), "%u-%u:%u.%u.%u*%u", code.medium, code.channel,
		      code.quantity, code.processing, code.tariff, code.period);
	return StaticString<24>{text};
}

void write_error(json::Writer &out, std::string_view message)
{
	out << json::begin_object << json::key("error") << message << json::end_object;
}

void write_ok(json::Writer &out)
{
	out << json::begin_object << json::key("ok") << true << json::end_object;
}

/*
 * Assign `target` from a member that may be absent. A member that is
 * present but unusable is reported with `message`.
 */
template <typename T>
Outcome assign(json::Member member, T &target, std::string_view message)
{
	const auto value = member.as<T>();

	if (value) {
		target = *value;
	} else if (value.error() != json::Error::Missing) {
		return std::unexpected{message};
	}
	return {};
}

/* Like assign(), for a number that must lie within [low, high]. */
template <std::integral T>
Outcome assign_in_range(json::Member member, T &target, std::int64_t low, std::int64_t high,
			std::string_view message)
{
	const auto value = member.as<std::int64_t>();

	if (value) {
		if (*value < low || *value > high) {
			return std::unexpected{message};
		}
		target = static_cast<T>(*value);
	} else if (value.error() != json::Error::Missing) {
		return std::unexpected{message};
	}
	return {};
}

template <typename T>
struct Choice {
	std::string_view name;
	T value;
};

/* Like assign(), for a member that selects one of a few named options. */
template <typename T, std::size_t Count>
Outcome assign_choice(json::Member member, T &target, const Choice<T> (&choices)[Count],
		      std::string_view message)
{
	const auto word = member.as<StaticString<16>>();

	if (!word) {
		return word.error() == json::Error::Missing ? Outcome{} : std::unexpected{message};
	}
	for (const auto &choice : choices) {
		if (word->view() == choice.name) {
			target = choice.value;
			return {};
		}
	}
	return std::unexpected{message};
}

constexpr Choice<MeterMode> meter_modes[] = {
	{"auto", MeterMode::Automatic},
	{"passive", MeterMode::Passive},
	{"active", MeterMode::Active},
};

constexpr Choice<bool> ir_formats[] = {
	{"8N1", false},
	{"7E1", true},
};

} /* namespace */

void RestApi::write_history(json::Writer &out)
{
	const std::size_t count = Hub::instance().history(history_);

	out.begin_object();
	out.member("interval_s", Hub::history_interval_ms / 1000);
	out.key("points").begin_array();
	for (const auto &point : std::span{history_}.first(count)) {
		if (point) {
			out << json::fixed(point->count(), 0);
		} else {
			out << json::null;
		}
	}
	out.end_array().end_object();
}

void RestApi::write_raw(json::Writer &out)
{
	const auto [size, total] = Hub::instance().raw_copy(raw_);

	for (std::size_t index = 0; index < size; index++) {
		raw_hex_[2 * index] = hex_digit(raw_[index] >> 4);
		raw_hex_[2 * index + 1] = hex_digit(raw_[index]);
	}

	out.begin_object();
	out.member("total", total);
	out.member("hex", std::string_view{raw_hex_.data(), 2 * size});
	out.end_object();
}

void RestApi::write_config(json::Writer &out) const
{
	const Config config = Settings::instance().get();

	out.begin_object();
	out.member("wifi_ssid", config.wifi_ssid);
	out.member("meter_mode", mode_name(config.meter_mode));
	out.member("ir_baud", config.ir_baud);
	out.member("ir_format", format_name(config.ir_7e1));
	out.member("poll_interval_s", config.poll_interval_s);
	out.member("mqtt_enabled", config.mqtt_enabled);
	out.member("mqtt_host", config.mqtt_host);
	out.member("mqtt_port", config.mqtt_port);
	out.member("mqtt_user", config.mqtt_user);
	/* secrets are write-only */
	out.member("mqtt_password_set", !config.mqtt_password.empty());
	out.member("mqtt_topic", config.mqtt_topic);
	out.member("mqtt_discovery", config.mqtt_discovery);
	out.end_object();
}

void RestApi::write_scan(json::Writer &out) const
{
	const WifiScan scan = Wifi::instance().scan();

	out.begin_object();
	out.member("scanning", scan.scanning);
	out.key("networks").begin_array();
	for (const auto &network : scan.networks) {
		out.begin_object();
		out.member("ssid", network.ssid);
		out.member("rssi", network.rssi);
		out.member("secure", network.secure);
		out.end_object();
	}
	out.end_array().end_object();
}

/* Apply the members present in `body` and store the result. */
RestApi::Outcome RestApi::update_config(std::string_view body) const
{
	const json::Object request{body};
	Config config = Settings::instance().get();

	const Outcome steps[] = {
		assign_choice(request["meter_mode"], config.meter_mode, meter_modes,
			      "meter_mode must be auto, passive or active"),
		assign_in_range(request["ir_baud"], config.ir_baud, irlink::min_ir_baud,
				irlink::max_ir_baud, "ir_baud must be a number from 300 to 38400"),
		assign_choice(request["ir_format"], config.ir_7e1, ir_formats,
			      "ir_format must be 8N1 or 7E1"),
		assign_in_range(request["poll_interval_s"], config.poll_interval_s, 10, 3600,
				"poll_interval_s must be a number from 10 to 3600"),
		assign(request["mqtt_enabled"], config.mqtt_enabled,
		       "mqtt_enabled must be true or false"),
		assign(request["mqtt_discovery"], config.mqtt_discovery,
		       "mqtt_discovery must be true or false"),
		assign_in_range(request["mqtt_port"], config.mqtt_port, 1, 65535,
				"mqtt_port must be a number from 1 to 65535"),
		assign(request["mqtt_host"], config.mqtt_host,
		       "mqtt_host must be text of at most 64 characters"),
		assign(request["mqtt_user"], config.mqtt_user,
		       "mqtt_user must be text of at most 32 characters"),
		assign(request["mqtt_password"], config.mqtt_password,
		       "mqtt_password must be text of at most 64 characters"),
		assign(request["mqtt_topic"], config.mqtt_topic,
		       "mqtt_topic must be text of at most 48 characters"),
	};

	for (const auto &step : steps) {
		if (!step) {
			return step;
		}
	}

	if (config.mqtt_enabled && config.mqtt_host.empty()) {
		return std::unexpected{"mqtt_host is required when MQTT is enabled"};
	}
	if (!Settings::instance().set(config)) {
		return std::unexpected{"cannot store settings"};
	}
	return {};
}

/* Store new Wi-Fi credentials; an empty ssid forgets the network. */
RestApi::Outcome RestApi::update_wifi(std::string_view body) const
{
	const json::Object request{body};
	Config config = Settings::instance().get();
	const auto ssid = request["ssid"].as<decltype(config.wifi_ssid)>();
	const auto psk = request["psk"].as<decltype(config.wifi_psk)>();

	if (!ssid) {
		return std::unexpected{"ssid is missing or longer than 32 characters"};
	}
	if (!psk && psk.error() != json::Error::Missing) {
		return std::unexpected{"psk must be text of at most 64 characters"};
	}

	config.wifi_ssid = *ssid;
	config.wifi_psk = psk.value_or(decltype(config.wifi_psk){});

	if (!config.wifi_psk.empty() && config.wifi_psk.size() < 8) {
		return std::unexpected{"a Wi-Fi password has at least 8 characters"};
	}
	if (!Settings::instance().set(config)) {
		return std::unexpected{"cannot store settings"};
	}
	return {};
}

/* Body is hex text; whitespace and separators between bytes are ignored. */
std::size_t RestApi::inject_hex(std::string_view body) const
{
	std::array<std::uint8_t, 64> chunk;
	std::size_t length = 0;
	std::size_t accepted = 0;
	std::optional<int> high_nibble;

	for (const char character : body) {
		const int nibble = hex_value(character);

		if (nibble < 0) {
			continue;
		}
		if (!high_nibble) {
			high_nibble = nibble;
			continue;
		}
		chunk[length++] = static_cast<std::uint8_t>((*high_nibble << 4) | nibble);
		high_nibble.reset();
		if (length == chunk.size()) {
			accepted += Link::instance().inject(chunk);
			length = 0;
		}
	}
	if (length > 0) {
		accepted += Link::instance().inject(std::span{chunk}.first(length));
	}
	return accepted;
}

namespace {

constexpr std::string_view pin_error_text(PinError error) noexcept
{
	switch (error) {
	case PinError::Empty:
	case PinError::TooLong:
		return "pin must have 1 to 8 digits";
	case PinError::NotDigits:
		return "pin may only contain the digits 0 to 9";
	case PinError::BadTiming:
		break;
	}
	return "pulse_ms and gap_ms must be numbers from 20 to 20000";
}

/* Hand a light signal to the read head. */
Outcome start_signal(const LightSequence &sequence)
{
	const auto started = Link::instance().signal(sequence);

	if (!started) {
		return std::unexpected{
			started.error() == SignalError::Busy
				? std::string_view{"a light signal is already running"}
				: std::string_view{"the read head is offline"}};
	}
	return {};
}

} /* namespace */

/* Flash a PIN at the meter. The PIN is used once and not stored. */
RestApi::Outcome RestApi::enter_pin(std::string_view body) const
{
	const json::Object request{body};
	const auto pin = request["pin"].as<StaticString<max_pin_digits>>();
	PinTiming timing;

	if (!pin) {
		return std::unexpected{pin_error_text(PinError::TooLong)};
	}

	const Outcome steps[] = {
		assign_in_range(request["wake_pulses"], timing.wake_pulses, 0,
				irlink::max_light_pulses,
				"wake_pulses must be a number from 0 to 20"),
		assign_in_range(request["pulse_ms"], timing.pulse_ms, irlink::min_light_ms,
				irlink::max_light_ms, pin_error_text(PinError::BadTiming)),
		assign_in_range(request["gap_ms"], timing.gap_ms, irlink::min_light_ms,
				irlink::max_light_ms, pin_error_text(PinError::BadTiming)),
		assign_in_range(request["wake_pause_ms"], timing.wake_pause_ms, 0, 60000,
				"wake_pause_ms must be a number from 0 to 60000"),
		assign_in_range(request["digit_pause_ms"], timing.digit_pause_ms, 500, 60000,
				"digit_pause_ms must be a number from 500 to 60000"),
	};

	for (const auto &step : steps) {
		if (!step) {
			return step;
		}
	}

	const auto sequence = pin_sequence(pin->view(), timing);

	if (!sequence) {
		return std::unexpected{pin_error_text(sequence.error())};
	}
	return start_signal(*sequence);
}

/* Send one burst of flashes: a short one steps through the display, a long one changes a setting.
 */
RestApi::Outcome RestApi::send_pulses(std::string_view body) const
{
	const json::Object request{body};
	irlink::LightPulses pulses{1, 400, 400};

	const Outcome steps[] = {
		assign_in_range(request["count"], pulses.count, 1, irlink::max_light_pulses,
				"count must be a number from 1 to 20"),
		assign_in_range(request["on_ms"], pulses.on_ms, irlink::min_light_ms,
				irlink::max_light_ms, "on_ms must be a number from 20 to 20000"),
		assign_in_range(request["off_ms"], pulses.off_ms, irlink::min_light_ms,
				irlink::max_light_ms, "off_ms must be a number from 20 to 20000"),
	};

	for (const auto &step : steps) {
		if (!step) {
			return step;
		}
	}

	LightSequence sequence;

	sequence.push_back({pulses, 0});
	return start_signal(sequence);
}

void RestApi::write_status(json::Writer &out) const
{
	const LinkInfo link = Link::instance().info();
	const WifiInfo wifi = Wifi::instance().info();
	const MqttInfo mqtt = Mqtt::instance().info();
	const Config config = Settings::instance().get();
	char head_version[12];

	std::snprintf(head_version, sizeof(head_version), "%u.%u", link.head.firmware_major,
		      link.head.firmware_minor);

	out.begin_object();

	out.key("device").begin_object();
	out.member("name", "S0 Master");
	out.member("id", Wifi::instance().device_id());
	out.member("firmware", firmware_version);
	out.member("uptime_s", k_uptime_get() / 1000);
	out.member("last_restart", s0_restart_reason());
	out.end_object();

	out.key("wifi").begin_object();
	out.member("station", station_state_name(wifi.station));
	out.member("ssid", wifi.ssid);
	out.member("ip", wifi.ip);
	out.member("rssi", wifi.rssi);
	out.member("access_point", wifi.access_point);
	out.member("ap_ssid", wifi.ap_ssid);
	out.member("ap_ip", wifi.ap_ip);
	out.member("mac", wifi.mac);
	out.end_object();

	out.key("head").begin_object();
	out.member("online", link.head_online);
	if (link.head_age_ms >= 0) {
		out.member("firmware", head_version);
		out.member("uptime_s", link.head.uptime_s);
		out.member("ir_baud", link.head.ir.baud);
		out.member("ir_format", format_name(link.head.ir.format));
		out.member("ir_rx_bytes", link.head.ir_rx_bytes);
		out.member("ir_rx_errors", link.head.ir_rx_errors);
		out.member("ir_rx_dropped", link.head.ir_rx_dropped);
		out.member("link_errors", link.head.link_errors);
	}
	out.member("frames_ok", link.frames_ok);
	out.member("frame_errors", link.frame_errors);
	out.member("data_gaps", link.data_gaps);
	out.end_object();

	out.key("meter").begin_object();
	out.member("mode", mode_name(config.meter_mode));
	out.member("state", detect_state_name(link.state));
	out.member("protocol", protocol_name(link.protocol));
	out.member("ir_baud", link.ir.baud);
	out.member("ir_format", format_name(link.ir.format));
	out.member("active", link.active);
	out.member("telegrams", link.telegrams);
	out.member("decode_errors", link.decode_errors);
	if (link.last_decode_error) {
		out.member("last_decode_error", sml_error_name(*link.last_decode_error));
	}
	out << json::key("telegram_age_ms");
	if (link.telegram_age_ms >= 0) {
		out << link.telegram_age_ms;
	} else {
		out << json::null;
	}
	out.key("signal").begin_object();
	out.member("state", signal_state_name(link.signal));
	out.member("remaining_ms", link.signal_remaining_ms);
	out.end_object();
	out.end_object();

	out.key("mqtt").begin_object();
	out.member("enabled", mqtt.enabled);
	out.member("connected", mqtt.connected);
	out.member("published", mqtt.published);
	out.end_object();

	out.end_object();
}

void RestApi::write_reading(json::Writer &out, const std::optional<Reading> &reading) const
{
	out.begin_object();

	if (!reading) {
		out.member("valid", false);
		out.end_object();
		return;
	}

	out.member("valid", true);
	out.member("sequence", reading->sequence);
	out.member("age_ms", k_uptime_get() - reading->uptime_ms);
	out.member("protocol", protocol_name(reading->protocol));
	out.member("meter_id", reading->meter_id);

	if (const auto power = reading->power()) {
		out.member("power_w", power->count(), 1);
	}
	if (const auto energy = reading->get<units::WattHours>(obis::energy_import)) {
		out.member("energy_import_wh", energy->count(), 1);
	}
	if (const auto energy = reading->get<units::WattHours>(obis::energy_export)) {
		out.member("energy_export_wh", energy->count(), 1);
	}

	out.key("values").begin_array();
	for (const auto &value : reading->values) {
		out.begin_object();
		out.member("obis", obis_text(value.obis));
		if (const auto *text = value.text()) {
			out.member("text", *text);
		} else {
			if (const auto *known = known_quantity(value.obis)) {
				out.member("key", known->key);
				out.member("label", known->label);
			}
			out.member("value", value.number(), 3);
			out.member("unit", value.unit());
		}
		out.end_object();
	}
	out.end_array();

	out.end_object();
}

RestApi::Response RestApi::call(s0_endpoint endpoint, bool post, std::string_view body,
				std::span<char> out)
{
	json::Writer writer{out};
	Outcome outcome;

	switch (endpoint) {
	case S0_EP_STATUS:
		write_status(writer);
		break;

	case S0_EP_READING:
		write_reading(writer, Hub::instance().latest());
		break;

	case S0_EP_HISTORY:
		write_history(writer);
		break;

	case S0_EP_RAW:
		write_raw(writer);
		break;

	case S0_EP_CONFIG:
		if (post) {
			outcome = update_config(body);
		}
		if (outcome) {
			write_config(writer);
		}
		break;

	case S0_EP_WIFI:
		outcome = update_wifi(body);
		if (outcome) {
			write_ok(writer);
		}
		break;

	case S0_EP_WIFI_SCAN:
		write_scan(writer);
		break;

	case S0_EP_INJECT:
		writer << json::begin_object << json::key("accepted") << inject_hex(body)
		       << json::end_object;
		break;

	case S0_EP_REBOOT:
		s0_schedule_reboot(false, reboot_delay_ms);
		write_ok(writer);
		break;

	case S0_EP_BOOTLOADER:
		s0_schedule_reboot(true, reboot_delay_ms);
		write_ok(writer);
		break;

	case S0_EP_METER_PIN:
		outcome = enter_pin(body);
		if (outcome) {
			write_ok(writer);
		}
		break;

	case S0_EP_METER_PULSES:
		outcome = send_pulses(body);
		if (outcome) {
			write_ok(writer);
		}
		break;

	case S0_EP_METER_CANCEL:
		Link::instance().cancel_signal();
		write_ok(writer);
		break;

	case S0_EP_FACTORY_RESET:
		if (Settings::instance().factory_reset()) {
			s0_schedule_reboot(false, reboot_delay_ms);
			write_ok(writer);
		} else {
			outcome = std::unexpected{"cannot erase settings"};
		}
		break;
	}

	if (!outcome) {
		write_error(writer, outcome.error());
		return {http_bad_request, writer.size()};
	}

	if (!writer.ok()) {
		LOG_ERR("response for endpoint %d does not fit", static_cast<int>(endpoint));

		json::Writer fallback{out};

		write_error(fallback, "response too large");
		return {http_internal_error, fallback.size()};
	}

	return {http_ok, writer.size()};
}

} /* namespace s0 */
