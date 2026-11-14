/**
 * @file
 * @brief IEC 62056-21 ("D0") text telegram decoder.
 *
 * Decodes the ASCII data block that meters either push on their own
 * (mode D) or send in reply to a request (modes A and C):
 *
 * @code
 *     /ISK5MT174-0001
 *     1-0:1.8.0*255(001234.567*kWh)
 *     1-0:16.7.0*255(000123*W)
 *     !
 * @endcode
 *
 * The stream is expected as 7 data bits; the parity bit of 7E1 data read
 * through an 8N1 receiver is masked off.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include "meter/meter.hpp"

namespace s0 {

/** @brief Why a piece of text is not an OBIS identifier. */
enum class ObisError : std::uint8_t {
	BadPrefix,     /**< the part before ':' is not "medium-channel" */
	BadGroup,      /**< a value group is neither 0..255 nor a letter code */
	TooFewGroups,  /**< fewer than quantity and processing are given */
	TooManyGroups, /**< more than three dot-separated groups */
};

class IecDecoder {
public:
	/**
	 * @brief Feed one received byte.
	 *
	 * @param byte The byte as received; the top bit is ignored.
	 * @return true when this byte completed a telegram with at least one
	 *         value. reading() is then valid until the next such call.
	 */
	bool push(std::uint8_t byte) noexcept;

	/** @brief The most recently completed telegram. */
	[[nodiscard]] const Reading &reading() const noexcept { return reading_; }

	/**
	 * @brief Fetch a newly received identification line, once.
	 *
	 * Used by the active read-out sequence, which has to react to the
	 * identification before the data block follows.
	 *
	 * @return The line including its leading slash, e.g. "/ISK5MT174-0001",
	 *         or nothing if none arrived since the last call.
	 */
	[[nodiscard]] std::optional<std::string_view> take_identification() noexcept;

	/** @brief Forget any partially received telegram. */
	void reset() noexcept;

	/**
	 * @brief Parse an OBIS identifier in any of its usual spellings.
	 *
	 * Accepts the full form "1-0:1.8.0*255", reduced forms such as
	 * "1.8.0" or "1.8.0*00", and the letter codes of IEC 62056-21
	 * ("C.1.0", "F.F").
	 *
	 * @param identifier The text in front of the opening parenthesis.
	 * @return The code, or the reason the text is not an OBIS identifier.
	 */
	[[nodiscard]] static std::expected<Obis, ObisError>
	parse_obis(std::string_view identifier) noexcept;

private:
	bool end_of_line() noexcept;
	void data_line(std::string_view line) noexcept;

	StaticString<120> line_{};
	bool line_unusable_{}; /**< too long, or contains binary data */
	bool in_telegram_{};
	bool have_identification_{};
	StaticString<40> identification_{};
	Reading work_{};    /**< the telegram being received */
	Reading reading_{}; /**< the last complete telegram */
};

} /* namespace s0 */
