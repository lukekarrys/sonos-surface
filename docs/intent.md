# MusicIntent and NFC card specification

## Contract

The shared core implements source, play/pause/next/previous, absolute/relative volume, shuffle, repeat, absolute seek, and existing active-queue item selection through shared planning and Sonos boundaries. Source-only and source + pause capture and preserve playing/non-playing state. Required extensions remain unsupported. The Stick writer uses the same parser and validator. The [capability contract](sonos-capabilities.md) defines timing and queue observations. See [hardware evidence](hardware.md) for physical validation and limitations.

A MusicIntent declares requested state and, optionally, one transport command. It is not a sequence of Sonos calls. NFC, touch/buttons, and resolved voice MUST normalize into this same model. Target, request identity, and policy provenance belong to the execution envelope, not the portable intent. The JSON/card contract is deliberately defined here; Sonos mapping and NFC driver/tag support require empirical validation, not assumptions in the format.

Structured v1 documents use UTF-8 JSON:

```json
{
  "format": "sonos-surface",
  "version": 1,
  "intent": {
    "source": {
      "service": "apple-music",
      "url": "https://music.apple.com/us/album/example-album/123456789"
    },
    "transport": "play",
    "volume": { "set": 25 }
  }
}
```

Example catalog IDs are illustrative, not guaranteed playable media. The same document shape is suitable for a card or future voice response. Internal callers MAY construct typed intents directly but MUST receive equivalent validation. Parsing and policy resolution MUST have no Sonos side effects.

## Fields and omission

| Intent field | V1 value | Meaning |
| --- | --- | --- |
| `source` | Object with exactly `service: "apple-music"` and `url` string | Replace/select the queue for an album, playlist, or track; select a direct radio source for a station |
| `transport` | `"play"`, `"pause"`, `"next"`, or `"previous"` | Start/resume, become non-playing, or advance/rewind once |
| `volume` | Exactly one of `{ "set": N }` or `{ "delta": N }` | Absolute level or relative adjustment |
| `shuffle` | Boolean | Requested shuffle state, independent of repeat |
| `seek` | Exactly `{ "positionMs": N }` | Absolute position in the current track; integer milliseconds, 0–4294967295 |
| `queueIndex` | Integer, 0–4294967294 | Select an existing item in the active queue; zero-based |
| `repeat` | `"off"`, `"all"`, or `"one"` | Requested repeat state, independent of shuffle |

Absent fields MUST remain absent until a matching policy fills them. Anything still absent after policy resolution means preserve; it does not mean a default, `false`, zero, or an empty source. Known intent fields reject `null`, stringified numbers/booleans, and other wrong types. Reject duplicate object keys everywhere and non-finite numbers. An empty intent, or one with only optional extensions and no recognized intent fields, is invalid. Extension values may use JSON null.

`volume.set` is an integer in 0–100; out-of-range values are invalid. `volume.delta` is an integer in -100–100. At execution, read fresh volume once and calculate `clamp(current + delta, 0, 100)`. Freeze that absolute target for this request's retries. A new request reads a new baseline; retrying MUST NOT apply the delta again. Zero delta is valid and has no write effect.

Omission preserves final state where changing another property has side effects:

- Without `source`, do not clear, replace, or select a queue as a convenience.
- With `source`, content and position necessarily change. Omitted transport preserves **playing versus non-playing**, not the exact paused/stopped label or old track position. Playing resumes on the new source; paused/stopped stays non-playing. Unknown or transitioning playback must be refreshed/resolved before mutation. Explicit `pause` also means non-playing; an already stopped speaker satisfies it.
- A normal “play this” card MUST normalize to explicit `transport: "play"`. Raw URL and `ss1` encodings imply Play by contract; structured JSON must include the field. The simple writer always constructs explicit Play; transport is not an option in that UI or a default for structured JSON.
- Omitted shuffle/repeat preserve their pre-mutation values unless policy fills them. When Sonos exposes one combined play mode, the planner must read and carry forward the omitted component. Source-change side effects may require restoring preserved mode or mute; do not otherwise reassert omitted volume.
- `next`/`previous` are imperative exceptions: exactly one native skip attempt with the speaker's native end-of-queue behavior. It MUST NOT be combined with `source` or repeat/shuffle changes in v1; combining it with volume is allowed. Do not invent a replay fallback at the queue boundary. Its exact paused-state behavior is a real-device capability to validate, not an implicit `play` request.

## Source-specific mode validity

Shared intent validation applies this matrix whenever the incoming intent contains its own normalized source. Every input surface, including direct typed touch intents, the writer, and future voice, receives the same validation before policy or Sonos mutation. Omission is valid for every field; [policy](policy.md) determines whether it derives a value or preserves state.

| Source in intent | Explicit shuffle | Explicit repeat |
| --- | --- | --- |
| Album | true / false | off / all |
| Playlist | true / false | off / all |
| Track (album URL with `i`) | Invalid, including false | off / one |
| Station | Invalid, including false | Invalid, including off |
| No source | true / false | off / all / one, subject to generic adapter capability |

Album/playlist repeat=one and track repeat=all reject. Never inspect currently playing media to infer a declarative source kind or retroactively apply this matrix/defaults. A no-source `{"repeat":"one"}` remains valid for generic Sonos queue modes. The adapter may still reject unavailable capabilities, such as mode writes against a live station; that does not classify it as a declarative source.

Stations derive neither shuffle nor repeat. Selection does not issue queue clear/add/select or mode writes, and leaves the stored queue intact. Do not report a station as a selected queue. Mute/volume preservation still applies.

Seek and queueIndex cannot combine with each other, source, or next/previous. Both preserve transport unless explicit play/pause is supplied, and may combine with volume or mode. Seek beyond known duration and selection outside fresh queue bounds reject before dispatch; see [details](sonos-capabilities.md#seek-and-queue-selection).

Combinations satisfying these constraints are valid, including source + pause, album/playlist + shuffle + play + volume, and volume alone. Validation rejects the entire intent before mutation if a known field/combination is invalid or unsupported. V1 has no toggle, queue append, delay, or sequence fields. UI toggles MUST resolve from fresh state to explicit values before submission; unknown state requires refresh rather than guessing.

## Apple Music normalization

Accept public `https://music.apple.com` URLs without credentials or nonstandard ports. V1 paths are `/{storefront}/album/{slug}/{id}` and `/{storefront}/playlist/{slug}/{id}`, plus `/{storefront}/station/{slug}/{id}`. A nonempty single `i` query parameter on an album URL identifies a **track**, not the album. Album/track IDs must be decimal digits; playlist IDs must be nonempty and treated as opaque. Station IDs start with `ra.` followed by a nonempty opaque identifier containing ASCII letters, digits, `.`, `_`, or `-`; this includes personal `ra.u-...` stations. Empty or repeated `i`, a track selector on a playlist/station, malformed paths, and unsupported types (artist, search) are invalid. Slugs are labels, not identifiers.

Normalization retains storefront, semantic query `i`, and catalog ID; it ignores fragments and nonsemantic tracking parameters. It produces a canonical source plus `kind: album | playlist | track | station`, `catalogId`, and `storefront` for policy and transport. These derived identity fields are not accepted as competing wire fields. Structured JSON serializes only `source.service` and `source.url`; raw/compact NFC encodings carry the Apple Music URL alone.

V1 does not follow short links or arbitrary redirects. Parsing a URL does not prove playback access. Transport preflight must resolve necessary Sonos URI/metadata and reject known unsupported content before clearing the queue. Sonos account/service registration and metadata generation are adapter concerns, not card content. Preserve the public URL for portability and editing. Personal station access depends on the Apple Music account registered with Sonos; a syntactically valid station URL does not establish account access.

## Compact NFC wire format

NFC wire encoding is separate from the in-memory MusicIntent model: NDEF decode → card payload parse → shared validation → policy → planner/executor. Three current input encodings normalize to that same model:

| Encoding | Payload | Meaning |
| --- | --- | --- |
| Raw URL | `<apple-music-url>` | Source + Play; shuffle/repeat omitted |
| Compact | `ss1:<options>;<apple-music-url>` | Source + Play with explicit shuffle/repeat overrides |
| Structured JSON | The v1 envelope | Full supported intent fields and optional metadata |

The compact grammar is case-sensitive:

```text
card    = "ss1:" options ";" url
options = shuffle | repeat | shuffle "," repeat | repeat "," shuffle
shuffle = "s=" ("0" | "1")
repeat  = "r=" ("0" | "1" | "a")
url     = supported Apple Music HTTPS URL
```

`s=1` means explicit shuffle true; `s=0` means explicit false. `r=0`, `r=1`, and `r=a` mean repeat off, one, and all respectively. Absence means Default/omitted; there is no encoded Default value. One or two distinct options are required, with at most seven option bytes. Canonical output orders `s` before `r`. Prefix/version, keys, values, commas, and the first semicolon must match exactly. Whitespace (including surrounding ASCII whitespace), duplicate/unknown keys, empty options, missing URLs, escaping/expression syntax in options, and payloads exceeding 4,096 UTF-8 bytes reject. The entire remainder after the first semicolon is the URL, normalized by the existing Apple Music normalizer; tracking parameters/fragments follow its ordinary rules. The compact parser is explicit and does not use JSON.

Raw and compact cards always construct explicit Play. Service is Apple Music and source kind derives from the URL; neither is stored separately. No target, room, group, policy result, or device setting is encoded. The [shared source/mode matrix](#source-specific-mode-validity) applies after decoding, including rejection of track shuffle/all, album/playlist repeat-one, and station overrides. No NFC-specific capability rules replace it.

The writer automatically chooses the smallest lossless canonical payload: raw URL for source + Play with omitted modes; `ss1` for explicit supported modes; structured JSON when hidden fields such as volume or retained label/extension metadata require representation. Generic omitted/non-Play transport, seek, queueIndex, and no-source intents also require JSON when serialized outside the simple authoring workflow. Capacity rejection never discards fields, truncates URLs, or introduces shortening. Reading a supported card does not rewrite it; a deliberate edit uses the same automatic encoding choice.

## Versioning, optional data, and NFC encoding

The envelope has required `format`, `version`, and `intent`, and optional `label`, `extensions`, and `requires`. `label` is a display-only string. V1 readers accept only integer version `1` and the exact format marker. Unknown versions are inspectable as raw text but MUST NOT execute or be rewritten as v1 implicitly.

Unknown keys outside the envelope's `extensions` object MUST be rejected, including unknown intent fields. This catches mistakes such as `volum` instead of silently ignoring a requested action. `extensions` maps namespaced names to JSON values; unknown extensions are ignored for execution and surfaced in the editor. `requires` is a duplicate-free array of extension names present in `extensions`; unsupported required extensions reject the whole document before any effect. No extensions are defined by v1. Writers MUST mark an extension required when ignoring it would change essential behavior. Editors MUST retain unknown extension values and `requires` when saving; if unable to round-trip them, they must refuse rewriting rather than silently drop them.

Structured v1 cards MUST contain one NDEF Text record (well-known type `T`), UTF-8, language `en`, whose text is the JSON document. No BOM is written. Whitespace and object key order have no semantic meaning; compact JSON is preferred for capacity. V1 execution accepts exactly one NDEF Text record, with any valid language tag and UTF-8 text. URL-only cards also accept one well-known NDEF URI record (`U`): prefix `0x00` plus the full URL, or `0x04` plus the URL after `https://`. The expanded URI MUST pass the same Apple Music URL validation and 4,096-byte limit; URI records cannot carry compact or JSON intents. Other prefixes, multiple records, UTF-16, and nested Smart Posters are unsupported and must be reported.

The supported household empty-type URL encoding is one record with TNF `0x01`, an empty type, and a raw UTF-8 Apple Music URL as its entire payload (no Text language/status header or URI prefix byte). Apply the same URL validation and 4,096-byte limit. This exception accepts only a URL, never JSON or commands, and does not apply to other TNFs/types or malformed `T`/`U` records. Writers emit raw URLs as URI records with prefix `0x04`, and `ss1`/structured JSON as UTF-8/en Text records. Compact cards are not accepted in empty-type or URI records.

URL-only, compact `ss1`, and structured v1 cards are supported current input formats. A trimmed Text-record payload consisting solely of an accepted Apple Music URL normalizes to `source` plus explicit `transport: "play"`, as do URI and empty-type URL records. All other settings remain omitted and policies apply normally. Structured v1 cards declare their intent fields directly; a source-only intent preserves transport. Do not guess at multi-line commands or silently overwrite cards. Malformed JSON does not fall back to a script.

Readers MUST bound payload size and JSON nesting/element counts before execution. Start with 4,096 UTF-8 bytes, eight nested containers, and 128 total members/items; these are tunable implementation limits, not wire-version changes. Expose limits to the local editor. The writer MUST also check actual tag capacity, including NDEF overhead, before writing. No truncation is permitted. Verify tag/NDEF and ST25R3916 driver read/write support early; no 4 KB card capacity is assumed.

## Simple family writer contract

The Stick hosts a [local writer page](tag-writer.md). The simple family-facing writer exposes **only Apple Music source, shuffle, and repeat**, adapting controls to the normalized source kind:

| Source              | Shuffle choices    | Repeat choices      |
| ------------------- | ------------------ | ------------------- |
| Album               | Default / On / Off | Default / Off / All |
| Playlist            | Default / On / Off | Default / Off / All |
| Track / single song | Hidden             | Default / Off / One |
| Station             | Hidden             | Hidden              |

`Default` omits the field from the card and lets room overrides, device policy, then code-owned source defaults resolve it later. Do not label it “Preserve”: a new album, playlist, or track normally derives repeat=off, and an album derives shuffle=false. Explicit On/Off/All/One choices serialize their exact values.

Every card created by this simple writer normalizes to **`transport: "play"`** with its source. Raw URL and compact encodings imply it; JSON stores it explicitly. Transport is fixed, not presented as an option. Source-and-stay-paused, play/pause-only commands, and queue manipulation are outside this family workflow.

Volume remains a supported power-user MusicIntent/card-schema capability, using the normal logical 0–100 range. Manual JSON cards or advanced tools may use it. The simple writer exposes no volume, seek, queueIndex, room, roomDisplayId, UUID, policy, device configuration, or read_only controls. Unsupported advanced cards must remain inspectable without silently rewriting/dropping their extra fields.

Examples using the illustrative room configuration in [policy](policy.md):

- One playlist card with source + play and omitted modes shuffles in Living Room, preserves shuffle in Office, and derives repeat=off in both.
- One album card with source + play and omitted modes plays in order in Office, shuffles in Sons Room, and derives repeat=off in both.
- One track card with source + play and omitted repeat loops in Sons Room and plays once in Office. The room policy, not the card, supplies that difference.

Room-independent cards are a central product requirement. Any effective policy preview is explanatory; it must not silently save the previewed values on the tag.

## Writer workflow

1. Paste an Apple Music URL or arm Read / Edit. Raw URL, compact, and structured cards pass through the normal decoder/parser. Read/edit owns the presentation and never submits playback.
2. Validate the complete draft and arm one write for 60 seconds. Source, shuffle, and repeat update only explicit fields; transport becomes Play. Supported hidden fields, `label`, optional `extensions`, and empty `requires` are retained. A conflicting hidden field rejects through the shared validator. Unsupported versions/required extensions cannot be rewritten.
3. Reserve the next presentation before tag preparation. A new card must be blank; a nonempty card requires Read / Edit first. Editing pins the read UID and payload and refuses a different/changed card. Check the supported tag's writable status, layout, and capacity including complete NDEF/TLV overhead before the first write.
4. Select the smallest lossless encoding, write one NDEF record, reread through the normal NFC decoder and intent parser, and compare normalized semantics plus retained metadata across encodings. Omission differs from explicit false/off; JSON whitespace and key order do not matter. Only equivalent read-back reports success.
5. Consume arming on any presentation attempt, cancellation, timeout, or completion. The physical presentation latch survives terminal writer states, so normal reading resumes only after removal. Reboot returns to the normal reader with no retained writer state.

The active card operation has a 15-second deadline. Interrupted/cancelled writes can leave an incomplete card; retain the browser draft and require explicit rearming. No retry, tag locking, configuration change, or Sonos execution is part of writing. See [writer implementation boundaries](tag-writer.md) and [physical validation limits](hardware.md#nfc-reading-and-writing-limits).

## Representative intent fixtures

These objects are `intent` values inside the v1 envelope:

```json
{ "volume": { "set": 25 } }
```

Only volume changes, even if an album currently plays. No source policy applies.

```json
{ "volume": { "delta": -5 }, "transport": "pause" }
```

At volume 3 the target is 0; pause and volume may be independent plan branches.

```json
{
  "source": {
    "service": "apple-music",
    "url": "https://music.apple.com/us/playlist/family/pl.example"
  },
  "shuffle": false,
  "repeat": "all",
  "transport": "play",
  "volume": { "set": 25 }
}
```

Explicit shuffle `false` wins in every room. Without that field, the configured playlist room derives `true`; another room preserves observed shuffle.

Required rejection fixtures: both `set` and `delta`; `volume: null`; `shuffle: 0`; `source` + `next`; duplicate JSON keys; unknown intent key; unknown required extension; unsupported version; oversized NDEF; an artist URL. Required preservation fixtures: explicit `false`/zero, source-only while playing and while paused, album URL with `i` receiving track defaults rather than album defaults, and round-tripped unknown optional extensions.
