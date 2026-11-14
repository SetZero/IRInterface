/**
 * @file
 * @brief RS-485 link to the IR head, telegram decoding and meter auto-detection.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include <irlink/protocol.hpp>

#include "link/light_signal.hpp"
#include "meter/meter.hpp"
#include "meter/sml.hpp"

namespace s0 {

/** @brief Where the search for the meter currently stands. */
enum class DetectState : std::uint8_t {
	NoHead,    /**< the IR head does not answer */
	Searching, /**< listening with one of the candidate settings */
	Probing,   /**< an IEC 62056-21 request is in progress */
	Locked,    /**< telegrams are being decoded */
};

/** @brief The name of a detection state as used in the API. */
[[nodiscard]] constexpr std::string_view detect_state_name(DetectState state) noexcept
{
	switch (state) {
	case DetectState::NoHead:
		return "no_head";
	case DetectState::Searching:
		return "searching";
	case DetectState::Probing:
		return "probing";
	case DetectState::Locked:
		return "locked";
	}
	return "";
}

/** @brief Progress of the light signal for the meter's optical button. */
enum class SignalState : std::uint8_t {
	Idle,    /**< none was sent since boot, or the last one was stopped */
	Running, /**< flashes are being sent */
	Done,    /**< the last signal was sent completely */
	Failed,  /**< the read head stopped confirming; the signal was abandoned */
};

/** @brief The name of a signal state as used in the API. */
[[nodiscard]] constexpr std::string_view signal_state_name(SignalState state) noexcept
{
	switch (state) {
	case SignalState::Idle:
		return "idle";
	case SignalState::Running:
		return "running";
	case SignalState::Done:
		return "done";
	case SignalState::Failed:
		return "failed";
	}
	return "";
}

/** @brief Why a light signal was not started. */
enum class SignalError : std::uint8_t {
	HeadOffline, /**< there is no read head to flash with */
	Busy,        /**< another signal is still running */
};

/** @brief Snapshot of the link and of the meter detection. */
struct LinkInfo {
	bool head_online{};
	irlink::Status head{};        /**< last status reported by the head */
	std::int64_t head_age_ms{-1}; /**< time since that status, -1 if there was none */
	std::uint32_t frames_ok{};
	std::uint32_t frame_errors{};
	std::uint32_t data_gaps{}; /**< IR data frames lost on the link */

	DetectState state{DetectState::NoHead};
	irlink::IrConfig ir{}; /**< optical settings currently requested from the head */
	bool active{};         /**< reading by request rather than by listening */
	Protocol protocol{Protocol::Unknown};
	std::uint32_t telegrams{};
	std::uint32_t decode_errors{};               /**< SML files that were rejected */
	std::optional<SmlError> last_decode_error{}; /**< why the most recent one was */
	std::int64_t telegram_age_ms{-1}; /**< time since the last telegram, -1 if none */

	SignalState signal{SignalState::Idle};
	std::uint32_t signal_remaining_ms{}; /**< time until a running signal is complete */
};

/**
 * @brief The connection to the IR head.
 *
 * Runs a thread that exchanges frames with the head, feeds what the
 * optical receiver sees into the decoders and steers the head until
 * telegrams decode.
 */
class Link {
public:
	/** @brief The one link of this device. */
	[[nodiscard]] static Link &instance() noexcept;

	Link(const Link &) = delete;
	Link &operator=(const Link &) = delete;

	/** @brief Start the link thread. Call once at boot. */
	void start() noexcept;

	/** @brief The current state of link and detection. */
	[[nodiscard]] LinkInfo info() noexcept;

	/**
	 * @brief Feed bytes to the decoders as if the optical receiver had seen them.
	 *
	 * For testing without a meter.
	 *
	 * @param bytes The simulated optical data.
	 * @return The number of bytes accepted; fewer than given if the queue is full.
	 */
	std::size_t inject(std::span<const std::uint8_t> bytes) noexcept;

	/**
	 * @brief Flash a light signal at the meter through the read head's LED.
	 *
	 * Returns at once; LinkInfo::signal reports the progress. Meter
	 * detection pauses while the signal runs, so that no serial request
	 * blinks into it.
	 *
	 * @param sequence The flashes and pauses to send.
	 */
	std::expected<void, SignalError> signal(const LightSequence &sequence) noexcept;

	/** @brief Stop a running light signal and switch the LED off. */
	void cancel_signal() noexcept;

private:
	constexpr Link() noexcept = default;
};

} /* namespace s0 */
