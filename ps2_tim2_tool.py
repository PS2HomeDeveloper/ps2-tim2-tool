#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════════════════╗
║            PS2 TIM2 Converter  —  ps2_tim2_tool.py                   ║
║   Convert any image (PNG, JPG, BMP, TGA, WebP …) → TIM2 (.tm2)     ║
║   Formats : 4-bit | 8-bit | 16-bit | 32-bit                        ║
╚══════════════════════════════════════════════════════════════════════╝

Usage:
  python3 ps2_tim2_tool.py <image(s)> --format <fmt> [options]
  python3 ps2_tim2_tool.py <file.tm2> --info
  python3 ps2_tim2_tool.py <file.tm2> --verify
  python3 ps2_tim2_tool.py --list convert.txt --format <fmt>

Examples:
  python3 ps2_tim2_tool.py hero.png    --format 32bit
  python3 ps2_tim2_tool.py bg.jpg      --format 16bit
  python3 ps2_tim2_tool.py sprite.bmp  --format 8bit  --dither
  python3 ps2_tim2_tool.py icon.tga    --format 4bit
  python3 ps2_tim2_tool.py *.png *.jpg --format 32bit
  python3 ps2_tim2_tool.py tex.png     --format 32bit --no-premult
  python3 ps2_tim2_tool.py img.png     --format 8bit  --output out.tm2
  python3 ps2_tim2_tool.py font1.tm2   --info
  python3 ps2_tim2_tool.py --list textures.txt
  python3 ps2_tim2_tool.py font1.tm2   --extract png
  python3 ps2_tim2_tool.py             --list-formats
"""

import argparse
import struct
import sys
import math
import warnings
from pathlib import Path
from PIL import Image

# ─── Suppress internal Pillow warnings (getdata is deprecated in Pillow 14) ───
warnings.filterwarnings("ignore", category=DeprecationWarning)


# ══════════════════════════════════════════════════════════════════════════════
#  Accepted input image formats  (conversion to TIM2)
# ══════════════════════════════════════════════════════════════════════════════
#
#  Total: 14 formats
#  ┌─────────────────────────────────────────────────────────────────────┐
#  │  .png   — Portable Network Graphics       (common, supports Alpha)│
#  │  .jpg   — JPEG                            (common, no Alpha)      │
#  │  .jpeg  — JPEG (alternate extension)                              │
#  │  .bmp   — Windows Bitmap                  (uncompressed)          │
#  │  .tga   — Targa                           (common in PS2 games)  │
#  │  .tiff  — Tagged Image File Format        (high quality)          │
#  │  .tif   — TIFF (alternate extension)                              │
#  │  .webp  — WebP                            (modern compression)    │
#  │  .gif   — Graphics Interchange Format     (only first frame is converted)│
#  │  .ppm   — Portable Pixmap                 (uncompressed)          │
#  │  .pgm   — Portable Graymap                (grayscale)             │
#  │  .pbm   — Portable Bitmap                 (black and white)       │
#  │  .ico   — Windows Icon                    (largest frame is converted)│
#  │  .dds   — DirectDraw Surface              (textures directly)     │
#  └─────────────────────────────────────────────────────────────────────┘
SUPPORTED_EXTENSIONS = {
    '.png', '.jpg', '.jpeg', '.bmp', '.tga', '.tiff', '.tif',
    '.webp', '.gif', '.ppm', '.pgm', '.pbm', '.ico', '.dds',
}


# ══════════════════════════════════════════════════════════════════════════════
#  TIM2 constants  —  taken directly from Sony's specifications
# ══════════════════════════════════════════════════════════════════════════════

TIM2_MAGIC   = b'TIM2'
TIM2_VERSION = 0x04
TIM2_ALIGN   = 128          # Every Picture Block is aligned to 128 bytes

# ─── Image Type  (byte 18 in Picture Block Header) ─────────────────────────
IMG_RGBA32   = 0x00         # Full 32-bit RGBA
IMG_RGBA16   = 0x01         # 16-bit RGBA5551
IMG_INDEXED8 = 0x05         # 8-bit indexed
IMG_INDEXED4 = 0x06         # 4-bit indexed

# ─── CLUT Type  (byte 17 in Picture Block Header) ─────────────────────────
CLUT_NONE   = 0x00          # No CLUT  (32-bit and 16-bit)
CLUT_RGBA32 = 0x02          # CLUT in RGBA32 format

# ─── GS Pixel Storage Modes  (from official GS specifications) ────────────
GS_PSM_CT32 = 0x00          # 32-bit RGBA8888
GS_PSM_CT16 = 0x02          # 16-bit RGBA5551
GS_PSM_T8   = 0x13          # 8-bit indexed
GS_PSM_T4   = 0x14          # 4-bit indexed

# ─── GS CLUT Pixel Storage Mode ──────────────────────────────────────────────
GS_CPSM_CT32 = 0x00         # CLUT in 32-bit format (default)


# ══════════════════════════════════════════════════════════════════════════════
#  Core helper functions
# ══════════════════════════════════════════════════════════════════════════════

def align_up(size: int, alignment: int) -> int:
    """Round size up to the nearest multiple of alignment."""
    return (size + alignment - 1) & ~(alignment - 1)


def ps2_alpha(a: int) -> int:
    """
    Convert Alpha from the standard range (0-255) to the PS2 range (0-0x80).
    ┌──────────────────────────────────────────────────────┐
    │  PS2 uses 0x80 (128) as full alpha, not 0xFF                       │
    │  Correct formula:  round(a × 128 / 255)                            │
    └──────────────────────────────────────────────────────┘
    """
    return round(a * 128 / 255)


def rgba_to_16bit(r: int, g: int, b: int, a: int) -> int:
    """
    Convert RGBA8888 → RGBA5551 in PS2 format (Little Endian).
    ┌─────────────────────────────────────┐
    │  Bit order:  A(1) B(5) G(5) R(5)                                    │
    └─────────────────────────────────────┘
    """
    r5 = (r >> 3) & 0x1F
    g5 = (g >> 3) & 0x1F
    b5 = (b >> 3) & 0x1F
    a1 = 1 if a >= 128 else 0
    return (a1 << 15) | (b5 << 10) | (g5 << 5) | r5


def open_any_image(path: Path) -> Image.Image:
    """
    Open any supported image format.
    Raises ValueError clearly if the extension is unsupported.
    """
    ext = path.suffix.lower()
    if ext not in SUPPORTED_EXTENSIONS:
        supported = ', '.join(sorted(SUPPORTED_EXTENSIONS))
        raise ValueError(
            f"Unsupported file format '{ext}'.\n"
            f"  Supported: {supported}"
        )
    return Image.open(path)


# ══════════════════════════════════════════════════════════════════════════════
#  --info  —  Read and display info from an existing TIM2 file
# ══════════════════════════════════════════════════════════════════════════════

# ─── Image Type → readable name table ──────────────────────────────────────
_IMG_TYPE_NAME = {
    0x00: '32-bit RGBA8888',
    0x01: '16-bit RGBA5551',
    0x05: '8-bit  Indexed (256 colors)',
    0x06: '4-bit  Indexed (16  colors)',
}

# ─── CLUT Type → readable name table ───────────────────────────────────────
_CLUT_TYPE_NAME = {
    0x00: 'none',
    0x01: 'RGBA5551',
    0x02: 'RGBA8888',
}

def tim2_info(path: Path) -> None:
    """
    Reads a TIM2 file header and displays its information in detail.
    ┌────────────────────────────────────────────────────────────────────┐
    │  Reads:    File Header (16 bytes) + Picture Block Header (48 bytes)│
    │  Displays: format, dimensions, size, CLUT, GsTex0, power-of-2 status│
    └────────────────────────────────────────────────────────────────────┘
    """
    if not path.exists():
        raise FileNotFoundError(f"File not found: {path}")

    data = path.read_bytes()
    size_bytes = len(data)

    # ─── Verify Magic Number ───────────────────────────────────────────────
    if data[:4] != b'TIM2':
        raise ValueError(f"Not a valid TIM2 file: {path.name}")

    # ─── File Header (16 bytes) ───────────────────────────────────────────────
    version  = data[4]
    fmt_byte = data[5]
    num_pics = struct.unpack_from('<H', data, 6)[0]

    # ─── Picture Block Header (offset 16, size 48) ────────────────────────────
    off = 16
    block_total  = struct.unpack_from('<I', data, off +  0)[0]
    clut_size    = struct.unpack_from('<I', data, off +  4)[0]
    img_size     = struct.unpack_from('<I', data, off +  8)[0]
    hdr_size     = struct.unpack_from('<H', data, off + 12)[0]
    clut_colors  = struct.unpack_from('<H', data, off + 14)[0]
    mipmap_count = data[off + 16]
    clut_type    = data[off + 17]
    img_type     = data[off + 18]
    width        = struct.unpack_from('<H', data, off + 20)[0]
    height       = struct.unpack_from('<H', data, off + 22)[0]
    gs_tex0      = struct.unpack_from('<Q', data, off + 24)[0]
    gs_texclut   = struct.unpack_from('<I', data, off + 40)[0]

    # ─── Extract GsTex0 fields ─────────────────────────────────────────────
    tbp0 = (gs_tex0 >>  0) & 0x3FFF
    tbw  = (gs_tex0 >> 14) & 0x3F
    psm  = (gs_tex0 >> 20) & 0x3F
    tw   = (gs_tex0 >> 26) & 0xF
    th   = (gs_tex0 >> 30) & 0xF
    tcc  = (gs_tex0 >> 34) & 0x1
    tfx  = (gs_tex0 >> 35) & 0x3
    cbp  = (gs_tex0 >> 37) & 0x3FFF
    cpsm = (gs_tex0 >> 51) & 0xF
    csm  = (gs_tex0 >> 55) & 0x1
    cld  = (gs_tex0 >> 61) & 0x7

    # ─── Readable names ────────────────────────────────────────────────────
    fmt_name  = _IMG_TYPE_NAME.get(img_type,  f'Unknown (0x{img_type:02X})')
    clut_name = _CLUT_TYPE_NAME.get(clut_type, f'Unknown (0x{clut_type:02X})')
    w_ok = is_power_of_2(width)
    h_ok = is_power_of_2(height)
    pow2_status = 'yes' if (w_ok and h_ok) else 'NO  <-- non-power-of-2'

    sep = '─' * 52
    print(f"""
{sep}
  File        :  {path.name}
  File size   :  {size_bytes / 1024:.2f} KB  ({size_bytes} bytes)
{sep}
  TIM2 version:  0x{version:02X}
  Pictures    :  {num_pics}
  Mipmaps     :  {mipmap_count}
{sep}
  Format      :  {fmt_name}
  Width       :  {width} px
  Height      :  {height} px
  Power-of-2  :  {pow2_status}
  Image data  :  {img_size / 1024:.2f} KB  ({img_size} bytes)
{sep}
  CLUT type   :  {clut_name}
  CLUT colors :  {clut_colors if clut_colors > 0 else 'none'}
  CLUT size   :  {clut_size} bytes
{sep}
  GsTex0      :  0x{gs_tex0:016X}
    TBP0      :  0x{tbp0:04X}  (Texture Base Pointer)
    TBW       :  {tbw}         (Buffer width in 64px units)
    PSM       :  0x{psm:02X}     (Pixel Storage Mode)
    TW        :  {tw}          (log2 width)
    TH        :  {th}          (log2 height)
    TCC       :  {tcc}          (alpha: 1=enabled)
    TFX       :  {tfx}          (0=MODULATE)
    CBP       :  0x{cbp:04X}  (CLUT Base Pointer)
    CPSM      :  0x{cpsm:02X}     (CLUT Pixel Storage Mode)
    CSM       :  {csm}          (0=CSM1)
    CLD       :  {cld}          (CLUT load control)
{sep}""")


# ══════════════════════════════════════════════════════════════════════════════
#  --list  —  Batch conversion from a text file
# ══════════════════════════════════════════════════════════════════════════════

def read_list_file(list_path: Path) -> list:
    """
    Reads a text file containing image filenames and the format for each.
    ┌──────────────────────────────────────────────────────────────────┐
    │  Format per line:  <filename>  <format>                            │
    │  Example:                                                           │
    │      hero.png    32bit                                           │
    │      icon.bmp    4bit                                            │
    │      bg.jpg      16bit                                           │
    │      sprite.tga  8bit                                            │
    │                                                                  │
    │  Rules:                                                              │
    │  • Empty lines are ignored                                          │
    │  • A line starting with # is a comment and is ignored               │
    │  • Accepted format: 4bit | 8bit | 16bit | 32bit                     │
    └──────────────────────────────────────────────────────────────────┘
    """
    if not list_path.exists():
        raise FileNotFoundError(f"List file not found: {list_path}")

    valid_formats = set(FORMATS.keys())
    entries = []
    with open(list_path, 'r', encoding='utf-8') as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line:                   # Empty line — skip
                continue
            if line.startswith('#'):       # Comment — skip
                continue
            parts = line.split()
            if len(parts) < 2:
                raise ValueError(
                    f"Line {lineno}: missing format.  "
                    f"Expected: <filename> <format>  Got: '{line}'"
                )
            filename = parts[0]
            fmt      = parts[1].lower()
            if fmt not in valid_formats:
                raise ValueError(
                    f"Line {lineno}: unknown format '{fmt}'.  "
                    f"Valid: {', '.join(valid_formats)}"
                )
            entries.append((lineno, filename, fmt))
    return entries


# ══════════════════════════════════════════════════════════════════════════════
#  Power-of-2  —  Validating and correcting dimensions
# ══════════════════════════════════════════════════════════════════════════════

def is_power_of_2(n: int) -> bool:
    """Checks whether the number is a valid power of two (8, 16, 32, 64, 128, 256, 512 ...)."""
    return n > 0 and (n & (n - 1)) == 0


def prev_power_of_2(n: int) -> int:
    """Nearest power of two less than or equal to n."""
    if n <= 0:
        return 1
    p = 1
    while p * 2 <= n:
        p *= 2
    return p


def next_power_of_2(n: int) -> int:
    """Nearest power of two greater than or equal to n."""
    if n <= 0:
        return 1
    p = 1
    while p < n:
        p *= 2
    return p


def check_and_warn_dimensions(name: str, w: int, h: int) -> None:
    """
    Checks the image dimensions and prints a detailed warning if they are not powers of two.
    ┌──────────────────────────────────────────────────────────────────┐
    │  PS2 GS requires power-of-two dimensions (8, 16, 32, 64, 128 ...)  │
    │  Irregular dimensions may cause:                                    │
    │  • Texture distortion                                               │
    │  • Errors in TBW calculation inside GsTex0                          │
    │  • Undefined behavior on real hardware                             │
    └──────────────────────────────────────────────────────────────────┘
    """
    w_ok = is_power_of_2(w)
    h_ok = is_power_of_2(h)

    if w_ok and h_ok:
        return   # Dimensions are valid — no warning

    lines = [f"  WARNING: '{name}' has non-power-of-2 dimensions ({w}x{h})."]
    lines.append(  "           PS2 requires power-of-2 sizes for correct rendering.")
    lines.append(  "           Suggested alternatives:")

    # ─── Suggest regular dimensions for each invalid dimension ──────────────
    for dim_name, val, ok in (('width', w, w_ok), ('height', h, h_ok)):
        if not ok:
            lo = prev_power_of_2(val)
            hi = next_power_of_2(val)
            lines.append(f"             {dim_name:6s}: {val}  ->  scale down to {lo}  |  scale up to {hi}")

    lines.append(  "           Use --resize up   to scale up   to the next  power-of-2.")
    lines.append(  "           Use --resize down to scale down to the prev  power-of-2.")

    print('\n'.join(lines))


def resize_to_power_of_2(img: Image.Image, mode: str) -> Image.Image:
    """
    Resize the image to regular (power-of-two) dimensions.
    ┌─────────────────────────────────────────────────────┐
    │  mode='up'   → Round up    (next power-of-2)                        │
    │  mode='down' → Round down  (prev power-of-2)                        │
    │                                                     │
    │  Uses LANCZOS for the highest quality when resizing.                │
    └─────────────────────────────────────────────────────┘
    """
    w, h = img.size
    fn   = next_power_of_2 if mode == 'up' else prev_power_of_2
    nw   = fn(w)
    nh   = fn(h)

    if nw == w and nh == h:
        return img   # No resizing needed

    print(f"  Resized:  {w}x{h}  ->  {nw}x{nh}  ({mode})")
    return img.resize((nw, nh), Image.LANCZOS)


# ══════════════════════════════════════════════════════════════════════════════
#  GsTex0  —  Full computation of the 64-bit GS Register
# ══════════════════════════════════════════════════════════════════════════════

def compute_gs_tex0(width: int, height: int, psm: int,
                    cpsm: int = GS_CPSM_CT32, cbp: int = 0) -> int:
    """
    Builds the complete GsTex0 Register value (64-bit) per the GS specification.
    ┌──────────────────────────────────────────────────────────────┐
    │  Bit         Field    Description                                   │
    │  ─────────  ───────  ──────────────────────────────────────  │
    │  [13:0]     TBP0     Texture Base Pointer  (= 0 in the file)       │
    │  [19:14]    TBW      Buffer width in units of 64 pixels             │
    │  [25:20]    PSM      Pixel Storage Mode                      │
    │  [29:26]    TW       log2(width)  rounded up                       │
    │  [33:30]    TH       log2(height) rounded up                       │
    │  [34]       TCC      1 = uses Alpha channel                        │
    │  [36:35]    TFX      0 = MODULATE (default)                        │
    │  [50:37]    CBP      CLUT Base Pointer  (= 0 in the file)          │
    │  [54:51]    CPSM     CLUT Pixel Storage Mode                 │
    │  [55]       CSM      0 = CSM1 (default CLUT ordering)              │
    │  [60:56]    CSA      CLUT Entry Offset = 0                   │
    │  [63:61]    CLD      1 = load CLUT on first use                    │
    └──────────────────────────────────────────────────────────────┘
    Note: TBP0 and CBP are set at runtime → we set them to 0 in the file.
    """
    # ─── TBW: Buffer width based on PSM ────────────────────────────────────
    tbw_map = {
        GS_PSM_CT32: align_up(width,  64) // 64,
        GS_PSM_CT16: align_up(width,  64) // 64,
        GS_PSM_T8:   align_up(width, 128) // 64,
        GS_PSM_T4:   align_up(width, 256) // 64,
    }
    tbw = max(1, tbw_map.get(psm, align_up(width, 64) // 64))

    # ─── TW, TH: log2 of the dimensions (rounded to nearest power of two) ──
    tw = max(0, math.ceil(math.log2(max(width,  1))))
    th = max(0, math.ceil(math.log2(max(height, 1))))

    v  = 0
    v |= (0    & 0x3FFF) <<  0   # TBP0  = 0
    v |= (tbw  & 0x3F)   << 14   # TBW
    v |= (psm  & 0x3F)   << 20   # PSM
    v |= (tw   & 0xF)    << 26   # TW
    v |= (th   & 0xF)    << 30   # TH
    v |= (1    & 0x1)    << 34   # TCC   = 1  (Alpha enabled)
    v |= (0    & 0x3)    << 35   # TFX   = MODULATE
    v |= (cbp  & 0x3FFF) << 37   # CBP   = 0
    v |= (cpsm & 0xF)    << 51   # CPSM
    v |= (0    & 0x1)    << 55   # CSM   = CSM1
    v |= (0    & 0x1F)   << 56   # CSA   = 0
    v |= (1    & 0x7)    << 61   # CLD   = 1
    return v & 0xFFFFFFFFFFFFFFFF


def compute_gs_texclut(cbw: int = 1, cou: int = 0, cov: int = 0) -> int:
    """
    GsTexClut Register — specifies the CLUT location in VRAM.
    ┌──────────────────────────────────────────────────────┐
    │  CBW = 1  →  CLUT buffer width = 64 pixels                         │
    │  COU, COV = 0  →  no offset                                        │
    └──────────────────────────────────────────────────────┘
    """
    v  = (cbw & 0x3F)  <<  0
    v |= (cou & 0x3F)  <<  6
    v |= (cov & 0x3FF) << 12
    return v


# ══════════════════════════════════════════════════════════════════════════════
#  Alpha Premultiplication
# ══════════════════════════════════════════════════════════════════════════════

def premultiply_alpha(img: Image.Image) -> Image.Image:
    """
    Alpha Premultiplication:  R' = R×A/255 ,  G' = G×A/255 ,  B' = B×A/255
    ┌──────────────────────────────────────────────────────────────────────┐
    │  Why?                                                                │
    │  PS2 GS blends colors using the formula:                            │
    │      Output = Src × Src.A  +  Dst × (1 − Src.A)                     │
    │  Without premult, dark fringing appears around transparent shapes. │
    │                                                                      │
    │  When to disable it (--no-premult)?                                 │
    │  If you control the blend mode manually in the game's code.        │
    └──────────────────────────────────────────────────────────────────────┘
    """
    img = img.convert('RGBA')
    r, g, b, a = img.split()

    # ─── Internal function: multiplies every channel by Alpha ───────────────
    def _mul_channel(ch: Image.Image) -> Image.Image:
        ch_vals = list(ch.getdata())
        a_vals  = list(a.getdata())
        result  = [round(c * av / 255) for c, av in zip(ch_vals, a_vals)]
        out = Image.new('L', img.size)
        out.putdata(result)
        return out

    return Image.merge('RGBA', (_mul_channel(r), _mul_channel(g), _mul_channel(b), a))


# ══════════════════════════════════════════════════════════════════════════════
#  Quantization  —  Reducing colors for indexed images (4-bit and 8-bit)
# ══════════════════════════════════════════════════════════════════════════════

def quantize_best(img_rgba: Image.Image, num_colors: int,
                  use_dither: bool = False) -> Image.Image:
    """
    Reduce colors using MEDIANCUT with Floyd-Steinberg Dithering support.
    ┌──────────────────────────────────────────────────────────────────┐
    │  MEDIANCUT (method=1) is better than Octree because:                │
    │  • It distributes colors more evenly across the color space        │
    │  • It gives better visual results for natural images               │
    │                                                                  │
    │  Pillow requires RGB for MEDIANCUT → we convert temporarily and    │
    │  save Alpha separately, then merge it back into the CLUT later.    │
    └──────────────────────────────────────────────────────────────────┘
    """
    dither_mode = Image.Dither.FLOYDSTEINBERG if use_dither else Image.Dither.NONE

    # ─── Paste onto a white background since MEDIANCUT doesn't accept RGBA directly ──
    bg = Image.new('RGB', img_rgba.size, (255, 255, 255))
    bg.paste(img_rgba.convert('RGB'), mask=img_rgba.split()[3])
    return bg.quantize(colors=num_colors, method=1, dither=dither_mode)


def extract_alpha_per_index(img_rgba: Image.Image,
                             img_q:    Image.Image,
                             num_colors: int) -> dict:
    """
    Extract the true average Alpha for each color in the palette.
    ┌──────────────────────────────────────────────────────┐
    │  More accurate than a fixed value (0x80) for all colors:           │
    │  Computes the average from all pixels using this color.            │
    └──────────────────────────────────────────────────────┘
    """
    alpha_data = list(img_rgba.split()[3].getdata())
    index_data = list(img_q.getdata())

    # ─── Accumulate Alpha values for each color in the palette ───────────────
    color_alpha: dict = {}
    for px_idx, a in zip(index_data, alpha_data):
        color_alpha.setdefault(px_idx, []).append(a)

    return {
        i: round(sum(color_alpha[i]) / len(color_alpha[i]))
        if i in color_alpha else 255
        for i in range(num_colors)
    }


# ══════════════════════════════════════════════════════════════════════════════
#  8-bit CLUT Swizzle  —  Mandatory CSM1 ordering for PS2
# ══════════════════════════════════════════════════════════════════════════════

def clut8_swizzle(palette: list) -> list:
    """
    Reorders the 8-bit CLUT according to the PS2 CSM1 system.
    ┌──────────────────────────────────────────────────────────────┐
    │  The CLUT is split into blocks of 32 colors.                       │
    │  Each block = 4 stripes of 8 colors.                               │
    │  Original order:  stripe 0 , 1 , 2 , 3                             │
    │  PS2 order:       stripe 0 , 2 , 1 , 3                             │
    │                                                              │
    │  NOTE: without this swizzle, colors will appear wrong in-game.     │
    └──────────────────────────────────────────────────────────────┘
    """
    out = list(palette)
    for i in range(256):
        block      = i // 32
        inner      = i % 32
        stripe     = inner // 8
        pos        = inner % 8
        new_stripe = [0, 2, 1, 3][stripe]
        j = block * 32 + new_stripe * 8 + pos
        out[j] = palette[i]
    return out



# ══════════════════════════════════════════════════════════════════════════════
#  GS Texture Swizzle  —  Reorders image data for PS2 VRAM
# ══════════════════════════════════════════════════════════════════════════════

def _gs_swizzle_32bit(data: bytes, width: int, height: int) -> bytes:
    """
    GS VRAM Swizzle for PSMCT32 (32-bit).
    ┌──────────────────────────────────────────────────────────────────────┐
    │  PS2 VRAM is organized into Pages / Blocks / Columns / Pixels       │
    │                                                                      │
    │  PSMCT32 (32-bit):                                                   │
    │  • Page     = 64 × 32 pixels  =  8192 bytes                         │
    │  • Block    = 8  × 8  pixels  =  256  bytes  (32 blocks/page)       │
    │  • Column   = 8  × 2  pixels  =  64   bytes  (4 columns/block)      │
    │                                                                      │
    │  Block ordering within the page (32 blocks):                        │
    │   0  1  4  5  16 17 20 21                                            │
    │   2  3  6  7  18 19 22 23                                            │
    │   8  9  12 13 24 25 28 29                                            │
    │  10 11  14 15 26 27 30 31                                            │
    │                                                                      │
    │  Important note:                                                     │
    │  This swizzle is only used when uploading a texture directly to VRAM│
    │  Many homebrew loaders perform the swizzle themselves → only use   │
    │  --swizzle if the project's loader expects pre-swizzled data.       │
    └──────────────────────────────────────────────────────────────────────┘
    """
    # ─── Block ordering within the page for PSMCT32 ───────────────────────────
    BLOCK_ORDER_32 = [
         0,  1,  4,  5, 16, 17, 20, 21,
         2,  3,  6,  7, 18, 19, 22, 23,
         8,  9, 12, 13, 24, 25, 28, 29,
        10, 11, 14, 15, 26, 27, 30, 31,
    ]

    PAGE_W  = 64    # Page width in pixels
    PAGE_H  = 32    # Page height in pixels
    BLOCK_W = 8     # Block width in pixels
    BLOCK_H = 8     # Block height in pixels
    BPP     = 4     # bytes per pixel (32-bit)

    src  = bytearray(data)
    dst  = bytearray(len(data))

    for y in range(height):
        for x in range(width):
            # ─── Determine the page ───────────────────────────────────────────
            page_x = x // PAGE_W
            page_y = y // PAGE_H
            pages_w = max(1, width // PAGE_W)

            # ─── Position within the page ────────────────────────────────────
            px = x % PAGE_W
            py = y % PAGE_H

            # ─── Determine the block within the page ───────────────────────────
            bx = px // BLOCK_W
            by = py // BLOCK_H
            block_idx = by * (PAGE_W // BLOCK_W) + bx   # 0-31

            # ─── Actual position of the block in VRAM ──────────────────────────
            actual_block = BLOCK_ORDER_32[block_idx % 32]

            # ─── Pixel position within the block ────────────────────────────────
            col_x = px % BLOCK_W
            col_y = py % BLOCK_H

            # ─── Final position in VRAM ─────────────────────────────────────────
            page_offset  = (page_y * pages_w + page_x) * (PAGE_W * PAGE_H)
            block_offset = actual_block * (BLOCK_W * BLOCK_H)
            pixel_offset = col_y * BLOCK_W + col_x

            dst_idx = (page_offset + block_offset + pixel_offset) * BPP
            src_idx = (y * width + x) * BPP

            if dst_idx + BPP <= len(dst) and src_idx + BPP <= len(src):
                dst[dst_idx:dst_idx+BPP] = src[src_idx:src_idx+BPP]

    return bytes(dst)


def _gs_swizzle_16bit(data: bytes, width: int, height: int) -> bytes:
    """
    GS VRAM Swizzle for PSMCT16 (16-bit).
    ┌──────────────────────────────────────────────────────────────────────┐
    │  PSMCT16:                                                            │
    │  • Page  = 64 × 64 pixels  =  8192 bytes                            │
    │  • Block = 16 × 8 pixels   =  256  bytes  (64 blocks/page)          │
    │                                                                      │
    │  Block ordering (64 blocks):                                        │
    │   0  2  8  10  32 34 40 42                                           │
    │   1  3  9  11  33 35 41 43                                           │
    │   4  6  12 14  36 38 44 46                                           │
    │   5  7  13 15  37 39 45 47                                           │
    │  16 18  24 26  48 50 56 58                                           │
    │  17 19  25 27  49 51 57 59                                           │
    │  20 22  28 30  52 54 60 62                                           │
    │  21 23  29 31  53 55 61 63                                           │
    └──────────────────────────────────────────────────────────────────────┘
    """
    BLOCK_ORDER_16 = [
         0,  2,  8, 10, 32, 34, 40, 42,
         1,  3,  9, 11, 33, 35, 41, 43,
         4,  6, 12, 14, 36, 38, 44, 46,
         5,  7, 13, 15, 37, 39, 45, 47,
        16, 18, 24, 26, 48, 50, 56, 58,
        17, 19, 25, 27, 49, 51, 57, 59,
        20, 22, 28, 30, 52, 54, 60, 62,
        21, 23, 29, 31, 53, 55, 61, 63,
    ]

    PAGE_W  = 64
    PAGE_H  = 64
    BLOCK_W = 16
    BLOCK_H = 8
    BPP     = 2

    src = bytearray(data)
    dst = bytearray(len(data))

    for y in range(height):
        for x in range(width):
            page_x  = x // PAGE_W
            page_y  = y // PAGE_H
            pages_w = max(1, width // PAGE_W)

            px = x % PAGE_W
            py = y % PAGE_H

            bx = px // BLOCK_W
            by = py // BLOCK_H
            block_idx   = by * (PAGE_W // BLOCK_W) + bx
            actual_block = BLOCK_ORDER_16[block_idx % 64]

            col_x = px % BLOCK_W
            col_y = py % BLOCK_H

            page_offset  = (page_y * pages_w + page_x) * (PAGE_W * PAGE_H)
            block_offset = actual_block * (BLOCK_W * BLOCK_H)
            pixel_offset = col_y * BLOCK_W + col_x

            dst_idx = (page_offset + block_offset + pixel_offset) * BPP
            src_idx = (y * width + x) * BPP

            if dst_idx + BPP <= len(dst) and src_idx + BPP <= len(src):
                dst[dst_idx:dst_idx+BPP] = src[src_idx:src_idx+BPP]

    return bytes(dst)


def apply_gs_swizzle(pixel_data: bytes, width: int, height: int,
                     img_type: int) -> bytes:
    """
    Applies GS Texture Swizzle to image data based on the type.
    ┌──────────────────────────────────────────────────────┐
    │  32-bit → _gs_swizzle_32bit                          │
    │  16-bit → _gs_swizzle_16bit                          │
    │  8-bit  → no swizzle (normally applied by the loader)              │
    │  4-bit  → no swizzle (normally applied by the loader)              │
    └──────────────────────────────────────────────────────┘
    """
    if img_type == IMG_RGBA32:
        return _gs_swizzle_32bit(pixel_data, width, height)
    elif img_type == IMG_RGBA16:
        return _gs_swizzle_16bit(pixel_data, width, height)
    else:
        return pixel_data   # 8-bit and 4-bit: no change


# ══════════════════════════════════════════════════════════════════════════════
#  Mipmaps  —  Generating multiple resolution levels
# ══════════════════════════════════════════════════════════════════════════════

def generate_mipmaps(img: Image.Image) -> list:
    """
    Generates a full Mipmap chain from the original image.
    ┌──────────────────────────────────────────────────────────────────────┐
    │  Mipmap = multiple resolution levels for a texture:                 │
    │  Level 0: original image  (256×256)                                 │
    │  Level 1: half size       (128×128)                                 │
    │  Level 2: quarter size    (64×64)                                   │
    │  ...down to 1×1                                                     │
    │                                                                      │
    │  Why does it matter?                                                 │
    │  PS2 GS picks the appropriate resolution level based on the         │
    │  texture's distance from the camera. Without mipmaps, distant       │
    │  textures appear distorted or shimmer; with mipmaps, the transition │
    │                                                                      │
    │  Uses LANCZOS for the highest quality when downscaling.             │
    └──────────────────────────────────────────────────────────────────────┘
    """
    levels = [img]
    w, h   = img.size

    while w > 1 or h > 1:
        w = max(1, w // 2)
        h = max(1, h // 2)
        mip = img.resize((w, h), Image.LANCZOS)
        levels.append(mip)

    return levels


# ══════════════════════════════════════════════════════════════════════════════
#  Building the Picture Block
# ══════════════════════════════════════════════════════════════════════════════

def build_picture_block(pixel_data: bytes, clut_data: bytes,
                        width: int,  height: int,
                        bpp: int,    img_type: int,
                        clut_type: int, clut_colors: int,
                        psm: int,    cpsm: int,
                        use_swizzle: bool = False,
                        mip_levels: list = None) -> bytes:
    """
    Builds the TIM2 Picture Block (Header + data + padding).
    ┌──────────────────────────────────────────────────────────────┐
    │  TIM2 Picture Block Header  =  48 bytes                      │
    │  ──────────────────────────────────────────────────────────  │
    │  Offset  Size  Field                                         │
    │   0       4B   Total Block Size  (aligned to 128 bytes)            │
    │   4       4B   CLUT Data Size                                │
    │   8       4B   Image Data Size                               │
    │  12       2B   Header Size  =  48                            │
    │  14       2B   CLUT Colors Count                             │
    │  16       1B   Mipmap Count  =  1                            │
    │  17       1B   CLUT Type                                     │
    │  18       1B   Image Type                                    │
    │  19       1B   reserved  =  0                                │
    │  20       2B   Width                                         │
    │  22       2B   Height                                        │
    │  24       8B   GsTex0  (computed with full precision)              │
    │  32       8B   GsTex1  =  0  (nearest filtering)            │
    │  40       4B   GsTexClut                                     │
    │  44       4B   reserved  =  0                                │
    │                                                              │
    │  Then:  [ pixel_data ]  [ clut_data ]  [ padding ]                  │
    └──────────────────────────────────────────────────────────────┘
    """
    # ─── Apply GS Swizzle if requested ─────────────────────────────────────
    if use_swizzle:
        pixel_data = apply_gs_swizzle(pixel_data, width, height, img_type)

    # ─── Merge base level data with the Mipmap levels ──────────────────────
    mip_data      = b''
    mipmap_count  = 1
    if mip_levels:
        mipmap_count = len(mip_levels)
        mip_data     = b''.join(mip_levels)

    HEADER     = 48
    raw_size   = HEADER + len(pixel_data) + len(mip_data) + len(clut_data)
    total_size = align_up(raw_size, TIM2_ALIGN)
    padding    = total_size - raw_size

    gs_tex0    = compute_gs_tex0(width, height, psm, cpsm)
    gs_texclut = compute_gs_texclut(cbw=1) if clut_colors > 0 else 0

    hdr = bytearray(HEADER)
    struct.pack_into('<I', hdr,  0, total_size)
    struct.pack_into('<I', hdr,  4, len(clut_data))
    struct.pack_into('<I', hdr,  8, len(pixel_data) + len(mip_data))
    struct.pack_into('<H', hdr, 12, HEADER)
    struct.pack_into('<H', hdr, 14, clut_colors)
    hdr[16] = mipmap_count
    hdr[17] = clut_type
    hdr[18] = img_type
    hdr[19] = 0            # reserved
    struct.pack_into('<H', hdr, 20, width)
    struct.pack_into('<H', hdr, 22, height)
    struct.pack_into('<Q', hdr, 24, gs_tex0)
    struct.pack_into('<Q', hdr, 32, 0)           # GsTex1 = nearest filter
    struct.pack_into('<I', hdr, 40, gs_texclut)
    struct.pack_into('<I', hdr, 44, 0)           # reserved

    return bytes(hdr) + pixel_data + mip_data + clut_data + (b'\x00' * padding)


# ══════════════════════════════════════════════════════════════════════════════
#  Building the complete TIM2 file
# ══════════════════════════════════════════════════════════════════════════════

def build_tim2_file(blocks: list) -> bytes:
    """
    Builds a complete TIM2 file from a list of Picture Blocks.
    ┌──────────────────────────────────────────┐
    │  TIM2 File Header  =  16 bytes           │
    │  ──────────────────────────────────────  │
    │  0   4B  Magic  "TIM2"                   │
    │  4   1B  Version  0x04                   │
    │  5   1B  Format   0x00  (Linear)         │
    │  6   2B  Number of Pictures              │
    │  8   8B  Padding  (zeros)                │
    └──────────────────────────────────────────┘
    """
    hdr = struct.pack('<4sBBH8s',
        TIM2_MAGIC, TIM2_VERSION, 0x00, len(blocks), b'\x00' * 8)
    return hdr + b''.join(blocks)


# ══════════════════════════════════════════════════════════════════════════════
#  The four format converters
# ══════════════════════════════════════════════════════════════════════════════

def convert_32bit(img: Image.Image, premult: bool = True,
                  use_swizzle: bool = False, use_mipmaps: bool = False,
                  **_) -> bytes:
    """
    32-bit RGBA8888  —  highest quality, full 8-bit transparency.
    ┌──────────────────────────────────────────────────────────┐
    │  Best for: 3D textures, menu backgrounds,                          │
    │            anything needing pixel-perfect quality and transparency.│
    │                                                          │
    │  Alpha is converted from 0-255 → 0-0x80  (PS2 range).               │
    │  --swizzle   : applies GS VRAM swizzle to the data.                 │
    │  --mipmaps   : generates a full mipmap chain.                       │
    └──────────────────────────────────────────────────────────┘
    """
    if premult:
        img = premultiply_alpha(img)
    img = img.convert('RGBA')
    w, h = img.size
    raw  = img.tobytes()

    data = bytearray(len(raw))
    for i in range(0, len(raw), 4):
        data[i]   = raw[i]
        data[i+1] = raw[i+1]
        data[i+2] = raw[i+2]
        data[i+3] = ps2_alpha(raw[i+3])

    # ─── Generate Mipmap levels ────────────────────────────────────────────
    mip_levels = None
    if use_mipmaps:
        mip_imgs = generate_mipmaps(img)[1:]   # Level 0 is the original (already included)
        mip_levels = []
        for mip in mip_imgs:
            mip_rgba = mip.convert('RGBA')
            mip_raw  = mip_rgba.tobytes()
            mip_data = bytearray(len(mip_raw))
            for i in range(0, len(mip_raw), 4):
                mip_data[i]   = mip_raw[i]
                mip_data[i+1] = mip_raw[i+1]
                mip_data[i+2] = mip_raw[i+2]
                mip_data[i+3] = ps2_alpha(mip_raw[i+3])
            mip_levels.append(bytes(mip_data))

    return build_picture_block(
        bytes(data), b'', w, h,
        32, IMG_RGBA32, CLUT_NONE, 0,
        GS_PSM_CT32, GS_CPSM_CT32,
        use_swizzle=use_swizzle, mip_levels=mip_levels)


def convert_16bit(img: Image.Image, premult: bool = True,
                  use_swizzle: bool = False, use_mipmaps: bool = False,
                  **_) -> bytes:
    """
    16-bit RGBA5551  —  high quality, half the size of 32-bit.
    ┌──────────────────────────────────────────────────────────┐
    │  Alpha: a single bit only  (fully opaque or fully transparent).    │
    │  Best for: textures with no partial transparency, backgrounds.     │
    │  --swizzle : applies GS VRAM swizzle to the data.                   │
    │  --mipmaps : generates a full mipmap chain.                         │
    └──────────────────────────────────────────────────────────┘
    """
    if premult:
        img = premultiply_alpha(img)
    img = img.convert('RGBA')
    w, h = img.size
    raw  = img.tobytes()

    data = bytearray()
    for i in range(0, len(raw), 4):
        val = rgba_to_16bit(raw[i], raw[i+1], raw[i+2], raw[i+3])
        data += struct.pack('<H', val)

    # ─── Generate Mipmap levels ────────────────────────────────────────────
    mip_levels = None
    if use_mipmaps:
        mip_imgs = generate_mipmaps(img)[1:]
        mip_levels = []
        for mip in mip_imgs:
            mip_rgba = mip.convert('RGBA')
            mip_raw  = mip_rgba.tobytes()
            mip_data = bytearray()
            for i in range(0, len(mip_raw), 4):
                val = rgba_to_16bit(mip_raw[i], mip_raw[i+1],
                                    mip_raw[i+2], mip_raw[i+3])
                mip_data += struct.pack('<H', val)
            mip_levels.append(bytes(mip_data))

    return build_picture_block(
        bytes(data), b'', w, h,
        16, IMG_RGBA16, CLUT_NONE, 0,
        GS_PSM_CT16, GS_CPSM_CT32,
        use_swizzle=use_swizzle, mip_levels=mip_levels)


def convert_8bit(img: Image.Image, premult: bool = False,
                 use_dither: bool = False, **_) -> bytes:
    """
    8-bit Indexed  —  256 colors with CLUT.
    ┌──────────────────────────────────────────────────────────┐
    │  Best for: characters, environments, any texture with limited colors.│
    │                                                          │
    │  • MEDIANCUT to pick the best 256 colors                            │
    │  • Alpha extracted from the original image (averaged per color)     │
    │  • CLUT Swizzle mandatory for PS2 (CSM1)                            │
    └──────────────────────────────────────────────────────────┘
    """
    img_rgba = img.convert('RGBA')
    w, h = img_rgba.size

    img_q     = quantize_best(img_rgba, 256, use_dither)
    palette   = img_q.getpalette()
    avg_alpha = extract_alpha_per_index(img_rgba, img_q, 256)

    # ─── Build CLUT then apply Swizzle ──────────────────────────────────────
    clut_raw = [(palette[i*3], palette[i*3+1], palette[i*3+2], avg_alpha[i])
                for i in range(256)]
    clut_raw  = clut8_swizzle(clut_raw)

    clut_data = bytearray()
    for r, g, b, a in clut_raw:
        clut_data += bytes([r, g, b, ps2_alpha(a)])

    return build_picture_block(
        img_q.tobytes(), bytes(clut_data), w, h,
        8, IMG_INDEXED8, CLUT_RGBA32, 256,
        GS_PSM_T8, GS_CPSM_CT32)


def convert_4bit(img: Image.Image, premult: bool = False,
                 use_dither: bool = False, **_) -> bytes:
    """
    4-bit Indexed  —  16 colors, smallest size.
    ┌──────────────────────────────────────────────────────────┐
    │  Best for: icons, simple interfaces, UI elements.                  │
    │                                                          │
    │  Nibble packing:  lo nibble = pixel[i]                  │
    │                   hi nibble = pixel[i+1]                │
    └──────────────────────────────────────────────────────────┘
    """
    img_rgba = img.convert('RGBA')
    w, h = img_rgba.size

    img_q     = quantize_best(img_rgba, 16, use_dither)
    palette   = img_q.getpalette()
    avg_alpha = extract_alpha_per_index(img_rgba, img_q, 16)

    # ─── Build CLUT (16 colors, no Swizzle needed) ─────────────────────────
    clut_data = bytearray()
    for i in range(16):
        r = palette[i*3]
        g = palette[i*3+1]
        b = palette[i*3+2]
        clut_data += bytes([r, g, b, ps2_alpha(avg_alpha[i])])

    # ─── Nibble packing: two pixels in one byte ────────────────────────────
    indices    = list(img_q.getdata())
    pixel_data = bytearray()
    for i in range(0, len(indices), 2):
        lo = indices[i] & 0x0F
        hi = (indices[i+1] & 0x0F) if i+1 < len(indices) else 0
        pixel_data.append(lo | (hi << 4))

    return build_picture_block(
        bytes(pixel_data), bytes(clut_data), w, h,
        4, IMG_INDEXED4, CLUT_RGBA32, 16,
        GS_PSM_T4, GS_CPSM_CT32)


# ══════════════════════════════════════════════════════════════════════════════
#  Format table
# ══════════════════════════════════════════════════════════════════════════════

FORMATS = {
    '32bit': (convert_32bit, 'RGBA8888  | full color + 8-bit alpha  | best for textures'),
    '16bit': (convert_16bit, 'RGBA5551  | 32K colors + 1-bit alpha  | smaller size'),
    '8bit':  (convert_8bit,  'Indexed8  | 256 colors + CLUT         | characters, environments'),
    '4bit':  (convert_4bit,  'Indexed4  | 16  colors + CLUT         | icons, UI elements'),
}


# ══════════════════════════════════════════════════════════════════════════════
#  Command-line interface  —  all visible text is in English
# ══════════════════════════════════════════════════════════════════════════════



# ══════════════════════════════════════════════════════════════════════════════
#  --verify  —  Verifying TIM2 file integrity
# ══════════════════════════════════════════════════════════════════════════════

def verify_tm2(path: Path) -> bool:
    """
    Verifies the integrity of a TIM2 file and prints a detailed report.
    ┌──────────────────────────────────────────────────────────────────────┐
    │  Checks:                                                             │
    │  1. Magic Number  "TIM2"  (first 4 bytes)                            │
    │  2. Version  =  0x04                                                 │
    │  3. Total file size ≥ File Header + Picture Block Header             │
    │  4. Image Data size in the header = actual data size                │
    │  5. CLUT size in the header = actual data size                       │
    │  6. Header Size = 48                                                 │
    │  7. Image Type is a known value (0x00 / 0x01 / 0x05 / 0x06)         │
    │  8. Dimensions > 0                                                   │
    │  9. Alignment: Block Size is a multiple of 128                       │
    │  10. GsTex0: TW and TH are consistent with the dimensions            │
    │  11. CLUT colors: correct according to Image Type                    │
    │  12. File is not truncated (no missing data)                         │
    └──────────────────────────────────────────────────────────────────────┘
    Returns True if the file is valid, False if an issue was found.
    """
    sep  = '─' * 52
    ok   = True
    warns = []
    errors = []

    def chk(condition: bool, msg_ok: str, msg_fail: str, is_warn: bool = False):
        nonlocal ok
        if condition:
            print(f"  [PASS]  {msg_ok}")
        else:
            tag = "WARN" if is_warn else "FAIL"
            print(f"  [{tag}]  {msg_fail}")
            if is_warn:
                warns.append(msg_fail)
            else:
                errors.append(msg_fail)
                ok = False

    print(f"\n{sep}")
    print(f"  Verifying: {path.name}")
    print(sep)

    # ─── Read the file ─────────────────────────────────────────────────────
    if not path.exists():
        print(f"  [FAIL]  File not found: {path}")
        print(sep)
        return False

    data       = path.read_bytes()
    file_size  = len(data)
    MIN_SIZE   = 16 + 48   # File Header + Picture Block Header

    # ─── 1. Minimum file size ──────────────────────────────────────────────
    chk(file_size >= MIN_SIZE,
        f"File size OK  ({file_size} bytes)",
        f"File too small  ({file_size} bytes, minimum {MIN_SIZE})")

    if file_size < MIN_SIZE:
        print(f"  Cannot continue — file is too small.")
        print(sep)
        return False

    # ─── 2. Magic Number ─────────────────────────────────────────────────────
    magic = data[:4]
    chk(magic == b'TIM2',
        f"Magic Number OK  ({magic})",
        f"Invalid Magic Number  (got {magic!r}, expected b'TIM2')")

    # ─── 3. Version ──────────────────────────────────────────────────────────
    version = data[4]
    chk(version == 0x04,
        f"Version OK  (0x{version:02X})",
        f"Unexpected Version  (0x{version:02X}, expected 0x04)", is_warn=True)

    # ─── Read the Picture Block Header (offset 16) ─────────────────────────
    off         = 16
    block_total = struct.unpack_from('<I', data, off +  0)[0]
    clut_size   = struct.unpack_from('<I', data, off +  4)[0]
    img_size    = struct.unpack_from('<I', data, off +  8)[0]
    hdr_size    = struct.unpack_from('<H', data, off + 12)[0]
    clut_colors = struct.unpack_from('<H', data, off + 14)[0]
    mip_count   = data[off + 16]
    clut_type   = data[off + 17]
    img_type    = data[off + 18]
    width       = struct.unpack_from('<H', data, off + 20)[0]
    height      = struct.unpack_from('<H', data, off + 22)[0]
    gs_tex0     = struct.unpack_from('<Q', data, off + 24)[0]

    # ─── 4. Header Size ──────────────────────────────────────────────────────
    chk(hdr_size == 48,
        f"Header Size OK  ({hdr_size} bytes)",
        f"Unexpected Header Size  ({hdr_size}, expected 48)")

    # ─── 5. Image Type ───────────────────────────────────────────────────────
    valid_types = {IMG_RGBA32, IMG_RGBA16, IMG_INDEXED8, IMG_INDEXED4}
    type_name   = _IMG_TYPE_NAME.get(img_type, f"Unknown (0x{img_type:02X})")
    chk(img_type in valid_types,
        f"Image Type OK  ({type_name})",
        f"Unknown Image Type  (0x{img_type:02X})")

    # ─── 6. Dimensions > 0 ─────────────────────────────────────────────────
    chk(width > 0 and height > 0,
        f"Dimensions OK  ({width}x{height})",
        f"Invalid Dimensions  ({width}x{height})")

    # ─── 7. Power-of-2 ───────────────────────────────────────────────────────
    w_ok = is_power_of_2(width)
    h_ok = is_power_of_2(height)
    chk(w_ok and h_ok,
        f"Power-of-2 OK  ({width}x{height})",
        f"Non-power-of-2 dimensions  ({width}x{height})  — may cause rendering issues",
        is_warn=True)

    # ─── 8. File size vs Block Total ───────────────────────────────────────
    expected_file_size = 16 + block_total
    chk(file_size >= expected_file_size,
        f"File completeness OK  (expected {expected_file_size} bytes, got {file_size})",
        f"File appears truncated  (expected {expected_file_size} bytes, got {file_size})")

    # ─── 9. Block Size aligned to 128 ──────────────────────────────────────
    chk(block_total % 128 == 0,
        f"Block alignment OK  ({block_total} bytes, aligned to 128)",
        f"Block size not aligned to 128  ({block_total} bytes)")

    # ─── 10. Image Data size + CLUT + Header = Block Total ────────────────
    computed = hdr_size + img_size + clut_size
    expected_aligned = align_up(computed, 128)
    chk(block_total == expected_aligned,
        f"Block size consistent  (header+img+clut={computed}, aligned={block_total})",
        f"Block size mismatch  (computed {expected_aligned}, stored {block_total})")

    # ─── 11. CLUT colors according to Image Type ──────────────────────────
    expected_clut = {
        IMG_RGBA32:   0,
        IMG_RGBA16:   0,
        IMG_INDEXED8: 256,
        IMG_INDEXED4: 16,
    }
    exp_colors = expected_clut.get(img_type, -1)
    if exp_colors != -1:
        chk(clut_colors == exp_colors,
            f"CLUT colors OK  ({clut_colors})",
            f"CLUT colors mismatch  (got {clut_colors}, expected {exp_colors} for {type_name})")

    # ─── 12. GsTex0: TW and TH are consistent ──────────────────────────────
    tw = (gs_tex0 >> 26) & 0xF
    th = (gs_tex0 >> 30) & 0xF
    expected_tw = max(0, math.ceil(math.log2(max(width,  1))))
    expected_th = max(0, math.ceil(math.log2(max(height, 1))))
    chk(tw == expected_tw and th == expected_th,
        f"GsTex0 TW/TH OK  (TW={tw}, TH={th})",
        f"GsTex0 TW/TH mismatch  (stored TW={tw}/TH={th}, expected TW={expected_tw}/TH={expected_th})",
        is_warn=True)

    # ─── Final result ───────────────────────────────────────────────────────
    print(sep)
    if ok and not warns:
        print(f"  Result: VALID  —  all checks passed.")
    elif ok and warns:
        print(f"  Result: VALID with warnings  ({len(warns)} warning(s)).")
    else:
        print(f"  Result: INVALID  —  {len(errors)} error(s), {len(warns)} warning(s).")
    print(sep)

    return ok

# ══════════════════════════════════════════════════════════════════════════════
#  --extract  —  Reverse conversion: TIM2 → any image format
# ══════════════════════════════════════════════════════════════════════════════

# ══════════════════════════════════════════════════════════════════════════════
#  Supported output formats for extraction  (conversion from TIM2)
# ══════════════════════════════════════════════════════════════════════════════
#
#  Total: 9 formats
#  ┌─────────────────────────────────────────────────────────────────────┐
#  │  .png   — Best choice: preserves Alpha fully with no quality loss │
#  │  .jpg   — No Alpha (white background), quality 95, 4:4:4 subsampling│
#  │  .jpeg  — Same as JPG (alternate extension)                       │
#  │  .bmp   — No Alpha (white background), uncompressed               │
#  │  .tga   — Supports Alpha, common in PS2 game tools                │
#  │  .tiff  — Supports Alpha, high quality, lossless                  │
#  │  .tif   — Same as TIFF (alternate extension)                      │
#  │  .webp  — Supports Alpha, modern compression, small size          │
#  │  .ppm   — No Alpha (white background), uncompressed               │
#  └─────────────────────────────────────────────────────────────────────┘
#
#  Note: formats without Alpha (jpg, bmp, ppm) are composited onto a white
#        background to preserve the appearance of transparency as much as possible.
EXTRACT_EXTENSIONS = {
    '.png', '.jpg', '.jpeg', '.bmp', '.tga',
    '.tiff', '.tif', '.webp', '.ppm',
}


def _ps2_alpha_to_255(a: int) -> int:
    """
    Reverses Alpha conversion from the PS2 range (0-0x80) to the standard range (0-255).
    ┌──────────────────────────────────────────────────────┐
    │  0x80 (128) in PS2  =  255 in the standard range                    │
    │  Formula:  min(round(a × 255 / 128), 255)                           │
    └──────────────────────────────────────────────────────┘
    """
    return min(round(a * 255 / 128), 255)


def _16bit_to_rgba(val: int):
    """
    Convert RGBA5551 (16-bit PS2) → RGBA8888.
    ┌─────────────────────────────────────────────────────────────┐
    │  We use bit expansion: R5 → R8 by multiplying by 255/31 for accuracy│
    │  This is more accurate than a simple shift (r << 3) because it      │
    │  restores full values like 0x1F → 255 instead of 248                │
    └─────────────────────────────────────────────────────────────┘
    """
    r5 = (val >>  0) & 0x1F
    g5 = (val >>  5) & 0x1F
    b5 = (val >> 10) & 0x1F
    a1 = (val >> 15) & 0x01
    r = round(r5 * 255 / 31)
    g = round(g5 * 255 / 31)
    b = round(b5 * 255 / 31)
    a = 255 if a1 else 0
    return r, g, b, a


def _unswizzle_clut8(palette: list) -> list:
    """
    Reverses the 8-bit CLUT Swizzle (CSM1).
    ┌──────────────────────────────────────────────────────────────┐
    │  When read from the file, colors are ordered per PS2 (swizzled)    │
    │  We restore the original order by reversing clut8_swizzle           │
    │  PS2 order:    0,2,1,3  →  Original: 0,1,2,3                       │
    └──────────────────────────────────────────────────────────────┘
    """
    # We build a reverse table: for each PS2 position → the original position
    out = list(palette)
    for i in range(256):
        block      = i // 32
        inner      = i % 32
        stripe     = inner // 8
        pos        = inner % 8
        new_stripe = [0, 2, 1, 3][stripe]   # Same swizzle — the operation is symmetric
        j = block * 32 + new_stripe * 8 + pos
        out[i] = palette[j]
    return out


def extract_tm2(src_path: Path, out_ext: str) -> str:
    """
    Extracts an image from a TIM2 file with high accuracy.
    ┌──────────────────────────────────────────────────────────────────────┐
    │  Logic by image type:                                               │
    │                                                                      │
    │  32-bit RGBA8888:                                                    │
    │    • Read R,G,B,A directly                                          │
    │    • Reverse ps2_alpha: A = round(A_ps2 × 255 / 128)                │
    │    • No data loss                                                    │
    │                                                                      │
    │  16-bit RGBA5551:                                                    │
    │    • Bit expansion for accuracy: R5→R8 by multiplying by 255/31     │
    │    • Alpha: 1-bit only (0 or 255)                                   │
    │                                                                      │
    │  8-bit Indexed:                                                      │
    │    • Reverse the CLUT Swizzle first                                 │
    │    • Build the full palette with Alpha                              │
    │    • Draw the image from indices                                    │
    │                                                                      │
    │  4-bit Indexed:                                                      │
    │    • Unpack nibbles: lo nibble = pixel[i], hi = pixel[i+1]          │
    │    • Build a 16-color palette with Alpha                            │
    └──────────────────────────────────────────────────────────────────────┘
    """
    if not src_path.exists():
        raise FileNotFoundError(f"File not found: {src_path}")

    data = src_path.read_bytes()

    # ─── Verify Magic Number ───────────────────────────────────────────────
    if data[:4] != b'TIM2':
        raise ValueError(f"Not a valid TIM2 file: {src_path.name}")

    # ─── Read the Picture Block Header ─────────────────────────────────────
    off         = 16   # After File Header
    clut_size   = struct.unpack_from('<I', data, off +  4)[0]
    img_size    = struct.unpack_from('<I', data, off +  8)[0]
    hdr_size    = struct.unpack_from('<H', data, off + 12)[0]
    clut_colors = struct.unpack_from('<H', data, off + 14)[0]
    clut_type   = data[off + 17]
    img_type    = data[off + 18]
    width       = struct.unpack_from('<H', data, off + 20)[0]
    height      = struct.unpack_from('<H', data, off + 22)[0]

    # ─── Extract the image data and CLUT ───────────────────────────────────
    img_start  = off + hdr_size
    img_end    = img_start + img_size
    clut_start = img_end
    clut_end   = clut_start + clut_size

    img_data  = data[img_start:img_end]
    clut_data = data[clut_start:clut_end]

    # ─── Build the image according to its type ─────────────────────────────

    if img_type == IMG_RGBA32:
        # ── 32-bit RGBA8888 ───────────────────────────────────────────────────
        pixels = []
        for i in range(0, len(img_data), 4):
            r = img_data[i]
            g = img_data[i+1]
            b = img_data[i+2]
            a = _ps2_alpha_to_255(img_data[i+3])
            pixels.append((r, g, b, a))
        img = Image.new('RGBA', (width, height))
        img.putdata(pixels)

    elif img_type == IMG_RGBA16:
        # ── 16-bit RGBA5551 ───────────────────────────────────────────────────
        pixels = []
        for i in range(0, len(img_data), 2):
            val = struct.unpack_from('<H', img_data, i)[0]
            pixels.append(_16bit_to_rgba(val))
        img = Image.new('RGBA', (width, height))
        img.putdata(pixels)

    elif img_type == IMG_INDEXED8:
        # ── 8-bit Indexed ─────────────────────────────────────────────────────
        # Read CLUT (RGBA32 × 256 colors)
        raw_clut = []
        for i in range(0, clut_colors * 4, 4):
            r = clut_data[i]
            g = clut_data[i+1]
            b = clut_data[i+2]
            a = _ps2_alpha_to_255(clut_data[i+3])
            raw_clut.append((r, g, b, a))

        # ─── Reverse the Swizzle ────────────────────────────────────────────
        palette = _unswizzle_clut8(raw_clut)

        # ─── Build the image from indices ──────────────────────────────────
        pixels = [palette[idx] for idx in img_data[:width * height]]
        img = Image.new('RGBA', (width, height))
        img.putdata(pixels)

    elif img_type == IMG_INDEXED4:
        # ── 4-bit Indexed ─────────────────────────────────────────────────────
        # Read CLUT (RGBA32 × 16 colors)
        palette = []
        for i in range(0, clut_colors * 4, 4):
            r = clut_data[i]
            g = clut_data[i+1]
            b = clut_data[i+2]
            a = _ps2_alpha_to_255(clut_data[i+3])
            palette.append((r, g, b, a))

        # ─── Unpack nibbles ─────────────────────────────────────────────────
        indices = []
        for byte in img_data:
            indices.append(byte & 0x0F)         # lo nibble = first pixel
            indices.append((byte >> 4) & 0x0F)  # hi nibble = second pixel

        pixels = [palette[idx] for idx in indices[:width * height]]
        img = Image.new('RGBA', (width, height))
        img.putdata(pixels)

    else:
        raise ValueError(f"Unsupported image type in TIM2: 0x{img_type:02X}")

    # ─── Save the image in the requested format ─────────────────────────────
    dst_path = src_path.with_suffix(out_ext)

    # ─── Convert to RGB if the format doesn't support Alpha ───────────────
    no_alpha_fmts = {'.jpg', '.jpeg', '.bmp', '.ppm'}
    if out_ext.lower() in no_alpha_fmts:
        # Paste onto a white background to preserve the appearance of transparency
        bg = Image.new('RGB', img.size, (255, 255, 255))
        bg.paste(img, mask=img.split()[3])
        img = bg

    # ─── High quality for JPEG ────────────────────────────────────────────
    save_kwargs = {}
    if out_ext.lower() in {'.jpg', '.jpeg'}:
        save_kwargs['quality'] = 95
        save_kwargs['subsampling'] = 0   # 4:4:4 for highest quality

    img.save(dst_path, **save_kwargs)
    return str(dst_path)

BANNER = """\
╔══════════════════════════════════════════════════════════════════════╗
║            PS2 TIM2 Converter  —  ps2_tim2_tool.py                   ║
║     Convert any image → TIM2 (.tm2) for PlayStation 2              ║
║     GsTex0 accurate | Alpha Premult | MEDIANCUT | Dithering         ║
╚══════════════════════════════════════════════════════════════════════╝"""

SUPPORTED_EXT_STR = '  ' + ', '.join(sorted(SUPPORTED_EXTENSIONS))


def list_formats():
    print("Supported input formats:")
    print(SUPPORTED_EXT_STR)
    print()
    print("Output TIM2 formats:\n")
    for name, (_, desc) in FORMATS.items():
        print(f"  --format {name:<8}  {desc}")
    print()
    print("Options:")
    print("  --no-premult   Disable Alpha Premultiplication")
    print("  --dither       Enable Floyd-Steinberg Dithering (4bit and 8bit)")
    print("  --output / -o  Output file path (single file only)")
    print("  --output-dir   Output directory for all converted files")
    print("  --resize up    Resize non-power-of-2 images UP   to next power-of-2")
    print("  --resize down  Resize non-power-of-2 images DOWN to prev power-of-2")
    print("  --swizzle      Apply GS VRAM Swizzle to pixel data (32-bit and 16-bit)")
    print("  --mipmaps      Generate full mipmap chain stored in the TIM2 file")
    print()
    print("  --info             Read and display info from an existing .tm2 file")
    print("  --verify           Verify integrity of a .tm2 file (12 checks)")
    print("  --list <file.txt>  Convert images listed in a text file (filename + format per line)")
    print("  --extract <ext>    Extract TIM2 to image: png jpg bmp tga tiff webp ppm")
    print()
    print("Examples:")
    print("  python3 ps2_tim2_tool.py hero.png       --format 32bit")
    print("  python3 ps2_tim2_tool.py bg.jpg         --format 16bit")
    print("  python3 ps2_tim2_tool.py sprite.bmp     --format 8bit --dither")
    print("  python3 ps2_tim2_tool.py icon.tga       --format 4bit")
    print("  python3 ps2_tim2_tool.py *.png *.jpg    --format 32bit")
    print("  python3 ps2_tim2_tool.py font1.tm2      --info")
    print("  python3 ps2_tim2_tool.py --list tex.txt")
    print("  python3 ps2_tim2_tool.py font1.tm2      --extract png")
    print("  python3 ps2_tim2_tool.py font1.tm2      --extract bmp")
    print()


def convert_one(src: str, fmt: str, dst: str = None,
                premult: bool = True, dither: bool = False,
                resize: str = None, swizzle: bool = False,
                mipmaps: bool = False,
                output_dir: Path = None) -> str:
    fn, _ = FORMATS[fmt]
    src_path = Path(src)

    if not src_path.exists():
        raise FileNotFoundError(f"File not found: {src}")

    if dst:
        dst_path = Path(dst)
    elif output_dir:
        # ─── Output to a specified directory with the same filename ───────────
        dst_path = output_dir / src_path.with_suffix('.tm2').name
    else:
        dst_path = src_path.with_suffix('.tm2')

    flags    = (['premult'] if premult else []) + (['dither'] if dither else [])
    if resize:   flags.append(f'resize-{resize}')
    if swizzle:  flags.append('swizzle')
    if mipmaps:  flags.append('mipmaps')
    flag_str = f"  [{chr(44).join(flags)}]" if flags else ''
    print(f"  Converting: {src_path.name}  ->  {dst_path.name}  [{fmt}]{flag_str}", end=' ... ', flush=True)

    img  = open_any_image(src_path)
    w, h = img.size

    # ─── Irregular dimensions warning — always shown, conversion does not stop ───
    if not (is_power_of_2(w) and is_power_of_2(h)):
        print()
        check_and_warn_dimensions(src_path.name, w, h)
        if resize:
            img = resize_to_power_of_2(img, resize)
        print(f"  Output:     {dst_path.name}", end=' ... ', flush=True)

    block = fn(img, premult=premult, use_dither=dither, use_swizzle=swizzle, use_mipmaps=mipmaps)
    data  = build_tim2_file([block])
    dst_path.write_bytes(data)

    print(f"done  ({len(data) / 1024:.1f} KB)")
    return str(dst_path)


def main():
    print(BANNER)
    print()

    parser = argparse.ArgumentParser(
        description='Convert any image to TIM2 (.tm2) for PlayStation 2',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 ps2_tim2_tool.py hero.png       --format 32bit
  python3 ps2_tim2_tool.py bg.jpg         --format 16bit
  python3 ps2_tim2_tool.py sprite.bmp     --format 8bit  --dither
  python3 ps2_tim2_tool.py icon.tga       --format 4bit
  python3 ps2_tim2_tool.py *.png *.jpg    --format 32bit
  python3 ps2_tim2_tool.py tex.png        --format 32bit --no-premult
  python3 ps2_tim2_tool.py img.png        --format 8bit  --output out.tm2
  python3 ps2_tim2_tool.py tex100.png     --format 32bit --resize up
  python3 ps2_tim2_tool.py tex100.png     --format 32bit --resize down
  python3 ps2_tim2_tool.py font1.tm2      --info
  python3 ps2_tim2_tool.py --list tex.txt
  python3 ps2_tim2_tool.py                --list-formats
        """
    )
    parser.add_argument('images',          nargs='*',
                        help='Input image files (PNG, JPG, BMP, TGA, ...) or .tm2 with --info')
    parser.add_argument('--format', '-f',  choices=list(FORMATS.keys()),
                        help='Output format: 4bit | 8bit | 16bit | 32bit')
    parser.add_argument('--output', '-o',  help='Output file path (single file only)')
    parser.add_argument('--output-dir',   metavar='DIR',
                        help='Output directory for all converted files')
    parser.add_argument('--no-premult',    action='store_true',
                        help='Disable Alpha Premultiplication')
    parser.add_argument('--dither',        action='store_true',
                        help='Enable Floyd-Steinberg Dithering (4bit and 8bit)')
    parser.add_argument('--resize',        choices=['up', 'down'],
                        help='Resize to power-of-2: up=scale up, down=scale down')
    parser.add_argument('--swizzle',       action='store_true',
                        help='Apply GS VRAM Swizzle (32-bit and 16-bit only)')
    parser.add_argument('--mipmaps',       action='store_true',
                        help='Generate full mipmap chain (32-bit and 16-bit)')
    parser.add_argument('--info',          action='store_true',
                        help='Read and display info from a .tm2 file (no conversion)')
    parser.add_argument('--verify',        action='store_true',
                        help='Verify integrity of a .tm2 file')
    parser.add_argument('--extract',       metavar='EXT',
                        help='Extract TIM2 to image format: png jpg bmp tga tiff webp ppm')
    parser.add_argument('--list',          metavar='FILE.TXT',
                        help='Text file with image filenames to convert (one per line)')
    parser.add_argument('--list-formats', '-l', action='store_true',
                        help='List available formats and options')
    args = parser.parse_args()

    # ─── --extract mode: extract TIM2 → image ───────────────────────────────
    if args.extract:
        ext = args.extract.lower()
        if not ext.startswith('.'):
            ext = '.' + ext
        if ext not in EXTRACT_EXTENSIONS:
            supported = ', '.join(sorted(e.lstrip('.') for e in EXTRACT_EXTENSIONS))
            print(f'ERROR: unsupported extract format "{args.extract}".  Supported: {supported}')
            sys.exit(1)
        if not args.images:
            print('ERROR: provide one or more .tm2 files with --extract')
            sys.exit(1)
        ok, fail = 0, 0
        outputs  = []
        print(f'Extracting {len(args.images)} file(s) -> [{ext}]\n')
        for src in args.images:
            src_path = Path(src)
            dst_path = src_path.with_suffix(ext)
            print(f'  Extracting: {src_path.name}  ->  {dst_path.name}', end=' ... ', flush=True)
            try:
                out = extract_tm2(src_path, ext)
                outputs.append(out)
                ok += 1
                size_kb = Path(out).stat().st_size / 1024
                print(f'done  ({size_kb:.1f} KB)')
            except Exception as e:
                print(f'FAILED  ({e})')
                fail += 1
        print(f'\n{"─" * 52}')
        print(f'Done: {ok} succeeded,  {fail} failed')
        if outputs:
            print('\nOutput files:')
            for o in outputs:
                print(f'  {o}')
        return

    # ─── --verify mode: verify TIM2 file integrity ─────────────────────────
    if args.verify:
        if not args.images:
            print('ERROR: provide one or more .tm2 files with --verify')
            sys.exit(1)
        all_ok = True
        for src in args.images:
            result = verify_tm2(Path(src))
            if not result:
                all_ok = False
        print()
        if all_ok:
            print('All files verified successfully.')
        else:
            print('One or more files failed verification.')
            sys.exit(1)
        return

    # ─── --info mode: display TIM2 file information ────────────────────────
    if args.info:
        if not args.images:
            print('ERROR: provide a .tm2 file with --info')
            sys.exit(1)
        for src in args.images:
            try:
                tim2_info(Path(src))
            except Exception as e:
                print(f'  FAILED: {src}  ({e})')
        return

    # ─── --list mode: read filenames from a text file ──────────────────────
    if args.list:
        list_path = Path(args.list)
        try:
            entries = read_list_file(list_path)
        except Exception as e:
            print(f'ERROR reading list file: {e}')
            sys.exit(1)
        if not entries:
            print(f'WARNING: list file is empty or has no valid entries: {list_path}')
            return
        premult    = not args.no_premult
        dither     = args.dither
        resize     = args.resize
        swizzle    = args.swizzle
        mipmaps    = args.mipmaps
        output_dir = None
        if args.output_dir:
            output_dir = Path(args.output_dir)
            output_dir.mkdir(parents=True, exist_ok=True)
        print(f'Reading list: {list_path.name}  ({len(entries)} entries)\n')
        ok, fail = 0, 0
        outputs  = []
        for lineno, filename, fmt in entries:
            try:
                out = convert_one(filename, fmt, None, premult, dither, resize, swizzle, mipmaps, output_dir)
                outputs.append(out)
                ok += 1
            except Exception as e:
                print(f'  FAILED (line {lineno}): {filename}  ({e})')
                fail += 1
        print(f'\n{"─" * 52}')
        print(f'Done: {ok} succeeded,  {fail} failed')
        if outputs:
            print('\nOutput files:')
            for o in outputs:
                print(f'  {o}')
        return

    # ─── Default mode: convert files from the command line ─────────────────
    if args.list_formats or not args.images:
        list_formats()
        return

    if not args.format:
        print('ERROR: --format is required.  Choose: 4bit | 8bit | 16bit | 32bit\n')
        list_formats()
        sys.exit(1)

    premult    = not args.no_premult
    dither     = args.dither
    resize     = args.resize
    swizzle    = args.swizzle
    mipmaps    = args.mipmaps
    out_arg    = args.output if len(args.images) == 1 else None
    output_dir = None

    if args.output_dir:
        output_dir = Path(args.output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)

    if args.output and len(args.images) > 1:
        print('WARNING: --output is ignored when converting multiple files.\n')
    if args.output and args.output_dir:
        print('WARNING: --output-dir is ignored when --output is specified.\n')
        output_dir = None

    print(f'Converting {len(args.images)} file(s) -> [{args.format}]\n')

    ok, fail = 0, 0
    outputs  = []
    for src in args.images:
        try:
            out = convert_one(src, args.format, out_arg, premult, dither, resize, swizzle, mipmaps, output_dir)
            outputs.append(out)
            ok += 1
        except Exception as e:
            print(f'  FAILED: {src}  ({e})')
            fail += 1

    print(f'\n{"─" * 52}')
    print(f'Done: {ok} succeeded,  {fail} failed')
    if outputs:
        print('\nOutput files:')
        for o in outputs:
            print(f'  {o}')


if __name__ == '__main__':
    main()
