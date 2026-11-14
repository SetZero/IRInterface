/*
 * Hardware bring-up test for the IR Interface / S0 Master pair.
 *
 * The role follows the board: a board with an "ir-uart" alias is the IR
 * head, anything else is the master.
 *
 * Master:  sends "ping <n>" over RS-485 once a second and prints every
 *          byte that comes back on the console.
 * IR head: echoes RS-485 bytes back to the master and repeats them on the
 *          IR LED. Bytes seen by the IR receiver are reported to the
 *          master as <hh> hex pairs, so an optical loopback (or a meter)
 *          can be told apart from the plain RS-485 echo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>

#define RX_BUF_SIZE 128

static const struct device *const rs485 = DEVICE_DT_GET(DT_ALIAS(rs485_uart));
RING_BUF_DECLARE(rs485_rx, RX_BUF_SIZE);

static void rx_isr(const struct device *dev, void *user_data)
{
	struct ring_buf *rb = user_data;
	uint8_t tmp[16];

	if (!uart_irq_update(dev)) {
		return;
	}

	while (uart_irq_rx_ready(dev)) {
		int n = uart_fifo_read(dev, tmp, sizeof(tmp));

		if (n <= 0) {
			break;
		}
		/* bytes that do not fit are dropped */
		(void)ring_buf_put(rb, tmp, n);
	}
}

static int rx_start(const struct device *dev, struct ring_buf *rb)
{
	int ret;

	if (!device_is_ready(dev)) {
		printk("%s: not ready\n", dev->name);
		return -ENODEV;
	}

	ret = uart_irq_callback_user_data_set(dev, rx_isr, rb);
	if (ret < 0) {
		printk("%s: cannot set callback (%d)\n", dev->name, ret);
		return ret;
	}
	uart_irq_rx_enable(dev);

	return 0;
}

static void tx(const struct device *dev, const char *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(dev, buf[i]);
	}
}

#if DT_HAS_ALIAS(ir_uart)

static const struct device *const ir = DEVICE_DT_GET(DT_ALIAS(ir_uart));
RING_BUF_DECLARE(ir_rx, RX_BUF_SIZE);

int main(void)
{
	static const char hex[] = "0123456789abcdef";
	uint8_t c;

	printk("IR head bring-up\n");

	if (rx_start(rs485, &rs485_rx) < 0 || rx_start(ir, &ir_rx) < 0) {
		return 0;
	}

	while (true) {
		bool idle = true;

		while (ring_buf_get(&rs485_rx, &c, 1) == 1) {
			uart_poll_out(rs485, c);
			uart_poll_out(ir, c);
			idle = false;
		}

		while (ring_buf_get(&ir_rx, &c, 1) == 1) {
			char msg[4] = { '<', hex[c >> 4], hex[c & 0xf], '>' };

			tx(rs485, msg, sizeof(msg));
			idle = false;
		}

		if (idle) {
			k_msleep(1);
		}
	}

	return 0;
}

#else /* master */

int main(void)
{
	int64_t next_ping = 0;
	unsigned int count = 0;
	uint8_t c;

	printk("S0 master bring-up\n");

	if (rx_start(rs485, &rs485_rx) < 0) {
		return 0;
	}

	while (true) {
		bool got = false;

		while (ring_buf_get(&rs485_rx, &c, 1) == 1) {
			if (c == '\n' || c == '\r' || (c >= 0x20 && c < 0x7f)) {
				printk("%c", c);
			} else {
				printk("\\x%02x", c);
			}
			got = true;
		}

		if (k_uptime_get() >= next_ping) {
			char msg[24];
			int len = snprintk(msg, sizeof(msg), "ping %u\r\n", count++);

			next_ping = k_uptime_get() + 1000;
			tx(rs485, msg, len);
		} else if (!got) {
			k_msleep(1);
		}
	}

	return 0;
}

#endif
