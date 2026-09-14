/* Compile and inspect the two configuration blobs used by JieLi BR23 images. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CFG_ITEM_MAX 800u

struct blob {
  uint8_t *data;
  size_t size, capacity;
};

struct id_name {
  uint16_t id;
  const char *name;
};

static const struct id_name known_ids[] = {
    {101, "CFG_BT_NAME"},
    {102, "CFG_BT_MAC_ADDR"},
    {513, "CFG_UART_ID"},
    {514, "CFG_HWI2C_ID"},
    {515, "CFG_SWI2C_ID"},
    {516, "CFG_HWSPI_ID"},
    {517, "CFG_SWSPI_ID"},
    {518, "CFG_SD_ID"},
    {519, "CFG_USB_ID"},
    {520, "CFG_LCD_ID"},
    {521, "CFG_TOUCH_ID"},
    {522, "CFG_IOKEY_ID"},
    {523, "CFG_ADKEY_ID"},
    {524, "CFG_AUDIO_ID"},
    {525, "CFG_VIDEO_ID"},
    {526, "CFG_WIFI_ID"},
    {527, "CFG_NIC_ID"},
    {528, "CFG_LED_ID"},
    {529, "CFG_POWER_MANG_ID"},
    {530, "CFG_IRFLT_ID"},
    {531, "CFG_PLCNT_ID"},
    {532, "CFG_PWMLED_ID"},
    {533, "CFG_RDEC_ID"},
    {534, "CFG_CHARGE_STORE_ID"},
    {535, "CFG_CHARGE_ID"},
    {536, "CFG_LOWPOWER_V_ID"},
    {537, "CFG_MIC_TYPE_ID"},
    {538, "CFG_COMBINE_SYS_VOL_ID"},
    {539, "CFG_COMBINE_CALL_VOL_ID"},
    {540, "CFG_LP_TOUCH_KEY_ID"},
    {601, "CFG_BT_RF_POWER_ID"},
    {602, "CFG_TWS_PAIR_CODE_ID"},
    {603, "CFG_AUTO_OFF_TIME_ID"},
    {604, "CFG_AEC_ID"},
    {605, "CFG_UI_TONE_STATUS_ID"},
    {606, "CFG_KEY_MSG_ID"},
    {607, "CFG_LRC_ID"},
    {608, "CFG_BLE_ID"},
    {609, "CFG_DMS_ID"},
    {610, "CFG_ANC_ID"},
    {612, "CFG_SMS_DNS_ID"},
    {613, "CFG_DMS_DNS_ID"},
    {614, "CFG_DMS_FLEXIBLE_ID"},
};

static void fail(const char *message) {
  fprintf(stderr, "jlcfg: %s\n", message);
  exit(1);
}

static void fail_file(const char *path) {
  fprintf(stderr, "jlcfg: %s: %s\n", path, strerror(errno));
  exit(1);
}

static uint16_t read_le16(const uint8_t *p) {
  return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static void write_le16(uint8_t *p, uint16_t value) {
  p[0] = value;
  p[1] = value >> 8;
}

static void write_le32(uint8_t *p, uint32_t value) {
  p[0] = value;
  p[1] = value >> 8;
  p[2] = value >> 16;
  p[3] = value >> 24;
}

static uint16_t crc16(const uint8_t *data, size_t size) {
  uint16_t crc = 0;
  while (size--) {
    crc ^= (uint16_t)*data++ << 8;
    for (unsigned bit = 0; bit < 8; bit++)
      crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
  }
  return crc;
}

static void reserve(struct blob *blob, size_t size) {
  if (size <= blob->capacity)
    return;
  size_t capacity = blob->capacity ? blob->capacity : 256;
  while (capacity < size)
    capacity *= 2;
  blob->data = realloc(blob->data, capacity);
  if (!blob->data)
    fail("out of memory");
  blob->capacity = capacity;
}

static void append(struct blob *blob, const void *data, size_t size) {
  if (!size)
    return;
  reserve(blob, blob->size + size);
  memcpy(blob->data + blob->size, data, size);
  blob->size += size;
}

static void append_byte(struct blob *blob, uint8_t value) {
  append(blob, &value, 1);
}

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  if (!file)
    fail_file(path);
  if (fseek(file, 0, SEEK_END))
    fail_file(path);
  long length = ftell(file);
  if (length < 0 || fseek(file, 0, SEEK_SET))
    fail_file(path);
  uint8_t *data = malloc(length ? (size_t)length : 1);
  if (!data)
    fail("out of memory");
  if (length && fread(data, 1, (size_t)length, file) != (size_t)length)
    fail_file(path);
  fclose(file);
  *size = (size_t)length;
  return data;
}

static void write_file(const char *path, const void *data, size_t size) {
  FILE *file = fopen(path, "wb");
  if (!file)
    fail_file(path);
  if (size && fwrite(data, 1, size, file) != size)
    fail_file(path);
  if (fclose(file))
    fail_file(path);
}

static char *trim(char *text) {
  while (isspace((unsigned char)*text))
    text++;
  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1]))
    *--end = 0;
  return text;
}

static void uppercase(char *text) {
  while (*text) {
    *text = (char)toupper((unsigned char)*text);
    text++;
  }
}

static uint32_t parse_number(const char *text) {
  char *end;
  unsigned long value = strtoul(text, &end, 0);
  while (isspace((unsigned char)*end))
    end++;
  if (*end || value > UINT32_MAX)
    fail("invalid integer");
  return (uint32_t)value;
}

static bool is_number(const char *text) {
  char *end;
  (void)strtoul(text, &end, 0);
  while (isspace((unsigned char)*end))
    end++;
  return *text && !*end;
}

static const char *id_to_name(uint16_t id) {
  for (size_t i = 0; i < sizeof known_ids / sizeof known_ids[0]; i++)
    if (known_ids[i].id == id)
      return known_ids[i].name;
  return NULL;
}

static uint16_t name_to_id(const char *text) {
  if (is_number(text)) {
    uint32_t id = parse_number(text);
    if (id > 0xfff)
      fail("cfg_tool item ID exceeds 12 bits");
    return (uint16_t)id;
  }
  for (size_t i = 0; i < sizeof known_ids / sizeof known_ids[0]; i++)
    if (!strcasecmp(known_ids[i].name, text))
      return known_ids[i].id;
  fail("unknown cfg_tool item name");
  return 0;
}

/* Deterministic carrier accepted by the SDK's chip-key decoder. */
static void append_chip_key(struct blob *output, uint16_t key) {
  uint8_t carrier[32];
  memset(carrier, 8, 16); /* Sum of the first half is 128. */
  for (unsigned bit = 0; bit < 16; bit++)
    carrier[16 + bit] = (uint8_t)(8 ^ ((key & (1u << bit)) ? 0x00 : 0x80));
  append(output, carrier, sizeof carrier);
  uint8_t checksum[2];
  write_le16(checksum, crc16(carrier, sizeof carrier));
  append(output, checksum, sizeof checksum);
}

static uint16_t decode_chip_key(const uint8_t carrier[32]) {
  uint8_t sum = 0;
  for (unsigned i = 0; i < 16; i++)
    sum += carrier[i];
  uint8_t threshold = sum >= 0xe0 ? 0xaa : sum <= 0x10 ? 0x55 : sum;
  uint16_t key = 0;
  for (unsigned bit = 0; bit < 16; bit++)
    if ((uint8_t)(carrier[16 + bit] ^ carrier[15 - bit]) < threshold)
      key |= (uint16_t)(1u << bit);
  return key;
}

static struct blob parse_hex(const char *text);

static void isd_encode(const char *input, const char *output, uint16_t key,
                       bool key_forced) {
  FILE *file = fopen(input, "r");
  if (!file)
    fail_file(input);
  struct blob body = {0};
  char line[1024], section[80] = "";
  while (fgets(line, sizeof line, file)) {
    char *comment = strstr(line, "//");
    if (comment)
      *comment = 0;
    char *text = trim(line);
    if (!*text || *text == '#' || *text == ';')
      continue;
    if (*text == '[') {
      char *end = strchr(text, ']');
      if (!end)
        fail("invalid INI section");
      *end = 0;
      snprintf(section, sizeof section, "%s", text + 1);
      uppercase(section);
      continue;
    }
    char *equals = strchr(text, '=');
    if (!equals)
      continue;
    *equals = 0;
    char *name = trim(text), *value = trim(equals + 1);
    char *semicolon = strrchr(value, ';');
    if (semicolon)
      *semicolon = 0;
    value = trim(value);
    uppercase(name);

    if (!strcmp(section, "ISD_CONFIG")) {
      if (!strcmp(name, "CHIP_KEY") && !key_forced) {
        uint32_t parsed = parse_number(value);
        if (parsed > UINT16_MAX)
          fail("chip key exceeds 16 bits");
        key = (uint16_t)parsed;
      }
      continue;
    }
    if (strcmp(section, "SYS_CFG_PARAM"))
      continue;

    uint8_t encoded[512];
    size_t encoded_size;
    if (!strncasecmp(value, "U32:", 4)) {
      write_le32(encoded, parse_number(trim(value + 4)));
      encoded_size = 4;
    } else if (!strncasecmp(value, "STR:", 4)) {
      value = trim(value + 4);
      uppercase(value);
      encoded_size = strlen(value);
      memcpy(encoded, value, encoded_size);
    } else if (!strncasecmp(value, "HEX:", 4)) {
      struct blob bytes = parse_hex(value + 4);
      if (bytes.size > sizeof encoded)
        fail("isd_config HEX value is too long");
      memcpy(encoded, bytes.data, bytes.size);
      encoded_size = bytes.size;
      free(bytes.data);
    } else if (is_number(value)) {
      write_le32(encoded, parse_number(value));
      encoded_size = 4;
    } else {
      uppercase(value);
      encoded_size = strlen(value);
      memcpy(encoded, value, encoded_size);
    }
    if (encoded_size > 255 || strlen(name) + 1 > 255)
      fail("isd_config value or key is too long");
    append_byte(&body, (uint8_t)encoded_size);
    append(&body, name, strlen(name) + 1);
    append(&body, encoded, encoded_size);
  }
  fclose(file);
  struct blob result = {0};
  append_chip_key(&result, key);
  append(&result, body.data, body.size);
  write_file(output, result.data, result.size);
  printf("wrote %s (%zu bytes, chip key 0x%04x)\n", output, result.size, key);
  free(result.data);
  free(body.data);
}

static bool printable_value(const uint8_t *data, size_t size) {
  if (!size)
    return false;
  for (size_t i = 0; i < size; i++)
    if (data[i] < 0x20 || data[i] > 0x7e)
      return false;
  return true;
}

static void print_hex(FILE *output, const uint8_t *data, size_t size) {
  for (size_t i = 0; i < size; i++)
    fprintf(output, "%s%02x", i ? " " : "", data[i]);
}

static float read_le_float(const uint8_t *data) {
  uint32_t bits = read_le32(data);
  float value;
  memcpy(&value, &bits, sizeof value);
  return value;
}

static void isd_decode(const char *input, const char *output_path) {
  size_t size;
  uint8_t *data = read_file(input, &size);
  if (size < 34)
    fail("isd_config.bin is shorter than its chip-key record");
  if (crc16(data, 32) != read_le16(data + 32))
    fail("isd_config.bin chip-key CRC mismatch");
  FILE *output = output_path ? fopen(output_path, "w") : stdout;
  if (!output)
    fail_file(output_path);
  fprintf(output, "# Generated by jlcfg; STR/U32 annotations are lossless.\n");
  fprintf(output, "[ISD_CONFIG]\nCHIP_KEY = 0x%04x\n\n[SYS_CFG_PARAM]\n",
          decode_chip_key(data));
  size_t offset = 34;
  while (offset < size) {
    uint8_t value_size = data[offset++];
    size_t name_start = offset;
    while (offset < size && data[offset])
      offset++;
    if (offset == size)
      fail("unterminated isd_config key");
    const char *name = (const char *)data + name_start;
    offset++;
    if (offset + value_size > size)
      fail("truncated isd_config value");
    const uint8_t *value = data + offset;
    if (printable_value(value, value_size)) {
      fprintf(output, "%s = STR:%.*s\n", name, value_size, value);
    } else if (value_size == 4) {
      fprintf(output, "%s = U32:%" PRIu32 "\n", name, read_le32(value));
    } else {
      fprintf(output, "%s = HEX:", name);
      print_hex(output, value, value_size);
      fputc('\n', output);
    }
    offset += value_size;
  }
  if (output_path && fclose(output))
    fail_file(output_path);
  free(data);
}

static void describe_cfg_item(FILE *output, uint16_t id, const uint8_t *data,
                              size_t size) {
  static const char *const status_names[] = {
      "charge_start",  "charge_full",      "power_on",      "power_off",
      "low_power",     "maximum_volume",   "incoming_call", "outgoing_call",
      "active_call",   "bluetooth_ready",  "bt_connected",  "bt_disconnected",
      "tws_connected", "tws_disconnected",
  };
  static const char *const aec_names[] = {
      "ndt_fade_in",
      "ndt_fade_out",
      "dt_fade_in",
      "dt_fade_out",
      "ndt_max_gain",
      "ndt_min_gain",
      "ndt_speech_threshold",
      "dt_max_gain",
      "dt_min_gain",
      "dt_speech_threshold",
      "echo_present_threshold",
      "aec_dt_aggress",
      "aec_reference_threshold",
      "es_aggress_factor",
      "es_min_suppress",
      "ans_aggress",
      "ans_suppress",
  };
  if (id == 101 && size % 33 == 0) {
    for (size_t i = 0; i < size / 33; i++)
      fprintf(output, "#   name[%zu]: enabled=%u value=\"%.*s\"\n", i,
              data[i * 33], 32, data + i * 33 + 1);
  } else if (id == 102 && size == 6) {
    fprintf(output, "#   Bluetooth MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
            data[5], data[4], data[3], data[2], data[1], data[0]);
  } else if (id == 524 && size == 4) {
    fprintf(output, "#   audio: enabled=%u max=%u default=%u tone=%u\n",
            data[0], data[1], data[2], data[3]);
  } else if (id == 535 && size == 5) {
    fprintf(output,
            "#   charge: enabled=%u power-on=%u full-V=%u full-mA=%u mA=%u\n",
            data[0], data[1], data[2], data[3], data[4]);
  } else if (id == 536 && size == 4) {
    fprintf(output, "#   low power: warning=%u0mV shutdown=%u0mV\n",
            read_le16(data), read_le16(data + 2));
  } else if (id == 601 && size == 1) {
    fprintf(output, "#   Bluetooth RF power: %u\n", data[0]);
  } else if (id == 602 && size == 2) {
    fprintf(output, "#   TWS pair code: 0x%04x\n", read_le16(data));
  } else if (id == 603 && size == 1) {
    fprintf(output, "#   automatic power-off: %u minutes\n", data[0]);
  } else if (id == 604 && size == 72) {
    fprintf(output, "#   AEC: mic_gain=%u dac_gain=%u mode=%u uplink_EQ=%u\n",
            data[0], data[1], data[2], data[3]);
    for (size_t i = 0; i < sizeof aec_names / sizeof aec_names[0]; i++)
      fprintf(output, "#   AEC.%s=%g\n", aec_names[i],
              (double)read_le_float(data + 4 + i * 4));
  } else if (id == 605 && size == 29) {
    fprintf(output, "#   status synchronization: enabled=%u\n", data[0]);
    for (size_t i = 0; i < sizeof status_names / sizeof status_names[0]; i++)
      fprintf(output, "#   status.%s: LED=%u tone=%u\n", status_names[i],
              data[1 + i], data[15 + i]);
  } else if (id == 606 && size % 6 == 0) {
    fprintf(output, "#   key mappings: %zu records of 6 bytes\n", size / 6);
  } else if (id == 607 && size == 9) {
    fprintf(
        output,
        "#   LRC: ws_inc=%u ws_init=%u btosc_inc=%u btosc_init=%u mode=%u\n",
        read_le16(data), read_le16(data + 2), read_le16(data + 4),
        read_le16(data + 6), data[8]);
  } else if (id == 537 && size == 3) {
    fprintf(output, "#   microphone: capless=%u bias_resistor=%u LDO=%u\n",
            data[0], data[1], data[2]);
  } else if (id == 608 && size == 41) {
    fprintf(output,
            "#   BLE: enabled=%u name=\"%.*s\" RF=%u address-enabled=%u "
            "address=%02x:%02x:%02x:%02x:%02x:%02x\n",
            data[0], 32, data + 1, data[33], data[34], data[40], data[39],
            data[38], data[37], data[36], data[35]);
  }
}

static void cfg_decode(const char *input, const char *output_path) {
  size_t size;
  uint8_t *data = read_file(input, &size);
  if (size < 8 || read_le32(data) != size - 4)
    fail("cfg_tool.bin length header mismatch");
  FILE *output = output_path ? fopen(output_path, "w") : stdout;
  if (!output)
    fail_file(output_path);
  fprintf(output,
          "# Lossless jlcfg input. Values are raw bytes; comments are decoded "
          "hints.\n[CFG_TOOL]\n");
  size_t offset = 4;
  bool terminated = false;
  while (offset < size) {
    if (size - offset < 4)
      fail("truncated cfg_tool item header");
    uint32_t header = read_le32(data + offset);
    uint8_t expected_crc = header;
    uint16_t id = (header >> 8) & 0xfff;
    uint16_t value_size = header >> 20;
    offset += 4;
    if (value_size > CFG_ITEM_MAX || offset + value_size > size)
      fail("invalid cfg_tool item length");
    if ((uint8_t)crc16(data + offset, value_size) != expected_crc)
      fail("cfg_tool item CRC mismatch");
    if (!id && !value_size) {
      terminated = true;
      if (offset != size)
        fail("data follows cfg_tool terminator");
      break;
    }
    const char *name = id_to_name(id);
    fprintf(output, "\n# ID %u, %u bytes, CRC 0x%02x\n", id, value_size,
            expected_crc);
    describe_cfg_item(output, id, data + offset, value_size);
    fprintf(output, "%s", name ? name : "");
    if (!name)
      fprintf(output, "%u", id);
    fprintf(output, " = HEX:");
    print_hex(output, data + offset, value_size);
    fputc('\n', output);
    offset += value_size;
  }
  if (!terminated)
    fail("cfg_tool.bin has no terminator item");
  if (output_path && fclose(output))
    fail_file(output_path);
  free(data);
}

static int hex_digit(int c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  c = tolower(c);
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

static struct blob parse_hex(const char *text) {
  struct blob result = {0};
  while (*text) {
    while (isspace((unsigned char)*text) || *text == ',' || *text == ':')
      text++;
    if (!*text)
      break;
    int high = hex_digit(text[0]), low = hex_digit(text[1]);
    if (high < 0 || low < 0)
      fail("invalid HEX byte in cfg_tool description");
    append_byte(&result, (uint8_t)(high << 4 | low));
    text += 2;
  }
  return result;
}

static struct blob parse_integers(const char *text, unsigned width) {
  struct blob result = {0};
  while (*text) {
    while (isspace((unsigned char)*text) || *text == ',')
      text++;
    if (!*text)
      break;
    char *end;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text ||
        value >= (width == 4 ? 0x100000000ul : 1ul << (width * 8)))
      fail("invalid or oversized integer in cfg_tool description");
    uint8_t encoded[4];
    write_le32(encoded, (uint32_t)value);
    append(&result, encoded, width);
    text = end;
  }
  return result;
}

/* A concise representation of ID 101's repeated {enabled, name[32]} group. */
static struct blob parse_bt_names(char *text) {
  struct blob result = {0};
  while (*text) {
    char *separator = strchr(text, '|');
    if (separator)
      *separator = 0;
    char *item = trim(text), *colon = strchr(item, ':');
    if (!colon)
      fail("BT_NAMES entries must look like enabled:name");
    *colon = 0;
    uint32_t enabled = parse_number(trim(item));
    char *name = trim(colon + 1);
    if (enabled > 255 || strlen(name) > 31)
      fail("invalid BT_NAMES enabled value or name length");
    uint8_t encoded[33] = {0};
    encoded[0] = (uint8_t)enabled;
    memcpy(encoded + 1, name, strlen(name));
    append(&result, encoded, sizeof encoded);
    if (!separator)
      break;
    text = separator + 1;
  }
  return result;
}

static void append_cfg_item(struct blob *result, uint16_t id,
                            const uint8_t *data, size_t size) {
  if (size > CFG_ITEM_MAX)
    fail("cfg_tool item exceeds this SDK's 800-byte limit");
  uint32_t header =
      (uint32_t)size << 20 | (uint32_t)id << 8 | (uint8_t)crc16(data, size);
  uint8_t encoded[4];
  write_le32(encoded, header);
  append(result, encoded, sizeof encoded);
  append(result, data, size);
}

static void cfg_encode(const char *input, const char *output) {
  FILE *file = fopen(input, "r");
  if (!file)
    fail_file(input);
  struct blob result = {0};
  uint8_t placeholder[4] = {0};
  append(&result, placeholder, sizeof placeholder);
  char line[8192], section[80] = "";
  while (fgets(line, sizeof line, file)) {
    char *text = trim(line);
    if (!*text || *text == '#' || *text == ';')
      continue;
    if (*text == '[') {
      char *end = strchr(text, ']');
      if (!end)
        fail("invalid config section");
      *end = 0;
      snprintf(section, sizeof section, "%s", text + 1);
      uppercase(section);
      continue;
    }
    if (strcmp(section, "CFG_TOOL"))
      continue;
    char *equals = strchr(text, '=');
    if (!equals)
      fail("expected NAME = HEX:bytes");
    *equals = 0;
    char *name = trim(text), *value = trim(equals + 1);
    uint16_t id = name_to_id(name);
    if (!id)
      continue; /* Terminator is always generated below. */
    struct blob bytes;
    if (!strncasecmp(value, "HEX:", 4))
      bytes = parse_hex(value + 4);
    else if (!strncasecmp(value, "U8:", 3))
      bytes = parse_integers(value + 3, 1);
    else if (!strncasecmp(value, "U16:", 4))
      bytes = parse_integers(value + 4, 2);
    else if (!strncasecmp(value, "U32:", 4))
      bytes = parse_integers(value + 4, 4);
    else if (!strncasecmp(value, "BT_NAMES:", 9)) {
      if (id != 101)
        fail("BT_NAMES is only valid for CFG_BT_NAME");
      bytes = parse_bt_names(value + 9);
    } else
      fail("cfg_tool value must use HEX, U8, U16, U32, or BT_NAMES");
    append_cfg_item(&result, id, bytes.data, bytes.size);
    free(bytes.data);
  }
  fclose(file);
  append_cfg_item(&result, 0, NULL, 0);
  write_le32(result.data, (uint32_t)result.size - 4);
  write_file(output, result.data, result.size);
  printf("wrote %s (%zu bytes)\n", output, result.size);
  free(result.data);
}

static void usage(void) {
  puts("usage:\n"
       "  jlcfg isd-decode INPUT [OUTPUT]\n"
       "  jlcfg isd-encode [-k CHIP_KEY] INPUT.ini OUTPUT.bin\n"
       "  jlcfg cfg-decode INPUT.bin [OUTPUT.conf]\n"
       "  jlcfg cfg-encode INPUT.conf OUTPUT.bin");
}

int main(int argc, char **argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  if (!strcmp(argv[1], "isd-decode") && (argc == 3 || argc == 4)) {
    isd_decode(argv[2], argc == 4 ? argv[3] : NULL);
    return 0;
  }
  if (!strcmp(argv[1], "isd-encode")) {
    uint16_t key = 0xffff;
    bool key_forced = false;
    int arg = 2;
    if (arg < argc && !strcmp(argv[arg], "-k")) {
      if (arg + 1 >= argc)
        fail("missing chip key");
      uint32_t parsed = parse_number(argv[arg + 1]);
      if (parsed > UINT16_MAX)
        fail("chip key exceeds 16 bits");
      key = (uint16_t)parsed;
      key_forced = true;
      arg += 2;
    }
    if (argc - arg != 2)
      fail("isd-encode requires input and output paths");
    isd_encode(argv[arg], argv[arg + 1], key, key_forced);
    return 0;
  }
  if (!strcmp(argv[1], "cfg-decode") && (argc == 3 || argc == 4)) {
    cfg_decode(argv[2], argc == 4 ? argv[3] : NULL);
    return 0;
  }
  if (!strcmp(argv[1], "cfg-encode") && argc == 4) {
    cfg_encode(argv[2], argv[3]);
    return 0;
  }
  usage();
  return 2;
}
