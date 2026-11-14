/**
 * @file
 * @brief Lock-free byte queue between one interrupt handler and one thread.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace irlink {

/**
 * @brief Single-producer, single-consumer queue of bytes.
 *
 * One context may call push() (typically a UART interrupt) and one other
 * context may call pop() and clear() (a thread); no lock is needed.
 *
 * @tparam Capacity Size of the storage. Must be a power of two. One slot
 *                  stays unused to tell a full queue from an empty one, so
 *                  the queue holds Capacity - 1 bytes.
 */
template <std::size_t Capacity>
class ByteQueue {
	static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
		      "capacity must be a power of two");
	static_assert(Capacity <= 32768, "indices are 16 bit");

public:
	/**
	 * @brief Append a byte. Producer side.
	 * @param byte The byte to queue.
	 * @return false if the queue is full; the byte is then dropped.
	 */
	bool push(std::uint8_t byte) noexcept
	{
		const std::uint16_t head = head_.load(std::memory_order_relaxed);
		const auto next = static_cast<std::uint16_t>((head + 1) & (Capacity - 1));

		if (next == tail_.load(std::memory_order_acquire)) {
			return false;
		}
		data_[head] = byte;
		head_.store(next, std::memory_order_release);
		return true;
	}

	/**
	 * @brief Remove the oldest byte. Consumer side.
	 * @return The byte, or nothing if the queue is empty.
	 */
	std::optional<std::uint8_t> pop() noexcept
	{
		const std::uint16_t tail = tail_.load(std::memory_order_relaxed);

		if (tail == head_.load(std::memory_order_acquire)) {
			return std::nullopt;
		}

		const std::uint8_t byte = data_[tail];

		tail_.store(static_cast<std::uint16_t>((tail + 1) & (Capacity - 1)),
			    std::memory_order_release);
		return byte;
	}

	/** @brief Drop everything queued so far. Consumer side. */
	void clear() noexcept
	{
		tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
	}

private:
	std::array<std::uint8_t, Capacity> data_{};
	std::atomic<std::uint16_t> head_{}; /**< next slot to write; owned by the producer */
	std::atomic<std::uint16_t> tail_{}; /**< next slot to read; owned by the consumer */
};

} /* namespace irlink */
