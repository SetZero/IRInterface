/**
 * @file
 * @brief CRC-16/X-25 (reflected polynomial 0x1021, initial value 0xFFFF,
 *        final XOR 0xFFFF).
 *
 * Used for the RS-485 link frames and, on the master, for SML telegrams,
 * which use the same checksum.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace irlink {

/** @brief Incremental CRC-16/X-25 calculation. */
class Crc16X25 {
public:
	/**
	 * @brief Include one byte in the checksum.
	 * @param byte The next byte of the message.
	 */
	constexpr void update(std::uint8_t byte) noexcept
	{
		/* two table steps, one per nibble, low nibble first */
		state_ ^= byte;
		state_ = static_cast<std::uint16_t>((state_ >> 4) ^ nibble_table[state_ & 0x0f]);
		state_ = static_cast<std::uint16_t>((state_ >> 4) ^ nibble_table[state_ & 0x0f]);
	}

	/**
	 * @brief Include a run of bytes in the checksum.
	 * @param data The next bytes of the message.
	 */
	constexpr void update(std::span<const std::uint8_t> data) noexcept
	{
		for (const auto byte : data) {
			update(byte);
		}
	}

	/** @brief The checksum of everything passed to update() so far. */
	[[nodiscard]] constexpr std::uint16_t value() const noexcept
	{
		return static_cast<std::uint16_t>(state_ ^ 0xffff);
	}

private:
	/** A table per nibble instead of per byte keeps the IR head's flash use small. */
	static constexpr std::array<std::uint16_t, 16> nibble_table{
		0x0000, 0x1081, 0x2102, 0x3183, 0x4204, 0x5285, 0x6306, 0x7387,
		0x8408, 0x9489, 0xa50a, 0xb58b, 0xc60c, 0xd68d, 0xe70e, 0xf78f,
	};

	std::uint16_t state_{0xffff};
};

/**
 * @brief The CRC-16/X-25 of a complete message.
 * @param data The message.
 * @return The checksum.
 */
[[nodiscard]] constexpr std::uint16_t crc16_x25(std::span<const std::uint8_t> data) noexcept
{
	Crc16X25 crc;

	crc.update(data);
	return crc.value();
}

/* "123456789" -> 0x906E is the published check value for CRC-16/X-25 */
static_assert([] {
	constexpr std::array<std::uint8_t, 9> check{'1', '2', '3', '4', '5', '6', '7', '8', '9'};

	return crc16_x25(check) == 0x906e;
}());

} /* namespace irlink */
