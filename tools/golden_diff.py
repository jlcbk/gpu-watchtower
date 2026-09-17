#!/usr/bin/env python3
"""golden_diff.py — rig-lookout golden 像素回归的比对/篡改工具（P2b）。

子命令：
  cmp <a.png> <b.png>          逐像素比对两帧（黑/白 1bit 语义），PASS/FAIL。
  flip <in.png> <out.png> X Y  解码 in.png，翻转 (X,Y) 一个像素，按模拟器同款
                               确定性编码（1-bit 灰度 + stored zlib）写出 out.png，
                               供注入自检用（golden 原件永不触碰）。

比对语义：像素级（黑=采样 0，白=采样 1）。支持 bit depth 1/8、color type 0
（灰度）、全部 5 种行滤波——模拟器新 writer（1-bit/filter None）与 sips 等第三方
重编码产物均可比对。尺寸不一致/解码失败按 FAIL 报告。

退出码：0=PASS  1=FAIL（含尺寸/解码失败）  2=用法或 IO 错误。
仅用标准库（zlib/struct），无第三方依赖。
"""
import struct
import sys
import zlib

PNG_SIG = b"\x89PNG\r\n\x1a\n"


class PngError(Exception):
    pass


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_gray_png(path):
    """读灰度 PNG（depth 1/8, colortype 0）→ (w, h, black 集合位图 bytes)。"""
    with open(path, "rb") as f:
        d = f.read()
    if d[:8] != PNG_SIG:
        raise PngError("not a PNG (bad signature)")
    off, idat, (w, h, depth, ctype) = 8, b"", (None, None, None, None)
    while off < len(d):
        ln, typ = struct.unpack(">I4s", d[off:off + 8])
        body = d[off + 8:off + 8 + ln]
        crc = struct.unpack(">I", d[off + 8 + ln:off + 12 + ln])[0]
        if crc != zlib.crc32(typ + body) & 0xFFFFFFFF:
            raise PngError(f"chunk {typ!r} CRC mismatch")
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        off += 12 + ln
    if w is None or not idat:
        raise PngError("missing IHDR/IDAT")
    if ctype != 0 or depth not in (1, 8):
        raise PngError(f"unsupported PNG: depth={depth} colortype={ctype} (want gray 1/8)")
    raw = zlib.decompress(idat)
    stride = (w * depth + 7) // 8
    if len(raw) != h * (stride + 1):
        raise PngError(f"raw size {len(raw)} != {h}x{stride + 1}")
    # 反滤波（字节域，1bpp 与 8bpp 同法）
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if ft == 1:
            for i in range(stride):
                row[i] = (row[i] + row[i - 1]) & 0xFF if i else row[i]
        elif ft == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                left = row[i - 1] if i else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                left = row[i - 1] if i else 0
                upleft = prev[i - 1] if i else 0
                row[i] = (row[i] + _paeth(left, prev[i], upleft)) & 0xFF
        elif ft != 0:
            raise PngError(f"bad filter type {ft} at row {y}")
        out[y * stride:(y + 1) * stride] = row
        prev = row
    return w, h, depth, stride, bytes(out)


def is_black(w, h, depth, stride, pix, x, y):
    if not (0 <= x < w and 0 <= y < h):
        raise PngError(f"pixel ({x},{y}) out of range {w}x{h}")
    if depth == 1:
        byte = pix[y * stride + (x >> 3)]
        return not (byte >> (7 - (x & 7))) & 1  # 采样 0=黑
    return pix[y * stride + x] < 0x80


def cmd_cmp(a_path, b_path):
    try:
        A = read_gray_png(a_path)
        B = read_gray_png(b_path)
    except (PngError, OSError, zlib.error) as e:
        print(f"FAIL decode {a_path} / {b_path}: {e}")
        return 1
    if A[:2] != B[:2]:
        print(f"FAIL dims {a_path} {A[0]}x{A[1]} vs {b_path} {B[0]}x{B[1]}")
        return 1
    w, h = A[:2]
    ndiff, first = 0, None
    for y in range(h):
        for x in range(w):
            ba = is_black(*A, x, y)
            bb = is_black(*B, x, y)
            if ba != bb:
                ndiff += 1
                if first is None:
                    first = (x, y)
    if ndiff == 0:
        print(f"PASS {a_path} == {b_path} ({w}x{h})")
        return 0
    print(f"FAIL {a_path} vs {b_path}: diff_pixels={ndiff} first={first}")
    return 1


def write_gray1_png(path, w, h, rows_bit_packed):
    """模拟器同款确定性编码：1-bit 灰度、filter None、zlib stored 块。"""
    stride = (w + 7) // 8
    raw = bytearray()
    for row in rows_bit_packed:
        raw.append(0)  # filter None
        raw += row
    z = bytearray(b"\x78\x01")
    off = 0
    while off < len(raw):
        blk = min(65535, len(raw) - off)
        last = 1 if off + blk >= len(raw) else 0
        z += bytes([last, blk & 0xFF, blk >> 8, ~blk & 0xFF, ~(blk >> 8) & 0xFF])
        z += raw[off:off + blk]
        off += blk
    ad = zlib.adler32(raw) & 0xFFFFFFFF
    z += bytes([ad >> 24, (ad >> 16) & 0xFF, (ad >> 8) & 0xFF, ad & 0xFF])

    def chunk(typ, body):
        c = struct.pack(">I", len(body)) + typ + body
        return c + struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 1, 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(PNG_SIG)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", bytes(z)))
        f.write(chunk(b"IEND", b""))


def cmd_flip(in_path, out_path, fx, fy):
    try:
        w, h, depth, stride, pix = read_gray_png(in_path)
        cur_black = is_black(w, h, depth, stride, pix, fx, fy)
        rows = []
        for y in range(h):
            row = bytearray(pix[y * stride:(y + 1) * stride])
            if y == fy:
                if depth == 1:
                    row[fx >> 3] ^= 1 << (7 - (fx & 7))
                else:
                    row[fx] = 0x00 if row[fx] >= 0x80 else 0xFF
            rows.append(bytes(row))
        write_gray1_png(out_path, w, h, rows)
        print(f"flipped ({fx},{fy}) in {in_path} -> {out_path} (was {'black' if cur_black else 'white'})")
        return 0
    except (PngError, OSError, zlib.error) as e:
        print(f"ERROR flip: {e}")
        return 2


def main(argv):
    if len(argv) >= 4 and argv[1] == "cmp":
        return cmd_cmp(argv[2], argv[3])
    if len(argv) >= 6 and argv[1] == "flip":
        return cmd_flip(argv[2], argv[3], int(argv[4]), int(argv[5]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
