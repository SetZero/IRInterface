/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "meter/iec62056.hpp"

#include <array>
#include <charconv>

namespace s0 {

namespace {

constexpr char start_of_text = 0x02;
constexpr char end_of_text = 0x03;
constexpr char delete_character = 0x7f;

/* Turns the number in front of a unit symbol into a quantity in its base unit. */
template <typename Unit>
Content in_unit(double number) noexcept
{
	return typename Unit::base{Unit{number}};
}

struct UnitSymbol {
	std::string_view symbol;
	Content (*make)(double number) noexcept;
};

constexpr UnitSymbol unit_symbols[] = {
	{"Wh", in_unit<units::WattHours>},
	{"kWh", in_unit<units::KilowattHours>},
	{"MWh", in_unit<units::MegawattHours>},
	{"W", in_unit<units::Watts>},
	{"kW", in_unit<units::Kilowatts>},
	{"MW", in_unit<units::Megawatts>},
	{"V", in_unit<units::Volts>},
	{"kV", in_unit<units::Kilovolts>},
	{"A", in_unit<units::Amperes>},
	{"mA", in_unit<units::Milliamperes>},
	{"Hz", in_unit<units::Hertz>},
	{"var", in_unit<units::Vars>},
	{"kvar", in_unit<units::Kilovars>},
	{"varh", in_unit<units::VarHours>},
	{"kvarh", in_unit<units::KilovarHours>},
	{"VA", in_unit<units::VoltAmperes>},
	{"kVA", in_unit<units::KilovoltAmperes>},
};

/* Decimal number with optional sign and fraction; nothing if not numeric. */
std::optional<double> parse_number(std::string_view text) noexcept
{
	bool negative = false;
	bool has_digits = false;
	bool in_fraction = false;
	double value = 0.0;
	double place = 0.1;

	if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
		negative = text.front() == '-';
		text.remove_prefix(1);
	}

	for (const char character : text) {
		if (character >= '0' && character <= '9') {
			const int digit = character - '0';

			has_digits = true;
			if (in_fraction) {
				value += digit * place;
				place /= 10.0;
			} else {
				value = value * 10.0 + digit;
			}
		} else if (character == '.' && !in_fraction) {
			in_fraction = true;
		} else {
			return std::nullopt;
		}
	}

	if (!has_digits) {
		return std::nullopt;
	}
	return negative ? -value : value;
}

/* One OBIS value group: a decimal number, or one of the letter codes. */
std::optional<std::uint8_t> parse_group(std::string_view text) noexcept
{
	if (text.size() == 1) {
		switch (text.front()) {
		case 'C':
			return 96;
		case 'F':
			return 97;
		case 'L':
			return 98;
		case 'P':
			return 99;
		default:
			break;
		}
	}

	unsigned value = 0;
	const char *const last = text.data() + text.size();
	const auto [end, error] = std::from_chars(text.data(), last, value);

	if (error != std::errc{} || end != last || value > 255) {
		return std::nullopt;
	}
	return static_cast<std::uint8_t>(value);
}

/* Split off the part of `text` before `separator`; nothing if it is absent. */
std::optional<std::string_view> split(std::string_view &text, char separator) noexcept
{
	const auto position = text.find(separator);

	if (position == std::string_view::npos) {
		return std::nullopt;
	}

	const auto head = text.substr(0, position);

	text.remove_prefix(position + 1);
	return head;
}

} /* namespace */

std::expected<Obis, ObisError> IecDecoder::parse_obis(std::string_view identifier) noexcept
{
	Obis code;

	/* optional "medium-channel:" prefix */
	if (const auto prefix = split(identifier, ':')) {
		auto channel_text = *prefix;
		const auto medium_text = split(channel_text, '-');

		if (!medium_text) {
			return std::unexpected{ObisError::BadPrefix};
		}

		const auto medium = parse_group(*medium_text);
		const auto channel = parse_group(channel_text);

		if (!medium || !channel) {
			return std::unexpected{ObisError::BadPrefix};
		}
		code.medium = *medium;
		code.channel = *channel;
	}

	/* optional "*period" (or "&period") suffix */
	if (const auto suffix = identifier.find_first_of("*&"); suffix != std::string_view::npos) {
		const auto period = parse_group(identifier.substr(suffix + 1));

		if (!period) {
			return std::unexpected{ObisError::BadGroup};
		}
		code.period = *period;
		identifier = identifier.substr(0, suffix);
	}

	/* "quantity.processing.tariff", of which the tariff may be left out */
	const std::array groups{&code.quantity, &code.processing, &code.tariff};
	std::size_t count = 0;

	while (!identifier.empty()) {
		if (count == groups.size()) {
			return std::unexpected{ObisError::TooManyGroups};
		}

		const auto dot = identifier.find('.');
		const auto group = parse_group(identifier.substr(0, dot));

		if (!group) {
			return std::unexpected{ObisError::BadGroup};
		}
		*groups[count++] = *group;
		identifier = dot == std::string_view::npos ? std::string_view{}
							   : identifier.substr(dot + 1);
	}

	if (count < 2) {
		return std::unexpected{ObisError::TooFewGroups};
	}
	return code;
}

void IecDecoder::reset() noexcept
{
	line_.clear();
	line_unusable_ = false;
	in_telegram_ = false;
	have_identification_ = false;
	work_.values.clear();
}

std::optional<std::string_view> IecDecoder::take_identification() noexcept
{
	if (!have_identification_) {
		return std::nullopt;
	}
	have_identification_ = false;
	return identification_.view();
}

/* "1-0:1.8.0*255(001234.567*kWh)": identifier, then value and unit in parentheses. */
void IecDecoder::data_line(std::string_view line) noexcept
{
	auto rest = line;
	const auto identifier = split(rest, '(');

	if (!identifier) {
		return;
	}

	const auto code = parse_obis(*identifier);
	const auto closing = rest.find(')');

	if (!code || closing == std::string_view::npos) {
		return;
	}

	auto number_text = rest.substr(0, closing);
	std::string_view unit_text;

	if (const auto star = number_text.find('*'); star != std::string_view::npos) {
		unit_text = number_text.substr(star + 1);
		number_text = number_text.substr(0, star);
	}

	const auto key = code->key();
	const bool identifies_meter =
		key == obis::serial_number || key == obis::device_address || key == obis::device_id;
	const auto number = parse_number(number_text);
	Value value{.obis = *code, .content = {}};

	if (identifies_meter || !number) {
		/* identification numbers look numeric but are names, not quantities */
		value.content = Text{number_text};
		if (identifies_meter && !number_text.empty()) {
			work_.meter_id.assign(number_text);
		}
	} else {
		value.content = units::Number{*number};
		for (const auto &unit : unit_symbols) {
			if (unit.symbol == unit_text) {
				value.content = unit.make(*number);
				break;
			}
		}
	}

	work_.values.push_back(value);
}

bool IecDecoder::end_of_line() noexcept
{
	const std::string_view line = line_.view();
	bool complete = false;

	if (line_unusable_ || line.empty()) {
		/* nothing to interpret */
	} else if (line.front() == '/' && line.size() >= 5) {
		/* identification: "/" manufacturer(3) baud(1) identification */
		in_telegram_ = true;
		work_.values.clear();
		work_.protocol = Protocol::Iec62056;
		work_.meter_id.assign(line.substr(1));
		identification_.assign(line);
		have_identification_ = true;
	} else if (line.front() == '!') {
		if (in_telegram_ && !work_.values.empty()) {
			reading_ = work_;
			complete = true;
		}
		in_telegram_ = false;
	} else if (in_telegram_) {
		data_line(line);
	}

	line_.clear();
	line_unusable_ = false;
	return complete;
}

bool IecDecoder::push(std::uint8_t byte) noexcept
{
	const char character = static_cast<char>(byte & 0x7f);

	if (character == '\n') {
		return end_of_line();
	}
	if (character == '\r' || character == start_of_text || character == end_of_text) {
		/* CR, and the STX/ETX that frame the data block in mode C */
		return false;
	}
	if (character < ' ' || character == delete_character) {
		/* binary data, certainly not a text telegram */
		line_unusable_ = true;
		return false;
	}
	if (!line_.push_back(character)) {
		line_unusable_ = true;
	}
	return false;
}

} /* namespace s0 */
