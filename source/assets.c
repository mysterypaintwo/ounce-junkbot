#include "assets.h"
#include "log.h"

#include <switch.h>

#include <SDL_image.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define OTR_MAGIC "JOTR"
#define OTR_VERSION 1
#define OTR_HEADER_SIZE 72
#define OTR_ENTRY_SIZE 152
#define OTR_NAME_SIZE 128
#define OTR_MAX_ENTRIES 8192
#define OTR_MAX_SIZE (256u * 1024u * 1024u)

typedef struct {
    char name[OTR_NAME_SIZE];
    uint64_t offset;
    uint64_t size;
    uint32_t crc32;
    uint32_t flags;
} OtrEntry;

typedef struct {
    uint8_t *file;
    size_t fileSize;
    OtrEntry *entries;
    uint32_t entryCount;
    uint32_t flags;
    char source[32];
    bool romfs;
} AssetState;

static AssetState s_assets;
static char s_packagePath[512] = JB_OTR_FILENAME;
static char s_dirPath[512] = JB_DIR_FILENAME;
static char s_loadError[256];

static void set_load_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_loadError, sizeof(s_loadError), fmt, args);
    va_end(args);
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int bit = 0; bit < 8; bit++)
                v = (v & 1) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
            table[i] = v;
        }
        ready = true;
    }
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

static uint32_t crc32_padded(const void *data, size_t len, size_t paddedLen) {
    uint32_t crc = crc32_update(0xFFFFFFFFu, data, len);
    static const uint8_t zeros[16] = {0};
    while (len < paddedLen) {
        size_t chunk = paddedLen - len;
        if (chunk > sizeof(zeros)) chunk = sizeof(zeros);
        crc = crc32_update(crc, zeros, chunk);
        len += chunk;
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static void derive_asset_paths(const char *arg0) {
    snprintf(s_packagePath, sizeof(s_packagePath), "%s", JB_OTR_FILENAME);
    snprintf(s_dirPath, sizeof(s_dirPath), "%s", JB_DIR_FILENAME);
    if (!arg0 || !*arg0) return;
    const char *slash = strrchr(arg0, '/');
    if (!slash || slash == arg0) return;
    size_t dirLen = (size_t)(slash - arg0);
    if (dirLen == 0 || dirLen > 480) return;
    memcpy(s_packagePath, arg0, dirLen);
    memcpy(s_packagePath + dirLen, "/" JB_OTR_FILENAME, sizeof("/" JB_OTR_FILENAME));
    memcpy(s_dirPath, arg0, dirLen);
    memcpy(s_dirPath + dirLen, "/" JB_DIR_FILENAME, sizeof("/" JB_DIR_FILENAME));
}

const char *assets_package_path(void) { return s_packagePath; }
const char *assets_dir_path(void) { return s_dirPath; }

static const OtrEntry *find_entry(const char *vpath) {
    if (!vpath || !*vpath || !s_assets.file) return NULL;
    for (uint32_t i = 0; i < s_assets.entryCount; i++)
        if (strcmp(s_assets.entries[i].name, vpath) == 0) return &s_assets.entries[i];
    return NULL;
}

static bool load_otr(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { set_load_error("package not found"); return false; }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f); set_load_error("cannot seek package"); return false;
    }
    long sizeLong = ftell(f);
    if (sizeLong < OTR_HEADER_SIZE || sizeLong > (long)OTR_MAX_SIZE) {
        fclose(f); set_load_error("package has invalid size"); return false;
    }
    rewind(f);
    uint8_t *data = (uint8_t *)malloc((size_t)sizeLong);
    if (!data || fread(data, 1, (size_t)sizeLong, f) != (size_t)sizeLong) {
        free(data); fclose(f); set_load_error("cannot read package"); return false;
    }
    fclose(f);

    if (memcmp(data, OTR_MAGIC, 4) != 0 || rd32(data + 4) != OTR_VERSION) {
        LOG("asset package has bad magic/version: %s", path);
        set_load_error("incompatible package version");
        free(data); return false;
    }
    uint32_t count = rd32(data + 12);
    uint64_t toc = rd64(data + 16);
    uint64_t dataOff = rd64(data + 24);
    if (count == 0 || count > OTR_MAX_ENTRIES || toc + (uint64_t)count * OTR_ENTRY_SIZE > (uint64_t)sizeLong ||
        dataOff > (uint64_t)sizeLong) {
        LOG("asset package TOC is invalid: %s", path);
        set_load_error("invalid package table of contents");
        free(data); return false;
    }

    OtrEntry *entries = (OtrEntry *)calloc(count, sizeof(OtrEntry));
    if (!entries) { free(data); return false; }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = data + toc + (uint64_t)i * OTR_ENTRY_SIZE;
        OtrEntry *e = &entries[i];
        memcpy(e->name, p, OTR_NAME_SIZE - 1);
        e->name[OTR_NAME_SIZE - 1] = 0;
        e->offset = rd64(p + OTR_NAME_SIZE);
        e->size = rd64(p + OTR_NAME_SIZE + 8);
        e->crc32 = rd32(p + OTR_NAME_SIZE + 16);
        e->flags = rd32(p + OTR_NAME_SIZE + 20);
        uint64_t paddedSize = (e->size + 15u) & ~15ull;
        if (e->offset > (uint64_t)sizeLong || paddedSize > (uint64_t)sizeLong - e->offset) {
            LOG("asset entry is out of range: %s", e->name);
            set_load_error("package entry is out of range");
            free(entries); free(data); return false;
        }
        if (crc32_padded(data + e->offset, (size_t)e->size,
                         (size_t)paddedSize) != e->crc32) {
            LOG("asset entry CRC failed: %s", e->name);
            set_load_error("package checksum failed");
            free(entries); free(data); return false;
        }
    }

    free(s_assets.file);
    free(s_assets.entries);
    s_assets.file = data;
    s_assets.fileSize = (size_t)sizeLong;
    s_assets.entries = entries;
    s_assets.entryCount = count;
    s_assets.flags = rd32(data + 8);
    memcpy(s_assets.source, data + 40, sizeof(s_assets.source) - 1);
    s_assets.source[sizeof(s_assets.source) - 1] = 0;
    s_loadError[0] = 0;
    LOG("asset package loaded: %s (%u entries, %s)", path, count,
        s_assets.source[0] ? s_assets.source : "unknown source");
    return true;
}

bool assets_init(const char *arg0) {
    free(s_assets.file);
    free(s_assets.entries);
    memset(&s_assets, 0, sizeof(s_assets));
    s_loadError[0] = 0;
    derive_asset_paths(arg0);

    Result rc = romfsInit();
    s_assets.romfs = R_SUCCEEDED(rc);
    LOG("romfsInit: 0x%x (%s)", R_VALUE(rc), s_assets.romfs ? "ok" : "FAILED");

    if (load_otr(s_packagePath)) {
        LOG("asset package: %s", s_packagePath);
        return true;
    }
    static const char *const fallbacks[] = {
        JB_OTR_FILENAME,
        "sdmc:/switch/junkbot/" JB_OTR_FILENAME,
        NULL
    };
    for (int i = 0; fallbacks[i]; i++) {
        if (strcmp(fallbacks[i], s_packagePath) == 0) continue;
        if (load_otr(fallbacks[i])) {
            snprintf(s_packagePath, sizeof(s_packagePath), "%s", fallbacks[i]);
            LOG("asset package: %s", s_packagePath);
            return true;
        }
    }
    LOG("no usable %s; package assets unavailable", JB_OTR_FILENAME);
    return true;
}

void assets_exit(void) {
    bool hadRomfs = s_assets.romfs;
    free(s_assets.file);
    free(s_assets.entries);
    memset(&s_assets, 0, sizeof(s_assets));
    if (hadRomfs) romfsExit();
}

bool assets_has_package(void) { return s_assets.file != NULL; }

const char *assets_package_source(void) {
    return s_assets.source[0] ? s_assets.source : "none";
}

const char *assets_error(void) { return s_loadError; }

void *assets_read(const char *vpath, size_t *outSize) {
    if (!vpath || !*vpath) return NULL;
    const OtrEntry *e = find_entry(vpath);
    if (e) {
        uint8_t *out = (uint8_t *)malloc((size_t)e->size + 1);
        if (!out) return NULL;
        if (e->size) memcpy(out, s_assets.file + e->offset, (size_t)e->size);
        out[e->size] = 0;
        if (outSize) *outSize = (size_t)e->size;
        return out;
    }
    if (!s_assets.romfs) return NULL;
    char path[512];
    snprintf(path, sizeof(path), "romfs:/%s", vpath);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *out = (uint8_t *)malloc((size_t)len + 1);
    if (!out || fread(out, 1, (size_t)len, f) != (size_t)len) {
        free(out); fclose(f); return NULL;
    }
    fclose(f);
    out[len] = 0;
    if (outSize) *outSize = (size_t)len;
    return out;
}

char *assets_read_text(const char *vpath) {
    return (char *)assets_read(vpath, NULL);
}

bool assets_has(const char *vpath) {
    if (find_entry(vpath)) return true;
    if (!s_assets.romfs || !vpath || !*vpath) return false;
    char path[512];
    snprintf(path, sizeof(path), "romfs:/%s", vpath);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static int SDLCALL asset_rw_close(SDL_RWops *ctx) {
    if (ctx) {
        free(ctx->hidden.mem.base);
        SDL_FreeRW(ctx);
    }
    return 0;
}

SDL_RWops *assets_open_rw(const char *vpath) {
    size_t size = 0;
    void *data = assets_read(vpath, &size);
    if (!data) return NULL;
    SDL_RWops *rw = SDL_RWFromMem(data, (int)size);
    if (!rw) { free(data); return NULL; }
    rw->close = asset_rw_close;
    return rw;
}

SDL_Texture *assets_load_texture(SDL_Renderer *renderer, const char *base) {
    static const char *const extensions[] = { ".png", ".tga", ".bmp", NULL };
    char vpath[512];
    size_t len = strlen(base ? base : "");
    bool hasExt = len >= 4 &&
        (strcasecmp(base + len - 4, ".png") == 0 ||
         strcasecmp(base + len - 4, ".tga") == 0 ||
         strcasecmp(base + len - 4, ".bmp") == 0);
    for (int i = 0; extensions[i]; i++) {
        if (hasExt && i > 0) break;
        snprintf(vpath, sizeof(vpath), "%s%s", base, hasExt ? "" : extensions[i]);
        SDL_RWops *rw = assets_open_rw(vpath);
        if (!rw) continue;
        SDL_Texture *tex = IMG_LoadTexture_RW(renderer, rw, 1);
        if (!tex) LOG("image load failed: %s: %s", vpath, IMG_GetError());
        return tex;
    }
    LOG("image not found: %s", base);
    return NULL;
}

bool assets_write(const char *vpath, const void *data, size_t size) {
    if (!vpath || !*vpath) return false;
    char path[512];
    snprintf(path, sizeof(path), "sdmc:/switch/junkbot/otr-stage/%s", vpath);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

int assets_list(const char *prefix, char ***outNames) {
    if (!outNames) return 0;
    *outNames = NULL;
    size_t plen = prefix ? strlen(prefix) : 0;
    int count = 0;
    for (uint32_t i = 0; i < s_assets.entryCount; i++) {
        if (!prefix || strncmp(s_assets.entries[i].name, prefix, plen) == 0) count++;
    }
    if (!count) return 0;
    char **names = (char **)calloc((size_t)count, sizeof(char *));
    if (!names) return 0;
    int n = 0;
    for (uint32_t i = 0; i < s_assets.entryCount; i++) {
        const char *name = s_assets.entries[i].name;
        if (prefix && strncmp(name, prefix, plen) != 0) continue;
        names[n++] = strdup(name);
    }
    *outNames = names;
    return n;
}

void assets_list_free(char **names, int count) {
    if (!names) return;
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}

typedef struct {
    char name[OTR_NAME_SIZE];
    uint8_t *data;
    size_t size;
    size_t paddedSize;
} PackFile;

static int compare_pack_file(const void *a, const void *b) {
    const PackFile *pa = (const PackFile *)a;
    const PackFile *pb = (const PackFile *)b;
    return strcmp(pa->name, pb->name);
}

static bool collect_pack_files(const char *dir, const char *rel,
                               PackFile **list, size_t *count, size_t *capacity,
                               char *error, size_t errorSize) {
    char dirPath[1024];
    if (rel && *rel) snprintf(dirPath, sizeof(dirPath), "%s/%s", dir, rel);
    else snprintf(dirPath, sizeof(dirPath), "%s", dir);
    DIR *d = opendir(dirPath);
    if (!d) {
        snprintf(error, errorSize, "cannot open %s", dirPath);
        return false;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strcmp(ent->d_name, ".DS_Store") == 0 || ent->d_name[0] == '.') continue;
        char childRel[256];
        char full[1024];
        if (rel && *rel) {
            if (snprintf(childRel, sizeof(childRel), "%s/%s", rel, ent->d_name) >= (int)sizeof(childRel) ||
                snprintf(full, sizeof(full), "%s/%s", dir, childRel) >= (int)sizeof(full)) {
                snprintf(error, errorSize, "asset path is too long");
                closedir(d); return false;
            }
        } else {
            snprintf(childRel, sizeof(childRel), "%s", ent->d_name);
            if (snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name) >= (int)sizeof(full)) {
                snprintf(error, errorSize, "asset path is too long");
                closedir(d); return false;
            }
        }
        if (strlen(childRel) >= OTR_NAME_SIZE) {
            snprintf(error, errorSize, "asset path is too long");
            closedir(d); return false;
        }
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!collect_pack_files(dir, childRel, list, count, capacity, error, errorSize)) {
                closedir(d); return false;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        if (*count >= OTR_MAX_ENTRIES) {
            snprintf(error, errorSize, "too many asset files");
            closedir(d); return false;
        }
        if (*count == *capacity) {
            size_t next = (*capacity) ? (*capacity) * 2 : 128;
            PackFile *grown = (PackFile *)realloc(*list, next * sizeof(PackFile));
            if (!grown) { snprintf(error, errorSize, "out of memory"); closedir(d); return false; }
            *list = grown; *capacity = next;
        }
        FILE *f = fopen(full, "rb");
        if (!f) continue;
        uint8_t *buf = (uint8_t *)malloc((size_t)st.st_size + 1);
        if (!buf || fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
            free(buf); fclose(f); continue;
        }
        fclose(f);
        PackFile *pf = &(*list)[(*count)++];
        memset(pf, 0, sizeof(*pf));
        memcpy(pf->name, childRel, strlen(childRel) + 1);
        pf->data = buf;
        pf->size = (size_t)st.st_size;
        pf->paddedSize = (pf->size + 15u) & ~15u;
    }
    closedir(d);
    return true;
}

bool otr_pack_directory(const char *srcDir, const char *outPath,
                        const char *sourceLabel, char *error, size_t errorSize) {
    if (error && errorSize) error[0] = 0;
    PackFile *files = NULL;
    size_t count = 0, capacity = 0;
    if (!collect_pack_files(srcDir, NULL, &files, &count, &capacity, error, errorSize)) {
        for (size_t i = 0; i < count; i++) free(files[i].data);
        free(files);
        return false;
    }
    if (!count) {
        if (error && errorSize) snprintf(error, errorSize, "staging directory is empty");
        free(files); return false;
    }
    qsort(files, count, sizeof(PackFile), compare_pack_file);

    uint64_t tocOffset = OTR_HEADER_SIZE;
    uint64_t dataOffset = tocOffset + (uint64_t)count * OTR_ENTRY_SIZE;
    dataOffset = (dataOffset + 15u) & ~15u;
    uint64_t total = dataOffset;
    for (size_t i = 0; i < count; i++) total += files[i].paddedSize;
    if (total > OTR_MAX_SIZE) {
        if (error && errorSize) snprintf(error, errorSize, "asset package would be too large");
        for (size_t i = 0; i < count; i++) free(files[i].data);
        free(files); return false;
    }

    uint8_t *header = (uint8_t *)calloc((size_t)dataOffset, 1);
    if (!header) {
        if (error && errorSize) snprintf(error, errorSize, "out of memory");
        for (size_t i = 0; i < count; i++) free(files[i].data);
        free(files); return false;
    }
    memcpy(header, OTR_MAGIC, 4);
    wr32(header + 4, OTR_VERSION);
    bool hasIntro = false;
    for (size_t i = 0; i < count; i++)
        if (strcmp(files[i].name, "intro/seq.bin") == 0) hasIntro = true;
    wr32(header + 8, hasIntro ? 1u : 0u);
    wr32(header + 12, (uint32_t)count);
    wr64(header + 16, tocOffset);
    wr64(header + 24, dataOffset);
    snprintf((char *)header + 40, 32, "%s", sourceLabel ? sourceLabel : "user-generated");

    uint64_t cursor = dataOffset;
    for (size_t i = 0; i < count; i++) {
        uint8_t *entry = header + tocOffset + (uint64_t)i * OTR_ENTRY_SIZE;
        memcpy(entry, files[i].name, strlen(files[i].name) + 1);
        wr64(entry + OTR_NAME_SIZE, cursor);
        wr64(entry + OTR_NAME_SIZE + 8, files[i].size);
        wr32(entry + OTR_NAME_SIZE + 16,
             crc32_padded(files[i].data, files[i].size, files[i].paddedSize));
        wr32(entry + OTR_NAME_SIZE + 20, 0);
        cursor += files[i].paddedSize;
    }

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", outPath);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        if (error && errorSize) snprintf(error, errorSize, "cannot create %s", tmp);
        free(header); for (size_t i = 0; i < count; i++) free(files[i].data); free(files);
        return false;
    }
    bool ok = fwrite(header, 1, (size_t)dataOffset, f) == (size_t)dataOffset;
    for (size_t i = 0; ok && i < count; i++) {
        ok = fwrite(files[i].data, 1, files[i].size, f) == files[i].size;
        if (ok && files[i].paddedSize > files[i].size) {
            static const uint8_t zeros[16] = {0};
            size_t pad = files[i].paddedSize - files[i].size;
            ok = fwrite(zeros, 1, pad, f) == pad;
        }
    }
    fclose(f);
    if (ok) {
        remove(outPath);
        if (rename(tmp, outPath) != 0) ok = false;
    }
    if (!ok && error && errorSize) snprintf(error, errorSize, "failed writing %s", outPath);
    free(header);
    for (size_t i = 0; i < count; i++) free(files[i].data);
    free(files);
    return ok;
}
