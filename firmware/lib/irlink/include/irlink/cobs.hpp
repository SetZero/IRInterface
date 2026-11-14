/**
 * @file
 * @brief Consistent Overhead Byte Stuffing.
 *
 * Encoded data never contains 0x00, so a single zero byte delimits frames
 * on the RS-485 link and a receiver can always resynchronise.
 *
 * The encoding cuts the data into blocks at every zero byte. Each block is
 * sent as a code byte holding its length plus one, followed by its non-zero
 * bytes; the zero itself is implied by the block boundary. A block of 254
 * bytes without a zero gets the code 0xFF, which implies no zero.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace irlink::cobs {

/** @brief Why encoding or decoding failed. */
enum class Error : std::uint8_t {
	OutputTooSmall, /**< the destination buffer cannot hold the result */
	Malformed,      /**< the input is not a valid COBS encoding */
};

/** @brief Code byte of a full block, one that is not followed by an implied zero. */
inline constexpr std::uint8_t full_block_code = 0xff;

/**
 * @brief Worst-case size of the encoded form.
 * @param length Number of bytes to encode.
 * @return The largest number of bytes encode() can produce for that input,
 *         not counting a frame delimiter.
 */
[[nodiscard]] constexpr std::size_t max_encoded_size(std::size_t length) noexcept
{
	return length + length / 254 + 1;
}

/**
 * @brief Encode data so that it contains no zero byte.
 *
 * @param input  The bytes to encode.
 * @param output Destination; max_encoded_size(input.size()) bytes always suffice.
 * @return The number of bytes written, or Error::OutputTooSmall.
 */
[[nodiscard]] constexpr std::expected<std::size_t, Error>
encode(std::span<const std::uint8_t> input, std::span<std::uint8_t> output) noexcept
{
	std::size_t code_position = 0; /* where the current block's code byte goes */
	std::size_t write_position = 1;
	std::uint8_t code = 1;

	if (output.empty()) {
		return std::unexpected{Error::OutputTooSmall};
	}

	for (const auto byte : input) {
		if (byte == 0) {
			/* the zero ends the block; it is implied, not stored */
			output[code_position] = code;
			code_position = write_position++;
			code = 1;
		} else {
			if (write_position >= output.size()) {
				return std::unexpected{Error::OutputTooSmall};
			}
			output[write_position++] = byte;
			if (++code == full_block_code) {
				output[code_position] = code;
				code_position = write_position++;
				code = 1;
			}
		}
		if (code_position >= output.size()) {
			return std::unexpected{Error::OutputTooSmall};
		}
	}

	output[code_position] = code;
	return write_position;
}

/**
 * @brief Reverse encode().
 *
 * @param input  One encoded frame without its delimiter.
 * @param output Destination; input.size() bytes always suffice.
 * @return The number of bytes written, or the reason for the failure.
 */
[[nodiscard]] constexpr std::expected<std::size_t, Error>
decode(std::span<const std::uint8_t> input, std::span<std::uint8_t> output) noexcept
{
	std::size_t read_position = 0;
	std::size_t write_position = 0;

	while (read_position < input.size()) {
		const std::uint8_t code = input[read_position++];

		if (code == 0 || read_position + code - 1 > input.size()) {
			return std::unexpected{Error::Malformed};
		}
		for (std::uint8_t copied = 1; copied < code; copied++) {
			if (write_position >= output.size()) {
				return std::unexpected{Error::OutputTooSmall};
			}
			output[write_position++] = input[read_position++];
		}
		if (code != full_block_code && read_position < input.size()) {
			if (write_position >= output.size()) {
				return std::unexpected{Error::OutputTooSmall};
			}
			output[write_position++] = 0;
		}
	}

	return write_position;
}

} /* namespace irlink::cobs */
