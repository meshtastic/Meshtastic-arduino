#ifndef MESHTASTIC_H
#define MESHTASTIC_H

#include <Arduino.h>
#include "meshtastic/mesh.pb.h"
#include <pb_encode.h>
#include <pb_decode.h>

// Some sane limits on a few strings that the protocol would otherwise allow to be unlimited length
#define MAX_USER_ID_LEN (sizeof(meshtastic_User().id) - 1)
#define MAX_LONG_NAME_LEN (sizeof(meshtastic_User().long_name) - 1)
#define MAX_SHORT_NAME_LEN (sizeof(meshtastic_User().short_name) - 1)

#define BAUD_DEFAULT 9600
#define BROADCAST_ADDR 0xFFFFFFFF

extern uint32_t my_node_num;

// The strings will be truncated if they're longer than the lengths above, but
// will always be NUL-terminated. If not available, they'll be NULL.
typedef struct {
  uint32_t node_num;
  bool is_mine;
  bool has_user;
  char user_id[MAX_USER_ID_LEN];
  char long_name[MAX_LONG_NAME_LEN];
  char short_name[MAX_SHORT_NAME_LEN];
  double latitude;
  double longitude;
  int8_t altitude;  // To the nearest meter above (or below) sea level
  uint16_t ground_speed; // meters per second
  uint8_t battery_level;
  uint32_t last_heard_from;
  uint32_t last_heard_position;
  uint32_t time_of_last_position;
  float voltage;
  float channel_utilization;
  float air_util_tx;
} mt_node_t;

// Initialize, using wifi to connect to the MT radio
void mt_wifi_init(int8_t cs_pin, int8_t irq_pin, int8_t reset_pin,
    int8_t enable_pin, const char * ssid, const char * password);

// Initialize, using serial pins and baud rate to connect to the MT radio
void mt_serial_init(int8_t rx_pin, int8_t tx_pin, uint32_t baud = BAUD_DEFAULT);

// Call this once per loop() and pass the current millis(). Returns bool indicating whether the connection is ready.
bool mt_loop(uint32_t now);

// Will print lots of (semi)useful information to the main Serial output
void mt_set_debug(bool on);

typedef enum {
  MT_NR_IN_PROGRESS,
  MT_NR_DONE,
  MT_NR_INVALID
} mt_nr_progress_t;

// Ask the MT radio for a node report (it won't arrive right away)
// For each node it receives, your callback will be called with
// the second parameter set to MT_NR_IN_PROGRESS. At the end of the
// report, if the IDs matched, the callback will be called with
// NULL as the first parameter and the second set to either MT_NR_DONE
// (if it was indeed the reply to our request) or MT_NR_INVALID (if it
// turned out to have been a reply to someone else's request).
//
// Everything we pass to your callback could be destroyed immediately
// after it returns, so it should save it somewhere else if it needs it.
//
// Returns true if we were able to request the report, false if we couldn't
// even do that.
bool mt_request_node_report(void (*callback)(mt_node_t *, mt_nr_progress_t));

// Set the callback function that gets called when the node receives a text message.
void set_text_message_callback(void (*callback)(uint32_t from, const char * text));

// Send a text message with *text* as payload, to a destination node (optional), on a certain channel (optional).
bool mt_send_text(const char * text, uint32_t dest = BROADCAST_ADDR, uint8_t channel_index = 0);

#endif
