/**
 * @file
 * @brief Physical quantities with compile-time units, modelled on std::chrono.
 *
 * A quantity is a dimension (power, energy, voltage ...) plus an order of
 * magnitude given as a std::ratio, exactly like std::chrono::duration is a
 * tick count plus a period:
 *
 *     using namespace s0::units;
 *
 *     Watts base = Kilowatts{1.5};            // converts: 1500 W
 *     auto sum = Kilowatts{1.5} + Watts{200}; // Watts{1700}, the finer unit
 *     bool more = Milliwatts{5} < Watts{1};   // compares across magnitudes
 *     double ratio = sum / Watts{850};        // 2.0, a plain number
 *     Volts mains = 230_V;
 *
 * Quantities of different dimensions do not mix: Watts{1} + Volts{1} does
 * not compile.
 *
 * Quantity is a CRTP base. A dimension derives from it, naming itself as
 * the template argument, and only adds the symbol of its base unit; all
 * conversion and arithmetic lives in the base.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <compare>
#include <cstdint>
#include <numeric>
#include <ratio>
#include <string_view>

namespace s0::units {

namespace detail {

template <typename T>
inline constexpr bool is_ratio = false;

template <std::intmax_t Num, std::intmax_t Den>
inline constexpr bool is_ratio<std::ratio<Num, Den>> = true;

/** @brief The coarsest magnitude both ratios convert to exactly (as in <chrono>). */
template <typename Ratio1, typename Ratio2>
using common_ratio =
	std::ratio<std::gcd(Ratio1::num, Ratio2::num), std::lcm(Ratio1::den, Ratio2::den)>;

/** @brief Multiplier that turns a count of `From` units into a count of `To` units. */
template <typename From, typename To>
inline constexpr double conversion = static_cast<double>(std::ratio_divide<From, To>::num) /
				     static_cast<double>(std::ratio_divide<From, To>::den);

} /* namespace detail */

/**
 * @brief CRTP base of every dimension. `Dimension` is the derived class template,
 * `Ratio` the size of one unit relative to the dimension's base unit.
 */
template <template <typename> class Dimension, typename Ratio>
class Quantity {
	static_assert(detail::is_ratio<Ratio>, "the magnitude must be a std::ratio");

public:
	using rep = double;
	using ratio = Ratio;
	using derived = Dimension<Ratio>;
	using base = Dimension<std::ratio<1>>; /**< the same dimension in its base unit */

	constexpr Quantity() noexcept = default;
	constexpr explicit Quantity(rep count) noexcept : count_{count} {}

	/** @brief Same dimension, other magnitude: converts implicitly and exactly enough. */
	template <typename OtherRatio>
	constexpr Quantity(const Quantity<Dimension, OtherRatio> &other) noexcept
		: count_{other.count() * detail::conversion<OtherRatio, Ratio>}
	{
	}

	/** @brief The number of units, like std::chrono::duration::count(). */
	[[nodiscard]] constexpr rep count() const noexcept { return count_; }

	[[nodiscard]] constexpr derived operator+() const noexcept { return derived{count_}; }
	[[nodiscard]] constexpr derived operator-() const noexcept { return derived{-count_}; }

	constexpr derived &operator+=(const derived &other) noexcept
	{
		count_ += other.count();
		return self();
	}

	constexpr derived &operator-=(const derived &other) noexcept
	{
		count_ -= other.count();
		return self();
	}

	constexpr derived &operator*=(rep factor) noexcept
	{
		count_ *= factor;
		return self();
	}

	constexpr derived &operator/=(rep divisor) noexcept
	{
		count_ /= divisor;
		return self();
	}

	/* Sums and differences come out in the finer of the two units. */

	template <typename OtherRatio>
	[[nodiscard]] friend constexpr auto operator+(const derived &lhs,
						      const Dimension<OtherRatio> &rhs) noexcept
	{
		using Common = Dimension<detail::common_ratio<Ratio, OtherRatio>>;

		return Common{Common{lhs}.count() + Common{rhs}.count()};
	}

	template <typename OtherRatio>
	[[nodiscard]] friend constexpr auto operator-(const derived &lhs,
						      const Dimension<OtherRatio> &rhs) noexcept
	{
		using Common = Dimension<detail::common_ratio<Ratio, OtherRatio>>;

		return Common{Common{lhs}.count() - Common{rhs}.count()};
	}

	[[nodiscard]] friend constexpr derived operator*(const derived &quantity,
							 rep factor) noexcept
	{
		return derived{quantity.count() * factor};
	}

	[[nodiscard]] friend constexpr derived operator*(rep factor,
							 const derived &quantity) noexcept
	{
		return derived{quantity.count() * factor};
	}

	[[nodiscard]] friend constexpr derived operator/(const derived &quantity,
							 rep divisor) noexcept
	{
		return derived{quantity.count() / divisor};
	}

	/** @brief The ratio of two quantities of one dimension is a plain number. */
	template <typename OtherRatio>
	[[nodiscard]] friend constexpr rep operator/(const derived &lhs,
						     const Dimension<OtherRatio> &rhs) noexcept
	{
		using Common = Dimension<detail::common_ratio<Ratio, OtherRatio>>;

		return Common{lhs}.count() / Common{rhs}.count();
	}

	template <typename OtherRatio>
	[[nodiscard]] friend constexpr bool operator==(const derived &lhs,
						       const Dimension<OtherRatio> &rhs) noexcept
	{
		using Common = Dimension<detail::common_ratio<Ratio, OtherRatio>>;

		return Common{lhs}.count() == Common{rhs}.count();
	}

	template <typename OtherRatio>
	[[nodiscard]] friend constexpr auto operator<=>(const derived &lhs,
							const Dimension<OtherRatio> &rhs) noexcept
	{
		using Common = Dimension<detail::common_ratio<Ratio, OtherRatio>>;

		return Common{lhs}.count() <=> Common{rhs}.count();
	}

private:
	constexpr derived &self() noexcept { return static_cast<derived &>(*this); }

	rep count_{};
};

/** @brief Explicit conversion, for when the target should be visible in the code. */
template <typename To, template <typename> class Dimension, typename Ratio>
[[nodiscard]] constexpr To quantity_cast(const Quantity<Dimension, Ratio> &quantity) noexcept
{
	return To{quantity};
}

/* --- dimensions ---------------------------------------------------------- */

template <typename Ratio = std::ratio<1>>
struct Power: Quantity<Power, Ratio> {
	using Quantity<Power, Ratio>::Quantity;
	static constexpr std::string_view symbol = "W";
};

/** @brief Meters count energy in watt-hours, so that is the base unit here, not the joule. */
template <typename Ratio = std::ratio<1>>
struct Energy: Quantity<Energy, Ratio> {
	using Quantity<Energy, Ratio>::Quantity;
	static constexpr std::string_view symbol = "Wh";
};

template <typename Ratio = std::ratio<1>>
struct Voltage: Quantity<Voltage, Ratio> {
	using Quantity<Voltage, Ratio>::Quantity;
	static constexpr std::string_view symbol = "V";
};

template <typename Ratio = std::ratio<1>>
struct Current: Quantity<Current, Ratio> {
	using Quantity<Current, Ratio>::Quantity;
	static constexpr std::string_view symbol = "A";
};

template <typename Ratio = std::ratio<1>>
struct Frequency: Quantity<Frequency, Ratio> {
	using Quantity<Frequency, Ratio>::Quantity;
	static constexpr std::string_view symbol = "Hz";
};

template <typename Ratio = std::ratio<1>>
struct ReactivePower: Quantity<ReactivePower, Ratio> {
	using Quantity<ReactivePower, Ratio>::Quantity;
	static constexpr std::string_view symbol = "var";
};

template <typename Ratio = std::ratio<1>>
struct ReactiveEnergy: Quantity<ReactiveEnergy, Ratio> {
	using Quantity<ReactiveEnergy, Ratio>::Quantity;
	static constexpr std::string_view symbol = "varh";
};

template <typename Ratio = std::ratio<1>>
struct ApparentPower: Quantity<ApparentPower, Ratio> {
	using Quantity<ApparentPower, Ratio>::Quantity;
	static constexpr std::string_view symbol = "VA";
};

template <typename Ratio = std::ratio<1>>
struct Angle: Quantity<Angle, Ratio> {
	using Quantity<Angle, Ratio>::Quantity;
	static constexpr std::string_view symbol = "deg";
};

/** @brief A number without a unit, e.g. a counter or a power factor. */
template <typename Ratio = std::ratio<1>>
struct Dimensionless: Quantity<Dimensionless, Ratio> {
	using Quantity<Dimensionless, Ratio>::Quantity;
	static constexpr std::string_view symbol = "";
};

/* --- units ---------------------------------------------------------------- */

using Milliwatts = Power<std::milli>;
using Watts = Power<>;
using Kilowatts = Power<std::kilo>;
using Megawatts = Power<std::mega>;

using WattHours = Energy<>;
using KilowattHours = Energy<std::kilo>;
using MegawattHours = Energy<std::mega>;

using Millivolts = Voltage<std::milli>;
using Volts = Voltage<>;
using Kilovolts = Voltage<std::kilo>;

using Milliamperes = Current<std::milli>;
using Amperes = Current<>;

using Hertz = Frequency<>;

using Vars = ReactivePower<>;
using Kilovars = ReactivePower<std::kilo>;

using VarHours = ReactiveEnergy<>;
using KilovarHours = ReactiveEnergy<std::kilo>;

using VoltAmperes = ApparentPower<>;
using KilovoltAmperes = ApparentPower<std::kilo>;

using Degrees = Angle<>;

using Number = Dimensionless<>;

/* --- literals -------------------------------------------------------------- */

inline namespace literals {

/* Each unit takes both spellings: 230_V and 1.5_kW. */

[[nodiscard]] constexpr Milliwatts operator""_mW(long double count) noexcept
{
	return Milliwatts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Milliwatts operator""_mW(unsigned long long count) noexcept
{
	return Milliwatts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Watts operator""_W(long double count) noexcept
{
	return Watts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Watts operator""_W(unsigned long long count) noexcept
{
	return Watts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Kilowatts operator""_kW(long double count) noexcept
{
	return Kilowatts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Kilowatts operator""_kW(unsigned long long count) noexcept
{
	return Kilowatts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Megawatts operator""_MW(long double count) noexcept
{
	return Megawatts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Megawatts operator""_MW(unsigned long long count) noexcept
{
	return Megawatts{static_cast<double>(count)};
}

[[nodiscard]] constexpr WattHours operator""_Wh(long double count) noexcept
{
	return WattHours{static_cast<double>(count)};
}

[[nodiscard]] constexpr WattHours operator""_Wh(unsigned long long count) noexcept
{
	return WattHours{static_cast<double>(count)};
}

[[nodiscard]] constexpr KilowattHours operator""_kWh(long double count) noexcept
{
	return KilowattHours{static_cast<double>(count)};
}

[[nodiscard]] constexpr KilowattHours operator""_kWh(unsigned long long count) noexcept
{
	return KilowattHours{static_cast<double>(count)};
}

[[nodiscard]] constexpr MegawattHours operator""_MWh(long double count) noexcept
{
	return MegawattHours{static_cast<double>(count)};
}

[[nodiscard]] constexpr MegawattHours operator""_MWh(unsigned long long count) noexcept
{
	return MegawattHours{static_cast<double>(count)};
}

[[nodiscard]] constexpr Millivolts operator""_mV(long double count) noexcept
{
	return Millivolts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Millivolts operator""_mV(unsigned long long count) noexcept
{
	return Millivolts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Volts operator""_V(long double count) noexcept
{
	return Volts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Volts operator""_V(unsigned long long count) noexcept
{
	return Volts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Kilovolts operator""_kV(long double count) noexcept
{
	return Kilovolts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Kilovolts operator""_kV(unsigned long long count) noexcept
{
	return Kilovolts{static_cast<double>(count)};
}

[[nodiscard]] constexpr Milliamperes operator""_mA(long double count) noexcept
{
	return Milliamperes{static_cast<double>(count)};
}

[[nodiscard]] constexpr Milliamperes operator""_mA(unsigned long long count) noexcept
{
	return Milliamperes{static_cast<double>(count)};
}

[[nodiscard]] constexpr Amperes operator""_A(long double count) noexcept
{
	return Amperes{static_cast<double>(count)};
}

[[nodiscard]] constexpr Amperes operator""_A(unsigned long long count) noexcept
{
	return Amperes{static_cast<double>(count)};
}

[[nodiscard]] constexpr Hertz operator""_Hz(long double count) noexcept
{
	return Hertz{static_cast<double>(count)};
}

[[nodiscard]] constexpr Hertz operator""_Hz(unsigned long long count) noexcept
{
	return Hertz{static_cast<double>(count)};
}

} /* namespace literals */

/* --- compile-time checks --------------------------------------------------- */

static_assert(Watts{1.5_kW} == 1500_W);
static_assert(Kilowatts{500_W} == 0.5_kW);
static_assert(1.5_kW + 200_W == 1700_W);
static_assert(2_kWh - 500_Wh == 1500_Wh);
static_assert(5_mW < 1_W && 1_MW > 999_kW);
static_assert(3_kW / 1500_W == 2.0);
static_assert((2 * 250_W).count() == 500.0 && (1_kW / 4).count() == 0.25);
static_assert(quantity_cast<Millivolts>(230_V).count() == 230'000.0);
static_assert(Watts::symbol == "W" && KilowattHours::symbol == "Wh");

} /* namespace s0::units */
