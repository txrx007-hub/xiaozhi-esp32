#pragma once

#include <expected>
#include <string>

// Turning towards a face, and the small body movements while Jarvis talks. All of it drives the wheels only (no neck).
//
// A look is one picture sent to the own server (POST /mcp/vision/face on the OTA address' host), which answers where the biggest face is.
// The turn is worked out by the robot: its duration follows from how far off-centre the face is and from the turning speed the robot has learned from its own
// earlier turns; the power is face_power (80% for small corrections, never lower); the margin before it turns at all is face_deadzone.
//   FaceMe()   one-shot "look at me": at most four pictures. Each one is inspected first, then the robot decides where and how far to turn (towards the face, or on the spot to search
//              when there is no face in it), and takes the next picture, until the face is in the middle.
//   follow     the follow_mode setting: a background task repeats a look every follow_ms while Jarvis is idle.
//   TalkTick() every 100 ms (own timer): while Jarvis speaks and sound is really coming out of the speaker, now and then a quick left-right sway, matched to the current emotion.
namespace walle_track {

struct Face {
    bool found = false;
    float x = 0;      // centre of the biggest face: -1 = left edge of the picture, +1 = right edge
    float width = 0;  // its width as a part of the picture width
    int count = 0;    // faces seen
};

// Creates the worker task and switches follow mode off (it is never on after a restart).
void Start();
// Tells the worker that a setting (follow_mode) changed or a one-shot was asked for.
void Wake();

// One picture and one server round trip. Blocks for a second or two.
std::expected<Face, std::string> Look();
// One-shot, blocking (about 3 to 8 s): returns what to tell the user.
std::expected<std::string, std::string> FaceMe();
// Same, but on the worker task (for the settings page and the app): returns at once.
void FaceMeLater();

// True while a look or a correction is under way; the talking movements wait for it.
bool Busy();

// Called every 100 ms by its own timer. Does a talking movement now and then while sound is coming out of the speaker.
void TalkTick();

}  // namespace walle_track
