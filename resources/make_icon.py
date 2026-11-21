import math
import os
import struct
import sys
import zlib

SS = 4                       # supersampling factor
BACKGROUND = (15, 23, 42)    # slate, matches a dark title bar
LOOP = (56, 189, 248)        # sky blue
GLOW = (14, 116, 144)        # dim halo so the loop keeps shape when scaled down


def draw(size):
    """Return a size x size RGBA bytearray."""
    big = size * SS
    # Work in floats per channel with an alpha plane.
    pixels = [[(0.0, 0.0, 0.0, 0.0)] * big for _ in range(big)]

    radius = big * 0.22          # rounded-square corner radius
    inset = big * 0.03

    def inside_rounded(x, y):
        left, top = inset, inset
        right, bottom = big - inset, big - inset
        cx = min(max(x, left + radius), right - radius)
        cy = min(max(y, top + radius), bottom - radius)
        if left <= x <= right and top <= y <= bottom:
            if (cx == x or cy == y):
                return True
            return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2
        return False

    for y in range(big):
        for x in range(big):
            if inside_rounded(x + 0.5, y + 0.5):
                # A gentle vertical lift, so the tile is not a flat slab.
                t = y / float(big)
                shade = 1.0 + 0.35 * (1.0 - t)
                pixels[y][x] = (BACKGROUND[0] * shade, BACKGROUND[1] * shade,
                                BACKGROUND[2] * shade, 255.0)

    # --- the loop ------------------------------------------------------
    # Two tanh branches offset by the coercive field: the standard shape of a
    # P-E hysteresis loop, which is this program's signature measurement.
    # Chosen so the two branches bow apart and read as a LOOP rather than as
    # a letter S. A sharper transition is more physical but, at 16 px, is no
    # longer recognisable as a hysteresis curve.
    coercive = 0.40
    width = 0.16
    # Sweep past saturation, so the loop gets the flat tails that make it read
    # as a hysteresis curve instead of a letter S.
    reach = 1.15
    margin = big * 0.23
    span = big - 2 * margin

    def point(field, polarization):
        x = margin + (field / reach + 1.0) * 0.5 * span
        y = margin + (1.0 - polarization) * 0.5 * span
        return x, y

    stroke = max(1.6, big * 0.056)
    halo = stroke * 1.9
    steps = big * 3

    track = []
    for branch, shift in ((0, +coercive), (1, -coercive)):
        for i in range(steps + 1):
            field = -reach + 2.0 * reach * i / steps
            polarization = math.tanh((field + shift) / width)
            track.append(point(field, polarization))

    def stamp(colour, thickness, alpha):
        half = thickness * 0.5
        for px, py in track:
            x0 = max(0, int(px - half) - 1)
            x1 = min(big - 1, int(px + half) + 1)
            y0 = max(0, int(py - half) - 1)
            y1 = min(big - 1, int(py + half) + 1)
            for y in range(y0, y1 + 1):
                dy = y + 0.5 - py
                for x in range(x0, x1 + 1):
                    dx = x + 0.5 - px
                    if dx * dx + dy * dy <= half * half:
                        r, g, b, a = pixels[y][x]
                        na = max(a, 255.0 * alpha)
                        pixels[y][x] = (colour[0], colour[1], colour[2], na)

    stamp(GLOW, halo, 1.0)
    stamp(LOOP, stroke, 1.0)

    # --- box downsample -------------------------------------------------
    out = bytearray(size * size * 4)
    area = float(SS * SS)
    for y in range(size):
        for x in range(size):
            r = g = b = a = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    pr, pg, pb, pa = pixels[y * SS + sy][x * SS + sx]
                    # Premultiply, so transparent pixels do not drag colour in.
                    weight = pa / 255.0
                    r += pr * weight
                    g += pg * weight
                    b += pb * weight
                    a += pa
            a /= area
            if a > 0.0:
                scale = area * (a / 255.0)
                r, g, b = r / scale, g / scale, b / scale
            index = (y * size + x) * 4
            out[index] = int(max(0, min(255, r)))
            out[index + 1] = int(max(0, min(255, g)))
            out[index + 2] = int(max(0, min(255, b)))
            out[index + 3] = int(max(0, min(255, a)))
    return out


def png(size, rgba):
    def chunk(tag, payload):
        data = tag + payload
        return (struct.pack(">I", len(payload)) + data +
                struct.pack(">I", zlib.crc32(data) & 0xFFFFFFFF))

    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter: none
        raw += rgba[y * size * 4:(y + 1) * size * 4]
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = []
    for size in sizes:
        data = png(size, draw(size))
        images.append((size, data))
        print("rendered %d" % size)

    with open(os.path.join(out_dir, "appicon.png"), "wb") as handle:
        handle.write(dict(images)[256])

    # ICO: a 6-byte header, one 16-byte entry per image, then the PNG payloads.
    # PNG inside ICO is read by every Windows since Vista.
    offset = 6 + 16 * len(images)
    header = struct.pack("<HHH", 0, 1, len(images))
    entries = b""
    payload = b""
    for size, data in images:
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32,
                               len(data), offset)
        payload += data
        offset += len(data)
    with open(os.path.join(out_dir, "appicon.ico"), "wb") as handle:
        handle.write(header + entries + payload)
    print("wrote appicon.ico and appicon.png in %s" % out_dir)


if __name__ == "__main__":
    main()
