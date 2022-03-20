#ifndef MESHTASTIC_H
#define MESHTASTIC_H

#include <Arduino.h>

// Some sane limits on a few strings that the protocol would otherwise allow to be unlimited length
#define MAX_USER_ID_LEN 32
#define MAX_LONG_NAME_LEN 32
#define MAX_SHORT_NAME_LEN 8
#define MAX_MACADDR_LEN 32

extern uint32_t my_node_num;

// The strings will be truncated if they're longer than the lengths above, but
// will always be NUL-terminated. If not available, they'll be NULL.
typedef struct {
  uint32_t node_num;
  bool is_mine;
  const char * user_id;
  const char * long_name;
  const char * short_name;
  const char * macaddr;
  double latitude;
  double longitude;
  int8_t altitude;  // To the nearest meter above (or below) sea level
  uint16_t ground_speed; // meters per second
  uint8_t battery_level;
  uint32_t last_heard_from;
  uint32_t last_heard_position;
  uint32_t time_of_last_position;
} mt_node_t;

// Initialize, using wifi to connect to the MT radio
void mt_wifi_init(int8_t cs_pin, int8_t irq_pin, int8_t reset_pin,
    int8_t enable_pin, const char * ssid, const char * password);

// Initialize, using serial pins to connect to the MT radio
void mt_serial_init(int8_t rx_pin, int8_t tx_pin);

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

#endif
