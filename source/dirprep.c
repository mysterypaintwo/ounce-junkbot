#include "dirprep.h"
#include "assets.h"
#include "jb_asset_names.h"
#include "jb_palettes.h"
#include "log.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#define MAX_CAST 4096
#define MAX_BITMAPS 2048
#define MAX_LEVELS 160
#define MAX_FRAMES 512
#define MAX_LAYERS 8
#define MAX_NAME 128

typedef struct {
    uint32_t member;
    uint32_t section;
    char tag[5];
} KeyRef;

typedef struct {
    uint32_t index;
    char tag[5];
    uint32_t position;
    uint32_t size;
    const uint8_t *data;
} Resource;

typedef struct {
    uint32_t index;
    uint32_t type;
    char name[MAX_NAME];
    const uint8_t *specific;
    uint32_t specificLen;
} CastMember;

typedef struct {
    char key[MAX_NAME];
    int w, h;
    uint8_t *rgba;
} Bitmap;

typedef struct {
    char source[MAX_NAME];
    char points[MAX_NAME];
    int offX, offY;
    uint32_t flags;
    uint8_t key[3];
} RecipeLayer;

typedef struct {
    char name[MAX_NAME];
    int layerCount;
    RecipeLayer layers[MAX_LAYERS];
    int crop[4];
} RecipeFrame;

typedef struct {
    char key[MAX_NAME];
    int16_t regX, regY, scrollX, scrollY;
} PointEntry;

typedef struct {
    char title[MAX_NAME];
    uint32_t member;
} LevelEntry;

typedef struct {
    uint8_t *file;
    size_t size;
    uint32_t version;
    Resource *resources;
    size_t resourceCount;
    KeyRef *keys;
    size_t keyCount;
    CastMember members[MAX_CAST];
    size_t memberCount;
} Director;

typedef struct {
    uint8_t *pixels;
    int w, h;
} Image;

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int16_t be16s(const uint8_t *p) { return (int16_t)be16(p); }
static int16_t le16s(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

static void le16_write(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32_write(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The outer RIFX resource map follows the file's byte order, while CASt,
   BITD, CAS* and KEY* payloads have their own fixed byte order. */
static bool s_outerLittle;

static uint32_t outer32(const uint8_t *p) {
    return s_outerLittle ? le32(p) : be32(p);
}

static void clean_key(const char *src, char *dst, size_t dstSize, bool stripSuffix) {
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < dstSize; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) dst[o++] = (char)tolower(c);
        else dst[o++] = '_';
    }
    dst[o] = 0;
    if (stripSuffix && o >= 3) {
        if (dst[o - 3] == '_' && dst[o - 2] == 's' && dst[o - 1] >= '1' && dst[o - 1] <= '4')
            dst[o - 3] = 0;
    }
    o = strlen(dst);
    while (o > 0 && dst[o - 1] == '_') dst[--o] = 0;
    while (o > 0 && dst[0] == '_') { memmove(dst, dst + 1, o--); }
}

static void audio_key_of(const char *src, char *dst, size_t dstSize) {
    char temp[MAX_NAME];
    snprintf(temp, sizeof(temp), "%s", src);
    size_t len = strlen(temp);
    if (len > 4) {
        if (strcasecmp(temp + len - 4, ".wav") == 0 ||
            strcasecmp(temp + len - 4, ".mp3") == 0) temp[len - 4] = 0;
    }
    clean_key(temp, dst, dstSize, false);
}

static void outer_tag(const uint8_t *p, char *out) {
    if (s_outerLittle) {
        out[0] = (char)p[3]; out[1] = (char)p[2];
        out[2] = (char)p[1]; out[3] = (char)p[0];
    } else {
        memcpy(out, p, 4);
    }
    out[4] = 0;
}

static bool ensure_parent(const char *path) {
    char copy[1200];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(copy, 0777);
        *p = '/';
    }
    return true;
}

static bool write_stage_file(const char *stage, const char *rel,
                             const void *data, size_t size) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", stage, rel);
    ensure_parent(path);
    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG("prep write failed: %s", path);
        return false;
    }
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

static bool copy_internal_to_stage(const char *stage, const char *vpath,
                                   const char *rel) {
    size_t size = 0;
    void *data = assets_read(vpath, &size);
    if (!data) return false;
    bool ok = write_stage_file(stage, rel, data, size);
    free(data);
    return ok;
}

static bool director_load(Director *d, const char *path, char *error, size_t errorSize) {
    memset(d, 0, sizeof(*d));
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(error, errorSize, "cannot open %s", path); return false; }
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    rewind(f);
    if (fileSize < 64) { fclose(f); snprintf(error, errorSize, "movie is too short"); return false; }
    d->file = malloc((size_t)fileSize);
    if (!d->file || fread(d->file, 1, (size_t)fileSize, f) != (size_t)fileSize) {
        free(d->file); d->file = NULL; fclose(f);
        snprintf(error, errorSize, "cannot read movie"); return false;
    }
    fclose(f);
    d->size = (size_t)fileSize;
    const uint8_t *p = d->file;
    if (memcmp(p, "RIFX", 4) == 0) s_outerLittle = false;
    else if (memcmp(p, "XFIR", 4) == 0) s_outerLittle = true;
    else { snprintf(error, errorSize, "unsupported Director archive"); goto fail; }

    char movieTag[5];
    outer_tag(p + 8, movieTag);
    if (strcmp(movieTag, "MV93") != 0 && strcmp(movieTag, "MV97") != 0 &&
        strcmp(movieTag, "MV95") != 0 && strcmp(movieTag, "MV85") != 0 &&
        strcmp(movieTag, "MV07") != 0 && strcmp(movieTag, "MV08") != 0) {
        snprintf(error, errorSize, "unsupported Director movie version");
        goto fail;
    }

    /* The imap resource starts at 0x14: flag, mmap position, director
       version. The mmap entry array begins after the 8-byte resource header
       and the 24-byte mmap header. */
    uint32_t imapPos = 20;
    if (imapPos + 12 > d->size) { snprintf(error, errorSize, "imap not found"); goto fail; }
    uint32_t mmapPos = outer32(p + imapPos + 4);
    d->version = outer32(p + imapPos + 8);

    char mmapTag[5];
    if (mmapPos + 32 > d->size) { snprintf(error, errorSize, "mmap not found"); goto fail; }
    outer_tag(p + mmapPos, mmapTag);
    if (strcmp(mmapTag, "mmap") != 0) { snprintf(error, errorSize, "mmap not found"); goto fail; }
    uint32_t count = outer32(p + mmapPos + 16);
    if (count > 400000 || mmapPos + 32 + (uint64_t)count * 20 > d->size) {
        snprintf(error, errorSize, "mmap table is invalid");
        goto fail;
    }
    d->resources = calloc(count, sizeof(Resource));
    if (!d->resources) { snprintf(error, errorSize, "out of memory"); goto fail; }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + mmapPos + 32 + (uint64_t)i * 20;
        Resource *r = &d->resources[d->resourceCount];
        outer_tag(e, r->tag);
        if (strcmp(r->tag, "free") == 0 || strcmp(r->tag, "junk") == 0) continue;
        r->index = i;
        r->size = outer32(e + 4);
        r->position = outer32(e + 8);
        if (r->position + 8 <= d->size) r->data = p + r->position + 8;
        d->resourceCount++;
    }

    for (size_t i = 0; i < d->resourceCount; i++) {
        if (strcmp(d->resources[i].tag, "KEY*") != 0) continue;
        const uint8_t *key = d->resources[i].data;
        if (!key || d->resources[i].size < 12) break;
        uint32_t used = le32(key + 8);
        if (12 + (uint64_t)used * 12 > d->resources[i].size) break;
        d->keys = calloc(used, sizeof(KeyRef));
        if (!d->keys) { snprintf(error, errorSize, "out of memory"); goto fail; }
        for (uint32_t k = 0; k < used; k++) {
            const uint8_t *entry = key + 12 + (uint64_t)k * 12;
            KeyRef *ref = &d->keys[d->keyCount];
            ref->section = le32(entry);
            ref->member = le32(entry + 4);
            outer_tag(entry + 8, ref->tag);
            d->keyCount++;
        }
        break;
    }
    if (!d->keyCount) { snprintf(error, errorSize, "no KEY* resource table"); goto fail; }
    return true;
fail:
    free(d->file); free(d->resources); free(d->keys);
    memset(d, 0, sizeof(*d));
    return false;
}

static void director_free(Director *d) {
    free(d->file); free(d->resources); free(d->keys);
    memset(d, 0, sizeof(*d));
}

static const Resource *resource_by_section(const Director *d, uint32_t section) {
    for (size_t i = 0; i < d->resourceCount; i++)
        if (d->resources[i].index == section) return &d->resources[i];
    return NULL;
}

static uint32_t linked_section(const Director *d, uint32_t member, const char *tag) {
    for (size_t i = 0; i < d->keyCount; i++)
        if (d->keys[i].member == member && strcmp(d->keys[i].tag, tag) == 0)
            return d->keys[i].section;
    return 0;
}

static const uint8_t *linked_data(const Director *d, uint32_t member, const char *tag,
                                  uint32_t *size) {
    uint32_t section = linked_section(d, member, tag);
    const Resource *r = resource_by_section(d, section);
    if (!r || !r->data) { if (size) *size = 0; return NULL; }
    if (size) *size = r->size;
    return r->data;
}

static bool read_member_name(const uint8_t *info, uint32_t infoLen, char *out, size_t outSize) {
    if (infoLen < 20) return false;
    uint32_t dataOffset = be32(info);
    if (dataOffset == 0 || dataOffset >= infoLen) return false;
    uint32_t pos = dataOffset;
    if (pos + 2 > infoLen) return false;
    uint16_t items = be16(info + pos);
    pos += 2;
    if (items <= 1 || items > 10000 || pos + (uint32_t)items * 4 + 4 > infoLen) return false;
    uint32_t nameOffset = be32(info + pos + 4);          /* offsets[1] */
    pos += (uint32_t)items * 4 + 4;                       /* skip offsets + items length */
    if (nameOffset >= infoLen - pos) return false;
    pos += nameOffset;
    uint8_t len = info[pos++];
    if (len == 0 || pos + len > infoLen) return false;
    size_t o = 0;
    for (uint8_t i = 0; i < len && o + 1 < outSize; i++) {
        unsigned char c = info[pos + i];
        out[o++] = c >= 0x20 && c < 0x7F ? (char)c : '_';
    }
    out[o] = 0;
    return true;
}

static void director_read_members(Director *d) {
    for (size_t i = 0; i < d->resourceCount && d->memberCount < MAX_CAST; i++) {
        const Resource *r = &d->resources[i];
        if (strcmp(r->tag, "CASt") != 0 || !r->data || r->size < 12) continue;
        uint32_t type = be32(r->data);
        uint32_t infoLen = be32(r->data + 4);
        uint32_t dataLen = be32(r->data + 8);
        if (12 + infoLen + dataLen > r->size) continue;
        CastMember *m = &d->members[d->memberCount];
        m->index = r->index;
        m->type = type;
        if (!read_member_name(r->data + 12, infoLen, m->name, sizeof(m->name)))
            snprintf(m->name, sizeof(m->name), "member_%u", r->index);
        m->specific = r->data + 12 + infoLen;
        m->specificLen = dataLen;
        d->memberCount++;
    }
}

static uint8_t *decompress_bitmap(const uint8_t *data, uint32_t size, size_t expected) {
    if (size >= expected) return (uint8_t *)data;
    if (size >= 2 && data[0] == 0x78) {
        uLongf outLen = (uLongf)expected;
        uint8_t *out = malloc(expected);
        if (out && uncompress(out, &outLen, data, (uLong)size) == Z_OK) return out;
        free(out);
    }
    uint8_t *out = calloc(expected ? expected : 1, 1);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    while (i < size && o < expected) {
        uint8_t control = data[i++];
        if (control < 0x80) {
            uint32_t count = control + 1;
            for (uint32_t k = 0; k < count && i < size && o < expected; k++) out[o++] = data[i++];
        } else if (control == 0x80) {
            continue;
        } else {
            uint32_t count = 257 - control;
            if (i < size) {
                uint8_t value = data[i++];
                for (uint32_t k = 0; k < count && o < expected; k++) out[o++] = value;
            }
        }
    }
    return out;
}

static const uint8_t *palette_for(int paletteId) {
    switch (paletteId) {
        case -102: return (const uint8_t *)jb_palette_main;
        case -8: return (const uint8_t *)jb_palette_ui;
        case -1: return (const uint8_t *)jb_palette_lose;
        case -3: return (const uint8_t *)jb_palette_grayscale;
        case -101: return (const uint8_t *)jb_palette_system_win;
        default: return (const uint8_t *)jb_palette_system_mac;
    }
}

static Image decode_bitmap(const uint8_t *data, uint32_t size,
                           int width, int height, int depth, int pitch, int paletteId) {
    Image image = {0};
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return image;
    image.w = width; image.h = height;
    image.pixels = calloc((size_t)width * height * 4, 1);
    if (!image.pixels) return image;
    size_t expected = (size_t)pitch * height;
    bool planes = size < expected;
    uint8_t *raw = decompress_bitmap(data, size, expected);
    if (!raw) { free(image.pixels); image.pixels = NULL; return image; }
    const uint8_t *palette = palette_for(paletteId);
    int scan = depth == 32 ? width : pitch;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t r = 0, g = 0, b = 0;
            if (depth == 8) {
                size_t pos = (size_t)y * scan + x;
                int index = pos < expected ? raw[pos] : 0;
                r = palette[index * 3]; g = palette[index * 3 + 1]; b = palette[index * 3 + 2];
            } else if (depth == 1 || depth == 2 || depth == 4) {
                int perByte = 8 / depth;
                int mask = (1 << depth) - 1;
                size_t pos = (size_t)y * scan + x / perByte;
                uint8_t byte = pos < expected ? raw[pos] : 0;
                int shift = 8 - depth - (x % perByte) * depth;
                int index = ((byte >> shift) & mask) * 255 / mask;
                r = palette[index * 3]; g = palette[index * 3 + 1]; b = palette[index * 3 + 2];
            } else if (depth == 16) {
                uint16_t value;
                if (planes) {
                    size_t hiPos = ((size_t)y * 2) * width + x;
                    size_t loPos = ((size_t)y * 2 + 1) * width + x;
                    uint8_t hi = hiPos < expected ? raw[hiPos] : 0;
                    uint8_t lo = loPos < expected ? raw[loPos] : 0;
                    value = (uint16_t)((hi << 8) | lo);
                } else {
                    size_t pos = (size_t)y * scan + x * 2;
                    uint8_t a = pos + 1 < expected ? raw[pos] : 0;
                    uint8_t b2 = pos + 1 < expected ? raw[pos + 1] : 0;
                    value = (uint16_t)((a << 8) | b2);
                }
                int r5 = (value >> 10) & 0x1F, g5 = (value >> 5) & 0x1F, b5 = value & 0x1F;
                r = (uint8_t)(r5 * 255 / 31); g = (uint8_t)(g5 * 255 / 31); b = (uint8_t)(b5 * 255 / 31);
            } else if (depth == 32) {
                size_t rp = ((size_t)y * 4 + 1) * width + x;
                size_t gp = ((size_t)y * 4 + 2) * width + x;
                size_t bp = ((size_t)y * 4 + 3) * width + x;
                r = rp < expected ? raw[rp] : 0;
                g = gp < expected ? raw[gp] : 0;
                b = bp < expected ? raw[bp] : 0;
            }
            uint8_t *out = image.pixels + ((size_t)y * width + x) * 4;
            out[0] = r; out[1] = g; out[2] = b; out[3] = 255;
        }
    }
    if (raw != data) free(raw);
    return image;
}

static void image_free(Image *image) {
    free(image->pixels);
    image->pixels = NULL; image->w = image->h = 0;
}

static bool is_key(const uint8_t *px, const uint8_t *key) {
    return px[0] == key[0] && px[1] == key[1] && px[2] == key[2];
}

static void make_transparent(Image *image, const uint8_t *key) {
    if (!image->pixels) return;
    int w = image->w, h = image->h;
    uint8_t *seen = calloc((size_t)w * h, 1);
    int *queue = malloc((size_t)w * h * sizeof(int));
    if (!seen || !queue) { free(seen); free(queue); return; }
    int head = 0, tail = 0;
    for (int x = 0; x < w; x++) {
        int top = x, bottom = (h - 1) * w + x;
        if (!seen[top] && is_key(image->pixels + top * 4, key)) { seen[top] = 1; queue[tail++] = top; }
        if (!seen[bottom] && is_key(image->pixels + bottom * 4, key)) { seen[bottom] = 1; queue[tail++] = bottom; }
    }
    for (int y = 0; y < h; y++) {
        int left = y * w, right = y * w + w - 1;
        if (!seen[left] && is_key(image->pixels + left * 4, key)) { seen[left] = 1; queue[tail++] = left; }
        if (!seen[right] && is_key(image->pixels + right * 4, key)) { seen[right] = 1; queue[tail++] = right; }
    }
    while (head < tail) {
        int pos = queue[head++];
        int x = pos % w, y = pos / w;
        int neighbours[4] = { x > 0 ? pos - 1 : -1, x + 1 < w ? pos + 1 : -1,
                              y > 0 ? pos - w : -1, y + 1 < h ? pos + w : -1 };
        for (int n = 0; n < 4; n++) {
            int np = neighbours[n];
            if (np < 0 || seen[np] || !is_key(image->pixels + np * 4, key)) continue;
            seen[np] = 1; queue[tail++] = np;
        }
    }
    for (int i = 0; i < w * h; i++) {
        if (!seen[i]) continue;
        uint8_t *px = image->pixels + i * 4;
        px[0] = px[1] = px[2] = 255; px[3] = 0;
    }
    free(seen); free(queue);
}

static void key_white(Image *image) {
    static const uint8_t white[3] = {255, 255, 255};
    if (!image->pixels) return;
    for (int i = 0; i < image->w * image->h; i++) {
        uint8_t *px = image->pixels + i * 4;
        if (is_key(px, white)) { px[0] = px[1] = px[2] = 255; px[3] = 0; }
    }
}

static bool is_marker(const uint8_t *px) {
    static const uint8_t colors[6][3] = {
        {255,0,0},{0,255,0},{0,0,255},{255,0,255},{0,255,255},{255,255,0}
    };
    for (int i = 0; i < 6; i++)
        if (px[0] == colors[i][0] && px[1] == colors[i][1] &&
            px[2] == colors[i][2]) return true;
    return false;
}

static void strip_markers(Image *image) {
    if (!image->pixels) return;
    int w = image->w, h = image->h;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *px = image->pixels + (y * w + x) * 4;
            if (px[3] == 0 || !is_marker(px)) continue;
            int neighbours = 0;
            if (x > 0 && image->pixels[(y * w + x - 1) * 4 + 3]) neighbours++;
            if (x + 1 < w && image->pixels[(y * w + x + 1) * 4 + 3]) neighbours++;
            if (y > 0 && image->pixels[((y - 1) * w + x) * 4 + 3]) neighbours++;
            if (y + 1 < h && image->pixels[((y + 1) * w + x) * 4 + 3]) neighbours++;
            if (neighbours <= 1) { px[0] = px[1] = px[2] = 255; px[3] = 0; }
        }
    }
}

static void alpha_composite(uint8_t *dst, const uint8_t *src) {
    if (src[3] == 0) return;
    if (src[3] == 255) { memcpy(dst, src, 4); return; }
    uint16_t a = src[3];
    for (int c = 0; c < 3; c++)
        dst[c] = (uint8_t)((src[c] * a + dst[c] * (255 - a)) / 255);
    dst[3] = (uint8_t)(a + dst[3] * (255 - a) / 255);
}

static void crop_image(Image *image, int x, int y, int w, int h) {
    if (!image->pixels || w <= 0 || h <= 0) return;
    /* Pillow's crop pads out-of-bounds pixels with transparency. */
    uint8_t *out = calloc((size_t)w * h * 4, 1);
    if (!out) return;
    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= image->h) continue;
        for (int col = 0; col < w; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= image->w) continue;
            memcpy(out + ((size_t)row * w + col) * 4,
                   image->pixels + ((size_t)sy * image->w + sx) * 4, 4);
        }
    }
    free(image->pixels);
    image->pixels = out; image->w = w; image->h = h;
}

static void crop_to_alpha(Image *image) {
    if (!image->pixels) return;
    int minX = image->w, minY = image->h, maxX = -1, maxY = -1;
    for (int y = 0; y < image->h; y++) {
        for (int x = 0; x < image->w; x++) {
            if (image->pixels[((size_t)y * image->w + x) * 4 + 3] == 0) continue;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    if (maxX < 0) return;
    crop_image(image, minX, minY, maxX - minX + 1, maxY - minY + 1);
}

static bool write_tga(const char *stage, const char *rel, const Image *image) {
    if (!image->pixels) return false;
    uint8_t header[18] = {0};
    header[2] = 2;
    header[12] = (uint8_t)(image->w & 0xFF); header[13] = (uint8_t)(image->w >> 8);
    header[14] = (uint8_t)(image->h & 0xFF); header[15] = (uint8_t)(image->h >> 8);
    header[16] = 32;
    header[17] = 0x20;
    size_t size = 18 + (size_t)image->w * image->h * 4;
    uint8_t *data = malloc(size);
    if (!data) return false;
    memcpy(data, header, sizeof(header));
    /* TGA true-colour pixels are BGR[A], while the compositor works in RGBA. */
    for (size_t i = 0; i < (size_t)image->w * image->h; i++) {
        const uint8_t *src = image->pixels + i * 4;
        uint8_t *dst = data + 18 + i * 4;
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
    }
    bool ok = write_stage_file(stage, rel, data, size);
    free(data);
    return ok;
}

static void bitmap_add(Bitmap *list, int *count, const char *name, Image *image) {
    if (*count >= MAX_BITMAPS || !image->pixels) return;
    Bitmap *b = &list[*count];
    clean_key(name, b->key, sizeof(b->key), false);
    bool duplicate = false;
    for (int i = 0; i < *count; i++) if (strcmp(list[i].key, b->key) == 0) { duplicate = true; break; }
    if (duplicate) { image_free(image); return; }
    b->w = image->w; b->h = image->h; b->rgba = image->pixels;
    (*count)++;
    image->pixels = NULL;
}

static const Bitmap *bitmap_find(const Bitmap *list, int count, const char *key) {
    for (int i = 0; i < count; i++) if (strcmp(list[i].key, key) == 0) return &list[i];
    return NULL;
}

static int load_points(PointEntry *entries, int max) {
    size_t size = 0;
    uint8_t *data = assets_read("manifests/points.bin", &size);
    if (!data || size < 8 || memcmp(data, "JBP1", 4) != 0) { free(data); return 0; }
    uint32_t count = le32(data + 4);
    if (count > (uint32_t)max) count = (uint32_t)max;
    const uint8_t *cursor = data + 8;
    int loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (cursor + 12 > data + size) break;
        uint32_t len = le32(cursor);
        PointEntry *e = &entries[loaded];
        e->regX = le16s(cursor + 4);
        e->regY = le16s(cursor + 6);
        e->scrollX = le16s(cursor + 8);
        e->scrollY = le16s(cursor + 10);
        cursor += 12;
        if (cursor + len > data + size || len >= sizeof(e->key)) break;
        memcpy(e->key, cursor, len); e->key[len] = 0; cursor += len;
        loaded++;
    }
    free(data);
    return loaded;
}

static const PointEntry *point_find(const PointEntry *list, int count, const char *key) {
    for (int i = 0; i < count; i++) if (strcmp(list[i].key, key) == 0) return &list[i];
    return NULL;
}

static int load_recipes(const char *vpath, RecipeFrame *frames, int max) {
    size_t size = 0;
    uint8_t *data = assets_read(vpath, &size);
    if (!data || size < 8 || memcmp(data, "JBR1", 4) != 0) { free(data); return 0; }
    uint32_t count = le32(data + 4);
    if (count > (uint32_t)max) count = (uint32_t)max;
    const uint8_t *cursor = data + 8;
    int loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (cursor + 8 > data + size) break;
        uint32_t nameLen = le32(cursor), layerCount = le32(cursor + 4);
        cursor += 8;
        if (cursor + nameLen > data + size || nameLen >= MAX_NAME) break;
        RecipeFrame *frame = &frames[loaded];
        memset(frame, 0, sizeof(*frame));
        memcpy(frame->name, cursor, nameLen); cursor += nameLen;
        frame->layerCount = layerCount > MAX_LAYERS ? MAX_LAYERS : (int)layerCount;
        for (int layer = 0; layer < frame->layerCount; layer++) {
            if (cursor + 24 > data + size) break;
            uint32_t srcLen = le32(cursor), ptsLen = le32(cursor + 4);
            RecipeLayer *l = &frame->layers[layer];
            l->offX = (int32_t)le32(cursor + 8);
            l->offY = (int32_t)le32(cursor + 12);
            l->flags = le32(cursor + 16);
            l->key[0] = cursor[21]; l->key[1] = cursor[22]; l->key[2] = cursor[23];
            cursor += 24;
            if (cursor + srcLen + ptsLen > data + size) break;
            if (srcLen >= sizeof(l->source)) srcLen = sizeof(l->source) - 1;
            if (ptsLen >= sizeof(l->points)) ptsLen = sizeof(l->points) - 1;
            memcpy(l->source, cursor, srcLen); l->source[srcLen] = 0; cursor += srcLen;
            memcpy(l->points, cursor, ptsLen); l->points[ptsLen] = 0; cursor += ptsLen;
        }
        if (cursor + 16 > data + size) break;
        for (int c = 0; c < 4; c++) frame->crop[c] = (int32_t)le32(cursor + c * 4);
        cursor += 16;
        loaded++;
    }
    free(data);
    return loaded;
}

static Image compose_frame(const RecipeFrame *frame, const Bitmap *bitmaps,
                           int bitmapCount, const PointEntry *points, int pointCount) {
    Image layers[MAX_LAYERS] = {0};
    int px[MAX_LAYERS], py[MAX_LAYERS];
    int maxX = 0, maxY = 0;
    for (int i = 0; i < frame->layerCount; i++) {
        const RecipeLayer *spec = &frame->layers[i];
        const Bitmap *bitmap = bitmap_find(bitmaps, bitmapCount, spec->source);
        if (!bitmap) { for (int k = 0; k < i; k++) image_free(&layers[k]); return (Image){0}; }
        Image image = {0};
        image.w = bitmap->w; image.h = bitmap->h;
        image.pixels = malloc((size_t)image.w * image.h * 4);
        if (!image.pixels) return (Image){0};
        memcpy(image.pixels, bitmap->rgba, (size_t)image.w * image.h * 4);
        make_transparent(&image, spec->key);
        if (spec->flags & 1) key_white(&image);
        layers[i] = image;
        const PointEntry *point = point_find(points, pointCount, spec->points);
        px[i] = (point ? point->regX : 0) + spec->offX;
        py[i] = (point ? point->regY : 0) + spec->offY;
        if (px[i] > maxX) maxX = px[i];
        if (py[i] > maxY) maxY = py[i];
    }
    int minOffsetX = 0, minOffsetY = 0;
    int baseW = 0, baseH = 0;
    for (int i = 0; i < frame->layerCount; i++) {
        int ox = maxX - px[i], oy = maxY - py[i];
        if (i == 0 || ox < minOffsetX) minOffsetX = ox;
        if (i == 0 || oy < minOffsetY) minOffsetY = oy;
        if (ox + layers[i].w > baseW) baseW = ox + layers[i].w;
        if (oy + layers[i].h > baseH) baseH = oy + layers[i].h;
    }
    baseW = baseW > minOffsetX ? baseW - minOffsetX : 1;
    baseH = baseH > minOffsetY ? baseH - minOffsetY : 1;
    Image out = {0};
    out.w = baseW; out.h = baseH;
    out.pixels = calloc((size_t)baseW * baseH * 4, 1);
    if (!out.pixels) { for (int i = 0; i < frame->layerCount; i++) image_free(&layers[i]); return out; }
    for (int i = 0; i < frame->layerCount; i++) {
        int dx = maxX - px[i] - minOffsetX;
        int dy = maxY - py[i] - minOffsetY;
        for (int y = 0; y < layers[i].h; y++) {
            int ty = dy + y;
            if (ty < 0 || ty >= out.h) continue;
            for (int x = 0; x < layers[i].w; x++) {
                int tx = dx + x;
                if (tx < 0 || tx >= out.w) continue;
                alpha_composite(out.pixels + ((size_t)ty * out.w + tx) * 4,
                                layers[i].pixels + ((size_t)y * layers[i].w + x) * 4);
            }
        }
    }
    for (int i = 0; i < frame->layerCount; i++) image_free(&layers[i]);
    if (frame->crop[2] > 0 && frame->crop[3] > 0)
        crop_image(&out, frame->crop[0], frame->crop[1], frame->crop[2], frame->crop[3]);
    else
        crop_to_alpha(&out);
    strip_markers(&out);
    for (int i = 0; i < out.w * out.h; i++)
        if (out.pixels[i * 4 + 3] == 0) {
            out.pixels[i * 4] = 255; out.pixels[i * 4 + 1] = 255; out.pixels[i * 4 + 2] = 255;
        }
    return out;
}

typedef struct {
    char name[MAX_NAME];
    Image image;
    int x, y;
} AtlasFrame;

static int compare_atlas_frame(const void *a, const void *b) {
    const AtlasFrame *fa = (const AtlasFrame *)a;
    const AtlasFrame *fb = (const AtlasFrame *)b;
    if (fb->image.h != fa->image.h) return fb->image.h - fa->image.h;
    if (fb->image.w != fa->image.w) return fb->image.w - fa->image.w;
    return strcmp(fa->name, fb->name);
}

static bool write_atlas(const char *stage, const char *base, RecipeFrame *frames,
                        int frameCount, const Bitmap *bitmaps, int bitmapCount,
                        const PointEntry *points, int pointCount) {
    AtlasFrame *atlasFrames = calloc((size_t)(frameCount > 0 ? frameCount : 1),
                                     sizeof(AtlasFrame));
    if (!atlasFrames) return false;
    int used = 0;
    for (int i = 0; i < frameCount; i++) {
        Image image = compose_frame(&frames[i], bitmaps, bitmapCount, points, pointCount);
        if (!image.pixels) continue;
        memcpy(atlasFrames[used].name, frames[i].name, MAX_NAME);
        atlasFrames[used].name[MAX_NAME - 1] = 0;
        atlasFrames[used].image = image;
        used++;
    }
    qsort(atlasFrames, (size_t)used, sizeof(AtlasFrame), compare_atlas_frame);
    int x = 0, y = 0, rowHeight = 0;
    for (int i = 0; i < used; i++) {
        if (x + atlasFrames[i].image.w + 1 > 2048) { x = 0; y += rowHeight + 1; rowHeight = 0; }
        atlasFrames[i].x = x; atlasFrames[i].y = y;
        x += atlasFrames[i].image.w + 1;
        if (atlasFrames[i].image.h > rowHeight) rowHeight = atlasFrames[i].image.h;
    }
    Image atlas = {0};
    atlas.w = 2048; atlas.h = y + rowHeight + 1;
    atlas.pixels = malloc((size_t)atlas.w * atlas.h * 4);
    if (!atlas.pixels) { free(atlasFrames); return false; }
    for (size_t i = 0; i < (size_t)atlas.w * atlas.h; i++) {
        atlas.pixels[i * 4] = 255;
        atlas.pixels[i * 4 + 1] = 255;
        atlas.pixels[i * 4 + 2] = 255;
        atlas.pixels[i * 4 + 3] = 0;
    }
    for (int i = 0; i < used; i++) {
        AtlasFrame *fr = &atlasFrames[i];
        for (int row = 0; row < fr->image.h; row++) {
            size_t dst = ((size_t)(fr->y + row) * atlas.w + fr->x) * 4;
            memcpy(atlas.pixels + dst, fr->image.pixels + (size_t)row * fr->image.w * 4,
                   (size_t)fr->image.w * 4);
        }
    }
    char rel[256];
    snprintf(rel, sizeof(rel), "%s.tga", base);
    bool ok = write_tga(stage, rel, &atlas);
    snprintf(rel, sizeof(rel), "%s.tsv", base);
    size_t tsvSize = (size_t)used * 128 + 1;
    char *tsv = calloc(tsvSize, 1);
    size_t pos = 0;
    for (int i = 0; i < used; i++) {
        AtlasFrame *fr = &atlasFrames[i];
        pos += (size_t)snprintf(tsv + pos, tsvSize - pos, "%s\t%d\t%d\t%d\t%d\n",
                                fr->name, fr->x, fr->y, fr->image.w, fr->image.h);
    }
    ok = write_stage_file(stage, rel, tsv, pos) && ok;
    free(tsv);
    LOG("prep atlas %s: %d/%d frames", base, used, frameCount);
    for (int i = 0; i < used; i++) image_free(&atlasFrames[i].image);
    free(atlasFrames);
    image_free(&atlas);
    return ok;
}

static bool extract_bitmaps(Director *d, Bitmap *bitmaps, int *count,
                            JbPrepProgress progress, void *user) {
    for (size_t i = 0; i < d->memberCount; i++) {
        CastMember *m = &d->members[i];
        if (m->type != 1 || m->specificLen < 24) continue;
        const uint8_t *s = m->specific;
        int rawPitch = be16(s);
        int top = be16s(s + 2), left = be16s(s + 4);
        int bottom = be16s(s + 6), right = be16s(s + 8);
        int width = right - left, height = bottom - top;
        int depth = d->version < 1200 ? (s[20] ? s[20] : 1) : (s[23] ? s[23] : 1);
        int paletteId = -1;
        if (d->version >= 1200 && m->specificLen >= 28) paletteId = be16s(s + 26) - 1;
        int pitch = rawPitch & 0x3FFF;
        uint32_t dataSize = 0;
        const uint8_t *data = linked_data(d, m->index, "BITD", &dataSize);
        if (!data || width <= 0 || height <= 0) continue;
        Image image = decode_bitmap(data, dataSize, width, height, depth, pitch, paletteId);
        bitmap_add(bitmaps, count, m->name, &image);
        if (progress && i % 200 == 0) progress("bitmaps", (int)i, (int)d->memberCount, user);
    }
    return *count > 0;
}

static bool write_wav(const char *stage, const char *rel, const uint8_t *pcm,
                      uint32_t pcmSize, uint32_t rate) {
    if (pcmSize > 0xFFFFF000u) return false;
    uint8_t header[44] = {0};
    memcpy(header, "RIFF", 4);
    le32_write(header + 4, 36 + pcmSize);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    le32_write(header + 16, 16);
    le16_write(header + 20, 1);
    le16_write(header + 22, 1);
    le32_write(header + 24, rate);
    le32_write(header + 28, rate * 2);
    le16_write(header + 32, 2);
    le16_write(header + 34, 16);
    memcpy(header + 36, "data", 4);
    le32_write(header + 40, pcmSize);
    uint8_t *out = malloc(sizeof(header) + pcmSize);
    if (!out) return false;
    memcpy(out, header, sizeof(header));
    memcpy(out + sizeof(header), pcm, pcmSize);
    bool ok = write_stage_file(stage, rel, out, sizeof(header) + pcmSize);
    free(out);
    return ok;
}

static const uint8_t *find_snd_payload(const Director *d, uint32_t member,
                                       uint32_t *outSize, uint32_t *outRate) {
    uint32_t section = linked_section(d, member, "snd ");
    const Resource *r = resource_by_section(d, section);
    if (!r || !r->data) return NULL;
    const uint8_t *p = d->file + r->position + 8;
    /* The outer `snd ` resource is a zero-length header: the nested sndH/sndS
       chunks are parsed directly from the file, exactly like dir_extract.py. */
    size_t available = d->size > (size_t)r->position + 8
        ? d->size - ((size_t)r->position + 8) : 0;
    size_t remaining = available < (1u << 20) ? available : (1u << 20);
    size_t pos = 0;
    while (pos + 8 <= remaining) {
        char tag[5] = { (char)p[pos + 3], (char)p[pos + 2], (char)p[pos + 1], (char)p[pos], 0 };
        uint32_t len = le32(p + pos + 4);
        if ((strcmp(tag, "sndS") == 0 || strcmp(tag, "sndX") == 0) && pos + 8 + len <= remaining) {
            *outSize = len;
            if (outRate) *outRate = 16000; /* the original Director samples */
            return p + pos + 8;
        }
        if (len > remaining) break;
        pos += 8 + len;
    }
    return NULL;
}

/* Match the host package's per-asset SFX trims when prep runs on the console. */
static double sfx_gain_db(const char *output) {
    static const struct { const char *name; double db; } trims[] = {
        { "eat1", -2.0 }, { "garbage1", -2.0 }, { "voice_ouch", -2.0 },
        { "drip1", -2.0 }, { "drip2", -2.0 }, { "drip3", -2.0 },
        { "electricity1", -1.5 }, { "voice_uhoh", -2.0 },
        { "h_button1", -2.0 }, { "h_powerup3", -3.0 },
    };
    for (size_t i = 0; i < sizeof(trims) / sizeof(trims[0]); i++)
        if (strcmp(output, trims[i].name) == 0) return trims[i].db;
    return 0.0;
}

static void apply_wav_gain(uint8_t *pcm, uint32_t pcmSize, double db) {
    if (fabs(db) < 0.001 || pcmSize < 2) return;
    double scale = pow(10.0, db / 20.0);
    int16_t *samples = (int16_t *)pcm;
    uint32_t count = pcmSize / 2;
    for (uint32_t i = 0; i < count; i++) {
        double v = (double)samples[i] * scale;
        if (v > 32767.0) v = 32767.0;
        else if (v < -32768.0) v = -32768.0;
        samples[i] = (int16_t)(v >= 0 ? v + 0.5 : v - 0.5);
    }
}

static void extract_audio(Director *d, const char *stage) {
    for (size_t i = 0; i < d->memberCount; i++) {
        CastMember *m = &d->members[i];
        if (m->type != 6) continue;
        char key[MAX_NAME];
        audio_key_of(m->name, key, sizeof(key));
        const char *output = NULL;
        bool music = false;
        for (size_t n = 0; n < JB_SFX_NAME_COUNT; n++)
            if (strcmp(key, jb_sfx_names[n].key) == 0) { output = jb_sfx_names[n].output; break; }
        if (!output) {
            for (size_t n = 0; n < JB_MUSIC_NAME_COUNT; n++)
                if (strcmp(key, jb_music_names[n].key) == 0) {
                    output = jb_music_names[n].output; music = true; break;
                }
        }
        if (!output) continue;
        uint32_t pcmSize = 0, rate = 16000;
        const uint8_t *pcm = find_snd_payload(d, m->index, &pcmSize, &rate);
        if (pcm && pcmSize > 4) {
            uint32_t pcmLen = pcmSize & ~1u;
            uint8_t *swapped = malloc(pcmLen ? pcmLen : 1);
            if (swapped) {
                for (uint32_t k = 0; k + 1 < pcmLen; k += 2) {
                    swapped[k] = pcm[k + 1];
                    swapped[k + 1] = pcm[k];
                }
                if (!music) apply_wav_gain(swapped, pcmLen, sfx_gain_db(output));
                char rel[256];
                snprintf(rel, sizeof(rel), "audio/%s/%s.wav",
                         music ? "music" : "sound-effects", output);
                write_wav(stage, rel, swapped, pcmLen, rate);
                free(swapped);
            }
        }
        uint32_t mp3Size = 0;
        const uint8_t *mp3 = linked_data(d, m->index, "ediM", &mp3Size);
        if (mp3 && mp3Size > 2) {
            uint32_t start = 0;
            while (start + 1 < mp3Size && !(mp3[start] == 0xFF && (mp3[start + 1] & 0xE0) == 0xE0)) start++;
            uint32_t end = mp3Size;
            while (end > start && mp3[end - 1] == 0xFF) end--;
            if (end > start) {
                char rel[256];
                snprintf(rel, sizeof(rel), "audio/%s/%s.mp3",
                         music ? "music" : "sound-effects", output);
                write_stage_file(stage, rel, mp3 + start, end - start);
            }
        }
    }
}

static char *extract_text(const uint8_t *data, uint32_t size) {
    if (!data) return NULL;
    const uint8_t *start = NULL;
    for (uint32_t i = 0; i + 8 <= size; i++)
        if (data[i] == ',' && memcmp(data + i + 1, "[info]", 6) == 0) { start = data + i + 1; break; }
    if (!start) return NULL;
    uint32_t len = 0;
    while (start + len < data + size) {
        uint8_t c = start[len];
        if (c == 9 || c == 10 || c == 13 || c == 0x18 || c >= 32) len++;
        else break;
    }
    char *text = malloc((size_t)len + 1);
    if (!text) return NULL;
    memcpy(text, start, len); text[len] = 0;
    return text;
}

static const char *text_title(const char *text) {
    const char *line = text;
    while (line && *line) {
        if (strncmp(line, "title=", 6) == 0) return line + 6;
        const char *cr = strchr(line, '\r');
        const char *lf = strchr(line, '\n');
        line = (cr && (!lf || cr < lf)) ? cr + 1 : (lf ? lf + 1 : NULL);
    }
    return NULL;
}

static void safe_filename(const char *title, char *out, size_t outSize) {
    size_t o = 0;
    for (const char *p = title; *p && o + 1 < outSize; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '/' || c == '\\' || c == ':') out[o++] = '_';
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static int extract_levels(Director *d, const char *stage, LevelEntry *levels, int max) {
    int count = 0;
    for (size_t i = 0; i < d->memberCount && count < max; i++) {
        CastMember *m = &d->members[i];
        if (m->type != 15) continue;
        uint32_t size = 0;
        const uint8_t *data = linked_data(d, m->index, "XMED", &size);
        char *text = extract_text(data, size);
        if (!text) continue;
        const char *title = text_title(text);
        bool valid = title && *title && strstr(text, "[playfield]") != NULL &&
                     strcmp(title, "loading level idea 2") != 0;
        if (valid) {
            while (*title == ' ' || *title == '\t') title++;
            const char *end = title;
            while (*end && *end != '\r' && *end != '\n') end++;
            while (end > title && (end[-1] == ' ' || end[-1] == '\t')) end--;
            char name[256];
            snprintf(name, sizeof(name), "%.*s", (int)(end - title), title);
            char file[256];
            safe_filename(name, file, sizeof(file));
            char rel[320];
            snprintf(rel, sizeof(rel), "levels/%s.txt", file);
            write_stage_file(stage, rel, text, strlen(text));
            size_t titleLen = strlen(name);
            if (titleLen >= MAX_NAME) titleLen = MAX_NAME - 1;
            memcpy(levels[count].title, name, titleLen);
            levels[count].title[titleLen] = 0;
            levels[count].member = m->index;
            count++;
        }
        free(text);
    }
    return count;
}

static void write_level_listing(Director *d, const char *stage,
                                const LevelEntry *levels, int levelCount) {
    char **lists[32] = {0};
    int listCounts[32] = {0};
    int listCount = 0;
    for (size_t i = 0; i < d->resourceCount && listCount < 32; i++) {
        const Resource *r = &d->resources[i];
        if (strcmp(r->tag, "CAS*") != 0 || !r->data) continue;
        uint32_t items = r->size / 4;
        if (!items || items > 4096) continue;
        char **ids = calloc(items, sizeof(char *));
        if (!ids) continue;
        bool allLevels = true;
        int used = 0;
        for (uint32_t k = 0; k < items; k++) {
            uint32_t member = be32(r->data + k * 4);
            const LevelEntry *match = NULL;
            for (int n = 0; n < levelCount; n++)
                if (levels[n].member == member) { match = &levels[n]; break; }
            if (!match) { allLevels = false; break; }
            ids[used++] = (char *)match->title;
        }
        if (allLevels && used) {
            lists[listCount] = ids; listCounts[listCount] = used; listCount++;
        } else free(ids);
    }
    size_t size = 1;
    for (int l = 0; l < listCount; l++)
        for (int i = 0; i < listCounts[l]; i++) size += strlen(lists[l][i]) + 1;
    char *text = calloc(size + 1, 1);
    if (text && listCount) {
        size_t pos = 0;
        for (int i = 0; i < listCounts[0]; i++)
            pos += (size_t)snprintf(text + pos, size - pos, "%s\n", lists[0][i]);
        write_stage_file(stage, "levels/_LEVEL_LISTING.txt", text, pos);
    } else {
        copy_internal_to_stage(stage, "manifests/level_listing.txt", "levels/_LEVEL_LISTING.txt");
    }
    for (int l = 0; l < listCount; l++) free(lists[l]);
    free(text);
}

bool dirprep_extract(const char *dirPath, const char *stageDir,
                     JbPrepProgress progress, void *user,
                     char *error, size_t errorSize) {
    if (error && errorSize) error[0] = 0;
    Director d;
    if (!director_load(&d, dirPath, error, errorSize)) return false;
    director_read_members(&d);
    if (progress) progress("cast", (int)d.memberCount, (int)d.memberCount, user);

    static Bitmap bitmaps[MAX_BITMAPS];
    static PointEntry points[2048];
    static RecipeFrame frames[MAX_FRAMES];
    static LevelEntry levels[MAX_LEVELS];
    int bitmapCount = 0;
    int pointCount = load_points(points, 2048);
    bool ok = true;

    ok = extract_bitmaps(&d, bitmaps, &bitmapCount, progress, user) && ok;
    if (progress) progress("menus", 0, 1, user);
    static const uint8_t white[3] = {255, 255, 255};
    for (size_t i = 0; i < JB_MENU_ART_COUNT && ok; i++) {
        const Bitmap *bitmap = bitmap_find(bitmaps, bitmapCount, jb_menu_art[i].source);
        if (!bitmap) continue;
        Image image = {0};
        image.w = bitmap->w; image.h = bitmap->h;
        image.pixels = malloc((size_t)image.w * image.h * 4);
        if (!image.pixels) continue;
        memcpy(image.pixels, bitmap->rgba, (size_t)image.w * image.h * 4);
        make_transparent(&image, white);
        key_white(&image);
        strip_markers(&image);
        char rel[256];
        snprintf(rel, sizeof(rel), "menus/%s.tga", jb_menu_art[i].output);
        ok = write_tga(stageDir, rel, &image) && ok;
        image_free(&image);
    }

    static const struct {
        const char *manifest;
        const char *atlas;
    } atlasGroups[] = {
        { "manifests/sprites.bin", "sprites/sprites" },
        { "manifests/backgrounds.bin", "sprites/backgrounds" },
        { "manifests/undercover_sprites.bin", "sprites/undercover/sprites" },
        { "manifests/undercover_backgrounds.bin", "sprites/undercover/backgrounds" },
    };
    for (size_t g = 0; g < sizeof(atlasGroups) / sizeof(atlasGroups[0]); g++) {
        int frameCount = load_recipes(atlasGroups[g].manifest, frames, MAX_FRAMES);
        LOG("prep recipes %s: %d frames", atlasGroups[g].manifest, frameCount);
        ok = write_atlas(stageDir, atlasGroups[g].atlas, frames, frameCount,
                         bitmaps, bitmapCount, points, pointCount) && ok;
    }

    if (progress) progress("audio", 0, 1, user);
    extract_audio(&d, stageDir);
    static const uint8_t silence[3200] = {0};
    write_wav(stageDir, "audio/sound-effects/undercover/laser_hit.wav",
              silence, sizeof(silence), 16000);
    write_wav(stageDir, "audio/sound-effects/undercover/teleport.wav",
              silence, sizeof(silence), 16000);

    if (progress) progress("levels", 0, 1, user);
    int levelCount = extract_levels(&d, stageDir, levels, MAX_LEVELS);
    write_level_listing(&d, stageDir, levels, levelCount);
    if (progress) progress("done", 1, 1, user);

    for (int i = 0; i < bitmapCount; i++) free(bitmaps[i].rgba);
    director_free(&d);
    if (!ok && error && errorSize) snprintf(error, errorSize, "one or more assets failed to write");
    return ok;
}
