#include "walle_sounds.h"

// Symbols created by EMBED_FILES for boards/wall-e-xiao/*.ogg (see main/CMakeLists.txt).
extern const char wake_chirp_start[] asm("_binary_wake_chirp_ogg_start");
extern const char wake_chirp_end[] asm("_binary_wake_chirp_ogg_end");
extern const char shutter_start[] asm("_binary_shutter_ogg_start");
extern const char shutter_end[] asm("_binary_shutter_ogg_end");
extern const char tone_start[] asm("_binary_tone_ogg_start");
extern const char tone_end[] asm("_binary_tone_ogg_end");
extern const char alarm_start[] asm("_binary_alarm_ogg_start");
extern const char alarm_end[] asm("_binary_alarm_ogg_end");

namespace walle_sounds {

std::string_view WakeChirp() {
    return {wake_chirp_start, static_cast<size_t>(wake_chirp_end - wake_chirp_start)};
}

std::string_view Shutter() {
    return {shutter_start, static_cast<size_t>(shutter_end - shutter_start)};
}

std::string_view Tone() { return {tone_start, static_cast<size_t>(tone_end - tone_start)}; }

std::string_view Alarm() { return {alarm_start, static_cast<size_t>(alarm_end - alarm_start)}; }

}  // namespace walle_sounds
