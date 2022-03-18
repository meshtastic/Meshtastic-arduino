// Meshtastic Arduino GPS client
// by Mike Schiraldi
//
// Get an Adafruit Feather M0 WiFi (or some other board, but then edit the pin config below)
// and put your Meshtastic node in WiFi AP mode, then put your SSID and wifi password in arduino_secrets.h
// and run this code with the serial monitor open. Your arduino should connect to the node, request information
// on it and all other nodes it knows about, and print a report of what it finds.
#include <Meshtastic.h>

// These are the pins for an Adafruit Feather M0 WiFi
#define WIFI_CS_PIN 8
#define WIFI_IRQ_PIN 7
#define WIFI_RESET_PIN 4
#define WIFI_ENABLE_PIN 2

#define SERIAL_RX 2
#define SERIAL_TX 3

// Request a node report every this many msec
#define NODE_REPORT_PERIOD (30 * 1000)

uint32_t next_node_report_time = 0;

void setup() {
  // Try for up to five seconds to find a serial port; if not, the show must go on
  Serial.begin(9600);
  while(true) {
    if (Serial) break;
    if (millis() > 5000) break;
  }

  Serial.print("Booted Meshtastic GPS client v1.0 in ");

// Change to 0 to use a serial connection
#if 1
  #include "arduino_secrets.h"
  Serial.print("wifi");
  mt_wifi_init(WIFI_CS_PIN, WIFI_IRQ_PIN, WIFI_RESET_PIN, WIFI_ENABLE_PIN, WIFI_SSID, WIFI_PASS);
#else
  Serial.print("serial");
  mt_serial_init(SERIAL_RX, SERIAL_TX);
#endif
  Serial.println(" mode");

  // Comment out if you want a quiet console
  mt_set_debug(true);
  randomSeed(micros());
}

void node_report_callback(mt_node_t * nodeinfo, mt_nr_progress_t progress) {
  if (progress == MT_NR_INVALID) {
    Serial.println("Oops, ignore all that. It was a reply to someone else's query.");
    return;
  } else if (progress == MT_NR_DONE) {
    Serial.println("And that's all the nodes!");
    return;
  }
  
  Serial.print("The node at ");
  Serial.print(nodeinfo->node_num);
  if (nodeinfo->is_mine) {
    Serial.print(" (that's mine!)");
  }
  Serial.print(", last reached at time=");
  Serial.print(nodeinfo->last_heard_from);

  if (nodeinfo->long_name != NULL) {
    Serial.print(", belongs to '");
    Serial.print(nodeinfo->long_name);
    Serial.print("' (a.k.a. '");
    Serial.print(nodeinfo->short_name);
    Serial.print("' or '");
    Serial.print(nodeinfo->user_id);
    Serial.print("' at '");
    Serial.print(nodeinfo->macaddr);
    Serial.print("') ");
  } else {
    Serial.print(", prefers to remain anonymous ");
  }

  if (!isnan(nodeinfo->latitude)) {
    Serial.print("and is at ");
    Serial.print(nodeinfo->latitude);
    Serial.print(", ");
    Serial.print(nodeinfo->longitude);
    Serial.print("; ");
    Serial.print(nodeinfo->altitude);
    Serial.print("m above sea level moving at ");
    Serial.print(nodeinfo->ground_speed);
    Serial.print(" m/s as of time=");
    Serial.print(nodeinfo->time_of_last_position);
    Serial.print(" and their battery is at ");
    Serial.print(nodeinfo->battery_level);
    Serial.print(" and they told our node at time=");
    Serial.println(nodeinfo->last_heard_position);
  } else {
    Serial.println(" has no position");
  }
}

void loop() {
  uint32_t now = millis();
  bool can_send = mt_loop(now);
  if (can_send && now >= next_node_report_time) {
    mt_request_node_report(node_report_callback);
    next_node_report_time = now + NODE_REPORT_PERIOD;
  }
}
