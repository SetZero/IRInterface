/**
 * @file
 * @brief IR head firmware: a remote UART for the optical meter interface.
 *
 * Everything the phototransistor receives is forwarded to the master in
 * framed, checksummed packets over RS-485. The master can change the
 * optical UART settings, transmit through the IR LED, and flash the LED in
 * slow pulses to operate a meter's optical button. See irlink/protocol.hpp
 * for the wire format.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <irlink/byte_queue.hpp>
#include <irlink/protocol.hpp>

/*
 * The board gives the LED pin two states (see zephyr,user in the board's
 * devicetree): "default" hands it to the optical UART, "lamp" holds the
 * LED lit.
 */
#define PINCTRL_STATE_LAMP PINCTRL_STATE_PRIV_START
PINCTRL_DT_DEFINE(DT_PATH(zephyr_user));

namespace {

constexpr std::uint8_t firmware_major = 1;
constexpr std::uint8_t firmware_minor = 2;

/** Forward a partial batch once the optical line has been quiet this long. */
constexpr std::int64_t ir_flush_gap_ms = 4;
constexpr std::int64_t status_period_ms = 1000;
/**
 * A command that arrives again with the same sequence number within this
 * time is a repetition by the master, which missed the acknowledgement.
 */
constexpr std::int64_t duplicate_window_ms = 2000;
/** Bits per character on the optical line, start and stop bits included. */
constexpr std::uint32_t bits_per_character = 10;

/**
 * @brief A UART whose received bytes are queued by its interrupt handler.
 * @tparam QueueSize Size of the receive queue; a power of two.
 */
template <std::size_t QueueSize>
class SerialPort {
public:
	/** @brief Why a port could not be started. */
	enum class StartError : std::uint8_t {
		NotReady,     /**< the driver failed to initialise */
		NoInterrupts, /**< the driver has no interrupt-driven API */
	};

	constexpr explicit SerialPort(const device *uart) noexcept : uart_{uart} {}

	/** @brief Start receiving into the queue. */
	std::expected<void, StartError> start() noexcept
	{
		if (!device_is_ready(uart_)) {
			return std::unexpected{StartError::NotReady};
		}
		if (uart_irq_callback_user_data_set(uart_, on_interrupt, this) < 0) {
			return std::unexpected{StartError::NoInterrupts};
		}
		uart_irq_rx_enable(uart_);
		return {};
	}

	/**
	 * @brief Send bytes, blocking until the last one is handed to the UART.
	 * @param bytes The bytes to send.
	 */
	void write(std::span<const std::uint8_t> bytes) noexcept
	{
		for (const auto byte : bytes) {
			uart_poll_out(uart_, byte);
		}
	}

	/**
	 * @brief Change baud rate and character format.
	 * @param config The settings to apply.
	 * @return The driver's errno value if it rejected them.
	 */
	std::expected<void, int> configure(const irlink::IrConfig &config) noexcept
	{
		const bool parity = config.format == irlink::IrFormat::F7E1;
		const uart_config settings{
			.baudrate = config.baud,
			.parity = static_cast<std::uint8_t>(parity ? UART_CFG_PARITY_EVEN
								   : UART_CFG_PARITY_NONE),
			.stop_bits = UART_CFG_STOP_BITS_1,
			.data_bits = static_cast<std::uint8_t>(parity ? UART_CFG_DATA_BITS_7
								      : UART_CFG_DATA_BITS_8),
			.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
		};

		if (const int result = uart_configure(uart_, &settings); result < 0) {
			return std::unexpected{-result};
		}
		return {};
	}

	/** @brief The bytes received and not yet consumed. */
	[[nodiscard]] irlink::ByteQueue<QueueSize> &received() noexcept { return received_; }

	/*
	 * The counters are written by the interrupt handler only, with a plain
	 * load and store: the Cortex-M0+ has no atomic read-modify-write.
	 */

	/** @brief Framing, parity and noise errors seen so far. */
	[[nodiscard]] std::uint16_t errors() const noexcept { return errors_.load(); }

	/** @brief Bytes lost because the queue was full. */
	[[nodiscard]] std::uint16_t dropped() const noexcept { return dropped_.load(); }

private:
	static void increment(std::atomic<std::uint16_t> &counter) noexcept
	{
		counter.store(
			static_cast<std::uint16_t>(counter.load(std::memory_order_relaxed) + 1),
			std::memory_order_relaxed);
	}

	static void on_interrupt(const device *uart, void *port)
	{
		auto &self = *static_cast<SerialPort *>(port);

		if (!uart_irq_update(uart)) {
			return;
		}

		if (uart_err_check(uart) > 0) {
			increment(self.errors_);
		}

		while (uart_irq_rx_ready(uart)) {
			std::array<std::uint8_t, 8> chunk;
			const int count = uart_fifo_read(uart, chunk.data(), chunk.size());

			if (count <= 0) {
				break;
			}
			for (const auto byte :
			     std::span{chunk}.first(static_cast<std::size_t>(count))) {
				if (!self.received_.push(byte)) {
					increment(self.dropped_);
				}
			}
		}
	}

	const device *uart_;
	irlink::ByteQueue<QueueSize> received_{};
	std::atomic<std::uint16_t> errors_{};
	std::atomic<std::uint16_t> dropped_{};
};

/**
 * @brief The IR LED as a lamp that can be held lit.
 *
 * Normally the optical UART drives the LED, which can only blink it for
 * the length of a character. For the slow flashes a meter's optical button
 * expects, the pin is taken away from the UART and driven steadily.
 */
class Lamp {
public:
	/** @brief Light the LED until off() is called. The UART cannot transmit meanwhile. */
	void on() noexcept { (void)pinctrl_apply_state(pins_, PINCTRL_STATE_LAMP); }

	/** @brief Give the LED back to the optical UART, which leaves it dark while idle. */
	void off() noexcept { (void)pinctrl_apply_state(pins_, PINCTRL_STATE_DEFAULT); }

private:
	const pinctrl_dev_config *pins_{PINCTRL_DT_DEV_CONFIG_GET(DT_PATH(zephyr_user))};
};

/**
 * @brief Flashes a Lamp in a burst of equal pulses, without blocking.
 *
 * poll() has to be called often; it switches the lamp when a flash or a
 * gap has run its time.
 */
class PulseTrain {
public:
	constexpr explicit PulseTrain(Lamp &lamp) noexcept : lamp_{lamp} {}

	/**
	 * @brief Start a burst, replacing one that is still running.
	 * @param pulses The burst; a count of 0 just stops.
	 * @param now    Current uptime in milliseconds.
	 */
	void start(const irlink::LightPulses &pulses, std::int64_t now) noexcept
	{
		pulses_ = pulses;
		lit_ = pulses.count > 0;
		edge_ms_ = now + pulses.on_ms;
		if (lit_) {
			lamp_.on();
		} else {
			lamp_.off();
		}
	}

	/**
	 * @brief Advance the burst.
	 * @param now Current uptime in milliseconds.
	 * @return true while a burst is running, its trailing gap included.
	 */
	bool poll(std::int64_t now) noexcept
	{
		if (pulses_.count == 0) {
			return false;
		}
		if (now < edge_ms_) {
			return true;
		}

		if (lit_) {
			lamp_.off();
			lit_ = false;
			edge_ms_ += pulses_.off_ms;
		} else if (--pulses_.count > 0) {
			lamp_.on();
			lit_ = true;
			edge_ms_ += pulses_.on_ms;
		}
		return pulses_.count > 0;
	}

private:
	Lamp &lamp_;
	irlink::LightPulses pulses_{}; /**< what is left of the burst */
	bool lit_{};
	std::int64_t edge_ms_{}; /**< when the lamp changes next */
};

/** @brief The application: bridges the optical port and the RS-485 link. */
class Head {
public:
	/** @brief Start both ports and serve the link. Returns only if a port fails to start. */
	void run() noexcept
	{
		if (!optical_.start() || !link_.start()) {
			printk("UART not available\n");
			return;
		}

		apply(config_);

		std::int64_t next_status_ms = 0;
		std::int64_t last_optical_byte_ms = 0;

		while (true) {
			const std::int64_t now = k_uptime_get();
			bool busy = false;

			while (const auto byte = link_.received().pop()) {
				busy = true;

				const auto frame = decoder_.push(*byte);

				if (!frame) {
					link_errors_++;
					report_link_error(frame.error());
				} else if (frame->has_value()) {
					handle(**frame, now);
				}
			}

			if (pulse_train_.poll(now)) {
				/* the receiver sees our own flashes; they are not meter data */
				discard_optical_input();
			}

			while (batch_length_ < batch_.size()) {
				const auto byte = optical_.received().pop();

				if (!byte) {
					break;
				}
				batch_[batch_length_++] = *byte;
				optical_bytes_++;
				last_optical_byte_ms = now;
				busy = true;
			}

			const bool batch_full = batch_length_ == batch_.size();
			const bool line_quiet = now - last_optical_byte_ms >= ir_flush_gap_ms;

			if (batch_full || (batch_length_ > 0 && line_quiet)) {
				send(irlink::MsgType::IrData, data_sequence_++,
				     std::span{batch_}.first(batch_length_));
				batch_length_ = 0;
			}

			if (now >= next_status_ms) {
				send_status(0);
				next_status_ms = now + status_period_ms;
			}

			if (!busy) {
				k_msleep(1);
			}
		}
	}

private:
	void handle(const irlink::Frame &frame, std::int64_t now) noexcept
	{
		const bool repeated = frame.sequence != 0 &&
				      frame.sequence == last_command_sequence_ &&
				      now - last_command_ms_ < duplicate_window_ms;

		if (repeated) {
			/* carried out already; flashing or transmitting twice would be wrong */
			send_status(frame.sequence);
			return;
		}

		switch (frame.type) {
		case irlink::MsgType::SetConfig:
			if (const auto config = irlink::parse_ir_config(frame.payload)) {
				apply(*config);
			}
			break;
		case irlink::MsgType::IrTx:
			transmit(frame.payload);
			break;
		case irlink::MsgType::LightPulses:
			if (const auto pulses = irlink::parse_light_pulses(frame.payload)) {
				pulse_train_.start(*pulses, now);
			} else {
				/* no acknowledgement: the master must not assume the flashes happen
				 */
				return;
			}
			break;
		case irlink::MsgType::GetStatus:
			break;
		case irlink::MsgType::IrData:
		case irlink::MsgType::Status:
		case irlink::MsgType::LinkError:
			/* these travel the other way; not addressed to the head */
			return;
		}

		last_command_sequence_ = frame.sequence;
		last_command_ms_ = now;

		/* every command is acknowledged with a status carrying its sequence number */
		send_status(frame.sequence);
	}

	void apply(const irlink::IrConfig &config) noexcept
	{
		if (const auto applied = optical_.configure(config); !applied) {
			printk("IR settings rejected (%d)\n", applied.error());
			return;
		}

		config_ = config;
		discard_optical_input();
		printk("IR: %u baud %s\n", static_cast<unsigned>(config.baud),
		       config.format == irlink::IrFormat::F7E1 ? "7E1" : "8N1");
	}

	void transmit(std::span<const std::uint8_t> bytes) noexcept
	{
		optical_.write(bytes);

		/*
		 * The receiver sees our own LED through reflections. Wait for
		 * the last two characters to leave the UART and throw the echo
		 * away; a meter never answers that quickly.
		 */
		k_msleep(static_cast<std::int32_t>(2 * bits_per_character * 1000 / config_.baud +
						   2));
		discard_optical_input();
	}

	void discard_optical_input() noexcept
	{
		optical_.received().clear();
		batch_length_ = 0;
	}

	/* Tell the master what arrived instead of a valid frame. */
	void report_link_error(irlink::FrameError error) noexcept
	{
		const auto received = decoder_.finished_frame();
		irlink::LinkErrorReport report{
			.error = error,
			.uart_errors = link_.errors(),
			.length = static_cast<std::uint8_t>(received.size()),
		};

		for (std::size_t index = 0; index < report.reported(); index++) {
			report.bytes[index] = received[index];
		}

		const auto [payload, length] = irlink::serialize(report);

		send(irlink::MsgType::LinkError, 0, std::span{payload}.first(length));
	}

	void send_status(std::uint8_t sequence) noexcept
	{
		const irlink::Status status{
			.protocol = irlink::protocol_version,
			.firmware_major = firmware_major,
			.firmware_minor = firmware_minor,
			.uptime_s = static_cast<std::uint32_t>(k_uptime_get() / 1000),
			.ir = config_,
			.ir_rx_bytes = optical_bytes_,
			.ir_rx_errors = optical_.errors(),
			.ir_rx_dropped = optical_.dropped(),
			.link_errors = static_cast<std::uint16_t>(link_errors_ + link_.dropped()),
		};

		send(irlink::MsgType::Status, sequence, irlink::serialize(status));
	}

	void send(irlink::MsgType type, std::uint8_t sequence,
		  std::span<const std::uint8_t> payload) noexcept
	{
		std::array<std::uint8_t, irlink::max_wire_frame> wire;

		/* cannot fail: payloads are bounded by max_payload and the buffer by max_wire_frame
		 */
		if (const auto length = irlink::encode_frame(type, sequence, payload, wire)) {
			link_.write(std::span{wire}.first(*length));
		}
	}

	SerialPort<256> optical_{DEVICE_DT_GET(DT_ALIAS(ir_uart))};
	SerialPort<256> link_{DEVICE_DT_GET(DT_ALIAS(rs485_uart))};
	Lamp lamp_{};
	PulseTrain pulse_train_{lamp_};

	irlink::IrConfig config_{};
	irlink::FrameDecoder decoder_{};
	std::array<std::uint8_t, irlink::max_payload> batch_{}; /**< optical bytes not yet sent */
	std::size_t batch_length_{};
	std::uint32_t optical_bytes_{};
	std::uint16_t link_errors_{};
	std::uint8_t data_sequence_{};
	std::uint8_t last_command_sequence_{}; /**< 0: none yet */
	std::int64_t last_command_ms_{};
};

constinit Head head;

} /* namespace */

int main()
{
	printk("IR head %u.%u, link protocol %u\n", firmware_major, firmware_minor,
	       irlink::protocol_version);

	head.run();
	return 0;
}
