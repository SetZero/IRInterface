/**
 * @file
 * @brief RS-485 link protocol between the S0 master and the IR head.
 *
 * The IR head acts as a remote UART: it forwards whatever the optical
 * receiver sees and takes commands to change the optical UART settings or
 * to transmit through the IR LED.
 *
 * Wire format of one frame:
 *
 * @code
 *     COBS( type | sequence | payload[0..max_payload] | crc low | crc high ) 0x00
 * @endcode
 *
 * The CRC is CRC-16/X-25 over type, sequence and payload. Multi-byte fields
 * in payloads are little endian.
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
#include <utility>

#include "cobs.hpp"
#include "crc16.hpp"

namespace irlink {

/** @brief Version of this protocol, reported in every Status. */
inline constexpr std::uint8_t protocol_version = 1;
/** @brief Baud rate of the RS-485 link (8N1). */
inline constexpr std::uint32_t link_baud = 115200;

/** @brief Largest payload of one frame. */
inline constexpr std::size_t max_payload = 64;
/** @brief Bytes in front of the payload: type and sequence number. */
inline constexpr std::size_t header_size = 2;
/** @brief Bytes of the checksum behind the payload. */
inline constexpr std::size_t crc_size = 2;
/** @brief Largest frame before COBS encoding. */
inline constexpr std::size_t max_raw_frame = header_size + max_payload + crc_size;
/** @brief Largest frame on the wire, including the trailing delimiter. */
inline constexpr std::size_t max_wire_frame = cobs::max_encoded_size(max_raw_frame) + 1;

/** @brief The byte that ends every frame and never occurs inside one. */
inline constexpr std::uint8_t delimiter = 0x00;

/** @brief What a frame carries. */
enum class MsgType : std::uint8_t {
	IrData = 0x01,    /**< head to master: raw bytes from the optical receiver */
	Status = 0x02,    /**< head to master: a Status, once a second and after every command */
	LinkError = 0x03, /**< head to master: a LinkErrorReport about a frame it rejected */

	SetConfig = 0x10,   /**< master to head: an IrConfig */
	IrTx = 0x11,        /**< master to head: bytes to send through the IR LED */
	GetStatus = 0x12,   /**< master to head: no payload */
	LightPulses = 0x13, /**< master to head: a LightPulses burst for a meter's optical button */
};

/** @brief Character format of the optical UART. */
enum class IrFormat : std::uint8_t {
	F8N1 = 0, /**< 8 data bits, no parity, 1 stop bit */
	F7E1 = 1, /**< 7 data bits, even parity, 1 stop bit */
};

/** @brief Slowest optical baud rate the head accepts. */
inline constexpr std::uint32_t min_ir_baud = 300;
/** @brief Fastest optical baud rate the head accepts. */
inline constexpr std::uint32_t max_ir_baud = 38400;

/** @brief Settings of the optical UART. */
struct IrConfig {
	std::uint32_t baud{9600};
	IrFormat format{IrFormat::F8N1};

	friend constexpr bool operator==(const IrConfig &, const IrConfig &) = default;
};

/** @brief State of the IR head, as reported to the master. */
struct Status {
	std::uint8_t protocol{protocol_version};
	std::uint8_t firmware_major{};
	std::uint8_t firmware_minor{};
	std::uint32_t uptime_s{};
	IrConfig ir{};                 /**< the optical settings in effect */
	std::uint32_t ir_rx_bytes{};   /**< bytes received on the optical side */
	std::uint16_t ir_rx_errors{};  /**< framing, parity and noise errors */
	std::uint16_t ir_rx_dropped{}; /**< bytes lost to buffer overruns */
	std::uint16_t link_errors{};   /**< RS-485 frames with bad CRC or encoding */
};

/**
 * @brief A burst of equal, slow flashes of the IR LED.
 *
 * Meant for the optical button of a meter, which reacts to light that
 * stays on for a fraction of a second, not to serial data.
 */
struct LightPulses {
	std::uint8_t count{};   /**< number of flashes; 0 stops a burst that is running */
	std::uint16_t on_ms{};  /**< how long the LED is lit for each flash */
	std::uint16_t off_ms{}; /**< darkness after each flash */

	friend constexpr bool operator==(const LightPulses &, const LightPulses &) = default;
};

/** @brief Most flashes one burst may have. */
inline constexpr std::uint8_t max_light_pulses = 20;
/** @brief Shortest flash or gap the head accepts, in milliseconds. */
inline constexpr std::uint16_t min_light_ms = 20;
/** @brief Longest flash or gap the head accepts, in milliseconds. */
inline constexpr std::uint16_t max_light_ms = 20000;

/**
 * @brief Whether the head will carry out a burst.
 * @param pulses The burst to check.
 */
[[nodiscard]] constexpr bool is_valid(const LightPulses &pulses) noexcept
{
	if (pulses.count == 0) {
		return true; /* a stop command; the times do not matter */
	}
	return pulses.count <= max_light_pulses && pulses.on_ms >= min_light_ms &&
	       pulses.on_ms <= max_light_ms && pulses.off_ms >= min_light_ms &&
	       pulses.off_ms <= max_light_ms;
}

/** @brief Size of a serialized LightPulses. */
inline constexpr std::size_t light_pulses_size = 5;

/** @brief Size of a serialized IrConfig. */
inline constexpr std::size_t ir_config_size = 5;
/** @brief Size of a serialized Status. */
inline constexpr std::size_t status_size = 3 + 4 + ir_config_size + 4 + 2 + 2 + 2;
static_assert(status_size <= max_payload);

/** @brief Why a payload could not be interpreted. */
enum class PayloadError : std::uint8_t {
	TooShort,     /**< fewer bytes than the message needs */
	InvalidValue, /**< a field holds a value the protocol does not define */
};

/** @brief Why a frame could not be built or was rejected. */
enum class FrameError : std::uint8_t {
	PayloadTooLarge, /**< more than max_payload bytes */
	BufferTooSmall,  /**< the caller's buffer cannot hold the encoded frame */
	Oversized,       /**< more bytes arrived before a delimiter than a frame can have */
	Encoding,        /**< the bytes are not valid COBS */
	TooShort,        /**< shorter than header plus checksum */
	Checksum,        /**< the CRC does not match */
};

namespace detail {

/** @brief Appends little-endian fields to a fixed-size payload. */
template <std::size_t Size>
class FieldWriter {
public:
	constexpr void u8(std::uint8_t value) noexcept { bytes_[position_++] = value; }

	constexpr void u16(std::uint16_t value) noexcept
	{
		u8(static_cast<std::uint8_t>(value));
		u8(static_cast<std::uint8_t>(value >> 8));
	}

	constexpr void u32(std::uint32_t value) noexcept
	{
		u16(static_cast<std::uint16_t>(value));
		u16(static_cast<std::uint16_t>(value >> 16));
	}

	[[nodiscard]] constexpr const std::array<std::uint8_t, Size> &bytes() const noexcept
	{
		return bytes_;
	}

private:
	std::array<std::uint8_t, Size> bytes_{};
	std::size_t position_{};
};

/** @brief Takes little-endian fields from a payload whose length was checked. */
class FieldReader {
public:
	constexpr explicit FieldReader(std::span<const std::uint8_t> bytes) noexcept : bytes_{bytes}
	{
	}

	constexpr std::uint8_t u8() noexcept { return bytes_[position_++]; }

	constexpr std::uint16_t u16() noexcept
	{
		const std::uint16_t low = u8();

		return static_cast<std::uint16_t>(low | (u8() << 8));
	}

	constexpr std::uint32_t u32() noexcept
	{
		const std::uint32_t low = u16();

		return low | (static_cast<std::uint32_t>(u16()) << 16);
	}

private:
	std::span<const std::uint8_t> bytes_;
	std::size_t position_{};
};

[[nodiscard]] constexpr std::expected<IrFormat, PayloadError> to_format(std::uint8_t raw) noexcept
{
	switch (raw) {
	case static_cast<std::uint8_t>(IrFormat::F8N1):
		return IrFormat::F8N1;
	case static_cast<std::uint8_t>(IrFormat::F7E1):
		return IrFormat::F7E1;
	default:
		return std::unexpected{PayloadError::InvalidValue};
	}
}

} /* namespace detail */

/**
 * @brief The payload of a MsgType::SetConfig frame.
 * @param config The settings to send.
 */
[[nodiscard]] constexpr std::array<std::uint8_t, ir_config_size>
serialize(const IrConfig &config) noexcept
{
	detail::FieldWriter<ir_config_size> writer;

	writer.u32(config.baud);
	writer.u8(static_cast<std::uint8_t>(config.format));
	return writer.bytes();
}

/**
 * @brief The payload of a MsgType::Status frame.
 * @param status The state to report.
 */
[[nodiscard]] constexpr std::array<std::uint8_t, status_size>
serialize(const Status &status) noexcept
{
	detail::FieldWriter<status_size> writer;

	writer.u8(status.protocol);
	writer.u8(status.firmware_major);
	writer.u8(status.firmware_minor);
	writer.u32(status.uptime_s);
	writer.u32(status.ir.baud);
	writer.u8(static_cast<std::uint8_t>(status.ir.format));
	writer.u32(status.ir_rx_bytes);
	writer.u16(status.ir_rx_errors);
	writer.u16(status.ir_rx_dropped);
	writer.u16(status.link_errors);
	return writer.bytes();
}

/**
 * @brief Interpret the payload of a MsgType::SetConfig frame.
 *
 * @param payload The frame's payload.
 * @return The settings, or why they are unusable. A baud rate outside
 *         [min_ir_baud, max_ir_baud] counts as invalid.
 */
[[nodiscard]] constexpr std::expected<IrConfig, PayloadError>
parse_ir_config(std::span<const std::uint8_t> payload) noexcept
{
	if (payload.size() < ir_config_size) {
		return std::unexpected{PayloadError::TooShort};
	}

	detail::FieldReader reader{payload};
	const std::uint32_t baud = reader.u32();
	const auto format = detail::to_format(reader.u8());

	if (!format) {
		return std::unexpected{format.error()};
	}
	if (baud < min_ir_baud || baud > max_ir_baud) {
		return std::unexpected{PayloadError::InvalidValue};
	}
	return IrConfig{baud, *format};
}

/**
 * @brief Interpret the payload of a MsgType::Status frame.
 * @param payload The frame's payload.
 * @return The reported state, or why the payload is unusable.
 */
[[nodiscard]] constexpr std::expected<Status, PayloadError>
parse_status(std::span<const std::uint8_t> payload) noexcept
{
	if (payload.size() < status_size) {
		return std::unexpected{PayloadError::TooShort};
	}

	detail::FieldReader reader{payload};
	Status status;

	status.protocol = reader.u8();
	status.firmware_major = reader.u8();
	status.firmware_minor = reader.u8();
	status.uptime_s = reader.u32();
	status.ir.baud = reader.u32();

	const auto format = detail::to_format(reader.u8());

	if (!format) {
		return std::unexpected{format.error()};
	}
	status.ir.format = *format;
	status.ir_rx_bytes = reader.u32();
	status.ir_rx_errors = reader.u16();
	status.ir_rx_dropped = reader.u16();
	status.link_errors = reader.u16();
	return status;
}

/**
 * @brief The payload of a MsgType::LightPulses frame.
 * @param pulses The burst to send.
 */
[[nodiscard]] constexpr std::array<std::uint8_t, light_pulses_size>
serialize(const LightPulses &pulses) noexcept
{
	detail::FieldWriter<light_pulses_size> writer;

	writer.u8(pulses.count);
	writer.u16(pulses.on_ms);
	writer.u16(pulses.off_ms);
	return writer.bytes();
}

/**
 * @brief Interpret the payload of a MsgType::LightPulses frame.
 * @param payload The frame's payload.
 * @return The burst, or why it is unusable; see is_valid().
 */
[[nodiscard]] constexpr std::expected<LightPulses, PayloadError>
parse_light_pulses(std::span<const std::uint8_t> payload) noexcept
{
	if (payload.size() < light_pulses_size) {
		return std::unexpected{PayloadError::TooShort};
	}

	detail::FieldReader reader{payload};
	LightPulses pulses;

	pulses.count = reader.u8();
	pulses.on_ms = reader.u16();
	pulses.off_ms = reader.u16();

	if (!is_valid(pulses)) {
		return std::unexpected{PayloadError::InvalidValue};
	}
	return pulses;
}

/** @brief Most received bytes a LinkErrorReport carries. */
inline constexpr std::size_t max_reported_bytes = 40;

/**
 * @brief What the head saw when it rejected a frame from the master.
 *
 * Sent so that problems on the master-to-head pair can be diagnosed from
 * the master's side; the head has no console of its own in the field.
 */
struct LinkErrorReport {
	FrameError error{};
	std::uint16_t uart_errors{}; /**< overrun, framing and noise errors of the head's receiver
					so far */
	std::uint8_t length{}; /**< bytes the head had received when it gave up on the frame */
	std::array<std::uint8_t, max_reported_bytes>
		bytes{}; /**< the first of them, as on the wire */

	/** @brief Number of entries of `bytes` that are valid. */
	[[nodiscard]] constexpr std::size_t reported() const noexcept
	{
		return length < max_reported_bytes ? length : max_reported_bytes;
	}
};

/** @brief Size of the fixed part of a serialized LinkErrorReport. */
inline constexpr std::size_t link_error_header_size = 4;
static_assert(link_error_header_size + max_reported_bytes <= max_payload);

/**
 * @brief The payload of a MsgType::LinkError frame.
 * @param report The report to send.
 * @return The payload and the number of its bytes that are used.
 */
[[nodiscard]] constexpr std::pair<
	std::array<std::uint8_t, link_error_header_size + max_reported_bytes>, std::size_t>
serialize(const LinkErrorReport &report) noexcept
{
	detail::FieldWriter<link_error_header_size + max_reported_bytes> writer;

	writer.u8(static_cast<std::uint8_t>(report.error));
	writer.u16(report.uart_errors);
	writer.u8(report.length);
	for (std::size_t index = 0; index < report.reported(); index++) {
		writer.u8(report.bytes[index]);
	}
	return {writer.bytes(), link_error_header_size + report.reported()};
}

/**
 * @brief Interpret the payload of a MsgType::LinkError frame.
 * @param payload The frame's payload.
 * @return The report, or why the payload is unusable.
 */
[[nodiscard]] constexpr std::expected<LinkErrorReport, PayloadError>
parse_link_error(std::span<const std::uint8_t> payload) noexcept
{
	if (payload.size() < link_error_header_size) {
		return std::unexpected{PayloadError::TooShort};
	}

	detail::FieldReader reader{payload};
	LinkErrorReport report;
	const std::uint8_t error = reader.u8();

	if (error > static_cast<std::uint8_t>(FrameError::Checksum)) {
		return std::unexpected{PayloadError::InvalidValue};
	}
	report.error = static_cast<FrameError>(error);
	report.uart_errors = reader.u16();
	report.length = reader.u8();
	if (payload.size() < link_error_header_size + report.reported()) {
		return std::unexpected{PayloadError::TooShort};
	}
	for (std::size_t index = 0; index < report.reported(); index++) {
		report.bytes[index] = reader.u8();
	}
	return report;
}

/** @brief The name of a frame error, for logs. */
[[nodiscard]] constexpr const char *frame_error_name(FrameError error) noexcept
{
	switch (error) {
	case FrameError::PayloadTooLarge:
		return "payload too large";
	case FrameError::BufferTooSmall:
		return "buffer too small";
	case FrameError::Oversized:
		return "oversized";
	case FrameError::Encoding:
		return "bad encoding";
	case FrameError::TooShort:
		return "too short";
	case FrameError::Checksum:
		return "bad checksum";
	}
	return "";
}

/** @brief One decoded frame. */
struct Frame {
	MsgType type{};
	std::uint8_t sequence{};
	std::span<const std::uint8_t> payload{};
};

/**
 * @brief Build a complete wire frame: COBS encoded, delimiter appended.
 *
 * @param type     What the frame carries.
 * @param sequence Sequence number; a reply repeats the number of its command.
 * @param payload  At most max_payload bytes.
 * @param wire     Destination; max_wire_frame bytes always suffice.
 * @return The number of bytes to transmit, or why the frame cannot be built.
 */
[[nodiscard]] constexpr std::expected<std::size_t, FrameError>
encode_frame(MsgType type, std::uint8_t sequence, std::span<const std::uint8_t> payload,
	     std::span<std::uint8_t> wire) noexcept
{
	std::array<std::uint8_t, max_raw_frame> raw{};
	std::size_t length = 0;

	if (payload.size() > max_payload) {
		return std::unexpected{FrameError::PayloadTooLarge};
	}
	if (wire.empty()) {
		return std::unexpected{FrameError::BufferTooSmall};
	}

	raw[length++] = static_cast<std::uint8_t>(type);
	raw[length++] = sequence;
	for (const auto byte : payload) {
		raw[length++] = byte;
	}

	const std::uint16_t crc = crc16_x25(std::span{raw}.first(length));

	raw[length++] = static_cast<std::uint8_t>(crc);
	raw[length++] = static_cast<std::uint8_t>(crc >> 8);

	/* keep the last byte of the buffer free for the delimiter */
	const auto encoded =
		cobs::encode(std::span{raw}.first(length), wire.first(wire.size() - 1));

	if (!encoded) {
		return std::unexpected{FrameError::BufferTooSmall};
	}
	wire[*encoded] = delimiter;
	return *encoded + 1;
}

/**
 * @brief Turns the received byte stream back into frames.
 *
 * Collects bytes until a delimiter arrives, then undoes the COBS encoding
 * and checks the CRC.
 */
class FrameDecoder {
public:
	/**
	 * @brief Feed one received byte.
	 *
	 * @param byte The next byte from the link.
	 * @return A frame when this byte completed a valid one, nothing while a
	 *         frame is still incomplete, or the reason a frame was rejected.
	 *         The frame's payload points into the decoder and stays valid
	 *         until the next call.
	 */
	constexpr std::expected<std::optional<Frame>, FrameError> push(std::uint8_t byte) noexcept
	{
		if (byte != delimiter) {
			if (wire_length_ < wire_.size()) {
				wire_[wire_length_++] = byte;
			} else {
				oversized_ = true;
			}
			return std::nullopt;
		}

		const std::size_t wire_length = wire_length_;
		const bool oversized = oversized_;

		wire_length_ = 0;
		oversized_ = false;
		finished_length_ = wire_length;

		if (oversized) {
			return std::unexpected{FrameError::Oversized};
		}
		if (wire_length == 0) {
			/* back-to-back delimiters are just idle fill */
			return std::nullopt;
		}

		const auto raw_length = cobs::decode(std::span{wire_}.first(wire_length), raw_);

		if (!raw_length) {
			return std::unexpected{FrameError::Encoding};
		}
		if (*raw_length < header_size + crc_size) {
			return std::unexpected{FrameError::TooShort};
		}

		const std::size_t checked_length = *raw_length - crc_size;
		const auto received_crc = static_cast<std::uint16_t>(
			raw_[checked_length] | (raw_[checked_length + 1] << 8));

		if (crc16_x25(std::span{raw_}.first(checked_length)) != received_crc) {
			return std::unexpected{FrameError::Checksum};
		}

		return Frame{
			.type = static_cast<MsgType>(raw_[0]),
			.sequence = raw_[1],
			.payload =
				std::span{raw_}.subspan(header_size, checked_length - header_size),
		};
	}

	/**
	 * @brief The bytes of the frame the last delimiter ended, as received.
	 *
	 * Meant for reporting a frame that push() has just rejected. Valid
	 * until the next call to push().
	 */
	[[nodiscard]] constexpr std::span<const std::uint8_t> finished_frame() const noexcept
	{
		return std::span{wire_}.first(finished_length_);
	}

private:
	std::array<std::uint8_t, max_wire_frame> wire_{}; /**< bytes since the last delimiter */
	std::array<std::uint8_t, max_raw_frame> raw_{};   /**< the last frame, COBS removed */
	std::size_t wire_length_{};
	std::size_t finished_length_{};
	bool oversized_{};
};

/* A frame survives the trip through encoder and decoder. */
static_assert([] {
	constexpr IrConfig config{2400, IrFormat::F7E1};
	std::array<std::uint8_t, max_wire_frame> wire{};
	const auto length = encode_frame(MsgType::SetConfig, 7, serialize(config), wire);
	FrameDecoder decoder;

	if (!length) {
		return false;
	}
	for (std::size_t index = 0; index + 1 < *length; index++) {
		const auto partial = decoder.push(wire[index]);

		if (!partial || partial->has_value()) {
			return false;
		}
	}

	const auto frame = decoder.push(wire[*length - 1]);

	return frame && frame->has_value() && (*frame)->type == MsgType::SetConfig &&
	       (*frame)->sequence == 7 && parse_ir_config((*frame)->payload) == config;
}());

} /* namespace irlink */
