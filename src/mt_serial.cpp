#include "mt_internals.h"

// Platform specific: select serial
#if defined(ESP_PLATFORM)
  #include "esp_log.h"
  #include "driver/uart.h"
  #include <string.h>
  // UART num defined via MESHTASTIC_UART_NUM
  #if !defined(MESHTASTIC_UART_NUM)
    #define MESHTASTIC_UART_NUM UART_NUM_1
  #endif
  #if !defined(MESHTASTIC_ESP_RXFLCTRLTRSH)
    #define MESHTASTIC_ESP_RXFLCTRLTRSH 122
  #endif
  #if !defined(MESHTASTIC_ESP_BUFSZ)
    #define MESHTASTIC_ESP_BUFSZ 1024
  #endif
  #if !defined(MESHTASTIC_ESP_QSZ)
    #define MESHTASTIC_ESP_QSZ 10
  #endif
  QueueHandle_t uart_queue;
#elif defined(ARDUINO_ARCH_SAMD)
  #define serial (&Serial1)
#elif defined(ARDUINO_ARCH_ESP32)
  #define serial (&Serial1)
#else
  // Fallback
  #include <SoftwareSerial.h>
  SoftwareSerial *serial;
#endif

void mt_serial_init(int8_t rx_pin, int8_t tx_pin, uint32_t baud) {

// Platform specific: init serial
#if defined(ESP_PLATFORM)
  uart_config_t uart_config;
  memset(&uart_config, 0, sizeof(uart_config));
  uart_config.baud_rate = (int)baud;
  uart_config.data_bits = UART_DATA_8_BITS;
  uart_config.parity = UART_PARITY_DISABLE;
  uart_config.stop_bits = UART_STOP_BITS_1;
  uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_config.rx_flow_ctrl_thresh = MESHTASTIC_ESP_RXFLCTRLTRSH;
  // Configure UART parameters
  ESP_ERROR_CHECK(uart_driver_install(MESHTASTIC_UART_NUM,
    MESHTASTIC_ESP_BUFSZ, MESHTASTIC_ESP_BUFSZ,
    MESHTASTIC_ESP_QSZ, &uart_queue, 0));
  ESP_ERROR_CHECK(uart_param_config(MESHTASTIC_UART_NUM, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(MESHTASTIC_UART_NUM, tx_pin, rx_pin,
    UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
#elif defined(ARDUINO_ARCH_SAMD)
  serial->begin(baud);
#elif defined(ARDUINO_ARCH_ESP32)
  serial->begin(baud, SERIAL_8N1, rx_pin, tx_pin);
#else
  // Fallback
  serial = new SoftwareSerial(rx_pin, tx_pin);
  serial->begin(baud);
#endif

  mt_wifi_mode = false;
  mt_serial_mode = true;
}

bool mt_serial_send_radio(const char * buf, size_t len) {
#if defined(ESP_PLATFORM)
    ESP_LOGD("Meshtastic", "Writting %lu bytes from %p to UART", len, buf);
    int r = uart_write_bytes(MESHTASTIC_UART_NUM, buf, len);
    if (r < 0)
      ESP_LOGE("Meshtastic", "Error writing UART");
    size_t wrote = (size_t)r;
#else
  size_t wrote = serial->write(buf, len);
#endif
  if (wrote == len) return true;
#if defined(ESP_PLATFORM)
    ESP_LOGE("Meshtastic", "Tried to send radio %zu but actually sent %zu",
      len, wrote);
#elifdef MT_DEBUGGING
    Serial.print("Tried to send radio ");
    Serial.print(len);
    Serial.print(" but actually sent ");
    Serial.println(wrote);
#endif

  return false;
}

bool mt_serial_loop() {
  return true;  // It's easy being a serial interface
}

size_t mt_serial_check_radio(char * buf, size_t space_left) {
  size_t bytes_read = 0;
#if defined(ESP_PLATFORM)
  while (true) {
      {
        ESP_LOGD("Meshtastic", "Polling UART");
        size_t avail = 0;
        auto e = uart_get_buffered_data_len(MESHTASTIC_UART_NUM, &avail);
        ESP_ERROR_CHECK_WITHOUT_ABORT(e);
        ESP_LOGD("Meshtastic", "UART has %lu bytes available", avail);
        if (avail == 0)
          break;
      }
    ESP_LOGD("Meshtastic", "UART data available");
    char c;
      {
        int r = uart_read_bytes(MESHTASTIC_UART_NUM, &c, 1, 1);
        if (r < 0)
            ESP_LOGE("Meshtastic", "Error reading UART");
        if (r != 1)
        {
          ESP_LOGW("Meshtastic", "Expected 1 byte from UART, got %d", r);
          break;
        }
      }
    ESP_LOGD("Meshtastic", "Read UART data");
#else
  while (serial->available()) {
    char c = serial->read();
#endif
    *buf++ = c;
    if (++bytes_read >= space_left) {
#if defined(ESP_PLATFORM)
      ESP_LOGD("Meshtastic", "Serial overflow");
#else
      d("Serial overflow");
#endif
      break;
    }
  }
  return bytes_read;
}
