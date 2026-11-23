"""make_gif.py — stitch the recorded frames into gif/WordReader-making-of.gif.

Frames come from gif/frames/dev2019 (framebuffer recorder) and gif/frames/duo
(DuoShot helper); gif/storyboard.json says which frames to use, in what order,
with which caption and for how long. Title cards are drawn here.
"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "gif")
W, IMG_H, BAR = 720, 540, 64           # frame area + caption bar
H = IMG_H + BAR
BG, FG, ACCENT, DIM = (24, 24, 28), (240, 240, 236), (255, 196, 64), (150, 150, 150)


def font(size, bold=False):
    for f in (["/System/Library/Fonts/Supplemental/Arial Bold.ttf"] if bold else []) + \
             ["/System/Library/Fonts/Supplemental/Arial.ttf", "/System/Library/Fonts/Helvetica.ttc"]:
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def wrap(draw, text, fnt, width):
    lines, line = [], ""
    for word in text.split():
        test = (line + " " + word).strip()
        if draw.textlength(test, font=fnt) <= width:
            line = test
        else:
            lines.append(line); line = word
    return lines + [line] if line else lines


def frame(path, tag, caption, crop=None):
    im = Image.open(path).convert("RGB")
    if crop:
        im = im.crop(tuple(crop))
    im.thumbnail((W, IMG_H), Image.LANCZOS)
    canvas = Image.new("RGB", (W, H), BG)
    canvas.paste(im, ((W - im.width) // 2, (IMG_H - im.height) // 2))
    d = ImageDraw.Draw(canvas)
    d.rectangle([0, IMG_H, W, H], fill=BG)
    d.text((14, IMG_H + 8), tag, font=font(15, True), fill=ACCENT)
    tw = d.textlength(tag, font=font(15, True))
    for i, line in enumerate(wrap(d, caption, font(15), W - 40 - tw)[:3]):
        d.text((24 + tw, IMG_H + 8 + i * 18), line, font=font(15), fill=FG)
    return canvas


def card(title, lines):
    canvas = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(canvas)
    d.text((48, 150), title, font=font(34, True), fill=ACCENT)
    y = 215
    for line in lines:
        for part in wrap(d, line, font(19), W - 96):
            d.text((48, y), part, font=font(19), fill=FG)
            y += 27
        y += 10
    d.text((48, H - 44), "MacWordReader · MPW on System 7.6.1 · 2026-11-23", font=font(13), fill=DIM)
    return canvas


def main():
    story = json.load(open(os.path.join(ROOT, "storyboard.json")))
    frames, durations = [], []
    for s in story:
        if "card" in s:
            frames.append(card(s["card"], s.get("lines", [])))
        else:
            frames.append(frame(os.path.join(ROOT, "frames", s["file"]), s["tag"], s["caption"], s.get("crop")))
        durations.append(int(s.get("seconds", 2.6) * 1000))
    # one shared palette keeps the GIF small and stops colours flickering between frames
    pal_src = Image.new("RGB", (W, H * len(frames)))
    for i, f in enumerate(frames):
        pal_src.paste(f, (0, i * H))
    palette = pal_src.quantize(colors=128, method=Image.MEDIANCUT)
    gif = [f.quantize(palette=palette, dither=Image.NONE) for f in frames]
    out = os.path.join(ROOT, "WordReader-making-of.gif")
    gif[0].save(out, save_all=True, append_images=gif[1:], duration=durations, loop=0, optimize=True)
    print(out, len(frames), "frames", os.path.getsize(out) // 1024, "KB")


if __name__ == "__main__":
    main()
