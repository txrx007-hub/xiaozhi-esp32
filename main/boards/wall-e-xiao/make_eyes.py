#!/usr/bin/env python3
"""Recolor xiaozhi's Otto eye GIFs (txp666/otto-emoji-gif-component, MIT) for Wall-E.

Only the GIF color tables are rewritten: every palette entry is scaled by its brightness
onto EYE_COLOR, so black stays black, white becomes the eye color and anti-aliased edges
become darker shades. Frames, timing and compression are untouched.

Run from the project root after the managed components are downloaded:
    python main/boards/wall-e-xiao/make_eyes.py
"""
import os
import shutil
import sys

EYE_COLOR = (0x20, 0xE0, 0xE0)  # cyan, the default for every emotion not listed below

# Per-emotion overrides. "thinking" stands in for "curious"; "relaxed" is the closed-eye
# emotion already used while listening (kListeningEmotion in walle_display.h).
PER_FILE_COLOR = {
    "angry": (0xFF, 0x30, 0x30),     # red
    "happy": (0xFF, 0xFF, 0xFF),     # white
    "thinking": (0x30, 0xE0, 0x30),  # green
    "relaxed": (0xF0, 0xD0, 0x20),   # yellow
    "neutral": (0x20, 0xE8, 0x90),   # turquoise/lime green
}

# "surprised" is baked cyan here (like the default), but WalleDisplay recolors it to a random
# color at runtime each time it's shown (see ApplyEmotionRecolor in walle_display.cc) - the GIF's
# real alpha transparency (checked directly: transparent_flag=1) means LVGL's image_recolor style
# retints only the eye shape, not the black background, so no extra baked variants are needed.

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SOURCE = os.path.join(ROOT, "managed_components", "txp666__otto-emoji-gif-component")
OUTPUT = os.path.join(HERE, "eyes")


def recolor_table(data, offset, entries, color):
    for i in range(entries):
        p = offset + i * 3
        brightness = max(data[p], data[p + 1], data[p + 2]) / 255.0
        for c in range(3):
            data[p + c] = int(round(color[c] * brightness))
    return offset + entries * 3


def skip_sub_blocks(data, pos):
    while True:
        size = data[pos]
        pos += 1
        if size == 0:
            return pos
        pos += size


def recolor_gif(raw, color):
    data = bytearray(raw)
    if data[:6] not in (b"GIF87a", b"GIF89a"):
        raise ValueError("not a GIF")
    packed = data[10]
    pos = 13
    if packed & 0x80:
        pos = recolor_table(data, pos, 2 ** ((packed & 0x07) + 1), color)
    tables = 1 if packed & 0x80 else 0
    while pos < len(data):
        block = data[pos]
        if block == 0x3B:  # trailer
            break
        if block == 0x21:  # extension: label, then sub-blocks
            pos = skip_sub_blocks(data, pos + 2)
        elif block == 0x2C:  # image descriptor
            local = data[pos + 9]
            pos += 10
            if local & 0x80:
                pos = recolor_table(data, pos, 2 ** ((local & 0x07) + 1), color)
                tables += 1
            pos = skip_sub_blocks(data, pos + 1)  # LZW minimum code size, then data
        else:
            raise ValueError(f"unexpected block 0x{block:02x} at {pos}")
    if tables == 0:
        raise ValueError("no color table found")
    return bytes(data)


def main():
    gifs = os.path.join(SOURCE, "gifs")
    if not os.path.isdir(gifs):
        sys.exit(f"Otto GIF component not found at {gifs}; run a build first")
    os.makedirs(OUTPUT, exist_ok=True)
    count = 0
    for name in sorted(os.listdir(gifs)):
        if not name.endswith(".gif"):
            continue
        emotion = name[:-4]
        color = PER_FILE_COLOR.get(emotion, EYE_COLOR)
        with open(os.path.join(gifs, name), "rb") as f:
            recolored = recolor_gif(f.read(), color)
        with open(os.path.join(OUTPUT, name), "wb") as f:
            f.write(recolored)
        count += 1
    shutil.copyfile(os.path.join(SOURCE, "LICENSE"), os.path.join(OUTPUT, "LICENSE-otto-emoji-gif"))
    print(f"Recolored {count} eye GIFs into {OUTPUT}")


if __name__ == "__main__":
    main()
