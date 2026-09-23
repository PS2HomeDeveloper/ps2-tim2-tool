/*
 * ps2_tim2_tool.c - self-contained PS2 TIM2 image converter
 *
 * Reimplementation of ps2_tim2_tool.py using libpng, libjpeg, giflib,
 * libtiff and libwebp. BMP/TGA/PNM/ICO/DDS codecs are implemented here.
 * No external converter or child process is used.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <dirent.h>

#include <png.h>
#include <jpeglib.h>
#include <gif_lib.h>
#include <tiffio.h>
#include <webp/decode.h>
#include <webp/encode.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TIM_ID 0x00000010u
#define PMODE_4BIT 0
#define PMODE_8BIT 1
#define PMODE_16BIT 2
#define PMODE_24BIT 3
#define CF_CLUT_PRESENT 0x8u

static char g_error[1024];
static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "fatal: out of memory (%zu bytes)\n", n);
        exit(2);
    }
    return p;
}
static void *xcalloc(size_t n, size_t s) {
    if (s && n > SIZE_MAX / s) {
        fprintf(stderr, "fatal: allocation overflow\n");
        exit(2);
    }
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(2);
    }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "fatal: out of memory (%zu bytes)\n", n);
        exit(2);
    }
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t rds32(const uint8_t *p) {
    return (int32_t)rd32(p);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static int align_up(int n, int m) {
    return ((n + m - 1) / m) * m;
}
static bool is_power2(int n) {
    return n > 0 && (n & (n - 1)) == 0;
}
static int prev_power2(int n) {
    int p = 1;
    if (n <= 0)
        return 1;
    while (p <= INT_MAX / 2 && p * 2 <= n)
        p *= 2;
    return p;
}
static int next_power2(int n) {
    int p = 1;
    if (n <= 0)
        return 1;
    while (p < n && p <= INT_MAX / 2)
        p *= 2;
    return p;
}

static bool checked_pixels(int w, int h, size_t channels, size_t *out) {
    if (w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / (size_t)h ||
        (size_t)w * (size_t)h > SIZE_MAX / channels) {
        set_error("invalid or excessive image dimensions: %dx%d", w, h);
        return false;
    }
    *out = (size_t)w * (size_t)h * channels;
    return true;
}

typedef struct {
    int w, h;
    uint8_t *rgba;
} Image;
static Image *image_new(int w, int h) {
    size_t n;
    if (!checked_pixels(w, h, 4, &n))
        return NULL;
    Image *im = xcalloc(1, sizeof(*im));
    im->w = w;
    im->h = h;
    im->rgba = xcalloc(n, 1);
    return im;
}
static void image_free(Image *im) {
    if (im) {
        free(im->rgba);
        free(im);
    }
}

static bool read_file(const char *path, uint8_t **data, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_error("cannot open '%s': %s", path, strerror(errno));
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        set_error("cannot seek '%s'", path);
        fclose(f);
        return false;
    }
    long z = ftell(f);
    if (z < 0) {
        set_error("cannot size '%s'", path);
        fclose(f);
        return false;
    }
    rewind(f);
    *data = xmalloc((size_t)z + 1);
    *len = (size_t)z;
    if (*len && fread(*data, 1, *len, f) != *len) {
        set_error("cannot read '%s'", path);
        free(*data);
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}
static bool write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        set_error("cannot create '%s': %s", path, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        set_error("cannot write '%s'", path);
    return ok;
}
static const char *path_ext(const char *p) {
    const char *slash = strrchr(p, '/'), *dot = strrchr(p, '.');
    return dot && (!slash || dot > slash) ? dot : "";
}
static const char *path_base(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static char *replace_ext(const char *p, const char *ext) {
    const char *dot = path_ext(p);
    size_t base = dot[0] ? (size_t)(dot - p) : strlen(p), ne = strlen(ext);
    char *r = xmalloc(base + ne + 1);
    memcpy(r, p, base);
    memcpy(r + base, ext, ne + 1);
    return r;
}
static char *join_output(const char *dir, const char *src, const char *ext) {
    const char *b = path_base(src), *dot = path_ext(b);
    size_t stem = dot[0] ? (size_t)(dot - b) : strlen(b);
    size_t nd = strlen(dir), ne = strlen(ext);
    char *r = xmalloc(nd + 1 + stem + ne + 1);
    memcpy(r, dir, nd);
    if (nd && dir[nd - 1] != '/')
        r[nd++] = '/';
    memcpy(r + nd, b, stem);
    memcpy(r + nd + stem, ext, ne + 1);
    return r;
}
static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool mkdir_p(const char *p) {
    char tmp[PATH_MAX];
    size_t n = strlen(p);
    if (n >= sizeof(tmp)) {
        set_error("path too long");
        return false;
    }
    memcpy(tmp, p, n + 1);
    if (!n)
        return true;
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') {
            *q = 0;
            if (mkdir(tmp, 0777) && errno != EEXIST) {
                set_error("mkdir '%s': %s", tmp, strerror(errno));
                return false;
            }
            *q = '/';
        }
    if (mkdir(tmp, 0777) && errno != EEXIST) {
        set_error("mkdir '%s': %s", tmp, strerror(errno));
        return false;
    }
    return true;
}

/* ---------- PNG ---------- */
static Image *load_png_memory(const uint8_t *buf, size_t len) {
    png_image p;
    memset(&p, 0, sizeof(p));
    p.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&p, buf, len)) {
        set_error("PNG: %s", p.message);
        return NULL;
    }
    if (p.width > INT_MAX || p.height > INT_MAX) {
        png_image_free(&p);
        set_error("PNG dimensions too large");
        return NULL;
    }
    p.format = PNG_FORMAT_RGBA;
    Image *im = image_new((int)p.width, (int)p.height);
    if (!im) {
        png_image_free(&p);
        return NULL;
    }
    if (!png_image_finish_read(&p, NULL, im->rgba, 0, NULL)) {
        set_error("PNG: %s", p.message);
        png_image_free(&p);
        image_free(im);
        return NULL;
    }
    png_image_free(&p);
    return im;
}
static Image *load_png(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    Image *im = load_png_memory(d, n);
    free(d);
    return im;
}

/* ---------- JPEG ---------- */
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
    char msg[JMSG_LENGTH_MAX];
} JpegErr;
static void jpeg_fail(j_common_ptr c) {
    JpegErr *e = (JpegErr *)c->err;
    (*c->err->format_message)(c, e->msg);
    longjmp(e->jump, 1);
}
static Image *load_jpeg(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_error("cannot open '%s'", path);
        return NULL;
    }
    struct jpeg_decompress_struct c;
    JpegErr e;
    Image *im = NULL;
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = jpeg_fail;
    if (setjmp(e.jump)) {
        set_error("JPEG: %s", e.msg);
        jpeg_destroy_decompress(&c);
        fclose(f);
        image_free(im);
        return NULL;
    }
    jpeg_create_decompress(&c);
    jpeg_stdio_src(&c, f);
    jpeg_read_header(&c, TRUE);
    c.out_color_space = JCS_RGB;
    jpeg_start_decompress(&c);
    if (c.output_width > INT_MAX || c.output_height > INT_MAX) {
        set_error("JPEG dimensions too large");
        jpeg_destroy_decompress(&c);
        fclose(f);
        return NULL;
    }
    im = image_new((int)c.output_width, (int)c.output_height);
    if (!im) {
        jpeg_destroy_decompress(&c);
        fclose(f);
        return NULL;
    }
    size_t row = (size_t)im->w * c.output_components;
    uint8_t *scan = xmalloc(row);
    while (c.output_scanline < c.output_height) {
        JSAMPROW rp = scan;
        jpeg_read_scanlines(&c, &rp, 1);
        size_t y = (size_t)c.output_scanline - 1;
        for (int x = 0; x < im->w; x++) {
            uint8_t *o = im->rgba + (y * (size_t)im->w + x) * 4;
            if (c.output_components == 1)
                o[0] = o[1] = o[2] = scan[x];
            else {
                o[0] = scan[x * c.output_components];
                o[1] = scan[x * c.output_components + 1];
                o[2] = scan[x * c.output_components + 2];
            }
            o[3] = 255;
        }
    }
    free(scan);
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    fclose(f);
    return im;
}

/* ---------- GIF (first frame) ---------- */
static Image *load_gif(const char *path) {
    int ec = 0;
    GifFileType *g = DGifOpenFileName(path, &ec);
    if (!g) {
        set_error("GIF open error %d", ec);
        return NULL;
    }
    if (DGifSlurp(g) != GIF_OK || g->ImageCount < 1) {
        set_error("GIF decode failed");
        DGifCloseFile(g, &ec);
        return NULL;
    }
    SavedImage *s = &g->SavedImages[0];
    int w = g->SWidth, h = g->SHeight;
    Image *im = image_new(w, h);
    if (!im) {
        DGifCloseFile(g, &ec);
        return NULL;
    }
    ColorMapObject *cm = s->ImageDesc.ColorMap ? s->ImageDesc.ColorMap : g->SColorMap;
    if (!cm) {
        set_error("GIF has no color map");
        image_free(im);
        DGifCloseFile(g, &ec);
        return NULL;
    }
    int trans = -1;
    GraphicsControlBlock cb;
    if (DGifSavedExtensionToGCB(g, 0, &cb) == GIF_OK)
        trans = cb.TransparentColor;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *o = im->rgba + ((size_t)y * w + x) * 4;
            o[0] = o[1] = o[2] = 0;
            o[3] = 0;
        }
    for (int y = 0; y < s->ImageDesc.Height; y++)
        for (int x = 0; x < s->ImageDesc.Width; x++) {
            int dx = s->ImageDesc.Left + x, dy = s->ImageDesc.Top + y;
            if (dx < 0 || dy < 0 || dx >= w || dy >= h)
                continue;
            int idx = s->RasterBits[(size_t)y * s->ImageDesc.Width + x];
            uint8_t *o = im->rgba + ((size_t)dy * w + dx) * 4;
            if (idx >= 0 && idx < cm->ColorCount) {
                o[0] = cm->Colors[idx].Red;
                o[1] = cm->Colors[idx].Green;
                o[2] = cm->Colors[idx].Blue;
                o[3] = (idx == trans) ? 0 : 255;
            }
        }
    DGifCloseFile(g, &ec);
    return im;
}

/* ---------- TIFF ---------- */
static Image *load_tiff(const char *path) {
    TIFF *t = TIFFOpen(path, "r");
    if (!t) {
        set_error("TIFF open failed");
        return NULL;
    }
    uint32_t w = 0, h = 0;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
    if (!w || !h || w > INT_MAX || h > INT_MAX) {
        set_error("bad TIFF dimensions");
        TIFFClose(t);
        return NULL;
    }
    size_t np = (size_t)w * h;
    if (np > SIZE_MAX / sizeof(uint32_t)) {
        set_error("TIFF too large");
        TIFFClose(t);
        return NULL;
    }
    uint32_t *r = xmalloc(np * sizeof(*r));
    if (!TIFFReadRGBAImageOriented(t, w, h, r, ORIENTATION_TOPLEFT, 0)) {
        set_error("TIFF decode failed");
        free(r);
        TIFFClose(t);
        return NULL;
    }
    Image *im = image_new((int)w, (int)h);
    if (im)
        for (size_t i = 0; i < np; i++) {
            im->rgba[i * 4] = TIFFGetR(r[i]);
            im->rgba[i * 4 + 1] = TIFFGetG(r[i]);
            im->rgba[i * 4 + 2] = TIFFGetB(r[i]);
            im->rgba[i * 4 + 3] = TIFFGetA(r[i]);
        }
    free(r);
    TIFFClose(t);
    return im;
}

/* ---------- WebP ---------- */
static Image *load_webp(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    int w, h;
    if (!WebPGetInfo(d, n, &w, &h)) {
        set_error("invalid WebP");
        free(d);
        return NULL;
    }
    Image *im = image_new(w, h);
    if (!im) {
        free(d);
        return NULL;
    }
    if (!WebPDecodeRGBAInto(d, n, im->rgba, (size_t)w * h * 4, w * 4)) {
        set_error("WebP decode failed");
        image_free(im);
        im = NULL;
    }
    free(d);
    return im;
}

/* ---------- Netpbm P1..P6 ---------- */
typedef struct {
    const uint8_t *p, *end;
} PnmScan;
static bool pnm_token(PnmScan *s, char *out, size_t cap) {
    while (s->p < s->end) {
        if (isspace(*s->p)) {
            s->p++;
            continue;
        }
        if (*s->p == '#') {
            while (s->p < s->end && *s->p != '\n')
                s->p++;
            continue;
        }
        break;
    }
    if (s->p >= s->end)
        return false;
    size_t n = 0;
    while (s->p < s->end && !isspace(*s->p) && *s->p != '#') {
        if (n + 1 < cap)
            out[n++] = (char)*s->p;
        s->p++;
    }
    out[n] = 0;
    return n > 0;
}
static bool pnm_int(PnmScan *s, int *v) {
    char t[64], *e;
    if (!pnm_token(s, t, sizeof(t)))
        return false;
    long x = strtol(t, &e, 10);
    if (*e || x < 0 || x > INT_MAX)
        return false;
    *v = (int)x;
    return true;
}
static Image *load_pnm(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    PnmScan s = {d, d + n};
    char magic[8];
    int w, h, maxv = 1;
    if (!pnm_token(&s, magic, sizeof(magic)) || strlen(magic) != 2 || magic[0] != 'P' ||
        magic[1] < '1' || magic[1] > '6' || !pnm_int(&s, &w) || !pnm_int(&s, &h)) {
        set_error("invalid PNM header");
        free(d);
        return NULL;
    }
    int type = magic[1] - '0';
    if (type != 1 && type != 4) {
        if (!pnm_int(&s, &maxv) || maxv <= 0 || maxv > 65535) {
            set_error("invalid PNM maxval");
            free(d);
            return NULL;
        }
    }
    Image *im = image_new(w, h);
    if (!im) {
        free(d);
        return NULL;
    }
    size_t np = (size_t)w * h;
    if (type <= 3) {
        for (size_t i = 0; i < np; i++) {
            int a = 0, b = 0, c = 0;
            if (type == 1) {
                if (!pnm_int(&s, &a))
                    goto bad;
            } else if (type == 2) {
                if (!pnm_int(&s, &a))
                    goto bad;
            } else {
                if (!pnm_int(&s, &a) || !pnm_int(&s, &b) || !pnm_int(&s, &c))
                    goto bad;
            }
            uint8_t *o = im->rgba + i * 4;
            if (type == 1)
                o[0] = o[1] = o[2] = (uint8_t)(a ? 0 : 255);
            else if (type == 2)
                o[0] = o[1] = o[2] = (uint8_t)((a * 255L + maxv / 2) / maxv);
            else {
                o[0] = (uint8_t)((a * 255L + maxv / 2) / maxv);
                o[1] = (uint8_t)((b * 255L + maxv / 2) / maxv);
                o[2] = (uint8_t)((c * 255L + maxv / 2) / maxv);
            }
            o[3] = 255;
        }
    } else {
        /* A binary PNM raster starts immediately after the single whitespace
           delimiter following the last header token. Skipping every whitespace
           byte would eat legitimate first pixels such as 0x0A and 0x20. */
        if (s.p < s.end && isspace((unsigned char)*s.p)) {
            uint8_t first = *s.p++;
            if (first == '\r' && s.p < s.end && *s.p == '\n')
                s.p++;
        }
        if (type == 4) {
            size_t stride = ((size_t)w + 7) / 8;
            if ((size_t)(s.end - s.p) < stride * (size_t)h)
                goto bad;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int bit = (s.p[(size_t)y * stride + x / 8] >> (7 - (x & 7))) & 1;
                    uint8_t *o = im->rgba + ((size_t)y * w + x) * 4;
                    o[0] = o[1] = o[2] = (uint8_t)(bit ? 0 : 255);
                    o[3] = 255;
                }
        } else {
            int chans = type == 6 ? 3 : 1, bps = maxv > 255 ? 2 : 1;
            size_t need = np * (size_t)chans * bps;
            if ((size_t)(s.end - s.p) < need)
                goto bad;
            const uint8_t *q = s.p;
            for (size_t i = 0; i < np; i++) {
                int v[3] = {0, 0, 0};
                for (int k = 0; k < chans; k++) {
                    v[k] = bps == 1 ? *q++ : ((int)q[0] << 8) | q[1];
                    q += bps == 2 ? 2 : 0;
                }
                uint8_t *o = im->rgba + i * 4;
                if (chans == 1)
                    o[0] = o[1] = o[2] = (uint8_t)((v[0] * 255L + maxv / 2) / maxv);
                else
                    for (int k = 0; k < 3; k++)
                        o[k] = (uint8_t)((v[k] * 255L + maxv / 2) / maxv);
                o[3] = 255;
            }
        }
    }
    free(d);
    return im;
bad:
    set_error("truncated or malformed PNM pixel data");
    image_free(im);
    free(d);
    return NULL;
}

/* ---------- BMP/DIB ---------- */
static uint8_t scale_mask(uint32_t v, uint32_t mask) {
    if (!mask)
        return 0;
    unsigned sh = 0, bits = 0;
    while (((mask >> sh) & 1u) == 0u && sh < 32)
        sh++;
    uint32_t m = mask >> sh;
    while (m & 1u) {
        bits++;
        m >>= 1;
    }
    uint32_t x = (v & mask) >> sh, maxv = bits >= 32 ? UINT32_MAX : ((1u << bits) - 1u);
    return maxv ? (uint8_t)((x * 255ULL + maxv / 2) / maxv) : 0;
}
static bool decode_bmp_rle(const uint8_t *src, size_t n, int w, int h, int bpp, uint8_t *idx) {
    size_t p = 0;
    int x = 0, y = 0;
    while (p < n && y < h) {
        uint8_t a = src[p++];
        if (a) {
            if (p >= n)
                return false;
            uint8_t v = src[p++];
            for (int k = 0; k < a; k++) {
                if (x < w && y < h)
                    idx[(size_t)y * w + x] = (bpp == 8 ? v : (uint8_t)((k & 1) ? v & 15 : v >> 4));
                x++;
            }
        } else {
            if (p >= n)
                return false;
            uint8_t cmd = src[p++];
            if (cmd == 0) {
                x = 0;
                y++;
            } else if (cmd == 1)
                return true;
            else if (cmd == 2) {
                if (p + 2 > n)
                    return false;
                x += src[p++];
                y += src[p++];
            } else {
                int count = cmd;
                if (bpp == 8) {
                    if (p + (size_t)count > n)
                        return false;
                    for (int k = 0; k < count; k++) {
                        if (x < w && y < h)
                            idx[(size_t)y * w + x] = src[p + k];
                        x++;
                    }
                    p += (size_t)count;
                    if (count & 1)
                        p++;
                } else {
                    size_t bytes = (size_t)(count + 1) / 2;
                    if (p + bytes > n)
                        return false;
                    for (int k = 0; k < count; k++) {
                        uint8_t v = src[p + k / 2];
                        if (x < w && y < h)
                            idx[(size_t)y * w + x] = (uint8_t)((k & 1) ? v & 15 : v >> 4);
                        x++;
                    }
                    p += bytes;
                    if (bytes & 1)
                        p++;
                }
            }
        }
    }
    return y >= h;
}
static Image *load_bmp_memory(const uint8_t *d, size_t n, bool dib_only) {
    size_t off = 0, hs_off = 0;
    if (!dib_only) {
        if (n < 14 || d[0] != 'B' || d[1] != 'M') {
            set_error("invalid BMP signature");
            return NULL;
        }
        off = rd32(d + 10);
        hs_off = 14;
    }
    if (n < hs_off + 4) {
        set_error("truncated BMP");
        return NULL;
    }
    uint32_t hs = rd32(d + hs_off);
    if (hs < 12 || hs_off + hs > n) {
        set_error("unsupported BMP/DIB header");
        return NULL;
    }
    int w = 0, hraw = 0, bpp = 0;
    uint32_t compression = 0, colors = 0;
    size_t palette_off = hs_off + hs;
    bool core = hs == 12;
    if (core) {
        w = rd16(d + hs_off + 4);
        hraw = rd16(d + hs_off + 6);
        bpp = rd16(d + hs_off + 10);
    } else {
        w = rds32(d + hs_off + 4);
        hraw = rds32(d + hs_off + 8);
        bpp = rd16(d + hs_off + 14);
        compression = rd32(d + hs_off + 16);
        colors = rd32(d + hs_off + 32);
    }
    if (w <= 0 || hraw == 0 || abs(hraw) > INT_MAX / 2) {
        set_error("bad BMP dimensions");
        return NULL;
    }
    bool top = hraw < 0;
    int h = abs(hraw);
    if (dib_only && off == 0) {
        size_t pals = (bpp <= 8 ? (colors ? colors : (1u << bpp)) : 0);
        off = palette_off + pals * (core ? 3 : 4);
    }
    if (off > n) {
        set_error("bad BMP pixel offset");
        return NULL;
    }
    uint32_t rm = 0, gm = 0, bm = 0, am = 0;
    if (compression == 3 || compression == 6) {
        size_t mo = hs >= 52 ? hs_off + 40 : hs_off + hs;
        if (mo + 12 > n) {
            set_error("truncated BMP masks");
            return NULL;
        }
        rm = rd32(d + mo);
        gm = rd32(d + mo + 4);
        bm = rd32(d + mo + 8);
        if ((hs >= 56 || compression == 6) && mo + 16 <= n)
            am = rd32(d + mo + 12);
        if (hs == 40)
            palette_off = mo + (compression == 6 ? 16 : 12);
    } else if (bpp == 16) {
        rm = 0x7c00;
        gm = 0x03e0;
        bm = 0x001f;
    } else if (bpp == 32) {
        rm = 0x00ff0000;
        gm = 0x0000ff00;
        bm = 0x000000ff;
        am = 0xff000000;
    }
    if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32)) {
        set_error("unsupported BMP bit depth %d", bpp);
        return NULL;
    }
    if (!(compression == 0 || compression == 3 || compression == 6 || compression == 1 ||
          compression == 2)) {
        set_error("unsupported BMP compression %u", compression);
        return NULL;
    }
    uint8_t pal[256][4];
    memset(pal, 0, sizeof(pal));
    if (bpp <= 8) {
        unsigned cnt = colors ? colors : (1u << bpp);
        if (cnt > 256)
            cnt = 256;
        size_t es = core ? 3 : 4;
        if (palette_off + (size_t)cnt * es > n) {
            set_error("truncated BMP palette");
            return NULL;
        }
        for (unsigned i = 0; i < cnt; i++) {
            pal[i][2] = d[palette_off + i * es];
            pal[i][1] = d[palette_off + i * es + 1];
            pal[i][0] = d[palette_off + i * es + 2];
            pal[i][3] = 255;
        }
    }
    Image *im = image_new(w, h);
    if (!im)
        return NULL;
    if (compression == 1 || compression == 2) {
        uint8_t *idx = xcalloc((size_t)w * h, 1);
        if (!decode_bmp_rle(d + off, n - off, w, h, bpp, idx)) {
            set_error("malformed BMP RLE");
            free(idx);
            image_free(im);
            return NULL;
        }
        for (int sy = 0; sy < h; sy++) {
            int dy = top ? sy : h - 1 - sy;
            for (int x = 0; x < w; x++) {
                uint8_t *o = im->rgba + ((size_t)dy * w + x) * 4;
                memcpy(o, pal[idx[(size_t)sy * w + x]], 4);
            }
        }
        free(idx);
        return im;
    }
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    if (stride * (size_t)h > n - off) {
        set_error("truncated BMP pixels");
        image_free(im);
        return NULL;
    }
    bool any_alpha = false;
    for (int sy = 0; sy < h; sy++) {
        int dy = top ? sy : h - 1 - sy;
        const uint8_t *row = d + off + (size_t)sy * stride;
        for (int x = 0; x < w; x++) {
            uint8_t *o = im->rgba + ((size_t)dy * w + x) * 4;
            if (bpp <= 8) {
                unsigned idx = bpp == 8   ? row[x]
                               : bpp == 4 ? ((x & 1) ? row[x / 2] & 15 : row[x / 2] >> 4)
                                          : ((row[x / 8] >> (7 - (x & 7))) & 1);
                memcpy(o, pal[idx], 4);
            } else if (bpp == 24) {
                o[2] = row[x * 3];
                o[1] = row[x * 3 + 1];
                o[0] = row[x * 3 + 2];
                o[3] = 255;
            } else {
                uint32_t v = bpp == 16 ? rd16(row + x * 2) : rd32(row + x * 4);
                o[0] = scale_mask(v, rm);
                o[1] = scale_mask(v, gm);
                o[2] = scale_mask(v, bm);
                o[3] = am ? scale_mask(v, am) : 255;
                if (o[3])
                    any_alpha = true;
            }
        }
    }
    if (bpp == 32 && am && !any_alpha)
        for (size_t i = 0; i < (size_t)w * h; i++)
            im->rgba[i * 4 + 3] = 255;
    return im;
}
static Image *load_bmp(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    Image *im = load_bmp_memory(d, n, false);
    free(d);
    return im;
}

/* ---------- TGA ---------- */
static Image *load_tga(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    if (n < 18) {
        set_error("truncated TGA");
        free(d);
        return NULL;
    }
    int idlen = d[0], cmaptype = d[1], type = d[2], cfirst = rd16(d + 3), clen = rd16(d + 5),
        cdepth = d[7], w = rd16(d + 12), h = rd16(d + 14), depth = d[16], desc = d[17];
    bool rle = type == 9 || type == 10 || type == 11;
    int base = rle ? type - 8 : type;
    if (!(base == 1 || base == 2 || base == 3) || w <= 0 || h <= 0) {
        set_error("unsupported TGA type/dimensions");
        free(d);
        return NULL;
    }
    size_t p = 18 + (size_t)idlen;
    if (p > n) {
        set_error("truncated TGA id");
        free(d);
        return NULL;
    }
    uint8_t pal[256][4];
    memset(pal, 0, sizeof(pal));
    if (cmaptype) {
        if (cfirst + clen > 256 ||
            !(cdepth == 15 || cdepth == 16 || cdepth == 24 || cdepth == 32)) {
            set_error("unsupported TGA palette");
            free(d);
            return NULL;
        }
        int cb = (cdepth + 7) / 8;
        if (p + (size_t)clen * cb > n) {
            set_error("truncated TGA palette");
            free(d);
            return NULL;
        }
        for (int i = 0; i < clen; i++) {
            const uint8_t *q = d + p + (size_t)i * cb;
            uint8_t *o = pal[cfirst + i];
            if (cb == 2) {
                uint16_t v = rd16(q);
                o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
                o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
                o[2] = (uint8_t)((v & 31) * 255 / 31);
                o[3] = (cdepth == 16 && !(v & 0x8000)) ? 0 : 255;
            } else {
                o[2] = q[0];
                o[1] = q[1];
                o[0] = q[2];
                o[3] = cb == 4 ? q[3] : 255;
            }
        }
        p += (size_t)clen * cb;
    }
    int bytes = (depth + 7) / 8;
    if (base == 1 && !(depth == 8 || depth == 16)) {
        set_error("unsupported TGA index depth");
        free(d);
        return NULL;
    }
    if (base == 2 && !(depth == 15 || depth == 16 || depth == 24 || depth == 32)) {
        set_error("unsupported TGA depth");
        free(d);
        return NULL;
    }
    if (base == 3 && !(depth == 8 || depth == 16)) {
        set_error("unsupported TGA gray depth");
        free(d);
        return NULL;
    }
    Image *im = image_new(w, h);
    if (!im) {
        free(d);
        return NULL;
    }
    size_t total = (size_t)w * h, pos = 0;
    bool top = !!(desc & 0x20), right = !!(desc & 0x10);
    uint8_t pix[4];
    while (pos < total) {
        int count = 1;
        bool repeat = false;
        if (rle) {
            if (p >= n)
                goto bad;
            uint8_t ph = d[p++];
            repeat = !!(ph & 0x80);
            count = (ph & 0x7f) + 1;
        }
        for (int j = 0; j < count && pos < total; j++) {
            if (!repeat || j == 0) {
                if (p + (size_t)bytes > n)
                    goto bad;
                memcpy(pix, d + p, bytes);
                p += (size_t)bytes;
            }
            int sx = (int)(pos % (size_t)w), sy = (int)(pos / (size_t)w);
            int x = right ? w - 1 - sx : sx, y = top ? sy : h - 1 - sy;
            uint8_t *o = im->rgba + ((size_t)y * w + x) * 4;
            if (base == 1) {
                unsigned idx = bytes == 1 ? pix[0] : rd16(pix);
                if (idx > 255)
                    goto bad;
                memcpy(o, pal[idx], 4);
            } else if (base == 3) {
                o[0] = o[1] = o[2] = pix[0];
                o[3] = bytes == 2 ? pix[1] : 255;
            } else if (bytes == 2) {
                uint16_t v = rd16(pix);
                /* TGA 15/16-bit words use A1R5G5B5 ordering. */
                o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
                o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
                o[2] = (uint8_t)((v & 31) * 255 / 31);
                o[3] = (depth == 16 && (desc & 15) && !(v & 0x8000)) ? 0 : 255;
            } else {
                o[2] = pix[0];
                o[1] = pix[1];
                o[0] = pix[2];
                o[3] = bytes == 4 ? pix[3] : 255;
            }
            pos++;
        }
    }
    free(d);
    return im;
bad:
    set_error("truncated or malformed TGA");
    free(d);
    image_free(im);
    return NULL;
}

/* ---------- ICO (PNG or DIB image; chooses largest entry) ---------- */
static Image *load_ico(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    if (n < 6 || rd16(d) != 0 || rd16(d + 2) != 1 || rd16(d + 4) == 0) {
        set_error("invalid ICO");
        free(d);
        return NULL;
    }
    unsigned count = rd16(d + 4);
    if (n < 6 + (size_t)count * 16) {
        set_error("truncated ICO directory");
        free(d);
        return NULL;
    }
    unsigned best = 0;
    uint64_t area = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *e = d + 6 + i * 16;
        uint64_t w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256, a = w * h;
        if (a > area) {
            area = a;
            best = i;
        }
    }
    const uint8_t *e = d + 6 + best * 16;
    size_t sz = rd32(e + 8), off = rd32(e + 12);
    if (off > n || sz > n - off) {
        set_error("bad ICO entry");
        free(d);
        return NULL;
    }
    Image *im = NULL;
    if (sz >= 8 && !memcmp(d + off, "\x89PNG\r\n\x1a\n", 8))
        im = load_png_memory(d + off, sz);
    else {
        uint8_t *copy = xmalloc(sz);
        memcpy(copy, d + off, sz);
        if (sz >= 12) {
            int32_t fullh = rds32(copy + 8);
            if (fullh > 1)
                wr32(copy + 8, (uint32_t)(fullh / 2));
        }
        im = load_bmp_memory(copy, sz, true);
        if (im) { /* DIB alpha may be accompanied by AND mask. */
            size_t hs = rd32(copy), bpp = hs >= 16 ? rd16(copy + 14) : 0;
            size_t pals = bpp <= 8 ? (1u << bpp) : 0, poff = hs + pals * 4,
                   row = (((size_t)im->w * bpp + 31) / 32) * 4,
                   and_off = poff + row * (size_t)im->h, size_and = ((size_t)im->w + 31) / 32 * 4;
            if (and_off + size_and * (size_t)im->h <= sz) {
                for (int y = 0; y < im->h; y++) {
                    const uint8_t *m = copy + and_off + (size_t)(im->h - 1 - y) * size_and;
                    for (int x = 0; x < im->w; x++)
                        if ((m[x / 8] >> (7 - (x & 7))) & 1)
                            im->rgba[((size_t)y * im->w + x) * 4 + 3] = 0;
                }
            }
        }
        free(copy);
    }
    free(d);
    return im;
}

/* ---------- DDS (uncompressed RGB(A), DXT1/DXT3/DXT5) ---------- */
static void dds_color(uint16_t v, uint8_t *c) {
    c[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
    c[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
    c[2] = (uint8_t)((v & 31) * 255 / 31);
    c[3] = 255;
}
static Image *load_dds(const char *path) {
    uint8_t *d;
    size_t n;
    if (!read_file(path, &d, &n))
        return NULL;
    if (n < 128 || memcmp(d, "DDS ", 4) || rd32(d + 4) != 124) {
        set_error("invalid DDS");
        free(d);
        return NULL;
    }
    int h = (int)rd32(d + 12), w = (int)rd32(d + 16);
    uint32_t pf = rd32(d + 80), four = rd32(d + 84), bpp = rd32(d + 88), rm = rd32(d + 92),
             gm = rd32(d + 96), bm = rd32(d + 100), am = rd32(d + 104);
    Image *im = image_new(w, h);
    if (!im) {
        free(d);
        return NULL;
    }
    const uint8_t *p = d + 128, *end = d + n;
    if (pf & 4) {
        int mode = four == 0x31545844u ? 1 : four == 0x33545844u ? 3 : four == 0x35545844u ? 5 : 0;
        if (!mode) {
            set_error("unsupported DDS FourCC");
            goto bad;
        }
        int block = mode == 1 ? 8 : 16;
        size_t need = (size_t)((w + 3) / 4) * ((h + 3) / 4) * block;
        if ((size_t)(end - p) < need) {
            set_error("truncated DDS blocks");
            goto bad;
        }
        for (int by = 0; by < h; by += 4)
            for (int bx = 0; bx < w; bx += 4) {
                uint8_t alpha[16];
                memset(alpha, 255, 16);
                const uint8_t *q = p;
                p += block;
                if (mode == 3) {
                    uint64_t a = 0;
                    for (int i = 0; i < 8; i++)
                        a |= (uint64_t)q[i] << (8 * i);
                    for (int i = 0; i < 16; i++)
                        alpha[i] = (uint8_t)(((a >> (i * 4)) & 15) * 17);
                    q += 8;
                } else if (mode == 5) {
                    uint8_t at[8], a0 = q[0], a1 = q[1];
                    at[0] = a0;
                    at[1] = a1;
                    if (a0 > a1) {
                        for (int i = 1; i <= 6; i++)
                            at[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
                    } else {
                        for (int i = 1; i <= 4; i++)
                            at[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
                        at[6] = 0;
                        at[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++)
                        bits |= (uint64_t)q[2 + i] << (8 * i);
                    for (int i = 0; i < 16; i++)
                        alpha[i] = at[(bits >> (i * 3)) & 7];
                    q += 8;
                }
                uint16_t c0 = rd16(q), c1 = rd16(q + 2);
                uint8_t c[4][4];
                dds_color(c0, c[0]);
                dds_color(c1, c[1]);
                if (c0 > c1 || mode != 1) {
                    for (int k = 0; k < 3; k++) {
                        c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k]) / 3);
                        c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k]) / 3);
                    }
                    c[2][3] = c[3][3] = 255;
                } else {
                    for (int k = 0; k < 3; k++)
                        c[2][k] = (uint8_t)((c[0][k] + c[1][k]) / 2);
                    c[2][3] = 255;
                    memset(c[3], 0, 4);
                }
                uint32_t bits = rd32(q + 4);
                for (int py = 0; py < 4; py++)
                    for (int px = 0; px < 4; px++) {
                        int x = bx + px, y = by + py;
                        if (x >= w || y >= h)
                            continue;
                        int i = py * 4 + px, ci = (bits >> (2 * i)) & 3;
                        uint8_t *o = im->rgba + ((size_t)y * w + x) * 4;
                        memcpy(o, c[ci], 4);
                        o[3] = (mode == 1 && ci == 3 && c0 <= c1) ? 0 : alpha[i];
                    }
            }
    } else if (pf & 0x40) {
        int bytes = (int)((bpp + 7) / 8);
        if (!(bpp == 16 || bpp == 24 || bpp == 32)) {
            set_error("unsupported uncompressed DDS depth");
            goto bad;
        }
        size_t row = ((size_t)w * bytes + 3) & ~3u;
        if ((size_t)(end - p) < row * (size_t)h) {
            set_error("truncated DDS pixels");
            goto bad;
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t v = 0;
                memcpy(&v, p + (size_t)y * row + (size_t)x * bytes, bytes);
                uint8_t *o = im->rgba + ((size_t)y * w + x) * 4;
                o[0] = scale_mask(v, rm);
                o[1] = scale_mask(v, gm);
                o[2] = scale_mask(v, bm);
                o[3] = am ? scale_mask(v, am) : 255;
            }
    } else {
        set_error("unsupported DDS pixel format");
        goto bad;
    }
    free(d);
    return im;
bad:
    image_free(im);
    free(d);
    return NULL;
}

static Image *load_image(const char *path) {
    const char *e = path_ext(path);
    if (!strcasecmp(e, ".png"))
        return load_png(path);
    if (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg"))
        return load_jpeg(path);
    if (!strcasecmp(e, ".gif"))
        return load_gif(path);
    if (!strcasecmp(e, ".tif") || !strcasecmp(e, ".tiff"))
        return load_tiff(path);
    if (!strcasecmp(e, ".webp"))
        return load_webp(path);
    if (!strcasecmp(e, ".bmp"))
        return load_bmp(path);
    if (!strcasecmp(e, ".tga"))
        return load_tga(path);
    if (!strcasecmp(e, ".ppm") || !strcasecmp(e, ".pgm") || !strcasecmp(e, ".pbm"))
        return load_pnm(path);
    if (!strcasecmp(e, ".ico"))
        return load_ico(path);
    if (!strcasecmp(e, ".dds"))
        return load_dds(path);
    set_error("unsupported input extension '%s'", e);
    return NULL;
}

/* ---------- image writers used by --extract ---------- */
static bool save_png(const char *path, const Image *im) {
    png_image p;
    memset(&p, 0, sizeof(p));
    p.version = PNG_IMAGE_VERSION;
    p.width = (png_uint_32)im->w;
    p.height = (png_uint_32)im->h;
    p.format = PNG_FORMAT_RGBA;
    if (!png_image_write_to_file(&p, path, 0, im->rgba, 0, NULL)) {
        set_error("PNG write: %s", p.message);
        return false;
    }
    return true;
}
static bool save_bmp(const char *path, const Image *im) {
    size_t row = ((size_t)im->w * 4 + 3) & ~3u, total = 14 + 40 + row * (size_t)im->h;
    uint8_t *d = xcalloc(total, 1);
    d[0] = 'B';
    d[1] = 'M';
    wr32(d + 2, (uint32_t)total);
    wr32(d + 10, 54);
    wr32(d + 14, 40);
    wr32(d + 18, (uint32_t)im->w);
    wr32(d + 22, (uint32_t)im->h);
    wr16(d + 26, 1);
    wr16(d + 28, 32);
    wr32(d + 34, (uint32_t)(row * (size_t)im->h));
    for (int y = 0; y < im->h; y++) {
        uint8_t *r = d + 54 + (size_t)(im->h - 1 - y) * row;
        for (int x = 0; x < im->w; x++) {
            const uint8_t *s = im->rgba + ((size_t)y * im->w + x) * 4;
            r[x * 4] = s[2];
            r[x * 4 + 1] = s[1];
            r[x * 4 + 2] = s[0];
            r[x * 4 + 3] = s[3];
        }
    }
    bool ok = write_file(path, d, total);
    free(d);
    return ok;
}
static bool save_tga(const char *path, const Image *im) {
    size_t n = 18 + (size_t)im->w * im->h * 4;
    uint8_t *d = xcalloc(n, 1);
    d[2] = 2;
    wr16(d + 12, (uint16_t)im->w);
    wr16(d + 14, (uint16_t)im->h);
    d[16] = 32;
    d[17] = 0x28;
    for (size_t i = 0; i < (size_t)im->w * im->h; i++) {
        d[18 + i * 4] = im->rgba[i * 4 + 2];
        d[19 + i * 4] = im->rgba[i * 4 + 1];
        d[20 + i * 4] = im->rgba[i * 4];
        d[21 + i * 4] = im->rgba[i * 4 + 3];
    }
    bool ok = write_file(path, d, n);
    free(d);
    return ok;
}
static bool save_ppm(const char *path, const Image *im) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        set_error("cannot create '%s'", path);
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
    bool ok = true;
    for (size_t i = 0; i < (size_t)im->w * im->h; i++)
        if (fwrite(im->rgba + i * 4, 1, 3, f) != 3) {
            ok = false;
            break;
        }
    if (fclose(f))
        ok = false;
    if (!ok)
        set_error("PPM write failed");
    return ok;
}
static bool save_tiff(const char *path, const Image *im) {
    TIFF *t = TIFFOpen(path, "w");
    if (!t) {
        set_error("TIFF create failed");
        return false;
    }
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, im->w);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, im->h);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(t, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    uint16_t extra = EXTRASAMPLE_UNASSALPHA;
    TIFFSetField(t, TIFFTAG_EXTRASAMPLES, 1, &extra);
    bool ok = true;
    for (int y = 0; y < im->h; y++)
        if (TIFFWriteScanline(t, im->rgba + (size_t)y * im->w * 4, y, 0) < 0) {
            ok = false;
            break;
        }
    TIFFClose(t);
    if (!ok)
        set_error("TIFF write failed");
    return ok;
}
static bool save_webp(const char *path, const Image *im) {
    uint8_t *out = NULL;
    size_t n = WebPEncodeLosslessRGBA(im->rgba, im->w, im->h, im->w * 4, &out);
    if (!n || !out) {
        set_error("WebP encode failed");
        return false;
    }
    bool ok = write_file(path, out, n);
    WebPFree(out);
    return ok;
}
static bool save_image(const char *path, const Image *im) {
    const char *e = path_ext(path);
    if (!strcasecmp(e, ".png"))
        return save_png(path, im);
    if (!strcasecmp(e, ".bmp"))
        return save_bmp(path, im);
    if (!strcasecmp(e, ".tga"))
        return save_tga(path, im);
    if (!strcasecmp(e, ".ppm"))
        return save_ppm(path, im);
    if (!strcasecmp(e, ".tif") || !strcasecmp(e, ".tiff"))
        return save_tiff(path, im);
    if (!strcasecmp(e, ".webp"))
        return save_webp(path, im);
    set_error("unsupported output extension '%s'", e);
    return false;
}

/* ========================================================================== */
/* IMAGE PROCESSING: alpha premultiplication, padding and Lanczos resampling   */
/* ========================================================================== */

static void premultiply_alpha(Image *im) {
    size_t np = (size_t)im->w * im->h;
    for (size_t i = 0; i < np; i++) {
        unsigned a = im->rgba[i * 4 + 3];
        /* Python round() and C round() differ on exact .5 ties.  Integer
           half-up is deterministic and differs only in rare tie cases. */
        im->rgba[i * 4] = (uint8_t)((im->rgba[i * 4] * a + 127) / 255);
        im->rgba[i * 4 + 1] = (uint8_t)((im->rgba[i * 4 + 1] * a + 127) / 255);
        im->rgba[i * 4 + 2] = (uint8_t)((im->rgba[i * 4 + 2] * a + 127) / 255);
    }
}

static double sinc_fn(double x) {
    if (fabs(x) < 1e-12)
        return 1.0;
    x *= 3.14159265358979323846;
    return sin(x) / x;
}
static double lanczos_kernel(double x) {
    x = fabs(x);
    if (x >= 3.0)
        return 0.0;
    return sinc_fn(x) * sinc_fn(x / 3.0);
}

/* Separable Lanczos-3.  When shrinking, the filter is widened by the inverse
   scale to perform proper low-pass filtering rather than merely sampling. */
static Image *resize_lanczos(Image *src, int nw, int nh) {
    if (nw <= 0 || nh <= 0) {
        set_error("resize dimensions must be positive");
        return NULL;
    }
    if (nw == src->w && nh == src->h) {
        Image *copy = image_new(nw, nh);
        if (copy)
            memcpy(copy->rgba, src->rgba, (size_t)nw * nh * 4);
        return copy;
    }
    size_t tmpn;
    if (!checked_pixels(nw, src->h, 4, &tmpn))
        return NULL;
    double *tmp = xcalloc(tmpn, sizeof(double));
    double sx = (double)nw / src->w;
    double support = sx < 1.0 ? 3.0 / sx : 3.0;
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < nw; x++) {
            double center = ((double)x + 0.5) / sx - 0.5;
            int lo = (int)floor(center - support + 1.0), hi = (int)floor(center + support);
            double sum = 0.0, v[4] = {0, 0, 0, 0};
            for (int q = lo; q <= hi; q++) {
                int xx = clampi(q, 0, src->w - 1);
                double dist = center - q;
                double wt = sx < 1.0 ? lanczos_kernel(dist * sx) * sx : lanczos_kernel(dist);
                sum += wt;
                const uint8_t *p = src->rgba + ((size_t)y * src->w + xx) * 4;
                for (int c = 0; c < 4; c++)
                    v[c] += p[c] * wt;
            }
            if (fabs(sum) < 1e-15)
                sum = 1.0;
            for (int c = 0; c < 4; c++)
                tmp[((size_t)y * nw + x) * 4 + c] = v[c] / sum;
        }
    Image *out = image_new(nw, nh);
    if (!out) {
        free(tmp);
        return NULL;
    }
    double sy = (double)nh / src->h;
    support = sy < 1.0 ? 3.0 / sy : 3.0;
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            double center = ((double)y + 0.5) / sy - 0.5;
            int lo = (int)floor(center - support + 1.0), hi = (int)floor(center + support);
            double sum = 0.0, v[4] = {0, 0, 0, 0};
            for (int q = lo; q <= hi; q++) {
                int yy = clampi(q, 0, src->h - 1);
                double dist = center - q;
                double wt = sy < 1.0 ? lanczos_kernel(dist * sy) * sy : lanczos_kernel(dist);
                sum += wt;
                for (int c = 0; c < 4; c++)
                    v[c] += tmp[((size_t)yy * nw + x) * 4 + c] * wt;
            }
            if (fabs(sum) < 1e-15)
                sum = 1.0;
            uint8_t *p = out->rgba + ((size_t)y * nw + x) * 4;
            for (int c = 0; c < 4; c++)
                p[c] = (uint8_t)clampi((int)floor(v[c] / sum + 0.5), 0, 255);
        }
    free(tmp);
    return out;
}

static bool parse_resize(const char *mode, int w, int h, int *nw, int *nh) {
    if (!mode) {
        *nw = w;
        *nh = h;
        return true;
    }
    if (!strcasecmp(mode, "up")) {
        *nw = next_power2(w);
        *nh = next_power2(h);
        return true;
    }
    if (!strcasecmp(mode, "down")) {
        *nw = prev_power2(w);
        *nh = prev_power2(h);
        return true;
    }
    char extra = 0;
    if (sscanf(mode, "%dx%d%c", nw, nh, &extra) == 2 && *nw > 0 && *nh > 0)
        return true;
    set_error("invalid --resize value '%s' (use up | down | WxH)", mode);
    return false;
}

/* ========================================================================== */
/* MEDIAN-CUT PALETTE GENERATION AND FLOYD-STEINBERG INDEX ASSIGNMENT          */
/* ========================================================================== */

/* The histogram stores 5-bit RGB bins.  It avoids pathological memory use on
   photographic sources while preserving the color-space partitioning expected
   from median-cut.  Counts weight both split medians and palette centroids. */
typedef struct {
    uint32_t count;
    uint64_t rsum, gsum, bsum;
    uint8_t r5, g5, b5;
} HistColor;

typedef struct {
    int begin, end;
    uint64_t weight;
    uint8_t rmin, rmax, gmin, gmax, bmin, bmax;
} ColorBox;

static int g_sort_channel = 0;
static int cmp_hist_color(const void *aa, const void *bb) {
    const HistColor *a = aa, *b = bb;
    int av = g_sort_channel == 0 ? a->r5 : g_sort_channel == 1 ? a->g5 : a->b5;
    int bv = g_sort_channel == 0 ? b->r5 : g_sort_channel == 1 ? b->g5 : b->b5;
    if (av != bv)
        return av - bv;
    if (a->r5 != b->r5)
        return a->r5 - b->r5;
    if (a->g5 != b->g5)
        return a->g5 - b->g5;
    return a->b5 - b->b5;
}
static void box_measure(ColorBox *b, HistColor *c) {
    b->weight = 0;
    b->rmin = b->gmin = b->bmin = 31;
    b->rmax = b->gmax = b->bmax = 0;
    for (int i = b->begin; i < b->end; i++) {
        if (c[i].r5 < b->rmin)
            b->rmin = c[i].r5;
        if (c[i].r5 > b->rmax)
            b->rmax = c[i].r5;
        if (c[i].g5 < b->gmin)
            b->gmin = c[i].g5;
        if (c[i].g5 > b->gmax)
            b->gmax = c[i].g5;
        if (c[i].b5 < b->bmin)
            b->bmin = c[i].b5;
        if (c[i].b5 > b->bmax)
            b->bmax = c[i].b5;
        b->weight += c[i].count;
    }
}
static int box_priority(const ColorBox *b) {
    int rr = b->rmax - b->rmin, gg = b->gmax - b->gmin, bb = b->bmax - b->bmin,
        range = rr > gg ? (rr > bb ? rr : bb) : (gg > bb ? gg : bb);
    return range * (int)(b->weight > INT_MAX ? INT_MAX : b->weight);
}

static int build_median_palette(const Image *im, int requested, uint8_t palette[256][3]) {
    typedef struct {
        uint32_t count;
        uint64_t r, g, b;
    } Bin;
    Bin *bins = xcalloc(32768, sizeof(*bins));
    size_t np = (size_t)im->w * im->h;
    for (size_t i = 0; i < np; i++) {
        const uint8_t *p = im->rgba + i * 4;
        unsigned a = p[3];
        /* Match Image.paste(..., mask=alpha): composite RGB over white before
           median-cut. Indexed converters deliberately ignore --no-premult. */
        uint8_t r = (uint8_t)((p[0] * a + 255u * (255u - a) + 127) / 255),
                g = (uint8_t)((p[1] * a + 255u * (255u - a) + 127) / 255),
                b = (uint8_t)((p[2] * a + 255u * (255u - a) + 127) / 255);
        unsigned key = ((unsigned)(r >> 3) << 10) | ((unsigned)(g >> 3) << 5) | (b >> 3);
        Bin *z = &bins[key];
        z->count++;
        z->r += r;
        z->g += g;
        z->b += b;
    }
    int used = 0;
    for (int i = 0; i < 32768; i++)
        if (bins[i].count)
            used++;
    HistColor *colors = xmalloc((size_t)(used ? used : 1) * sizeof(*colors));
    int ci = 0;
    for (int i = 0; i < 32768; i++)
        if (bins[i].count) {
            colors[ci].count = bins[i].count;
            colors[ci].rsum = bins[i].r;
            colors[ci].gsum = bins[i].g;
            colors[ci].bsum = bins[i].b;
            colors[ci].r5 = (uint8_t)((i >> 10) & 31);
            colors[ci].g5 = (uint8_t)((i >> 5) & 31);
            colors[ci].b5 = (uint8_t)(i & 31);
            ci++;
        }
    free(bins);
    memset(palette, 0, 256 * 3);
    if (!used) {
        free(colors);
        return 1;
    }
    int target = requested < used ? requested : used;
    ColorBox boxes[256];
    boxes[0] = (ColorBox){0, used, 0, 0, 0, 0, 0, 0, 0};
    box_measure(&boxes[0], colors);
    int nb = 1;
    while (nb < target) {
        int pick = -1, best = -1;
        for (int i = 0; i < nb; i++)
            if (boxes[i].end - boxes[i].begin > 1) {
                int p = box_priority(&boxes[i]);
                if (p > best) {
                    best = p;
                    pick = i;
                }
            }
        if (pick < 0)
            break;
        ColorBox old = boxes[pick];
        int rr = old.rmax - old.rmin, gg = old.gmax - old.gmin, bb = old.bmax - old.bmin;
        g_sort_channel = (rr >= gg && rr >= bb) ? 0 : (gg >= bb ? 1 : 2);
        qsort(colors + old.begin, (size_t)(old.end - old.begin), sizeof(*colors), cmp_hist_color);
        uint64_t half = (old.weight + 1) / 2, acc = 0;
        int split = old.begin + 1;
        for (int i = old.begin; i < old.end - 1; i++) {
            acc += colors[i].count;
            if (acc >= half) {
                split = i + 1;
                break;
            }
        }
        boxes[pick] = (ColorBox){old.begin, split, 0, 0, 0, 0, 0, 0, 0};
        boxes[nb] = (ColorBox){split, old.end, 0, 0, 0, 0, 0, 0, 0};
        box_measure(&boxes[pick], colors);
        box_measure(&boxes[nb], colors);
        nb++;
    }
    for (int i = 0; i < nb; i++) {
        uint64_t count = 0, rs = 0, gs = 0, bs = 0;
        for (int j = boxes[i].begin; j < boxes[i].end; j++) {
            count += colors[j].count;
            rs += colors[j].rsum;
            gs += colors[j].gsum;
            bs += colors[j].bsum;
        }
        if (count) {
            palette[i][0] = (uint8_t)((rs + count / 2) / count);
            palette[i][1] = (uint8_t)((gs + count / 2) / count);
            palette[i][2] = (uint8_t)((bs + count / 2) / count);
        }
    }
    free(colors);
    return nb;
}

static int nearest_palette(double r, double g, double b, const uint8_t pal[256][3], int n) {
    int best = 0;
    double bd = 1e100;
    for (int i = 0; i < n; i++) {
        double dr = r - pal[i][0], dg = g - pal[i][1],
               db = b - pal[i][2]; /* Luma-aware metric remains Euclidean enough to preserve
                                      median-cut intent. */
        double d = dr * dr + dg * dg + db * db;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static uint8_t *map_palette(const Image *im, const uint8_t pal[256][3], int n, bool dither,
                            uint64_t asum[256], uint32_t acount[256]) {
    size_t np = (size_t)im->w * im->h;
    uint8_t *idx = xmalloc(np);
    memset(asum, 0, 256 * sizeof(*asum));
    memset(acount, 0, 256 * sizeof(*acount));
    if (!dither) {
        for (size_t i = 0; i < np; i++) {
            const uint8_t *p = im->rgba + i * 4;
            double a = p[3] / 255.0;
            int k = nearest_palette(p[0] * a + 255.0 * (1.0 - a),
                                    p[1] * a + 255.0 * (1.0 - a),
                                    p[2] * a + 255.0 * (1.0 - a), pal, n);
            idx[i] = (uint8_t)k;
            asum[k] += p[3];
            acount[k]++;
        }
        return idx;
    }
    int w = im->w;
    double *er0 = xcalloc((size_t)(w + 2) * 3, sizeof(double)),
           *er1 = xcalloc((size_t)(w + 2) * 3, sizeof(double));
    for (int y = 0; y < im->h; y++) {
        memset(er1, 0, (size_t)(w + 2) * 3 * sizeof(double));
        bool lr = (y & 1) == 0;
        int start = lr ? 0 : w - 1, end = lr ? w : -1, step = lr ? 1 : -1;
        for (int x = start; x != end; x += step) {
            const uint8_t *p = im->rgba + ((size_t)y * w + x) * 4;
            double a = p[3] / 255.0;
            double r = clampd(p[0] * a + 255.0 * (1.0 - a) + er0[(x + 1) * 3], 0, 255),
                   g = clampd(p[1] * a + 255.0 * (1.0 - a) + er0[(x + 1) * 3 + 1], 0, 255),
                   b = clampd(p[2] * a + 255.0 * (1.0 - a) + er0[(x + 1) * 3 + 2], 0, 255);
            int k = nearest_palette(r, g, b, pal, n);
            idx[(size_t)y * w + x] = (uint8_t)k;
            asum[k] += p[3];
            acount[k]++;
            double e[3] = {r - pal[k][0], g - pal[k][1], b - pal[k][2]};
            int d = lr ? 1 : -1, f = x + 1 + d, back = x + 1 - d;
            for (int c = 0; c < 3; c++) {
                er0[f * 3 + c] += e[c] * 7 / 16;
                er1[back * 3 + c] += e[c] * 3 / 16;
                er1[(x + 1) * 3 + c] += e[c] * 5 / 16;
                er1[f * 3 + c] += e[c] * 1 / 16;
            }
        }
        double *t = er0;
        er0 = er1;
        er1 = t;
    }
    free(er0);
    free(er1);
    return idx;
}

/* Direct 16-bit channel dithering, matching the serpentine diffusion geometry
   of the Python implementation.  Output channels hold 5-bit values. */
static uint8_t *dither_direct_5bit(const Image *im) {
    int w = im->w;
    size_t np = (size_t)w * im->h;
    uint8_t *out = xmalloc(np * 4);
    double *cur = xcalloc((size_t)(w + 2) * 3, sizeof(double)),
           *next = xcalloc((size_t)(w + 2) * 3, sizeof(double));
    const double qstep = 255.0 / 31.0;
    for (int y = 0; y < im->h; y++) {
        memset(next, 0, (size_t)(w + 2) * 3 * sizeof(double));
        bool lr = (y & 1) == 0;
        int first = lr ? 0 : w - 1, last = lr ? w : -1, dx = lr ? 1 : -1;
        for (int x = first; x != last; x += dx) {
            const uint8_t *p = im->rgba + ((size_t)y * w + x) * 4;
            uint8_t *o = out + ((size_t)y * w + x) * 4;
            double adj[3];
            int q[3];
            for (int c = 0; c < 3; c++) {
                adj[c] = p[c] + cur[(x + 1) * 3 + c];
                q[c] = clampi((int)floor(adj[c] / qstep + 0.5), 0, 31);
                o[c] = (uint8_t)q[c];
            }
            o[3] = p[3];
            int f = x + 1 + dx, back = x + 1 - dx;
            for (int c = 0; c < 3; c++) {
                double e = adj[c] - q[c] * qstep;
                cur[f * 3 + c] += e * 7 / 16;
                next[back * 3 + c] += e * 3 / 16;
                next[(x + 1) * 3 + c] += e * 5 / 16;
                next[f * 3 + c] += e / 16;
            }
        }
        double *t = cur;
        cur = next;
        next = t;
    }
    free(cur);
    free(next);
    return out;
}

/* ========================================================================== */
/* PS2 TIM2 FORMAT, PACKING, SWIZZLE, PARSING, EXTRACTION, DIFF AND CLI       */
/* ========================================================================== */

#define TIM2_VERSION 0x04
#define TIM2_ALIGN 128
#define IMG_RGBA32 0x00
#define IMG_RGBA16 0x01
#define IMG_RGB24 0x02
#define IMG_INDEXED8 0x05
#define IMG_INDEXED4 0x06
#define IMG_INDEXED4_SONY 0x03
#define CLUT_NONE 0x00
#define CLUT_RGBA16 0x01
#define CLUT_RGBA32 0x02
#define GS_PSM_CT32 0x00
#define GS_PSM_CT16 0x02
#define GS_PSM_T8 0x13
#define GS_PSM_T4 0x14
#define GS_CPSM_CT32 0x00
#define PS2_TOOL_VERSION "1.0.0"

/* Growing binary byte buffer used by the TIM2 encoder. */
typedef struct { uint8_t *p; size_t n, cap; } Buffer;
static void buf_reserve(Buffer *b, size_t add) {
    if (add > SIZE_MAX - b->n) { fprintf(stderr, "fatal: buffer overflow\n"); exit(2); }
    size_t need = b->n + add;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    b->p = xrealloc(b->p, cap); b->cap = cap;
}
static void buf_put(Buffer *b, const void *p, size_t n) {
    buf_reserve(b, n); memcpy(b->p + b->n, p, n); b->n += n;
}
static void buf_zero(Buffer *b, size_t n) {
    buf_reserve(b, n); memset(b->p + b->n, 0, n); b->n += n;
}
static void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8); return v;
}

static unsigned ceil_log2_u(unsigned n) {
    unsigned p = 0, v = 1;
    while (v < n && p < 31) { v <<= 1; p++; }
    return p;
}
static uint8_t ps2_alpha(unsigned a) {
    /* Python round is nearest-even. Exact ties are impossible because 255 is odd. */
    return (uint8_t)((a * 128u + 127u) / 255u);
}
static uint8_t alpha_from_ps2(unsigned a) {
    unsigned v = (a * 255u + 64u) / 128u; return (uint8_t)(v > 255 ? 255 : v);
}
static uint16_t pack_ps2_5551(int r5, int g5, int b5, int a) {
    return (uint16_t)(((a >= 128) ? 0x8000 : 0) | ((b5 & 31) << 10) | ((g5 & 31) << 5) |
                      (r5 & 31));
}
static uint16_t rgba_to_ps2_5551(int r, int g, int b, int a) {
    int r5 = (r * 31 + 127) / 255, g5 = (g * 31 + 127) / 255, b5 = (b * 31 + 127) / 255;
    return pack_ps2_5551(r5, g5, b5, a);
}
static void ps2_5551_to_rgba(uint16_t v, uint8_t out[4]) {
    out[0] = (uint8_t)((((v >> 0) & 31) * 255 + 15) / 31);
    out[1] = (uint8_t)((((v >> 5) & 31) * 255 + 15) / 31);
    out[2] = (uint8_t)((((v >> 10) & 31) * 255 + 15) / 31);
    out[3] = (v & 0x8000) ? 255 : 0;
}
static uint64_t compute_gs_tex0(int w, int h, int psm, int cpsm, int cbp) {
    int unit = (psm == GS_PSM_T8) ? 128 : (psm == GS_PSM_T4) ? 256 : 64;
    int tbw = align_up(w, unit) / 64; if (tbw < 1) tbw = 1;
    uint64_t v = 0;
    v |= ((uint64_t)tbw & 0x3f) << 14;
    v |= ((uint64_t)psm & 0x3f) << 20;
    v |= ((uint64_t)ceil_log2_u((unsigned)(w > 0 ? w : 1)) & 0xf) << 26;
    v |= ((uint64_t)ceil_log2_u((unsigned)(h > 0 ? h : 1)) & 0xf) << 30;
    v |= UINT64_C(1) << 34;
    v |= ((uint64_t)cbp & 0x3fff) << 37;
    v |= ((uint64_t)cpsm & 0xf) << 51;
    v |= UINT64_C(1) << 61;
    return v;
}
static uint32_t compute_gs_texclut(int cbw, int cou, int cov) {
    return (uint32_t)((cbw & 0x3f) | ((cou & 0x3f) << 6) | ((cov & 0x3ff) << 12));
}

/* The CSM1 indexed-8 CLUT permutation. It is an involution. */
static void clut8_swizzle_rgba(uint8_t pal[256][4]) {
    uint8_t copy[256][4]; memcpy(copy, pal, sizeof(copy));
    static const int stripe_map[4] = {0, 2, 1, 3};
    for (int i = 0; i < 256; i++) {
        int block = i / 32, inner = i % 32, stripe = inner / 8, pos = inner % 8;
        int j = block * 32 + stripe_map[stripe] * 8 + pos;
        memcpy(pal[j], copy[i], 4);
    }
}

/* Pixel-address permutations matching the Python implementation exactly.
   Storage remains the original linear byte length; out-of-range page addresses
   are ignored, which matters for non-page-aligned input dimensions. */
static uint8_t *gs_swizzle_32(const uint8_t *src, size_t n, int w, int h) {
    static const uint8_t bo[32] = {
        0,1,4,5,16,17,20,21,2,3,6,7,18,19,22,23,
        8,9,12,13,24,25,28,29,10,11,14,15,26,27,30,31
    };
    uint8_t *dst = xcalloc(n, 1); int pages_w = w / 64; if (pages_w < 1) pages_w = 1;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int px=x%64, py=y%32, bi=(py/8)*8+(px/8), ab=bo[bi&31];
        size_t di=(size_t)(((y/32)*pages_w+x/64)*2048+ab*64+(py%8)*8+(px%8))*4;
        size_t si=((size_t)y*w+x)*4;
        if (di + 4 <= n && si + 4 <= n) memcpy(dst+di,src+si,4);
    }
    return dst;
}
static uint8_t *gs_swizzle_16(const uint8_t *src, size_t n, int w, int h) {
    static const uint8_t bo[64] = {
        0,2,8,10,32,34,40,42,1,3,9,11,33,35,41,43,
        4,6,12,14,36,38,44,46,5,7,13,15,37,39,45,47,
        16,18,24,26,48,50,56,58,17,19,25,27,49,51,57,59,
        20,22,28,30,52,54,60,62,21,23,29,31,53,55,61,63
    };
    uint8_t *dst=xcalloc(n,1); int pages_w=w/64; if(pages_w<1)pages_w=1;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
        int px=x%64,py=y%64,bi=(py/8)*4+(px/16),ab=bo[bi&63];
        size_t di=(size_t)(((y/64)*pages_w+x/64)*4096+ab*128+(py%8)*16+(px%16))*2;
        size_t si=((size_t)y*w+x)*2;
        if(di+2<=n&&si+2<=n)memcpy(dst+di,src+si,2);
    }
    return dst;
}

/* Indexed texture swizzles. These are provided for callers that need explicit
   GS layout support; the Python-compatible TIM2 path intentionally keeps T8/T4
   image bytes linear because the original CLI documents loader-side swizzling. */
static void swizzle8_ps2(uint8_t *dst, const uint8_t *src, int width, int height) {
    memset(dst,0,(size_t)width*height);
    for(int y=0;y<height;y++)for(int x=0;x<width;x++){
        int block_location=(y&(~0xf))*width+(x&(~0xf))*2;
        int swap_selector=(((y+2)>>2)&1)*4;
        int posY=(((y&(~3))>>1)+(y&1))&7;
        int column_location=posY*width*2+((x+swap_selector)&7)*4;
        int byte_num=((y>>1)&1)+((x>>2)&2);
        size_t d=(size_t)block_location+column_location+byte_num;
        if(d<(size_t)width*height)dst[d]=src[(size_t)y*width+x];
    }
}
static void swizzle4_ps2(uint8_t *dst,const uint8_t *src,int width,int height){
    /* Convert packed nibbles through a byte-index plane and apply the standard
       PSMT4 128x128-page/32x16-block mapping. */
    size_t np=(size_t)width*height; uint8_t *idx=xmalloc(np);
    for(size_t i=0;i<np;i++)idx[i]=(uint8_t)((i&1)?src[i/2]>>4:src[i/2]&15);
    memset(dst,0,(np+1)/2);
    static const uint8_t block4[32]={0,2,8,10,1,3,9,11,4,6,12,14,5,7,13,15,
                                    16,18,24,26,17,19,25,27,20,22,28,30,21,23,29,31};
    int pages_w=(width+127)/128;
    for(int y=0;y<height;y++)for(int x=0;x<width;x++){
        int px=x&127,py=y&127,bx=px/32,by=py/16;
        size_t page=(size_t)((y/128)*pages_w+x/128)*16384;
        size_t pi=page+(size_t)block4[(by*4+bx)&31]*512+(size_t)(py&15)*32+(px&31);
        if(pi<np){size_t b=pi/2;if(pi&1)dst[b]=(uint8_t)((dst[b]&15)|(idx[(size_t)y*width+x]<<4));
                  else dst[b]=(uint8_t)((dst[b]&0xf0)|idx[(size_t)y*width+x]);}
    }
    free(idx);
}

static uint8_t *gs_unswizzle_32(const uint8_t *src, size_t n, int w, int h) {
    static const uint8_t bo[32] = {
        0,1,4,5,16,17,20,21,2,3,6,7,18,19,22,23,
        8,9,12,13,24,25,28,29,10,11,14,15,26,27,30,31
    };
    uint8_t *dst=xcalloc(n,1);int pages_w=w/64;if(pages_w<1)pages_w=1;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
        int px=x%64,py=y%32,bi=(py/8)*8+(px/8),ab=bo[bi&31];
        size_t si=(size_t)(((y/32)*pages_w+x/64)*2048+ab*64+(py%8)*8+(px%8))*4;
        size_t di=((size_t)y*w+x)*4;if(si+4<=n&&di+4<=n)memcpy(dst+di,src+si,4);
    }return dst;
}
static uint8_t *gs_unswizzle_16(const uint8_t *src,size_t n,int w,int h){
    static const uint8_t bo[64]={0,2,8,10,32,34,40,42,1,3,9,11,33,35,41,43,
        4,6,12,14,36,38,44,46,5,7,13,15,37,39,45,47,16,18,24,26,48,50,56,58,
        17,19,25,27,49,51,57,59,20,22,28,30,52,54,60,62,21,23,29,31,53,55,61,63};
    uint8_t *dst=xcalloc(n,1);int pages_w=w/64;if(pages_w<1)pages_w=1;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
        int px=x%64,py=y%64,bi=(py/8)*4+(px/16),ab=bo[bi&63];
        size_t si=(size_t)(((y/64)*pages_w+x/64)*4096+ab*128+(py%8)*16+(px%16))*2;
        size_t di=((size_t)y*w+x)*2;if(si+2<=n&&di+2<=n)memcpy(dst+di,src+si,2);
    }return dst;
}
static uint8_t *unswizzle8_ps2(const uint8_t *src,int width,int height){
    size_t n=(size_t)width*height;uint8_t *dst=xcalloc(n,1);
    for(int y=0;y<height;y++)for(int x=0;x<width;x++){
        int block_location=(y&(~0xf))*width+(x&(~0xf))*2;
        int swap_selector=(((y+2)>>2)&1)*4;
        int posY=(((y&(~3))>>1)+(y&1))&7;
        int column_location=posY*width*2+((x+swap_selector)&7)*4;
        int byte_num=((y>>1)&1)+((x>>2)&2);
        size_t si=(size_t)block_location+column_location+byte_num;
        if(si<n)dst[(size_t)y*width+x]=src[si];
    }return dst;
}
static uint8_t *unswizzle4_ps2(const uint8_t *src,int width,int height){
    size_t np=(size_t)width*height;uint8_t *idx=xcalloc(np,1),*dst=xcalloc((np+1)/2,1);
    static const uint8_t block4[32]={0,2,8,10,1,3,9,11,4,6,12,14,5,7,13,15,
                                    16,18,24,26,17,19,25,27,20,22,28,30,21,23,29,31};
    int pages_w=(width+127)/128;
    for(int y=0;y<height;y++)for(int x=0;x<width;x++){
        int px=x&127,py=y&127,bx=px/32,by=py/16;
        size_t page=(size_t)((y/128)*pages_w+x/128)*16384;
        size_t pi=page+(size_t)block4[(by*4+bx)&31]*512+(size_t)(py&15)*32+(px&31);
        if(pi<np)idx[(size_t)y*width+x]=(uint8_t)((pi&1)?src[pi/2]>>4:src[pi/2]&15);
    }
    for(size_t i=0;i<np;i++)if(i&1)dst[i/2]|=(uint8_t)(idx[i]<<4);else dst[i/2]|=idx[i];
    free(idx);return dst;
}

static uint8_t *pack_32(const Image *im, size_t *len) {
    *len=(size_t)im->w*im->h*4; uint8_t *p=xmalloc(*len);
    for(size_t i=0;i<(size_t)im->w*im->h;i++){
        memcpy(p+i*4,im->rgba+i*4,3); p[i*4+3]=ps2_alpha(im->rgba[i*4+3]);
    }
    return p;
}
static uint8_t *pack_24(const Image *im, size_t *len) {
    *len=(size_t)im->w*im->h*3; uint8_t *p=xmalloc(*len);
    for(size_t i=0;i<(size_t)im->w*im->h;i++)memcpy(p+i*3,im->rgba+i*4,3);
    return p;
}
static uint8_t *pack_16(const Image *im, bool dither, size_t *len) {
    size_t np=(size_t)im->w*im->h; *len=np*2; uint8_t *p=xmalloc(*len);
    if(dither){
        uint8_t *q=dither_direct_5bit(im);
        for(size_t i=0;i<np;i++)
            wr16(p+i*2,pack_ps2_5551(q[i*4],q[i*4+1],q[i*4+2],q[i*4+3]));
        free(q);
    }
    else for(size_t i=0;i<np;i++){const uint8_t *s=im->rgba+i*4;
        wr16(p+i*2,rgba_to_ps2_5551(s[0],s[1],s[2],s[3]));}
    return p;
}

typedef struct { uint8_t *pixels,*clut; size_t pixel_n,clut_n; } IndexedPack;
static IndexedPack pack_indexed(const Image *im,int colors,bool dither) {
    IndexedPack q={0}; uint8_t pal[256][3], cpal[256][4];
    int actual=build_median_palette(im,colors,pal);
    uint64_t asum[256]; uint32_t acount[256];
    uint8_t *idx=map_palette(im,pal,actual,dither,asum,acount);
    size_t np=(size_t)im->w*im->h;
    if(colors==256){q.pixel_n=np;q.pixels=idx;}
    else {q.pixel_n=(np+1)/2;q.pixels=xcalloc(q.pixel_n,1);
        for(size_t i=0;i<np;i+=2)q.pixels[i/2]=(uint8_t)((idx[i]&15)|((i+1<np?idx[i+1]:0)&15)<<4);
        free(idx);}
    memset(cpal,0,sizeof(cpal));
    for(int i=0;i<colors;i++){
        cpal[i][0]=pal[i][0];cpal[i][1]=pal[i][1];cpal[i][2]=pal[i][2];
        cpal[i][3]=ps2_alpha(acount[i]?(unsigned)llrint((double)asum[i]/acount[i]):255);
    }
    if(colors==256)clut8_swizzle_rgba(cpal);
    q.clut_n=(size_t)colors*4;q.clut=xmalloc(q.clut_n);memcpy(q.clut,cpal,q.clut_n);
    return q;
}

static void append_mipmap_sizes(Buffer *dst,const Image *base,int fmt,bool dither) {
    int w=base->w,h=base->h;
    while(w>1||h>1){w=w>1?w/2:1;h=h>1?h/2:1;Image *m=resize_lanczos((Image*)base,w,h);
        if(!m){fprintf(stderr,"fatal: mipmap resize failed: %s\n",g_error);exit(2);}size_t n=0;uint8_t *p=NULL;
        if(fmt==32)p=pack_32(m,&n);else if(fmt==24)p=pack_24(m,&n);else if(fmt==16)p=pack_16(m,dither,&n);
        else{IndexedPack q=pack_indexed(m,fmt==8?256:16,dither);p=q.pixels;n=q.pixel_n;free(q.clut);}
        buf_put(dst,p,n);free(p);image_free(m);
    }
}
static int mipmap_levels_for(int w,int h){int n=1;while(w>1||h>1){w=w>1?w/2:1;h=h>1?h/2:1;n++;}return n;}

/* Build one 48-byte TIM2 picture header followed by base image, optional full
   mip chain, indexed palette, and 128-byte block padding. */
static bool build_tim2(Image *im,int fmt,bool premult,bool dither,bool swizzle,bool mipmaps,Buffer *out){
    if(im->w<=0||im->h<=0||im->w>65535||im->h>65535){set_error("TIM2 dimensions must be 1..65535");return false;}
    if(premult&&(fmt==32||fmt==16))premultiply_alpha(im);
    uint8_t img_type=0,clut_type=CLUT_NONE;uint16_t clut_colors=0;int psm=GS_PSM_CT32;
    uint8_t *base=NULL,*clut=NULL;size_t base_n=0,clut_n=0;
    if(fmt==32){img_type=IMG_RGBA32;psm=GS_PSM_CT32;base=pack_32(im,&base_n);}
    else if(fmt==24){img_type=IMG_RGB24;psm=GS_PSM_CT32;base=pack_24(im,&base_n);}
    else if(fmt==16){img_type=IMG_RGBA16;psm=GS_PSM_CT16;base=pack_16(im,dither,&base_n);}
    else {int nc=fmt==8?256:16;IndexedPack q=pack_indexed(im,nc,dither);base=q.pixels;base_n=q.pixel_n;
        clut=q.clut;clut_n=q.clut_n;clut_colors=(uint16_t)nc;clut_type=CLUT_RGBA32;
        img_type=fmt==8?IMG_INDEXED8:IMG_INDEXED4;psm=fmt==8?GS_PSM_T8:GS_PSM_T4;}
    if(swizzle){uint8_t *s=NULL;
        if(fmt==32)s=gs_swizzle_32(base,base_n,im->w,im->h);
        else if(fmt==16)s=gs_swizzle_16(base,base_n,im->w,im->h);
        if(s){free(base);base=s;}
    }
    Buffer images={0};buf_put(&images,base,base_n);free(base);
    int mip_count=1;
    if(mipmaps){append_mipmap_sizes(&images,im,fmt,dither);mip_count=mipmap_levels_for(im->w,im->h)-1;if(mip_count<1)mip_count=1;}
    if(images.n>UINT32_MAX||clut_n>UINT32_MAX){free(images.p);free(clut);set_error("TIM2 data exceeds 32-bit block fields");return false;}
    size_t raw=48+images.n+clut_n,total=(raw+127)&~(size_t)127;
    if(total>UINT32_MAX){free(images.p);free(clut);set_error("TIM2 block too large");return false;}
    uint8_t h[48]={0};wr32(h,(uint32_t)total);wr32(h+4,(uint32_t)clut_n);wr32(h+8,(uint32_t)images.n);
    wr16(h+12,48);wr16(h+14,clut_colors);h[16]=(uint8_t)mip_count;h[17]=clut_type;h[18]=img_type;
    wr16(h+20,(uint16_t)im->w);wr16(h+22,(uint16_t)im->h);
    wr64(h+24,compute_gs_tex0(im->w,im->h,psm,GS_CPSM_CT32,0));
    wr32(h+40,clut_colors?compute_gs_texclut(1,0,0):0);
    uint8_t fh[16]={ 'T','I','M','2',TIM2_VERSION,0,1,0,0,0,0,0,0,0,0,0 };
    buf_put(out,fh,16);buf_put(out,h,48);buf_put(out,images.p,images.n);if(clut_n)buf_put(out,clut,clut_n);
    buf_zero(out,total-raw);free(images.p);free(clut);return true;
}

/* Defensive parser for the first picture in a TIM2 file. */
typedef struct {
    const uint8_t *raw,*pixels,*clut;size_t file_n,pixel_available,clut_available;
    uint8_t version,file_format,mipmap_count,clut_type,img_type;
    uint16_t num_pictures,hdr_size,clut_colors,width,height;
    uint32_t block_total,clut_size,img_size,gs_texclut;uint64_t gs_tex0,gs_tex1;
    size_t image_start,clut_start,block_end;
} Tim2Info;
static const char *img_type_name(int t){switch(t){case IMG_RGBA32:return "32-bit RGBA8888";case IMG_RGBA16:return "16-bit RGBA5551";case IMG_RGB24:return "24-bit RGB888 (no Alpha)";case IMG_INDEXED8:return "8-bit Indexed (256 colors)";case IMG_INDEXED4:return "4-bit Indexed (16 colors)";case IMG_INDEXED4_SONY:return "4-bit Indexed (Sony variant)";default:return "Unknown";}}
static const char *clut_type_name(int t){switch(t){case 0:return "none";case 1:return "RGBA5551";case 2:return "RGBA8888";default:return "Unknown";}}
static bool valid_img_type(int t){return t==0||t==1||t==2||t==3||t==5||t==6;}
static bool parse_tim2_mem(const uint8_t*d,size_t n,Tim2Info*t){
    memset(t,0,sizeof(*t));t->raw=d;t->file_n=n;
    if(n<16){set_error("file too small for TIM2 file header");return false;}
    if(memcmp(d,"TIM2",4)){set_error("not a valid TIM2 (bad magic)");return false;}
    t->version=d[4];t->file_format=d[5];t->num_pictures=rd16(d+6);
    if(!t->num_pictures){set_error("TIM2 contains no pictures");return false;}
    if(n<64){set_error("truncated TIM2 picture header");return false;}
    const uint8_t*h=d+16;t->block_total=rd32(h);t->clut_size=rd32(h+4);t->img_size=rd32(h+8);
    t->hdr_size=rd16(h+12);t->clut_colors=rd16(h+14);t->mipmap_count=h[16];t->clut_type=h[17];t->img_type=h[18];
    t->width=rd16(h+20);t->height=rd16(h+22);t->gs_tex0=rd64(h+24);t->gs_tex1=rd64(h+32);t->gs_texclut=rd32(h+40);
    if(t->hdr_size<48||t->hdr_size>n-16){set_error("invalid picture header size %u",t->hdr_size);return false;}
    t->image_start=16+t->hdr_size;
    if(t->image_start>n||t->img_size>n-t->image_start){set_error("truncated image data (declared %u bytes)",t->img_size);return false;}
    t->pixels=d+t->image_start;t->pixel_available=t->img_size;t->clut_start=t->image_start+t->img_size;
    if(t->clut_start>n||t->clut_size>n-t->clut_start){set_error("truncated CLUT data (declared %u bytes)",t->clut_size);return false;}
    t->clut=d+t->clut_start;t->clut_available=t->clut_size;
    t->block_end=16+(size_t)t->block_total;if(t->block_end>n){set_error("truncated picture block");return false;}
    return true;
}
static bool read_tim2(const char*path,uint8_t**d,size_t*n,Tim2Info*t){if(!read_file(path,d,n))return false;if(!parse_tim2_mem(*d,*n,t)){free(*d);*d=NULL;return false;}return true;}
static size_t base_pixel_size(const Tim2Info*t){size_t np=(size_t)t->width*t->height;switch(t->img_type){case IMG_RGBA32:return np*4;case IMG_RGBA16:return np*2;case IMG_RGB24:return np*3;case IMG_INDEXED8:return np;case IMG_INDEXED4:case IMG_INDEXED4_SONY:return(np+1)/2;default:return 0;}}

static bool tim2_info_file(const char*path){uint8_t*d;size_t n;Tim2Info t;if(!read_tim2(path,&d,&n,&t))return false;
    uint64_t v=t.gs_tex0;unsigned tbp0=v&0x3fff,tbw=(v>>14)&0x3f,psm=(v>>20)&0x3f,tw=(v>>26)&15,th=(v>>30)&15,tcc=(v>>34)&1,tfx=(v>>35)&3,cbp=(v>>37)&0x3fff,cpsm=(v>>51)&15,csm=(v>>55)&1,cld=(v>>61)&7;
    printf("\n----------------------------------------------------\n  File        :  %s\n  File size   :  %.2f KB  (%zu bytes)\n----------------------------------------------------\n",path_base(path),n/1024.0,n);
    printf("  TIM2 version:  0x%02X\n  Pictures    :  %u\n  Mipmaps     :  %u\n----------------------------------------------------\n",t.version,t.num_pictures,t.mipmap_count);
    printf("  Format      :  %s\n  Width       :  %u px\n  Height      :  %u px\n  Power-of-2  :  %s\n  Image data  :  %.2f KB  (%u bytes)\n----------------------------------------------------\n",img_type_name(t.img_type),t.width,t.height,(is_power2(t.width)&&is_power2(t.height))?"yes":"NO <-- non-power-of-2",t.img_size/1024.0,t.img_size);
    printf("  CLUT type   :  %s\n  CLUT colors :  %s",clut_type_name(t.clut_type),t.clut_colors?"":"none\n");if(t.clut_colors)printf("%u\n",t.clut_colors);printf("  CLUT size   :  %u bytes\n----------------------------------------------------\n",t.clut_size);
    printf("  GsTex0      :  0x%016llX\n    TBP0      :  0x%04X\n    TBW       :  %u\n    PSM       :  0x%02X\n    TW/TH     :  %u/%u\n    TCC/TFX   :  %u/%u\n    CBP       :  0x%04X\n    CPSM/CSM  :  0x%02X/%u\n    CLD       :  %u\n  GsTexClut   :  0x%08X\n----------------------------------------------------\n",(unsigned long long)v,tbp0,tbw,psm,tw,th,tcc,tfx,cbp,cpsm,csm,cld,t.gs_texclut);free(d);return true;}

static bool verify_tim2_file(const char*path){printf("\n----------------------------------------------------\n  Verifying: %s\n----------------------------------------------------\n",path_base(path));uint8_t*d=NULL;size_t n=0;Tim2Info t;
    if(!read_file(path,&d,&n)){printf("  [FAIL]  %s\n",g_error);return false;}
    bool ok=true;int warns=0,errors=0;
#define CHK(c,okmsg,badmsg,warn) do{if(c)printf("  [PASS]  %s\n",okmsg);else{printf("  [%s]  %s\n",warn?"WARN":"FAIL",badmsg);if(warn)warns++;else{errors++;ok=false;}}}while(0)
    CHK(n>=64,"File size OK","File too small",false);if(n<64){free(d);return false;}
    CHK(!memcmp(d,"TIM2",4),"Magic Number OK","Invalid Magic Number",false);if(memcmp(d,"TIM2",4)){free(d);return false;}
    if(!parse_tim2_mem(d,n,&t)){printf("  [FAIL]  %s\n",g_error);free(d);return false;}
    CHK(t.version==4,"Version OK","Unexpected Version",true);CHK(t.hdr_size==48,"Header Size OK","Unexpected Header Size",false);
    CHK(valid_img_type(t.img_type),"Image Type OK","Unknown Image Type",false);CHK(t.width&&t.height,"Dimensions OK","Invalid Dimensions",false);
    CHK(is_power2(t.width)&&is_power2(t.height),"Power-of-2 OK","Non-power-of-2 dimensions",true);
    CHK(n>=16+(size_t)t.block_total,"File completeness OK","File appears truncated",false);CHK(t.block_total%128==0,"Block alignment OK","Block not aligned to 128",false);
    size_t computed=(size_t)t.hdr_size+t.img_size+t.clut_size,aligned=(computed+127)&~(size_t)127;
    CHK(t.block_total==aligned,"Block size consistent","Block size mismatch",false);
    int ec=(t.img_type==IMG_INDEXED8)?256:(t.img_type==IMG_INDEXED4||t.img_type==IMG_INDEXED4_SONY)?16:0;
    CHK(t.clut_colors==ec,"CLUT colors OK","CLUT color count mismatch",false);
    unsigned tw=(unsigned)((t.gs_tex0>>26)&15),th=(unsigned)((t.gs_tex0>>30)&15);
    CHK(tw==ceil_log2_u(t.width)&&th==ceil_log2_u(t.height),"GsTex0 TW/TH OK","GsTex0 TW/TH mismatch",true);
    size_t base=base_pixel_size(&t);CHK(t.img_size>=base,"Base image data size OK","Image data smaller than base level",false);
    if(ec)CHK(t.clut_size==(uint32_t)ec*4,"CLUT byte size OK","CLUT byte size mismatch",false);
#undef CHK
    printf("----------------------------------------------------\n  Result: %s",ok?(warns?"VALID with warnings":"VALID - all checks passed"):"INVALID");if(errors||warns)printf(" (%d error(s), %d warning(s))",errors,warns);puts("\n----------------------------------------------------");free(d);return ok;}

static Image*decode_tim2_image(const Tim2Info*t){int w=t->width,h=t->height;size_t np=(size_t)w*h,need=base_pixel_size(t);if(!need||t->pixel_available<need){set_error("unsupported or truncated base image");return NULL;}Image*im=image_new(w,h);if(!im)return NULL;const uint8_t*pix=t->pixels;uint8_t*linear=NULL;if(t->file_format==1){if(t->img_type==IMG_RGBA32)linear=gs_unswizzle_32(pix,need,w,h);else if(t->img_type==IMG_RGBA16)linear=gs_unswizzle_16(pix,need,w,h);else if(t->img_type==IMG_INDEXED8)linear=unswizzle8_ps2(pix,w,h);else if(t->img_type==IMG_INDEXED4||t->img_type==IMG_INDEXED4_SONY)linear=unswizzle4_ps2(pix,w,h);if(linear)pix=linear;}
    if(t->img_type==IMG_RGBA32){for(size_t i=0;i<np;i++){memcpy(im->rgba+i*4,pix+i*4,3);im->rgba[i*4+3]=alpha_from_ps2(pix[i*4+3]);}}
    else if(t->img_type==IMG_RGBA16){for(size_t i=0;i<np;i++)ps2_5551_to_rgba(rd16(pix+i*2),im->rgba+i*4);}
    else if(t->img_type==IMG_RGB24){for(size_t i=0;i<np;i++){memcpy(im->rgba+i*4,pix+i*3,3);im->rgba[i*4+3]=255;}}
    else {int nc=(t->img_type==IMG_INDEXED8)?256:16;if(t->clut_colors<nc||t->clut_available<(size_t)nc*4){image_free(im);set_error("indexed TIM2 has missing CLUT");return NULL;}uint8_t pal[256][4];memset(pal,0,sizeof(pal));for(int i=0;i<nc;i++){memcpy(pal[i],t->clut+i*4,3);pal[i][3]=alpha_from_ps2(t->clut[i*4+3]);}if(nc==256)clut8_swizzle_rgba(pal);for(size_t i=0;i<np;i++){unsigned k=nc==256?t->pixels[i]:((i&1)?t->pixels[i/2]>>4:t->pixels[i/2]&15);memcpy(im->rgba+i*4,pal[k],4);}}
    free(linear);return im;
}

static Image*composite_white(const Image*im){Image*o=image_new(im->w,im->h);if(!o)return NULL;for(size_t i=0;i<(size_t)im->w*im->h;i++){unsigned a=im->rgba[i*4+3];for(int c=0;c<3;c++)o->rgba[i*4+c]=(uint8_t)((im->rgba[i*4+c]*a+255*(255-a)+127)/255);o->rgba[i*4+3]=255;}return o;}
static bool save_jpeg(const char*path,const Image*im){FILE*f=fopen(path,"wb");if(!f){set_error("cannot create '%s'",path);return false;}struct jpeg_compress_struct c;JpegErr e;c.err=jpeg_std_error(&e.pub);e.pub.error_exit=jpeg_fail;if(setjmp(e.jump)){set_error("JPEG: %s",e.msg);jpeg_destroy_compress(&c);fclose(f);return false;}jpeg_create_compress(&c);jpeg_stdio_dest(&c,f);c.image_width=im->w;c.image_height=im->h;c.input_components=3;c.in_color_space=JCS_RGB;jpeg_set_defaults(&c);for(int i=0;i<c.num_components;i++){c.comp_info[i].h_samp_factor=1;c.comp_info[i].v_samp_factor=1;}jpeg_set_quality(&c,95,TRUE);jpeg_start_compress(&c,TRUE);uint8_t*row=xmalloc((size_t)im->w*3);while(c.next_scanline<c.image_height){size_t y=c.next_scanline;for(int x=0;x<im->w;x++)memcpy(row+x*3,im->rgba+(y*(size_t)im->w+x)*4,3);JSAMPROW rp=row;jpeg_write_scanlines(&c,&rp,1);}free(row);jpeg_finish_compress(&c);jpeg_destroy_compress(&c);bool ok=fclose(f)==0;if(!ok)set_error("JPEG close failed");return ok;}
static bool save_extract(const char*path,const Image*src){const char*e=path_ext(path);bool flat=!strcasecmp(e,".jpg")||!strcasecmp(e,".jpeg")||!strcasecmp(e,".bmp")||!strcasecmp(e,".ppm");Image*tmp=flat?composite_white(src):NULL;const Image*im=tmp?tmp:src;bool ok;if(!strcasecmp(e,".jpg")||!strcasecmp(e,".jpeg"))ok=save_jpeg(path,im);else ok=save_image(path,im);image_free(tmp);return ok;}
static bool extract_tim2(const char*path,const char*ext,char**outpath){uint8_t*d;size_t n;Tim2Info t;if(!read_tim2(path,&d,&n,&t))return false;Image*im=decode_tim2_image(&t);free(d);if(!im)return false;*outpath=replace_ext(path,ext);bool ok=save_extract(*outpath,im);image_free(im);if(!ok){free(*outpath);*outpath=NULL;}return ok;}

static bool has_transparency(const Tim2Info*t){size_t base=base_pixel_size(t);if(t->img_type==IMG_RGBA32){for(size_t i=3;i<base;i+=4)if(t->pixels[i]<0x80)return true;}else if(t->img_type==IMG_RGBA16){for(size_t i=0;i+1<base;i+=2)if(!(rd16(t->pixels+i)&0x8000))return true;}else if(t->img_type==IMG_INDEXED8||t->img_type==IMG_INDEXED4||t->img_type==IMG_INDEXED4_SONY){size_t bpc=t->clut_colors?t->clut_size/t->clut_colors:4;if(bpc==4)for(size_t i=3;i<t->clut_size;i+=4)if(t->clut[i]<0x80)return true;}return false;}
typedef enum{DIFF_SAFE,DIFF_WARNING,DIFF_UNSAFE}DiffLevel;
static DiffLevel diff_files(const char*a,const char*b,bool heading){uint8_t*da=NULL,*db=NULL;size_t na=0,nb=0;Tim2Info x,y;if(heading)printf("\n--------------------------------------------------------\n  Original : %s\n  Modified : %s\n--------------------------------------------------------\n",path_base(a),path_base(b));if(!read_tim2(a,&da,&na,&x)){printf("  [FAIL] Cannot read original: %s\n",g_error);return DIFF_UNSAFE;}if(!read_tim2(b,&db,&nb,&y)){printf("  [FAIL] Cannot read modified: %s\n",g_error);free(da);return DIFF_UNSAFE;}int issues=0,warns=0;
    if(x.img_type==y.img_type)printf("  [OK]    Format       : %s\n",img_type_name(x.img_type));else{printf("  [FAIL]  Format       : %s -> %s\n",img_type_name(x.img_type),img_type_name(y.img_type));issues++;}
    if(x.width==y.width&&x.height==y.height)printf("  [OK]    Dimensions   : %ux%u\n",x.width,x.height);else{printf("  [FAIL]  Dimensions   : %ux%u -> %ux%u\n",x.width,x.height,y.width,y.height);issues++;}
    if(na==nb)printf("  [OK]    File size    : %.2f KB\n",na/1024.0);else{printf("  [WARN]  File size    : %.2f KB -> %.2f KB\n",na/1024.0,nb/1024.0);warns++;}
    bool xa=has_transparency(&x),ya=has_transparency(&y);if(xa==ya)printf("  [OK]    Transparency: %s\n",xa?"has transparency":"solid");else if(xa&&!ya){puts("  [WARN]  Transparency lost in modified file");warns++;}else puts("  [INFO]  Transparency added in modified file");
    if(x.img_type==y.img_type&&x.width==y.width&&x.height==y.height){size_t npx=(size_t)x.width*x.height,changed=0;unsigned md=0;if(x.img_type==IMG_RGBA32){size_t z=npx*4;if(z>x.img_size)z=x.img_size;if(z>y.img_size)z=y.img_size;for(size_t i=0;i+3<z;i+=4){unsigned d=0;for(int c=0;c<4;c++){unsigned q=(unsigned)abs((int)x.pixels[i+c]-(int)y.pixels[i+c]);if(q>d)d=q;}if(d)changed++;if(d>md)md=d;}}else if(x.img_type==IMG_RGBA16){size_t z=npx*2;if(z>x.img_size)z=x.img_size;if(z>y.img_size)z=y.img_size;for(size_t i=0;i+1<z;i+=2){unsigned v1=rd16(x.pixels+i),v2=rd16(y.pixels+i),d=v1>v2?v1-v2:v2-v1;if(d)changed++;if(d>md)md=d;}}else{size_t z=x.img_size<y.img_size?x.img_size:y.img_size;if(z>npx)z=npx;for(size_t i=0;i<z;i++){unsigned d=(unsigned)abs((int)x.pixels[i]-(int)y.pixels[i]);if(d)changed++;if(d>md)md=d;}}printf("  [INFO]  Pixels changed: %zu / %zu (%.1f%%)\n  [INFO]  Max color diff: %u / 255\n",changed,npx,npx?changed*100.0/npx:0,md);}else puts("  [INFO]  Pixel diff skipped");
    DiffLevel r=issues?DIFF_UNSAFE:warns?DIFF_WARNING:DIFF_SAFE;printf("--------------------------------------------------------\n  Compatibility: %s\n--------------------------------------------------------\n",r==DIFF_SAFE?"SAFE":r==DIFF_WARNING?"WARNING":"UNSAFE");free(da);free(db);return r;}

typedef struct{char**v;size_t n,cap;}StrVec;
static void sv_add(StrVec*s,const char*x){if(s->n==s->cap){s->cap=s->cap?2*s->cap:8;s->v=xrealloc(s->v,s->cap*sizeof(*s->v));}s->v[s->n++]=xstrdup(x);}
static void sv_free(StrVec*s){for(size_t i=0;i<s->n;i++)free(s->v[i]);free(s->v);memset(s,0,sizeof(*s));}
static int cmp_strp(const void*a,const void*b){return strcmp(*(char*const*)a,*(char*const*)b);}
static bool is_tm2_name(const char*n){const char*e=path_ext(n);return !strcasecmp(e,".tm2")||!strcasecmp(e,".tim2");}
static char*join_path(const char*a,const char*b){size_t na=strlen(a),nb=strlen(b);char*p=xmalloc(na+nb+2);memcpy(p,a,na);if(na&&a[na-1]!='/')p[na++]='/';memcpy(p+na,b,nb+1);return p;}
static void scan_tm2_recursive(const char*dir,StrVec*out){DIR*d=opendir(dir);if(!d)return;struct dirent*e;while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char*p=join_path(dir,e->d_name);if(is_dir(p))scan_tm2_recursive(p,out);else if(is_tm2_name(e->d_name))sv_add(out,p);free(p);}closedir(d);}
static const char*filename_only(const char*p){return path_base(p);}
static char*find_by_name(const StrVec*s,const char*n){for(size_t i=0;i<s->n;i++)if(!strcmp(filename_only(s->v[i]),n))return s->v[i];return NULL;}
static void diff_summary(int n,int safe,int warn,int bad){printf("\n========================================================\n  Summary: %d compared | SAFE %d | WARNING %d | UNSAFE %d\n========================================================\n",n,safe,warn,bad);}
static int diff_folders(const char*a,const char*b){StrVec x={0},y={0};scan_tm2_recursive(a,&x);scan_tm2_recursive(b,&y);qsort(x.v,x.n,sizeof(char*),cmp_strp);qsort(y.v,y.n,sizeof(char*),cmp_strp);printf("\n========================================================\n  Comparing folders:\n  Original : %s (%zu TIM2 files)\n  Modified : %s (%zu TIM2 files)\n========================================================\n",a,x.n,b,y.n);int n=0,safe=0,warn=0,bad=0,missing=0,extra=0;for(size_t i=0;i<x.n;i++){char*q=find_by_name(&y,filename_only(x.v[i]));if(!q){printf("  [WARN] Missing in modified: %s\n",filename_only(x.v[i]));missing++;}else{DiffLevel z=diff_files(x.v[i],q,true);n++;safe+=z==DIFF_SAFE;warn+=z==DIFF_WARNING;bad+=z==DIFF_UNSAFE;}}for(size_t i=0;i<y.n;i++)if(!find_by_name(&x,filename_only(y.v[i]))){printf("  [INFO] Extra in modified: %s\n",filename_only(y.v[i]));extra++;}diff_summary(n,safe,warn,bad);printf("  Missing: %d | Extra: %d\n",missing,extra);sv_free(&x);sv_free(&y);return bad?1:0;}

typedef struct{StrVec images,original,modified;const char*format,*output,*output_dir,*resize,*extract,*diff_list,*list;bool dither,no_premult,swizzle,mipmaps,info,verify,diff,list_formats,help,version;}Options;
static void usage(FILE*f,const char*p){fprintf(f,"PS2 TIM2 Converter - native C edition\nUsage:\n  %s <image(s)> --format <4bit|8bit|16bit|24bit|32bit> [options]\n  %s <file.tm2> --info | --verify | --extract png\n  %s --list convert.txt [options]\n  %s --diff --original a.tm2 --modified b.tm2\n\nOptions:\n  -f, --format FMT     output TIM2 format\n  -o, --output FILE    output for one input\n  --output-dir DIR     output folder\n  --no-premult         disable alpha premultiplication\n  --dither             Floyd-Steinberg dithering\n  --resize up|down     resize to a power of two (Lanczos-3)\n  --swizzle            pre-swizzle GS texture data\n  --mipmaps            append a full mipmap chain\n  --info               display TIM2 metadata\n  --verify             run structural checks\n  --extract EXT        png, jpg, jpeg, bmp, tga, tif, tiff, webp, ppm\n  --list FILE          batch list: <filename> <format>\n  --diff               compare files/folders\n  --original PATH...   original inputs for diff\n  --modified PATH...   modified inputs for diff\n  --diff-list FILE     pairs list (first content row is header)\n  -l, --list-formats   show supported formats\n  --version            show version\n  -h, --help           show this help\n",p,p,p,p);}
static void list_formats(void){puts("Supported input formats:\n  .bmp, .dds, .gif, .ico, .jpeg, .jpg, .pbm, .pgm, .png, .ppm, .tga, .tif, .tiff, .webp\n\nOutput TIM2 formats:\n  --format 32bit    RGBA8888 | full color + 8-bit alpha\n  --format 24bit    RGB888   | full color, no alpha\n  --format 16bit    RGBA5551 | 32K colors + 1-bit alpha\n  --format 8bit     Indexed8 | 256 colors + CSM1 CLUT\n  --format 4bit     Indexed4 | 16 colors + CLUT\n\nExtraction: png, jpg/jpeg, bmp, tga, tif/tiff, webp, ppm\nAll codecs and processing are in-process; no external converter is invoked.");}
static bool option_value(const char*s){return !strcmp(s,"--format")||!strcmp(s,"-f")||!strcmp(s,"--output")||!strcmp(s,"-o")||!strcmp(s,"--output-dir")||!strcmp(s,"--resize")||!strcmp(s,"--extract")||!strcmp(s,"--list")||!strcmp(s,"--diff-list");}
static bool parse_options(int argc,char**argv,Options*o){memset(o,0,sizeof(*o));enum{NORMAL,ORIG,MOD}mode=NORMAL;for(int i=1;i<argc;i++){const char*a=argv[i];if(!strcmp(a,"--original")){mode=ORIG;continue;}if(!strcmp(a,"--modified")){mode=MOD;continue;}if(a[0]=='-'){mode=NORMAL;if(!strcmp(a,"-h")||!strcmp(a,"--help"))o->help=true;else if(!strcmp(a,"--version"))o->version=true;else if(!strcmp(a,"--dither"))o->dither=true;else if(!strcmp(a,"--no-premult"))o->no_premult=true;else if(!strcmp(a,"--swizzle"))o->swizzle=true;else if(!strcmp(a,"--mipmaps"))o->mipmaps=true;else if(!strcmp(a,"--info"))o->info=true;else if(!strcmp(a,"--verify"))o->verify=true;else if(!strcmp(a,"--diff"))o->diff=true;else if(!strcmp(a,"--list-formats")||!strcmp(a,"-l"))o->list_formats=true;else if(option_value(a)){if(++i>=argc){fprintf(stderr,"ERROR: %s requires a value\n",a);return false;}const char*v=argv[i];if(!strcmp(a,"--format")||!strcmp(a,"-f"))o->format=v;else if(!strcmp(a,"--output")||!strcmp(a,"-o"))o->output=v;else if(!strcmp(a,"--output-dir"))o->output_dir=v;else if(!strcmp(a,"--resize"))o->resize=v;else if(!strcmp(a,"--extract"))o->extract=v;else if(!strcmp(a,"--list"))o->list=v;else o->diff_list=v;}else{fprintf(stderr,"ERROR: unknown option '%s'\n",a);return false;}continue;}if(mode==ORIG)sv_add(&o->original,a);else if(mode==MOD)sv_add(&o->modified,a);else sv_add(&o->images,a);}return true;}
static int fmt_number(const char*s){if(!s)return-1;if(!strcasecmp(s,"4bit"))return 4;if(!strcasecmp(s,"8bit"))return 8;if(!strcasecmp(s,"16bit"))return 16;if(!strcasecmp(s,"24bit"))return 24;if(!strcasecmp(s,"32bit"))return 32;return-1;}
static char*output_name(const char*src,const Options*o,const char*override){if(override)return xstrdup(override);if(o->output_dir)return join_output(o->output_dir,src,".tm2");return replace_ext(src,".tm2");}
static void warn_dimensions_ps2(const char*n,int w,int h){if(is_power2(w)&&is_power2(h))return;printf("\n  WARNING: '%s' has non-power-of-2 dimensions (%dx%d).\n           PS2 requires power-of-2 sizes for reliable texture rendering.\n           Suggested: %dx%d (down) or %dx%d (up).\n",n,w,h,prev_power2(w),prev_power2(h),next_power2(w),next_power2(h));}
static char*convert_one(const char*src,const char*fmt,const Options*o,const char*override){int f=fmt_number(fmt);if(f<0){set_error("unknown format '%s'",fmt?fmt:"");return NULL;}Image*im=load_image(src);if(!im)return NULL;warn_dimensions_ps2(path_base(src),im->w,im->h);if(o->resize){int nw,nh;if(!parse_resize(o->resize,im->w,im->h,&nw,&nh)||(!strcmp(o->resize,"up")&&!is_power2(nw))){image_free(im);return NULL;}if(strcasecmp(o->resize,"up")&&strcasecmp(o->resize,"down")){set_error("--resize accepts only up or down");image_free(im);return NULL;}if(nw!=im->w||nh!=im->h){Image*r=resize_lanczos(im,nw,nh);if(!r){image_free(im);return NULL;}printf("  Resized: %dx%d -> %dx%d (%s)\n",im->w,im->h,nw,nh,o->resize);image_free(im);im=r;}}
    char*out=output_name(src,o,override);printf("  Converting: %s -> %s [%s]%s%s%s%s ... ",path_base(src),path_base(out),fmt,o->no_premult?" [no-premult]":" [premult]",o->dither?" [dither]":"",o->swizzle?" [swizzle]":"",o->mipmaps?" [mipmaps]":"");fflush(stdout);Buffer b={0};bool ok=build_tim2(im,f,!o->no_premult,o->dither,o->swizzle,o->mipmaps,&b);image_free(im);if(ok)ok=write_file(out,b.p,b.n);if(ok)printf("done (%.1f KB)\n",b.n/1024.0);else printf("FAILED (%s)\n",g_error);free(b.p);if(!ok){free(out);return NULL;}return out;}
static char*trim(char*s){while(isspace((unsigned char)*s))s++;char*e=s+strlen(s);while(e>s&&isspace((unsigned char)e[-1]))*--e=0;return s;}
static bool split_two(char*line,char**a,char**b){char*s=trim(line);if(!*s||*s=='#')return false;*a=s;while(*s&&!isspace((unsigned char)*s))s++;if(!*s){*b=NULL;return true;}*s++=0;s=trim(s);*b=s;while(*s&&!isspace((unsigned char)*s))s++;*s=0;return true;}
static int run_batch(const Options*o){FILE*f=fopen(o->list,"r");if(!f){fprintf(stderr,"ERROR: cannot open list '%s'\n",o->list);return 1;}if(o->output_dir&&!mkdir_p(o->output_dir)){fprintf(stderr,"ERROR: %s\n",g_error);fclose(f);return 1;}char*line=NULL;size_t cap=0;ssize_t z;int ln=0,ok=0,fail=0;while((z=getline(&line,&cap,f))>=0){(void)z;ln++;char*a,*b;if(!split_two(line,&a,&b))continue;if(!b||fmt_number(b)<0){fprintf(stderr,"  FAILED (line %d): expected <filename> <format>\n",ln);fail++;continue;}char*out=convert_one(a,b,o,NULL);if(out){ok++;free(out);}else fail++;}free(line);fclose(f);printf("\nDone: %d succeeded, %d failed\n",ok,fail);return fail?1:0;}
static int run_diff(const Options*o){if(o->original.n==1&&o->modified.n==1&&is_dir(o->original.v[0])&&is_dir(o->modified.v[0]))return diff_folders(o->original.v[0],o->modified.v[0]);int n=0,safe=0,warn=0,bad=0;if(o->original.n||o->modified.n){if(o->original.n!=o->modified.n){fprintf(stderr,"ERROR: original/modified counts must match\n");return 1;}for(size_t i=0;i<o->original.n;i++){DiffLevel z=diff_files(o->original.v[i],o->modified.v[i],true);n++;safe+=z==DIFF_SAFE;warn+=z==DIFF_WARNING;bad+=z==DIFF_UNSAFE;}}if(o->diff_list){FILE*f=fopen(o->diff_list,"r");if(!f){fprintf(stderr,"ERROR: cannot open diff list\n");return 1;}char*line=NULL;size_t cap=0;ssize_t z;bool header=false;while((z=getline(&line,&cap,f))>=0){(void)z;char*a,*b;if(!split_two(line,&a,&b))continue;if(!header){header=true;continue;}if(!b){fprintf(stderr,"ERROR: malformed diff-list row\n");continue;}DiffLevel q=diff_files(a,b,true);n++;safe+=q==DIFF_SAFE;warn+=q==DIFF_WARNING;bad+=q==DIFF_UNSAFE;}free(line);fclose(f);}if(!n){fprintf(stderr,"ERROR: no files to compare\n");return 1;}if(n>1)diff_summary(n,safe,warn,bad);return bad?1:0;}
int main(int argc,char**argv){Options o;if(!parse_options(argc,argv,&o)){usage(stderr,argv[0]);return 2;}int rc=0;if(o.help){usage(stdout,argv[0]);goto done;}if(o.version){printf("ps2_tim2_tool %s\n",PS2_TOOL_VERSION);goto done;}if(o.diff||o.diff_list){rc=run_diff(&o);goto done;}if(o.extract){char ext[32];snprintf(ext,sizeof(ext),"%s%s",o.extract[0]=='.'?"":".",o.extract);if(strcasecmp(ext,".png")&&strcasecmp(ext,".jpg")&&strcasecmp(ext,".jpeg")&&strcasecmp(ext,".bmp")&&strcasecmp(ext,".tga")&&strcasecmp(ext,".tif")&&strcasecmp(ext,".tiff")&&strcasecmp(ext,".webp")&&strcasecmp(ext,".ppm")){fprintf(stderr,"ERROR: unsupported extract format '%s'\n",o.extract);rc=1;goto done;}if(!o.images.n){fprintf(stderr,"ERROR: provide TIM2 file(s) with --extract\n");rc=1;goto done;}int ok=0,fail=0;for(size_t i=0;i<o.images.n;i++){char*out=NULL;printf("  Extracting: %s ... ",path_base(o.images.v[i]));if(extract_tim2(o.images.v[i],ext,&out)){printf("done -> %s\n",out);free(out);ok++;}else{printf("FAILED (%s)\n",g_error);fail++;}}printf("Done: %d succeeded, %d failed\n",ok,fail);rc=fail?1:0;goto done;}if(o.verify){if(!o.images.n){fprintf(stderr,"ERROR: provide TIM2 file(s) with --verify\n");rc=1;goto done;}bool all=true;for(size_t i=0;i<o.images.n;i++)if(!verify_tim2_file(o.images.v[i]))all=false;puts(all?"All files verified successfully.":"One or more files failed verification.");rc=all?0:1;goto done;}if(o.info){if(!o.images.n){fprintf(stderr,"ERROR: provide TIM2 file(s) with --info\n");rc=1;goto done;}for(size_t i=0;i<o.images.n;i++)if(!tim2_info_file(o.images.v[i])){fprintf(stderr,"FAILED: %s (%s)\n",o.images.v[i],g_error);rc=1;}goto done;}if(o.list){rc=run_batch(&o);goto done;}if(o.list_formats||!o.images.n){list_formats();goto done;}if(fmt_number(o.format)<0){fprintf(stderr,"ERROR: --format is required: 4bit|8bit|16bit|24bit|32bit\n");rc=1;goto done;}if(o.output_dir&&!mkdir_p(o.output_dir)){fprintf(stderr,"ERROR: %s\n",g_error);rc=1;goto done;}if(o.output&&o.images.n>1)fprintf(stderr,"WARNING: --output ignored for multiple inputs\n");printf("Converting %zu file(s) -> [%s]\n\n",o.images.n,o.format);int ok=0,fail=0;for(size_t i=0;i<o.images.n;i++){char*out=convert_one(o.images.v[i],o.format,&o,o.images.n==1?o.output:NULL);if(out){ok++;free(out);}else fail++;}printf("\nDone: %d succeeded, %d failed\n",ok,fail);rc=fail?1:0;done:sv_free(&o.images);sv_free(&o.original);sv_free(&o.modified);return rc;}
