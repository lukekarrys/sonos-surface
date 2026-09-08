# MusicIntent and NFC card specification

## Contract

The shared core implements source, play/pause/next/previous, absolute/relative volume, shuffle, repeat, absolute seek, and existing active-queue item selection through shared planning and Sonos boundaries. Source-only and source + pause capture and preserve playing/non-playing state. Writer mode and required extensions remain unsupported. The [capability contract](sonos-capabilities.md) defines timing and queue observations. See [hardware evidence](hardware.md) for physical validation and limitations.

A MusicIntent declares requested state and, optionally, one transport command. It is not a sequence of Sonos calls. NFC, touch/buttons, and resolved voice MUST normalize into this same model. Target, request identity, and policy provenance belong to the execution envelope, not the portable intent. The JSON/card contract is deliberately defined here; Sonos mapping and NFC driver/tag support require empirical validation, not assumptions in the format.

V1 serialized documents use UTF-8 JSON:

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
- A normal “play this” card MUST include `transport: "play"`. The simple writer always writes this explicitly; transport is not an option in that UI or a parser default.
- Omitted shuffle/repeat preserve their pre-mutation values unless policy fills them. When Sonos exposes one combined play mode, the planner must read and carry forward the omitted component. Source-change side effects may require restoring preserved mode or mute; do not otherwise reassert omitted volume.
- `next`/`previous` are imperative exceptions: exactly one native skip attempt with the speaker's native end-of-queue behavior. It MUST NOT be combined with `source` or repeat/shuffle changes in v1; combining it with volume is allowed. Do not invent a replay fallback at the queue boundary. Its exact paused-state behavior is a real-device capability to validate, not an implicit `play` request.

## Source-specific mode validity

Shared intent validation applies this matrix whenever the incoming intent contains its own normalized source. Every input surface, including direct typed touch intents and future writers/voice, receives the same validation before policy or Sonos mutation. Omission is valid for every field; [policy](policy.md) determines whether it derives a value or preserves state.

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

Normalization retains storefront, semantic query `i`, and catalog ID; it ignores fragments and nonsemantic tracking parameters. It produces a canonical source plus `kind: album | playlist | track | station`, `catalogId`, and `storefront` for policy and transport. These derived identity fields are not accepted as competing wire fields. Only `source.service` and `source.url` are serialized.

V1 does not follow short links or arbitrary redirects. Parsing a URL does not prove playback access. Transport preflight must resolve necessary Sonos URI/metadata and reject known unsupported content before clearing the queue. Sonos account/service registration and metadata generation are adapter concerns, not card content. Preserve the public URL for portability and editing. Personal station access depends on the Apple Music account registered with Sonos; a syntactically valid station URL does not establish account access.

## Versioning, optional data, and NFC encoding

The envelope has required `format`, `version`, and `intent`, and optional `label`, `extensions`, and `requires`. `label` is a display-only string. V1 readers accept only integer version `1` and the exact format marker. Unknown versions are inspectable as raw text but MUST NOT execute or be rewritten as v1 implicitly.

Unknown keys outside the envelope's `extensions` object MUST be rejected, including unknown intent fields. This catches mistakes such as `volum` instead of silently ignoring a requested action. `extensions` maps namespaced names to JSON values; unknown extensions are ignored for execution and surfaced in the editor. `requires` is a duplicate-free array of extension names present in `extensions`; unsupported required extensions reject the whole document before any effect. No extensions are defined by v1. Future writers MUST mark an extension required when ignoring it would change essential behavior. Editors MUST retain unknown extension values and `requires` when saving; if unable to round-trip them, they must refuse rewriting rather than silently drop them.

Structured v1 cards MUST contain one NDEF Text record (well-known type `T`), UTF-8, language `en`, whose text is the JSON document. No BOM is written. Whitespace and object key order have no semantic meaning; compact JSON is preferred for capacity. V1 execution accepts exactly one NDEF Text record, with any valid language tag and UTF-8 text. URL-only cards also accept one well-known NDEF URI record (`U`): prefix `0x00` plus the full URL, or `0x04` plus the URL after `https://`. The expanded URI MUST pass the same Apple Music URL validation and 4,096-byte limit; URI records cannot carry JSON intents. Other prefixes, multiple records, UTF-16, and nested Smart Posters are unsupported and must be reported.

The supported household empty-type URL encoding is one record with TNF `0x01`, an empty type, and a raw UTF-8 Apple Music URL as its entire payload (no Text language/status header or URI prefix byte). Apply the same URL validation and 4,096-byte limit. This exception accepts only a URL, never JSON or commands, and does not apply to other TNFs/types or malformed `T`/`U` records. Writers MUST emit v1 JSON Text records.

URL-only and structured v1 cards are both supported current input formats. A trimmed Text-record payload consisting solely of an accepted Apple Music URL normalizes to `source` plus explicit `transport: "play"`, as do URI and empty-type URL records. All other settings remain omitted and policies apply normally. Structured v1 cards declare their intent fields directly; a source-only intent preserves transport. Do not guess at multi-line commands or silently overwrite cards. Malformed JSON does not fall back to a script.

Readers MUST bound payload size and JSON nesting/element counts before execution. Start with 4,096 UTF-8 bytes, eight nested containers, and 128 total members/items; these are tunable implementation limits, not wire-version changes. Expose limits to the local editor. The writer MUST also check actual tag capacity, including NDEF overhead, before writing. No truncation is permitted. Verify tag/NDEF and ST25R3916 driver read/write support early; no 4 KB card capacity is assumed.

## Simple family writer contract (future)

Writing and its web server are not implemented yet. The simple family-facing writer exposes **only Apple Music source, shuffle, and repeat**, adapting controls to the normalized source kind:

| Source              | Shuffle choices    | Repeat choices      |
| ------------------- | ------------------ | ------------------- |
| Album               | Default / On / Off | Default / Off / All |
| Playlist            | Default / On / Off | Default / Off / All |
| Track / single song | Hidden             | Default / Off / One |
| Station             | Hidden             | Hidden              |

`Default` omits the field from the card and lets selected-room overrides plus code-owned source defaults resolve it later. Do not label it “Preserve”: a new album, playlist, or track normally derives repeat=off, and an album derives shuffle=false. Explicit On/Off/All/One choices serialize their exact values.

Every card created by this simple writer includes **`transport: "play"`** with its source. Transport is fixed, not presented as an option. Source-and-stay-paused, play/pause-only commands, and queue manipulation are outside this family workflow.

Volume remains a supported power-user MusicIntent/card-schema capability, using the normal logical 0–100 range. Manual JSON cards or advanced tools may use it. The simple writer exposes no volume, seek, queueIndex, room, roomDisplayId, UUID, policy, device configuration, or read_only controls. Unsupported advanced cards must remain inspectable without silently rewriting/dropping their extra fields.

Examples using the illustrative room configuration in [policy](policy.md):

- One playlist card with source + play and omitted modes shuffles in Living Room, preserves shuffle in Office, and derives repeat=off in both.
- One album card with source + play and omitted modes plays in order in Office, shuffles in Sons Room, and derives repeat=off in both.
- One track card with source + play and omitted repeat loops in Sons Room and plays once in Office. The room policy, not the card, supplies that difference.

Room-independent cards are a central product requirement. Any effective policy preview is explanatory; it must not silently save the previewed values on the tag.

## Writer workflow (future)

1. Read/inspect or create a draft. Show explicit settings separately from an effective policy preview for the selected target; do not save derived values onto the card unless the user explicitly chooses them.
2. Validate the complete draft, show its source and playback intent, and arm one write for 60 seconds. Arming or editing MUST NOT submit playback. An edit session pins the read tag identity and payload; reject a different/changed tag.
3. While armed, the next compatible tag presentation is reserved for writing, with normal playback disabled. Verify writability/capacity. Require explicit overwrite confirmation if an unrelated nonempty tag is presented.
4. Write, reread, and compare the intended text bytes. Only verified readback reports success. Consume the arm after an attempt, cancellation, or expiry; require rearming for retries. Normal reading resumes only after tag removal.

Read/inspect mode also suppresses playback. Interrupted writes may corrupt a card; retain the draft and show recovery/retry instructions. Never lock a tag read-only automatically. UID checks prevent accidental edits, not authenticate cards. Unsupported versions/formats remain inspectable without executing.

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
