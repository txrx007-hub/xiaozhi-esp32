#!/usr/bin/env python3
"""Generate the robot's own sounds (original, synthesized) as Ogg Opus like upstream's assets.

Upstream encodes its sounds with: ffmpeg -c:a libopus -b:a 16k -ac 1 -ar 16000 -frame_duration 60
Needs an ffmpeg with libopus: pass its path as the first argument, set FFMPEG, or have
imageio-ffmpeg installed (pip install imageio-ffmpeg).

    python main/boards/wall-e-xiao/make_sounds.py [path/to/ffmpeg]
"""
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
import wave

RATE = 16000
HERE = os.path.dirname(os.path.abspath(__file__))


def find_ffmpeg():
    if len(sys.argv) > 1:
        return sys.argv[1]
    if os.environ.get("FFMPEG"):
        return os.environ["FFMPEG"]
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        return "ffmpeg"


def envelope(i, n, attack, release):
    a = min(1.0, i / max(1, attack))
    r = min(1.0, (n - i) / max(1, release))
    return min(a, r)


def sweep(f0, f1, seconds, amp, vibrato=0.0):
    n = int(RATE * seconds)
    out, phase = [], 0.0
    for i in range(n):
        t = i / n
        f = f0 + (f1 - f0) * t + vibrato * math.sin(2 * math.pi * 30 * i / RATE)
        phase += 2 * math.pi * f / RATE
        # a touch of the 2nd harmonic gives a more "robotic" timbre
        s = math.sin(phase) + 0.25 * math.sin(2 * phase)
        out.append(amp * 0.8 * s * envelope(i, n, RATE * 0.006, RATE * 0.025))
    return out


def silence(seconds):
    return [0.0] * int(RATE * seconds)


def tone(freq, seconds, amp):
    n = int(RATE * seconds)
    return [amp * (math.sin(2 * math.pi * freq * i / RATE) +
                   0.3 * math.sin(2 * math.pi * 2 * freq * i / RATE)) / 1.3 *
            envelope(i, n, RATE * 0.02, RATE * 0.03) for i in range(n)]


def wake_chirp():
    # two quick rising "bee-boop" sweeps, about 0.35 s
    return sweep(900, 1600, 0.13, 0.55) + silence(0.03) + sweep(1300, 2200, 0.18, 0.55, 60)


def shutter():
    rnd = random.Random(7)
    n = int(RATE * 0.09)
    click = [0.7 * math.exp(-i / (RATE * 0.012)) * (rnd.uniform(-1, 1) * 0.6 +
             0.4 * math.sin(2 * math.pi * 2500 * i / RATE)) for i in range(n)]
    return click


def test_tone():
    return tone(660, 0.6, 0.35)


def alarm():
    out = []
    for freq in (1000, 1250, 1000):
        out += tone(freq, 0.15, 0.55) + silence(0.12)
    return out


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767))))
                               for s in samples))


def main():
    ffmpeg = find_ffmpeg()
    sounds = {"wake_chirp": wake_chirp(), "shutter": shutter(), "tone": test_tone(),
              "alarm": alarm()}
    with tempfile.TemporaryDirectory() as tmp:
        for name, samples in sounds.items():
            wav = os.path.join(tmp, name + ".wav")
            write_wav(wav, samples)
            out = os.path.join(HERE, name + ".ogg")
            subprocess.run([ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", wav,
                            "-c:a", "libopus", "-b:a", "16k", "-ac", "1", "-ar", "16000",
                            "-frame_duration", "60", out], check=True)
            print(f"{name}.ogg  {len(samples) / RATE:.2f} s  {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main()
