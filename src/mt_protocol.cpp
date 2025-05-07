#include "mt_internals.h"

// Magic number at the start of all MT packets
#define MT_MAGIC_0 0x94
#define MT_MAGIC_1 0xc3

// The header is the magic number plus a 16-bit payload-length field
#define MT_HEADER_SIZE 4

// The buffer used for protobuf encoding/decoding. Since there's only one, and it's global, we
// have to make sure we're only ever doing one encoding or decoding at a time.
#define PB_BUFSIZE 512
pb_byte_t pb_buf[PB_BUFSIZE+4];
size_t pb_size = 0; // Number of bytes currently in the buffer

// Nonce to request only my nodeinfo and skip other nodes in the db
#define SPECIAL_NONCE 69420

// Wait this many msec if there's nothing new on the channel
#define NO_NEWS_PAUSE 25

// Serial connections require at least one ping every 15 minutes
// Otherwise the connection is closed, and packets will no longer be received
// We will send a ping every 60 seconds, which is what the web client does
// https://github.com/meshtastic/js/blob/715e35d2374276a43ffa93c628e3710875d43907/src/adapters/serialConnection.ts#L160
#define HEARTBEAT_INTERVAL_MS 60000
uint32_t last_heartbeat_at = 0;

// The ID of the current WANT_CONFIG request
uint32_t want_config_id = 0;

// Node number of the MT node hosting our WiFi
uint32_t my_node_num = 0;

bool mt_debugging = false;
void (*text_message_callback)(uint32_t from, uint32_t to,  uint8_t channel, const char* text) = NULL;
void (*node_report_callback)(mt_node_t *, mt_nr_progress_t) = NULL;
mt_node_t node;

bool mt_wifi_mode = false;
bool mt_serial_mode = false;

void d(const char * s) {
  if (mt_debugging) Serial.println(s);
}

void mt_set_debug(bool on) {
  mt_debugging = on;
}

bool mt_send_radio(const char * buf, size_t len) {
  if (mt_wifi_mode) {
    #ifdef MT_WIFI_SUPPORTED
    return mt_wifi_send_radio(buf, len);
    #else
    return false;
    #endif
  } else if (mt_serial_mode) {
    return mt_serial_send_radio(buf, len);
  } else {
    Serial.println("mt_send_radio() called but it was never initialized");
    while(1);
  }
}

bool _mt_send_toRadio(meshtastic_ToRadio toRadio) {
  pb_buf[0] = MT_MAGIC_0;
  pb_buf[1] = MT_MAGIC_1;

  pb_ostream_t stream = pb_ostream_from_buffer(pb_buf + 4, PB_BUFSIZE);
  bool status = pb_encode(&stream, meshtastic_ToRadio_fields, &toRadio);
  if (!status) {
    d("Couldn't encode toRadio");
    return false;
  }

  // Store the payload length in the header
  pb_buf[2] = stream.bytes_written / 256;
  pb_buf[3] = stream.bytes_written % 256;

  bool rv = mt_send_radio((const char *)pb_buf, 4 + stream.bytes_written);

  // Clear the buffer so it can be used to hold reply packets
  pb_size = 0;

  return rv;
}

// Request a node report from our MT
bool mt_request_node_report(void (*callback)(mt_node_t *, mt_nr_progress_t)) {
  meshtastic_ToRadio toRadio = meshtastic_ToRadio_init_default;
  toRadio.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
  want_config_id = random(0x7FffFFff);  // random() can't handle anything bigger
  toRadio.want_config_id = want_config_id;

  if (mt_debugging) {
    Serial.print("Requesting node report with random ID ");
    Serial.println(want_config_id);
  }

  bool rv = _mt_send_toRadio(toRadio);

  if (rv) node_report_callback = callback;
  return rv;
}

bool mt_send_text(const char * text, uint32_t dest, uint8_t channel_index) {
  meshtastic_MeshPacket meshPacket = meshtastic_MeshPacket_init_default;
  meshPacket.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
  meshPacket.id = random(0x7FFFFFFF);
  meshPacket.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
  meshPacket.to = dest;
  meshPacket.channel = channel_index;
  meshPacket.want_ack = true;
  meshPacket.decoded.payload.size = strlen(text);
  memcpy(meshPacket.decoded.payload.bytes, text, meshPacket.decoded.payload.size);

  meshtastic_ToRadio toRadio = meshtastic_ToRadio_init_default;
  toRadio.which_payload_variant = meshtastic_ToRadio_packet_tag;
  toRadio.packet = meshPacket;
  
  Serial.print("Sending text message '");
  Serial.print(text);
  Serial.print("' to ");
  Serial.println(dest);
  return _mt_send_toRadio(toRadio);
}

bool mt_send_heartbeat() {

  d("Sending heartbeat");

  meshtastic_ToRadio toRadio = meshtastic_ToRadio_init_default;
  toRadio.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
  toRadio.heartbeat = meshtastic_Heartbeat_init_default;

  return _mt_send_toRadio(toRadio);

}

void set_text_message_callback(void (*callback)(uint32_t from, uint32_t to,  uint8_t channel, const char* text)) {
  text_message_callback = callback;
}

bool handle_id_tag(uint32_t id) {
  Serial.printf("Handle_id_tag: ID: %d\r\n", id);
  return true;
}

bool handle_config_tag(meshtastic_Config *config) {
  switch (config->which_payload_variant) {
    case meshtastic_Config_device_tag:
      Serial.printf("Config:device_tag:  role: %d\r\n", config->payload_variant.device.role);
      Serial.printf("Config:device_tag:  serial enabled: %d\r\n", config->payload_variant.device.serial_enabled);
      Serial.printf("Config:device_tag:  button gpio: %d\r\n", config->payload_variant.device.button_gpio);
      Serial.printf("Config:device_tag:  buzzer gpio: %d\r\n", config->payload_variant.device.buzzer_gpio);
      Serial.printf("Config:device_tag:  rebroadcast mode: %d\r\n", config->payload_variant.device.rebroadcast_mode);
      Serial.printf("Config:device_tag:  node_info_broadcast_secs: %d\r\n", config->payload_variant.device.node_info_broadcast_secs);
      Serial.printf("Config:device_tag:  double-tap-as-button-press: %d\r\n", config->payload_variant.device.double_tap_as_button_press);
      Serial.printf("Config:device_tag:  is_managed: %d\r\n", config->payload_variant.device.is_managed);
      Serial.printf("Config:device_tag:  disable_triple_click: %d\r\n", config->payload_variant.device.disable_triple_click);
      Serial.printf("Config:device_tag:  tz_def: %s\r\n", config->payload_variant.device.tzdef);
      Serial.printf("Config:device_tag:  led_heartbeat_disabled: %d\r\n", config->payload_variant.device.led_heartbeat_disabled);
      break;

    case meshtastic_Config_position_tag:
      Serial.printf("Config:position_tag:  position_broadcast_secs: %d\r\n", config->payload_variant.position.position_broadcast_secs);
      Serial.printf("Config:position_tag:  position_broadcast_smart_enabled: %d\r\n", config->payload_variant.position.position_broadcast_smart_enabled);
      Serial.printf("Config:position_tag:  fixed_position: %d\r\n", config->payload_variant.position.fixed_position);
      Serial.printf("Config:position_tag:  gps_enabled: %d\r\n", config->payload_variant.position.gps_enabled);
      Serial.printf("Config:position_tag:  gps_update_interval: %d\r\n", config->payload_variant.position.gps_update_interval);
      Serial.printf("Config:position_tag:  gps_attempt_time: %d\r\n", config->payload_variant.position.gps_attempt_time);
      Serial.printf("Config:position_tag:  position_flags: %d\r\n", config->payload_variant.position.position_flags);
      Serial.printf("Config:position_tag:  rx_gpio: %d\r\n", config->payload_variant.position.rx_gpio);
      Serial.printf("Config:position_tag:  tx_gpio: %d\r\n", config->payload_variant.position.tx_gpio);
      Serial.printf("Config:position_tag:  broadcast_smart_min_distance: %d\r\n", config->payload_variant.position.broadcast_smart_minimum_distance);
      Serial.printf("Config:position_tag:  broadcast_smart_min_interval_secs: %d\r\n", config->payload_variant.position.broadcast_smart_minimum_interval_secs);
      Serial.printf("Config:position_tag:  gps_en_gpio: %d\r\n", config->payload_variant.position.gps_en_gpio);
      Serial.printf("Config:position_tag:  gps_mode %d\r\n", config->payload_variant.position.gps_mode);
      break;

    case meshtastic_Config_power_tag: 
      Serial.printf("Config:power_tag:  is_power_saving %d\r\n", config->payload_variant.power.is_power_saving);
      Serial.printf("Config:power_tag:  on_battery_shutdown_after_secs %d\r\n", config->payload_variant.power.on_battery_shutdown_after_secs);
      Serial.printf("Config:power_tag:  adv_multiplier_override %f\r\n", config->payload_variant.power.adc_multiplier_override);
      Serial.printf("Config:power_tag:  wait_bluetooth_secs %d\r\n", config->payload_variant.power.wait_bluetooth_secs);
      Serial.printf("Config:power_tag:  sds_secs %d\r\n", config->payload_variant.power.sds_secs);
      Serial.printf("Config:power_tag:  ls_secs %d\r\n", config->payload_variant.power.ls_secs);
      Serial.printf("Config:power_tag:  min_wake_secs %d\r\n", config->payload_variant.power.min_wake_secs);
      Serial.printf("Config:power_tag:  device_battery_ina_aaddr %d\r\n", config->payload_variant.power.device_battery_ina_address);
      Serial.printf("Config:power_tag:  powermon_enables %d\r\n", config->payload_variant.power.powermon_enables);
      break;

    case meshtastic_Config_network_tag:
      Serial.printf("Config:network_tag:wifi_enabled: %d  \r\n", config->payload_variant.network.wifi_enabled);
      Serial.printf("Config:network_tag:wifi_ssid: %s  \r\n", config->payload_variant.network.wifi_ssid);
      Serial.printf("Config:network_tag:wifi_psk: %s  \r\n", config->payload_variant.network.wifi_psk);
      Serial.printf("Config:network_tag:ntp_server: %s  \r\n", config->payload_variant.network.ntp_server);
      Serial.printf("Config:network_tag:eth_enabled: %d  \r\n", config->payload_variant.network.eth_enabled);
      Serial.printf("Config:network_tag:addr_mode: %d  \r\n", config->payload_variant.network.address_mode);
      Serial.printf("Config:network_tag:has_ipv4_config: %d  \r\n", config->payload_variant.network.has_ipv4_config);
      Serial.printf("Config:network_tag:ipv4_config: %d  \r\n", config->payload_variant.network.ipv4_config);
      Serial.printf("Config:network_tag:rsyslog_server: %s  \r\n", config->payload_variant.network.rsyslog_server);
      break;

    case meshtastic_Config_display_tag: 
      Serial.printf("Config:display_tag:screen_on_seconds: %d  \r\n", config->payload_variant.display.screen_on_secs);
      Serial.printf("Config:display_tag:gps_format: %d  \r\n", config->payload_variant.display.gps_format);
      Serial.printf("Config:display_tag:auto_screen_carousel_secs: %d  \r\n", config->payload_variant.display.auto_screen_carousel_secs);
      Serial.printf("Config:display_tag:compass_north_top: %d  \r\n", config->payload_variant.display.compass_north_top);
      Serial.printf("Config:display_tag:flip_screen: %d  \r\n", config->payload_variant.display.flip_screen);
      Serial.printf("Config:display_tag:units: %d  \r\n", config->payload_variant.display.units);
      Serial.printf("Config:display_tag:oled: %d  \r\n", config->payload_variant.display.oled);
      Serial.printf("Config:display_tag:displayMode: %d  \r\n", config->payload_variant.display.displaymode);
      Serial.printf("Config:display_tag:heading_bold: %d  \r\n", config->payload_variant.display.heading_bold);
      Serial.printf("Config:display_tag:wake_on_tap_or_motion: %d  \r\n", config->payload_variant.display.wake_on_tap_or_motion);
      Serial.printf("Config:display_tag:compass_orientation: %d  \r\n", config->payload_variant.display.compass_orientation);
      break;

    case meshtastic_Config_lora_tag:
      Serial.printf("Config:lora_tag:use_preset: %d  \r\n", config->payload_variant.lora.use_preset);
      Serial.printf("Config:lora_tag:modem_preset: %d  \r\n", config->payload_variant.lora.modem_preset);
      Serial.printf("Config:lora_tag:bandwidth: %d  \r\n", config->payload_variant.lora.bandwidth);
      Serial.printf("Config:lora_tag:spread_factor: %d  \r\n", config->payload_variant.lora.spread_factor);
      Serial.printf("Config:lora_tag:coding_rate: %d  \r\n", config->payload_variant.lora.coding_rate);
      Serial.printf("Config:lora_tag:frequency_offset: %d  \r\n", config->payload_variant.lora.frequency_offset);
      Serial.printf("Config:lora_tag:region: %d  \r\n", config->payload_variant.lora.region);
      Serial.printf("Config:lora_tag:hot_limit: %d  \r\n", config->payload_variant.lora.hop_limit);
      Serial.printf("Config:lora_tag:tx_enabled: %d  \r\n", config->payload_variant.lora.tx_enabled);
      Serial.printf("Config:lora_tag:tx_power: %d  \r\n", config->payload_variant.lora.tx_power);
      Serial.printf("Config:lora_tag:channel_num: %d  \r\n", config->payload_variant.lora.channel_num);
      Serial.printf("Config:lora_tag:override_duty_cycle: %d  \r\n", config->payload_variant.lora.override_duty_cycle);
      Serial.printf("Config:lora_tag:sx126x_rx_boosted_gain: %d  \r\n", config->payload_variant.lora.sx126x_rx_boosted_gain);
      Serial.printf("Config:lora_tag:override_frequency: %d  \r\n", config->payload_variant.lora.override_frequency);
      Serial.printf("Config:lora_tag:pa_fan_disabled: %d  \r\n", config->payload_variant.lora.pa_fan_disabled);
      Serial.printf("Config:lora_tag:ignore_incoming_count: %d  \r\n", config->payload_variant.lora.ignore_incoming_count);
      Serial.printf("Config:lora_tag:ignore_mqtt: %d  \r\n", config->payload_variant.lora.ignore_mqtt);
      Serial.printf("Config:lora_tag:config_okay_to_mqtt: %d  \r\n", config->payload_variant.lora.config_ok_to_mqtt);
      break;

    case meshtastic_Config_bluetooth_tag: 
      Serial.printf("Config:bluetooth_tag:  \r\n");
      break;

    case meshtastic_Config_security_tag: 
      Serial.printf("Config:security_tag:  \r\n");
      break;

    case meshtastic_Config_sessionkey_tag: 
      Serial.printf("Config:sessionkey_tag:  \r\n");
      break;

    default:
      Serial.printf("Unknown Config_Tag payload variant: %d\r\n", config->which_payload_variant);
  }
  return true;
}

bool handle_channel_tag(meshtastic_Channel *channel) {
  Serial.printf("Handle_channel_tag %d\r\n", channel->index);
  return true;
}

bool handle_FromRadio_log_record_tag(meshtastic_LogRecord *record) {
  Serial.printf("Handle_FromRadio_log_record_tag %s\r\n", record->message);
  return true;
}

bool handle_moduleConfig_tag(meshtastic_ModuleConfig *module){ 
  switch (module->which_payload_variant) {
      case meshtastic_ModuleConfig_mqtt_tag:
      Serial.printf("ModuleConfig:mqtt:enabled: %d\r\n", module->payload_variant.mqtt.enabled);
      Serial.printf("ModuleConfig:mqtt:address: %s\r\n", module->payload_variant.mqtt.address);
      Serial.printf("ModuleConfig:mqtt:username: %s\r\n", module->payload_variant.mqtt.username);
      Serial.printf("ModuleConfig:mqtt:password: %s\r\n", module->payload_variant.mqtt.password);
      Serial.printf("ModuleConfig:mqtt:encryption_enabled: %d\r\n", module->payload_variant.mqtt.encryption_enabled);
      Serial.printf("ModuleConfig:mqtt:json_enabled: %d\r\n", module->payload_variant.mqtt.json_enabled);
      Serial.printf("ModuleConfig:mqtt:root: %s\r\n", module->payload_variant.mqtt.root);
      Serial.printf("ModuleConfig:mqtt:proxy_to_client_enabled: %d\r\n", module->payload_variant.mqtt.proxy_to_client_enabled);
      Serial.printf("ModuleConfig:mqtt:map_reporting_enabled %d\r\n", module->payload_variant.mqtt.map_report_settings);
      Serial.printf("ModuleConfig:mqtt:has_map_report_settings%d\r\n", module->payload_variant.mqtt.has_map_report_settings);
      break;

      case meshtastic_ModuleConfig_serial_tag:
      break;

      case meshtastic_ModuleConfig_external_notification_tag:
      break;

      case meshtastic_ModuleConfig_store_forward_tag:
      break;

      case meshtastic_ModuleConfig_range_test_tag: 
      break;

      case meshtastic_ModuleConfig_telemetry_tag:
      break;

      case meshtastic_ModuleConfig_canned_message_tag: 
      break;

      case meshtastic_ModuleConfig_audio_tag: 
      break;

      case meshtastic_ModuleConfig_remote_hardware_tag: 
      break;

      case meshtastic_ModuleConfig_neighbor_info_tag: 
      break;

      case meshtastic_ModuleConfig_ambient_lighting_tag:
      break;

      case meshtastic_ModuleConfig_detection_sensor_tag: 
      break;

      case meshtastic_ModuleConfig_paxcounter_tag:
      break;

      default:
        Serial.printf("Unknown payload variant: %d\r\n", module->which_payload_variant);
  }
  return true;
}

bool handle_queueStatus_tag(meshtastic_QueueStatus *qstatus) {
  Serial.printf("Handle_queueStatus_tag maxlen: %d\r\n", qstatus->maxlen);
  return true;
}

bool handle_xmodemPacket_tag(meshtastic_XModem *packet) {
  Serial.printf("Handle_xmodem_tag XModem Sequence # %d\r\n", packet->seq);
  return true;
}

bool handle_metatag_data(meshtastic_DeviceMetadata *meta) {
  Serial.printf("Handle_metatag_data FW Version: %s\r\n", meta->firmware_version);
  return true;
}

bool handle_mqttClientProxyMessage_tag(meshtastic_MqttClientProxyMessage *mqtt) {
  Serial.printf("Handle_mqttClientProxyMessage_tag Topic: %s\r\n", mqtt->topic);
  return true;
}

bool handle_fileInfo_tag(meshtastic_FileInfo *fInfo) {
  Serial.printf("Handle_fileInfo_tag FileName: %s\r\n", fInfo->file_name);
  return true;
}

bool handle_my_info(meshtastic_MyNodeInfo *myNodeInfo) {
  my_node_num = myNodeInfo->my_node_num;
  return true;
}

bool handle_node_info(meshtastic_NodeInfo *nodeInfo) {
  if (node_report_callback == NULL) {
    d("Got a node report, but we don't have a callback");
    return false;
  }
  node.node_num = nodeInfo->num;
  node.is_mine = nodeInfo->num == my_node_num;
  node.last_heard_from = nodeInfo->last_heard;
  node.has_user = nodeInfo->has_user;
  if (node.has_user) {
    memcpy(node.user_id, nodeInfo->user.id, MAX_USER_ID_LEN);
    memcpy(node.long_name, nodeInfo->user.long_name, MAX_LONG_NAME_LEN);
    memcpy(node.short_name, nodeInfo->user.short_name, MAX_SHORT_NAME_LEN);
  }

  if (nodeInfo->has_position) {
    node.latitude = nodeInfo->position.latitude_i / 1e7;
    node.longitude = nodeInfo->position.longitude_i / 1e7;
    node.altitude = nodeInfo->position.altitude;
    node.ground_speed = nodeInfo->position.ground_speed;
    node.last_heard_position = nodeInfo->position.time;
    node.time_of_last_position = nodeInfo->position.timestamp;
  } else {
    node.latitude = NAN;
    node.longitude = NAN;
    node.altitude = 0;
    node.ground_speed = 0;
    node.battery_level = 0;
    node.last_heard_position = 0;
    node.time_of_last_position = 0;
  }
  if (nodeInfo->has_device_metrics) {
    node.battery_level = nodeInfo->device_metrics.battery_level;
    node.voltage = nodeInfo->device_metrics.voltage;
    node.channel_utilization = nodeInfo->device_metrics.channel_utilization;
    node.air_util_tx = nodeInfo->device_metrics.air_util_tx;
  } else {
    node.battery_level = 0;
    node.voltage = NAN;
    node.channel_utilization = NAN; 
    node.air_util_tx = NAN;
  }

  node_report_callback(&node, MT_NR_IN_PROGRESS);
  return true;
}

bool handle_config_complete_id(uint32_t now, uint32_t config_complete_id) {
  if (config_complete_id == want_config_id) {
    #ifdef MT_WIFI_SUPPORTED
    mt_wifi_reset_idle_timeout(now);  // It's fine if we're actually in serial mode
    #endif
    want_config_id = 0;
    node_report_callback(NULL, MT_NR_DONE);
    node_report_callback = NULL;
  } else {
    node_report_callback(NULL, MT_NR_INVALID);  // but return true, since it was still a valid packet
  }
  return true;
}

bool handle_mesh_packet(meshtastic_MeshPacket *meshPacket) {
  if (meshPacket->which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
    if (meshPacket->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
      if (text_message_callback != NULL)
        text_message_callback(meshPacket->from, meshPacket->to, meshPacket->channel, (const char*)meshPacket->decoded.payload.bytes);
    } else {
      // TODO handle other portnums
      return false;
    }
  } else {
    return false;
  }
  return true;
}

// Parse a packet that came in, and handle it. Return true if we were able to parse it.
bool handle_packet(uint32_t now, size_t payload_len) {
  meshtastic_FromRadio fromRadio = meshtastic_FromRadio_init_zero;

  // Decode the protobuf and shift forward any remaining bytes in the buffer (which, if
  // present, belong to the packet that we're going to process on the next loop)
  pb_istream_t stream = pb_istream_from_buffer(pb_buf + 4, payload_len);
  bool status = pb_decode(&stream, meshtastic_FromRadio_fields, &fromRadio);
  memmove(pb_buf, pb_buf+4+payload_len, PB_BUFSIZE-4-payload_len);
  pb_size -= 4 + payload_len;

  // Be prepared to request a node report to re-establish flow after an MT reboot
  meshtastic_ToRadio toRadio = meshtastic_ToRadio_init_default;
  toRadio.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
  toRadio.want_config_id = SPECIAL_NONCE;

  if (!status) {
    d("Decoding failed");
    return false;
  }

  switch (fromRadio.which_payload_variant) {
    case meshtastic_FromRadio_id_tag: // 1
      return handle_id_tag(fromRadio.id);
    case meshtastic_FromRadio_packet_tag: //2
      return handle_mesh_packet(&fromRadio.packet);
    case meshtastic_FromRadio_my_info_tag: // 3
      return handle_my_info(&fromRadio.my_info);
    case meshtastic_FromRadio_node_info_tag: // 4
      return handle_node_info(&fromRadio.node_info);
    case meshtastic_FromRadio_config_tag : // 5
      return handle_config_tag(&fromRadio.config);
    case meshtastic_FromRadio_log_record_tag: // 6
      return handle_FromRadio_log_record_tag(&fromRadio.log_record);
    case meshtastic_FromRadio_config_complete_id_tag: // 7
      return handle_config_complete_id(now, fromRadio.config_complete_id);
    case meshtastic_FromRadio_rebooted_tag: // 8
      _mt_send_toRadio(toRadio);

    case  meshtastic_FromRadio_moduleConfig_tag: // 9
      return handle_moduleConfig_tag(&fromRadio.moduleConfig);
    case meshtastic_FromRadio_channel_tag: // 10
      return handle_channel_tag(&fromRadio.channel);
    case meshtastic_FromRadio_queueStatus_tag: // 11
      return handle_queueStatus_tag(&fromRadio.queueStatus); 
    case  meshtastic_FromRadio_xmodemPacket_tag: // 12
      return handle_xmodemPacket_tag(&fromRadio.xmodemPacket);
    case meshtastic_FromRadio_metadata_tag: //        13
      return handle_metatag_data(&fromRadio.metadata);
    case meshtastic_FromRadio_mqttClientProxyMessage_tag: // 14
      return handle_mqttClientProxyMessage_tag(&fromRadio.mqttClientProxyMessage);
    case meshtastic_FromRadio_fileInfo_tag :  // 15
      return handle_fileInfo_tag(&fromRadio.fileInfo); 

    default:
      if (mt_debugging) {
        // Rate limit
        // Serial input buffer overflows during initial connection, while we're slowly printing these at 9600 baud
        constexpr uint32_t limitMs = 100; 
        static uint32_t lastLog = 0;
        uint32_t now = millis();
        if (now - lastLog > limitMs) {
            lastLog = now;
            Serial.print("Got a payloadVariant we don't recognize: ");
            Serial.println(fromRadio.which_payload_variant);
        }
      }
      return false;
  }

  d("Handled a packet");
}

void mt_protocol_check_packet(uint32_t now) {
  if (pb_size < MT_HEADER_SIZE) {
    // We don't even have a header yet
    delay(NO_NEWS_PAUSE);
    return;
  }

  if (pb_buf[0] != MT_MAGIC_0 || pb_buf[1] != MT_MAGIC_1) {
    d("Got bad magic");
    memset(pb_buf, 0, PB_BUFSIZE);
    pb_size = 0;
    return;
  }

  uint16_t payload_len = pb_buf[2] << 8 | pb_buf[3];
  if (payload_len > PB_BUFSIZE) {
    d("Got packet claiming to be ridiculous length");
    return;
  }

  if ((size_t)(payload_len + 4) > pb_size) {
    // d("Partial packet");
    delay(NO_NEWS_PAUSE);
    return;
  }

  /*
  if (mt_debugging) {
    Serial.print("Got a full packet! ");
    for (int i = 0 ; i < pb_size ; i++) {
      Serial.print(pb_buf[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
  }
  */

  handle_packet(now, payload_len);
}

bool mt_loop(uint32_t now) {
  bool rv;
  size_t bytes_read = 0;

  // See if there are any more bytes to add to our buffer.
  size_t space_left = PB_BUFSIZE - pb_size;
 
  if (mt_wifi_mode) {
#ifdef MT_WIFI_SUPPORTED
    rv = mt_wifi_loop(now);
    if (rv) bytes_read = mt_wifi_check_radio((char *)pb_buf + pb_size, space_left);
#else
    return false;
#endif
  } else if (mt_serial_mode) {

    rv = mt_serial_loop();
    if (rv) bytes_read = mt_serial_check_radio((char *)pb_buf + pb_size, space_left);

    // if heartbeat interval has passed, send a heartbeat to keep serial connection alive
    if(now >= (last_heartbeat_at + HEARTBEAT_INTERVAL_MS)){
        mt_send_heartbeat();
        last_heartbeat_at = now;
    }

  } else {
    Serial.println("mt_loop() called but it was never initialized");
    while(1);
  }

  pb_size += bytes_read;
  mt_protocol_check_packet(now); 
  return rv;
}
