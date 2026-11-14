/**
 * @file
 * @brief Meter data model shared by the decoders, the web API and MQTT.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>

#include "core/units.hpp"
#include "core/util.hpp"

namespace s0 {

/**
 * @brief The three OBIS value groups that say what a value is, independent of the
 * medium and channel it was measured on: "16.7.0" in "1-0:16.7.0*255".
 */
struct ObisKey {
	std::uint8_t quantity{};   /**< group C: what is measured, 16 = total active power */
	std::uint8_t processing{}; /**< group D: how, 7 = instantaneous, 8 = time integral */
	std::uint8_t tariff{};     /**< group E: 0 = total, 1.. = tariff register */

	friend constexpr bool operator==(const ObisKey &, const ObisKey &) = default;
};

/** @brief Complete OBIS identifier, written "medium-channel:quantity.processing.tariff*period". */
struct Obis {
	std::uint8_t medium{1};    /**< group A: 1 = electricity */
	std::uint8_t channel{0};   /**< group B */
	std::uint8_t quantity{};   /**< group C */
	std::uint8_t processing{}; /**< group D */
	std::uint8_t tariff{};     /**< group E */
	std::uint8_t period{255};  /**< group F: billing period, 255 = current value */

	friend constexpr auto operator<=>(const Obis &, const Obis &) = default;

	[[nodiscard]] constexpr ObisKey key() const noexcept
	{
		return {quantity, processing, tariff};
	}
};

/** @brief The OBIS keys this firmware gives a meaning to. */
namespace obis {

inline constexpr ObisKey energy_import{1, 8, 0};
inline constexpr ObisKey energy_import_t1{1, 8, 1};
inline constexpr ObisKey energy_import_t2{1, 8, 2};
inline constexpr ObisKey energy_export{2, 8, 0};
inline constexpr ObisKey energy_export_t1{2, 8, 1};
inline constexpr ObisKey energy_export_t2{2, 8, 2};
inline constexpr ObisKey power_total{16, 7, 0};
inline constexpr ObisKey power_import{1, 7, 0};
inline constexpr ObisKey power_export{2, 7, 0};
inline constexpr ObisKey power_l1{36, 7, 0};
inline constexpr ObisKey power_l2{56, 7, 0};
inline constexpr ObisKey power_l3{76, 7, 0};
inline constexpr ObisKey voltage_l1{32, 7, 0};
inline constexpr ObisKey voltage_l2{52, 7, 0};
inline constexpr ObisKey voltage_l3{72, 7, 0};
inline constexpr ObisKey current_l1{31, 7, 0};
inline constexpr ObisKey current_l2{51, 7, 0};
inline constexpr ObisKey current_l3{71, 7, 0};
inline constexpr ObisKey frequency{14, 7, 0};

/** @brief identification, reported as text */
inline constexpr ObisKey device_address{0, 0, 0};
inline constexpr ObisKey device_id{0, 0, 9};
inline constexpr ObisKey serial_number{96, 1, 0};

} /* namespace obis */

using Text = StaticString<32>;

/**
 * @brief What a telegram can carry for one OBIS code: a quantity in its base unit,
 * or text such as a serial number.
 */
using Content = std::variant<units::Number, units::Watts, units::WattHours, units::Volts,
			     units::Amperes, units::Hertz, units::Vars, units::VarHours,
			     units::VoltAmperes, units::Degrees, Text>;

struct Value {
	Obis obis{};
	Content content{};

	[[nodiscard]] constexpr const Text *text() const noexcept
	{
		return std::get_if<Text>(&content);
	}
	[[nodiscard]] constexpr bool is_text() const noexcept { return text() != nullptr; }

	/** @brief Magnitude in the base unit; 0 for text. */
	[[nodiscard]] constexpr double number() const noexcept
	{
		return std::visit(
			[](const auto &held) {
				if constexpr (std::is_same_v<std::remove_cvref_t<decltype(held)>,
							     Text>) {
					return 0.0;
				} else {
					return held.count();
				}
			},
			content);
	}

	/** @brief Symbol of the base unit, empty for text and plain numbers. */
	[[nodiscard]] constexpr std::string_view unit() const noexcept
	{
		return std::visit(
			[](const auto &held) -> std::string_view {
				if constexpr (std::is_same_v<std::remove_cvref_t<decltype(held)>,
							     Text>) {
					return "";
				} else {
					return std::remove_cvref_t<decltype(held)>::symbol;
				}
			},
			content);
	}
};

enum class Protocol : std::uint8_t {
	Unknown,
	Sml,
	Iec62056,
};

[[nodiscard]] constexpr std::string_view protocol_name(Protocol protocol) noexcept
{
	switch (protocol) {
	case Protocol::Sml:
		return "sml";
	case Protocol::Iec62056:
		return "iec62056-21";
	case Protocol::Unknown:
		break;
	}
	return "unknown";
}

inline constexpr std::size_t max_values = 40;

/** @brief One decoded telegram. */
struct Reading {
	Protocol protocol{Protocol::Unknown};
	std::uint32_t sequence{}; /**< counts telegrams since boot */
	std::int64_t uptime_ms{}; /**< when it was decoded */
	StaticString<40> meter_id{};
	StaticVector<Value, max_values> values{};

	/** @brief The value stored under `key`, if the telegram has it as a `Quantity`. */
	template <typename Quantity>
	[[nodiscard]] constexpr std::optional<Quantity> get(ObisKey key) const noexcept
	{
		for (const auto &value : values) {
			if (value.obis.key() != key) {
				continue;
			}
			if (const auto *quantity = std::get_if<Quantity>(&value.content)) {
				return *quantity;
			}
		}
		return std::nullopt;
	}

	/** @brief Net active power, derived from whatever the meter provides. */
	[[nodiscard]] constexpr std::optional<units::Watts> power() const noexcept
	{
		using units::Watts;

		if (const auto total = get<Watts>(obis::power_total)) {
			return total;
		}

		const auto imported = get<Watts>(obis::power_import);
		const auto exported = get<Watts>(obis::power_export);

		if (imported || exported) {
			return imported.value_or(Watts{}) - exported.value_or(Watts{});
		}

		const auto phase1 = get<Watts>(obis::power_l1);
		const auto phase2 = get<Watts>(obis::power_l2);
		const auto phase3 = get<Watts>(obis::power_l3);

		if (phase1 || phase2 || phase3) {
			return phase1.value_or(Watts{}) + phase2.value_or(Watts{}) +
			       phase3.value_or(Watts{});
		}
		return std::nullopt;
	}
};

struct KnownQuantity {
	ObisKey obis;
	std::string_view key;   /**< stable identifier for JSON and MQTT */
	std::string_view label; /**< human readable name */
};

/** @brief Quantities with a well-known meaning; everything else is passed through. */
inline constexpr KnownQuantity known_quantities[] = {
	{obis::energy_import, "energy_import", "Energy import"},
	{obis::energy_import_t1, "energy_import_t1", "Energy import T1"},
	{obis::energy_import_t2, "energy_import_t2", "Energy import T2"},
	{obis::energy_export, "energy_export", "Energy export"},
	{obis::energy_export_t1, "energy_export_t1", "Energy export T1"},
	{obis::energy_export_t2, "energy_export_t2", "Energy export T2"},
	{obis::power_total, "power", "Power"},
	{obis::power_import, "power_import", "Power import"},
	{obis::power_export, "power_export", "Power export"},
	{obis::power_l1, "power_l1", "Power L1"},
	{obis::power_l2, "power_l2", "Power L2"},
	{obis::power_l3, "power_l3", "Power L3"},
	{obis::voltage_l1, "voltage_l1", "Voltage L1"},
	{obis::voltage_l2, "voltage_l2", "Voltage L2"},
	{obis::voltage_l3, "voltage_l3", "Voltage L3"},
	{obis::current_l1, "current_l1", "Current L1"},
	{obis::current_l2, "current_l2", "Current L2"},
	{obis::current_l3, "current_l3", "Current L3"},
	{obis::frequency, "frequency", "Frequency"},
};

[[nodiscard]] constexpr const KnownQuantity *known_quantity(const Obis &code) noexcept
{
	for (const auto &known : known_quantities) {
		if (code.key() == known.obis) {
			return &known;
		}
	}
	return nullptr;
}

} /* namespace s0 */
