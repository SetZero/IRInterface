/**
 * @file
 * @brief Light signals for a meter's optical button.
 *
 * Meters without a mechanical button are operated by flashing light at
 * their optical port: short flashes step through the display and count up
 * the digits of the PIN, a pause accepts a digit, a long flash changes a
 * setting. The read head's LED can produce such flashes.
 *
 * A signal is a sequence of steps; each step is a burst of equal flashes
 * followed by a pause. Nothing in this file depends on Zephyr.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

#include <irlink/protocol.hpp>

#include "core/util.hpp"

namespace s0 {

/** @brief A burst of flashes and the pause that follows it. */
struct LightStep {
	irlink::LightPulses pulses{}; /**< the burst; a count of 0 makes the step a plain pause */
	std::uint32_t pause_ms{};     /**< darkness after the burst, on top of its last gap */

	/** @brief Time the step takes from its first flash to the start of the next step. */
	[[nodiscard]] constexpr std::uint32_t duration_ms() const noexcept
	{
		return pulses.count * (static_cast<std::uint32_t>(pulses.on_ms) + pulses.off_ms) +
		       pause_ms;
	}
};

/** @brief Longest PIN the sequencer accepts. */
inline constexpr std::size_t max_pin_digits = 8;

/** @brief A complete light signal: at most a wake-up burst and one step per PIN digit. */
using LightSequence = StaticVector<LightStep, max_pin_digits + 1>;

/** @brief Time a whole sequence takes. */
[[nodiscard]] constexpr std::uint32_t duration_ms(const LightSequence &sequence) noexcept
{
	std::uint32_t total = 0;

	for (const auto &step : sequence) {
		total += step.duration_ms();
	}
	return total;
}

/**
 * @brief How a PIN is flashed.
 *
 * The defaults follow the usual procedure of German electronic meters:
 * two flashes wake the display and bring up the PIN prompt, each digit is
 * counted up by one flash per unit, and three seconds without a flash
 * accept the digit. The manual of the meter has the exact numbers.
 */
struct PinTiming {
	std::uint8_t wake_pulses{2};        /**< flashes that bring up the PIN prompt; 0 to skip */
	std::uint16_t pulse_ms{400};        /**< length of one flash */
	std::uint16_t gap_ms{400};          /**< darkness between two flashes of a burst */
	std::uint16_t wake_pause_ms{4000};  /**< wait for the PIN prompt after the wake-up */
	std::uint16_t digit_pause_ms{3500}; /**< wait that accepts a digit */
};

/** @brief Why a PIN cannot be turned into a light signal. */
enum class PinError : std::uint8_t {
	Empty,     /**< no digits given */
	TooLong,   /**< more than max_pin_digits digits */
	NotDigits, /**< a character that is not 0-9 */
	BadTiming, /**< a flash or gap outside what the read head accepts */
};

/**
 * @brief Build the light signal that enters a PIN.
 *
 * @param pin    The digits, for example "0420".
 * @param timing How long flashes and pauses are.
 * @return The sequence, or why the PIN or the timing is unusable.
 */
[[nodiscard]] constexpr std::expected<LightSequence, PinError>
pin_sequence(std::string_view pin, const PinTiming &timing = {}) noexcept
{
	if (pin.empty()) {
		return std::unexpected{PinError::Empty};
	}
	if (pin.size() > max_pin_digits) {
		return std::unexpected{PinError::TooLong};
	}

	const irlink::LightPulses one_flash{1, timing.pulse_ms, timing.gap_ms};

	if (!irlink::is_valid(one_flash) || timing.wake_pulses > irlink::max_light_pulses) {
		return std::unexpected{PinError::BadTiming};
	}

	LightSequence sequence;

	if (timing.wake_pulses > 0) {
		sequence.push_back({{timing.wake_pulses, timing.pulse_ms, timing.gap_ms},
				    timing.wake_pause_ms});
	}

	for (const char character : pin) {
		if (character < '0' || character > '9') {
			return std::unexpected{PinError::NotDigits};
		}

		/* a digit is reached by that many flashes; for 0 the wait alone accepts it */
		const auto flashes = static_cast<std::uint8_t>(character - '0');

		sequence.push_back(
			{{flashes, timing.pulse_ms, timing.gap_ms}, timing.digit_pause_ms});
	}
	return sequence;
}

static_assert([] {
	const auto sequence = pin_sequence("0420");

	return sequence && sequence->size() == 5 && (*sequence)[0].pulses.count == 2 &&
	       (*sequence)[1].pulses.count == 0 && (*sequence)[2].pulses.count == 4 &&
	       (*sequence)[3].pulses.count == 2 && (*sequence)[4].pulses.count == 0 &&
	       duration_ms(*sequence) == 2 * 800 + 4000 + 6 * 800 + 4 * 3500;
}());
static_assert(pin_sequence("12a4").error() == PinError::NotDigits);
static_assert(pin_sequence("").error() == PinError::Empty);
static_assert(pin_sequence("123456789").error() == PinError::TooLong);
static_assert(pin_sequence("1", {.pulse_ms = 1}).error() == PinError::BadTiming);

} /* namespace s0 */
