#pragma once
#include <CardWriter.h>
#include <algorithm>
#include <array>

namespace surface::device {
// NXP NTAG213_215_216 table 5: dynamic lock bytes are outside user memory.
inline constexpr std::array<uint8_t, 5> ntag213LockControl{0x01, 0x03, 0xa0, 0x0c, 0x34};
struct NtagLayout {
  size_t prefixBytes = 0, messageBytes = 0, terminator = 0;
};
// Inspect the first 16 user bytes. Only the documented NTAG213 lock descriptor
// may precede NDEF; arbitrary reserved ranges/control TLVs remain unsupported.
inline Result inspectNtagLayout(size_t userBytes, size_t capacity, const uint8_t* first,
                                NtagLayout& layout) {
  layout = {};
  if (userBytes == 144 && std::equal(ntag213LockControl.begin(), ntag213LockControl.end(), first))
    layout.prefixBytes = ntag213LockControl.size();
  const auto* tlv = first + layout.prefixBytes;
  if (tlv[0] != 0x03) {
    const char* hex = "0123456789ABCDEF";
    std::string bytes;
    for (size_t i = 0; i < 8; ++i) {
      if (i)
        bytes += ' ';
      bytes += hex[first[i] >> 4];
      bytes += hex[first[i] & 15];
    }
    return Result::fail("Unsupported tag layout; first bytes: " + bytes);
  }
  const size_t lengthBytes = tlv[1] == 0xff ? 4 : 2;
  layout.messageBytes = tlv[1] == 0xff ? (size_t(tlv[2]) << 8) + tlv[3] : tlv[1];
  layout.terminator = layout.prefixBytes + lengthBytes + layout.messageBytes;
  if (layout.terminator >= capacity)
    return Result::fail("Invalid NDEF length or missing terminator");
  return {};
}
// NTAG213/215/216 memory layout, NXP NTAG213_215_216 sections 8.5/8.7.
// Only ordinary, already Type-2-formatted tags are written. No lock, CC,
// password, configuration, reserved TLV, or manufacturer bytes are modified.
inline Result inspectNtag(size_t userBytes, const uint8_t* page0, const uint8_t* dynamic,
                          size_t& capacity) {
  if (userBytes != 144 && userBytes != 504 && userBytes != 888)
    return Result::fail("Writer supports NTAG213/215/216 only");
  if (page0[12] != 0xe1 || page0[13] != 0x10 || page0[15] != 0)
    return Result::fail("Tag needs an unrestricted Type 2 capability container");
  capacity = std::min(userBytes, size_t(page0[14]) * 8);
  if (!capacity)
    return Result::fail("Tag reports zero NDEF capacity");
  if (page0[10] || page0[11] || dynamic[0] || dynamic[1] || dynamic[2])
    return Result::fail("Tag has locked pages; use an unlocked card");
  if (dynamic[7] <= 3 + userBytes / 4)
    return Result::fail("Tag is password protected; use an unprotected card");
  if (dynamic[4] & 0xc0)
    return Result::fail("Tag has UID/counter mirroring enabled");
  return {};
}

// Page-sized transaction: invalidate the message length, write the body, then
// commit the page containing the NDEF length last. Preserve the factory prefix.
// Removal/power loss is still not atomic or recoverable.
// This bounds each UI loop to one low-level write without sleeps or retries.
class NtagWritePages {
  std::vector<uint8_t> bytes;
  size_t next = 0, commit = 0, prefix = 0;

public:
  Result begin(const std::string& payload, size_t capacity, size_t prefixBytes = 0) {
    clear();
    if (prefixBytes != 0 && prefixBytes != ntag213LockControl.size())
      return Result::fail("Unsupported NDEF offset");
    prefix = prefixBytes;
    commit = prefix / 4;
    bytes = cardNdef(payload);
    if (prefix)
      bytes.insert(bytes.begin(), ntag213LockControl.begin(), ntag213LockControl.end());
    if (bytes.size() > capacity || (bytes.size() + 3) / 4 * 4 > capacity) {
      const auto needed = bytes.size();
      bytes.clear();
      const std::string prefix = std::string(cardEncodingName(payload)) == "json"
                                     ? "Card too small for this advanced intent: card needs "
                                     : "Card needs ";
      return Result::fail(prefix + std::to_string(needed) + " bytes; tag capacity is " +
                          std::to_string(capacity) + " bytes");
    }
    bytes.resize((bytes.size() + 3) / 4 * 4, 0);
    next = 0;
    return {};
  }
  bool done() const { return bytes.empty() || next > bytes.size() / 4 - commit; }
  std::pair<uint8_t, std::array<uint8_t, 4>> page() const {
    if (next == 0)
      return prefix ? std::make_pair(uint8_t(5), std::array<uint8_t, 4>{0x34, 0x03, 0x00, 0xfe})
                    : std::make_pair(uint8_t(4), std::array<uint8_t, 4>{0x03, 0x00, 0xfe, 0x00});
    const size_t offset = (next == bytes.size() / 4 - commit ? commit : next + commit) * 4;
    return {uint8_t(4 + offset / 4),
            {bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]}};
  }
  void advance() { ++next; }
  void clear() {
    bytes.clear();
    next = 0;
  }
};
} // namespace surface::device
