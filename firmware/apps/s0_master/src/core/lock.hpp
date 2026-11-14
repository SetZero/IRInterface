/**
 * @file
 * @brief Scoped ownership of a Zephyr mutex.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <zephyr/kernel.h>

namespace s0 {

/**
 * @brief Holds a Zephyr mutex for the lifetime of the object.
 *
 * @code
 *     const Lock lock{mutex_};
 * @endcode
 */
class Lock {
public:
	/**
	 * @brief Take the mutex, waiting for it as long as necessary.
	 * @param mutex The mutex to hold; must outlive the lock.
	 */
	explicit Lock(k_mutex &mutex) noexcept : mutex_{mutex} { k_mutex_lock(&mutex_, K_FOREVER); }
	~Lock() { k_mutex_unlock(&mutex_); }

	/* a lock guards exactly one scope */
	Lock(const Lock &) = delete;
	Lock &operator=(const Lock &) = delete;

private:
	k_mutex &mutex_;
};

} /* namespace s0 */
