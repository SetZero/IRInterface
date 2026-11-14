/**
 * @file
 * @brief Minimal JSON support for the REST API.
 *
 * A streaming Writer into a fixed buffer and a reader (Object, Member) for
 * the flat objects the API accepts. Nothing here allocates or throws;
 * failures are reported through Writer::ok() and std::expected.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include "core/util.hpp"

namespace s0::json {

/** @brief The name of an object member, for streaming into a Writer. */
struct Key {
	std::string_view name;
};

/**
 * @brief Name the next object member.
 *
 * @code
 *     out << key("power") << 42;
 * @endcode
 *
 * @param name The member's name.
 */
[[nodiscard]] constexpr Key key(std::string_view name) noexcept
{
	return {name};
}

/** @brief A number with a chosen count of decimal places, for streaming into a Writer. */
struct Fixed {
	double number;
	unsigned decimals;
};

/**
 * @brief Format a number with a fixed count of decimal places.
 * @param number   The value.
 * @param decimals Digits behind the decimal point.
 */
[[nodiscard]] constexpr Fixed fixed(double number, unsigned decimals) noexcept
{
	return {number, decimals};
}

/** @brief Structural elements that can be streamed into a Writer. */
enum class Token : std::uint8_t {
	BeginObject,
	EndObject,
	BeginArray,
	EndArray,
	Null,
};

inline constexpr Token begin_object = Token::BeginObject;
inline constexpr Token end_object = Token::EndObject;
inline constexpr Token begin_array = Token::BeginArray;
inline constexpr Token end_array = Token::EndArray;
inline constexpr Token null = Token::Null;

/**
 * @brief Writes JSON text into a caller-provided buffer.
 *
 * Commas are inserted automatically. If the buffer is too small the output
 * is cut off and ok() returns false.
 *
 * @code
 *     Writer out{buffer};
 *     out << begin_object << key("power") << fixed(231.5, 1) << end_object;
 * @endcode
 */
class Writer {
public:
	/**
	 * @brief Start writing at the beginning of a buffer.
	 * @param buffer Destination of the JSON text; must outlive the writer.
	 */
	constexpr explicit Writer(std::span<char> buffer) noexcept : buffer_{buffer} {}

	constexpr Writer &begin_object() noexcept { return open('{'); }
	constexpr Writer &end_object() noexcept { return close('}'); }
	constexpr Writer &begin_array() noexcept { return open('['); }
	constexpr Writer &end_array() noexcept { return close(']'); }

	/**
	 * @brief Write the name of the next object member.
	 * @param name The member's name.
	 */
	constexpr Writer &key(std::string_view name) noexcept
	{
		separate();
		quoted(name);
		put(':');
		after_key_ = true;
		return *this;
	}

	/** @brief Write a string value, escaped as JSON requires. */
	constexpr Writer &value(std::string_view text) noexcept
	{
		separate();
		quoted(text);
		return *this;
	}

	/** @brief Write a string value from a NUL-terminated string. */
	constexpr Writer &value(const char *text) noexcept { return value(std::string_view{text}); }

	/** @brief Write `true` or `false`. */
	constexpr Writer &value(bool flag) noexcept
	{
		separate();
		put(flag ? "true" : "false");
		return *this;
	}

	/** @brief Write an integer of any width and signedness. */
	template <std::integral Integer>
		requires(!std::same_as<Integer, bool>)
	constexpr Writer &value(Integer number) noexcept
	{
		separate();
		if (std::cmp_less(number, 0)) {
			put('-');
			put_unsigned(0 - static_cast<std::uint64_t>(number));
		} else {
			put_unsigned(static_cast<std::uint64_t>(number));
		}
		return *this;
	}

	/**
	 * @brief Write a number in fixed-point notation.
	 *
	 * Values that JSON cannot represent (NaN, infinities) and values
	 * beyond 9e15 are written as null.
	 *
	 * @param number   The value.
	 * @param decimals Digits behind the decimal point.
	 */
	constexpr Writer &value(double number, unsigned decimals = 3) noexcept
	{
		separate();
		put_fixed(number, decimals);
		return *this;
	}

	/** @brief Write `null`. */
	constexpr Writer &null() noexcept
	{
		separate();
		put("null");
		return *this;
	}

	/**
	 * @brief Write an object member: its name and its value.
	 * @param name     The member's name.
	 * @param contents Anything value() accepts.
	 */
	template <typename Value>
	constexpr Writer &member(std::string_view name, const Value &contents) noexcept
	{
		return key(name).value(contents);
	}

	/** @brief Write an object member holding a fixed-point number. */
	constexpr Writer &member(std::string_view name, double number, unsigned decimals) noexcept
	{
		return key(name).value(number, decimals);
	}

	/** @brief Stream anything value() accepts. */
	template <typename Value>
		requires requires(Writer &writer, const Value &contents) { writer.value(contents); }
	constexpr Writer &operator<<(const Value &contents) noexcept
	{
		return value(contents);
	}

	/** @brief Stream the name of the next member, see json::key(). */
	constexpr Writer &operator<<(Key member_key) noexcept { return key(member_key.name); }

	/** @brief Stream a fixed-point number, see json::fixed(). */
	constexpr Writer &operator<<(Fixed formatted) noexcept
	{
		return value(formatted.number, formatted.decimals);
	}

	/** @brief Stream a structural element such as json::begin_object. */
	constexpr Writer &operator<<(Token token) noexcept
	{
		switch (token) {
		case Token::BeginObject:
			return begin_object();
		case Token::EndObject:
			return end_object();
		case Token::BeginArray:
			return begin_array();
		case Token::EndArray:
			return end_array();
		case Token::Null:
			return null();
		}
		return *this;
	}

	/** @brief false once output had to be cut off because the buffer is full. */
	[[nodiscard]] constexpr bool ok() const noexcept { return !overflowed_; }

	/** @brief Number of characters written so far. */
	[[nodiscard]] constexpr std::size_t size() const noexcept { return length_; }

	/** @brief The text written so far. */
	[[nodiscard]] constexpr std::string_view view() const noexcept
	{
		return {buffer_.data(), length_};
	}

private:
	constexpr Writer &open(char bracket) noexcept
	{
		separate();
		put(bracket);
		container_is_empty_ = true;
		return *this;
	}

	constexpr Writer &close(char bracket) noexcept
	{
		put(bracket);
		container_is_empty_ = false;
		return *this;
	}

	/** Write the comma that separates this element from the previous one, if any. */
	constexpr void separate() noexcept
	{
		if (after_key_) {
			after_key_ = false;
		} else if (!container_is_empty_ && length_ > 0) {
			put(',');
		}
		container_is_empty_ = false;
	}

	constexpr void put(char character) noexcept
	{
		if (length_ < buffer_.size()) {
			buffer_[length_++] = character;
		} else {
			overflowed_ = true;
		}
	}

	constexpr void put(std::string_view text) noexcept
	{
		for (const char character : text) {
			put(character);
		}
	}

	constexpr void quoted(std::string_view text) noexcept
	{
		put('"');
		for (const char character : text) {
			const auto code = static_cast<unsigned char>(character);

			if (character == '"' || character == '\\') {
				put('\\');
				put(character);
			} else if (code < 0x20) {
				put("\\u00");
				put(hex_digit(code >> 4));
				put(hex_digit(code));
			} else {
				put(character);
			}
		}
		put('"');
	}

	constexpr void put_unsigned(std::uint64_t number) noexcept
	{
		char digits[20]{}; /* 2^64 has 20 decimal digits */
		std::size_t count = 0;

		do {
			digits[count++] = static_cast<char>('0' + number % 10);
			number /= 10;
		} while (number != 0);

		while (count > 0) {
			put(digits[--count]);
		}
	}

	/* printf's float support is not linked in, so the digits are produced here */
	constexpr void put_fixed(double number, unsigned decimals) noexcept
	{
		constexpr double largest_exact = 9e15;

		/* written so that NaN fails the test as well */
		if (!(number > -largest_exact && number < largest_exact)) {
			put("null");
			return;
		}

		std::uint64_t scale = 1;

		for (unsigned digit = 0; digit < decimals; digit++) {
			scale *= 10;
		}

		const bool negative = number < 0;
		const double magnitude = negative ? -number : number;
		const auto scaled =
			static_cast<std::uint64_t>(magnitude * static_cast<double>(scale) + 0.5);

		if (negative && scaled != 0) {
			put('-');
		}
		put_unsigned(scaled / scale);
		if (decimals > 0) {
			put('.');
			for (std::uint64_t place = scale / 10; place > 0; place /= 10) {
				put(static_cast<char>('0' + (scaled / place) % 10));
			}
		}
	}

	std::span<char> buffer_;
	std::size_t length_{};
	bool container_is_empty_{true}; /**< nothing written yet since the last opening bracket */
	bool after_key_{};
	bool overflowed_{};
};

/** @brief Why a member of a request could not be read. */
enum class Error : std::uint8_t {
	Missing,    /**< the object has no such member */
	WrongType,  /**< for example a number where a string is expected */
	OutOfRange, /**< a number that does not fit the requested type */
	TooLong,    /**< a string that does not fit the requested buffer */
};

/** @brief A fixed-capacity string type such as StaticString. */
template <typename Type>
concept TextBuffer = requires(Type text, char character) {
	{ Type::capacity() } -> std::convertible_to<std::size_t>;
	{ text.push_back(character) } -> std::same_as<bool>;
};

/**
 * @brief One member of an Object.
 *
 * Converts to std::expected<Type, Error> where Type is bool, an integer
 * type or a fixed-capacity string:
 *
 * @code
 *     const std::expected<std::uint16_t, Error> port = object["port"];
 *     const auto host = object["host"].as<StaticString<64>>();
 * @endcode
 */
class Member {
public:
	/** @brief A member that does not exist; every conversion yields Error::Missing. */
	constexpr Member() noexcept = default;

	/**
	 * @brief A member that was found.
	 * @param token  The value as written: a string's contents without the
	 *               quotes, or a bare literal such as `42` or `true`.
	 * @param quoted Whether the value was a string.
	 */
	constexpr Member(std::string_view token, bool quoted) noexcept
		: token_{token}, quoted_{quoted}, present_{true}
	{
	}

	/** @brief Whether the object has this member. */
	[[nodiscard]] constexpr bool present() const noexcept { return present_; }

	/**
	 * @brief Read the member as a given type.
	 * @tparam Type bool, an integer type, or a TextBuffer.
	 * @return The value, or why it cannot be read as that type.
	 */
	template <typename Type>
	[[nodiscard]] constexpr std::expected<Type, Error> as() const noexcept
	{
		if (!present_) {
			return std::unexpected{Error::Missing};
		}

		if constexpr (std::same_as<Type, bool>) {
			return as_boolean();
		} else if constexpr (std::integral<Type>) {
			return as_integer<Type>();
		} else if constexpr (TextBuffer<Type>) {
			return as_text<Type>();
		} else {
			static_assert(false, "no JSON conversion for this type");
		}
	}

	/** @brief Implicit form of as(), selected by the type being initialised. */
	template <typename Type>
	constexpr operator std::expected<Type, Error>() const noexcept
	{
		return as<Type>();
	}

private:
	[[nodiscard]] constexpr std::expected<bool, Error> as_boolean() const noexcept
	{
		if (!quoted_ && token_ == "true") {
			return true;
		}
		if (!quoted_ && token_ == "false") {
			return false;
		}
		return std::unexpected{Error::WrongType};
	}

	template <std::integral Integer>
	[[nodiscard]] constexpr std::expected<Integer, Error> as_integer() const noexcept
	{
		/* more digits than this cannot be accumulated in 64 bits without overflow */
		constexpr std::size_t max_digits = 18;

		if (quoted_ || token_.empty()) {
			return std::unexpected{Error::WrongType};
		}

		const bool negative = token_.front() == '-';
		const auto digits = token_.substr(negative ? 1 : 0);
		std::uint64_t magnitude = 0;

		if (digits.empty()) {
			return std::unexpected{Error::WrongType};
		}
		if (digits.size() > max_digits) {
			return std::unexpected{Error::OutOfRange};
		}
		for (const char digit : digits) {
			if (digit < '0' || digit > '9') {
				return std::unexpected{Error::WrongType};
			}
			magnitude = magnitude * 10 + static_cast<std::uint64_t>(digit - '0');
		}

		const auto number = negative ? -static_cast<std::int64_t>(magnitude)
					     : static_cast<std::int64_t>(magnitude);

		if (!std::in_range<Integer>(number)) {
			return std::unexpected{Error::OutOfRange};
		}
		return static_cast<Integer>(number);
	}

	/* Copies the string, resolving the common escape sequences. */
	template <TextBuffer Text>
	[[nodiscard]] constexpr std::expected<Text, Error> as_text() const noexcept
	{
		if (!quoted_) {
			return std::unexpected{Error::WrongType};
		}

		Text text;

		for (std::size_t position = 0; position < token_.size(); position++) {
			char character = token_[position];

			if (character == '\\' && position + 1 < token_.size()) {
				character = token_[++position];
				switch (character) {
				case 'n':
					character = '\n';
					break;
				case 't':
					character = '\t';
					break;
				case 'u': {
					/* \uXXXX; only the Latin-1 range is representable here */
					unsigned code_point = 0;

					for (int digit = 0;
					     digit < 4 && position + 1 < token_.size(); digit++) {
						const int nibble = hex_value(token_[++position]);

						code_point = (code_point << 4) |
							     static_cast<unsigned>(nibble & 0x0f);
					}
					character = static_cast<char>(code_point & 0xff);
					break;
				}
				default:
					break; /* \" \\ \/ stand for the character itself */
				}
			}
			if (!text.push_back(character)) {
				return std::unexpected{Error::TooLong};
			}
		}
		return text;
	}

	std::string_view token_{};
	bool quoted_{};
	bool present_{};
};

/**
 * @brief Read access to a flat JSON object.
 *
 * Looks up top-level members of an object such as
 * `{"ssid":"home","port":1883,"enabled":true}`. Nested objects and arrays
 * are not supported, which is all the API needs.
 */
class Object {
public:
	/**
	 * @brief Wrap JSON text.
	 * @param text The object; must outlive this instance and its members.
	 */
	constexpr explicit Object(std::string_view text) noexcept : text_{text} {}

	/**
	 * @brief Look up a member.
	 * @param name The member's name.
	 * @return The member; Member::present() tells whether it exists.
	 */
	[[nodiscard]] constexpr Member operator[](std::string_view name) const noexcept
	{
		std::size_t position = 0;

		while (position < text_.size()) {
			/* the next string at this level is a member name */
			const auto name_start = text_.find('"', position);

			if (name_start == std::string_view::npos) {
				break;
			}

			const auto name_end = closing_quote(name_start + 1);

			if (name_end == std::string_view::npos) {
				break;
			}

			const auto found_name =
				text_.substr(name_start + 1, name_end - name_start - 1);
			auto value_start = skip_whitespace(name_end + 1);

			if (value_start >= text_.size() || text_[value_start] != ':') {
				position = name_end + 1;
				continue;
			}
			value_start = skip_whitespace(value_start + 1);
			if (value_start >= text_.size()) {
				break;
			}

			std::string_view token;
			bool quoted = false;

			if (text_[value_start] == '"') {
				const auto value_end = closing_quote(value_start + 1);

				if (value_end == std::string_view::npos) {
					break;
				}
				token = text_.substr(value_start + 1, value_end - value_start - 1);
				quoted = true;
				position = value_end + 1;
			} else {
				const auto value_end =
					text_.find_first_of(",}] \r\n\t", value_start);

				token = text_.substr(value_start,
						     value_end == std::string_view::npos
							     ? std::string_view::npos
							     : value_end - value_start);
				position = value_start + token.size();
			}

			if (found_name == name) {
				return Member{token, quoted};
			}
		}

		return Member{};
	}

private:
	[[nodiscard]] constexpr std::size_t skip_whitespace(std::size_t position) const noexcept
	{
		constexpr std::string_view whitespace = " \t\r\n";

		while (position < text_.size() && whitespace.contains(text_[position])) {
			position++;
		}
		return position;
	}

	/* Position of the quote that ends a string whose contents start at `position`. */
	[[nodiscard]] constexpr std::size_t closing_quote(std::size_t position) const noexcept
	{
		for (; position < text_.size(); position++) {
			if (text_[position] == '\\') {
				position++; /* the escaped character cannot end the string */
			} else if (text_[position] == '"') {
				return position;
			}
		}
		return std::string_view::npos;
	}

	std::string_view text_;
};

/* --- compile-time checks -------------------------------------------------- */

static_assert([] {
	char buffer[64]{};
	Writer out{buffer};

	out << begin_object << key("a") << 1 << key("b") << begin_array << true << null
	    << fixed(-2.5, 1) << end_array << key("c") << "x\"y" << end_object;
	return out.ok() && out.view() == R"({"a":1,"b":[true,null,-2.5],"c":"x\"y"})";
}());

static_assert([] {
	constexpr Object object{R"({"ssid":"home net", "port": 1883,"on":true,"neg":-7})"};
	const std::expected<std::uint16_t, Error> port = object["port"];
	const std::expected<bool, Error> enabled = object["on"];
	const auto ssid = object["ssid"].as<StaticString<16>>();
	const auto too_small = object["ssid"].as<StaticString<4>>();
	const auto too_narrow = object["port"].as<std::uint8_t>();
	const auto negative = object["neg"].as<int>();

	return port == 1883 && enabled == true && ssid && ssid->view() == "home net" &&
	       too_small.error() == Error::TooLong && too_narrow.error() == Error::OutOfRange &&
	       negative == -7 && object["nope"].as<int>().error() == Error::Missing &&
	       object["ssid"].as<int>().error() == Error::WrongType;
}());

} /* namespace s0::json */
