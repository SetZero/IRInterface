/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "system/config.hpp"
#include "core/lock.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(s0_config, LOG_LEVEL_INF);

namespace s0 {

namespace {

/* --- how each kind of member is stored ----------------------------------- */

/* Access to one stored item, as handed out by the settings subsystem. */
struct StoredItem {
	std::size_t length;
	settings_read_cb read;
	void *read_arg;
};

template <typename T>
inline constexpr bool is_static_string = false;

template <std::size_t Capacity>
inline constexpr bool is_static_string<StaticString<Capacity>> = true;

/* Read an item as a `T`; nothing if what is stored does not fit that type. */
template <typename T>
std::optional<T> read_value(const StoredItem &item)
{
	if constexpr (is_static_string<T>) {
		char text[T::capacity()];
		const auto length =
			item.read(item.read_arg, text, std::min(item.length, sizeof(text)));

		if (length < 0) {
			return std::nullopt;
		}
		return T{std::string_view{text, static_cast<std::size_t>(length)}};
	} else if constexpr (std::is_enum_v<T>) {
		const auto raw = read_value<std::underlying_type_t<T>>(item);

		/* MeterMode is the only stored enumeration; reject values it does not have */
		if (!raw || *raw > std::to_underlying(MeterMode::Active)) {
			return std::nullopt;
		}
		return static_cast<T>(*raw);
	} else {
		static_assert(std::is_arithmetic_v<T>, "no storage format for this type");

		T value{};

		if (item.length != sizeof(value) ||
		    item.read(item.read_arg, &value, sizeof(value)) != sizeof(value)) {
			return std::nullopt;
		}
		return value;
	}
}

/* Write a value under `name`; returns 0 or a negative errno. */
template <typename T>
int write_value(const char *name, const T &value)
{
	if constexpr (is_static_string<T>) {
		return settings_save_one(name, value.c_str(), value.size());
	} else if constexpr (std::is_enum_v<T>) {
		return write_value(name, std::to_underlying(value));
	} else {
		static_assert(std::is_arithmetic_v<T>, "no storage format for this type");
		return settings_save_one(name, &value, sizeof(value));
	}
}

/* --- the table of stored settings ---------------------------------------- */

constexpr std::string_view subtree = "s0";

/* String literal usable as a template argument. */
template <std::size_t Size>
struct FixedString {
	constexpr FixedString(const char (&text)[Size]) noexcept { std::copy_n(text, Size, data); }

	char data[Size]{};
};

struct Field {
	const char *name;     /* full settings name, "s0/wifi/ssid" */
	std::string_view key; /* name below the subtree, "wifi/ssid" */
	void (*load)(Config &config, const StoredItem &item);
	int (*save)(const Config &config);
};

/*
 * Ties a settings key to a Config member. The full name is assembled at
 * compile time and the member's type selects how it is read and written.
 */
template <FixedString Key, auto Member>
struct Stored {
	static constexpr auto name = [] {
		std::array<char, subtree.size() + 1 + sizeof(Key.data)> out{};
		auto end = std::ranges::copy(subtree, out.begin()).out;

		*end++ = '/';
		std::ranges::copy(Key.data, end);
		return out;
	}();

	static constexpr Field field{
		.name = name.data(),
		.key = std::string_view{Key.data, sizeof(Key.data) - 1},
		.load =
			[](Config &config, const StoredItem &item) {
				using Type = std::remove_cvref_t<decltype(config.*Member)>;

				/* an unreadable item leaves the default in place */
				if (const auto value = read_value<Type>(item)) {
					config.*Member = *value;
				}
			},
		.save =
			[](const Config &config) {
				return write_value(name.data(), config.*Member);
			},
	};
};

template <FixedString Key, auto Member>
inline constexpr const Field &stored = Stored<Key, Member>::field;

constexpr std::array fields{
	stored<"wifi/ssid", &Config::wifi_ssid>,        stored<"wifi/psk", &Config::wifi_psk>,
	stored<"mqtt/en", &Config::mqtt_enabled>,       stored<"mqtt/host", &Config::mqtt_host>,
	stored<"mqtt/port", &Config::mqtt_port>,        stored<"mqtt/user", &Config::mqtt_user>,
	stored<"mqtt/pass", &Config::mqtt_password>,    stored<"mqtt/topic", &Config::mqtt_topic>,
	stored<"mqtt/ha", &Config::mqtt_discovery>,     stored<"meter/mode", &Config::meter_mode>,
	stored<"meter/baud", &Config::ir_baud>,         stored<"meter/7e1", &Config::ir_7e1>,
	stored<"meter/poll", &Config::poll_interval_s>,
};

static_assert(
	[] {
		for (std::size_t first = 0; first < fields.size(); first++) {
			for (std::size_t second = first + 1; second < fields.size(); second++) {
				if (fields[first].key == fields[second].key) {
					return false;
				}
			}
		}
		return true;
	}(),
	"two settings share the same key");

/* Called by the settings subsystem for every stored item below "s0/". */
int load_item(const char *key, std::size_t length, settings_read_cb read, void *read_arg,
	      void *config)
{
	const auto field = std::ranges::find(fields, std::string_view{key}, &Field::key);

	if (field != fields.end()) {
		field->load(*static_cast<Config *>(config), StoredItem{length, read, read_arg});
	}
	return 0;
}

std::expected<void, StorageError> save_all(const Config &config)
{
	for (const auto &field : fields) {
		if (const int result = field.save(config); result < 0) {
			LOG_ERR("cannot store %s (%d)", field.name, result);
			return std::unexpected{StorageError::WriteFailed};
		}
	}
	return {};
}

} /* namespace */

Settings::Settings() noexcept
{
	k_mutex_init(&mutex_);
}

Settings &Settings::instance() noexcept
{
	static Settings settings;

	return settings;
}

std::expected<void, StorageError> Settings::load() noexcept
{
	if (const int result = settings_subsys_init(); result < 0) {
		LOG_ERR("settings storage unavailable (%d), using defaults", result);
		return std::unexpected{StorageError::Unavailable};
	}

	Config loaded;

	if (const int result = settings_load_subtree_direct(subtree.data(), load_item, &loaded);
	    result < 0) {
		LOG_ERR("cannot read settings (%d), using defaults", result);
		return std::unexpected{StorageError::ReadFailed};
	}

	const Lock lock{mutex_};

	current_ = loaded;
	return {};
}

Config Settings::get() noexcept
{
	const Lock lock{mutex_};

	return current_;
}

std::expected<void, StorageError> Settings::set(const Config &config) noexcept
{
	{
		const Lock lock{mutex_};

		current_ = config;
	}
	generation_.fetch_add(1);

	return save_all(config);
}

std::expected<void, StorageError> Settings::factory_reset() noexcept
{
	return set(Config{});
}

} /* namespace s0 */
