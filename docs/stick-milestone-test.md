# Stick room and policy manual checkpoint

## Preparation

Keep the Stick on the default read-only image for steps 1–4. Open a monitor:

```sh
.deps/venv/bin/python scripts/device.py monitor stick --port BOARD_PORT
```

Wait for an idle heartbeat between actions. A single-click refreshes, an A
double-click cycles eligible rooms, and B pauses. USB `room-next` exercises the
same room-selection path. A busy gesture/request is rejected; repeat it after
completion. The display's first line is the selected room. Do not interpret a
read-only rejection as a playback success.

`playlist_shuffle_rooms` in `surface/config` maps stable room UUIDs to playlist
shuffle defaults. The test Stick currently has `{ "LIVING_ROOM_UUID": true }`
using its actual discovered UUID. Add more UUID/boolean entries to configure
more rooms. Remove an entry to preserve shuffle there; `false` derives false.
To change rules, edit `.local/config.json` and upload with `scripts/configure.py`;
after the firmware upgrade that adds map support, policy edits need no build or
flash. An empty object disables playlist rules. Legacy singular string configs
still load as a one-entry true map. Do not put room IDs on cards.

## Read-only physical test

1. Double-click A: Office → Living Room. Continue double-clicking to cycle the
   remaining eligible rooms and return to Office. Verify the first line and
   observed playback/title match each room in the Sonos app. An empty title on
   a stopped room with no selected content is valid. Confirm no playback changes.
2. In Office and Living Room, present an existing album card, hold, remove, and
   present again. Each presentation should submit once. Serial must show
   `sourceKind=album` (JSON formatting), shuffle input absent, resolved false,
   `shuffleOrigin=albums-in-order`, with volume and repeat listed as preserved.
3. Present the same playlist card in both rooms. Office must show shuffle
   preserved; Living Room must show true with `playlist-room-shuffle`. The
   explicit payload is identical; only accepted target/policy context changes.
4. Use USB v1 JSON for explicit settings without rewriting cards. Paste a
   known Apple URL into the source example below. Explicit false must remain
   false in both rooms with origin explicit. Also test explicit true on an
   album; it must beat the household false policy. Reboot after selecting
   Living Room, confirm its UUID is restored, then cycle back to Office.

```json
{"format":"sonos-surface","version":1,"intent":{"source":{"service":"apple-music","url":"https://music.apple.com/us/playlist/example/pl.example"},"transport":"play","shuffle":false}}
```

The illustrative URL above is for read-only parsing/policy diagnostics only;
replace it with known playable content before enabling mutations. An album URL
with `?i=TRACK_ID` must normalize as track and receive no album shuffle rule.

## Authorized playback test

Office retains its existing authorization. **Every other room requires explicit
owner authorization naming its stable UUID before playback, mode, or volume
testing.** Discovery, policy configuration, and selection do not grant it.
The present playback image enforces an explicitly built UUID and the name Office;
keep other rooms read-only until that boundary is deliberately updated following
authorization. Start Office playback testing at a comfortable, owner-chosen volume.

```sh
python3 scripts/device.py build stick --allow-sonos-mutations --mutation-target VERIFIED_OFFICE_UUID
.deps/venv/bin/python scripts/device.py flash stick --allow-sonos-mutations --mutation-target VERIFIED_OFFICE_UUID --port BOARD_PORT
```

1. In an authorized room, record its volume and repeat, then tap album and
   playlist cards. Verify policy mode results and that omitted volume/repeat
   remain unchanged. Retest after setting repeat=all, then repeat=one.
2. Send repeat-only JSON and verify shuffle is preserved; send shuffle-only
   JSON and verify repeat is preserved. Send volume-only JSON and verify source,
   queue, transport, and mode are untouched. Delta zero must issue no SetVolume.
3. Send `next`, then `previous`, observing the native track behavior and exactly
   one SOAP attempt per command. No automatic retry is expected after uncertainty.
   Send a small volume delta; the log must show the fresh baseline and one frozen
   absolute level. Set an absolute level and confirm it is observed.
4. Test a v1 source with transport omitted while playing, then while paused.
   The new source should retain that playing/non-playing category. Source + pause
   must stay non-playing throughout. Legacy URL cards still explicitly play.

```json
{"format":"sonos-surface","version":1,"intent":{"repeat":"all"}}
{"format":"sonos-surface","version":1,"intent":{"shuffle":false}}
{"format":"sonos-surface","version":1,"intent":{"volume":{"delta":-2}}}
{"format":"sonos-surface","version":1,"intent":{"volume":{"set":20}}}
```

Record serial output plus what the display and speaker actually did. Grouping
invalidation, rename/IP change, initial AP absence, and reconnect remain separate
physical tests; host fixtures cover their target-selection contracts. Do not have
the agent create/dissolve groups to test them. Seeking and queue browsing remain
outside this checkpoint.
