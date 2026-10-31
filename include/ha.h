/*
 * ha.h: Home Assistant over MQTT, with discovery.
 *
 * On connect the device announces itself (retained discovery configs under
 * <prefix>/<component>/bedside_<mac>/<key>/config) as one HA device with every
 * control the panel and web app have: sleep sounds and rain shaping, thunder,
 * the dawn chorus and each bird, the alarms (up to four slots), lights and A/C
 * intents, night mode, key lights, the dashboard, buttons, sensors and an event
 * entity for automations (prewake, ringing, snoozed, stopped, thunder ...).
 *
 * Topics (base defaults to "bedside"):
 *   <base>/status        online / offline (last will), retained
 *   <base>/state         the device state JSON (as /api/state "ui"), retained
 *   <base>/diag          Wi-Fi, uptime, Tesserae link, firmware, retained
 *   <base>/event         {"event_type": "...", "detail": "..."}
 *   <base>/set/<key>     commands from HA (the same keys as the web API)
 * HA's birth message (<prefix>/status "online") republishes the discovery.
 *
 * Broker settings come from the web app (Device tab) and live in their own
 * flash slot; include/secrets.h may seed them (MQTT_HOST, MQTT_PORT,
 * MQTT_USER, MQTT_PASS).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

void   ha_init(void);
bool   ha_connected(void);
/* Main loop: connect, (re)announce, publish state changes. */
void   ha_poll(void);
/* Forward a device event ("alarm_ringing", "prewake 06:15" ...). */
void   ha_event(const char *text);
/* An incoming command (queued from the lwIP context as app command "ha",
 * "<key>|<payload>"): translate it into an app command name and value.
 * Returns false when there is nothing to run. */
bool   ha_translate(const char *msg, char *name, size_t nn, char *value, size_t vn);
/* Web app: "enabled|host|port|user|prefix|base|password" (password last, may
 * be "*" to keep the stored one). */
bool   ha_configure(const char *v);
/* {"enabled":..,"host":..,"state":..,"error":..,"entities":..} for /api/state. */
size_t ha_status_json(char *b, size_t cap);
