/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "link/link.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <irlink/byte_queue.hpp>

#include "system/config.hpp"
#include "meter/hub.hpp"
#include "meter/iec62056.hpp"
#include "core/lock.hpp"
#include "meter/sml.hpp"

LOG_MODULE_REGISTER(s0_link, LOG_LEVEL_INF);

namespace s0 {

namespace {

using irlink::IrConfig;
using irlink::IrFormat;
using irlink::MsgType;

constexpr std::int64_t head_timeout_ms = 3500;
constexpr std::int64_t config_retry_ms = 500;
constexpr std::int64_t listen_dwell_ms = 15'000;
constexpr std::int64_t lock_timeout_ms = 90'000;
constexpr std::int64_t probe_config_timeout_ms = 3000;
constexpr std::int64_t probe_ident_timeout_ms = 4000;
constexpr std::int64_t probe_ack_delay_ms = 300;
constexpr std::int64_t probe_data_timeout_ms = 60'000;
constexpr std::int64_t probe_retry_ms = 10'000;
constexpr unsigned probe_max_failures = 3;
/* the head confirms a command within a few milliseconds */
constexpr std::int64_t signal_ack_timeout_ms = 120;
/* how often a light command is sent before the signal is given up */
constexpr unsigned signal_max_attempts = 4;

constexpr IrConfig probe_config{300, IrFormat::F7E1};

struct Step {
	bool probe;
	IrConfig listen;
};

/*
 * Order in which the automatic mode tries things. 9600 baud covers SML and
 * most pushed text telegrams: 7E1 text read as 8N1 only differs in the top
 * bit, which the text decoder masks.
 */
constexpr Step auto_steps[] = {
	{false, {9600, IrFormat::F8N1}}, {true, {}},
	{false, {2400, IrFormat::F8N1}}, {false, {4800, IrFormat::F8N1}},
	{false, {1200, IrFormat::F8N1}}, {false, {19200, IrFormat::F8N1}},
	{false, {300, IrFormat::F8N1}},
};

enum class Probe : std::uint8_t {
	Configure,   /* waiting for the head to switch to 300 baud 7E1 */
	RequestSent, /* "/?!" is out, waiting for the identification */
	AckWait,     /* mode C: pause before the acknowledgement */
	Reading,     /* waiting for the data block */
};

K_KERNEL_STACK_DEFINE(link_stack, 8192);

class LinkTask {
public:
	void start()
	{
		k_mutex_init(&mutex_);
		k_mutex_init(&inject_mutex_);
		k_sem_init(&wake_, 0, 1);

		k_thread_create(
			&thread_, link_stack, K_KERNEL_STACK_SIZEOF(link_stack),
			[](void *self, void *, void *) { static_cast<LinkTask *>(self)->run(); },
			this, nullptr, nullptr, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
		k_thread_name_set(&thread_, "s0_link");
	}

	LinkInfo info()
	{
		const Lock lock{mutex_};
		const std::int64_t now = k_uptime_get();
		LinkInfo out = info_;

		out.head_age_ms = head_seen_ms_ < 0 ? -1 : now - head_seen_ms_;
		out.telegram_age_ms = telegram_ms_ < 0 ? -1 : now - telegram_ms_;
		if (out.signal == SignalState::Running) {
			out.signal_remaining_ms = static_cast<std::uint32_t>(
				std::max<std::int64_t>(signal_end_ms_ - now, 0));
		}
		return out;
	}

	std::size_t inject(std::span<const std::uint8_t> bytes)
	{
		const Lock lock{inject_mutex_};
		std::size_t accepted = 0;

		for (const auto byte : bytes) {
			if (!inject_.push(byte)) {
				break;
			}
			accepted++;
		}
		k_sem_give(&wake_);
		return accepted;
	}

	std::expected<void, SignalError> signal(const LightSequence &sequence)
	{
		const Lock lock{mutex_};

		if (!info_.head_online) {
			return std::unexpected{SignalError::HeadOffline};
		}
		if (info_.signal == SignalState::Running) {
			return std::unexpected{SignalError::Busy};
		}

		requested_sequence_ = sequence;
		signal_requested_ = true;
		signal_end_ms_ = k_uptime_get() + duration_ms(sequence);
		info_.signal = SignalState::Running;
		k_sem_give(&wake_);
		return {};
	}

	void cancel_signal()
	{
		const Lock lock{mutex_};

		cancel_requested_ = true;
		k_sem_give(&wake_);
	}

private:
	static void on_uart_interrupt(const device *uart, void *user_data)
	{
		auto &self = *static_cast<LinkTask *>(user_data);

		if (!uart_irq_update(uart)) {
			return;
		}

		while (uart_irq_rx_ready(uart)) {
			std::array<std::uint8_t, 32> chunk;
			const int received = uart_fifo_read(uart, chunk.data(), chunk.size());

			if (received <= 0) {
				break;
			}
			for (int index = 0; index < received; index++) {
				if (!self.received_.push(chunk[index])) {
					self.received_dropped_.fetch_add(1,
									 std::memory_order_relaxed);
				}
			}
		}
		k_sem_give(&self.wake_);
	}

	[[noreturn]] void run()
	{
		if (!device_is_ready(uart_)) {
			LOG_ERR("RS-485 UART not ready");
			k_sleep(K_FOREVER);
		}

		uart_irq_callback_user_data_set(uart_, on_uart_interrupt, this);
		uart_irq_rx_enable(uart_);

		load_config();

		while (true) {
			k_sem_take(&wake_, K_MSEC(50));

			while (const auto byte = received_.pop()) {
				on_link_byte(*byte);
			}

			/* test data bypasses the head and goes straight to the decoders */
			std::array<std::uint8_t, 64> chunk;
			std::size_t length = 0;

			while (const auto byte = inject_.pop()) {
				chunk[length++] = *byte;
				if (length == chunk.size()) {
					on_ir_data(chunk);
					length = 0;
				}
			}
			if (length > 0) {
				on_ir_data(std::span{chunk}.first(length));
			}

			const std::int64_t now = k_uptime_get();

			Hub::instance().tick(now);
			tick(now);
		}
	}

	/* --- frames from the head ------------------------------------------- */

	void on_link_byte(std::uint8_t byte)
	{
		const auto frame = decoder_.push(byte);

		if (!frame) {
			const Lock lock{mutex_};

			info_.frame_errors++;
		} else if (frame->has_value()) {
			on_frame(**frame);
		}
	}

	void on_frame(const irlink::Frame &frame)
	{
		{
			const Lock lock{mutex_};

			info_.frames_ok++;
		}

		switch (frame.type) {
		case MsgType::IrData:
			if (data_sequence_ && *data_sequence_ != frame.sequence) {
				const Lock lock{mutex_};

				info_.data_gaps++;
			}
			data_sequence_ = static_cast<std::uint8_t>(frame.sequence + 1);
			on_ir_data(frame.payload);
			break;

		case MsgType::Status:
			if (const auto status = irlink::parse_status(frame.payload)) {
				const Lock lock{mutex_};

				if (status->uptime_s < info_.head.uptime_s) {
					/* the head restarted: its data counter did too */
					data_sequence_.reset();
				}
				info_.head = *status;
				head_seen_ms_ = k_uptime_get();
			}
			if (awaiting_ack_ == frame.sequence) {
				/* the head repeats a command's sequence number to confirm it */
				awaiting_ack_.reset();
			}
			break;

		case MsgType::LinkError:
			if (const auto report = irlink::parse_link_error(frame.payload)) {
				LOG_WRN("read head rejected a frame: %s, %u bytes, %u receiver "
					"errors",
					irlink::frame_error_name(report->error), report->length,
					report->uart_errors);
				LOG_HEXDUMP_WRN(report->bytes.data(), report->reported(),
						"as received");
			}
			break;

		case MsgType::SetConfig:
		case MsgType::IrTx:
		case MsgType::GetStatus:
		case MsgType::LightPulses:
			/* commands travel the other way; an echo of our own is ignored */
			break;
		}
	}

	void on_ir_data(std::span<const std::uint8_t> data)
	{
		Hub::instance().raw_append(data);

		for (const auto byte : data) {
			if (sml_decoder_.push(byte)) {
				publish(sml_decoder_.reading());
			}
			if (iec_decoder_.push(byte)) {
				publish(iec_decoder_.reading());
			}
		}

		/* the decoder belongs to this thread; readers get a copy of its counters */
		const Lock lock{mutex_};

		info_.decode_errors = sml_decoder_.errors();
		info_.last_decode_error = sml_decoder_.last_error();
	}

	void publish(const Reading &reading)
	{
		Hub::instance().publish(reading);
		telegram_in_phase_ = true;

		const Lock lock{mutex_};

		info_.telegrams++;
		info_.protocol = reading.protocol;
		telegram_ms_ = k_uptime_get();
	}

	/* --- commands to the head ------------------------------------------- */

	void send(MsgType type, std::span<const std::uint8_t> payload)
	{
		transmit(type, next_sequence(), payload);
	}

	void transmit(MsgType type, std::uint8_t sequence, std::span<const std::uint8_t> payload)
	{
		std::array<std::uint8_t, irlink::max_wire_frame> wire;
		const auto length = irlink::encode_frame(type, sequence, payload, wire);

		if (!length) {
			LOG_ERR("frame of %zu bytes cannot be encoded", payload.size());
			return;
		}
		for (const auto byte : std::span{wire}.first(*length)) {
			uart_poll_out(uart_, byte);
		}
	}

	/* Sequence numbers of commands skip 0, which the head uses for unprompted status. */
	std::uint8_t next_sequence()
	{
		if (++transmit_sequence_ == 0) {
			transmit_sequence_ = 1;
		}
		return transmit_sequence_;
	}

	void send_config(const IrConfig &config)
	{
		send(MsgType::SetConfig, irlink::serialize(config));
	}

	void ir_transmit(std::string_view text)
	{
		send(MsgType::IrTx,
		     std::span{reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
	}

	/* --- detection ------------------------------------------------------ */

	void load_config()
	{
		const Config config = Settings::instance().get();

		config_generation_ = Settings::instance().generation();
		mode_ = config.meter_mode;
		poll_interval_ms_ = std::max<std::int64_t>(config.poll_interval_s, 10) * 1000;
		passive_ = IrConfig{
			std::clamp(config.ir_baud, irlink::min_ir_baud, irlink::max_ir_baud),
			config.ir_7e1 ? IrFormat::F7E1 : IrFormat::F8N1,
		};
		restart_ = true;
	}

	void set_state(DetectState state)
	{
		const Lock lock{mutex_};

		info_.state = state;
		info_.ir = desired_;
		info_.active = active_locked_ || state == DetectState::Probing;
	}

	void begin_listen(const IrConfig &config, std::int64_t now)
	{
		desired_ = config;
		phase_start_ms_ = now;
		telegram_in_phase_ = false;
		active_locked_ = false;
		set_state(DetectState::Searching);
	}

	void begin_probe(std::int64_t now)
	{
		desired_ = probe_config;
		probe_ = Probe::Configure;
		probe_ms_ = now;
		telegram_in_phase_ = false;
		set_state(DetectState::Probing);
	}

	void begin_step(std::int64_t now)
	{
		switch (mode_) {
		case MeterMode::Passive:
			begin_listen(passive_, now);
			break;
		case MeterMode::Active:
			begin_probe(now);
			break;
		case MeterMode::Automatic:
			if (auto_steps[step_].probe) {
				begin_probe(now);
			} else {
				begin_listen(auto_steps[step_].listen, now);
			}
			break;
		}
	}

	void next_step(std::int64_t now)
	{
		step_ = (step_ + 1) % std::size(auto_steps);
		begin_step(now);
	}

	void probe_failed(std::int64_t now)
	{
		probe_failures_++;

		if (mode_ == MeterMode::Automatic &&
		    (!active_locked_ || probe_failures_ >= probe_max_failures)) {
			active_locked_ = false;
			probe_failures_ = 0;
			next_step(now);
			return;
		}

		/* keep trying: active mode, or a meter that answered before */
		next_probe_ms_ = now + probe_retry_ms;
		set_state(active_locked_ ? DetectState::Locked : DetectState::Searching);
		waiting_for_probe_ = true;
	}

	void probe_succeeded(std::int64_t now)
	{
		probe_failures_ = 0;
		active_locked_ = true;
		waiting_for_probe_ = true;
		next_probe_ms_ = now + poll_interval_ms_;
		set_state(DetectState::Locked);
	}

	void run_probe(std::int64_t now, bool config_ok)
	{
		switch (probe_) {
		case Probe::Configure:
			if (config_ok) {
				iec_decoder_.reset();
				telegram_in_phase_ = false;
				ir_transmit("/?!\r\n");
				probe_ = Probe::RequestSent;
				probe_ms_ = now;
			} else if (now - probe_ms_ > probe_config_timeout_ms) {
				probe_failed(now);
			}
			break;

		case Probe::RequestSent:
			if (const auto identification = iec_decoder_.take_identification()) {
				/*
				 * "/XXXZ...": Z is the baud rate code. A digit
				 * means mode C, where the meter waits for an
				 * acknowledgement; otherwise data follows unasked.
				 */
				const char baud_code =
					identification->size() > 4 ? (*identification)[4] : '\0';

				LOG_INF("meter identification: %.*s",
					static_cast<int>(identification->size()),
					identification->data());
				probe_ = (baud_code >= '0' && baud_code <= '9') ? Probe::AckWait
										: Probe::Reading;
				probe_ms_ = now;
			} else if (now - probe_ms_ > probe_ident_timeout_ms) {
				probe_failed(now);
			}
			break;

		case Probe::AckWait:
			if (now - probe_ms_ >= probe_ack_delay_ms) {
				/* ACK, protocol 0, stay at 300 baud, mode 0 = data read-out */
				ir_transmit("\x06"
					    "000\r\n");
				probe_ = Probe::Reading;
				probe_ms_ = now;
			}
			break;

		case Probe::Reading:
			if (telegram_in_phase_) {
				probe_succeeded(now);
			} else if (now - probe_ms_ > probe_data_timeout_ms) {
				probe_failed(now);
			}
			break;
		}
	}

	/* --- light signals -------------------------------------------------- */

	void finish_signal(SignalState result)
	{
		if (result != SignalState::Done) {
			/* make sure the LED does not stay lit */
			send(MsgType::LightPulses, irlink::serialize(irlink::LightPulses{}));
		}

		signal_active_ = false;
		awaiting_ack_.reset();
		restart_ = true; /* detection starts over once the meter has been operated */

		const Lock lock{mutex_};

		info_.signal = result;
	}

	/*
	 * Advance the light signal, if there is one. Returns true while one
	 * is running and meter detection has to keep out of its way.
	 */
	bool run_signal(std::int64_t now)
	{
		bool cancelled = false;

		{
			const Lock lock{mutex_};

			cancelled = cancel_requested_;
			cancel_requested_ = false;
			if (cancelled) {
				signal_requested_ = false;
				if (!signal_active_ && info_.signal == SignalState::Running) {
					info_.signal =
						SignalState::Idle; /* stopped before it began */
				}
			} else if (signal_requested_) {
				signal_requested_ = false;
				sequence_ = requested_sequence_;
				signal_step_ = 0;
				next_signal_step_ms_ = now;
				signal_active_ = true;
			}
		}

		if (!signal_active_) {
			return false;
		}
		if (cancelled) {
			LOG_INF("light signal stopped");
			finish_signal(SignalState::Idle);
			return false;
		}
		if (awaiting_ack_) {
			if (now < ack_deadline_ms_) {
				return true; /* nothing else is sent until the head has confirmed */
			}
			if (command_attempts_ >= signal_max_attempts) {
				LOG_WRN("read head did not confirm the light signal");
				finish_signal(SignalState::Failed);
				return false;
			}

			/*
			 * Sent again under the same sequence number: the head carries
			 * out a command once per number, so a repetition cannot make
			 * it flash twice. The rest of the signal moves back by the
			 * time that was lost.
			 */
			transmit(MsgType::LightPulses, *awaiting_ack_, command_);
			command_attempts_++;
			ack_deadline_ms_ = now + signal_ack_timeout_ms;
			next_signal_step_ms_ += signal_ack_timeout_ms;

			const Lock lock{mutex_};

			signal_end_ms_ += signal_ack_timeout_ms;
			return true;
		}
		if (now < next_signal_step_ms_) {
			return true;
		}
		if (signal_step_ == sequence_.size()) {
			LOG_INF("light signal sent");
			finish_signal(SignalState::Done);
			return false;
		}

		const LightStep &step = sequence_[signal_step_++];

		if (step.pulses.count > 0) {
			command_ = irlink::serialize(step.pulses);
			send(MsgType::LightPulses, command_);
			awaiting_ack_ = transmit_sequence_;
			command_attempts_ = 1;
			ack_deadline_ms_ = now + signal_ack_timeout_ms;
		}
		next_signal_step_ms_ = now + step.duration_ms();
		return true;
	}

	void tick(std::int64_t now)
	{
		if (config_generation_ != Settings::instance().generation()) {
			load_config();
		}

		bool online;
		IrConfig head_ir;

		{
			const Lock lock{mutex_};

			online = head_seen_ms_ >= 0 && now - head_seen_ms_ < head_timeout_ms;
			info_.head_online = online;
			head_ir = info_.head.ir;
		}

		if (!online) {
			if (state() != DetectState::NoHead) {
				LOG_WRN("IR head offline");
				set_state(DetectState::NoHead);
			}
			if (signal_active_) {
				finish_signal(SignalState::Failed);
			}
			restart_ = true;
			return;
		}

		if (run_signal(now)) {
			return;
		}

		if (restart_) {
			restart_ = false;
			step_ = 0;
			probe_failures_ = 0;
			active_locked_ = false;
			waiting_for_probe_ = false;
			begin_step(now);
		}

		const bool config_ok = head_ir == desired_;

		if (!config_ok && now - config_tx_ms_ >= config_retry_ms) {
			send_config(desired_);
			config_tx_ms_ = now;
		}

		switch (state()) {
		case DetectState::NoHead:
			break;

		case DetectState::Searching:
			if (waiting_for_probe_) {
				if (now >= next_probe_ms_) {
					waiting_for_probe_ = false;
					begin_probe(now);
				}
				break;
			}
			if (!config_ok) {
				/* the dwell time starts once the head has switched */
				phase_start_ms_ = now;
				break;
			}
			if (telegram_in_phase_) {
				LOG_INF("meter found: %u baud",
					static_cast<unsigned>(desired_.baud));
				set_state(DetectState::Locked);
			} else if (mode_ == MeterMode::Automatic &&
				   now - phase_start_ms_ >= listen_dwell_ms) {
				next_step(now);
			}
			break;

		case DetectState::Probing:
			run_probe(now, config_ok);
			break;

		case DetectState::Locked:
			if (active_locked_) {
				if (waiting_for_probe_ && now >= next_probe_ms_) {
					waiting_for_probe_ = false;
					begin_probe(now);
				}
				break;
			}
			if (mode_ == MeterMode::Automatic) {
				std::int64_t last;

				{
					const Lock lock{mutex_};

					last = telegram_ms_;
				}
				if (now - last > lock_timeout_ms) {
					LOG_WRN("meter went quiet, searching again");
					restart_ = true;
				}
			}
			break;
		}
	}

	DetectState state()
	{
		const Lock lock{mutex_};

		return info_.state;
	}

	const device *const uart_{DEVICE_DT_GET(DT_ALIAS(rs485_uart))};

	irlink::ByteQueue<2048> received_{};
	std::atomic<std::uint32_t> received_dropped_{};
	irlink::ByteQueue<2048> inject_{};
	k_mutex inject_mutex_{};
	k_sem wake_{};

	k_mutex mutex_{}; /* guards info_, head_seen_ms_, telegram_ms_ */
	LinkInfo info_{};
	std::int64_t head_seen_ms_{-1};
	std::int64_t telegram_ms_{-1};

	irlink::FrameDecoder decoder_{};
	std::optional<std::uint8_t> data_sequence_{};
	std::uint8_t transmit_sequence_{};

	SmlDecoder sml_decoder_{};
	IecDecoder iec_decoder_{};

	std::uint32_t config_generation_{};
	MeterMode mode_{MeterMode::Automatic};
	IrConfig passive_{};
	std::int64_t poll_interval_ms_{60'000};

	bool restart_{true};
	std::size_t step_{};
	IrConfig desired_{};
	std::int64_t config_tx_ms_{-config_retry_ms};
	std::int64_t phase_start_ms_{};
	bool telegram_in_phase_{};

	Probe probe_{Probe::Configure};
	std::int64_t probe_ms_{};
	unsigned probe_failures_{};
	bool active_locked_{};
	bool waiting_for_probe_{};
	std::int64_t next_probe_ms_{};

	/* handed over from the caller of signal(); guarded by mutex_ */
	LightSequence requested_sequence_{};
	bool signal_requested_{};
	bool cancel_requested_{};
	std::int64_t signal_end_ms_{};

	/* the signal being sent; owned by the link thread */
	LightSequence sequence_{};
	bool signal_active_{};
	std::size_t signal_step_{};
	std::int64_t next_signal_step_ms_{};
	std::optional<std::uint8_t> awaiting_ack_{}; /* sequence number of an unconfirmed command */
	std::array<std::uint8_t, irlink::light_pulses_size> command_{}; /* that command's payload */
	unsigned command_attempts_{};
	std::int64_t ack_deadline_ms_{};

	k_thread thread_{};
};

constinit LinkTask task;

} /* namespace */

Link &Link::instance() noexcept
{
	static constinit Link link;

	return link;
}

void Link::start() noexcept
{
	task.start();
}

LinkInfo Link::info() noexcept
{
	return task.info();
}

std::size_t Link::inject(std::span<const std::uint8_t> bytes) noexcept
{
	return task.inject(bytes);
}

std::expected<void, SignalError> Link::signal(const LightSequence &sequence) noexcept
{
	return task.signal(sequence);
}

void Link::cancel_signal() noexcept
{
	task.cancel_signal();
}

} /* namespace s0 */
