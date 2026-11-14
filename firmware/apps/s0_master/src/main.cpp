/**
 * @file
 * @brief S0 master: reads an electricity meter through the IR head and serves
 *        the values over a web dashboard, REST, WebSocket and MQTT.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "web/api.hpp"
#include "system/config.hpp"
#include "link/link.hpp"
#include "network/captive_portal.hpp"
#include "system/glue.h"
#include "network/mqtt.hpp"
#include "web/web.hpp"
#include "network/wifi.hpp"

LOG_MODULE_REGISTER(s0_main, LOG_LEVEL_INF);

int main()
{
	LOG_INF("S0 master %.*s", static_cast<int>(s0::firmware_version.size()),
		s0::firmware_version.data());

	LOG_INF("last restart: %s", s0_restart_reason());

	/* every failure below is logged where it happens and the device carries on */
	(void)s0::Settings::instance().load();

	s0::Link::instance().start();
	s0::Wifi::instance().start();
	s0::CaptivePortal::instance().start();
	s0::Mqtt::instance().start();
	(void)s0::WebServer::instance().start();

	return 0;
}
