/**
 * @file
 * @brief Small fixed-capacity containers.
 *
 * The firmware does not use the heap for its own data, so everything has a
 * compile-time bound. Nothing in this file depends on Zephyr.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace s0 {

/**
 * @brief NUL-terminated string with inline storage.
 * @tparam Capacity The largest number of characters it can hold.
 */
template <std::size_t Capacity>
class StaticString {
public:
	constexpr StaticString() noexcept = default;

	/**
	 * @brief Construct from text, cutting it off at the capacity.
	 * @param text The initial contents.
	 */
	constexpr StaticString(std::string_view text) noexcept { assign(text); }

	/**
	 * @brief Replace the contents, cutting the text off at the capacity.
	 * @param text The new contents.
	 */
	constexpr void assign(std::string_view text) noexcept
	{
		length_ = std::min(text.size(), Capacity);
		std::copy_n(text.begin(), length_, characters_.begin());
		characters_[length_] = '\0';
	}

	/**
	 * @brief Append one character.
	 * @param character The character to append.
	 * @return false if the string is full; it is then left unchanged.
	 */
	constexpr bool push_back(char character) noexcept
	{
		if (length_ >= Capacity) {
			return false;
		}
		characters_[length_++] = character;
		characters_[length_] = '\0';
		return true;
	}

	/**
	 * @brief Append text, as much of it as fits.
	 * @param text The characters to append.
	 * @return false if the text had to be cut off.
	 */
	constexpr bool append(std::string_view text) noexcept
	{
		for (const char character : text) {
			if (!push_back(character)) {
				return false;
			}
		}
		return true;
	}

	constexpr void clear() noexcept
	{
		length_ = 0;
		characters_[0] = '\0';
	}

	[[nodiscard]] constexpr const char *c_str() const noexcept { return characters_.data(); }
	[[nodiscard]] constexpr std::size_t size() const noexcept { return length_; }
	[[nodiscard]] constexpr bool empty() const noexcept { return length_ == 0; }
	[[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

	[[nodiscard]] constexpr std::string_view view() const noexcept
	{
		return {characters_.data(), length_};
	}

	constexpr operator std::string_view() const noexcept { return view(); }

	friend constexpr bool operator==(const StaticString &text, std::string_view other) noexcept
	{
		return text.view() == other;
	}

private:
	std::array<char, Capacity + 1> characters_{};
	std::size_t length_{};
};

/**
 * @brief Vector with inline storage.
 * @tparam Element  The element type; must be default constructible.
 * @tparam Capacity The largest number of elements it can hold.
 */
template <typename Element, std::size_t Capacity>
class StaticVector {
public:
	/**
	 * @brief Append an element.
	 * @param element The element to append.
	 * @return false if the vector is full; the element is then dropped.
	 */
	constexpr bool push_back(const Element &element) noexcept
	{
		if (length_ >= Capacity) {
			return false;
		}
		elements_[length_++] = element;
		return true;
	}

	constexpr void clear() noexcept { length_ = 0; }

	[[nodiscard]] constexpr std::size_t size() const noexcept { return length_; }
	[[nodiscard]] constexpr bool empty() const noexcept { return length_ == 0; }
	[[nodiscard]] constexpr bool full() const noexcept { return length_ == Capacity; }

	constexpr Element *begin() noexcept { return elements_.data(); }
	constexpr Element *end() noexcept { return elements_.data() + length_; }
	constexpr const Element *begin() const noexcept { return elements_.data(); }
	constexpr const Element *end() const noexcept { return elements_.data() + length_; }
	constexpr Element &operator[](std::size_t index) noexcept { return elements_[index]; }
	constexpr const Element &operator[](std::size_t index) const noexcept
	{
		return elements_[index];
	}

private:
	std::array<Element, Capacity> elements_{};
	std::size_t length_{};
};

/**
 * @brief The lower-case hex digit for a value.
 * @param nibble The value; only its low four bits are used.
 */
[[nodiscard]] constexpr char hex_digit(unsigned nibble) noexcept
{
	return "0123456789abcdef"[nibble & 0x0f];
}

/**
 * @brief The value of a hex digit.
 * @param digit A character.
 * @return 0..15, or -1 if the character is not a hex digit.
 */
[[nodiscard]] constexpr int hex_value(char digit) noexcept
{
	if (digit >= '0' && digit <= '9') {
		return digit - '0';
	}
	if (digit >= 'a' && digit <= 'f') {
		return digit - 'a' + 10;
	}
	if (digit >= 'A' && digit <= 'F') {
		return digit - 'A' + 10;
	}
	return -1;
}

} /* namespace s0 */
