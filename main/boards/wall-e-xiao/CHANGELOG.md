# Jarvis robot: change log

Everything that was done on the Jarvis robot (the Wall-E-shaped board `wall-e-xiao`) from **firmware build 1** (2026-09-29) up to **build 81** (2026-10-27).
Four parts belong together, and each phase below lists what changed in each of them:

| Part | Where it lives | First | Latest |
|---|---|---|---|
| **Firmware** (robot) | this repository, `main/boards/wall-e-xiao/`, archives in `walle-backup/builds/<n>-<label>` | build 1, 29 Sep | build 81, 27 Oct |
| **Landing page** (the robot's own web page, embedded in the firmware) | submodule `main/boards/wall-e-xiao/web` | 18 Oct | 27 Oct (with build 81) |
| **Android app** | submodule `android`, archives in `walle-backup/android-builds/<n>-v<x.y>-<label>` | v1.0, 18 Oct | v2.33, 27 Oct |
| **Backend** (own voice server, control panel, modules) | `xiaozhi-server/temp-test`, snapshots in `walle-backup/backend/<date>-<hash>` | 19 Oct | 27 Oct |

How to read it: newest phase at the bottom of the table of contents, but each phase is in time order. *Not flashed* means built and archived but not yet on the robot.
The robot runs build **79** today; builds 80 and 81 are waiting for a go-ahead to flash.

## Contents
1. [Starting point](#starting-point-28-sep)
2. [Phase 1 · Bring-up and crash hunting (builds 1-9)](#phase-1--bring-up-and-crash-hunting-builds-1-9-29-sep)
3. [Phase 2 · Memory, sockets and nap fixes (builds 10-15)](#phase-2--memory-sockets-and-nap-fixes-builds-10-15-29-30-sep)
4. [Phase 3 · Camera colour, Wi-Fi setup, visualizers (builds 16-27)](#phase-3--camera-colour-wi-fi-setup-visualizers-builds-16-27-3-4-oct)
5. [Phase 4 · Own voice server, landing page and app (builds 28-36)](#phase-4--own-voice-server-landing-page-and-app-builds-28-36-18-19-oct)
6. [Phase 5 · Instant commands, Home Assistant, music and radio (builds 37-52)](#phase-5--instant-commands-home-assistant-music-and-radio-builds-37-52-20-21-oct)
7. [Phase 6 · Camera, health numbers and mute (builds 53-61)](#phase-6--camera-health-numbers-and-mute-builds-53-61-22-23-oct)
8. [Phase 7 · Music screens, themes and language (builds 62-70)](#phase-7--music-screens-themes-and-language-builds-62-70-23-oct)
9. [Phase 8 · Boot screen and reliability (builds 71-77)](#phase-8--boot-screen-and-reliability-builds-71-77-23-26-oct)
10. [Phase 9 · Camera colour, alerts, mute icon (builds 78-80)](#phase-9--camera-colour-alerts-mute-icon-builds-78-80-26-oct)
11. [Phase 10 · Turn to face me, follow mode, talking movements, Smart turn (build 81)](#phase-10--turn-to-face-me-follow-mode-talking-movements-smart-turn-build-81-27-oct)
12. [Hardware limits that shaped the work](#hardware-limits-that-shaped-the-work)
13. [Parked and open items](#parked-and-open-items)
14. [Where everything is stored](#where-everything-is-stored)

---

## Starting point (28 Sep)
- **Fork** of the open-source `xiaozhi-esp32` firmware (upstream 2.5.0) with a new board, **`wall-e-xiao`**: Seeed XIAO ESP32-S3 Sense, ST7789 240x240 screen, MAX98357A amplifier, Mini L298N motor driver with two N20 gear motors, OV3660 camera. Hardware design by Huy Vector, built by the owner; only the pinout is used as reference, none of that project's code or assets.
- The assistant is called **Jarvis** (wake word and identity renamed throughout). All board code lives under `main/boards/wall-e-xiao/`; the few edits in shared code are marked `WALL-E` and listed in the board README.
- What the first commit already had: recoloured Otto eye GIFs (a colour per emotion, random tint on "surprised"), idle clock, level-bar ribbon, Winamp-style 32-band spectrum, voice-triggered screensaver; OV3660 photos and the `look_around` four-photo collage; motors with trim, invert and speed settings; voice tools for movement, faces, audio tuning, settings, standby and deep sleep, timers, weather (Open-Meteo) and diagnostics; a LAN settings page with live sliders generated from one shared settings list; a USB console.
- The on-device Home Assistant integration was removed the same day (the cloud service had its own): tool count 31 to 28.

## Phase 1 · Bring-up and crash hunting (builds 1-9, 29 Sep)
Random reboots were the first problem. Crash reports first, then the cause.

| Build | What changed |
|---|---|
| 1 | **Crash reports**: a 256 KB `coredump` partition keeps the registers, backtrace and stack of a crash across reboots (read with `idf.py coredump-info` and that build's own ELF). |
| 2 | Real fix for the above: `espcoredump` must be an unconditional requirement of `main` (a board-gated one was silently ignored by the minimal build); whole-heap capture tried and reverted (needs more flash than the partition has). |
| 3 | **Root cause of the random reboots**: the idle clock's labels could be created before the theme font had loaded, leaving a label with a null font that crashed on the next relayout (entering power save). Labels now always fall back to the compiled-in default font. |
| 4 | Static-font fix and a live ping on the settings page. |
| 5 | Photo screen effect removed: eyes no longer freeze on a random "surprised" colour while the camera captures. |
| 6 | Camera capture could hang forever: a timeout on the frame wait (`VIDIOC_S_DQBUF_TIMEOUT`). |
| 7 | No "listening" text on screen; camera tint correction split into U and V. |
| 8 | A nap request no longer lets the server say one more line after the screen went dark (abort speaking first); classic Winamp bars. |
| 9 | CPU load, RAM and PSRAM live on the settings page. |

## Phase 2 · Memory, sockets and nap fixes (builds 10-15, 29-30 Sep)
| Build | What changed |
|---|---|
| 10 | Camera tint retune (a software correction; replaced by the real fix in build 23). |
| 11 | **File-descriptor leak** in the MQTT client on reconnect (a socket was left open per dropped connection): short delay before the client is destroyed. |
| 12 | **`CONFIG_LWIP_MAX_SOCKETS=16`** (was 10): the settings page alone could take 7 of 10 sockets, so a weather check or photo upload failed with ENFILE. This was the main cause of "the page stops answering after 20-30 minutes". |
| 13 | R2-D2 style **wake chirp** after the wake word. |
| 14 | Nap cancelled itself because the robot heard its own "I'm napping" through the microphone (no echo cancellation): wake word ignored while a nap is pending. |
| 15 | Same guard extended to about 4 s after the nap has started. |

## Phase 3 · Camera colour, Wi-Fi setup, visualizers (builds 16-27, 3-4 Oct)
| Build | What changed |
|---|---|
| 16 | **Camera cold-start probe**: after a true power-up the OV3660 sometimes delivers no frames; 6 s after boot one restart if none arrived (never after a software restart, so it cannot loop). |
| 17 | Top speed and shutter sound defaulted (100% / on) and hidden from the page. |
| 18 | Voice command "reconfigure Wi-Fi" opens the Wi-Fi setup hotspot. |
| 19 | **Remote control**: drive pad on the settings page; the Wi-Fi setup hotspot closes itself (2 min idle, 10 min total). |
| 20 | Seven speaking visualizers (winamp, scope, mouth, radial, vu, orb, ...), listening-text removal, screensaver fix. |
| 21-22 | Camera diagnostics (temporary debug pages, register writes) that pinned the purple cast down. |
| 23 | **Camera white balance fixed**: the sensor's auto white balance never started; one write of `0x94` to ISP register `0x5183` wakes it and pictures are neutral. All diagnostics and the old software tint removed. |
| 24 | `rainbow` visualizer (the dim end of each bar raised from 50 to 80). |
| 25 | Settings page: 50 px strips, board power mode, collapsible theme group. |
| 26 | Board name and camera detection (`YES (OV3660)`) on the page. |
| 27 | Power card (Nap / Wake up, Deep sleep with hours and minutes), two-column info table, screen and quiet hours as a toggle group. |

## Phase 4 · Own voice server, landing page and app (builds 28-36, 18-19 Oct)
The robot stopped depending on the xiaozhi.me cloud: an **own backend** on the owner's PC, plus a landing page and an Android app.

**Firmware**
| Build | What changed |
|---|---|
| 28 | Landing page and Android app become git submodules; module overview in the READMEs; a build guard that refuses to build without the page. |
| 29 | **Voice server switch** (own server or cloud) through the saved OTA address: `POST /api/server`, USB `!server`. |
| 30 | QR pages (`self.display.show_qr`: settings page, backend, both), volume card, Open backend button. |
| 31 | Backend PIN shown on the landing page (only while the backend's skip-PIN option is on). |
| 32 | QR pages 10 s each; live check that the server answers. |
| 33 | Shorter tool descriptions (fewer tokens per request). |
| 34 | **Full power, no volume cap**: 100% is full volume; only night mode limits it. |
| 35 | Weather retry, quieter chirp. |
| 36 | R2-D2 chirp restored at half volume. |

**Landing page** (first release 18 Oct): fake robot for testing, API description; voice-server card, volume card, backend button with PIN, QR commands in the voice list; no volume cap any more.
**Android app**: **v1.0** public release (18 Oct), then **v2.0** (19 Oct; it had been labelled 3.0 while in development): volume card, voice-server card, updated voice commands.
**Backend** (19 Oct, steps 1-5):
- Own server with brains (Gemini, Groq, later Cerebras and Claude Haiku), prompt, **control panel** (pick ears, brain and voice; keys; start/stop/restart; tests), web search (Tavily) and a usage dashboard with quota warnings.
- **Piper voices** (local, free, fast), named console windows, Dutch voice defaults, Edge voices selectable next to Piper.
- **Fallback brain**: when the brain fails a second one answers, with Groq's smaller models as extra layers; memory with a fixed summariser brain; fewer tokens per request; robot clock follows the PC's time zone.
- Autostart at log-in, daily backup and restore, debug console with colour legend and click-to-filter.
- **Step 5**: instant commands, mood presets, explore macro, daily briefing, memory safeguards.

## Phase 5 · Instant commands, Home Assistant, music and radio (builds 37-52, 20-21 Oct)
**Firmware**
| Build | What changed |
|---|---|
| 37 | The landing page lists the **instant commands** (own colour: done by the own backend before the brain). |
| 38 | `reset_reason` in `/api/status` (why the robot last restarted). |
| 39 | **Camera button** on the page: `POST /api/photo` and a photo window. |
| 40 | Home Assistant commands listed, only while the backend reports Home Assistant works. |
| 41-42 | **Music and radio**: Stop button (`POST /api/stop`), music/radio command lists gated by the backend; podcasts from the owner's own list. |
| 43-44 | Next button on the Music card and card spacing. |
| 45-46 | **Music screen**: the eyes are replaced by a drawn headphones picture with classic Winamp bars while music or radio plays; the top status line stays blank. |
| 47 | Landing page: one confirm popup, long-press hints, vibration ticks. |
| 48 | Advanced / Jarvis settings buttons; "Music mode" as status while music plays. |
| 49 | **Wi-Fi packet counters** (`GET /api/net`, wraps the Wi-Fi interface) for live Tx / Rx numbers. |
| 50 | **Voice commands as coloured tiles**; a tap opens one group. |
| 51 | The page's Camera photo is handed to the backend (`dcim`). |
| 52 | "Come here" listed in the instant commands. |

**Landing page** (20-21 Oct): Camera button with photo window, Home Assistant and music/radio cards, podcast text, Next button, music-screen picture setting, confirm popup / hints / ticks, matching Advanced buttons (voice-server card removed), `/api/net` docs, tiles, Come here.
**Android app** v2.1-v2.20: instant commands in the list (v2.1), colour themes Classic / Blueberry / Succulent and an unfolded-Fold tablet layout (v2.2), backend page inside the app (v2.3), Camera button (v2.4), Home Assistant commands (v2.6), **Music card** with Stop / Next (v2.7), music and radio commands and podcasts text (v2.8-v2.10), music-screen setting (v2.11), vibration ticks, long-press hints, TalkBack names, home-network-only addresses, predictive back, themed icon (v2.12), music-menu flicker fix (v2.13), **panel sign-in fix** (the panel closes every connection; v2.14), version label (v2.15), matching top buttons (v2.16), one toggle button and the vanished-buttons fix (v2.17), command tiles (v2.18), photos to the backend (v2.19), Come here (v2.20).
**Backend**:
- **Instant commands** (zero tokens, work with no brain): stop, volume, time and date, timers, themes, brightness, drive and turn, dance, moods, explore macro, briefing, "come here"; English and Dutch.
- **Home Assistant** module (key slots, test card, action log, lights / switches / scenes / climate by voice with a confirm step, light colours, safety exclusions).
- **Music module**: voice-chosen clips from a folder (title on the robot's screen, Stop / Next), **radio and podcasts** (own station list with presets 1-12, "save this station as preset N"), https-to-http fallback; Next by source.
- **Telemetry**: reply speed per stage, robot health, services and PC load, phone-app download link; Tx/Rx tiles; ping per part; panel search bar; themes in the panel; failover counter; key checks that show OK or a red error.
- **Voice backup**: the Edge voice falls back to local Piper; robot answers questions about itself ("how is your Wi-Fi?"); photos are kept in `dcim/<day>/`.

## Phase 6 · Camera, health numbers and mute (builds 53-61, 22-23 Oct)
| Build | What changed |
|---|---|
| 53 | Photo preview centred and fitted to the screen for 3.5 s; new setting **`camera_brightness`** (exposure target -5..5); upload quality 90. |
| 54 | **Live view** next to the drive pad; photos taken asynchronously so the page stays responsive. |
| 55 | **Chip temperature**, microphone peak and clipping, Wi-Fi disconnect counter and reason in `/api/status`. |
| 56-59 | Tx / Rx numbers cleaned up (rounding, rates only), chip-temperature row placement. |
| 57 | The microphone start-up click is ignored (it counted as clipping). |
| 60 | Server self-check uses the real port. |
| 61 | **Mute button** (microphone off until unmuted) on the page. |

**Landing page / app**: live view, Camera settings group, shopping list and notes/macros commands (app v2.21-v2.23, plus a **demo build "Jarvis Demo Mel"** that pretends to be a robot); chip temperature and Tx / Rx rows (v2.24-v2.26); pull-down refresh (v2.25); Mute / Unmute button (v2.27).
**Backend** (22-23 Oct): the **camera question race** (the first usable vision answer wins, a second module backs up a slow one, spoken apology instead of the robot's error), follow-up questions about the last photo, **voice macros**, shopping list, notes search, Google Calendar helper, **watchdog and phone alerts**, **conversation telemetry** (wake-ups, false wakes, dropped conversations, fallback counts), robot health graphs, Pim and Ronnie voice profiles, RAGFlow prepared (not switched on), Gemini brain `gemini-3-flash-preview`, brain badge, key boxes that reveal the saved key on click.

## Phase 7 · Music screens, themes and language (builds 62-70, 23 Oct)
| Build | What changed |
|---|---|
| 62-66 | Music-screen pictures: bigger icons, top bar stays visible, **music icons 1-4** in a list, direct-write drawing for speed. |
| 67 | Landing page colour themes (Blueberry, Succulent). |
| 68 | **Brain row** on the page (which brain answered last, the backup in amber). |
| 69-70 | **Peach theme** (sage-green dark base, blush-pink accent) and an **English / Dutch switch**. |

App v2.28 (Peach theme, English / Dutch). The panel got the same theme and language switch.

## Phase 8 · Boot screen and reliability (builds 71-77, 23-26 Oct)
| Build | What changed |
|---|---|
| 71-73 | **Boot screen**: `boot.gif` (a pulsing eye, not in git) plays full screen from power-on until Jarvis is ready; built into the app; no white flash at power-on. To remove it, flash build 70. |
| 74 | **Server retry and Wi-Fi rejoin**: a wake word that ends in "Unable to connect" is retried 5 s later, twice; then Wi-Fi is dropped and joined again, followed by one more try. |
| 75 | **NVS key fix**: long setting names (over 15 characters) crashed saving; they now get short stored names. |
| 76 | **Camera restart in place**: the cold-start recovery restarts the camera inside the running firmware instead of rebooting. |
| 77 | Boot GIF starts dark. |

## Phase 9 · Camera colour, alerts, mute icon (builds 78-80, 26 Oct)
| Build | What changed |
|---|---|
| 78 | **`camera_saturation`** (-4..4, written to the OV3660 colour matrix); the app and page get a colour slider in the Camera settings group (app v2.29-v2.32: full-screen photo with pinch zoom, brightness and colour sliders, sliders only under Camera settings). |
| 79 | **Mute icon**: a small red crossed-out microphone blinks slowly at the top left of the eyes and the clock, level with the green dot. |
| 80 | The Wi-Fi icon also on the **clock screen**, mute next to it. *Not flashed.* |

**Backend**: **phone alerts** through ntfy (8 kinds, QR, outbox, at most one key alert per hour per brain), Piper and Whisper watchdogs, **stale-key detection** (a key changed after the server started is not used until a restart: the panel now says so instead of reporting a rejected key), "settings changed after the server started" alert; the phone message size limit is 4,096 bytes.

## Phase 10 · Turn to face me, follow mode, talking movements, Smart turn (build 81, 27 Oct)
*Built, archived, tested offline; not flashed. Server side is installed on the PC but the voice server has not been restarted yet.*

**Firmware (build 81, new module `walle_track.cc`)**
- **Turn to face me** (`self.robot.face_me`, "look at me"): one picture goes to the own server, which answers where the biggest face is. The wheels turn in place, at `face_power` (full power by default); only the duration changes with the offset: small / medium / big correction (settings `face_short_ms` 300, `face_mid_ms` 500, `face_long_ms` 1000), looked at again up to three times. A turn against the previous one is 40% shorter (it went too far). `face_invert` swaps left and right (default on since build 82: the camera picture is mirrored against the wheels). **`face_power`** (30-100%, default 100) is a slider for the power of these turns and of follow mode; it is read at every turn, so it can be changed live while Jarvis tracks you.
- **Follow mode** (`follow_mode`, off after every restart): one look every `follow_ms` (1000) while Jarvis is idle; never during a conversation, a nap, an alarm or while the wheels are busy; looks less often after five empty looks.
- **Talking movements** (`talk_motion`, `talk_power` 80): while Jarvis speaks, every 2-4.5 s a quick left-right shake in place at 80% power, matched to the current emotion (lively for happy, slow and small for sad, a glance for thinking). Never during music or radio, a nap, a photo or a follow look.
- `/api/power` gets `follow_on`, `follow_off` and `face`; `/api/status` reports `follow`. New settings group "Face and movement". Tools: 28 to 29.

**Landing page**: a **Follow me** card with an on/off toggle and a **Face me** button; the new settings group; voice commands (English and Dutch).
**Android app v2.33**: the same **Follow me** card (toggle and Face me), new settings group, voice commands, demo build updated.
**Backend**:
- **Voice commands** (instant): "look at me", "face me", "turn to me", "kijk naar mij"; "follow me" / "stop following me", "volg me" / "stop met volgen"; "don't move while you talk" / "move when you talk".
- **Face finding** (block 28): `POST /mcp/vision/face` finds the biggest face with OpenCV on the PC (about 30 ms); local network only. On the robot's own photos the `alt2` cascade found only real faces. **Face pictures are kept** like the robot's other photos, in `dcim/<day>/<time>_face.jpg`, with the detection drawn on them (green box round the biggest face, thinner boxes round others, a blue centre line, the measured offset as text): every look where Jarvis has to turn, the first centred look after a turn, otherwise at most one per 20 s; at most 1500 a day; looks without a face are not kept.
- **Smart turn** (block 29): a small model (smart-turn-v3.2, 8.7 MB, BSD-2-Clause) tells a pause from a finished sentence and keeps an unfinished one open for up to 1.5 s. **Panel card with an on/off switch and counters**: times it stepped in, of those you carried on talking, pauses checked, average time per check, reset. Measured: about 80 ms per check; about 5% of the turns in the logs ended on an unfinished word (8% after the pause was shortened to 125 ms). Dutch panel strings added.
- Docs: `docs/face-and-smart-turn.md`.
- **Memory fixes**: (1) notes forgotten in the panel came back, because the memory module is one shared object that kept the notes in RAM and wrote them back after the next conversation; it now reads `data/.memory.yaml` again at every save and every question. (2) A leftover Chinese note ("likes listening to stories", from the stock Chinese prompt) could never go away, because the guard compared with the old notes too; stray Chinese notes are now dropped one by one unless Chinese was really spoken. Both checked with a test of the module.

---

## Phase 11 · First real test of face and follow (builds 82-88, 28 Oct)
- **Robot turned away from the face**: the saved face pictures showed the offset growing after a turn (x 0.35 to 0.81). The camera picture is mirrored against the wheels, so `face_invert` is now **on by default**; with it, one turn took x from 0.35 to 0.01 and it stayed centred. (Applied live on the robot before the flash.)
- **Photo error "the camera did not deliver a picture"** (seen after about 10 hours of running; no log of that moment, the robot was off USB): `CaptureJpeg` now encodes the frame in place instead of making a second 600 KB copy in PSRAM, tries a second time after 300 ms, logs the memory figures when it fails, and the web call and "look at me" answer with the reason. Not reproduced on a fresh robot (photos and looks work); the next failure will say why.
- Landing page mock, API notes, backend notes and the app (v2.34, demo robot) follow the new default.
- **Build 83, crash after 49 minutes of follow mode** (watchdog reset): use after free in the ml307 HTTP client; the server closes the connection right after its answer and the client was freed while its network task still called back into it. `walle_track::Look()` now keeps each client alive until the next look. Details: `walle-backup/incidents/2026-10-28-follow-mode-crash.md`. `follow_ms` minimum is now 100 ms.
- **Build 84, faster follow looks**: `CaptureJpeg(..., fast)` takes the first camera frame instead of the third and makes no screen preview (used by follow/face looks and the live view of the page). A look without a turn now takes about 0.3 s; measured 26 looks in 20 s with a moving face (turns add about 1 s each), against 19 before.
- **Build 85, the robot works out the turn itself**: the fixed durations (`face_short_ms`, `face_mid_ms`, `face_long_ms`) and the idea of a power slider for small corrections are gone. Duration = offset / learned turning speed (measured from the picture shift after every turn, EMA, seeded 0.0007 per ms at 100%), aimed 15% short; power `face_power` for big turns, 80% of it (never lower) for corrections under 30%. The only new slider is `face_deadzone` (5-40%, default 18; the margin where it does not turn at all). "Look at me" now takes at most **four** centring steps and, when no face is in view, **searches** first (up to six turns of about half the field of view, towards the side where a face was last seen), because the button used to answer "I cannot see a face" whenever you were outside the camera's view. App v2.36 and the landing page lost the removed sliders.
- **Build 87 (revision 87), "look at me" starts from a fresh picture**: pressing Face me while sitting in front of the robot made it turn away at random. The log showed no face in the first picture, then a search. Cause: the camera queue holds the frames of the last time it was used, and a one-frame look after a pause used an old one (the view from the end of the previous search). Every look after a turn or after more than 1.5 s without a look now takes all three frames, and the one-shot starts with a fresh picture. The routine is now **at most four pictures**: each is inspected first, then the robot decides where and how far to turn (towards the face, or on the spot when there is none in it), then the next picture. The backend keeps a picture of a look without a face too (`<time>_noface.jpg`, at most one every 20 s, 300 a day) to show why it missed.
- **Build 86 (revision 86), looks after a turn see the turn**: the log of the first adaptive test showed "I turned, but now I cannot see your face" and turns that moved the picture only 30-50% of the expected distance. The camera queue holds frames from before the turn, and the 1-frame fast look of build 84 used the first of them (old view or blur). After any turn the look now takes all three frames (no screen preview); between turns of the follow mode one frame is still enough. Learning of the turning speed now ignores samples further than 50-60% from the current value and averages more slowly (a person walking about while the robot learns skewed it). **Revision numbering** added: `walle_revision.h` (`WALLE_REVISION` 86), `revision` in `/api/status`, a small "Rev <n>" label bottom-left next to the app version ("App v2.36 - Rev 87") and at the bottom of the landing page (it is not in the Info table), `REVISIONS.md`, a `REVISION` file in the backend, git tags `rev-<n>` in all four repositories.
- **Build 88, talking movements follow the sound**: the "Speaking" state (and its icon) starts well before the first word comes out of the speaker, so the old rule (0.8 s after the state started) often moved the robot in silence. `AudioService::MsSinceLastOutput()` says when the last chunk went to the speaker; `TalkTick()` now runs every 100 ms on its own timer, begins 0.15 s after the first sound of a reply, and stands still while no sound comes out (before the first word and in the pauses between sentences). New setting `talk_alternate` (default on): each movement swings one wheel only, left then right then left, the body pivoting about the other wheel; off = both wheels turn the body on the spot as before.

## Hardware limits that shaped the work
- **Wi-Fi at the desk is about -81 dBm** and can go silent for minutes: it dominates latency more than any server setting. Fix with the signal, not the code.
- **No echo cancellation** (the amplifier gives no reference signal): interrupt Jarvis by saying "Jarvis" or pressing BOOT; the full-duplex listen mode is not usable. A second robot with a microphone that has AEC is planned.
- **lwIP has 16 sockets**; the settings page, MQTT, a conversation and a photo upload all share them.
- **Tool cap**: the xiaozhi.me cloud accepts 32 MCP tools; the robot has 29 (5 upstream + 24 here). Fold new actions into existing tools when possible.
- The microSD slot must stay empty (it shares pins with the display); in deep sleep the backlight stays on.

## Parked and open items
- **Flash builds 80 and 81** (waiting for a go-ahead); then restart the voice server so face finding, the voice commands and Smart turn go live, and try them on the robot (calibrate `face_short_ms` / `face_mid_ms` / `face_long_ms` on the real table).
- Wake-up crash (`xEventGroupSetBits` in the ML307 TCP task): seen once, left alone unless it comes back.
- Watch for a cold power-on to confirm the in-place camera restart prevents the double boot.
- Messages to the robot through ntfy ("send a message ..."): designed, 16 questions waiting for answers.
- One window with tabs for all backend services (Windows Terminal, or hidden services with logs in the panel): waiting for a choice.
- Smaller ideas: DeepSeek brain, Haiku comparison, default server compiled into the firmware, OpenRouter as a last-resort brain, speaker prebuffer that adapts to underruns.

## Where everything is stored
- **Git**: firmware repository (this file, `main/boards/wall-e-xiao/`), submodules `web` (landing page) and `android` (app), and the backend repository `xiaozhi-server/temp-test`. Nothing is published to GitHub without a clear yes and a scrub of private details first.
- **Local backups** (`walle-backup/`): `builds/<n>-<label>` (bins, ELF, manifest with the flash command, source copy) for builds 1-87, `android-builds/` for app builds 1-v2.34 (debug APK, demo APK, checksums), `backend/<date>-<hash>` (snapshot of the tracked backend files), flash logs, incident reports, and a copy of this file.
