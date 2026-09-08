# Device and room source policy

## Device configuration

`rooms` is one object keyed by canonical `roomDisplayId`. Its keys are the device's entire room allowlist; each value contains only that room's policy exceptions. An empty room object allows that room without overriding device policy or code-owned source defaults. Optional top-level `policy` supplies device-wide overrides only for these configured rooms; it cannot enroll rooms or affect discovery/selection. Missing or empty `policy` means no device overrides. Both layers use the same typed source-policy schema.

```json
{
  "read_only": true,
  "policy": { "playlist": { "shuffle": true } },
  "rooms": {
    "office": {},
    "living-room": { "playlist": { "shuffle": false } },
    "bedroom": {}
  }
}
```

Office and Bedroom inherit playlist shuffle=true from device policy; Living Room overrides it to false. Settings are local to each controller; there is no automatic policy distribution or general rules language.

`read_only` is the sole Sonos mutation switch: true blocks all mutations at HTTP dispatch; false permits requested effects for configured, uniquely resolved, topology-eligible targets. Room configuration controls target availability, not mutation permission. Missing `read_only` defaults true; missing/empty `rooms` selects nothing even when `policy` is present. [Execution](planner-executor.md) owns dispatch/identity safeguards.

## Room identity and selection

The display ID derives from the current Sonos name: lowercase ASCII letters and digits; whitespace, hyphens, and underscores collapse to one hyphen; other ASCII punctuation is stripped; edge hyphens are trimmed. `Office` → `office`, `Living Room` → `living-room`, `Kids' Room` → `kids-room`. Non-ASCII names, unsupported control characters, empty results, and IDs over 64 characters are invalid. There is no transliteration; configured keys must already be canonical.

At topology refresh, each config key must match exactly one discovered room across the entire snapshot, including ineligible players. Missing, invalid, or ambiguous IDs fail resolution and report an error. Grouped, bonded, invisible, addressless, or otherwise ineligible targets fail as unavailable. Valid entries continue; discovered but unconfigured rooms never enter selection. Only selectable UUIDs receive resolved room policies. There is no separate policy map capable of enrolling another room. If no configured room resolves to an eligible target, playback is blocked.

Resolution is `config display ID → discovered room → UUID`. A rename intentionally breaks the old binding until configuration is edited. UUIDs are internal identities, never human config keys. Speaker addresses come from SSDP and live topology. A successfully discovered player address may be reused in memory to read fresh topology; it is never configured or persisted. Failed topology reads trigger SSDP again. If discovery fails, all room bindings are cleared and playback is blocked.

Selection sorts by case-insensitive human name then UUID. A room switch persists its display ID in `surface/preferred-id`. Boot restores it if eligible; otherwise warn and use the first valid configured room. The same fallback applies when the selection disappears. A recovered room rejoins without stealing selection. Accepted work never follows a fallback or a later room switch.

## Policy layers and valid overrides

There are four semantic layers:

- Code-owned source defaults define universal product behavior in shared code.
- Device-wide `policy` persists this controller's preferences across its allowed rooms.
- Room policy under `rooms` persists exceptions for a specific room.
- Explicit intent declares request-specific values and always wins.

`sourceDefaults(SourceKind)` in SurfaceCore defines product behavior. Defaults run only when the incoming intent contains a normalized source, and only fill omitted fields. They are not stored in NVS, serialized into `policy` or room entries, or written onto cards. No policy examines the currently playing Sonos media.

| Incoming source | Default shuffle | Default repeat | Allowed device/room override fields |
| --- | --- | --- | --- |
| Album | false | off | shuffle true/false; repeat off/all |
| Playlist | absent/preserve | off | shuffle true/false; repeat off/all |
| Track | unsupported; absent | off | repeat off/one |
| Station | unsupported; absent | unsupported; absent | None; `station` config key rejects |
| No source | absent/preserve | absent/preserve | None |

A new album plays in order for one pass. Playlists retain shuffle unless explicitly set or overridden by device/room policy, but clear prior repeat-one/all by default. A track plays once unless device/room policy or explicit intent requests repeat-one. These defaults prevent a previous source's repeat mode leaking into a new card's behavior. The [intent source/mode matrix](intent.md#source-specific-mode-validity) is also used to validate both device and room overrides; config cannot express extra source capabilities.

## Per-field precedence and provenance

For each field independently: **explicit intent → room policy → device policy → code-owned source default → absent/preserve**. Explicit false and repeat off are values, not omission. Policies never derive source, target, volume, transport, seek, or queue index.

With the configuration above:

| Request and room | Resolved shuffle / origin | Resolved repeat / origin |
| --- | --- | --- |
| Album, Office | false / `source-default:album` | off / `source-default:album` |
| Playlist, Office or Bedroom | true / `device-policy` | off / `source-default:playlist` |
| Playlist, Living Room | false / `room-policy:living-room` | off / `source-default:playlist` |
| Playlist, Living Room, explicit shuffle=true | true / `explicit` | off / `source-default:playlist` |
| Playlist, Office, explicit shuffle=false | false / `explicit` | off / `source-default:playlist` |
| Track, Office | absent / `preserve` | off / `source-default:track` |
| No-source repeat=one | absent / `preserve` | one / `explicit` |

Resolution fills fields independently: if device playlist policy sets shuffle=true and repeat=all, a room override of shuffle=false changes only shuffle; repeat remains all with `device-policy` origin. A mode-only intent without a source invokes neither policy layer nor source defaults, regardless of currently playing media.

`PolicyProvenance` is a small map of typed fields (Shuffle, Repeat) to typed origins (Preserved, Explicit, RoomPolicy, DevicePolicy, SourceDefault), with a display ID or source-kind key where applicable. Logs pair each origin with its resolved value:

```json
{
  "shuffle": { "value": true, "origin": "device-policy" },
  "repeat": { "value": "off", "origin": "source-default:playlist" }
}
```

Absent fields have JSON `null` and origin `preserve`, distinct from false/off. Accepted diagnostics include room/display ID, frozen UUID, policy revision, source kind, explicit fields, this per-field `policy` object, preserved fields, and the static operation plan. Preservation baselines belong to execution reads, not policy. USB `preview URL` or `preview {v1 envelope}` prints the same policy/plan without submitting work, preflight reads, or mutations, in either runtime mode.

## Configuration replacement and request acceptance

Configuration validation completes before NVS replacement; invalid updates retain the prior config. Successful replacement advances the persisted local revision and reboots. Busy updates reject. Acceptance validates input first and freezes target UUID, resolved values, provenance, and revision before handing work to the worker. Execution never reruns policy; later config or selection changes cannot alter it.

Config permits at most 32 rooms, 64 bytes per ID, eight JSON container levels, and 4,088 UTF-8 bytes. Wrong types, duplicate keys at any depth, unknown fields, unsupported source/mode policies, and excessive sizes reject atomically. Invalid ID strings within the size bound remain stored for actionable topology warnings. Device and room policy reject the same unknown source keys/fields, wrong types, station policies, and source/mode invariant violations. Empty source policy objects are allowed and equivalent to omitting that override. Diagnostic serialization retains exceptions only, never expands defaults.

If stored configuration is invalid, the runtime uses read-only/no-target defaults until a valid configuration is uploaded. Household replacement leaves the separate `surface/touch` calibration intact. See [README](../README.md#flash-and-configure) for configuration upload and [intent](intent.md#simple-family-writer-contract-future) for future writer controls and omission labels.
