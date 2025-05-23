/*
    Meshtastic send/receive client

    Connects to a Meshtastic node via WiFi or Serial (and maybe one day Bluetooth),
    and instructs it to send a text message every SEND_PERIOD milliseconds.
    The destination and channel to use can be specified.

    If the Meshtastic nodes receives a text message, it will call a callback function,
    which prints the message to the serial console.
*/

// Uncomment the line below to enable debugging 
// #define MT_DEBUGGING

#include <Meshtastic.h>

// Pins to use for WiFi; these defaults are for an Adafruit Feather M0 WiFi.
#define WIFI_CS_PIN 8
#define WIFI_IRQ_PIN 7
#define WIFI_RESET_PIN 4
#define WIFI_ENABLE_PIN 2

// Pins to use for SoftwareSerial. Boards that don't use SoftwareSerial, and
// instead provide their own Serial1 connection through fixed pins
// will ignore these settings and use their own.
#define SERIAL_RX_PIN 13
#define SERIAL_TX_PIN 15
// A different baud rate to communicate with the Meshtastic device can be specified here
#define BAUD_RATE 38400

// Send a text message every this many seconds
#define SEND_PERIOD 300

uint32_t next_send_time = 0;
bool not_yet_connected = true;

// This callback function will be called whenever the radio connects to a node
void connected_callback(mt_node_t *node, mt_nr_progress_t progress) {
  if (not_yet_connected) 
    Serial.println("Connected to Meshtastic device!");
  not_yet_connected = false;
}


const char* meshtastic_portnum_to_string(meshtastic_PortNum port) {
  switch (port) {
      case meshtastic_PortNum_UNKNOWN_APP: return "UNKNOWN_APP";
      case meshtastic_PortNum_TEXT_MESSAGE_APP: return "TEXT_MESSAGE_APP";
      case meshtastic_PortNum_REMOTE_HARDWARE_APP: return "REMOTE_HARDWARE_APP";
      case meshtastic_PortNum_POSITION_APP: return "POSITION_APP";
      case meshtastic_PortNum_NODEINFO_APP: return "NODEINFO_APP";
      case meshtastic_PortNum_ROUTING_APP: return "ROUTING_APP";
      case meshtastic_PortNum_ADMIN_APP: return "ADMIN_APP";
      case meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP: return "TEXT_MESSAGE_COMPRESSED_APP";
      case meshtastic_PortNum_WAYPOINT_APP: return "WAYPOINT_APP";
      case meshtastic_PortNum_AUDIO_APP: return "AUDIO_APP";
      case meshtastic_PortNum_DETECTION_SENSOR_APP: return "DETECTION_SENSOR_APP";
      case meshtastic_PortNum_REPLY_APP: return "REPLY_APP";
      case meshtastic_PortNum_IP_TUNNEL_APP: return "IP_TUNNEL_APP";
      case meshtastic_PortNum_PAXCOUNTER_APP: return "PAXCOUNTER_APP";
      case meshtastic_PortNum_SERIAL_APP: return "SERIAL_APP";
      case meshtastic_PortNum_STORE_FORWARD_APP: return "STORE_FORWARD_APP";
      case meshtastic_PortNum_RANGE_TEST_APP: return "RANGE_TEST_APP";
      case meshtastic_PortNum_TELEMETRY_APP: return "TELEMETRY_APP";
      case meshtastic_PortNum_ZPS_APP: return "ZPS_APP";
      case meshtastic_PortNum_SIMULATOR_APP: return "SIMULATOR_APP";
      case meshtastic_PortNum_TRACEROUTE_APP: return "TRACEROUTE_APP";
      case meshtastic_PortNum_NEIGHBORINFO_APP: return "NEIGHBORINFO_APP";
      case meshtastic_PortNum_ATAK_PLUGIN: return "ATAK_PLUGIN";
      case meshtastic_PortNum_MAP_REPORT_APP: return "MAP_REPORT_APP";
      case meshtastic_PortNum_POWERSTRESS_APP: return "POWERSTRESS_APP";
      case meshtastic_PortNum_PRIVATE_APP: return "PRIVATE_APP";
      case meshtastic_PortNum_ATAK_FORWARDER: return "ATAK_FORWARDER";
      case meshtastic_PortNum_MAX: return "MAX";
      default: return "UNKNOWN_PORTNUM";
  }
}

void displayPubKey(meshtastic_MeshPacket_public_key_t pubKey, char *hex_str) {
      for (int i = 0; i < 32; i++) {
          sprintf(&hex_str[i * 2], "%02x", (unsigned char)pubKey.bytes[i]);
      }
  
      hex_str[64] = '\0'; // Null terminator
}


void encrypted_callback(uint32_t from, uint32_t to,  uint8_t channel, meshtastic_MeshPacket_public_key_t pubKey, meshtastic_MeshPacket_encrypted_t *enc_payload) {
  Serial.printf("Received an ENCRYPTED callback from: %x to: %x\r\n", from, to);
}

void portnum_callback(uint32_t from, uint32_t to,  uint8_t channel, meshtastic_PortNum portNum, meshtastic_Data_payload_t *payload) {
  Serial.printf("Received a callback for PortNum %s\r\n", meshtastic_portnum_to_string(portNum));
}

// This callback function will be called whenever the radio receives a text message
void text_message_callback(uint32_t from, uint32_t to,  uint8_t channel, const char* text) {
  // Do your own thing here. This example just prints the message to the serial console.
  Serial.print("Received a text message on channel: ");
  Serial.print(channel);
  Serial.print(" from: ");
  Serial.print(from);
  Serial.print(" to: ");
  Serial.print(to);
  Serial.print(" message: ");
  Serial.println(text);
  if (to == 0xFFFFFFFF){
    Serial.println("This is a BROADCAST message.");
  } else if (to == my_node_num){
    Serial.println("This is a DM to me!");
  } else {
    Serial.println("This is a DM to someone else.");
  }
}

void setup() {
  // Try for up to five seconds to find a serial port; if not, the show must gox on
  Serial.begin(115200);
  while(true) {
    if (Serial) break;
    if (millis() > 5000) {
      Serial.print("Couldn't find a serial port after 5 seconds, continuing anyway");
      break;
    }
  }

  Serial.print("Booted Meshtastic send/receive client in ");

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

  randomSeed(micros());

  // Initial connection to the Meshtastic device
  mt_request_node_report(connected_callback);

  // Register a callback function to be called whenever a text message is received
  set_text_message_callback(text_message_callback);
  set_portnum_callback(portnum_callback);
  set_encrypted_callback(encrypted_callback);
}

void loop() {
  // Record the time that this loop began (in milliseconds since the device booted)
  uint32_t now = millis();

  // Run the Meshtastic loop, and see if it's able to send requests to the device yet
  bool can_send = mt_loop(now);

  // If we can send, and it's time to do so, send a text message and schedule the next one.
  if (can_send && now >= next_send_time) {
    
    // Change this to a specific node number if you want to send to just one node
    uint32_t dest = BROADCAST_ADDR; 
    // Change this to another index if you want to send on a different channel
    uint8_t channel_index = 0; 

    mt_send_text("Hello, world!", dest, channel_index);

    next_send_time = now + SEND_PERIOD * 1000;
  }
}
