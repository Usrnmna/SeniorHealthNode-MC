#pragma once
#include <HealthNodeConfig.h>
#include <Utils.h>
#include <cstdio>
#include <cstring>

// A channel identity is a key, never a companion's editable channel slot.
// Copies are intentional: each incident and queued message owns its destination.
struct FallChannel {
  static constexpr size_t value_capacity = 112;
  char name[32] = {};
  uint8_t key[32] = {};
  uint8_t key_length = sizeof(HealthNodeConfig::ack_channel_key);
  bool public_channel = true;

  FallChannel() {
    static_assert(sizeof(HealthNodeConfig::ack_channel_name) <= sizeof(name), "Channel name too long");
    memcpy(name, HealthNodeConfig::ack_channel_name, sizeof(HealthNodeConfig::ack_channel_name));
    memcpy(key, HealthNodeConfig::ack_channel_key, sizeof(HealthNodeConfig::ack_channel_key));
  }
  bool valid() const { return key_length == 16 || key_length == 32; }

  // Parse one complete custom-variable value. Reject malformed input without
  // changing the caller's existing selection. Names are single UTF-8 tokens,
  // at most 31 bytes; protocol separators and control characters are excluded.
  static bool parse(const char* value, FallChannel& result) {
    if (strlen(value) >= value_capacity) return false;
    FallChannel candidate;
    const char* label;
    if (strncmp(value, "public ", 7) == 0) {
      candidate.public_channel = true; label = value + 7;
    } else if (strncmp(value, "private ", 8) == 0) {
      candidate.public_channel = false; label = value + 8;
    } else return false;
    const char* separator = strchr(label, ' ');
    const size_t length = separator ? size_t(separator - label) : strlen(label);
    if (!length || length >= sizeof(candidate.name) || !validUtf8(label, length)) return false;
    for (size_t i = 0; i < length; ++i) {
      const auto c = static_cast<unsigned char>(label[i]);
      if (c <= 32 || c == 127 || c == ',' || c == ':' || c == '"' || c == '\\') return false;
    }
    memset(candidate.name, 0, sizeof(candidate.name));
    memcpy(candidate.name, label, length);
    memset(candidate.key, 0, sizeof(candidate.key));
    if (candidate.public_channel) {
      if (separator || length < 2 || label[0] != '#') return false;
      candidate.key_length = 16;
      mesh::Utils::sha256(candidate.key, 16, reinterpret_cast<const uint8_t*>(label), length);
    } else {
      if (!separator || !decodeKey(separator + 1, candidate.key, candidate.key_length)) return false;
      // BaseChatMesh treats a zero-padded 16-byte key as 128-bit when deriving
      // the channel hash, even if it arrived in a 32-byte storage buffer.
      const uint8_t zeroes[16] = {};
      if (candidate.key_length == 32 && memcmp(candidate.key + 16, zeroes, 16) == 0)
        candidate.key_length = 16;
    }
    result = candidate;
    return true;
  }

  // Diagnostic output never contains a private key. The storage form is used
  // only inside the node's settings files and must never be logged or returned.
  void describe(char* dest, size_t capacity) const {
    if (!valid()) snprintf(dest, capacity, "invalid; set fall.channel required");
    else snprintf(dest, capacity, "%s %s", public_channel ? "public" : "private", name);
  }
  void storageValue(char (&dest)[value_capacity]) const {
    describe(dest, sizeof(dest));
    if (!public_channel && valid()) {
      const size_t offset = strlen(dest);
      dest[offset] = ' ';
      mesh::Utils::toHex(dest + offset + 1, key, key_length);
    }
  }

private:
  static bool validUtf8(const char* text, size_t length) {
    for (size_t i = 0; i < length;) {
      const uint8_t lead = uint8_t(text[i++]);
      if (lead < 0x80) continue;
      const unsigned extra = lead >= 0xc2 && lead <= 0xdf ? 1 :
          lead >= 0xe0 && lead <= 0xef ? 2 : lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
      if (!extra || i + extra > length) return false;
      uint32_t point = lead & (0x7f >> extra);
      for (unsigned n = 0; n < extra; ++n) {
        const uint8_t byte = uint8_t(text[i++]);
        if ((byte & 0xc0) != 0x80) return false;
        point = (point << 6) | (byte & 0x3f);
      }
      if (point < (extra == 1 ? 0x80u : extra == 2 ? 0x800u : 0x10000u) ||
          point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
    }
    return true;
  }
  static int base64Digit(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
  }
  // Accept 16/32 bytes as exact hex or canonical padded Base64 (as exported by
  // MeshCore clients). Do not treat a password or a channel label as a key.
  static bool decodeKey(const char* text, uint8_t* dest, uint8_t& length) {
    const size_t n = strlen(text);
    if (n == 32 || n == 64) {
      for (size_t i = 0; i < n; ++i) if (!mesh::Utils::isHexChar(text[i])) return false;
      length = n / 2;
      return mesh::Utils::fromHex(dest, length, text);
    }
    if (n != 24 && n != 44) return false;
    length = n == 24 ? 16 : 32;
    const size_t digits = n == 24 ? 22 : 43;
    for (size_t i = digits; i < n; ++i) if (text[i] != '=') return false;
    unsigned bits = 0, count = 0, written = 0;
    for (size_t i = 0; i < digits; ++i) {
      const int digit = base64Digit(text[i]);
      if (digit < 0) return false;
      bits = (bits << 6) | unsigned(digit); count += 6;
      if (count >= 8) { count -= 8; dest[written++] = uint8_t(bits >> count); }
    }
    return written == length && (bits & ((1u << count) - 1)) == 0;
  }
};
