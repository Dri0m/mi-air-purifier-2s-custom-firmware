/* api_internal.h - private definitions shared by the ESPHome API server,
 * the entity table and the mDNS responder.
 *
 * Message IDs and field numbers match esphome/esphome tag 2026.10.2,
 * esphome/components/api/api.proto (API version 1.19), as decoded by
 * aioesphomeapi 46.25.1. */
#ifndef ESPHOME_API_INTERNAL_H
#define ESPHOME_API_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
#include "esphome_api.h"
#include "pb.h"

#define ESPHOME_COMPAT_VERSION "2026.10.2"   /* api.proto we matched */
#define API_VERSION_MAJOR 1
#define API_VERSION_MINOR 19

#define API_PROJECT_NAME  "Xiaomi.Mi Air Purifier 2S"   /* HA: manufacturer.model */
#define API_MODEL         "Mi Air Purifier 2S"
#define API_MANUFACTURER  "Xiaomi"
#define API_PLATFORM      "MW300"
#define API_BOARD         "mi-air-purifier-2s"

#ifndef API_PORT
#define API_PORT 6053
#endif

/* Message types (option (id) in api.proto) */
enum {
	MSG_HELLO_REQ = 1,
	MSG_HELLO_RESP = 2,
	MSG_AUTH_REQ = 3,           /* legacy ConnectRequest, removed in 2026.1 */
	MSG_AUTH_RESP = 4,
	MSG_DISCONNECT_REQ = 5,
	MSG_DISCONNECT_RESP = 6,
	MSG_PING_REQ = 7,
	MSG_PING_RESP = 8,
	MSG_DEVICE_INFO_REQ = 9,
	MSG_DEVICE_INFO_RESP = 10,
	MSG_LIST_ENTITIES_REQ = 11,
	MSG_LIST_BINARY_SENSOR_RESP = 12,
	MSG_LIST_FAN_RESP = 14,
	MSG_LIST_SENSOR_RESP = 16,
	MSG_LIST_SWITCH_RESP = 17,
	MSG_LIST_TEXT_SENSOR_RESP = 18,
	MSG_LIST_DONE_RESP = 19,
	MSG_SUBSCRIBE_STATES_REQ = 20,
	MSG_BINARY_SENSOR_STATE = 21,
	MSG_FAN_STATE = 23,
	MSG_SENSOR_STATE = 25,
	MSG_SWITCH_STATE = 26,
	MSG_TEXT_SENSOR_STATE = 27,
	MSG_SUBSCRIBE_LOGS_REQ = 28,
	MSG_SUBSCRIBE_LOGS_RESP = 29,
	MSG_FAN_CMD = 31,
	MSG_SWITCH_CMD = 33,
	MSG_SUBSCRIBE_HA_SERVICES_REQ = 34,
	MSG_GET_TIME_REQ = 36,
	MSG_GET_TIME_RESP = 37,
	MSG_SUBSCRIBE_HA_STATES_REQ = 38,
	MSG_LIST_NUMBER_RESP = 49,
	MSG_NUMBER_STATE = 50,
	MSG_NUMBER_CMD = 51,
	MSG_LIST_SELECT_RESP = 52,
	MSG_SELECT_STATE = 53,
	MSG_SELECT_CMD = 54,
	MSG_LIST_BUTTON_RESP = 61,
	MSG_BUTTON_CMD = 62,
	MSG_DEVICE_CAPS_REQ = 149,
	MSG_DEVICE_CAPS_RESP = 150,
};

/* enum EntityCategory / SensorStateClass */
#define ENTITY_CATEGORY_NONE       0
#define ENTITY_CATEGORY_CONFIG     1
#define ENTITY_CATEGORY_DIAGNOSTIC 2
#define STATE_CLASS_NONE             0
#define STATE_CLASS_MEASUREMENT      1
#define STATE_CLASS_TOTAL_INCREASING 2
/* enum NumberMode */
#define NUMBER_MODE_AUTO   0
#define NUMBER_MODE_BOX    1
#define NUMBER_MODE_SLIDER 2

/* enum LogLevel */
#define API_LOG_ERROR   1
#define API_LOG_WARN    2
#define API_LOG_INFO    3
#define API_LOG_CONFIG  4
#define API_LOG_DEBUG   5

/* ---- entity table (api_entities.c) ---- */
enum {
	E_FAN = 0,
	E_FAN_LEVEL,
	E_MODE,
	E_PM25,
	E_TEMP,
	E_HUM,
	E_RPM,
	E_FILTER_LIFE,
	E_FILTER_HOURS,
	E_RSSI,
	E_LIGHT,
	E_LED,
	E_BUZZER,
	E_LOCK,
	E_DOOR,
	E_MOTOR_FAULT,
	E_PM_FAULT,
	E_RESET_FILTER,
	E_AUTO_BIAS,
	/* diagnostics (U1/U2) */
	E_FIRMWARE,
	E_RESET_REASON,
	E_OTA_SLOT,
	E_IP,
	E_UPTIME,
	E_HEAP,
	E_HEAP_MIN,
	E_RESTART,
	E_COUNT
};
/* entity bit masks are uint32_t (dirty sets, list_next is int8_t) */
_Static_assert(E_COUNT <= 31, "entity masks are 32-bit");
#define E_ALL_MASK ((1u << E_COUNT) - 1)

void api_entities_init(void);
uint32_t api_entity_key(unsigned idx);
const char *api_entity_object_id(unsigned idx);
/* Encode ListEntities*Response for entity idx; returns the message type. */
uint16_t api_entity_encode_list(unsigned idx, pb_writer *w);
/* Encode *StateResponse; returns the message type, 0 if stateless. */
uint16_t api_entity_encode_state(unsigned idx, pb_writer *w);
/* Entities whose state messages are rate limited (sensors). */
uint32_t api_entity_throttled_mask(void);
/* Entities with a state message. */
uint32_t api_entity_stateful_mask(void);
/* Map purifier PF_* change flags to entity bits.  Also runs
 * api_entity_poll(hal_millis()) and ORs in its bits: api_server.c calls
 * this exactly once per api_server_poll(), from the app loop. */
uint32_t api_entity_dirty_from_pf(uint32_t pf);
/* Diagnostics tick: returns the diagnostic entities due for a push
 * (uptime / heap every 60 s if changed, IP on change, OTA slot when a
 * trial ends) and performs a pending Restart (500 ms after the button,
 * re-checked against an OTA upload / trial; hal_reboot() does not return
 * on the device). */
uint32_t api_entity_poll(uint32_t now);
/* Handle a command message; returns entity bits to re-send (even if the
 * purifier rejected the command, so HA sees the real state). */
uint32_t api_entity_command(uint16_t type, const uint8_t *data, size_t len);

/* ---- logging to subscribed API clients (api_server.c) ---- */
void api_log(uint8_t level, const char *msg);
void api_log_int(uint8_t level, const char *msg, long v);

/* small helpers */
char *api_strcpy_end(char *dst, const char *end, const char *src);
char *api_itoa_end(char *dst, const char *end, long v);
#endif
