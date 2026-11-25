"""Tiny screenshot analysis helpers (Pillow) used by the scenario scripts."""
import os
import tempfile

from PIL import Image


def grab(rt, name="grab.png"):
    path = os.path.join(tempfile.gettempdir(), "a2_" + name)
    rt.shot(path)
    return Image.open(path).convert("RGB")


MENU_ROWS = (112, 130, 148)   # title screen: Demarrer, Continuer, Options


def _cursor_pixels(img, row):
    px = img.load()
    n = 0
    for y in range(row - 5, row + 6):
        for x in range(100, 123):
            r, g, b = px[x, y]
            if (r > 150 and g < 90 and b < 90) or (r > 215 and g > 215 and b > 215):
                n += 1
    return n


def menu_cursor_row(rt, samples=6, gap=7):
    """Row index (0..2) of the blinking title cursor, or None. Looks over several frames."""
    score = [0] * len(MENU_ROWS)
    for _ in range(samples):
        img = grab(rt)
        for i, row in enumerate(MENU_ROWS):
            score[i] += _cursor_pixels(img, row)
        rt.wait_frames(gap)
    best = max(range(len(MENU_ROWS)), key=lambda i: score[i])
    others = sorted(score)[:-1]
    return best if score[best] > 12 and score[best] > 2 * (others[-1] + 1) else None


def menu_arrow_y(img):
    """Mean y of the red menu cursor (x 95..125), or None (only while it is not blinking)."""
    px = img.load()
    ys = [y for y in range(img.height) for x in range(95, 125)
          if px[x, y][0] > 190 and px[x, y][1] < 70 and px[x, y][2] < 70]
    return sum(ys) / len(ys) if len(ys) > 6 else None


def hud_present(img):
    """True when the in-game HUD (gold hearts frame, top-left) is on screen."""
    px = img.load()
    gold = sum(1 for y in range(4, 44) for x in range(4, 70)
               if px[x, y][0] > 150 and 90 < px[x, y][1] < 200 and px[x, y][2] < 90)
    return gold > 60


def mean_brightness(img):
    px = img.load()
    n = img.width * img.height
    return sum(sum(px[x, y]) for y in range(0, img.height, 4) for x in range(0, img.width, 4)) / (n / 16) / 3
