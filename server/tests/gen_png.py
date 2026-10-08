#!/usr/bin/env python3
"""Genera un PNG de color liso: gen_png.py ANCHO ALTO SALIDA (sólo biblioteca estándar)."""
import struct, sys, zlib

w, h, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
raw = b"".join(b"\x00" + b"\x20\x40\x80" * w for _ in range(h))
open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
