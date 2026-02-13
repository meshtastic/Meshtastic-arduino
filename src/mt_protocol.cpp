#include "mt_internals.h"

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <random>

#define delay(ms) vTaskDelay((ms) / portTICK_PERIOD_MS)
#define millis() (xTaskGetTickCount() * portTICK_PERIOD_MS)

static std::random_device rand_dev;
static std::mt19937       generator(rand_dev());
#define random(x) std::uniform_int_distribution<uint32_t>(0, x)(generator)
#endif

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

#if defined(ESP_PLATFORM)
#define D(...) ESP_LOGD("Meshtastic", __VA_ARGS__)
#define D_CRLF
#else
#define D d
#define  D_CRLF " \r\n"
#define VA_BUFSIZE 512
void _d(const char * fmt, ...) {
  static char vabuf[VA_BUFSIZE];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(vabuf, sizeof(vabuf), fmt, ap);
  Serial.println(vabuf);
  Serial.flush();
}
#endif

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
#if defined(ESP_PLATFORM)
    ESP_LOGE("Meshtastic", "mt_send_radio() called but it was never initialized");
#else
    Serial.println("mt_send_radio() called but it was never initialized");
#endif
    while(1);
  }
}

bool _mt_send_toRadio(meshtastic_ToRadio toRadio) {
  pb_buf[0] = MT_MAGIC_0;
  pb_buf[1] = MT_MAGIC_1;

  pb_ostream_t stream = pb_ostream_from_buffer(pb_buf + 4, PB_BUFSIZE);
  bool status = pb_encode(&stream, meshtastic_ToRadio_fields, &toRadio);
  if (!status) {
#if defined(ESP_PLATFORM)
    ESP_LOGE("Meshtastic", "Couldn't encode toRadio");
#else
    d("Couldn't encode toRadio");
#endif
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

#if defined(ESP_PLATFORM)
  ESP_LOGD("Meshtastic", "Sending text message '%s' to %d", text, dest);
#else
  Serial.print("Sending text message '");
  Serial.print(text);
  Serial.print("' to ");
  Serial.println(dest);
#endif
  return _mt_send_toRadio(toRadio);
}

bool mt_send_heartbeat() {

#if defined(ESP_PLATFORM)
  ESP_LOGD("Meshtastic", "Sending heartbeat");
#else
  d("Sending heartbeat");
#endif

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
  D("id_tag: ID: %d" D_CRLF, id);
  return true;
}

bool handle_config_tag(meshtastic_Config *config) {
  switch (config->which_payload_variant) {
    case meshtastic_Config_device_tag:
      D("Config:device_tag:  role: %" D_CRLF, config->payload_variant.device.role);
      D("Config:device_tag:  serial enabled: %" D_CRLF, config->payload_variant.device.serial_enabled);
      D("Config:device_tag:  button gpio: %" D_CRLF, config->payload_variant.device.button_gpio);
      D("Config:device_tag:  buzzer gpio: %" D_CRLF, config->payload_variant.device.buzzer_gpio);
      D("Config:device_tag:  rebroadcast mode: %" D_CRLF, config->payload_variant.device.rebroadcast_mode);
      D("Config:device_tag:  node_info_broadcast_secs: %" D_CRLF, config->payload_variant.device.node_info_broadcast_secs);
      D("Config:device_tag:  double-tap-as-button-press: %" D_CRLF, config->payload_variant.device.double_tap_as_button_press);
      D("Config:device_tag:  is_managed: %" D_CRLF, config->payload_variant.device.is_managed);
      D("Config:device_tag:  disable_triple_click: %" D_CRLF, config->payload_variant.device.disable_triple_click);
      D("Config:device_tag:  tz_def: %" D_CRLF, config->payload_variant.device.tzdef);
      D("Config:device_tag:  led_heartbeat_disabled: %" D_CRLF, config->payload_variant.device.led_heartbeat_disabled);
      break;

    case meshtastic_Config_position_tag:
      D("Config:position_tag:  position_broadcast_secs: %" D_CRLF, config->payload_variant.position.position_broadcast_secs);
      D("Config:position_tag:  position_broadcast_smart_enabled: %" D_CRLF, config->payload_variant.position.position_broadcast_smart_enabled);
      D("Config:position_tag:  fixed_position: %" D_CRLF, config->payload_variant.position.fixed_position);
      D("Config:position_tag:  gps_enabled: %" D_CRLF, config->payload_variant.position.gps_enabled);
      D("Config:position_tag:  gps_update_interval: %" D_CRLF, config->payload_variant.position.gps_update_interval);
      D("Config:position_tag:  gps_attempt_time: %" D_CRLF, config->payload_variant.position.gps_attempt_time);
      D("Config:position_tag:  position_flags: %" D_CRLF, config->payload_variant.position.position_flags);
      D("Config:position_tag:  rx_gpio: %" D_CRLF, config->payload_variant.position.rx_gpio);
      D("Config:position_tag:  tx_gpio: %" D_CRLF, config->payload_variant.position.tx_gpio);
      D("Config:position_tag:  broadcast_smart_min_distance: %" D_CRLF, config->payload_variant.position.broadcast_smart_minimum_distance);
      D("Config:position_tag:  broadcast_smart_min_interval_secs: %" D_CRLF, config->payload_variant.position.broadcast_smart_minimum_interval_secs);
      D("Config:position_tag:  gps_en_gpio: %" D_CRLF, config->payload_variant.position.gps_en_gpio);
      D("Config:position_tag:  gps_mode %" D_CRLF, config->payload_variant.position.gps_mode);
      break;

    case meshtastic_Config_power_tag:
      D("Config:power_tag:  is_power_saving %" D_CRLF, config->payload_variant.power.is_power_saving);
      D("Config:power_tag:  on_battery_shutdown_after_secs %" D_CRLF, config->payload_variant.power.on_battery_shutdown_after_secs);
      D("Config:power_tag:  adv_multiplier_override %" D_CRLF, config->payload_variant.power.adc_multiplier_override);
      D("Config:power_tag:  wait_bluetooth_secs %" D_CRLF, config->payload_variant.power.wait_bluetooth_secs);
      D("Config:power_tag:  sds_secs %" D_CRLF, config->payload_variant.power.sds_secs);
      D("Config:power_tag:  ls_secs %" D_CRLF, config->payload_variant.power.ls_secs);
      D("Config:power_tag:  min_wake_secs %" D_CRLF, config->payload_variant.power.min_wake_secs);
      D("Config:power_tag:  device_battery_ina_aaddr %" D_CRLF, config->payload_variant.power.device_battery_ina_address);
      D("Config:power_tag:  powermon_enables %" D_CRLF, config->payload_variant.power.powermon_enables);
      break;

    case meshtastic_Config_network_tag:
      D("Config:network_tag:wifi_enabled: %d " D_CRLF, config->payload_variant.network.wifi_enabled);
      D("Config:network_tag:wifi_ssid: %s " D_CRLF, config->payload_variant.network.wifi_ssid);
      D("Config:network_tag:wifi_psk: %s " D_CRLF, config->payload_variant.network.wifi_psk);
      D("Config:network_tag:ntp_server: %s " D_CRLF, config->payload_variant.network.ntp_server);
      D("Config:network_tag:eth_enabled: %d " D_CRLF, config->payload_variant.network.eth_enabled);
      D("Config:network_tag:addr_mode: %d " D_CRLF, config->payload_variant.network.address_mode);
      D("Config:network_tag:has_ipv4_config: %d " D_CRLF, config->payload_variant.network.has_ipv4_config);
      D("Config:network_tag:ipv4_config: %d " D_CRLF, config->payload_variant.network.ipv4_config);
      D("Config:network_tag:rsyslog_server: %s " D_CRLF, config->payload_variant.network.rsyslog_server);
      break;

    case meshtastic_Config_display_tag:
      D("Config:display_tag:screen_on_seconds: %d " D_CRLF, config->payload_variant.display.screen_on_secs);
      D("Config:display_tag:gps_format: %d " D_CRLF, config->payload_variant.display.gps_format);
      D("Config:display_tag:auto_screen_carousel_secs: %d " D_CRLF, config->payload_variant.display.auto_screen_carousel_secs);
      D("Config:display_tag:compass_north_top: %d " D_CRLF, config->payload_variant.display.compass_north_top);
      D("Config:display_tag:flip_screen: %d " D_CRLF, config->payload_variant.display.flip_screen);
      D("Config:display_tag:units: %d " D_CRLF, config->payload_variant.display.units);
      D("Config:display_tag:oled: %d " D_CRLF, config->payload_variant.display.oled);
      D("Config:display_tag:displayMode: %d " D_CRLF, config->payload_variant.display.displaymode);
      D("Config:display_tag:heading_bold: %d " D_CRLF, config->payload_variant.display.heading_bold);
      D("Config:display_tag:wake_on_tap_or_motion: %d " D_CRLF, config->payload_variant.display.wake_on_tap_or_motion);
      D("Config:display_tag:compass_orientation: %d " D_CRLF, config->payload_variant.display.compass_orientation);
      break;

    case meshtastic_Config_lora_tag:
      D("Config:lora_tag:use_preset: %d " D_CRLF, config->payload_variant.lora.use_preset);
      D("Config:lora_tag:modem_preset: %d " D_CRLF, config->payload_variant.lora.modem_preset);
      D("Config:lora_tag:bandwidth: %d " D_CRLF, config->payload_variant.lora.bandwidth);
      D("Config:lora_tag:spread_factor: %d " D_CRLF, config->payload_variant.lora.spread_factor);
      D("Config:lora_tag:coding_rate: %d " D_CRLF, config->payload_variant.lora.coding_rate);
      D("Config:lora_tag:frequency_offset: %d " D_CRLF, config->payload_variant.lora.frequency_offset);
      D("Config:lora_tag:region: %d " D_CRLF, config->payload_variant.lora.region);
      D("Config:lora_tag:hot_limit: %d " D_CRLF, config->payload_variant.lora.hop_limit);
      D("Config:lora_tag:tx_enabled: %d " D_CRLF, config->payload_variant.lora.tx_enabled);
      D("Config:lora_tag:tx_power: %d " D_CRLF, config->payload_variant.lora.tx_power);
      D("Config:lora_tag:channel_num: %d " D_CRLF, config->payload_variant.lora.channel_num);
      D("Config:lora_tag:override_duty_cycle: %d " D_CRLF, config->payload_variant.lora.override_duty_cycle);
      D("Config:lora_tag:sx126x_rx_boosted_gain: %d " D_CRLF, config->payload_variant.lora.sx126x_rx_boosted_gain);
      D("Config:lora_tag:override_frequency: %d " D_CRLF, config->payload_variant.lora.override_frequency);
      D("Config:lora_tag:pa_fan_disabled: %d " D_CRLF, config->payload_variant.lora.pa_fan_disabled);
      D("Config:lora_tag:ignore_incoming_count: %d " D_CRLF, config->payload_variant.lora.ignore_incoming_count);
      D("Config:lora_tag:ignore_mqtt: %d " D_CRLF, config->payload_variant.lora.ignore_mqtt);
      D("Config:lora_tag:config_okay_to_mqtt: %d " D_CRLF, config->payload_variant.lora.config_ok_to_mqtt);
      break;

    case meshtastic_Config_bluetooth_tag:
      D("Config:bluetooth_tag:enabled: %d " D_CRLF, config->payload_variant.bluetooth.enabled);
      D("Config:bluetooth_tag:fixed_pin: %d " D_CRLF, config->payload_variant.bluetooth.fixed_pin);
      D("Config:bluetooth_tag:mode: %d " D_CRLF, config->payload_variant.bluetooth.mode);
      break;

    case meshtastic_Config_security_tag:
      D("Config:security_tag:is_managed: %d" D_CRLF, config->payload_variant.security.is_managed);
      D("Config:security_tag:public_key: %x" D_CRLF, config->payload_variant.security.public_key);
      D("Config:security_tag:private_key: %x" D_CRLF, config->payload_variant.security.private_key);
      D("Config:security_tag:admin_key_count: %x" D_CRLF, config->payload_variant.security.admin_key_count);
      D("Config:security_tag:serial_enabled: %x" D_CRLF, config->payload_variant.security.serial_enabled);
      D("Config:security_tag:debug_log_api_enabled: %x" D_CRLF, config->payload_variant.security.debug_log_api_enabled);
      D("Config:security_tag:admin_channel_enabled: %x" D_CRLF, config->payload_variant.security.admin_channel_enabled);
      break;

    case meshtastic_Config_sessionkey_tag:
      D("Config:sessionkey_tag:dummy_field: %x" D_CRLF, config->payload_variant.sessionkey.dummy_field);
      break;

    case meshtastic_Config_device_ui_tag:
      D("Config.device_ui:alert_enabled: %" D_CRLF, config->payload_variant.device_ui.alert_enabled);
      D("Config.device_ui:banner_enabled: %" D_CRLF, config->payload_variant.device_ui.banner_enabled);
      D("Config.device_ui:has_node_filter: %" D_CRLF, config->payload_variant.device_ui.has_node_filter);
      D("Config.device_ui:has_node_highlight: %" D_CRLF, config->payload_variant.device_ui.has_node_highlight);
      D("Config.device_ui:language: %" D_CRLF, config->payload_variant.device_ui.language);
      D("Config.device_ui:node_filter: %" D_CRLF, config->payload_variant.device_ui.node_filter);
      D("Config.device_ui:node_highlight: %" D_CRLF, config->payload_variant.device_ui.node_highlight);
      D("Config.device_ui:pin_code: %" D_CRLF, config->payload_variant.device_ui.pin_code);
      D("Config.device_ui:ring_tone_id: %" D_CRLF, config->payload_variant.device_ui.ring_tone_id);
      D("Config.device_ui:screen_brightness: %" D_CRLF, config->payload_variant.device_ui.screen_brightness);
      D("Config.device_ui:screen_lock: %" D_CRLF, config->payload_variant.device_ui.screen_lock);
      D("Config.device_ui:screen_timeout: %" D_CRLF, config->payload_variant.device_ui.screen_timeout);
      break;

    default:
#if defined(ESP_PLATFORM)
      ESP_LOGW("Meshtastic", "Unknown Config_Tag payload variant: %d", config->which_payload_variant);
#else
      d("Unknown Config_Tag payload variant: %d\r\n", config->which_payload_variant);
#endif
  }
  return true;
}

bool handle_channel_tag(meshtastic_Channel *channel) {
  D("ChannelTag:index: %d" D_CRLF, channel->index);
  D("ChannelTag:has_settings: %d" D_CRLF, channel->has_settings);
  D("ChannelTag:role: %d" D_CRLF, channel->role);
  return true;
}

bool handle_FromRadio_log_record_tag(meshtastic_LogRecord *record) {
  D("FromRadio_log_record:message: %s" D_CRLF, record->message);
  D("FromRadio_log_record:time: %d" D_CRLF, record->time);
  D("FromRadio_log_record:source: %s" D_CRLF, record->source);
  D("FromRadio_log_record:level: %s" D_CRLF, record->level);
  return true;
}

bool handle_moduleConfig_tag(meshtastic_ModuleConfig *module){
  switch (module->which_payload_variant) {
      case meshtastic_ModuleConfig_mqtt_tag:
      D("ModuleConfig:mqtt:enabled: %d" D_CRLF, module->payload_variant.mqtt.enabled);
      D("ModuleConfig:mqtt:address: %s" D_CRLF, module->payload_variant.mqtt.address);
      D("ModuleConfig:mqtt:username: %s" D_CRLF, module->payload_variant.mqtt.username);
      D("ModuleConfig:mqtt:password: %s" D_CRLF, module->payload_variant.mqtt.password);
      D("ModuleConfig:mqtt:encryption_enabled: %d" D_CRLF, module->payload_variant.mqtt.encryption_enabled);
      D("ModuleConfig:mqtt:json_enabled: %d" D_CRLF, module->payload_variant.mqtt.json_enabled);
      D("ModuleConfig:mqtt:root: %s" D_CRLF, module->payload_variant.mqtt.root);
      D("ModuleConfig:mqtt:proxy_to_client_enabled: %d" D_CRLF, module->payload_variant.mqtt.proxy_to_client_enabled);
      D("ModuleConfig:mqtt:map_reporting_enabled %d" D_CRLF, module->payload_variant.mqtt.map_report_settings);
      D("ModuleConfig:mqtt:has_map_report_settings %d" D_CRLF, module->payload_variant.mqtt.has_map_report_settings);
      break;

      case meshtastic_ModuleConfig_serial_tag:
        D("ModuleConfig:serial:enabled: %d" D_CRLF, module->payload_variant.serial.enabled);
        D("ModuleConfig:serial:echo: %d" D_CRLF, module->payload_variant.serial.echo);
        D("ModuleConfig:serial:rxd-gpio-pin: %d" D_CRLF, module->payload_variant.serial.rxd);
        D("ModuleConfig:serial:txd-gpio-pin: %d" D_CRLF, module->payload_variant.serial.txd);
        D("ModuleConfig:serial:baud: %d" D_CRLF, module->payload_variant.serial.baud);
        D("ModuleConfig:serial:timeout: %d" D_CRLF, module->payload_variant.serial.timeout);
        D("ModuleConfig:serial:mode: %d" D_CRLF, module->payload_variant.serial.mode);
        D("ModuleConfig:serial:override_console_serial_port: %d" D_CRLF, module->payload_variant.serial.override_console_serial_port);
      break;

      case meshtastic_ModuleConfig_external_notification_tag:
        D("ModuleConfig:external_notification:enabled: %d" D_CRLF, module->payload_variant.external_notification.enabled);
        D("ModuleConfig:external_notification:output_ms: %d" D_CRLF, module->payload_variant.external_notification.output_ms);
        D("ModuleConfig:external_notification:output: %d" D_CRLF, module->payload_variant.external_notification.output);
        D("ModuleConfig:external_notification:active: %d" D_CRLF, module->payload_variant.external_notification.active);
        D("ModuleConfig:external_notification:alert_message: %d" D_CRLF, module->payload_variant.external_notification.alert_message);
        D("ModuleConfig:external_notification:alert_bell: %d" D_CRLF, module->payload_variant.external_notification.alert_bell);
        D("ModuleConfig:external_notification:use_pwm: %d" D_CRLF, module->payload_variant.external_notification.use_pwm);
        D("ModuleConfig:external_notification:output_vibra: %d" D_CRLF, module->payload_variant.external_notification.output_vibra);
        D("ModuleConfig:external_notification:output_buzzer: %d" D_CRLF, module->payload_variant.external_notification.output_buzzer);
        D("ModuleConfig:external_notification:alert_message_vibra: %d" D_CRLF, module->payload_variant.external_notification.alert_message_vibra);
        D("ModuleConfig:external_notification:alert_message_buzzer: %d" D_CRLF, module->payload_variant.external_notification.alert_message_buzzer);
        D("ModuleConfig:external_notification:alert_bell_vibra: %d" D_CRLF, module->payload_variant.external_notification.alert_bell_vibra);
        D("ModuleConfig:external_notification:alert_bell_buzzer: %d" D_CRLF, module->payload_variant.external_notification.alert_bell_buzzer);
        D("ModuleConfig:external_notification:nag_timeout: %d" D_CRLF, module->payload_variant.external_notification.nag_timeout);
        D("ModuleConfig:external_notification:use_i2s_as_buzzer: %d" D_CRLF, module->payload_variant.external_notification.use_i2s_as_buzzer);
      break;

      case meshtastic_ModuleConfig_store_forward_tag:
        D("ModuleConfig:store_forward:enabled: %d" D_CRLF, module->payload_variant.store_forward.enabled);
        D("ModuleConfig:store_forward:heartbeat: %d" D_CRLF, module->payload_variant.store_forward.heartbeat);
        D("ModuleConfig:store_forward:history_return_max: %d" D_CRLF, module->payload_variant.store_forward.history_return_max);
        D("ModuleConfig:store_forward:history_return_window: %d" D_CRLF, module->payload_variant.store_forward.history_return_window);
        D("ModuleConfig:store_forward:is_server: %d" D_CRLF, module->payload_variant.store_forward.is_server);
        D("ModuleConfig:store_forward:records: %d" D_CRLF, module->payload_variant.store_forward.records);
      break;

      case meshtastic_ModuleConfig_range_test_tag:
        D("ModuleConfig:range_test:enabled: %d" D_CRLF, module->payload_variant.range_test.enabled);
        D("ModuleConfig:range_test:save: %d" D_CRLF, module->payload_variant.range_test.save);
        D("ModuleConfig:range_test:sender: %d" D_CRLF, module->payload_variant.range_test.sender);
      break;

      case meshtastic_ModuleConfig_telemetry_tag:
        D("ModuleConfig:telemetry:air_quality_enabled: %d" D_CRLF, module->payload_variant.telemetry.air_quality_enabled);
        D("ModuleConfig:telemetry:air_quality_interval: %d" D_CRLF, module->payload_variant.telemetry.air_quality_interval);
        D("ModuleConfig:telemetry:device_update_interval: %d" D_CRLF, module->payload_variant.telemetry.device_update_interval);
        D("ModuleConfig:telemetry:environment_display_fahrenheit: %d" D_CRLF, module->payload_variant.telemetry.environment_display_fahrenheit);
        D("ModuleConfig:telemetry:environment_measurement_enabled: %d" D_CRLF, module->payload_variant.telemetry.environment_measurement_enabled);
        D("ModuleConfig:telemetry:environment_screen_enabled: %d" D_CRLF, module->payload_variant.telemetry.environment_screen_enabled);
        D("ModuleConfig:telemetry:environment_update_interval: %d" D_CRLF, module->payload_variant.telemetry.environment_update_interval);
        D("ModuleConfig:telemetry:health_measurement_enabled: %d" D_CRLF, module->payload_variant.telemetry.health_measurement_enabled);
        D("ModuleConfig:telemetry:health_screen_enabled: %d" D_CRLF, module->payload_variant.telemetry.health_screen_enabled);
        D("ModuleConfig:telemetry:health_update_interval: %d" D_CRLF, module->payload_variant.telemetry.health_update_interval);
        D("ModuleConfig:telemetry:power_measurement_enabled: %d" D_CRLF, module->payload_variant.telemetry.power_measurement_enabled);
        D("ModuleConfig:telemetry:power_update_interval: %d" D_CRLF, module->payload_variant.telemetry.power_update_interval);

      break;

      case meshtastic_ModuleConfig_canned_message_tag:
        D("ModuleConfig:canned_message:enabled: %d" D_CRLF, module->payload_variant.canned_message.enabled);
        D("ModuleConfig:canned_message:allow_input_source: %d" D_CRLF, module->payload_variant.canned_message.allow_input_source);
        D("ModuleConfig:canned_message:inputbroker_event_ccw: %d" D_CRLF, module->payload_variant.canned_message.inputbroker_event_ccw);
        D("ModuleConfig:canned_message:inputbroker_event_cw: %d" D_CRLF, module->payload_variant.canned_message.inputbroker_event_cw);
        D("ModuleConfig:canned_message:inputbroker_event_pass: %d" D_CRLF, module->payload_variant.canned_message.inputbroker_event_press);
        D("ModuleConfig:canned_message:inputbroker_pin_a: %d" D_CRLF, module->payload_variant.canned_message.inputbroker_pin_a);
        D("ModuleConfig:canned_message:inputbroker_pin_b: %d" D_CRLF, module->payload_variant.canned_message.inputbroker_pin_b);
        D("ModuleConfig:canned_message:inputbroker_pin_press: %d" D_CRLF, module->payload_variant.canned_message.inputbroker_pin_press);
        D("ModuleConfig:canned_message:rotary1_enabled: %d" D_CRLF, module->payload_variant.canned_message.rotary1_enabled);
        D("ModuleConfig:canned_message:send_bell: %d" D_CRLF, module->payload_variant.canned_message.send_bell);
        D("ModuleConfig:canned_message:updown1_enabled: %d" D_CRLF, module->payload_variant.canned_message.updown1_enabled);
      break;

      case meshtastic_ModuleConfig_audio_tag:
        D("ModuleConfig:audio:codec2_enabled: %d" D_CRLF, module->payload_variant.audio.codec2_enabled);
        D("ModuleConfig:audio:bitrate: %d" D_CRLF, module->payload_variant.audio.bitrate);
        D("ModuleConfig:audio:i2s_din: %d" D_CRLF, module->payload_variant.audio.i2s_din);
        D("ModuleConfig:audio:i2s_sck: %d" D_CRLF, module->payload_variant.audio.i2s_sck);
        D("ModuleConfig:audio:i2s_sd: %d" D_CRLF, module->payload_variant.audio.i2s_sd);
        D("ModuleConfig:audio:i2s_ws: %d" D_CRLF, module->payload_variant.audio.i2s_ws);
        D("ModuleConfig:audio:ptt_pin: %d" D_CRLF, module->payload_variant.audio.ptt_pin);
      break;

      case meshtastic_ModuleConfig_remote_hardware_tag:
        D("ModuleConfig:remote_hardware:enabled: %d" D_CRLF, module->payload_variant.remote_hardware.enabled);
        D("ModuleConfig:remote_hardware:allow_undefined_pin_access: %d" D_CRLF, module->payload_variant.remote_hardware.allow_undefined_pin_access);
        D("ModuleConfig:remote_hardware:available_pins: %d" D_CRLF, module->payload_variant.remote_hardware.available_pins);
        D("ModuleConfig:remote_hardware:available_pins_count: %d" D_CRLF, module->payload_variant.remote_hardware.available_pins_count);

      break;

      case meshtastic_ModuleConfig_neighbor_info_tag:
        D("ModuleConfig:neighbor_info:enabled: %d" D_CRLF, module->payload_variant.neighbor_info.enabled);
        D("ModuleConfig:neighbor_info:transmit_over_lora: %d" D_CRLF, module->payload_variant.neighbor_info.transmit_over_lora);
        D("ModuleConfig:neighbor_info:update_interval: %d" D_CRLF, module->payload_variant.neighbor_info.update_interval);
      break;

      case meshtastic_ModuleConfig_ambient_lighting_tag:
        D("ModuleConfig:ambient_lighting:led_state: %d" D_CRLF, module->payload_variant.ambient_lighting.led_state);
        D("ModuleConfig:ambient_lighting:current: %d" D_CRLF, module->payload_variant.ambient_lighting.current);
        D("ModuleConfig:ambient_lighting:red: %d" D_CRLF, module->payload_variant.ambient_lighting.red);
        D("ModuleConfig:ambient_lighting:green: %d" D_CRLF, module->payload_variant.ambient_lighting.green);
        D("ModuleConfig:ambient_lighting:blue: %d" D_CRLF, module->payload_variant.ambient_lighting.blue);
      break;

      case meshtastic_ModuleConfig_detection_sensor_tag:
        D("ModuleConfig:detection_sensor:enabled: %d" D_CRLF, module->payload_variant.detection_sensor.enabled);
        D("ModuleConfig:detection_sensor:detection_trigger_type: %d" D_CRLF, module->payload_variant.detection_sensor.detection_trigger_type);
        D("ModuleConfig:detection_sensor:min_broadcast_secs: %d" D_CRLF, module->payload_variant.detection_sensor.minimum_broadcast_secs);
        D("ModuleConfig:detection_sensor:monitor_pin: %d" D_CRLF, module->payload_variant.detection_sensor.monitor_pin);
        D("ModuleConfig:detection_sensor:name: %d" D_CRLF, module->payload_variant.detection_sensor.name);
        D("ModuleConfig:detection_sensor:send_bell: %d" D_CRLF, module->payload_variant.detection_sensor.send_bell);
        D("ModuleConfig:detection_sensor:state_broadcast_secs: %d" D_CRLF, module->payload_variant.detection_sensor.state_broadcast_secs);
        D("ModuleConfig:detection_sensor:use_pullup: %d" D_CRLF, module->payload_variant.detection_sensor.use_pullup);
      break;

      case meshtastic_ModuleConfig_paxcounter_tag:
        D("ModuleConfig:paxcounter:enabled: %d" D_CRLF, module->payload_variant.paxcounter.enabled);
        D("ModuleConfig:paxcounter:ble_threshold: %d" D_CRLF, module->payload_variant.paxcounter.ble_threshold);
        D("ModuleConfig:paxcounter:paxcounter_update_interval: %d" D_CRLF, module->payload_variant.paxcounter.paxcounter_update_interval);
        D("ModuleConfig:paxcounter:wifi_threshold: %d" D_CRLF, module->payload_variant.paxcounter.wifi_threshold);
      break;

      default:
#if defined(ESP_PLATFORM)
        ESP_LOGW("Meshtastic", "Unknown payload variant: %d", module->which_payload_variant);
#else
        d("Unknown payload variant: %d\r\n", module->which_payload_variant);
#endif
  }
  return true;
}

bool handle_queueStatus_tag(meshtastic_QueueStatus *qstatus) {
  D("queueStatus: maxlen: %d" D_CRLF, qstatus->maxlen);
  D("queueStatus: res: %d" D_CRLF, qstatus->res);
  D("queueStatus: free: %d" D_CRLF, qstatus->free);
  D("queueStatus: mesh_packet_id: %d" D_CRLF, qstatus->mesh_packet_id);
  return true;
}

bool handle_xmodemPacket_tag(meshtastic_XModem *packet) {
  D("XmodemPacket: XModem control #: %d" D_CRLF, packet->control);
  D("XmodemPacket: XModem sequence #: %d" D_CRLF, packet->seq);
  D("XmodemPacket: XModem crc16: %d" D_CRLF, packet->crc16);
  return true;
}

bool handle_metatag_data(meshtastic_DeviceMetadata *meta) {
  D("metatag_data:FW Version: %s" D_CRLF, meta->firmware_version);
  D("metatag_data:device_state_version: %d" D_CRLF, meta->device_state_version);
  D("metatag_data:canShutdown: %d" D_CRLF, meta->canShutdown);
  D("metatag_data:hasWiFi: %d" D_CRLF, meta->hasWifi);
  D("metatag_data:hasBluetooth: %d" D_CRLF, meta->hasBluetooth);
  D("metatag_data:hasEthernet: %d" D_CRLF, meta->hasEthernet);
  D("metatag_data:role: %d" D_CRLF, meta->role);
  D("metatag_data:positionFlags: %d" D_CRLF, meta->position_flags);
  D("metatag_data:hw_model: %d" D_CRLF, meta->hw_model);
  D("metatag_data:hasRemoteHardware: %d" D_CRLF, meta->hasRemoteHardware);
  D("metatag_data:excludedModules: %d" D_CRLF, meta->excluded_modules);
  return true;
}

bool handle_mqttClientProxyMessage_tag(meshtastic_MqttClientProxyMessage *mqtt) {
  D("mqttClientProxyMessage:Topic: %s" D_CRLF, mqtt->topic);
  switch (mqtt->which_payload_variant) {
    case meshtastic_MqttClientProxyMessage_data_tag:
      // TODO - INVALID d("mqttClientProxyMessage:data: %s\r\n", mqtt->payload_variant.data);
      break;
    case meshtastic_MqttClientProxyMessage_text_tag:
      D("mqttClientProxyMessage:text %s" D_CRLF, mqtt->payload_variant.text);
      break;
  }
  D("mqttClientProxyMessage:retained: %d" D_CRLF, mqtt->retained);
  return true;
}

bool handle_fileInfo_tag(meshtastic_FileInfo *fInfo) {
  D("fileInfo:fileName: %s" D_CRLF, fInfo->file_name);
  D("fileInfo:sizeBytes: %d" D_CRLF, fInfo->size_bytes);
  return true;
}

bool handle_my_info(meshtastic_MyNodeInfo *myNodeInfo) {
  my_node_num = myNodeInfo->my_node_num;
  return true;
}

bool handle_node_info(meshtastic_NodeInfo *nodeInfo) {
  if (node_report_callback == NULL) {
#if defined(ESP_PLATFORM)
    ESP_LOGW("Meshtastic", "Got a node report, but we don't have a callback");
#else
    d("Got a node report, but we don't have a callback");
#endif
    return false;
  }
  node.node_num = nodeInfo->num;
  node.is_mine = nodeInfo->num == my_node_num;
  node.last_heard_from = nodeInfo->last_heard;
  node.is_favorite = nodeInfo->is_favorite;
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
#if defined(ESP_PLATFORM)
          ESP_LOGW("Meshtastic", "Unknown portnum %d\r\n", meshPacket->decoded.portnum);
#else
          d("Unknown portnum %d\r\n", meshPacket->decoded.portnum);
#endif
          return false;
    }
  } else if  (meshPacket -> which_payload_variant == meshtastic_MeshPacket_encrypted_tag ) {
#if defined(ESP_PLATFORM)
      ESP_LOGD("Meshtastic", "encoded packet From: %x To: %x\r\n", meshPacket->from, meshPacket->to);
#else
      d("encoded packet From: %x To: %x\r\n", meshPacket->from, meshPacket->to);
#endif
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
#if defined(ESP_PLATFORM)
    ESP_LOGW("Meshtastic", "Decoding failed");
#else
    d("Decoding failed");
#endif
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
#if defined(MT_DEBUGGING) || defined(ESP_PLATFORM)
        // Rate limit
        // Serial input buffer overflows during initial connection, while we're slowly printing these at 9600 baud
        constexpr uint32_t limitMs = 100; 
        static uint32_t lastLog = 0;
        uint32_t now = millis();
        if (now - lastLog > limitMs) {
            lastLog = now;
#if defined(ESP_PLATFORM)
            ESP_LOGW("Meshtastic", "Got a payloadVariant we don't recognize: %i",
                (int)fromRadio.which_payload_variant);
#else
            Serial.print("Got a payloadVariant we don't recognize: ");
            Serial.println(fromRadio.which_payload_variant);
#endif
        }
#endif
      return false;
  }

#if defined(ESP_PLATFORM)
  ESP_LOGD("Meshtastic", "Handled a packet");
#else
  d("Handled a packet");
#endif
}

void mt_protocol_check_packet(uint32_t now) {
  if (pb_size < MT_HEADER_SIZE) {
    // We don't even have a header yet
    delay(NO_NEWS_PAUSE);
    return;
  }

  if (pb_buf[0] != MT_MAGIC_0 || pb_buf[1] != MT_MAGIC_1) {
#if defined(ESP_PLATFORM)
    ESP_LOGW("Meshtastic", "Got bad magic");
#else
    d("Got bad magic");
#endif
    memset(pb_buf, 0, PB_BUFSIZE);
    pb_size = 0;
    return;
  }

  uint16_t payload_len = pb_buf[2] << 8 | pb_buf[3];
  if (payload_len > PB_BUFSIZE) {
#if defined(ESP_PLATFORM)
    ESP_LOGW("Meshtastic", "Got packet claiming to be ridiculous length");
#else
    d("Got packet claiming to be ridiculous length");
#endif
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
#if defined(ESP_PLATFORM)
    ESP_LOGE("Meshtastic", "mt_loop() called but it was never initialized");
#else
    Serial.println("mt_loop() called but it was never initialized");
#endif
    while(1);
  }

  pb_size += bytes_read;
  mt_protocol_check_packet(now); 
  return rv;
}
