# Wall-E robot ("Jarvis") on a Seeed XIAO ESP32-S3 Sense

Custom xiaozhi board for the Huy Vector Wall-E build (hardware only; his firmware is not used).
The robot answers to **"Jarvis"** (WakeNet9 `wn9_jarvis_tts`).

| Part | Details |
|---|---|
| Board | XIAO ESP32-S3 Sense, 8 MB flash, 8 MB octal PSRAM |
| Camera | OV3660 (esp_video, OV2640/OV5640 auto-detect also enabled) |
| Audio | on-board PDM mic (CLK 42, DATA 41) · MAX98357A (DIN 5, BCLK 6, LRC 43) |
| Display | ST7789 240x240 SPI (SCLK 9, MOSI 8, DC 44, RST 7, CS to GND, BL to 3V3) |
| Motors | Mini L298N / MX1508: IN1 4, IN2 3 (motor A, right wheel) · IN3 2, IN4 1 (motor B, left) |

## Build

```
python scripts/build.py wall-e-xiao --language en-US --wake-word wn9_jarvis_tts
```

Sounds and eyes are generated files that are committed; regenerate them with
`make_sounds.py` (needs ffmpeg with libopus) and `make_eyes.py`.

## Flash (keeps Wi-Fi and the xiaozhi.me pairing)

The partition table keeps `nvs` at 0x9000 (24 KB) like the original firmware. Flash the four
parts, never the merged image (a merged image overwrites the settings area with 0xFF):

```
esptool --chip esp32s3 -p COM10 -b 921600 write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 xiaozhi.bin 0x400000 generated_assets.bin
```

Restore the original firmware and settings: `esptool --chip esp32s3 -p COM10 write-flash 0x0 wall-e-original.bin`.

After an unexplained reboot, pull the crash report it left behind (see Notes: crash reports)
before doing anything else — flashing new firmware doesn't erase it, but a second crash overwrites
it with the newer one:

```
idf.py -p COM10 coredump-info -c build/xiaozhi.elf
```

## Voice (MCP tools)

| Tool | Say for example |
|---|---|
| `self.robot.move` / `turn` / `stop` | "drive forward for two seconds", "turn around", "stop" |
| `self.robot.dance` | "do a little dance" (random, wiggle, spin, happy) |
| `self.robot.look_around` | "look around the room" (4 photos, one 2x2 collage, one answer) |
| `self.screen.set_emotion` | "look sad", "what faces can you make?" (the emotion list is in the tool's own description) |
| `self.audio.set_mic_gain` / `set_wake_threshold` / `get_tuning` | "make the microphone more sensitive", "wake up more easily" |
| `self.settings.list` / `get` / `set` / `reset` | "you drift to the left", "set quiet hours from 22:00 to 07:00" |
| `self.system.enter_standby` / `get_standby` | "take a nap" |
| `self.system.deep_sleep` | "sleep for two hours", "sleep until 7 o'clock" |
| `self.timer.set` / `list` / `cancel` | "set a tea timer for 5 minutes", "alarm at 7:30" |
| `self.weather.get` | "what's the weather?" (city: "set my weather city to Amsterdam") |
| `self.display.set_visualizer` | "show the winamp visualizer" (mode: off or winamp; setting `visualizer_mode`) |
| `self.display.show_screensaver` | "turn on screensaver mode" - shows the idle clock right away; leaves by itself on the next wake word |
| `self.diagnostics.run_check` | "check everything", "run a diagnostic on the camera" (the check list is in the tool's own description) |
| upstream | volume, take photo (silent, surprised eyes), device status, theme |

## Screen

Cyan eye expressions (xiaozhi's Otto set, 21 emotions; gold neutral, and surprised picks a fresh
random color each time it's shown) · Wi-Fi icon and status text on top · status dot top-right
(green ready, amber starting/connecting, red offline) · level bars under the eyes (Jarvis's voice
while speaking, your voice while listening) - replaced by a 32-band Winamp-style mirrored spectrum
with peak-hold dots while speaking when `visualizer_mode` is `winamp` (default `off`, plain bars) ·
one scrolling line of Jarvis's words · closed eyes when listening · big clock with date, weather
and wake word hint after 5 idle minutes, or right away by voice ("turn on screensaver mode") ·
blank screen while napping · timer alert · `!pattern` test pattern.

## LAN settings page

`http://<robot-ip>/` (also shown by `!status` and the console). Plain HTTP, no login — your
home network only, same trust level as the USB console. Every setting gets the matching
control: a slider with a live numeric readout while dragging for a number, a dropdown for an
on/off or multiple-choice setting, a time picker (with an Off switch) for the quiet hours, and
a text box for the weather city. Changes save the instant you release the slider or change the
dropdown, through the same `WalleSettings::Set()` path as voice and the console. A "reset all"
button is at the bottom. The header also shows MAC address, uptime, current state (idle,
listening, speaking, napping, ...) and a live LAN round-trip time, polled 3x/sec against
`GET /api/ping` (an empty response, so the number reflects request/response overhead rather than
payload transfer). Served from the app itself (`GET /`, `GET`/`POST /api/settings`,
`POST /api/reset`, `GET /api/status`, `GET /api/ping`); starts when Wi-Fi connects, stops on
disconnect.

## USB console

`!status`, `!server IP|URL|default`, `!wifi SSID PASS|list|clear`, `!camera`,
`!mic status|gain N|mute|unmute|meter [s]`, `!speaker [vol N|status]`, `!stop`, `!motors`,
`!face EMOTION [ms]`, `!settings [set KEY VALUE|reset]`, `!standby`/`!nap`, `!wake`,
`!sleep [MIN]`, `!diag CHECK`, `!timers`, `!weather`, `!pattern [s]`, `!reboot`, `!help`.

## Settings (NVS namespace `walle`)

motor_max_speed 70 · motor_trim 0 · turn_ms_per_90 600 · motor_a_invert / motor_b_invert /
motor_swap 0 · soft_start_ms 200 · mic_gain_db 18 · wake_threshold 0.52 (model default 0.65 while
speaking) · max_volume 80 · shutter_sound 0 · wake_chirp 1 · quiet_start /
quiet_end off (night mode: no chirp, volume cap 40) · idle_clock_min 5 · weather_city off ·
visualizer_mode off · log_level warn.

## Changes outside this folder (all marked `WALL-E`)

- `audio/engines/afe_audio_engine.*`, `audio/audio_engine.h`, `audio/audio_service.*`: runtime
  WakeNet threshold; AFE reset uses a try-lock (producer/consumer deadlock, as in polunzh's fork).
- `application.cc`: stop buffered audio at once when the wake word interrupts a reply; the board's
  wake cue replaces the popup sound.
- `boards/common/board.h`: `OnWakeWordDetected()` hook (no-op for other boards).
- `boards/common/esp_video.h`: `frame_` is protected (look_around collage).
- `scripts/build_default_assets.py`: an emoji collection may be a project folder.
- `Kconfig.projbuild`, `CMakeLists.txt`: board entry, board sounds and web page embedding,
  USB console and LAN web page component requirements (`esp_driver_usb_serial_jtag`,
  `esp_http_server`).

## Notes

- No echo cancellation (the amp gives no reference), so interrupt by saying "Jarvis" or pressing
  BOOT. Add "never say the word computer" to the agent's role prompt on xiaozhi.me.
- Keep the microSD slot empty: it shares GPIO7/8/9 with the display.
- Deep sleep: the backlight is wired to 3V3 and stays lit; the timer drifts by minutes overnight.
- Weather comes from Open-Meteo (free, no key); the city name and its coordinates are sent there.
- Camera color: this OV3660 module's raw YUYV output carries a uniform magenta/purple cast (its
  own AWB doesn't correct it, and esp_video/esp_cam_sensor define a white-balance control -
  `ESP_CAM_SENSOR_WB` - that nothing in this stack actually wires through a V4L2 control, so there
  is no hardware knob to turn from here). Fixed in software instead: `WalleCamera::CorrectColorCast()`
  subtracts a fixed bias (found by eye against `/debug/photo.jpg`, not derived from a calibration)
  from every U/V byte, pulling the color back toward neutral. An earlier attempt retagged the frame
  from YUYV to UYVY, assuming a byte-order swap; that was wrong and made it worse (a banded
  green/magenta corruption, not a tint) - the sensor's own YUYV tag was correct all along.
- Crash reports: random reboots used to leave nothing behind - the panic handler prints a
  backtrace once, live, to the USB serial console, and without a coredump partition that's gone
  the moment nobody was watching. A `coredump` partition (256 KB, carved out of factory's spare
  room, right before `assets`) now catches it: `esp_core_dump` writes the crashed task's registers,
  backtrace and stack to flash. `espcoredump` must be an unconditional `PRIV_REQUIRES` of `main`,
  not gated behind `if(CONFIG_BOARD_TYPE_WALL_E_XIAO)` - MINIMAL_BUILD resolves Kconfig visibility
  in an early pass with no `CONFIG_*` values defined yet, so a board-gated require is invisible to
  it and every `CONFIG_ESP_COREDUMP_*` line silently no-ops (confirmed the hard way: a full build
  cycle with no errors, yet the option never reached `sdkconfig`). `ESP_COREDUMP_CAPTURE_DRAM`
  (the whole heap/.bss/.data, not just registers and a backtrace) was tried and reverted - it
  needs several hundred KB beyond what registers-and-backtrace alone need, more than this board's
  internal-RAM headroom leaves room for in a 256 KB partition; the very first real crash after
  enabling it failed to write with "Not enough space to save core dump!". Survives a normal
  reflash (it isn't in the write-flash command above) - only a fresh crash or a full chip erase
  clears it. Read it with `idf.py -p COM10 coredump-info -c build/xiaozhi.elf` (needs the exact ELF
  the crashed build was compiled from - `build/xiaozhi.elf` after a matching rebuild, or the one
  saved alongside that build in `walle-backup/builds`). One caveat: several of our real tasks have
  their stacks in PSRAM (`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`); if the *crashing* task's own
  stack was one of those, its backtrace may come back incomplete - the register dump and other
  tasks' backtraces are unaffected.
- Random reboots (root cause found via the crash reports above): `WalleDisplay::CreateClock()`
  reads `theme->text_font()` while building the idle clock's labels; if the theme's font asset
  hadn't finished loading yet at that point in boot, `text_font` came back null, and `MakeLabel()`
  used to skip setting a font at all rather than fail loudly. On `clock_root_` - a style-stripped
  box on `lv_layer_top()`, so there's no cascaded font to fall back on - that label was left with a
  permanently null font. It rendered fine (nothing to draw, nothing to lay out) until the next full
  relayout - entering power save (`ShowIdleClock(true)`) is one - which walks into
  `lv_font_get_glyph_width` through that null pointer: `Guru Meditation Error ... PC: 0x00000000`.
  Fixed at the one chokepoint instead of each of `MakeLabel()`'s 7 call sites: it now always falls
  back to `LV_FONT_DEFAULT` (compiled in - Montserrat 14) rather than silently leaving a label
  fontless.
  are unaffected.
- xiaozhi.me caps a device at 32 registered MCP tools (not enforced by this firmware - the cloud
  side rejects a bigger tools/list). Currently at 28 (5 upstream + 23 here): mind that headroom
  before adding another voice tool, and prefer folding a new action into an existing tool's
  parameters over adding a whole new one.
- How long Jarvis stays listening after the wake word before giving up on silence is decided by
  the xiaozhi.me backend's own speech/VAD detection, not by this firmware - there is no local
  timeout to tune (a client-side one was tried and reverted: it rebooted on every settings-page
  change and did not reliably shorten the wait, so this stays stock xiaozhi behavior).
- Screensaver: `self.display.show_screensaver` shows the idle clock immediately instead of waiting
  for `idle_clock_min`. It is deferred to the next time the device is actually idle (same pattern
  as the nap request), since the tool runs mid-conversation, and it exits
  through the exact same path idle_clock_min's own timeout already uses - there's no separate
  "turn it off" command, saying the wake word already exits it.
- Visualizer: the 32-band FFT (esp-dsp, Hann-windowed, log-spaced 80 Hz-8 kHz bands) runs inside
  `WalleAudioCodec::Write()` - i.e. inline on the audio task, once per ~43 ms (1024 samples at
  24 kHz) it has accumulated - not in a separate task. It only ever looks at what is actually being
  sent to the speaker, and only runs at all when `visualizer_mode` is `winamp`, so it costs nothing
  while off or while Jarvis is silent.
- Credits: eye GIFs by txp666 (otto-emoji-gif-component, MIT, see `eyes/LICENSE-otto-emoji-gif`);
  ideas from polunzh/xiaozhi-esp32 (instant wake cue, idle clock) and ricklon/xiaozhi-esp32
  (console, standby, diagnostics). No code or assets from the original Wall-E firmware.
