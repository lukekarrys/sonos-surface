# Shared capability physical checkpoint

Use M5StickS3 and USB diagnostics; leave Waveshare unchanged. The detailed
[contract](sonos-capabilities.md) and [hardware evidence](hardware.md) distinguish
software tests from actual speaker observations. Do not run later mutation steps
until the read-only observation checkpoint is confirmed.

## External observation with Stick read-only

Keep `config-status` reporting `read_only:true`. Select Office with `room-next`
and wait for its `metadata roomDisplayId=office` line. Keep a USB capture running.
Using the official Sonos app or another controller, deliberately make these changes
at a comfortable physical output level, waiting for an updated Stick snapshot
after each (normally about 10 seconds, longer during network failure):

1. Play, then pause. Position should advance while playing and settle when paused.
2. Select a different existing queue track. Title, album/artwork, index, and timing
   must change together. Select a different source/playlist if validating queue
   replacement; a new queue revision must invalidate the earlier page.
3. Change volume and shuffle/repeat, then read `status`. Values must match Sonos.
4. Select an Apple Music station externally. Queue index/total and duration must
   become unknown. `queue [0,2]` may still read the separate stored queue.

The Stick must send only GET/Get*/Browse during this checkpoint. Subsequent polls
must retain the new reality without replaying an old seek, selection, or source.
Sonos controls made outside this device are intentional human actions; the agent
must not generate them autonomously to manufacture external-change evidence.
A subset can establish a real external transition; software fixtures cover the
complete field matrix. Record which real transitions actually occurred.

## Small seek and selection test, only after the observation checkpoint

Restore active queue playback in Office with at least three ordinary tracks.
Pause it in Sonos and choose a track longer than 30 seconds. Check the selected
room, queue page, and comfortable output level. Office's Sonos Port uses fixed
line output; its reported 100 is not a safe physical amplifier level by itself.

Deliberately send `read-only false`, wait for reboot, and verify `config-status`.
Then issue each request once, waiting for its terminal result:

```json
{"format":"sonos-surface","version":1,"intent":{"seek":{"positionMs":10000}}}
```

Expect position near 10 seconds (2-second tolerance), the same track/source, and
continued non-playing transport. Volume/mute and modes should stay unchanged.

```json
{"format":"sonos-surface","version":1,"intent":{"queueIndex":2}}
```

Expect the third existing item, matching title/index, unchanged queue contents,
and continued non-playing transport. This uses direct track selection, not Next.
If either request reports uncertainty, inspect Sonos and stop; do not resend it.

For a minimal playing-state follow-up, deliberately resume using the existing
Sonos controls and repeat a seek to 20 seconds, then select a different existing
index. Confirm playback continues and subsequent polls advance naturally instead
of reasserting the seek. Finish by deliberately restoring `read-only true` and
matching the private config file. No additional mutation capability is introduced
by this milestone.
