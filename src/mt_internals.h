#ifndef MT_INTERNALS_H
#define MT_INTERNALS_H

#include "Meshtastic.h"

#ifdef MT_DEBUGGING
#define d(...) _d(__VA_ARGS__)
#else
#define d(...) do {} while (0) 
#endif

void _d(const char * fmt, ...);

extern bool mt_wifi_mode;
extern bool mt_serial_mode;

bool mt_wifi_loop(uint32_t now);
bool mt_serial_loop();

size_t mt_wifi_check_radio(char * buf, size_t space_left);
size_t mt_serial_check_radio(char * buf, size_t space_left);

bool mt_wifi_send_radio(const char * buf, size_t len);
bool mt_serial_send_radio(const char * buf, size_t len);

void mt_wifi_reset_idle_timeout(uint32_t now);

#endif
