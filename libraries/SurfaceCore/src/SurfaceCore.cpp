#include "SurfaceCore.h"
#include <surface_json.hpp>
#include <algorithm>
#include <cctype>
#include <set>

namespace surface {
namespace {
using Json = nlohmann::json;
bool validUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const auto first = static_cast<uint8_t>(s[i++]);
    if (first < 0x80) continue;
    unsigned extra;
    uint32_t code, minimum;
    if (first >= 0xC2 && first <= 0xDF) { extra = 1; code = first & 0x1F; minimum = 0x80; }
    else if (first >= 0xE0 && first <= 0xEF) { extra = 2; code = first & 0x0F; minimum = 0x800; }
    else if (first >= 0xF0 && first <= 0xF4) { extra = 3; code = first & 7; minimum = 0x10000; }
    else return false;
    if (i + extra > s.size()) return false;
    while (extra--) {
      const auto next = static_cast<uint8_t>(s[i++]);
      if ((next & 0xC0) != 0x80) return false;
      code = (code << 6) | (next & 0x3F);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
  }
  return true;
}
std::string trim(const std::string& s) {
  auto begin = s.find_first_not_of(" \r\n\t");
  return begin == std::string::npos ? "" : s.substr(begin, s.find_last_not_of(" \r\n\t") - begin + 1);
}
bool digits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}
std::vector<std::string> split(const std::string& s, char separator) {
  std::vector<std::string> result;
  size_t start = 0, end;
  do {
    end = s.find(separator, start);
    result.push_back(s.substr(start, end == std::string::npos ? end : end - start));
    start = end + 1;
  } while (end != std::string::npos);
  return result;
}
int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
bool decode(const std::string& s, std::string& out) {
  out.clear();
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = s[i];
    if (c == '%') {
      if (i + 2 >= s.size() || hex(s[i + 1]) < 0 || hex(s[i + 2]) < 0) return false;
      c = hex(s[i + 1]) * 16 + hex(s[i + 2]);
      i += 2;
    }
    if (c < 32 || c == 127) return false;
    out += static_cast<char>(c);
  }
  return true;
}
bool keys(const Json& object, std::initializer_list<const char*> allowed) {
  if (!object.is_object()) return false;
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (std::none_of(allowed.begin(), allowed.end(), [&](const char* key) { return key == it.key(); })) return false;
  }
  return true;
}
bool boundedJson(const std::string& text) {
  // A lexical nesting guard, not a replacement JSON parser.
  bool quoted = false, escaped = false;
  int depth = 0;
  for (char c : text) {
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
    } else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') { if (++depth > 8) return false; }
    else if (c == '}' || c == ']') { if (--depth < 0) return false; }
  }
  return depth == 0 && !quoted;
}
size_t members(const Json& value) {
  size_t total = 0;
  if (value.is_structured()) {
    total += value.size();
    for (const auto& child : value) total += members(child);
  }
  return total;
}
} // namespace

Result normalizeAppleUrl(const std::string& input, Source& source) {
  const auto url = trim(input);
  const auto bad = Result::fail("Unsupported Apple Music URL: use an album, playlist, track, or station share URL");
  if (url.size() > 4096 || !validUtf8(url) || url.compare(0, 8, "https://") != 0) return bad;
  for (unsigned char c : url) if (c <= 32 || c == 127 || c == '\\') return bad;
  auto slash = url.find('/', 8);
  if (slash == std::string::npos) return bad;
  std::string host = url.substr(8, slash - 8);
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
  if (host != "music.apple.com" && host != "music.apple.com:443") return bad;
  auto fragment = url.find('#', slash);
  std::string pathQuery = url.substr(slash + 1, fragment == std::string::npos ? fragment : fragment - slash - 1);
  auto question = pathQuery.find('?');
  auto path = pathQuery.substr(0, question);
  auto parts = split(path, '/');
  if (parts.size() != 4 || parts[0].empty() || parts[2].empty() || parts[3].empty()) return bad;
  Source parsed;
  if (!decode(parts[0], parsed.storefront) || !decode(parts[3], parsed.catalogId)) return bad;
  std::string slug;
  if (!decode(parts[2], slug)) return bad;
  if (parts[1] == "album") {
    if (!digits(parsed.catalogId)) return bad;
    parsed.kind = SourceKind::Album;
  } else if (parts[1] == "playlist") parsed.kind = SourceKind::Playlist;
  else if (parts[1] == "station") {
    if (parsed.catalogId.compare(0, 3, "ra.") != 0 || parsed.catalogId.size() <= 3 ||
        parsed.catalogId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos)
      return bad;
    parsed.kind = SourceKind::Station;
  }
  else return bad;
  std::string trackId;
  bool hasTrack = false;
  if (question != std::string::npos) {
    for (const auto& pair : split(pathQuery.substr(question + 1), '&')) {
      const auto equals = pair.find('=');
      std::string name, value;
      if (!decode(pair.substr(0, equals), name) ||
          !decode(equals == std::string::npos ? "" : pair.substr(equals + 1), value)) return bad;
      if (name == "i") {
        if (hasTrack || parts[1] != "album" || !digits(value)) return bad;
        hasTrack = true;
        trackId = value;
      }
    }
  }
  parsed.url = "https://music.apple.com/" + path;
  if (hasTrack) {
    parsed.kind = SourceKind::Track;
    parsed.catalogId = trackId;
    parsed.url += "?i=" + trackId;
  }
  source = std::move(parsed);
  return {};
}

Result validateIntent(const MusicIntent& intent) {
  if (!intent.source && !intent.transport && !intent.shuffle.has_value()) return Result::fail("Empty intent");
  if (intent.transport && *intent.transport != TransportCommand::Play && *intent.transport != TransportCommand::Pause)
    return Result::fail("Unsupported transport");
  if (intent.source) {
    if (intent.source->kind == SourceKind::Station && intent.shuffle.has_value())
      return Result::fail("Shuffle is unsupported for stations");
    if (intent.transport != TransportCommand::Play) return Result::fail("This slice requires explicit transport=play with source");
    Source normalized;
    auto r = normalizeAppleUrl(intent.source->url, normalized);
    if (!r.ok) return r;
    if (normalized.catalogId != intent.source->catalogId || normalized.kind != intent.source->kind ||
        normalized.storefront != intent.source->storefront) return Result::fail("Inconsistent normalized source");
  }
  return {};
}

Result parseIntent(const std::string& payload, MusicIntent& output) {
  if (payload.size() > 4096) return Result::fail("Card exceeds 4096 bytes");
  const auto text = trim(payload);
  MusicIntent intent;
  if (text.compare(0, 8, "https://") == 0) {
    Source source;
    auto result = normalizeAppleUrl(text, source);
    if (!result.ok) return result;
    intent.source = source;
    intent.transport = TransportCommand::Play;
  } else {
    if (!boundedJson(text)) return Result::fail("Invalid JSON or nesting exceeds eight containers");
    bool duplicate = false;
    std::vector<std::set<std::string>> keyStack;
    auto root = Json::parse(text, [&](int, Json::parse_event_t event, Json& value) {
      if (event == Json::parse_event_t::object_start) keyStack.emplace_back();
      if (event == Json::parse_event_t::key && !keyStack.back().insert(value.get<std::string>()).second) duplicate = true;
      if (event == Json::parse_event_t::object_end) keyStack.pop_back();
      return true;
    }, false);
    if (root.is_discarded() || duplicate || members(root) > 128) return Result::fail("Invalid/duplicate/oversized JSON");
    if (!keys(root, {"format", "version", "intent", "label", "extensions", "requires"}) ||
        !root.contains("format") || root["format"] != "sonos-surface" ||
        !root.contains("version") || !root["version"].is_number_integer() || root["version"] != 1 ||
        !root.contains("intent")) return Result::fail("Expected sonos-surface version 1 envelope");
    if (root.contains("label") && !root["label"].is_string()) return Result::fail("label must be text");
    if (root.contains("extensions")) {
      if (!root["extensions"].is_object()) return Result::fail("extensions must be an object");
      for (auto it = root["extensions"].begin(); it != root["extensions"].end(); ++it)
        if (it.key().find('.') == std::string::npos) return Result::fail("Extension names must be namespaced");
    }
    if (root.contains("requires") && (!root["requires"].is_array() || !root["requires"].empty()))
      return Result::fail("Required extensions are unsupported by this slice");
    const auto& fields = root["intent"];
    if (!keys(fields, {"source", "transport", "shuffle"}))
      return Result::fail("Unsupported intent field (volume/repeat/next are not implemented)");
    if (fields.contains("source")) {
      const auto& s = fields["source"];
      if (!keys(s, {"service", "url"}) || !s.contains("service") || s["service"] != "apple-music" ||
          !s.contains("url") || !s["url"].is_string()) return Result::fail("Invalid source");
      Source source;
      auto result = normalizeAppleUrl(s["url"].get<std::string>(), source);
      if (!result.ok) return result;
      intent.source = source;
    }
    if (fields.contains("transport")) {
      if (fields["transport"] == "play") intent.transport = TransportCommand::Play;
      else if (fields["transport"] == "pause") intent.transport = TransportCommand::Pause;
      else return Result::fail("Only play/pause transport is implemented");
    }
    if (fields.contains("shuffle")) {
      if (!fields["shuffle"].is_boolean()) return Result::fail("shuffle must be true or false");
      intent.shuffle = fields["shuffle"].get<bool>();
    }
  }
  auto result = validateIntent(intent);
  if (result.ok) output = std::move(intent);
  return result;
}

ResolvedIntent resolvePolicy(const MusicIntent& intent, const PolicyContext& context) {
  ResolvedIntent result{intent, intent.shuffle.has_value() ? "explicit" : "preserve", context.revision};
  if (!intent.shuffle.has_value() && intent.source) {
    if (intent.source->kind == SourceKind::Playlist && !context.playlistShuffleRoom.empty() &&
        context.targetId == context.playlistShuffleRoom) {
      result.intent.shuffle = true;
      result.shuffleOrigin = "playlist-room-shuffle";
    } else if (intent.source->kind == SourceKind::Album) {
      result.intent.shuffle = false;
      result.shuffleOrigin = "albums-in-order";
    }
  }
  return result;
}

Result decodeNdefText(const uint8_t* data, size_t size, std::string& text) {
  if (!data || size < 2 || size > 4160 || (data[0] & 0xC0)) return Result::fail("Expected UTF-8 NDEF Text payload");
  size_t language = data[0] & 0x3F;
  if (!language || language + 1 >= size || size - language - 1 > 4096) return Result::fail("Invalid NDEF language/length");
  for (size_t i = 1; i <= language; ++i)
    if (!std::isalnum(data[i]) && data[i] != '-') return Result::fail("Invalid NDEF language");
  text.assign(reinterpret_cast<const char*>(data + 1 + language), size - 1 - language);
  if (!validUtf8(text)) return Result::fail("Malformed UTF-8 NDEF text");
  return {};
}

Result decodeNdefUri(const uint8_t* data, size_t size, std::string& url) {
  if (!data || size < 2 || size > 4097) return Result::fail("Invalid NDEF URI length");
  // NFC URI RTD: 0x00 carries the full URI; 0x04 abbreviates https://.
  // Other prefixes cannot represent an accepted https://music.apple.com URL.
  if (data[0] != 0x00 && data[0] != 0x04) return Result::fail("Unsupported NDEF URI prefix (need Apple Music HTTPS URL)");
  std::string decoded = data[0] == 0x04 ? "https://" : "";
  if (size - 1 > 4096 - decoded.size()) return Result::fail("NDEF URI exceeds 4096 bytes");
  decoded.append(reinterpret_cast<const char*>(data + 1), size - 1);
  if (!validUtf8(decoded)) return Result::fail("Malformed UTF-8 NDEF URI");
  Source source;
  auto result = normalizeAppleUrl(decoded, source);
  if (!result.ok) return result;
  url = std::move(source.url);
  return {};
}

Result decodeNdefRecord(uint8_t tnf, const std::string& type, const uint8_t* data, size_t size, std::string& text) {
  if (tnf == 0 && type.empty() && !size) return Result::fail("Empty NDEF record: no music URL");
  if (tnf != 1) return Result::fail("Unsupported NFC TNF=" + std::to_string(tnf));
  if (type == "T") return decodeNdefText(data, size, text);
  if (type == "U") return decodeNdefUri(data, size, text);
  // Observed household legacy cards: well-known TNF, zero type bytes, raw URL.
  // This exception must not turn arbitrary record payloads into JSON intents.
  if (!type.empty()) return Result::fail("Unsupported NFC type=" + type.substr(0, 32));
  if (!data || !size || size > 4096) return Result::fail("Invalid legacy NFC URL length");
  Source source;
  auto result = normalizeAppleUrl(std::string(reinterpret_cast<const char*>(data), size), source);
  if (!result.ok) return result;
  text = std::move(source.url);
  return {};
}

const char* operationName(Operation op) {
  switch (op) {
    case Operation::Stop: return "Stop";
    case Operation::ClearQueue: return "ClearQueue";
    case Operation::AddSource: return "AddSource";
    case Operation::SelectQueue: return "SelectQueue";
    case Operation::SelectStation: return "SelectStation";
    case Operation::ApplyMode: return "ApplyMode";
    case Operation::Play: return "Play";
    case Operation::Pause: return "Pause";
  }
  return "Unknown";
}
Result makePlan(const ResolvedIntent& resolved, Plan& plan) {
  auto valid = validateIntent(resolved.intent);
  if (!valid.ok) return valid;
  plan = {resolved, {}};
  if (resolved.intent.source) {
    if (resolved.intent.source->kind == SourceKind::Station) plan.operations = {Operation::SelectStation};
    else plan.operations = {Operation::Stop, Operation::ClearQueue, Operation::AddSource,
                       Operation::SelectQueue, Operation::ApplyMode};
  } else if (resolved.intent.shuffle.has_value()) plan.operations.push_back(Operation::ApplyMode);
  if (resolved.intent.transport)
    plan.operations.push_back(*resolved.intent.transport == TransportCommand::Play ? Operation::Play : Operation::Pause);
  return {};
}

Application::Application(SonosTransport& transport, PolicyContext context, Changed changed)
    : transport_(transport), context_(std::move(context)), changed_(std::move(changed)) {}
void Application::publish() { if (changed_) changed_(state_); }
Result Application::refresh() {
  if (busy_) return Result::fail("Busy");
  auto next = state_.observed;
  auto result = transport_.refresh(next);
  if (result.ok) state_.observed = std::move(next);
  else { state_.observed.stale = true; state_.detail = "Refresh: " + result.error; }
  publish();
  return result;
}
Result Application::submit(const std::string& payload) {
  if (busy_) return Result::fail("Busy");
  if (state_.recoveryRequired) return Result::fail("Uncertain previous effect; inspect speaker and reboot before a deliberate retry");
  ++state_.requestId;
  MusicIntent intent;
  auto result = parseIntent(payload, intent);
  Plan plan;
  if (result.ok) result = makePlan(resolvePolicy(intent, context_), plan);
  if (!result.ok) {
    state_.status = "failed";
    state_.detail = result.error;
    publish();
    return result;
  }
  busy_ = true;
  state_.status = "pending";
  state_.detail = "Preflight";
  state_.shuffleOrigin = plan.resolved.shuffleOrigin;
  publish();
  result = transport_.prepare(plan.resolved);
  size_t completed = 0;
  if (result.ok) {
    for (auto op : plan.operations) {
      state_.detail = operationName(op);
      publish();
      result = transport_.execute(op);
      if (!result.ok) break;
      ++completed;
    }
  }
  if (result.ok) {
    state_.detail = "Verify playback";
    publish();
    result = transport_.verify(plan.resolved, state_.observed);
  }
  if (!result.ok) {
    PlaybackState next = state_.observed;
    if (transport_.refresh(next).ok) state_.observed = std::move(next);
    else state_.observed.stale = true;
  }
  state_.recoveryRequired = result.uncertain;
  state_.status = result.ok ? "succeeded" : result.uncertain ? "uncertain" : completed ? "partial" : "failed";
  state_.detail = result.ok ? "Observed requested result" : result.error;
  busy_ = false;
  publish();
  return result;
}
} // namespace surface
