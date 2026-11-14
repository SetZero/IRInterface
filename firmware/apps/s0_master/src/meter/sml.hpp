/**
 * @file
 * @brief Smart Message Language (SML) decoder.
 *
 * Handles the SML transport protocol version 1 (escape sequences, fill
 * bytes, CRC) and extracts the value list of GetList responses, which is
 * what German "moderne Messeinrichtung" meters push on their optical port.
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
#include <string_view>

#include <irlink/crc16.hpp>

#include "meter/meter.hpp"

namespace s0 {

enum class SmlError : std::uint8_t {
	Truncated,  /**< the data ends in the middle of an element */
	BadType,    /**< an element has a type the encoding does not define */
	BadLength,  /**< an element's length field contradicts itself */
	TooDeep,    /**< lists nested further than any meter telegram needs */
	Unexpected, /**< a valid element, but not the one the structure requires */
	NoValues,   /**< a well-formed file without a value list */
	Checksum,   /**< the transport CRC does not match */
	Oversized,  /**< the file does not fit the receive buffer */
	Framing,    /**< an escape sequence that is neither end, literal nor start */
};

[[nodiscard]] constexpr std::string_view sml_error_name(SmlError error) noexcept
{
	switch (error) {
	case SmlError::Truncated:
		return "truncated";
	case SmlError::BadType:
		return "bad_type";
	case SmlError::BadLength:
		return "bad_length";
	case SmlError::TooDeep:
		return "too_deep";
	case SmlError::Unexpected:
		return "unexpected";
	case SmlError::NoValues:
		return "no_values";
	case SmlError::Checksum:
		return "checksum";
	case SmlError::Oversized:
		return "oversized";
	case SmlError::Framing:
		return "framing";
	}
	return "";
}

class SmlDecoder {
public:
	/**
	 * @brief Feed one received byte. Returns true when this byte completed a
	 * telegram that passed the CRC check and contained meter values;
	 * reading() is then valid until the next call that returns true.
	 */
	bool push(std::uint8_t byte) noexcept;

	[[nodiscard]] const Reading &reading() const noexcept { return reading_; }

	/** @brief Files that started correctly but were then rejected. */
	[[nodiscard]] std::uint32_t errors() const noexcept { return errors_; }

	/** @brief Why the most recent rejected file was rejected. */
	[[nodiscard]] std::optional<SmlError> last_error() const noexcept { return last_error_; }

	/** @brief Parse the body of an SML file (the part between the escape sequences). */
	[[nodiscard]] static std::expected<Reading, SmlError>
	parse(std::span<const std::uint8_t> body) noexcept;

private:
	enum class State : std::uint8_t {
		Sync,    /**< looking for the start sequence */
		Data,    /**< collecting the file body */
		Trailer, /**< the four bytes after an escape sequence */
	};

	void begin_file() noexcept;
	void reject(SmlError error) noexcept;
	bool finish() noexcept;

	static constexpr std::size_t max_body = 1024;

	State state_{State::Sync};
	std::uint64_t window_{};
	std::array<std::uint8_t, max_body> body_{};
	std::size_t body_len_{};
	unsigned escape_run_{};
	std::array<std::uint8_t, 4> trailer_{};
	unsigned trailer_len_{};
	irlink::Crc16X25 crc_{};
	std::uint16_t crc_before_checksum_{};
	std::uint32_t errors_{};
	std::optional<SmlError> last_error_{};
	Reading reading_{};
};

} /* namespace s0 */
