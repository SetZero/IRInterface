/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "meter/sml.hpp"

#include <algorithm>

namespace s0 {

namespace {

constexpr std::uint64_t start_sequence = 0x1b1b1b1b01010101ULL;
constexpr std::uint8_t escape_byte = 0x1b;
constexpr std::uint8_t end_marker = 0x1a;
constexpr std::uint8_t start_byte = 0x01;
constexpr std::size_t max_fill_bytes = 3;
constexpr unsigned max_list_depth = 8;

constexpr std::uint16_t get_list_response = 0x0701;

template <typename T>
using Parsed = std::expected<T, SmlError>;

/* Leave the enclosing function with the error of a failed step. */
#define S0_TRY(_step)                                                                              \
	do {                                                                                       \
		if (const auto step_ = (_step); !step_) {                                          \
			return std::unexpected{step_.error()};                                     \
		}                                                                                  \
	} while (false)

enum class Kind : std::uint8_t {
	OctetString,
	Boolean,
	Integer,
	UnsignedInteger,
	List,
	EndOfMessage,
};

/* The type-length field in front of every SML element. */
struct TypeLength {
	Kind kind{};
	std::size_t length{}; /* data bytes, or the number of elements for a list */
};

/* Bounds-checked reader over the SML type-length-value encoding. */
class Cursor {
public:
	explicit Cursor(std::span<const std::uint8_t> data) noexcept : data_{data} {}

	[[nodiscard]] bool at_end() const noexcept { return position_ >= data_.size(); }
	[[nodiscard]] std::uint8_t peek() const noexcept { return data_[position_]; }
	void advance() noexcept { position_++; }

	Parsed<TypeLength> read_type_length() noexcept
	{
		if (at_end()) {
			return std::unexpected{SmlError::Truncated};
		}

		std::uint8_t byte = data_[position_++];

		if (byte == 0x00) {
			return TypeLength{Kind::EndOfMessage, 0};
		}

		const unsigned type = (byte >> 4) & 0x07;
		std::size_t length = byte & 0x0f;
		std::size_t header_bytes = 1;

		/* the top bit announces another length nibble in the next byte */
		while (byte & 0x80) {
			if (at_end()) {
				return std::unexpected{SmlError::Truncated};
			}
			if (header_bytes >= 4) {
				return std::unexpected{SmlError::BadLength};
			}
			byte = data_[position_++];
			length = (length << 4) | (byte & 0x0f);
			header_bytes++;
		}

		Kind kind;

		switch (type) {
		case 0:
			kind = Kind::OctetString;
			break;
		case 4:
			kind = Kind::Boolean;
			break;
		case 5:
			kind = Kind::Integer;
			break;
		case 6:
			kind = Kind::UnsignedInteger;
			break;
		case 7:
			return TypeLength{Kind::List, length};
		default:
			return std::unexpected{SmlError::BadType};
		}

		/* for everything but lists the length includes the header itself */
		if (length < header_bytes) {
			return std::unexpected{SmlError::BadLength};
		}
		if (position_ + (length - header_bytes) > data_.size()) {
			return std::unexpected{SmlError::Truncated};
		}
		return TypeLength{kind, length - header_bytes};
	}

	/* The data bytes of a non-list element just described by read_type_length(). */
	std::span<const std::uint8_t> take(std::size_t length) noexcept
	{
		const auto bytes = data_.subspan(position_, length);

		position_ += length;
		return bytes;
	}

	/* Step over one element, including everything nested in it. */
	Parsed<void> skip(unsigned depth = 0) noexcept
	{
		const auto element = read_type_length();

		if (!element) {
			return std::unexpected{element.error()};
		}
		if (element->kind != Kind::List) {
			position_ += element->length;
			return {};
		}
		if (depth >= max_list_depth) {
			return std::unexpected{SmlError::TooDeep};
		}
		for (std::size_t index = 0; index < element->length; index++) {
			S0_TRY(skip(depth + 1));
		}
		return {};
	}

	Parsed<void> skip(std::size_t count, unsigned depth) noexcept
	{
		for (std::size_t index = 0; index < count; index++) {
			S0_TRY(skip(depth));
		}
		return {};
	}

	/* Expect a list with exactly `count` elements and step into it. */
	Parsed<void> enter_list(std::size_t count) noexcept
	{
		const auto element = read_type_length();

		if (!element) {
			return std::unexpected{element.error()};
		}
		if (element->kind != Kind::List || element->length != count) {
			return std::unexpected{SmlError::Unexpected};
		}
		return {};
	}

private:
	std::span<const std::uint8_t> data_;
	std::size_t position_{};
};

std::uint64_t read_unsigned(std::span<const std::uint8_t> bytes) noexcept
{
	std::uint64_t value = 0;

	for (const auto byte : bytes) {
		value = (value << 8) | byte;
	}
	return value;
}

std::int64_t read_signed(std::span<const std::uint8_t> bytes) noexcept
{
	if (bytes.empty()) {
		return 0;
	}

	/* sign-extend from the most significant transmitted byte */
	std::uint64_t value = (bytes.front() & 0x80) ? ~0ULL : 0ULL;

	for (const auto byte : bytes) {
		value = (value << 8) | byte;
	}
	return static_cast<std::int64_t>(value);
}

/* Apply the decimal scaler that accompanies every SML value. */
double scale(double value, int power_of_ten) noexcept
{
	for (; power_of_ten > 0; power_of_ten--) {
		value *= 10.0;
	}
	for (; power_of_ten < 0; power_of_ten++) {
		value /= 10.0;
	}
	return value;
}

/* Attach the unit named by a DLMS unit code to a number. */
Content with_unit(std::uint64_t dlms_unit, double number) noexcept
{
	switch (dlms_unit) {
	case 8:
		return units::Degrees{number};
	case 27:
		return units::Watts{number};
	case 28:
		return units::VoltAmperes{number};
	case 29:
		return units::Vars{number};
	case 30:
		return units::WattHours{number};
	case 32:
		return units::VarHours{number};
	case 33:
		return units::Amperes{number};
	case 35:
		return units::Volts{number};
	case 44:
		return units::Hertz{number};
	default:
		return units::Number{number};
	}
}

/* Printable ASCII is kept as text, anything else is shown as hex. */
template <std::size_t Capacity>
StaticString<Capacity> octets_to_text(std::span<const std::uint8_t> bytes) noexcept
{
	const bool printable = std::ranges::all_of(
		bytes, [](std::uint8_t byte) { return byte >= 0x20 && byte < 0x7f; });
	StaticString<Capacity> text;

	for (const auto byte : bytes) {
		if (printable) {
			text.push_back(static_cast<char>(byte));
		} else {
			text.push_back(hex_digit(byte >> 4));
			text.push_back(hex_digit(byte));
		}
	}
	return text;
}

/*
 * SML_ListEntry: objName, status, valTime, unit, scaler, value, signature.
 * Yields nothing for entries that are valid but carry nothing to show.
 */
Parsed<std::optional<Value>> parse_entry(Cursor &cursor) noexcept
{
	S0_TRY(cursor.enter_list(7));

	const auto name = cursor.read_type_length();

	if (!name) {
		return std::unexpected{name.error()};
	}
	if (name->kind != Kind::OctetString || name->length != 6) {
		return std::unexpected{SmlError::Unexpected};
	}

	const auto code = cursor.take(6);
	Value value{.obis = {code[0], code[1], code[2], code[3], code[4], code[5]}, .content = {}};

	S0_TRY(cursor.skip(2, 1)); /* status, valTime */

	const auto unit = cursor.read_type_length();

	if (!unit) {
		return std::unexpected{unit.error()};
	}
	if (unit->kind == Kind::List) {
		return std::unexpected{SmlError::Unexpected};
	}

	/* both unit and scaler are optional; an absent one is an empty string */
	const auto unit_bytes = cursor.take(unit->length);
	const std::uint64_t dlms_unit =
		unit->kind == Kind::UnsignedInteger ? read_unsigned(unit_bytes) : 0;

	const auto scaler = cursor.read_type_length();

	if (!scaler) {
		return std::unexpected{scaler.error()};
	}
	if (scaler->kind == Kind::List) {
		return std::unexpected{SmlError::Unexpected};
	}

	const auto scaler_bytes = cursor.take(scaler->length);
	const int power_of_ten =
		scaler->kind == Kind::Integer ? static_cast<int>(read_signed(scaler_bytes)) : 0;

	const auto data = cursor.read_type_length();

	if (!data) {
		return std::unexpected{data.error()};
	}

	bool usable = true;

	switch (data->kind) {
	case Kind::Integer:
		value.content =
			with_unit(dlms_unit,
				  scale(static_cast<double>(read_signed(cursor.take(data->length))),
					power_of_ten));
		break;
	case Kind::UnsignedInteger:
		value.content = with_unit(
			dlms_unit,
			scale(static_cast<double>(read_unsigned(cursor.take(data->length))),
			      power_of_ten));
		break;
	case Kind::Boolean:
		value.content =
			units::Number{read_unsigned(cursor.take(data->length)) != 0 ? 1.0 : 0.0};
		break;
	case Kind::OctetString: {
		const auto text = octets_to_text<Text::capacity()>(cursor.take(data->length));

		usable = !text.empty();
		value.content = text;
		break;
	}
	case Kind::List:
	case Kind::EndOfMessage:
		/* legal in SML, but never used for a register value */
		return std::unexpected{SmlError::Unexpected};
	}

	S0_TRY(cursor.skip()); /* valueSignature */

	return usable ? std::optional{value} : std::nullopt;
}

/*
 * SML_GetList.Res: clientId, serverId, listName, actSensorTime, valList,
 * listSignature, actGatewayTime
 */
Parsed<void> parse_get_list_response(Cursor &cursor, Reading &reading) noexcept
{
	S0_TRY(cursor.enter_list(7));
	S0_TRY(cursor.skip()); /* clientId */

	const auto server_id = cursor.read_type_length();

	if (!server_id) {
		return std::unexpected{server_id.error()};
	}
	if (server_id->kind != Kind::OctetString) {
		return std::unexpected{SmlError::Unexpected};
	}
	reading.meter_id = octets_to_text<decltype(reading.meter_id)::capacity()>(
		cursor.take(server_id->length));

	S0_TRY(cursor.skip(2, 1)); /* listName, actSensorTime */

	const auto list = cursor.read_type_length();

	if (!list) {
		return std::unexpected{list.error()};
	}
	if (list->kind != Kind::List) {
		return std::unexpected{SmlError::Unexpected};
	}

	for (std::size_t index = 0; index < list->length; index++) {
		const auto entry = parse_entry(cursor);

		if (!entry) {
			return std::unexpected{entry.error()};
		}
		if (*entry) {
			reading.values.push_back(**entry);
		}
	}

	return cursor.skip(2, 1); /* listSignature, actGatewayTime */
}

/* SML_Message: transactionId, groupNo, abortOnError, messageBody, crc16, endOfMessage */
Parsed<void> parse_message(Cursor &cursor, Reading &reading) noexcept
{
	S0_TRY(cursor.enter_list(6));
	S0_TRY(cursor.skip(3, 1)); /* transactionId, groupNo, abortOnError */
	S0_TRY(cursor.enter_list(2));

	const auto tag = cursor.read_type_length();

	if (!tag) {
		return std::unexpected{tag.error()};
	}
	if (tag->kind != Kind::UnsignedInteger) {
		return std::unexpected{SmlError::Unexpected};
	}

	if (read_unsigned(cursor.take(tag->length)) == get_list_response) {
		S0_TRY(parse_get_list_response(cursor, reading));
	} else {
		S0_TRY(cursor.skip()); /* open/close and other responses */
	}

	/* The message CRC is not checked: the transport CRC covers the whole file. */
	return cursor.skip();
}

#undef S0_TRY

} /* namespace */

std::expected<Reading, SmlError> SmlDecoder::parse(std::span<const std::uint8_t> body) noexcept
{
	Cursor cursor{body};
	Reading reading;

	reading.protocol = Protocol::Sml;

	while (!cursor.at_end()) {
		if (cursor.peek() == 0x00) {
			/* end-of-message marker or fill byte */
			cursor.advance();
			continue;
		}
		if (const auto message = parse_message(cursor, reading); !message) {
			return std::unexpected{message.error()};
		}
	}

	if (reading.values.empty()) {
		return std::unexpected{SmlError::NoValues};
	}
	return reading;
}

void SmlDecoder::begin_file() noexcept
{
	state_ = State::Data;
	body_len_ = 0;
	escape_run_ = 0;
	crc_ = irlink::Crc16X25{};
	for (int shift = 56; shift >= 0; shift -= 8) {
		crc_.update(static_cast<std::uint8_t>(start_sequence >> shift));
	}
}

void SmlDecoder::reject(SmlError error) noexcept
{
	errors_++;
	last_error_ = error;
	state_ = State::Sync;
	window_ = 0;
}

/* The trailer is complete: end marker, fill byte count, CRC low, CRC high. */
bool SmlDecoder::finish() noexcept
{
	const std::size_t fill_bytes = trailer_[1];
	/* the checksum is sent low byte first; tolerate meters that swap it */
	const auto received = static_cast<std::uint16_t>(trailer_[2] | (trailer_[3] << 8));
	const auto swapped = static_cast<std::uint16_t>((received << 8) | (received >> 8));

	if (crc_before_checksum_ != received && crc_before_checksum_ != swapped) {
		reject(SmlError::Checksum);
		return false;
	}
	if (fill_bytes > max_fill_bytes || fill_bytes > body_len_) {
		reject(SmlError::Framing);
		return false;
	}

	const auto reading = parse(std::span{body_}.first(body_len_ - fill_bytes));

	if (!reading) {
		reject(reading.error());
		return false;
	}

	reading_ = *reading;
	state_ = State::Sync;
	window_ = 0;
	return true;
}

bool SmlDecoder::push(std::uint8_t byte) noexcept
{
	switch (state_) {
	case State::Sync:
		window_ = (window_ << 8) | byte;
		if (window_ == start_sequence) {
			begin_file();
		}
		return false;

	case State::Data:
		crc_.update(byte);
		if (body_len_ >= body_.size()) {
			reject(SmlError::Oversized);
			return false;
		}
		body_[body_len_++] = byte;
		escape_run_ = byte == escape_byte ? escape_run_ + 1 : 0;
		if (escape_run_ == 4) {
			/* the escape sequence itself is not part of the body */
			body_len_ -= 4;
			escape_run_ = 0;
			trailer_len_ = 0;
			state_ = State::Trailer;
		}
		return false;

	case State::Trailer:
		trailer_[trailer_len_++] = byte;

		if (trailer_[0] == end_marker) {
			/* the CRC covers everything up to the fill byte count */
			if (trailer_len_ <= 2) {
				crc_.update(byte);
			}
			if (trailer_len_ == 2) {
				crc_before_checksum_ = crc_.value();
			}
			return trailer_len_ == trailer_.size() ? finish() : false;
		}

		crc_.update(byte);
		if (trailer_len_ < trailer_.size()) {
			return false;
		}

		if (std::ranges::all_of(trailer_,
					[](std::uint8_t value) { return value == escape_byte; })) {
			/* an escaped literal 1b1b1b1b inside the data */
			if (body_len_ + trailer_.size() > body_.size()) {
				reject(SmlError::Oversized);
				return false;
			}
			body_len_ = static_cast<std::size_t>(
				std::ranges::copy(trailer_, body_.begin() + body_len_).out -
				body_.begin());
			state_ = State::Data;
			return false;
		}

		if (std::ranges::all_of(trailer_,
					[](std::uint8_t value) { return value == start_byte; })) {
			/* a new file starts: the previous one was cut short */
			errors_++;
			last_error_ = SmlError::Truncated;
			begin_file();
			return false;
		}

		reject(SmlError::Framing);
		return false;
	}

	return false;
}

} /* namespace s0 */
