/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "meter/hub.hpp"

#include <algorithm>

#include "core/lock.hpp"

namespace s0 {

Hub::Hub() noexcept
{
	k_mutex_init(&mutex_);
	k_condvar_init(&condvar_);
}

Hub &Hub::instance() noexcept
{
	static Hub hub;

	return hub;
}

void Hub::publish(const Reading &reading) noexcept
{
	const Lock lock{mutex_};

	latest_ = reading;
	latest_.sequence = ++generation_;
	latest_.uptime_ms = k_uptime_get();
	have_reading_ = true;

	if (const auto power = reading.power()) {
		power_sum_ += *power;
		power_samples_++;
	}

	k_condvar_broadcast(&condvar_);
}

std::optional<Reading> Hub::latest() noexcept
{
	const Lock lock{mutex_};

	if (!have_reading_) {
		return std::nullopt;
	}
	return latest_;
}

std::uint32_t Hub::generation() noexcept
{
	const Lock lock{mutex_};

	return generation_;
}

std::optional<std::uint32_t> Hub::wait_new(std::uint32_t seen, k_timeout_t timeout) noexcept
{
	const Lock lock{mutex_};

	if (generation_ == seen) {
		k_condvar_wait(&condvar_, &mutex_, timeout);
	}
	if (generation_ == seen) {
		return std::nullopt;
	}
	return generation_;
}

void Hub::history_push(std::optional<units::Watts> sample) noexcept
{
	history_[history_head_] = sample;
	history_head_ = (history_head_ + 1) % history_points;
	history_count_ = std::min(history_count_ + 1, history_points);
}

void Hub::tick(std::int64_t now_ms) noexcept
{
	const Lock lock{mutex_};

	while (now_ms >= history_next_ms_) {
		if (power_samples_ > 0) {
			history_push(power_sum_ / power_samples_);
		} else if (history_count_ > 0) {
			/* keep the time axis continuous once data has started */
			history_push(std::nullopt);
		}
		power_sum_ = units::Watts{};
		power_samples_ = 0;
		history_next_ms_ += history_interval_ms;
	}
}

std::size_t Hub::history(std::span<std::optional<units::Watts>> out) noexcept
{
	const Lock lock{mutex_};
	const std::size_t count = std::min(history_count_, out.size());
	const std::size_t start = (history_head_ + history_points - count) % history_points;

	for (std::size_t index = 0; index < count; index++) {
		out[index] = history_[(start + index) % history_points];
	}
	return count;
}

void Hub::raw_append(std::span<const std::uint8_t> data) noexcept
{
	const Lock lock{mutex_};

	for (const auto byte : data) {
		raw_[raw_head_] = byte;
		raw_head_ = (raw_head_ + 1) % raw_capacity;
	}
	raw_total_ += static_cast<std::uint32_t>(data.size());
}

Hub::RawSnapshot Hub::raw_copy(std::span<std::uint8_t> out) noexcept
{
	const Lock lock{mutex_};
	const std::size_t available = std::min<std::size_t>(raw_total_, raw_capacity);
	const std::size_t count = std::min(available, out.size());
	const std::size_t start = (raw_head_ + raw_capacity - count) % raw_capacity;

	for (std::size_t index = 0; index < count; index++) {
		out[index] = raw_[(start + index) % raw_capacity];
	}
	return {count, raw_total_};
}

} /* namespace s0 */
