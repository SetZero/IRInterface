/**
 * @file
 * @brief Host tests for the parts of the firmware that do not need hardware:
 *        the SML and IEC 62056-21 decoders and the RS-485 link framing.
 *
 * Built and run by tests/run.sh with the host compiler. The SML telegrams
 * are assembled here from the encoding rules rather than copied from a
 * meter, so the tests cover the decoder's handling of the format, not the
 * quirks of a particular meter.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

#include <irlink/crc16.hpp>
#include <irlink/protocol.hpp>

#include "link/light_signal.hpp"
#include "meter/iec62056.hpp"
#include "meter/sml.hpp"
#include "network/dns_redirect.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

int checks = 0;
int failures = 0;

void check(bool condition, std::string_view expectation,
	   std::source_location where = std::source_location::current())
{
	checks++;
	if (!condition) {
		failures++;
		std::printf("FAILED %s:%u: %.*s\n", where.file_name(),
			    static_cast<unsigned>(where.line()),
			    static_cast<int>(expectation.size()), expectation.data());
	}
}

bool close_to(double actual, double expected)
{
	return std::fabs(actual - expected) < 1e-6 * std::fmax(1.0, std::fabs(expected));
}

void append(Bytes &target, std::span<const std::uint8_t> bytes)
{
	target.insert(target.end(), bytes.begin(), bytes.end());
}

/* --- building SML ---------------------------------------------------------- */

namespace sml {

constexpr std::uint8_t octet_string_type = 0x00;
constexpr std::uint8_t integer_type = 0x50;
constexpr std::uint8_t unsigned_type = 0x60;
constexpr std::uint8_t list_type = 0x70;

constexpr std::uint16_t open_response = 0x0101;
constexpr std::uint16_t close_response = 0x0201;
constexpr std::uint16_t get_list_response = 0x0701;

constexpr std::uint8_t unit_watt = 27;
constexpr std::uint8_t unit_watt_hour = 30;
constexpr std::uint8_t unit_volt = 35;

/* The type-length field; lengths above 15 take a second byte. */
Bytes type_length(std::uint8_t type, std::size_t length)
{
	if (length <= 0x0f) {
		return {static_cast<std::uint8_t>(type | length)};
	}
	return {static_cast<std::uint8_t>(0x80 | type | (length >> 4)),
		static_cast<std::uint8_t>(length & 0x0f)};
}

/* An element that is left out. */
Bytes absent()
{
	return {0x01};
}

Bytes octets(std::span<const std::uint8_t> data)
{
	/* the length counts the type-length bytes themselves */
	const std::size_t header = data.size() + 1 <= 0x0f ? 1 : 2;
	Bytes element = type_length(octet_string_type, data.size() + header);

	append(element, data);
	return element;
}

Bytes number(std::uint8_t type, std::uint64_t value, std::size_t width)
{
	Bytes element = type_length(type, width + 1);

	for (std::size_t index = 0; index < width; index++) {
		element.push_back(static_cast<std::uint8_t>(value >> (8 * (width - 1 - index))));
	}
	return element;
}

Bytes unsigned_number(std::uint64_t value, std::size_t width)
{
	return number(unsigned_type, value, width);
}

Bytes signed_number(std::int64_t value, std::size_t width)
{
	return number(integer_type, static_cast<std::uint64_t>(value), width);
}

Bytes list(std::initializer_list<Bytes> elements)
{
	Bytes element = type_length(list_type, elements.size());

	for (const auto &member : elements) {
		append(element, member);
	}
	return element;
}

Bytes list(const std::vector<Bytes> &elements)
{
	Bytes element = type_length(list_type, elements.size());

	for (const auto &member : elements) {
		append(element, member);
	}
	return element;
}

/* One register: OBIS code, unit, decimal scaler and the value element. */
Bytes entry(std::initializer_list<std::uint8_t> obis, std::uint8_t unit, int scaler,
	    const Bytes &value)
{
	return list({
		octets(Bytes{obis}),
		absent(), /* status */
		absent(), /* valTime */
		unit != 0 ? unsigned_number(unit, 1) : absent(),
		scaler != 0 ? signed_number(scaler, 1) : absent(),
		value,
		absent(), /* signature */
	});
}

Bytes message(std::uint16_t tag, const Bytes &body)
{
	return list({
		octets(Bytes{0x00, 0x8f, 0x63, 0xa9}), /* transaction id */
		unsigned_number(0, 1),                 /* group */
		unsigned_number(0, 1),                 /* abort on error */
		list({unsigned_number(tag, 2), body}),
		unsigned_number(0xfa6f, 2), /* message CRC, not checked by the decoder */
		Bytes{0x00},                /* end of message, the list's sixth element */
	});
}

/* The three messages a meter sends: open, value list, close. */
Bytes file_body(const Bytes &server_id, const std::vector<Bytes> &entries)
{
	Bytes body;

	append(body, message(open_response,
			     list({absent(), absent(), octets(Bytes{0x00, 0x2f, 0xcb, 0xe3}),
				   octets(server_id), absent(), absent()})));
	append(body,
	       message(get_list_response, list({absent(), octets(server_id), absent(), absent(),
						list(entries), absent(), absent()})));
	append(body, message(close_response, list({absent()})));
	return body;
}

/* Wrap a file body in the transport protocol: escapes, fill bytes, CRC. */
Bytes transport(const Bytes &body, bool swap_crc = false)
{
	Bytes frame{0x1b, 0x1b, 0x1b, 0x1b, 0x01, 0x01, 0x01, 0x01};
	unsigned escape_run = 0;

	for (const auto byte : body) {
		frame.push_back(byte);
		escape_run = byte == 0x1b ? escape_run + 1 : 0;
		if (escape_run == 4) {
			/* a literal escape sequence in the data is sent twice */
			frame.insert(frame.end(), {0x1b, 0x1b, 0x1b, 0x1b});
			escape_run = 0;
		}
	}

	const auto fill = static_cast<std::uint8_t>((4 - frame.size() % 4) % 4);

	frame.insert(frame.end(), fill, 0x00);
	frame.insert(frame.end(), {0x1b, 0x1b, 0x1b, 0x1b, 0x1a, fill});

	const std::uint16_t crc = irlink::crc16_x25(frame);
	const auto low = static_cast<std::uint8_t>(crc);
	const auto high = static_cast<std::uint8_t>(crc >> 8);

	frame.push_back(swap_crc ? high : low);
	frame.push_back(swap_crc ? low : high);
	return frame;
}

} /* namespace sml */

const Bytes server_id{0x09, 0x01, 0x45, 0x4d, 0x48, 0x00, 0x00, 0x7a, 0xc6, 0x42};

std::vector<Bytes> typical_entries(std::int64_t power)
{
	return {
		sml::entry({0x81, 0x81, 0xc7, 0x82, 0x03, 0xff}, 0, 0,
			   sml::octets(Bytes{'E', 'M', 'H'})),
		sml::entry({0x01, 0x00, 0x01, 0x08, 0x00, 0xff}, sml::unit_watt_hour, -1,
			   sml::unsigned_number(12345678, 5)),
		sml::entry({0x01, 0x00, 0x02, 0x08, 0x00, 0xff}, sml::unit_watt_hour, -1,
			   sml::unsigned_number(420, 5)),
		sml::entry({0x01, 0x00, 0x10, 0x07, 0x00, 0xff}, sml::unit_watt, 0,
			   sml::signed_number(power, 4)),
		sml::entry({0x01, 0x00, 0x20, 0x07, 0x00, 0xff}, sml::unit_volt, -1,
			   sml::unsigned_number(2304, 2)),
	};
}

/* Feed bytes to a decoder and count the telegrams it reports. */
template <typename Decoder>
int feed(Decoder &decoder, std::span<const std::uint8_t> bytes)
{
	int telegrams = 0;

	for (const auto byte : bytes) {
		telegrams += decoder.push(byte) ? 1 : 0;
	}
	return telegrams;
}

/* --- SML ------------------------------------------------------------------- */

void test_sml_values()
{
	s0::SmlDecoder decoder;
	const Bytes telegram = sml::transport(sml::file_body(server_id, typical_entries(-150)));

	check(feed(decoder, telegram) == 1, "one telegram is decoded");

	const s0::Reading &reading = decoder.reading();
	const auto energy = reading.get<s0::units::WattHours>(s0::obis::energy_import);
	const auto exported = reading.get<s0::units::WattHours>(s0::obis::energy_export);
	const auto voltage = reading.get<s0::units::Volts>(s0::obis::voltage_l1);
	const auto power = reading.power();

	check(reading.protocol == s0::Protocol::Sml, "protocol is SML");
	check(reading.meter_id.view() == "0901454d4800007ac642",
	      "binary server id is shown as hex");
	check(reading.values.size() == 5, "all five registers are present");
	check(energy && close_to(energy->count(), 1234567.8), "scaler -1 gives 1234567.8 Wh");
	check(exported && close_to(exported->count(), 42.0), "export register");
	check(voltage && close_to(voltage->count(), 230.4), "voltage");
	check(power && close_to(power->count(), -150.0), "negative power keeps its sign");
	check(energy && s0::units::KilowattHours{*energy} > s0::units::KilowattHours{1234.5},
	      "quantities compare across magnitudes");
	check(reading.values[0].is_text() && reading.values[0].text()->view() == "EMH",
	      "printable octet strings are text");
	check(!reading.get<s0::units::Watts>(s0::obis::energy_import),
	      "an energy register is not a power");
	check(decoder.errors() == 0, "no errors counted");
}

void test_sml_stream()
{
	s0::SmlDecoder decoder;
	Bytes stream{0x00, 0xff, 0x1b, 0x1b, 0x42}; /* noise, including a partial escape */

	append(stream, sml::transport(sml::file_body(server_id, typical_entries(100))));
	append(stream, sml::transport(sml::file_body(server_id, typical_entries(200))));

	check(feed(decoder, stream) == 2, "two telegrams back to back, after noise");
	check(decoder.reading().power() == s0::units::Watts{200}, "the last telegram wins");
}

void test_sml_fill_bytes()
{
	/* longer and longer server ids walk through every fill byte count */
	for (std::size_t extra = 0; extra < 4; extra++) {
		s0::SmlDecoder decoder;
		Bytes long_id = server_id;

		long_id.insert(long_id.end(), extra, 0x55);
		check(feed(decoder, sml::transport(sml::file_body(long_id, typical_entries(1)))) ==
			      1,
		      "telegram decodes with any number of fill bytes");
	}
}

void test_sml_swapped_crc()
{
	s0::SmlDecoder decoder;
	const Bytes telegram = sml::transport(sml::file_body(server_id, typical_entries(1)), true);

	check(feed(decoder, telegram) == 1, "a checksum sent high byte first is accepted");
}

void test_sml_corruption()
{
	s0::SmlDecoder decoder;
	Bytes telegram = sml::transport(sml::file_body(server_id, typical_entries(1)));

	telegram[40] ^= 0x04;
	check(feed(decoder, telegram) == 0, "a flipped bit is not decoded");
	check(decoder.errors() == 1, "and is counted");
	check(decoder.last_error() == s0::SmlError::Checksum, "as a checksum error");

	telegram[40] ^= 0x04;
	check(feed(decoder, telegram) == 1, "the decoder recovers with the next telegram");
}

void test_sml_truncated()
{
	s0::SmlDecoder decoder;
	const Bytes telegram = sml::transport(sml::file_body(server_id, typical_entries(1)));
	Bytes stream{telegram.begin(), telegram.begin() + 60};

	append(stream, telegram);
	check(feed(decoder, stream) == 1, "a cut-off telegram does not swallow the next one");
	check(decoder.last_error() == s0::SmlError::Truncated, "and is reported as truncated");
}

void test_sml_escape_in_data()
{
	s0::SmlDecoder decoder;
	std::vector<Bytes> entries = typical_entries(1);

	/* a register whose value happens to be the escape sequence */
	entries.push_back(sml::entry({0x01, 0x00, 0x60, 0x05, 0x05, 0xff}, 0, 0,
				     sml::unsigned_number(0x1b1b1b1b, 4)));

	check(feed(decoder, sml::transport(sml::file_body(server_id, entries))) == 1,
	      "an escape sequence inside the data is unescaped");
	check(close_to(decoder.reading().values[5].number(), static_cast<double>(0x1b1b1b1b)),
	      "and the value survives");
}

void test_sml_long_string()
{
	s0::SmlDecoder decoder;
	std::vector<Bytes> entries = typical_entries(77);
	const Bytes public_key(48, 0xa5);

	/* 48 bytes need the two-byte form of the type-length field */
	entries.insert(entries.begin(), sml::entry({0x81, 0x81, 0xc7, 0x82, 0x05, 0xff}, 0, 0,
						   sml::octets(public_key)));

	check(feed(decoder, sml::transport(sml::file_body(server_id, entries))) == 1,
	      "a long octet string is stepped over correctly");
	check(decoder.reading().power() == s0::units::Watts{77},
	      "the registers behind it are read");
}

void test_sml_garbage()
{
	const Bytes not_a_list{0x76, 0x05};
	const auto parsed = s0::SmlDecoder::parse(not_a_list);

	check(!parsed && parsed.error() == s0::SmlError::Truncated, "a cut-off body is truncated");
	check(s0::SmlDecoder::parse(Bytes{}).error() == s0::SmlError::NoValues,
	      "an empty body has no values");
	check(s0::SmlDecoder::parse(Bytes{0x76, 0x30}).error() == s0::SmlError::BadType,
	      "an undefined type is rejected");
}

/* --- IEC 62056-21 ---------------------------------------------------------- */

constexpr std::string_view iec_telegram = "/ISK5MT174-0001\r\n"
					  "\r\n"
					  "1-0:0.0.0*255(12345678)\r\n"
					  "1-0:1.8.0*255(001234.567*kWh)\r\n"
					  "1-0:2.8.0*255(000010.000*kWh)\r\n"
					  "1-0:16.7.0*255(-000123*W)\r\n"
					  "1-0:32.7.0*255(230.1*V)\r\n"
					  "1-0:31.7.0*255(1500*mA)\r\n"
					  "F.F(00000000)\r\n"
					  "!\r\n";

Bytes as_bytes(std::string_view text)
{
	return {text.begin(), text.end()};
}

/* Set the top bit the way an even-parity transmitter does. */
Bytes with_even_parity(std::string_view text)
{
	Bytes bytes;

	for (const char character : text) {
		const auto value = static_cast<std::uint8_t>(character);
		const bool odd = (std::popcount(value) & 1) != 0;

		bytes.push_back(static_cast<std::uint8_t>(value | (odd ? 0x80 : 0x00)));
	}
	return bytes;
}

void check_iec_reading(const s0::Reading &reading)
{
	const auto energy = reading.get<s0::units::WattHours>(s0::obis::energy_import);
	const auto voltage = reading.get<s0::units::Volts>(s0::obis::voltage_l1);
	const auto current = reading.get<s0::units::Amperes>(s0::obis::current_l1);

	check(reading.protocol == s0::Protocol::Iec62056, "protocol is IEC 62056-21");
	check(reading.meter_id.view() == "12345678", "the device address names the meter");
	check(energy && close_to(energy->count(), 1234567.0), "kWh are converted to Wh");
	check(reading.power() == s0::units::Watts{-123}, "signed power");
	check(voltage && close_to(voltage->count(), 230.1), "voltage");
	check(current && close_to(current->count(), 1.5), "mA are converted to A");
	check(reading.values.size() == 7, "seven data lines");
}

void test_iec_telegram()
{
	s0::IecDecoder decoder;

	check(feed(decoder, as_bytes(iec_telegram)) == 1, "one text telegram is decoded");
	check_iec_reading(decoder.reading());
	check(decoder.take_identification() == std::string_view{"/ISK5MT174-0001"},
	      "the identification line is available");
	check(!decoder.take_identification(), "but only once");
}

void test_iec_parity()
{
	s0::IecDecoder decoder;

	check(feed(decoder, with_even_parity(iec_telegram)) == 1,
	      "7E1 data read through an 8N1 receiver decodes");
	check_iec_reading(decoder.reading());
}

void test_iec_noise()
{
	s0::IecDecoder decoder;
	Bytes stream{0x1b, 0x1b, 0x01, 0x76, 0x05, '\n'};

	append(stream, as_bytes("1-0:1.8.0*255(000001.000*kWh)\r\n!\r\n")); /* no identification */
	check(feed(decoder, stream) == 0, "data lines without an identification are ignored");

	append(stream, as_bytes(iec_telegram));
	check(feed(decoder, as_bytes(iec_telegram)) == 1, "a complete telegram afterwards decodes");
}

void test_obis_text()
{
	using s0::IecDecoder;
	using s0::Obis;
	using s0::ObisError;

	check(IecDecoder::parse_obis("1-0:1.8.0*255") == Obis{1, 0, 1, 8, 0, 255}, "full form");
	check(IecDecoder::parse_obis("1.8.1") == Obis{1, 0, 1, 8, 1, 255}, "reduced form");
	check(IecDecoder::parse_obis("1.8.0*00") == Obis{1, 0, 1, 8, 0, 0}, "billing period");
	check(IecDecoder::parse_obis("C.1.0") == Obis{1, 0, 96, 1, 0, 255}, "letter code C");
	check(IecDecoder::parse_obis("F.F") == Obis{1, 0, 97, 97, 0, 255}, "error register");
	check(IecDecoder::parse_obis("abc").error() == ObisError::BadGroup, "not a number");
	check(IecDecoder::parse_obis("1").error() == ObisError::TooFewGroups, "one group");
	check(IecDecoder::parse_obis("1.2.3.4").error() == ObisError::TooManyGroups, "four groups");
	check(IecDecoder::parse_obis("x:1.8.0").error() == ObisError::BadPrefix, "bad prefix");
	check(IecDecoder::parse_obis("1.8.300").error() == ObisError::BadGroup, "group above 255");
}

/* --- link framing ---------------------------------------------------------- */

void test_link_frames()
{
	irlink::FrameDecoder decoder;
	int frames = 0;

	/* every payload length, filled with a pattern that includes zeros */
	for (std::size_t length = 0; length <= irlink::max_payload; length++) {
		Bytes payload(length);
		std::array<std::uint8_t, irlink::max_wire_frame> wire{};

		for (std::size_t index = 0; index < length; index++) {
			payload[index] = static_cast<std::uint8_t>(
				(index * 7 + length) % 5 == 0 ? 0 : index + length);
		}

		const auto wire_length = irlink::encode_frame(
			irlink::MsgType::IrData, static_cast<std::uint8_t>(length), payload, wire);

		check(wire_length.has_value(), "every payload up to the maximum can be framed");
		if (!wire_length) {
			continue;
		}

		for (const auto byte : std::span{wire}.first(*wire_length)) {
			const auto frame = decoder.push(byte);

			check(frame.has_value(), "no errors on a clean link");
			if (frame && frame->has_value()) {
				const Bytes received{(*frame)->payload.begin(),
						     (*frame)->payload.end()};

				frames++;
				check(received == payload && (*frame)->sequence == length,
				      "the frame arrives unchanged");
			}
		}
	}
	check(frames == static_cast<int>(irlink::max_payload) + 1, "all frames arrived");

	const Bytes too_long(irlink::max_payload + 1);
	std::array<std::uint8_t, irlink::max_wire_frame> wire{};

	check(irlink::encode_frame(irlink::MsgType::IrData, 0, too_long, wire).error() ==
		      irlink::FrameError::PayloadTooLarge,
	      "an oversized payload is refused");
}

void test_link_resync()
{
	irlink::FrameDecoder decoder;
	std::array<std::uint8_t, irlink::max_wire_frame> wire{};
	const irlink::Status status{.uptime_s = 1234, .ir = {300, irlink::IrFormat::F7E1}};
	const auto wire_length =
		irlink::encode_frame(irlink::MsgType::Status, 9, irlink::serialize(status), wire);
	int errors = 0;
	int frames = 0;

	check(wire_length.has_value(), "a status can be framed");
	if (!wire_length) {
		return;
	}

	/* half a frame, then the delimiter of a frame whose start was lost, then a good one */
	Bytes stream{wire.begin(), wire.begin() + 10};

	stream.push_back(irlink::delimiter);
	stream.insert(stream.end(), wire.begin(), wire.begin() + *wire_length);

	for (const auto byte : stream) {
		const auto frame = decoder.push(byte);

		if (!frame) {
			errors++;
		} else if (frame->has_value()) {
			const auto parsed = irlink::parse_status((*frame)->payload);

			frames++;
			check(parsed && parsed->uptime_s == 1234 && parsed->ir == status.ir,
			      "the status is read back");
		}
	}
	check(errors == 1 && frames == 1, "one damaged frame is dropped, the next one is good");
}

/* --- light signals ------------------------------------------------------------ */

void test_light_signals()
{
	constexpr irlink::LightPulses burst{3, 400, 250};
	std::array<std::uint8_t, irlink::max_wire_frame> wire{};
	irlink::FrameDecoder decoder;
	const auto length = irlink::encode_frame(irlink::MsgType::LightPulses, 5,
						 irlink::serialize(burst), wire);
	bool received = false;

	check(length.has_value(), "a burst can be framed");
	for (const auto byte : std::span{wire}.first(length.value_or(0))) {
		const auto frame = decoder.push(byte);

		if (frame && frame->has_value()) {
			received = (*frame)->type == irlink::MsgType::LightPulses &&
				   irlink::parse_light_pulses((*frame)->payload) == burst;
		}
	}
	check(received, "and arrives unchanged");

	check(irlink::parse_light_pulses(irlink::serialize(irlink::LightPulses{21, 400, 400}))
			      .error() == irlink::PayloadError::InvalidValue,
	      "more flashes than the head allows are refused");
	check(irlink::parse_light_pulses(irlink::serialize(irlink::LightPulses{1, 5, 400}))
			      .error() == irlink::PayloadError::InvalidValue,
	      "a flash too short to be seen is refused");
	check(irlink::parse_light_pulses(irlink::serialize(irlink::LightPulses{})).has_value(),
	      "a stop command is accepted");

	const auto sequence = s0::pin_sequence("9051", {.wake_pulses = 0});

	check(sequence && sequence->size() == 4, "without wake-up there is one step per digit");
	check(sequence && (*sequence)[0].pulses.count == 9 && (*sequence)[1].pulses.count == 0 &&
		      (*sequence)[2].pulses.count == 5 && (*sequence)[3].pulses.count == 1,
	      "each digit is that many flashes");
	check(sequence && (*sequence)[1].duration_ms() == 3500,
	      "a zero is only the accepting pause");
}

/* --- captive portal name server ------------------------------------------ */

/* A query for one name, as a resolver sends it. */
Bytes dns_query(std::string_view name, std::uint16_t type, bool with_edns = false)
{
	Bytes query{0x12, 0x34, 0x01, 0x00, 0x00, 0x01,
		    0x00, 0x00, 0x00, 0x00, 0x00, static_cast<std::uint8_t>(with_edns ? 1 : 0)};

	while (!name.empty()) {
		const auto dot = name.find('.');
		const auto label = name.substr(0, dot);

		query.push_back(static_cast<std::uint8_t>(label.size()));
		query.insert(query.end(), label.begin(), label.end());
		name = dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
	}
	query.insert(query.end(), {0x00, static_cast<std::uint8_t>(type >> 8),
				   static_cast<std::uint8_t>(type), 0x00, 0x01});
	if (with_edns) {
		query.insert(query.end(),
			     {0x00, 0x00, 0x29, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
	}
	return query;
}

void test_dns_redirect()
{
	constexpr s0::dns::Ipv4Address address{192, 168, 4, 1};
	constexpr std::uint16_t type_a = 1;
	constexpr std::uint16_t type_aaaa = 28;
	std::array<std::uint8_t, s0::dns::max_message_size> reply{};

	const Bytes query = dns_query("connectivitycheck.gstatic.com", type_a);
	const auto length = s0::dns::build_reply(query, address, reply);

	check(length == query.size() + 16, "an address query gets one answer record");
	check(reply[0] == 0x12 && reply[1] == 0x34, "the reply carries the query's id");
	check((reply[2] & 0x80) != 0 && (reply[3] & 0x0f) == 0, "it is a response without error");
	check(reply[7] == 1, "with one answer");
	check(length && Bytes(reply.begin() + *length - 4, reply.begin() + *length) ==
				Bytes(address.begin(), address.end()),
	      "which is the portal's address");
	check(length && reply[query.size()] == 0xc0 && reply[query.size() + 1] == 12,
	      "named by a pointer to the question");

	const Bytes with_edns = dns_query("captive.apple.com", type_a, true);
	const auto edns_length = s0::dns::build_reply(with_edns, address, reply);

	check(edns_length == with_edns.size() - 11 + 16,
	      "an EDNS record behind the question is dropped");
	check(reply[11] == 0, "and not announced in the reply");

	const Bytes ipv6 = dns_query("www.msftconnecttest.com", type_aaaa);
	const auto ipv6_length = s0::dns::build_reply(ipv6, address, reply);

	check(ipv6_length == ipv6.size() && reply[7] == 0, "an IPv6 query gets an empty answer");

	Bytes response = query;

	response[2] |= 0x80;
	check(s0::dns::build_reply(response, address, reply).error() == s0::dns::Error::NotAQuery,
	      "responses are not answered");
	check(s0::dns::build_reply(Bytes(query.begin(), query.begin() + 20), address, reply)
			      .error() == s0::dns::Error::Malformed,
	      "a cut-off question is rejected");
	check(s0::dns::build_reply(Bytes{0x00, 0x01}, address, reply).error() ==
		      s0::dns::Error::TooShort,
	      "a datagram shorter than a header is rejected");

	Bytes compressed = query;

	compressed[12] = 0xc0;
	check(s0::dns::build_reply(compressed, address, reply).error() == s0::dns::Error::Malformed,
	      "a compressed name in a question is rejected");
	check(s0::dns::build_reply(query, address, std::span{reply}.first(query.size())).error() ==
		      s0::dns::Error::BufferTooSmall,
	      "a reply that does not fit is not sent");
}

} /* namespace */

int main()
{
	test_sml_values();
	test_sml_stream();
	test_sml_fill_bytes();
	test_sml_swapped_crc();
	test_sml_corruption();
	test_sml_truncated();
	test_sml_escape_in_data();
	test_sml_long_string();
	test_sml_garbage();
	test_iec_telegram();
	test_iec_parity();
	test_iec_noise();
	test_obis_text();
	test_link_frames();
	test_link_resync();
	test_light_signals();
	test_dns_redirect();

	std::printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
