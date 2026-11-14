/**
 * @file
 * @brief MQTT publisher with Home Assistant discovery.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

namespace s0 {

/** @brief Snapshot of the MQTT connection. */
struct MqttInfo {
	bool enabled{};            /**< a broker is configured and MQTT is switched on */
	bool connected{};          /**< the broker has accepted the session */
	std::uint32_t published{}; /**< state messages sent since boot */
};

/**
 * @brief Publishes every telegram to an MQTT broker.
 *
 * Stays idle until a broker is configured, and reconnects on its own.
 */
class Mqtt {
public:
	/** @brief The one MQTT publisher of this device. */
	[[nodiscard]] static Mqtt &instance() noexcept;

	Mqtt(const Mqtt &) = delete;
	Mqtt &operator=(const Mqtt &) = delete;

	/** @brief Start the MQTT thread. Call once at boot. */
	void start() noexcept;

	/** @brief The current state of the connection. */
	[[nodiscard]] MqttInfo info() const noexcept;

private:
	constexpr Mqtt() noexcept = default;
};

} /* namespace s0 */
