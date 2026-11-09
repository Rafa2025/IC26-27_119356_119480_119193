"""
imgio.py - read/write RAW, PGM (P5) and PPM (P6) images with numpy.

Image data is always returned as a numpy array:
  - grayscale (RAW / PGM): shape (rows, cols)
  - color (PPM):           shape (rows, cols, 3)   channel order R, G, B
  dtype is uint8 for 8 bpp and uint16 for 16 bpp.
Files ending in .gz are read transparently.
"""
import gzip
import re
from dataclasses import dataclass

import numpy as np


@dataclass
class Image:
    data: np.ndarray      # (rows, cols) or (rows, cols, 3)
    fmt: str              # 'raw', 'pgm' or 'ppm'
    bpp: int              # bits per sample: 8 or 16
    maxval: int           # max sample value (from header, or 2**bpp - 1 for RAW)
    endian: str = "be"    # byte order of 16 bpp samples in the file ('be' or 'le')

    @property
    def rows(self):
        return self.data.shape[0]

    @property
    def cols(self):
        return self.data.shape[1]

    @property
    def channels(self):
        return 1 if self.data.ndim == 2 else self.data.shape[2]

    def channel(self, i):
        """Return channel i as a 2-D array (for grayscale, i must be 0)."""
        return self.data if self.data.ndim == 2 else self.data[:, :, i]

    def channel_names(self):
        return ["Gray"] if self.channels == 1 else ["R", "G", "B"]


# ---------------------------------------------------------------- helpers
def _open(path, mode):
    return gzip.open(path, mode) if str(path).endswith(".gz") else open(path, mode)


def _read_header(buf):
    """Parse a PNM header. Returns (magic, cols, rows, maxval, offset_of_pixel_data)."""
    tokens, pos = [], 0
    while len(tokens) < 4:
        # skip whitespace
        while pos < len(buf) and buf[pos:pos + 1].isspace():
            pos += 1
        if pos >= len(buf):
            raise ValueError("truncated PNM header")
        # skip comment lines
        if buf[pos:pos + 1] == b"#":
            while pos < len(buf) and buf[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < len(buf) and not buf[pos:pos + 1].isspace():
            pos += 1
        tokens.append(buf[start:pos])
    pos += 1  # exactly one whitespace char after maxval
    magic = tokens[0].decode()
    cols, rows, maxval = int(tokens[1]), int(tokens[2]), int(tokens[3])
    return magic, cols, rows, maxval, pos


def dims_from_name(path):
    """Try to get (rows, cols, bpp, endian) from names like 'xxx_u16be-1x2046x2048.raw'.
    Returns None if the pattern is not found."""
    m = re.search(r"u(8|16)(be|le)?-1x(\d+)x(\d+)", str(path))
    if not m:
        return None
    return int(m[3]), int(m[4]), int(m[1]), (m[2] or "be")


# ---------------------------------------------------------------- read
def read_image(path, rows=None, cols=None, bpp=None, endian=None):
    """Read a RAW/PGM/PPM image.
    For RAW, rows and cols are needed (bpp defaults to 8, endian to 'be').
    If they are missing, we try to read them from the file name."""
    with _open(path, "rb") as f:
        buf = f.read()

    if buf[:2] in (b"P5", b"P6"):
        magic, c, r, maxval, off = _read_header(buf)
        b = 8 if maxval < 256 else 16
        nch = 1 if magic == "P5" else 3
        dtype = np.uint8 if b == 8 else np.dtype(">u2")   # 16-bit PNM is always big-endian
        n = r * c * nch
        data = np.frombuffer(buf, dtype=dtype, count=n, offset=off)
        data = data.astype(np.uint16) if b == 16 else data.copy()
        shape = (r, c) if nch == 1 else (r, c, 3)
        return Image(data.reshape(shape), "pgm" if nch == 1 else "ppm", b, maxval, "be")

    if buf[:1] == b"P":
        raise ValueError(f"unsupported PNM type {buf[:2]!r} (only binary P5/P6)")

    # ---- RAW
    if rows is None or cols is None:
        guess = dims_from_name(path)
        if guess is None:
            raise ValueError("RAW image: give --rows and --cols (and --bpp 16 --be/--le if needed)")
        rows, cols, gb, ge = guess
        bpp = bpp or gb
        endian = endian or ge
        print(f"[imgio] RAW size taken from file name: rows={rows} cols={cols} bpp={bpp} {endian}")
    bpp = bpp or 8
    endian = endian or "be"
    if bpp == 8:
        dtype = np.uint8
    elif bpp == 16:
        dtype = np.dtype(">u2" if endian == "be" else "<u2")
    else:
        raise ValueError("bpp must be 8 or 16")
    n = rows * cols
    if len(buf) < n * (bpp // 8):
        raise ValueError(f"RAW file too small: expected {n * (bpp // 8)} bytes, got {len(buf)}")
    data = np.frombuffer(buf, dtype=dtype, count=n).astype(np.uint8 if bpp == 8 else np.uint16)
    return Image(data.reshape(rows, cols), "raw", bpp, (1 << bpp) - 1, endian)


# ---------------------------------------------------------------- write
def write_image(path, img, fmt=None, maxval=None, endian=None):
    """Write img (an Image) as RAW/PGM/PPM. fmt defaults to img.fmt.
    maxval overrides the header maxval (PGM/PPM). endian only matters for 16 bpp RAW."""
    fmt = fmt or img.fmt
    maxval = img.maxval if maxval is None else maxval
    endian = endian or img.endian
    data = img.data

    if fmt == "ppm" and data.ndim != 3:
        raise ValueError("PPM output needs a color image")
    if fmt == "pgm" and data.ndim != 2:
        raise ValueError("PGM output needs a grayscale image")

    if fmt in ("pgm", "ppm"):
        wide = maxval > 255
        body = data.astype(">u2" if wide else np.uint8).tobytes()
        magic = "P5" if fmt == "pgm" else "P6"
        header = f"{magic}\n{data.shape[1]} {data.shape[0]}\n{maxval}\n".encode()
        out = header + body
    elif fmt == "raw":
        if img.bpp == 8:
            out = data.astype(np.uint8).tobytes()
        else:
            out = data.astype(">u2" if endian == "be" else "<u2").tobytes()
    else:
        raise ValueError(f"unknown format {fmt}")

    with _open(path, "wb") as f:
        f.write(out)


# ---------------------------------------------------------------- CLI helpers
def add_raw_args(parser):
    """Add the RAW options (--rows --cols --bpp --be/--le) to an argparse parser."""
    g = parser.add_argument_group("RAW options")
    g.add_argument("-rows", "--rows", type=int, help="number of rows (RAW)")
    g.add_argument("-cols", "--cols", type=int, help="number of columns (RAW)")
    g.add_argument("-bpp", "--bpp", type=int, choices=(8, 16), help="bits per pixel (RAW, default 8)")
    e = g.add_mutually_exclusive_group()
    e.add_argument("-be", "--be", dest="endian", action="store_const", const="be", help="big-endian (16 bpp RAW, default)")
    e.add_argument("-le", "--le", dest="endian", action="store_const", const="le", help="little-endian (16 bpp RAW)")


def read_from_args(path, args):
    return read_image(path, args.rows, args.cols, args.bpp, args.endian)