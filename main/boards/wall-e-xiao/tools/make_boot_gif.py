"""Makes the boot screen: boot.gif next to the board code, built into the app and shown full screen from power-on until Jarvis is ready
(see WalleDisplay::ShowBootSplash).

    python make_boot_gif.py <picture> [--steps 24] [--frame-ms 60]        (needs Pillow)

The picture (any size or shape) is cut to a square around its brightest red spot - a glowing eye - and turned into a 240 x 240 loop in which that
red glow dims to off and lights up again: lit, fading out, dark, fading back in. Only the glow changes, so the GIF stays small (about 250 KB).

boot.gif is NOT in git (the pictures are not mine to publish): without it the firmware still builds and boots with the normal screen.
After making or removing it, build again (the build checks for the file).
"""
import argparse
import math
from pathlib import Path

from PIL import Image

OUT = Path(__file__).resolve().parents[1] / "boot.gif"
SIZE = 240


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("picture")
    ap.add_argument("--steps", type=int, default=24, help="in-between frames for each fade (more = smoother, bigger file)")
    ap.add_argument("--frame-ms", type=int, default=60, help="time per frame")
    args = ap.parse_args()

    im = Image.open(args.picture).convert("RGB")
    w, h = im.size

    # the eye: the strongest red-dominant spot, searched on a smaller copy
    small = im.resize((max(1, w // 3), max(1, h // 3)))
    px = small.load()
    best, ex, ey = -1, w // 2, h // 2
    for y in range(small.size[1]):
        for x in range(small.size[0]):
            r, g, b = px[x, y]
            s = (r - max(g, b)) * r
            if s > best:
                best, ex, ey = s, x * 3, y * 3

    # a square around it (the eye a little above the middle), kept inside the picture
    side = min(w, h)
    top = int(min(max(ey - side * 0.45, 0), h - side))
    left = int(min(max(ex - side / 2, 0), w - side))
    sq = im.crop((left, top, left + side, top + side)).resize((SIZE, SIZE), Image.LANCZOS)

    # the same picture with the eye off: the red taken out of the glow, the white-hot centre darkened
    cy, cx = (ey - top) * SIZE / side, (ex - left) * SIZE / side
    on = list(sq.getdata())
    off = []
    for i, (r, g, b) in enumerate(on):
        x, y = i % SIZE, i // SIZE
        excess = max(0, r - max(g, b))
        k = 1 - 0.75 * max(0.0, 1 - math.hypot(y - cy, x - cx) / 9.0)
        off.append((int((r - excess) * k), int(g * k), int(b * k)))

    def frame(level):   # 1 = fully lit, 0 = off
        img = Image.new("RGB", (SIZE, SIZE))
        img.putdata([tuple(int(o[c] + (n[c] - o[c]) * level) for c in range(3)) for n, o in zip(on, off)])
        return img

    n = args.steps
    levels = ([1.0] * 10 + [0.5 + 0.5 * math.cos(math.pi * i / n) for i in range(1, n)]
              + [0.0] * 6 + [0.5 - 0.5 * math.cos(math.pi * i / n) for i in range(1, n)])
    frames = [frame(level) for level in levels]
    strip = Image.new("RGB", (SIZE, SIZE * len(frames)))
    for i, f in enumerate(frames):
        strip.paste(f, (0, SIZE * i))
    palette = strip.quantize(colors=256, method=Image.Quantize.MEDIANCUT)   # one palette for all frames: unchanged pixels stay identical
    q = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in frames]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    q[0].save(OUT, save_all=True, append_images=q[1:], duration=args.frame_ms, loop=0, optimize=True)
    print(f"wrote {OUT} ({OUT.stat().st_size // 1024} KB, {len(frames)} frames at {args.frame_ms} ms; eye found at {ex},{ey})")


if __name__ == "__main__":
    main()
