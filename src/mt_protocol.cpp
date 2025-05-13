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

void (*text_message_callback)(uint32_t from, uint32_t to,  uint8_t channel, const char* text) = NULL;
void (*portnum_callback)(uint32_t from, uint32_t to,  uint8_t channel, meshtastic_PortNum port, meshtastic_Data_payload_t *payload) = NULL;
void (*encrypted_callback)(uint32_t from, uint32_t to,  uint8_t channel, meshtastic_MeshPacket_public_key_t pubKey, meshtastic_MeshPacket_encrypted_t *enc_payload) = NULL;

void (*node_report_callback)(mt_node_t *, mt_nr_progress_t) = NULL;
mt_node_t node;

bool mt_wifi_mode = false;
bool mt_serial_mode = false;

#define VA_BUFSIZE 512
void _d(const char * fmt, ...) {
  static char vabuf[VA_BUFSIZE];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(vabuf, sizeof(vabuf), fmt, ap);
  Serial.println(vabuf);
  Serial.flush();
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

#ifdef MT_DEBUGGING
  Serial.print("Requesting node report with random ID ");
  Serial.println(want_config_id);
#endif

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

void set_portnum_callback(void (*callback)(uint32_t from, uint32_t to,  uint8_t channel, meshtastic_PortNum port, meshtastic_Data_payload_t *payload)) {
  portnum_callback = callback;
}

void set_encrypted_callback(void (*callback)(uint32_t from, uint32_t to,  uint8_t channel, meshtastic_MeshPacket_public_key_t pubKey, meshtastic_MeshPacket_encrypted_t *payload)) {
  encrypted_callback = callback;
}

void set_text_message_callback(void (*callback)(uint32_t from, uint32_t to,  uint8_t channel, const char* text)) {
  text_message_callback = callback;
}

bool handle_id_tag(uint32_t id) {
  d("id_tag: ID: %d\r\n", id);
  return true;
}

bool handle_config_tag(meshtastic_Config *config) {
  switch (config->which_payload_variant) {
    case meshtastic_Config_device_tag:
      d("Config:device_tag:  role: %d\r\n", config->payload_variant.device.role);
      d("Config:device_tag:  serial enabled: %d\r\n", config->payload_variant.device.serial_enabled);
      d("Config:device_tag:  button gpio: %d\r\n", config->payload_variant.device.button_gpio);
      d("Config:device_tag:  buzzer gpio: %d\r\n", config->payload_variant.device.buzzer_gpio);
      d("Config:device_tag:  rebroadcast mode: %d\r\n", config->payload_variant.device.rebroadcast_mode);
      d("Config:device_tag:  node_info_broadcast_secs: %d\r\n", config->payload_variant.device.node_info_broadcast_secs);
      d("Config:device_tag:  double-tap-as-button-press: %d\r\n", config->payload_variant.device.double_tap_as_button_press);
      d("Config:device_tag:  is_managed: %d\r\n", config->payload_variant.device.is_managed);
      d("Config:device_tag:  disable_triple_click: %d\r\n", config->payload_variant.device.disable_triple_click);
      d("Config:device_tag:  tz_def: %s\r\n", config->payload_variant.device.tzdef);
      d("Config:device_tag:  led_heartbeat_disabled: %d\r\n", config->payload_variant.device.led_heartbeat_disabled);
      break;

    case meshtastic_Config_position_tag:
      d("Config:position_tag:  position_broadcast_secs: %d\r\n", config->payload_variant.position.position_broadcast_secs);
      d("Config:position_tag:  position_broadcast_smart_enabled: %d\r\n", config->payload_variant.position.position_broadcast_smart_enabled);
      d("Config:position_tag:  fixed_position: %d\r\n", config->payload_variant.position.fixed_position);
      d("Config:position_tag:  gps_enabled: %d\r\n", config->payload_variant.position.gps_enabled);
      d("Config:position_tag:  gps_update_interval: %d\r\n", config->payload_variant.position.gps_update_interval);
      d("Config:position_tag:  gps_attempt_time: %d\r\n", config->payload_variant.position.gps_attempt_time);
      d("Config:position_tag:  position_flags: %d\r\n", config->payload_variant.position.position_flags);
      d("Config:position_tag:  rx_gpio: %d\r\n", config->payload_variant.position.rx_gpio);
      d("Config:position_tag:  tx_gpio: %d\r\n", config->payload_variant.position.tx_gpio);
      d("Config:position_tag:  broadcast_smart_min_distance: %d\r\n", config->payload_variant.position.broadcast_smart_minimum_distance);
      d("Config:position_tag:  broadcast_smart_min_interval_secs: %d\r\n", config->payload_variant.position.broadcast_smart_minimum_interval_secs);
      d("Config:position_tag:  gps_en_gpio: %d\r\n", config->payload_variant.position.gps_en_gpio);
      d("Config:position_tag:  gps_mode %d\r\n", config->payload_variant.position.gps_mode);
      break;

    case meshtastic_Config_power_tag: 
      d("Config:power_tag:  is_power_saving %d\r\n", config->payload_variant.power.is_power_saving);
      d("Config:power_tag:  on_battery_shutdown_after_secs %d\r\n", config->payload_variant.power.on_battery_shutdown_after_secs);
      d("Config:power_tag:  adv_multiplier_override %f\r\n", config->payload_variant.power.adc_multiplier_override);
      d("Config:power_tag:  wait_bluetooth_secs %d\r\n", config->payload_variant.power.wait_bluetooth_secs);
      d("Config:power_tag:  sds_secs %d\r\n", config->payload_variant.power.sds_secs);
      d("Config:power_tag:  ls_secs %d\r\n", config->payload_variant.power.ls_secs);
      d("Config:power_tag:  min_wake_secs %d\r\n", config->payload_variant.power.min_wake_secs);
      d("Config:power_tag:  device_battery_ina_aaddr %d\r\n", config->payload_variant.power.device_battery_ina_address);
      d("Config:power_tag:  powermon_enables %d\r\n", config->payload_variant.power.powermon_enables);
      break;

    case meshtastic_Config_network_tag:
      d("Config:network_tag:wifi_enabled: %d  \r\n", config->payload_variant.network.wifi_enabled);
      d("Config:network_tag:wifi_ssid: %s  \r\n", config->payload_variant.network.wifi_ssid);
      d("Config:network_tag:wifi_psk: %s  \r\n", config->payload_variant.network.wifi_psk);
      d("Config:network_tag:ntp_server: %s  \r\n", config->payload_variant.network.ntp_server);
      d("Config:network_tag:eth_enabled: %d  \r\n", config->payload_variant.network.eth_enabled);
      d("Config:network_tag:addr_mode: %d  \r\n", config->payload_variant.network.address_mode);
      d("Config:network_tag:has_ipv4_config: %d  \r\n", config->payload_variant.network.has_ipv4_config);
      d("Config:network_tag:ipv4_config: %d  \r\n", config->payload_variant.network.ipv4_config);
      d("Config:network_tag:rsyslog_server: %s  \r\n", config->payload_variant.network.rsyslog_server);
      break;

    case meshtastic_Config_display_tag: 
      d("Config:display_tag:screen_on_seconds: %d  \r\n", config->payload_variant.display.screen_on_secs);
      d("Config:display_tag:gps_format: %d  \r\n", config->payload_variant.display.gps_format);
      d("Config:display_tag:auto_screen_carousel_secs: %d  \r\n", config->payload_variant.display.auto_screen_carousel_secs);
      d("Config:display_tag:compass_north_top: %d  \r\n", config->payload_variant.display.compass_north_top);
      d("Config:display_tag:flip_screen: %d  \r\n", config->payload_variant.display.flip_screen);
      d("Config:display_tag:units: %d  \r\n", config->payload_variant.display.units);
      d("Config:display_tag:oled: %d  \r\n", config->payload_variant.display.oled);
      d("Config:display_tag:displayMode: %d  \r\n", config->payload_variant.display.displaymode);
      d("Config:display_tag:heading_bold: %d  \r\n", config->payload_variant.display.heading_bold);
      d("Config:display_tag:wake_on_tap_or_motion: %d  \r\n", config->payload_variant.display.wake_on_tap_or_motion);
      d("Config:display_tag:compass_orientation: %d  \r\n", config->payload_variant.display.compass_orientation);
      break;

    case meshtastic_Config_lora_tag:
      d("Config:lora_tag:use_preset: %d  \r\n", config->payload_variant.lora.use_preset);
      d("Config:lora_tag:modem_preset: %d  \r\n", config->payload_variant.lora.modem_preset);
      d("Config:lora_tag:bandwidth: %d  \r\n", config->payload_variant.lora.bandwidth);
      d("Config:lora_tag:spread_factor: %d  \r\n", config->payload_variant.lora.spread_factor);
      d("Config:lora_tag:coding_rate: %d  \r\n", config->payload_variant.lora.coding_rate);
      d("Config:lora_tag:frequency_offset: %d  \r\n", config->payload_variant.lora.frequency_offset);
      d("Config:lora_tag:region: %d  \r\n", config->payload_variant.lora.region);
      d("Config:lora_tag:hot_limit: %d  \r\n", config->payload_variant.lora.hop_limit);
      d("Config:lora_tag:tx_enabled: %d  \r\n", config->payload_variant.lora.tx_enabled);
      d("Config:lora_tag:tx_power: %d  \r\n", config->payload_variant.lora.tx_power);
      d("Config:lora_tag:channel_num: %d  \r\n", config->payload_variant.lora.channel_num);
      d("Config:lora_tag:override_duty_cycle: %d  \r\n", config->payload_variant.lora.override_duty_cycle);
      d("Config:lora_tag:sx126x_rx_boosted_gain: %d  \r\n", config->payload_variant.lora.sx126x_rx_boosted_gain);
      d("Config:lora_tag:override_frequency: %d  \r\n", config->payload_variant.lora.override_frequency);
      d("Config:lora_tag:pa_fan_disabled: %d  \r\n", config->payload_variant.lora.pa_fan_disabled);
      d("Config:lora_tag:ignore_incoming_count: %d  \r\n", config->payload_variant.lora.ignore_incoming_count);
      d("Config:lora_tag:ignore_mqtt: %d  \r\n", config->payload_variant.lora.ignore_mqtt);
      d("Config:lora_tag:config_okay_to_mqtt: %d  \r\n", config->payload_variant.lora.config_ok_to_mqtt);
      break;

    case meshtastic_Config_bluetooth_tag: 
      d("Config:bluetooth_tag:enabled: %d  \r\n", config->payload_variant.bluetooth.enabled);
      d("Config:bluetooth_tag:fixed_pin: %d  \r\n", config->payload_variant.bluetooth.fixed_pin);
      d("Config:bluetooth_tag:mode: %d  \r\n", config->payload_variant.bluetooth.mode);
      break;

    case meshtastic_Config_security_tag: 
      d("Config:security_tag:is_managed: %d \r\n", config->payload_variant.security.is_managed);
      d("Config:security_tag:public_key: %x \r\n", config->payload_variant.security.public_key);
      d("Config:security_tag:private_key: %x \r\n", config->payload_variant.security.private_key);
      d("Config:security_tag:admin_key_count: %x \r\n", config->payload_variant.security.admin_key_count);
      d("Config:security_tag:serial_enabled: %x \r\n", config->payload_variant.security.serial_enabled);
      d("Config:security_tag:debug_log_api_enabled: %x \r\n", config->payload_variant.security.debug_log_api_enabled);
      d("Config:security_tag:admin_channel_enabled: %x \r\n", config->payload_variant.security.admin_channel_enabled);
      break;

    case meshtastic_Config_sessionkey_tag: 
      d("Config:sessionkey_tag:dummy_field: %x \r\n", config->payload_variant.sessionkey.dummy_field);
      break;

    case meshtastic_Config_device_ui_tag:
      d("Config.device_ui:alert_enabled: %d\r\n", config->payload_variant.device_ui.alert_enabled);
      d("Config.device_ui:banner_enabled: %d\r\n", config->payload_variant.device_ui.banner_enabled);
      d("Config.device_ui:has_node_filter: %d\r\n", config->payload_variant.device_ui.has_node_filter);
      d("Config.device_ui:has_node_highlight: %d\r\n", config->payload_variant.device_ui.has_node_highlight);
      d("Config.device_ui:language: %d\r\n", config->payload_variant.device_ui.language);
      d("Config.device_ui:node_filter: %d\r\n", config->payload_variant.device_ui.node_filter);
      d("Config.device_ui:node_highlight: %d\r\n", config->payload_variant.device_ui.node_highlight);
      d("Config.device_ui:pin_code: %d\r\n", config->payload_variant.device_ui.pin_code);
      d("Config.device_ui:ring_tone_id: %d\r\n", config->payload_variant.device_ui.ring_tone_id);
      d("Config.device_ui:screen_brightness: %d\r\n", config->payload_variant.device_ui.screen_brightness);
      d("Config.device_ui:screen_lock: %d\r\n", config->payload_variant.device_ui.screen_lock);
      d("Config.device_ui:screen_timeout: %d\r\n", config->payload_variant.device_ui.screen_timeout);
      break;

    default:
      d("Unknown Config_Tag payload variant: %d\r\n", config->which_payload_variant);
  }
  return true;
}

bool handle_channel_tag(meshtastic_Channel *channel) {
  d("ChannelTag:index: %d\r\n", channel->index);
  d("ChannelTag:has_settings: %d\r\n", channel->has_settings);
  d("ChannelTag:role: %d\r\n", channel->role);
  return true;
}

bool handle_FromRadio_log_record_tag(meshtastic_LogRecord *record) {
  d("FromRadio_log_record:message: %s\r\n", record->message);
  d("FromRadio_log_record:time: %d\r\n", record->time);
  d("FromRadio_log_record:source: %s\r\n", record->source);
  d("FromRadio_log_record:level: %s\r\n", record->level);
  return true;
}

bool handle_moduleConfig_tag(meshtastic_ModuleConfig *module){ 
  switch (module->which_payload_variant) {
      case meshtastic_ModuleConfig_mqtt_tag:
      d("ModuleConfig:mqtt:enabled: %d\r\n", module->payload_variant.mqtt.enabled);
      d("ModuleConfig:mqtt:address: %s\r\n", module->payload_variant.mqtt.address);
      d("ModuleConfig:mqtt:username: %s\r\n", module->payload_variant.mqtt.username);
      d("ModuleConfig:mqtt:password: %s\r\n", module->payload_variant.mqtt.password);
      d("ModuleConfig:mqtt:encryption_enabled: %d\r\n", module->payload_variant.mqtt.encryption_enabled);
      d("ModuleConfig:mqtt:json_enabled: %d\r\n", module->payload_variant.mqtt.json_enabled);
      d("ModuleConfig:mqtt:root: %s\r\n", module->payload_variant.mqtt.root);
      d("ModuleConfig:mqtt:proxy_to_client_enabled: %d\r\n", module->payload_variant.mqtt.proxy_to_client_enabled);
      d("ModuleConfig:mqtt:map_reporting_enabled %d\r\n", module->payload_variant.mqtt.map_report_settings);
      d("ModuleConfig:mqtt:has_map_report_settings %d\r\n", module->payload_variant.mqtt.has_map_report_settings);
      break;

      case meshtastic_ModuleConfig_serial_tag:
        d("ModuleConfig:serial:enabled: %d\r\n", module->payload_variant.serial.enabled);
        d("ModuleConfig:serial:echo: %d\r\n", module->payload_variant.serial.echo);
        d("ModuleConfig:serial:rxd-gpio-pin: %d\r\n", module->payload_variant.serial.rxd);
        d("ModuleConfig:serial:txd-gpio-pin: %d\r\n", module->payload_variant.serial.txd);
        d("ModuleConfig:serial:baud: %d\r\n", module->payload_variant.serial.baud);
        d("ModuleConfig:serial:timeout: %d\r\n", module->payload_variant.serial.timeout);
        d("ModuleConfig:serial:mode: %d\r\n", module->payload_variant.serial.mode);
        d("ModuleConfig:serial:override_console_serial_port: %d\r\n", module->payload_variant.serial.override_console_serial_port);
      break;

      case meshtastic_ModuleConfig_external_notification_tag:
        d("ModuleConfig:external_notification:enabled: %d\r\n", module->payload_variant.external_notification.enabled);
        d("ModuleConfig:external_notification:output_ms: %d\r\n", module->payload_variant.external_notification.output_ms);
        d("ModuleConfig:external_notification:output: %d\r\n", module->payload_variant.external_notification.output);
        d("ModuleConfig:external_notification:active: %d\r\n", module->payload_variant.external_notification.active);
        d("ModuleConfig:external_notification:alert_message: %d\r\n", module->payload_variant.external_notification.alert_message);
        d("ModuleConfig:external_notification:alert_bell: %d\r\n", module->payload_variant.external_notification.alert_bell);
        d("ModuleConfig:external_notification:use_pwm: %d\r\n", module->payload_variant.external_notification.use_pwm);
        d("ModuleConfig:external_notification:output_vibra: %d\r\n", module->payload_variant.external_notification.output_vibra);
        d("ModuleConfig:external_notification:output_buzzer: %d\r\n", module->payload_variant.external_notification.output_buzzer);
        d("ModuleConfig:external_notification:alert_message_vibra: %d\r\n", module->payload_variant.external_notification.alert_message_vibra);
        d("ModuleConfig:external_notification:alert_message_buzzer: %d\r\n", module->payload_variant.external_notification.alert_message_buzzer);
        d("ModuleConfig:external_notification:alert_bell_vibra: %d\r\n", module->payload_variant.external_notification.alert_bell_vibra);
        d("ModuleConfig:external_notification:alert_bell_buzzer: %d\r\n", module->payload_variant.external_notification.alert_bell_buzzer);
        d("ModuleConfig:external_notification:nag_timeout: %d\r\n", module->payload_variant.external_notification.nag_timeout);
        d("ModuleConfig:external_notification:use_i2s_as_buzzer: %d\r\n", module->payload_variant.external_notification.use_i2s_as_buzzer);
      break;

      case meshtastic_ModuleConfig_store_forward_tag:
        d("ModuleConfig:store_forward:enabled: %d\r\n", module->payload_variant.store_forward.enabled);
        d("ModuleConfig:store_forward:heartbeat: %d\r\n", module->payload_variant.store_forward.heartbeat);
        d("ModuleConfig:store_forward:history_return_max: %d\r\n", module->payload_variant.store_forward.history_return_max);
        d("ModuleConfig:store_forward:history_return_window: %d\r\n", module->payload_variant.store_forward.history_return_window);
        d("ModuleConfig:store_forward:is_server: %d\r\n", module->payload_variant.store_forward.is_server);
        d("ModuleConfig:store_forward:records: %d\r\n", module->payload_variant.store_forward.records);
      break;

      case meshtastic_ModuleConfig_range_test_tag: 
        d("ModuleConfig:range_test:enabled: %d\r\n", module->payload_variant.range_test.enabled);
        d("ModuleConfig:range_test:save: %d\r\n", module->payload_variant.range_test.save);
        d("ModuleConfig:range_test:sender: %d\r\n", module->payload_variant.range_test.sender);
      break;

      case meshtastic_ModuleConfig_telemetry_tag:
        d("ModuleConfig:telemetry:air_quality_enabled: %d\r\n", module->payload_variant.telemetry.air_quality_enabled);
        d("ModuleConfig:telemetry:air_quality_interval: %d\r\n", module->payload_variant.telemetry.air_quality_interval);
        d("ModuleConfig:telemetry:device_update_interval: %d\r\n", module->payload_variant.telemetry.device_update_interval);
        d("ModuleConfig:telemetry:environment_display_fahrenheit: %d\r\n", module->payload_variant.telemetry.environment_display_fahrenheit);
        d("ModuleConfig:telemetry:environment_measurement_enabled: %d\r\n", module->payload_variant.telemetry.environment_measurement_enabled);
        d("ModuleConfig:telemetry:environment_screen_enabled: %d\r\n", module->payload_variant.telemetry.environment_screen_enabled);
        d("ModuleConfig:telemetry:environment_update_interval: %d\r\n", module->payload_variant.telemetry.environment_update_interval);
        d("ModuleConfig:telemetry:health_measurement_enabled: %d\r\n", module->payload_variant.telemetry.health_measurement_enabled);
        d("ModuleConfig:telemetry:health_screen_enabled: %d\r\n", module->payload_variant.telemetry.health_screen_enabled);
        d("ModuleConfig:telemetry:health_update_interval: %d\r\n", module->payload_variant.telemetry.health_update_interval);
        d("ModuleConfig:telemetry:power_measurement_enabled: %d\r\n", module->payload_variant.telemetry.power_measurement_enabled);
        d("ModuleConfig:telemetry:power_update_interval: %d\r\n", module->payload_variant.telemetry.power_update_interval);

      break;

      case meshtastic_ModuleConfig_canned_message_tag: 
        d("ModuleConfig:canned_message:enabled: %d\r\n", module->payload_variant.canned_message.enabled);
        d("ModuleConfig:canned_message:allow_input_source: %d\r\n", module->payload_variant.canned_message.allow_input_source);
        d("ModuleConfig:canned_message:inputbroker_event_ccw: %d\r\n", module->payload_variant.canned_message.inputbroker_event_ccw);
        d("ModuleConfig:canned_message:inputbroker_event_cw: %d\r\n", module->payload_variant.canned_message.inputbroker_event_cw);
        d("ModuleConfig:canned_message:inputbroker_event_pass: %d\r\n", module->payload_variant.canned_message.inputbroker_event_press);
        d("ModuleConfig:canned_message:inputbroker_pin_a: %d\r\n", module->payload_variant.canned_message.inputbroker_pin_a);
        d("ModuleConfig:canned_message:inputbroker_pin_b: %d\r\n", module->payload_variant.canned_message.inputbroker_pin_b);
        d("ModuleConfig:canned_message:inputbroker_pin_press: %d\r\n", module->payload_variant.canned_message.inputbroker_pin_press);
        d("ModuleConfig:canned_message:rotary1_enabled: %d\r\n", module->payload_variant.canned_message.rotary1_enabled);
        d("ModuleConfig:canned_message:send_bell: %d\r\n", module->payload_variant.canned_message.send_bell);
        d("ModuleConfig:canned_message:updown1_enabled: %d\r\n", module->payload_variant.canned_message.updown1_enabled);
      break;

      case meshtastic_ModuleConfig_audio_tag: 
        d("ModuleConfig:audio:codec2_enabled: %d\r\n", module->payload_variant.audio.codec2_enabled);
        d("ModuleConfig:audio:bitrate: %d\r\n", module->payload_variant.audio.bitrate);
        d("ModuleConfig:audio:i2s_din: %d\r\n", module->payload_variant.audio.i2s_din);
        d("ModuleConfig:audio:i2s_sck: %d\r\n", module->payload_variant.audio.i2s_sck);
        d("ModuleConfig:audio:i2s_sd: %d\r\n", module->payload_variant.audio.i2s_sd);
        d("ModuleConfig:audio:i2s_ws: %d\r\n", module->payload_variant.audio.i2s_ws);
        d("ModuleConfig:audio:ptt_pin: %d\r\n", module->payload_variant.audio.ptt_pin);
      break;

      case meshtastic_ModuleConfig_remote_hardware_tag: 
        d("ModuleConfig:remote_hardware:enabled: %d\r\n", module->payload_variant.remote_hardware.enabled);
        d("ModuleConfig:remote_hardware:allow_undefined_pin_access: %d\r\n", module->payload_variant.remote_hardware.allow_undefined_pin_access);
        d("ModuleConfig:remote_hardware:available_pins: %d\r\n", module->payload_variant.remote_hardware.available_pins);
        d("ModuleConfig:remote_hardware:available_pins_count: %d\r\n", module->payload_variant.remote_hardware.available_pins_count);

      break;

      case meshtastic_ModuleConfig_neighbor_info_tag: 
        d("ModuleConfig:neighbor_info:enabled: %d\r\n", module->payload_variant.neighbor_info.enabled);
        d("ModuleConfig:neighbor_info:transmit_over_lora: %d\r\n", module->payload_variant.neighbor_info.transmit_over_lora);
        d("ModuleConfig:neighbor_info:update_interval: %d\r\n", module->payload_variant.neighbor_info.update_interval);
      break;

      case meshtastic_ModuleConfig_ambient_lighting_tag:
        d("ModuleConfig:ambient_lighting:led_state: %d\r\n", module->payload_variant.ambient_lighting.led_state);
        d("ModuleConfig:ambient_lighting:current: %d\r\n", module->payload_variant.ambient_lighting.current);
        d("ModuleConfig:ambient_lighting:red: %d\r\n", module->payload_variant.ambient_lighting.red);
        d("ModuleConfig:ambient_lighting:green: %d\r\n", module->payload_variant.ambient_lighting.green);
        d("ModuleConfig:ambient_lighting:blue: %d\r\n", module->payload_variant.ambient_lighting.blue);
      break;

      case meshtastic_ModuleConfig_detection_sensor_tag: 
        d("ModuleConfig:detection_sensor:enabled: %d\r\n", module->payload_variant.detection_sensor.enabled);
        d("ModuleConfig:detection_sensor:detection_trigger_type: %d\r\n", module->payload_variant.detection_sensor.detection_trigger_type);
        d("ModuleConfig:detection_sensor:min_broadcast_secs: %d\r\n", module->payload_variant.detection_sensor.minimum_broadcast_secs);
        d("ModuleConfig:detection_sensor:monitor_pin: %d\r\n", module->payload_variant.detection_sensor.monitor_pin);
        d("ModuleConfig:detection_sensor:name: %d\r\n", module->payload_variant.detection_sensor.name);
        d("ModuleConfig:detection_sensor:send_bell: %d\r\n", module->payload_variant.detection_sensor.send_bell);
        d("ModuleConfig:detection_sensor:state_broadcast_secs: %d\r\n", module->payload_variant.detection_sensor.state_broadcast_secs);
        d("ModuleConfig:detection_sensor:use_pullup: %d\r\n", module->payload_variant.detection_sensor.use_pullup);
      break;

      case meshtastic_ModuleConfig_paxcounter_tag:
        d("ModuleConfig:paxcounter:enabled: %d\r\n", module->payload_variant.paxcounter.enabled);
        d("ModuleConfig:paxcounter:ble_threshold: %d\r\n", module->payload_variant.paxcounter.ble_threshold);
        d("ModuleConfig:paxcounter:paxcounter_update_interval: %d\r\n", module->payload_variant.paxcounter.paxcounter_update_interval);
        d("ModuleConfig:paxcounter:wifi_threshold: %d\r\n", module->payload_variant.paxcounter.wifi_threshold);
      break;

      default:
        d("Unknown payload variant: %d\r\n", module->which_payload_variant);
  }
  return true;
}

bool handle_queueStatus_tag(meshtastic_QueueStatus *qstatus) {
  d("queueStatus: maxlen: %d\r\n", qstatus->maxlen);
  d("queueStatus: res: %d\r\n", qstatus->res);
  d("queueStatus: free: %d\r\n", qstatus->free);
  d("queueStatus: mesh_packet_id: %d\r\n", qstatus->mesh_packet_id);
  return true;
}

bool handle_xmodemPacket_tag(meshtastic_XModem *packet) {
  d("XmodemPacket: XModem control #: %d\r\n", packet->control);
  d("XmodemPacket: XModem sequence #: %d\r\n", packet->seq);
  d("XmodemPacket: XModem crc16: %d\r\n", packet->crc16);
  return true;
}

bool handle_metatag_data(meshtastic_DeviceMetadata *meta) {
  d("metatag_data:FW Version: %s\r\n", meta->firmware_version);
  d("metatag_data:device_state_version: %d\r\n", meta->device_state_version);
  d("metatag_data:canShutdown: %d\r\n", meta->canShutdown);
  d("metatag_data:hasWiFi: %d\r\n", meta->hasWifi);
  d("metatag_data:hasBluetooth: %d\r\n", meta->hasBluetooth);
  d("metatag_data:hasEthernet: %d\r\n", meta->hasEthernet);
  d("metatag_data:role: %d\r\n", meta->role);
  d("metatag_data:positionFlags: %d\r\n", meta->position_flags);
  d("metatag_data:hw_model: %d\r\n", meta->hw_model);
  d("metatag_data:hasRemoteHardware: %d\r\n", meta->hasRemoteHardware);
  d("metatag_data:excludedModules: %d\r\n", meta->excluded_modules);
  return true;
}

bool handle_mqttClientProxyMessage_tag(meshtastic_MqttClientProxyMessage *mqtt) {
  d("mqttClientProxyMessage:Topic: %s\r\n", mqtt->topic);
  switch (mqtt->which_payload_variant) {
    case meshtastic_MqttClientProxyMessage_data_tag:
      // TODO - INVALID d("mqttClientProxyMessage:data: %s\r\n", mqtt->payload_variant.data);
      break;
    case meshtastic_MqttClientProxyMessage_text_tag:
      d("mqttClientProxyMessage:text %s\r\n", mqtt->payload_variant.text);
      break;
  }
  d("mqttClientProxyMessage:retained: %d\r\n", mqtt->retained);
  return true;
}

bool handle_fileInfo_tag(meshtastic_FileInfo *fInfo) {
  d("fileInfo:fileName: %s\r\n", fInfo->file_name);
  d("fileInfo:sizeBytes: %d\r\n", fInfo->size_bytes);
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
    switch (meshPacket->decoded.portnum) {
        case meshtastic_PortNum_TEXT_MESSAGE_APP:
            if (text_message_callback != NULL) {
              text_message_callback(meshPacket->from, meshPacket->to, meshPacket->channel, (const char*)meshPacket->decoded.payload.bytes);
          } else {
        }
        break;
      case meshtastic_PortNum_ADMIN_APP:
      case meshtastic_PortNum_ATAK_FORWARDER:
      case meshtastic_PortNum_ATAK_PLUGIN:
      case meshtastic_PortNum_AUDIO_APP: 
      case meshtastic_PortNum_DETECTION_SENSOR_APP: 
      case meshtastic_PortNum_IP_TUNNEL_APP: 
      case meshtastic_PortNum_MAP_REPORT_APP:
      case meshtastic_PortNum_MAX: 
      case meshtastic_PortNum_NEIGHBORINFO_APP: 
      case meshtastic_PortNum_NODEINFO_APP: 
      case meshtastic_PortNum_PAXCOUNTER_APP:
      case meshtastic_PortNum_POSITION_APP: 
      case meshtastic_PortNum_POWERSTRESS_APP: 
      case meshtastic_PortNum_PRIVATE_APP: 
      case meshtastic_PortNum_RANGE_TEST_APP:
      case meshtastic_PortNum_REMOTE_HARDWARE_APP: 
      case meshtastic_PortNum_REPLY_APP: 
      case meshtastic_PortNum_ROUTING_APP:
      case meshtastic_PortNum_SERIAL_APP:
      case meshtastic_PortNum_SIMULATOR_APP:
      case meshtastic_PortNum_STORE_FORWARD_APP:
      case meshtastic_PortNum_TELEMETRY_APP: 
      case meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP:
      case meshtastic_PortNum_TRACEROUTE_APP: 
      case meshtastic_PortNum_UNKNOWN_APP: 
      case meshtastic_PortNum_WAYPOINT_APP: 
      case meshtastic_PortNum_ZPS_APP:
        if (portnum_callback != NULL)
          portnum_callback(meshPacket->from, meshPacket->to, meshPacket->channel, meshPacket->decoded.portnum, &meshPacket->decoded.payload);
        break;

      default:
          d("Unknown portnum %d\r\n", meshPacket->decoded.portnum);
            return false;
    }
  } else if  (meshPacket -> which_payload_variant == meshtastic_MeshPacket_encrypted_tag ) {
      d("encoded packet From: %x To: %x\r\n", meshPacket->from, meshPacket->to);
      if (encrypted_callback != NULL) {
          encrypted_callback(meshPacket->from, meshPacket->to, meshPacket->channel, meshPacket->public_key, &meshPacket->encrypted);
    	    return true;
      }
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
#ifdef MT_DEBUGGING
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
#endif
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
#ifdef MT_DEBUGGING
    Serial.print("Got a full packet! ");
    for (int i = 0 ; i < pb_size ; i++) {
      Serial.print(pb_buf[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
#endif
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
