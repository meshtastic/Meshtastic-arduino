/*
    Meshtastic NodeInfo client

    Connects to a Meshtastic node via WiFi or Serial (and maybe one day Bluetooth),
    asks it for information about itself and all other nodes in the mesh that it
    knows about, and prints a report on their basic characteristics, including all
    available location data

    To use, get an Adafruit Feather M0 WiFi (or some other board, but then edit
    the pin config below) and put your Meshtastic node in WiFi AP mode, then put
    your SSID and wifi password in arduino_secrets.h and run this code with the
    serial monitor open.

    Created March 2022
    By Mike Schiraldi

*/

#include <Meshtastic.h>

// Pins to use for WiFi; these defaults are for an Adafruit Feather M0 WiFi.
#define WIFI_CS_PIN 8
#define WIFI_IRQ_PIN 7
#define WIFI_RESET_PIN 4
#define WIFI_ENABLE_PIN 2

// Pins to use for SoftwareSerial. Boards that don't use SoftwareSerial, and
// instead provide their own Serial1 connection through fixed pins (like the
// aforementioned Feather) will ignore these settings and use their own.
// On the Feather, these pins are marked "RX0" and "TX1".
#define SERIAL_RX_PIN 2
#define SERIAL_TX_PIN 3
// A different baud rate to communicate with the Meshtastic device can be specified here
#define BAUD_RATE 9600

// Request a node report every this many msec
#define NODE_REPORT_PERIOD (30 * 1000)

// Storage for node reports when they come in, such that they can be worked with afterwards.
mt_node_t node_infos[100];
uint8_t node_infos_count = 0;

// When millis() is >= this, it's time to request a node report.
uint32_t next_node_report_time = 0;

void setup() {
  // Try for up to five seconds to find a serial port; if not, the show must go on
  Serial.begin(9600);
  while(true) {
    if (Serial) break;
    if (millis() > 5000) {
      Serial.print("Couldn't find a serial port after 5 seconds, continuing anyway");
      break;
    }
  }

  Serial.print("Booted Meshtastic NodeInfo client v2.0 in ");

// Change to 1 to use a WiFi connection
#if 0
  #include "arduino_secrets.h"
  Serial.print("wifi");
  mt_wifi_init(WIFI_CS_PIN, WIFI_IRQ_PIN, WIFI_RESET_PIN, WIFI_ENABLE_PIN, WIFI_SSID, WIFI_PASS);
#else
  Serial.print("serial");
  mt_serial_init(SERIAL_RX_PIN, SERIAL_TX_PIN, BAUD_RATE);
#endif
  Serial.println(" mode");

  // Set to true if you want debug messages
  mt_set_debug(false);
  
  randomSeed(micros());
}

void print_node_infos() {
  Serial.print("There are "); 
  Serial.print(node_infos_count);
  Serial.println(" nodes in the database.");

  for (uint8_t i = 0; i < node_infos_count; i++) {
    mt_node_t* nodeinfo = &node_infos[i];
    Serial.print("The node with number ");
    Serial.print(nodeinfo->node_num);
    Serial.print(" (");
    Serial.print(nodeinfo->user_id);
    Serial.print(")");
    if (nodeinfo->is_mine) {
      Serial.print(" (that's mine!)");
    }
    Serial.print(", last reached at time=");
    Serial.print(nodeinfo->last_heard_from);

    if (nodeinfo->has_user) {
      Serial.print(", belongs to '");
      Serial.print(nodeinfo->long_name);
      Serial.print("' (a.k.a. '");
      Serial.print(nodeinfo->short_name);
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
      Serial.print(" meters above sea level moving ");
      Serial.print(nodeinfo->ground_speed);
      Serial.print(" m/s as of time=");
      Serial.print(nodeinfo->time_of_last_position);
      Serial.print(" and they told our node at time=");
      Serial.print(nodeinfo->last_heard_position);
    } else {
      Serial.print("has no position");
    }

    if (!isnan(nodeinfo->voltage)) {
      Serial.print(" and their battery voltage is ");
      Serial.print(nodeinfo->voltage);
      Serial.print("V, ");
      Serial.print("and their battery level is ");
      Serial.print(nodeinfo->battery_level);
      Serial.print("%, ");
      Serial.print("and their channel utilization is ");
      Serial.print(nodeinfo->channel_utilization);
      Serial.print("%, ");
      Serial.print("and their Tx air utilization is ");
      Serial.print(nodeinfo->air_util_tx);
      Serial.println("%.");
    } else {
      Serial.println(" and their device metrics are unknown.");
    };
  }
}

// This callback function will be called repeatedly as the radio's node
// report comes in. For each node received, it will be called with
// the second parameter set to MT_NR_IN_PROGRESS. At the end of the
// report, if the IDs matched, the callback will be called with
// NULL as the first parameter and the second set to either MT_NR_DONE
// (if it was indeed the reply to our request) or MT_NR_INVALID (if it
// turned out to have been a reply to someone else's request).
//
// Everything passed to this callback could be destroyed immediately
// after it returns, so it should save it somewhere else if it needs it.
void node_report_callback(mt_node_t * nodeinfo, mt_nr_progress_t progress) {
  if (progress == MT_NR_IN_PROGRESS) {
    // We're still in the middle of the report, so save this node info
    // for later
    node_infos[node_infos_count++] = *nodeinfo;
    return;
  } else if (progress == MT_NR_INVALID) {
    Serial.println("Oops, ignore all that. It was a reply to someone else's query.");
    return;
  } else if (progress == MT_NR_DONE) {
    // At the end of the reports, we print the info we've collected
    print_node_infos();
    node_infos_count = 0;
    return;
  }
}

void loop() {
  // Record the time that this loop began (in milliseconds since the device booted)
  uint32_t now = millis();

  // Run the Meshtastic loop, and see if it's able to send requests to the device yet
  bool can_send = mt_loop(now);

  // If we can send requests, and it's time to do so, make a request and schedule
  // the next one.
  if (can_send && now >= next_node_report_time) {
    mt_request_node_report(node_report_callback);
    next_node_report_time = now + NODE_REPORT_PERIOD;
  }
}
