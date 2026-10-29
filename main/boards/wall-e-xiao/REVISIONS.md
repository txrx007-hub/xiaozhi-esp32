# Revisions

One number for the whole project. The **revision** goes up by one with every firmware build that is archived (`walle-backup\builds\<n>-<label>`), and every
git repository of the project carries a tag `rev-<n>` at the commit that belongs to it. The number is built into the firmware (`walle_revision.h`,
`WALLE_REVISION`), reported as `revision` in `/api/status`, shown bottom-left as **Rev <n>** next to the app version ("App v2.37 - Rev 90") and at the bottom of the landing page, and printed in the boot log
(`Jarvis is ready (revision 90)`).

The app and the backend keep their own version names (app `versionName`, backend snapshot folders); this table says which of them went with which revision.

| Revision | Date | What | App | Landing page / backend |
|---|---|---|---|---|
| 90 | 2026-10-29 | Music and radio stutter less: the robot keeps 3 s of incoming audio (was 1.2 s) and counts gaps in a stream and dropped packets (`audio_underruns`, `audio_worst_gap_ms`, `audio_dropped` in `/api/status`); includes the Wi-Fi bars of 89 | 2.37 | backend: lead of 15 packets for music and radio, radio buffer, `Audio:` line in the daily summary |
| 89 | 2026-10-29 | The Wi-Fi icon is five signal bars like a phone's (drawn, see-through, white on dark grey) instead of the font's fan; the level follows the signal strength (-55 / -65 / -72 / -80 dBm); same icon on the clock | 2.37 | backend: Smart turn counters restart every 24 h, face card shows how sure the last look was |
| 88 | 2026-10-28 | Talking movements follow the sound: they start about 0.15 s after the first sound reaches the speaker (not when the "Speaking" icon appears, which is before the first word) and stand still in the pauses between sentences; checked every 100 ms instead of once a second; new setting `talk_alternate` (default on): the sway swings left wheel, right wheel, left wheel, ... instead of always turning on the spot | 2.37 | landing page + app: new setting label |
| 87 | 2026-10-28 | "Look at me": the first picture is always fresh (the camera queue held an old frame after a pause: no face in the first picture, so it started searching while you sat in front of it); at most four pictures, each inspected first, then turn, then the next picture; the server also keeps rare "no face" pictures to see why | 2.36 | backend: `_noface` pictures |
| 86 | 2026-10-28 | After-turn looks take three camera frames (the first one was stale and blurred, "turned, but cannot see your face"); steadier turning-speed learning; revision numbering | 2.36 | tags `rev-86` |
| 85 | 2026-10-28 | The robot works out the face turn from a learned turning speed; "look at me" searches when no face is in view, four centring steps at most; `face_deadzone` | 2.36 | |
| 84 | 2026-10-28 | Faster follow looks (first frame, no screen preview) | 2.34 | |
| 83 | 2026-10-28 | HTTP client use-after-free fix (crash after 49 minutes of follow mode); `follow_ms` from 100 ms | 2.34 | |
| 82 | 2026-10-28 | Face turns swapped left/right by default; failed photos are retried and say why | 2.34 | |
| 81 | 2026-10-27 | Turn to face me, follow mode, talking movements; Smart turn on the server; memory fixes | 2.33 | |
| 80 and older | | see `CHANGELOG.md` (phases 1-10) | | |

## How to add one

1. Raise `WALLE_REVISION` in `walle_revision.h`, build, archive with `walle-backup\save-build.ps1 -Label "<n>-<what>"` (the archive number must equal the revision).
2. Add a row here and a section in `CHANGELOG.md`.
3. Commit in every repository that changed, then `git tag rev-<n>` in all four (firmware, `android`, `web`, backend) and push the tags with the branches.
