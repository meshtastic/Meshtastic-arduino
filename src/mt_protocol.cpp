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

// Wait this many msec if there's nothing new on the channel
#define NO_NEWS_PAUSE 25

// The ID of the current WANT_CONFIG request
uint32_t want_config_id = 0;

// Node number of the MT node hosting our WiFi
uint32_t my_node_num = 0;

bool mt_debugging = false;
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
    return mt_wifi_send_radio(buf, len);
  } else if (mt_serial_mode) {
    return mt_serial_send_radio(buf, len);
  } else {
    Serial.println("mt_send_radio() called but it was never initialized");
    while(1);
  }
}

// Request a node report from our MT
bool mt_request_node_report(void (*callback)(mt_node_t *, mt_nr_progress_t)) {
  ToRadio toRadio = ToRadio_init_default;
  toRadio.which_payloadVariant = ToRadio_want_config_id_tag;
  want_config_id = random(0x7FffFFff);  // random() can't handle anything bigger
  toRadio.payloadVariant.want_config_id = want_config_id;

  pb_buf[0] = MT_MAGIC_0;
  pb_buf[1] = MT_MAGIC_1;

  pb_ostream_t stream = pb_ostream_from_buffer(pb_buf + 4, PB_BUFSIZE);
  bool status = pb_encode(&stream, ToRadio_fields, &toRadio);
  if (!status) {
    d("Couldn't encode wantconfig");
    return false;
  }

  if (mt_debugging) {
    Serial.print("Requesting node report with random ID ");
    Serial.println(want_config_id);
  }

  // Store the payload length in the header
  pb_buf[2] = stream.bytes_written / 256;
  pb_buf[3] = stream.bytes_written % 256;

  bool rv = mt_send_radio((const char *)pb_buf, 4 + stream.bytes_written);

  // Clear the buffer so it can be used to hold reply packets
  pb_size = 0;
  if (rv) node_report_callback = callback;
  return rv;
}

bool handle_my_info(MyNodeInfo *myNodeInfo) {
  my_node_num = myNodeInfo->my_node_num;
  if (mt_debugging) {
    Serial.print("Looks like my node number is ");
    Serial.println(my_node_num);
  }
  return true;
}

bool handle_node_info(NodeInfo *nodeInfo, pb_byte_t * user_id, pb_byte_t * long_name,
                      pb_byte_t * short_name, pb_byte_t * macaddr) {
  if (node_report_callback == NULL) {
    d("Got a node report, but we don't have a callback");
    return false;
  }
  node.node_num = nodeInfo->num;
  node.is_mine = nodeInfo->num == my_node_num;
  node.last_heard_from = nodeInfo->last_heard;
  if (nodeInfo->has_user) {
    node.user_id = (const char *) user_id;
    node.long_name = (const char *) long_name;
    node.short_name = (const char *) short_name;
    node.macaddr = (const char *) macaddr;
  } else {
    node.user_id = NULL;
    node.long_name = NULL;
    node.short_name = NULL;
    node.macaddr = NULL; 
  }

  if (nodeInfo->has_position) {
    node.latitude = nodeInfo->position.latitude_i / 1e7;
    node.longitude = nodeInfo->position.longitude_i / 1e7;
    node.altitude = nodeInfo->position.altitude;
    node.ground_speed = nodeInfo->position.ground_speed;
    node.battery_level = nodeInfo->position.battery_level;
    node.last_heard_position = nodeInfo->position.time;
    node.time_of_last_position = nodeInfo->position.pos_timestamp;
  } else {
    node.latitude = NAN;
    node.longitude = NAN;
    node.altitude = 0;
    node.ground_speed = 0;
    node.battery_level = 0;
    node.last_heard_position = 0;
    node.time_of_last_position = 0;
  }
  node_report_callback(&node, MT_NR_IN_PROGRESS);
  return true;
}

bool handle_config_complete_id(uint32_t now, uint32_t config_complete_id) {
  if (config_complete_id == want_config_id) {
    mt_wifi_reset_idle_timeout(now);  // It's fine if we're actually in serial mode
    want_config_id = 0;
    node_report_callback(NULL, MT_NR_DONE);
    node_report_callback = NULL;
  } else {
    node_report_callback(NULL, MT_NR_INVALID);  // but return true, since it was still a valid packet
  }
  return true;
}

// The nanopb library we're using to decode protobufs requires callback functions to handle
// arbitrary-length strings. So here they all are:
bool decode_string(pb_istream_t *stream, const pb_field_t *field, void ** arg, uint8_t len) {
  pb_byte_t *buf = *(pb_byte_t **)arg;
  if (stream->bytes_left < len) len = stream->bytes_left;
  return pb_read(stream, buf, len);
}

bool decode_user_id (pb_istream_t *stream, const pb_field_t *field, void ** arg) {
  return decode_string(stream, field, arg, MAX_USER_ID_LEN);
}

bool decode_long_name (pb_istream_t *stream, const pb_field_t *field, void ** arg) {
  return decode_string(stream, field, arg, MAX_LONG_NAME_LEN);
}

bool decode_short_name (pb_istream_t *stream, const pb_field_t *field, void ** arg) {
  return decode_string(stream, field, arg, MAX_SHORT_NAME_LEN);
}

bool decode_macaddr (pb_istream_t *stream, const pb_field_t *field, void ** arg) {
  return decode_string(stream, field, arg, MAX_MACADDR_LEN);
}

// Parse a packet that came in, and handle it. Return true iff we were able to parse it.
bool handle_packet(uint32_t now, size_t payload_len) {
  FromRadio fromRadio = FromRadio_init_zero;
  pb_byte_t user_id[MAX_USER_ID_LEN+1] = {0};
  pb_byte_t long_name[MAX_LONG_NAME_LEN+1] = {0};
  pb_byte_t short_name[MAX_SHORT_NAME_LEN+1] = {0};
  pb_byte_t macaddr[MAX_MACADDR_LEN+1] = {0};

  // Set up some callback functions
  fromRadio.payloadVariant.node_info.user.id.funcs.decode = &decode_user_id;
  fromRadio.payloadVariant.node_info.user.id.arg = &user_id;
  fromRadio.payloadVariant.node_info.user.long_name.funcs.decode = &decode_long_name;
  fromRadio.payloadVariant.node_info.user.long_name.arg = &long_name;
  fromRadio.payloadVariant.node_info.user.short_name.funcs.decode = &decode_short_name;
  fromRadio.payloadVariant.node_info.user.short_name.arg = &short_name;
  fromRadio.payloadVariant.node_info.user.macaddr.funcs.decode = &decode_macaddr;
  fromRadio.payloadVariant.node_info.user.macaddr.arg = &macaddr;

  // Decode the protobuf and shift forward any remaining bytes in the buffer (which, if
  // present, belong to the packet that we're going to process on the next loop)
  pb_istream_t stream;
  stream = pb_istream_from_buffer(pb_buf + 4, payload_len);
  bool status = pb_decode(&stream, FromRadio_fields, &fromRadio);
  memmove(pb_buf, pb_buf+4+payload_len, PB_BUFSIZE-4-payload_len);
  pb_size -= 4 + payload_len;

  if (!status) {
    d("Decoding failed");
    return false;
  }

  switch (fromRadio.which_payloadVariant) {
    case FromRadio_my_info_tag:
      return handle_my_info(&fromRadio.payloadVariant.my_info);
    case FromRadio_node_info_tag:
      return handle_node_info(&fromRadio.payloadVariant.node_info, user_id, long_name, short_name, macaddr);
    case FromRadio_config_complete_id_tag:
      return handle_config_complete_id(now, fromRadio.payloadVariant.config_complete_id);
    case FromRadio_packet_tag:
      return false;  // A packet was sent over the network. Could be anything! This would be a good place
                     // to expand this library's functionality in the future, adding support for new kinds
                     // of packet as needed. See
                     // https://github.com/meshtastic/Meshtastic-protobufs/blob/3bd1aec912d4bc1f4d9c42f6c60c766ed281d801/mesh.proto#L721-L922
                     // (or the latest version of that file) for all the fields you'll need to implement.
    default:
      if (mt_debugging) {
        Serial.print("Got a payloadVariant we don't recognize: ");
        Serial.println(fromRadio.which_payloadVariant);
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
    return;
  }

  uint16_t payload_len = pb_buf[2] << 8 | pb_buf[3];
  if (payload_len > PB_BUFSIZE) {
    d("Got packet claiming to be ridiculous length");
    return;
  }

  if (payload_len + 4 > pb_size) {
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
    rv = mt_wifi_loop(now);
    if (rv) bytes_read = mt_wifi_check_radio((char *)pb_buf + pb_size, space_left);
  } else if (mt_serial_mode) {
    rv = mt_serial_loop();
    if (rv) bytes_read = mt_serial_check_radio((char *)pb_buf + pb_size, space_left);
  } else {
    Serial.println("mt_loop() called but it was never initialized");
    while(1);
  }

  pb_size += bytes_read;
  mt_protocol_check_packet(now); 
  return rv;
}
