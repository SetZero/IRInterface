/**
 * @file
 * @brief DNS replies that send every name to one address.
 *
 * This is the name-server half of the captive portal: while a phone is
 * joined to the setup access point, whatever host it asks for resolves to
 * the device, so its connectivity check reaches our web server. Nothing in
 * this file depends on Zephyr.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace s0::dns {

/** @brief An IPv4 address in network byte order. */
using Ipv4Address = std::array<std::uint8_t, 4>;

/** @brief Why a received datagram gets no reply. */
enum class Error : std::uint8_t {
	TooShort,       /**< shorter than a DNS header */
	NotAQuery,      /**< a response, a non-standard opcode, or no question */
	Malformed,      /**< the question runs past the end or uses compression */
	BufferTooSmall, /**< the reply does not fit the caller's buffer */
};

/** @brief Size of the fixed DNS message header. */
inline constexpr std::size_t header_size = 12;
/** @brief Largest DNS message carried in plain UDP. */
inline constexpr std::size_t max_message_size = 512;
/**
 * @brief How long clients may cache an answer, in seconds.
 *
 * Short, so that a phone forgets the redirection soon after it has left
 * the access point.
 */
inline constexpr std::uint32_t answer_lifetime_s = 10;

namespace detail {

inline constexpr std::uint8_t response_flag = 0x80;       /* QR, in the first flags byte */
inline constexpr std::uint8_t opcode_mask = 0x78;         /* in the first flags byte */
inline constexpr std::uint8_t authoritative_flag = 0x04;  /* AA, in the first flags byte */
inline constexpr std::uint8_t recursion_desired = 0x01;   /* RD, in the first flags byte */
inline constexpr std::uint8_t recursion_available = 0x80; /* RA, in the second flags byte */
inline constexpr std::uint8_t compression_mask = 0xc0;    /* top bits of a label length */
inline constexpr std::size_t max_name_length = 255;
inline constexpr std::size_t question_tail_size = 4; /* type and class */
inline constexpr std::uint16_t type_address = 1;     /* A */
inline constexpr std::uint16_t class_internet = 1;   /* IN */
inline constexpr std::size_t answer_size = 16;

[[nodiscard]] constexpr std::uint16_t read_u16(std::span<const std::uint8_t> bytes,
					       std::size_t position) noexcept
{
	return static_cast<std::uint16_t>((bytes[position] << 8) | bytes[position + 1]);
}

} /* namespace detail */

/**
 * @brief Build the reply to a DNS query.
 *
 * Address (A) questions are answered with @p address, whatever the name.
 * Questions of any other type get an empty answer without an error, which
 * makes clients fall back to their A query.
 *
 * @param query   The datagram as received.
 * @param address The address every name resolves to.
 * @param reply   Destination; max_message_size bytes always suffice.
 * @return The length of the reply, or why none is sent.
 */
[[nodiscard]] constexpr std::expected<std::size_t, Error>
build_reply(std::span<const std::uint8_t> query, const Ipv4Address &address,
	    std::span<std::uint8_t> reply) noexcept
{
	if (query.size() < header_size) {
		return std::unexpected{Error::TooShort};
	}

	const std::uint8_t flags = query[2];
	const std::uint16_t question_count = detail::read_u16(query, 4);

	if ((flags & detail::response_flag) != 0 || (flags & detail::opcode_mask) != 0 ||
	    question_count == 0) {
		return std::unexpected{Error::NotAQuery};
	}

	/* walk the labels of the first question's name: length, characters, ... 0 */
	std::size_t position = header_size;

	while (true) {
		if (position >= query.size()) {
			return std::unexpected{Error::Malformed};
		}

		const std::uint8_t label_length = query[position];

		if ((label_length & detail::compression_mask) != 0) {
			return std::unexpected{Error::Malformed};
		}
		position += 1 + label_length;
		if (label_length == 0) {
			break;
		}
		if (position - header_size > detail::max_name_length) {
			return std::unexpected{Error::Malformed};
		}
	}

	if (position + detail::question_tail_size > query.size()) {
		return std::unexpected{Error::Malformed};
	}

	const std::size_t question_end = position + detail::question_tail_size;
	const bool wants_address = detail::read_u16(query, position) == detail::type_address &&
				   detail::read_u16(query, position + 2) == detail::class_internet;
	const std::size_t reply_size = question_end + (wants_address ? detail::answer_size : 0);

	if (reply.size() < reply_size) {
		return std::unexpected{Error::BufferTooSmall};
	}

	/* header and question are repeated; anything behind them (EDNS) is dropped */
	std::copy_n(query.begin(), question_end, reply.begin());
	reply[2] = static_cast<std::uint8_t>(detail::response_flag | detail::authoritative_flag |
					     (flags & detail::recursion_desired));
	reply[3] = detail::recursion_available; /* response code 0: no error */
	reply[4] = 0;
	reply[5] = 1; /* one question */
	reply[6] = 0;
	reply[7] = wants_address ? 1 : 0; /* answers */
	std::fill_n(reply.begin() + 8, 4,
		    std::uint8_t{0}); /* no authority, no additional records */

	if (wants_address) {
		const std::array<std::uint8_t, detail::answer_size> answer{
			0xc0,
			header_size, /* the name: a pointer to the question's */
			0,
			detail::type_address,
			0,
			detail::class_internet,
			static_cast<std::uint8_t>(answer_lifetime_s >> 24),
			static_cast<std::uint8_t>(answer_lifetime_s >> 16),
			static_cast<std::uint8_t>(answer_lifetime_s >> 8),
			static_cast<std::uint8_t>(answer_lifetime_s),
			0,
			static_cast<std::uint8_t>(address.size()),
			address[0],
			address[1],
			address[2],
			address[3],
		};

		std::ranges::copy(answer,
				  reply.begin() + static_cast<std::ptrdiff_t>(question_end));
	}

	return reply_size;
}

} /* namespace s0::dns */
