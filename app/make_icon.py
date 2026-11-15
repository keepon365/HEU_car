import zlib, struct, math, os

OUT = os.path.dirname(os.path.abspath(__file__)) + "/res"

def png(path, w, h, rgba):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw += rgba[y*stride:(y+1)*stride]
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + \
          chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(out)

TOP = (0x18, 0x24, 0x30)
BOT = (0x08, 0x0C, 0x11)
ACCENT = (0x35, 0xE0, 0xA1)
DIMRING = (0x25, 0x6B, 0x57)
WHITE = (0xEC, 0xF3, 0xFA)

def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i]-a[i])*t)) for i in range(3))

def inside_round_rect(x, y, r):
    qx = max(r - x, 0.0, x - (1.0 - r))
    qy = max(r - y, 0.0, y - (1.0 - r))
    return qx*qx + qy*qy <= r*r

def in_ring(x, y, rad, wid):
    d = math.hypot(x-0.5, y-0.5)
    return abs(d - rad) <= wid/2.0

def in_tri(x, y):
    # 朝上的等腰三角：顶点 (0.5,0.375)，底 y=0.585，半宽 0.088
    ax, ay = 0.5, 0.375
    by = 0.585
    hw = 0.088
    if y < ay or y > by:
        return False
    t = (y - ay) / (by - ay)
    return abs(x - ax) <= hw * t

def render(size):
    ss = 3
    buf = bytearray(size*size*4)
    for py in range(size):
        for px in range(size):
            ar = ag = ab = aa = 0.0
            for sy in range(ss):
                for sx in range(ss):
                    x = (px + (sx+0.5)/ss) / size
                    y = (py + (sy+0.5)/ss) / size
                    if not inside_round_rect(x, y, 0.225):
                        continue
                    col = lerp(TOP, BOT, y)
                    if in_ring(x, y, 0.355, 0.050):
                        col = ACCENT
                    elif in_ring(x, y, 0.245, 0.012):
                        col = DIMRING
                    elif in_tri(x, y):
                        col = WHITE
                    ar += col[0]; ag += col[1]; ab += col[2]; aa += 1.0
            n = ss*ss
            if aa == 0:
                continue
            o = (py*size + px) * 4
            buf[o]   = int(ar/aa)
            buf[o+1] = int(ag/aa)
            buf[o+2] = int(ab/aa)
            buf[o+3] = int(255 * aa / n)
    return buf

for d, sz in [("mipmap-mdpi",48), ("mipmap-hdpi",72), ("mipmap-xhdpi",96),
              ("mipmap-xxhdpi",144), ("mipmap-xxxhdpi",192)]:
    os.makedirs(os.path.join(OUT, d), exist_ok=True)
    p = os.path.join(OUT, d, "ic_launcher.png")
    png(p, sz, sz, render(sz))
    print(d, sz, os.path.getsize(p), "bytes")
