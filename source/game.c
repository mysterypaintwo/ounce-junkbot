#include "game.h"
#include "audio.h"
#include "render.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dirent.h>

Level g_level;
int g_moves = 0;
int g_frameCounter = 0;
bool g_paused = false;
int g_result = 0;
int g_pointerWorldX = 0, g_pointerWorldY = 0;
char g_levelNames[JB_MAX_LEVELS][256];
int g_levelCount = 0;
WindColumn g_winds[32];
int g_windCount = 0;
LaserBeam g_laserBeams[32];
int g_laserBeamCount = 0;

typedef enum {
    F_ANY,
    F_NOT_DROPLET,
    F_NOT_BIN_OR_DROPLET,
    F_NOT_BIN_OR_DROPLET_OR_ENEMY,
    F_NOT_DROPLET_OR_JUNKBOT,
    F_BIN,
    F_JUMP,
    F_CRATE
} Filter;

static char s_currentTitle[256];
/* Title screen runs the real level for animation only: no win, no SFX, no score. */
static bool s_ambient = false;
static Entity s_bounds;
static Entity *s_dragging[64];
static int s_dragCount = 0;
static GrabOptions s_pending;
static int s_pendingAtY = 0;
static bool s_hasPending = false;

/* Snapshot of the board taken just before the most recent grab. Entities are
   plain value structs (no owned pointers), so a memcpy is enough. */
static Entity s_undoEntities[JB_MAX_ENTITIES];
static int s_undoCount = 0;
static bool s_hasUndo = false;
/* Move count before the current grab. A move is charged when the piece is
   picked up, so an undo (or an invalid drop) has to roll this value back. */
static int s_grabStartMoves = 0;

/* Multi-level undo: each successful move pushes the pre-move board and move
   count. Up to 500 moves can be undone; snapshots are allocated at the actual
   entity count so a typical level costs only a few KB per move. */
#define JB_UNDO_MAX 500
typedef struct {
    Entity *entities;
    int count;
    int moves;
} UndoSnapshot;
static UndoSnapshot s_undoHistory[JB_UNDO_MAX];
static int s_historyStart = 0;
static int s_historyCount = 0;

static void undo_history_clear(void) {
    for (int i = 0; i < s_historyCount; i++) {
        free(s_undoHistory[(s_historyStart + i) % JB_UNDO_MAX].entities);
    }
    s_historyStart = 0;
    s_historyCount = 0;
}

static void undo_history_push(const Entity *src, int count, int moves) {
    if (!src || count <= 0) return;
    if (s_historyCount == JB_UNDO_MAX) {
        int oldest = s_historyStart;
        free(s_undoHistory[oldest].entities);
        s_historyStart = (s_historyStart + 1) % JB_UNDO_MAX;
        s_historyCount--;
    }
    int idx = (s_historyStart + s_historyCount) % JB_UNDO_MAX;
    Entity *copy = malloc(sizeof(Entity) * (size_t)count);
    if (!copy) return;
    memcpy(copy, src, sizeof(Entity) * (size_t)count);
    s_undoHistory[idx].entities = copy;
    s_undoHistory[idx].count = count;
    s_undoHistory[idx].moves = moves;
    s_historyCount++;
}

static bool undo_history_pop(Entity *dst, int capacity, int *count, int *moves) {
    if (s_historyCount <= 0) return false;
    int idx = (s_historyStart + s_historyCount - 1) % JB_UNDO_MAX;
    UndoSnapshot *snap = &s_undoHistory[idx];
    int n = snap->count > capacity ? capacity : snap->count;
    memcpy(dst, snap->entities, sizeof(Entity) * (size_t)n);
    *count = n;
    *moves = snap->moves;
    free(snap->entities);
    snap->entities = NULL;
    s_historyCount--;
    return true;
}

int game_undo_depth(void) { return s_historyCount; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int signi(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }
static int snap(int v, int grid) { return (v / grid) * grid; }

static char *skip_bom(char *s) {
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        return s + 3;
    return s;
}

static void strip_cr(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

static void lower_str(char *dst, const char *src, size_t dstSize) {
    size_t i;
    for (i = 0; i + 1 < dstSize && src[i]; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = 0;
    return s;
}

static char *read_text_file(const char *path, long *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[len] = 0;
    if (outLen) *outLen = len;
    return buf;
}

static void normalize_title(const char *src, char *dst, size_t dstSize) {
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < dstSize; p++) {
        if (isalnum((unsigned char)*p)) dst[o++] = (char)tolower((unsigned char)*p);
    }
    dst[o] = 0;
}

int game_scan_levels(void) {
    g_levelCount = 0;
    char *text = read_text_file("romfs:/levels/_LEVEL_LISTING.txt", NULL);
    if (!text) return 0;
    char *save = NULL;
    for (char *line = strtok_r(skip_bom(text), "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        strip_cr(line);
        line = trim(line);
        if (!*line || g_levelCount >= JB_MAX_LEVELS) continue;
        snprintf(g_levelNames[g_levelCount], sizeof(g_levelNames[0]), "%s", line);
        g_levelCount++;
    }
    free(text);
    return g_levelCount;
}

static bool find_level_file(const char *title, char *out, size_t outSize) {
    char wanted[256];
    normalize_title(title, wanted, sizeof(wanted));
    DIR *dir = opendir("romfs:/levels");
    if (!dir) return false;
    struct dirent *ent;
    bool found = false;
    while ((ent = readdir(dir))) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 4 || strcasecmp(name + len - 4, ".txt") != 0) continue;
        char base[256];
        snprintf(base, sizeof(base), "%.*s", (int)(len - 4), name);
        char normalized[256];
        normalize_title(base, normalized, sizeof(normalized));
        if (strcmp(normalized, wanted) == 0) {
            snprintf(out, outSize, "romfs:/levels/%s", name);
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

static void add_entity(Entity *e) {
    if (g_level.entityCount >= JB_MAX_ENTITIES) return;
    e->id = g_level.entityCount + 1;
    g_level.entities[g_level.entityCount++] = *e;
}

static Entity make_entity(EntityType type, int x, int y, int w, int h) {
    Entity e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.x = x;
    e.y = y;
    e.w = w;
    e.h = h;
    e.facing = 1;
    e.timer = -1;
    return e;
}

static int color_index(const char *name) {
    if (strcasecmp(name, "gray") == 0 || strcasecmp(name, "grey") == 0) return C_GRAY;
    if (strcasecmp(name, "blue") == 0) return C_BLUE;
    if (strcasecmp(name, "red") == 0) return C_RED;
    if (strcasecmp(name, "green") == 0) return C_GREEN;
    if (strcasecmp(name, "yellow") == 0) return C_YELLOW;
    if (strcasecmp(name, "white") == 0) return C_WHITE;
    return C_GRAY;
}

static void parse_parts(const char *value, char types[][64], int typeCount,
                        char colors[][64], int colorCount, int sx, int sy) {
    char copy[16384];
    snprintf(copy, sizeof(copy), "%s", value);
    char *save = NULL;
    for (char *def = strtok_r(copy, ",", &save); def; def = strtok_r(NULL, ",", &save)) {
        def = trim(def);
        if (!*def) continue;
        char *fields[8];
        int nf = 0;
        char *fsave = NULL;
        for (char *f = strtok_r(def, ";", &fsave); f && nf < 8; f = strtok_r(NULL, ";", &fsave))
            fields[nf++] = trim(f);
        if (nf < 4) continue;
        int gx = atoi(fields[0]);
        int gy = atoi(fields[1]);
        int ti = atoi(fields[2]) - 1;
        int ci = atoi(fields[3]) - 1;
        const char *anim = nf > 4 ? fields[4] : "0";
        const char *relation = nf > 6 ? fields[6] : "0";
        if (ti < 0 || ti >= typeCount || ci < 0 || ci >= colorCount) continue;
        int x = (gx - 1) * sx;
        int y = (gy - 1) * sy;
        char type[64], animLower[64];
        lower_str(type, types[ti], sizeof(type));
        lower_str(animLower, anim, sizeof(animLower));

        int facing = strstr(animLower, "_l") ? -1 : 1;
        int facingY = 0;
        if (strstr(animLower, "_u")) facingY = -1;
        else if (strstr(animLower, "_d")) facingY = 1;
        bool on = strcmp(animLower, "on") == 0 || strcmp(animLower, "none") == 0;
        int rel = atoi(relation);

        char *digits = type;
        while (*digits && !isdigit((unsigned char)*digits)) digits++;
        int studs = *digits ? atoi(digits) : 0;
        bool isBrickType = strncmp(type, "brick_", 6) == 0 && studs > 0;

        Entity e;
        if (isBrickType) {
            e = make_entity(ET_BRICK, x, y, studs * JB_GRID_X, JB_GRID_Y);
            e.studs = studs;
            e.color = color_index(colors[ci]);
            e.fixed = e.color == C_GRAY;
        } else if (strcmp(type, "minifig") == 0) {
            e = make_entity(ET_JUNKBOT, x, y - 18 * 3, 30, 72);
            e.facing = facing;
        } else if (strcmp(type, "haz_walker") == 0) {
            e = make_entity(ET_GEARBOT, x, y - 18, 30, 36);
            e.facing = facing;
        } else if (strcmp(type, "haz_climber") == 0) {
            e = make_entity(ET_CLIMBBOT, x, y - 18, 30, 36);
            e.facing = facing;
            e.facingY = facingY;
        } else if (strcmp(type, "haz_dumbfloat") == 0) {
            e = make_entity(ET_FLYBOT, x, y - 18, 30, 36);
            e.facing = facing;
        } else if (strcmp(type, "haz_float") == 0) {
            e = make_entity(ET_EYEBOT, x, y - 18, 30, 36);
            e.facing = facing;
            e.facingY = facingY;
        } else if (strcmp(type, "flag") == 0 || strcmp(type, "scaredy") == 0) {
            e = make_entity(ET_BIN, x, y - 18 * 2, 30, 54);
            e.facing = facing;
            e.scaredy = strcmp(type, "scaredy") == 0;
        } else if (strcmp(type, "haz_slickcrate") == 0) {
            e = make_entity(ET_CRATE, x, y - 18, 45, 36);
        } else if (strcmp(type, "haz_slickfire") == 0) {
            e = make_entity(ET_FIRE, x, y, 60, 18); e.on = on; e.switchID = rel; e.fixed = true;
        } else if (strcmp(type, "haz_slickfan") == 0) {
            e = make_entity(ET_FAN, x, y, 60, 18); e.on = on; e.switchID = rel; e.fixed = true;
        } else if (strcmp(type, "haz_slicklaser_l") == 0) {
            e = make_entity(ET_LASER, x, y, 30, 18); e.on = on; e.switchID = rel; e.facing = 1; e.fixed = true;
        } else if (strcmp(type, "haz_slicklaser_r") == 0) {
            e = make_entity(ET_LASER, x, y, 30, 18); e.on = on; e.switchID = rel; e.facing = -1; e.fixed = true;
        } else if (strcmp(type, "haz_slickswitch") == 0) {
            e = make_entity(ET_SWITCH, x, y, 30, 18); e.on = on; e.switchID = rel; e.fixed = true;
        } else if (strcmp(type, "haz_slickteleport") == 0) {
            e = make_entity(ET_TELEPORT, x, y, 60, 18); e.teleportID = rel; e.fixed = true;
        } else if (strcmp(type, "haz_slickjump") == 0 || strcmp(type, "brick_slickjump") == 0) {
            e = make_entity(ET_JUMP, x, y, 30, 18);
            e.fixed = strcmp(type, "haz_slickjump") == 0;
        } else if (strcmp(type, "haz_slickshield") == 0 || strcmp(type, "brick_slickshield") == 0) {
            e = make_entity(ET_SHIELD, x, y, 30, 18);
            e.used = strcmp(animLower, "off") == 0;
            e.fixed = strcmp(type, "haz_slickshield") == 0;
        } else if (strcmp(type, "haz_slickpipe") == 0) {
            e = make_entity(ET_PIPE, x, y, 30, 18); e.fixed = true;
        } else if (strcmp(type, "haz_droplet") == 0) {
            e = make_entity(ET_DROPLET, x, y, 30, 18);
        } else {
            e = make_entity(ET_BRICK, x, y, 30, 18);
            e.studs = 2;
            e.color = color_index(colors[ci]);
            e.fixed = e.color == C_GRAY;
        }
        add_entity(&e);
    }
}

static void parse_level_text(const char *text) {
    memset(&g_level, 0, sizeof(g_level));
    snprintf(g_level.backdrop, sizeof(g_level.backdrop), "bkg1");
    int spacingX = 15, spacingY = 18;
    char types[64][64];
    char colors[64][64];
    int typeCount = 0, colorCount = 0;
    char section[32] = "";
    char *save = NULL;
    for (char *raw = strtok_r(skip_bom((char *)text), "\r\n", &save); raw; raw = strtok_r(NULL, "\r\n", &save)) {
        strip_cr(raw);
        char *line = trim(raw);
        if (!*line || *line == '#') continue;
        if (line[0] == '[' && line[strlen(line) - 1] == ']') {
            snprintf(section, sizeof(section), "%.*s", (int)(strlen(line) - 2), line + 1);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line);
        char *value = trim(eq + 1);
        if (strcasecmp(section, "info") == 0) {
            if (strcasecmp(key, "title") == 0) snprintf(g_level.title, sizeof(g_level.title), "%s", value);
            else if (strcasecmp(key, "hint") == 0) snprintf(g_level.hint, sizeof(g_level.hint), "%s", value);
            else if (strcasecmp(key, "par") == 0) g_level.par = atoi(value);
        } else if (strcasecmp(section, "playfield") == 0) {
            if (strcasecmp(key, "spacing") == 0) {
                int a = 15, b = 18;
                if (sscanf(value, "%d,%d", &a, &b) == 2) { spacingX = a; spacingY = b; }
            } else if (strcasecmp(key, "size") == 0) {
                int w = 0, h = 0;
                if (sscanf(value, "%d,%d", &w, &h) == 2) {
                    g_level.boundsW = w * spacingX;
                    g_level.boundsH = h * spacingY;
                }
            }
        } else if (strcasecmp(section, "background") == 0) {
            if (strcasecmp(key, "backdrop") == 0) snprintf(g_level.backdrop, sizeof(g_level.backdrop), "%s", value);
            else if (strcasecmp(key, "decals") == 0 || strcasecmp(key, "bgdecals") == 0) {
                Decal *dst = strcasecmp(key, "bgdecals") == 0 ? g_level.bgdecals : g_level.decals;
                int *count = strcasecmp(key, "bgdecals") == 0 ? &g_level.bgdecalCount : &g_level.decalCount;
                char copy[8192];
                snprintf(copy, sizeof(copy), "%s", value);
                char *csave = NULL;
                for (char *d = strtok_r(copy, ",", &csave); d && *count < JB_MAX_DECALS; d = strtok_r(NULL, ",", &csave)) {
                    int x = 0, y = 0;
                    char name[64] = "";
                    if (sscanf(d, "%d;%d;%63s", &x, &y, name) == 3) {
                        Decal dec;
                        dec.x = x; dec.y = y;
                        snprintf(dec.name, sizeof(dec.name), "%s", name);
                        dst[(*count)++] = dec;
                    }
                }
            }
        } else if (strcasecmp(section, "partslist") == 0) {
            if (strcasecmp(key, "types") == 0 && typeCount < 64) {
                char copy[4096];
                snprintf(copy, sizeof(copy), "%s", value);
                char *csave = NULL;
                for (char *t = strtok_r(copy, ",", &csave); t && typeCount < 64; t = strtok_r(NULL, ",", &csave))
                    snprintf(types[typeCount++], 64, "%s", trim(t));
            } else if (strcasecmp(key, "colors") == 0 && colorCount < 64) {
                char copy[4096];
                snprintf(copy, sizeof(copy), "%s", value);
                char *csave = NULL;
                for (char *c = strtok_r(copy, ",", &csave); c && colorCount < 64; c = strtok_r(NULL, ",", &csave))
                    snprintf(colors[colorCount++], 64, "%s", trim(c));
            } else if (strcasecmp(key, "parts") == 0) {
                parse_parts(value, types, typeCount, colors, colorCount, spacingX, spacingY);
            }
        }
    }
    if (!*g_level.title) snprintf(g_level.title, sizeof(g_level.title), "%s", s_currentTitle);
}

bool game_load_level(const char *title) {
    char path[320];
    if (!find_level_file(title, path, sizeof(path))) return false;
    char *text = read_text_file(path, NULL);
    if (!text) return false;
    snprintf(s_currentTitle, sizeof(s_currentTitle), "%s", title);
    parse_level_text(text);
    free(text);
    g_moves = 0;
    g_frameCounter = 0;
    g_paused = false;
    g_result = 0;
    s_dragCount = 0;
    s_hasPending = false;
    g_windCount = 0;
    g_laserBeamCount = 0;
    s_hasUndo = false;
    s_undoCount = 0;
    undo_history_clear();
    return true;
}

void game_reset_level(void) {
    game_load_level(s_currentTitle);
}

Entity *game_junkbot(void) {
    for (int i = 0; i < g_level.entityCount; i++)
        if (g_level.entities[i].type == ET_JUNKBOT) return &g_level.entities[i];
    return NULL;
}

static bool accept(Entity *e, Filter f) {
    if (!e) return false;
    switch (f) {
        case F_ANY: return true;
        case F_NOT_DROPLET: return e->type != ET_DROPLET;
        case F_NOT_BIN_OR_DROPLET: return e->type != ET_BIN && e->type != ET_DROPLET;
        case F_NOT_BIN_OR_DROPLET_OR_ENEMY:
            return e->type != ET_BIN && e->type != ET_DROPLET && e->type != ET_GEARBOT &&
                   e->type != ET_CLIMBBOT && e->type != ET_FLYBOT && e->type != ET_EYEBOT;
        case F_NOT_DROPLET_OR_JUNKBOT: return e->type != ET_DROPLET && e->type != ET_JUNKBOT;
        case F_BIN: return e->type == ET_BIN;
        case F_JUMP: return e->type == ET_JUMP;
        case F_CRATE: return e->type == ET_CRATE;
    }
    return false;
}

static bool bounds_hit(int x, int y, int w, int h) {
    if (g_level.boundsW <= 0 || g_level.boundsH <= 0) return false;
    memset(&s_bounds, 0, sizeof(s_bounds));
    s_bounds.type = ET_BOUNDS;
    if (x < 0) { s_bounds.x = -15; s_bounds.y = 0; s_bounds.w = 15; s_bounds.h = g_level.boundsH; return true; }
    if (y < 0) { s_bounds.x = 0; s_bounds.y = -18; s_bounds.w = g_level.boundsW; s_bounds.h = 18; return true; }
    if (x + w > g_level.boundsW) { s_bounds.x = g_level.boundsW; s_bounds.y = 0; s_bounds.w = 15; s_bounds.h = g_level.boundsH; return true; }
    if (y + h > g_level.boundsH) { s_bounds.x = 0; s_bounds.y = g_level.boundsH; s_bounds.w = g_level.boundsW; s_bounds.h = 18; return true; }
    return false;
}

static bool rects_intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax + aw > bx && ax < bx + bw && ay + ah > by && ay < by + bh;
}

static Entity *rect_collision(int x, int y, int w, int h, Filter f, Entity *exclude) {
    if (bounds_hit(x, y, w, h) && accept(&s_bounds, f)) return &s_bounds;
    for (int i = 0; i < g_level.entityCount; i++) {
        Entity *e = &g_level.entities[i];
        if (e == exclude || e->grabbed || !accept(e, f)) continue;
        if (rects_intersect(x, y, w, h, e->x, e->y, e->w, e->h)) return e;
    }
    return NULL;
}

static int rect_collision_all(int x, int y, int w, int h, Filter f, Entity *exclude,
                              Entity **out, int max) {
    int n = 0;
    if (bounds_hit(x, y, w, h) && accept(&s_bounds, f) && n < max) out[n++] = &s_bounds;
    for (int i = 0; i < g_level.entityCount && n < max; i++) {
        Entity *e = &g_level.entities[i];
        if (e == exclude || e->grabbed || !accept(e, f)) continue;
        if (rects_intersect(x, y, w, h, e->x, e->y, e->w, e->h)) out[n++] = e;
    }
    return n;
}

static Entity *entity_collision(int x, int y, Entity *e, Filter f) {
    return rect_collision(x, y, e->w, e->h, f, e);
}

static int entity_collision_all(int x, int y, Entity *e, Filter f, Entity **out, int max) {
    return rect_collision_all(x, y, e->w, e->h, f, e, out, max);
}

#define PICK_TOLERANCE 6   /* world pixels; doubled by the 2x display scale */
/* The logical entity slot is 18px tall, but a brick sprite is 32px tall and
   bottom-aligned to that slot: its visible top hangs 15px above e->y, while
   the solid base reaches the bottom of the slot. Two-stud sprites are also
   11px wider than the 30px slot. These extents are used only when the point
   misses every logical rectangle, so the reference brickAt() ordering stays
   authoritative for the base and body of a stacked piece. */
#define PICK_VISUAL_TOP 15
#define PICK_VISUAL_BASE 2
#define PICK_VISUAL_SIDE 5

static bool draggable_entity(const Entity *e) {
    return !e->fixed && !e->grabbed &&
           (e->type == ET_BRICK || e->type == ET_JUMP || e->type == ET_SHIELD);
}

static void entity_rect(const Entity *e, bool visual,
                        int *x, int *y, int *w, int *h) {
    *x = e->x;
    *y = e->y;
    *w = e->w;
    *h = e->h;
    if (!visual || !draggable_entity(e)) return;
    *x -= PICK_VISUAL_SIDE;
    *y -= PICK_VISUAL_TOP;
    *w += PICK_VISUAL_SIDE * 2 + 1;   /* sprite is one pixel wider on the right */
    *h += PICK_VISUAL_TOP + PICK_VISUAL_BASE;
}

/* Match the reference brickAt(): the topmost entity under the point wins, even
   when it is not draggable. A lower brick must never be selected through an
   enemy, a fixed block, or another piece sitting on top of it. */
static Entity *entity_at_point(int wx, int wy) {
    for (int i = g_level.entityCount - 1; i >= 0; i--) {
        Entity *e = &g_level.entities[i];
        int x, y, w, h;
        entity_rect(e, false, &x, &y, &w, &h);
        if (wx >= x && wx < x + w && wy >= y && wy < y + h) return e;
    }
    return NULL;
}

/* Only reached when the point is outside every logical entity slot. Use the
   same topmost-first rule so a fixed block or enemy visible over a piece still
   blocks the expanded pickup. */
static Entity *visual_entity_at_point(int wx, int wy) {
    for (int i = g_level.entityCount - 1; i >= 0; i--) {
        Entity *e = &g_level.entities[i];
        int x, y, w, h;
        entity_rect(e, true, &x, &y, &w, &h);
        if (wx >= x && wx < x + w && wy >= y && wy < y + h) return e;
    }
    return NULL;
}

/* Liang-Barsky segment/AABB test. The rectangle is inset by one pixel so that
   merely touching its edge does not count as blocking a tolerant pickup. */
static bool segment_hits_rect(int x0, int y0, int x1, int y1,
                              int rx, int ry, int rw, int rh) {
    if (rw <= 2 || rh <= 2) return false;
    rx += 1; ry += 1; rw -= 2; rh -= 2;
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    float t0 = 0.0f, t1 = 1.0f;
    const float p[4] = { -dx, dx, -dy, dy };
    const float q[4] = {
        (float)(x0 - rx), (float)(rx + rw - x0),
        (float)(y0 - ry), (float)(ry + rh - y0),
    };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) return false;
        } else {
            float t = q[i] / p[i];
            if (p[i] < 0.0f) {
                if (t > t1) return false;
                if (t > t0) t0 = t;
            } else {
                if (t < t0) return false;
                if (t < t1) t1 = t;
            }
        }
    }
    return true;
}

/* Pick a draggable block. Exact hits keep the original topmost-first ordering.
   Near misses are allowed only when there is a clear, unambiguous path to that
   block, so a nearby attached block can never silently become the target. */
static Entity *draggable_at(int wx, int wy) {
    /* The 18px slot is the primary hitbox, exactly like the original. This is
       what makes the base of a piece under an attached structure selectable:
       the upper piece's expanded sprite never steals those clicks. */
    Entity *top = entity_at_point(wx, wy);
    if (top) return draggable_entity(top) ? top : NULL;

    /* Outside all logical slots, fall back to the full visible sprite so the
       hanging top, nub and bottom edge are still grabbable. */
    Entity *visual = visual_entity_at_point(wx, wy);
    if (visual) return draggable_entity(visual) ? visual : NULL;

    Entity *best = NULL;
    int bestDist = PICK_TOLERANCE * PICK_TOLERANCE + 1;
    bool ambiguous = false;
    for (int i = g_level.entityCount - 1; i >= 0; i--) {
        Entity *e = &g_level.entities[i];
        if (!draggable_entity(e)) continue;
        int x, y, w, h;
        entity_rect(e, true, &x, &y, &w, &h);
        int nx = wx < x ? x : (wx > x + w ? x + w : wx);
        int ny = wy < y ? y : (wy > y + h ? y + h : wy);
        int dx = wx - nx, dy = wy - ny;
        int dist = dx * dx + dy * dy;
        if (dist > PICK_TOLERANCE * PICK_TOLERANCE) continue;

        /* Never expand a hitbox through another entity: attached neighbours,
           enemies standing on the brick and fixed blocks all stop the pick. */
        bool blocked = false;
        for (int j = 0; j < g_level.entityCount; j++) {
            Entity *other = &g_level.entities[j];
            if (other == e || other->grabbed) continue;
            if (segment_hits_rect(wx, wy, nx, ny,
                                  other->x, other->y, other->w, other->h)) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;

        if (dist < bestDist) {
            best = e;
            bestDist = dist;
            ambiguous = false;
        } else if (dist == bestDist && e != best) {
            ambiguous = true;
        }
    }
    return ambiguous ? NULL : best;
}

static bool connects(Entity *a, Entity *b, int direction) {
    if (!b || b->type == ET_BOUNDS) return false;
    bool below = b->y == a->y + a->h;
    bool above = b->y + b->h == a->y;
    return ((direction >= 0 && below) || (direction <= 0 && above)) &&
           a->x + a->w > b->x && a->x < b->x + b->w;
}

static bool entity_in_list(Entity *e, Entity **list, int count) {
    for (int i = 0; i < count; i++) if (list[i] == e) return true;
    return false;
}

static bool connects_to_fixed(Entity *start, int direction, Entity **ignore, int ignoreCount) {
    Entity *visited[256];
    int visitedCount = 0;
    Entity *stack[256];
    int stackCount = 0;
    stack[stackCount++] = start;
    visited[visitedCount++] = start;
    while (stackCount) {
        Entity *cur = stack[--stackCount];
        if (g_level.boundsH > 0 && cur->y + cur->h >= g_level.boundsH) return true;
        for (int i = 0; i < g_level.entityCount; i++) {
            Entity *other = &g_level.entities[i];
            if (other->grabbed || other == cur || entity_in_list(other, ignore, ignoreCount) ||
                entity_in_list(other, visited, visitedCount)) continue;
            bool fromBottom = (cur != start || direction != -1) && connects(cur, other, +1);
            bool fromTop = (cur != start || direction != +1) && connects(cur, other, -1);
            if (!fromBottom && !fromTop) continue;
            if (visitedCount >= 256) continue;
            visited[visitedCount++] = other;
            if (other->fixed) return true;
            stack[stackCount++] = other;
        }
    }
    return false;
}

static void compute_connected_fixed(bool *connected) {
    for (int i = 0; i < g_level.entityCount; i++) connected[i] = false;
    int stack[1024], count = 0;
    for (int i = 0; i < g_level.entityCount; i++) {
        if (g_level.entities[i].fixed && !connected[i]) {
            connected[i] = true;
            stack[count++] = i;
        }
    }
    while (count) {
        int idx = stack[--count];
        Entity *cur = &g_level.entities[idx];
        for (int i = 0; i < g_level.entityCount; i++) {
            Entity *other = &g_level.entities[i];
            if (connected[i] || other->grabbed) continue;
            if (connects(cur, other, 0)) {
                connected[i] = true;
                if (count < 1024) stack[count++] = i;
            }
        }
    }
}

static void hurt_junkbot(Entity *jb, const char *cause) {
    if (!jb || jb->dying || jb->dead || jb->grabbed) return;
    if (strcmp(cause, "fire") == 0) audio_play(SFX_DEATH_FIRE);
    else if (strcmp(cause, "water") == 0) audio_play(SFX_DEATH_WATER);
    else if (strcmp(cause, "laser") == 0) audio_play(SFX_DEATH_LASER);
    else audio_play(SFX_DEATH_BOT);
    if (jb->armored) {
        if (!jb->losingShield) jb->losingShield = true;
    } else {
        jb->anim = 0;
        jb->collectingBin = false;
        jb->dying = true;
        jb->dyingWater = strcmp(cause, "water") == 0;
    }
}

static void activate_jump(Entity *jb, Entity *jump) {
    if (!jump->active) {
        jb->anim = 0;
        jb->momentumY = -3;
        jb->momentumX = jb->facing * 5;
        audio_play(SFX_JUMP);
        jump->active = true;
        jump->anim = 0;
    }
}

static int walk(Entity *jb) {
    int frontX = jb->x + jb->facing * JB_GRID_X;
    Entity *stepOrWall = entity_collision(frontX, jb->y, jb, F_NOT_BIN_OR_DROPLET_OR_ENEMY);
    if (stepOrWall && stepOrWall->type != ET_BOUNDS) {
        int upY = stepOrWall->y - jb->h;
        if (upY - jb->y >= -18 && upY - jb->y < 0 &&
            !entity_collision(frontX, upY, jb, F_NOT_BIN_OR_DROPLET)) {
            jb->x = frontX;
            jb->y = upY;
            return 0;
        }
    }
    Entity *ground = entity_collision(frontX, jb->y + 1, jb, F_NOT_BIN_OR_DROPLET_OR_ENEMY);
    if (ground && !entity_collision(frontX, jb->y, jb, F_NOT_BIN_OR_DROPLET)) {
        jb->x = frontX;
        jb->y = jb->y;
        return 0;
    }
    Entity *candidates[128];
    int n = entity_collision_all(frontX, jb->y + 18 + 1, jb, F_NOT_BIN_OR_DROPLET_OR_ENEMY,
                                 candidates, 128);
    Entity *step = NULL;
    for (int i = 0; i < n; i++) if (!step || candidates[i]->y < step->y) step = candidates[i];
    if (step && step->type != ET_BOUNDS) {
        int downY = step->y - jb->h;
        Entity *c2[128];
        int n2 = entity_collision_all(frontX, downY + 1, jb, F_NOT_BIN_OR_DROPLET_OR_ENEMY, c2, 128);
        Entity *s2 = NULL;
        for (int i = 0; i < n2; i++) if (!s2 || c2[i]->y < s2->y) s2 = c2[i];
        if (downY - jb->y <= 18 && downY - jb->y > 0 && s2 &&
            !entity_collision(frontX, downY, jb, F_NOT_BIN_OR_DROPLET)) {
            jb->x = frontX;
            jb->y = downY;
            return 0;
        }
    }
    jb->facing *= -1;
    audio_play(SFX_TURN);
    return 1;
}

static Entity *find_linked_teleport(Entity *teleport) {
    for (int i = 0; i < g_level.entityCount; i++) {
        Entity *e = &g_level.entities[i];
        if (e != teleport && e->type == ET_TELEPORT && e->teleportID == teleport->teleportID)
            return e;
    }
    return NULL;
}

static void simulate_junkbot(Entity *jb) {
    Entity *above = entity_collision(jb->x, jb->y - 1, jb, F_NOT_DROPLET);
    bool headLoaded = above && (jb->floating ||
        (!above->fixed && above->type != ET_BOUNDS && above->type != ET_FLYBOT &&
         above->type != ET_EYEBOT && !connects_to_fixed(above, 0, (Entity *[]){jb}, 1)));
    if (jb->headLoaded && !headLoaded) jb->headLoaded = false;
    else if (headLoaded && !jb->headLoaded && !jb->grabbed) {
        jb->headLoaded = true;
        audio_play(SFX_HEAD_BONK);
    }
    if (jb->losingShield) {
        if (++jb->losingShieldTime > 36) {
            jb->armored = false;
            jb->losingShield = false;
            jb->losingShieldTime = 0;
            audio_play(SFX_LOSE_POWERUP);
        }
    }
    jb->anim++;
    if (jb->collectingBin) {
        if (jb->anim >= 17) { jb->collectingBin = false; jb->anim = 0; }
        else return;
    }
    if (jb->dying) {
        if (jb->anim >= 10) { jb->anim = 0; jb->dead = true; }
        return;
    }
    if (jb->gettingShield) {
        if (jb->anim >= 11) { jb->gettingShield = false; jb->armored = true; }
        else return;
    }
    if (entity_collision(jb->x, jb->y, jb, F_NOT_DROPLET)) return;
    if (jb->floating) {
        int upY = jb->y - 18;
        if (!entity_collision(jb->x, upY, jb, F_NOT_DROPLET)) jb->y = upY;
        return;
    }
    jb->momentumX = clampi(jb->momentumX, -5, 5);
    jb->momentumY = clampi(jb->momentumY, -5, 5);
    bool inAir = !entity_collision(jb->x, jb->y + 1, jb, F_NOT_DROPLET);
    bool unaligned = jb->x % 15 != 0;
    bool jumpStarting = jb->momentumY < -2;
    if (inAir || jumpStarting || unaligned) {
        int dirX = jb->momentumY < -2 ? 0 : signi(jb->momentumX);
        int dirY = signi(jb->momentumY);
        int newX = jb->x + dirX * 15;
        int newY = jb->y + dirY * 18;
        if (entity_collision(newX, newY, jb, F_NOT_DROPLET)) {
            if (!entity_collision(jb->x, newY, jb, F_NOT_DROPLET)) {
                jb->momentumX = 0;
                jb->y = newY;
            } else if (!entity_collision(newX, jb->y, jb, F_NOT_DROPLET)) {
                jb->momentumY = 0;
                jb->x = newX;
            } else {
                jb->momentumX = 0;
                jb->momentumY = 0;
            }
            audio_play(SFX_HEAD_BONK);
        } else {
            jb->x = newX;
            jb->y = newY;
            jb->momentumX -= dirX;
        }
        jb->momentumY++;
        if (jb->momentumY < 5) jb->anim = 9;
        if (jb->momentumY == 5) audio_play(SFX_FALL);
        Entity *jumpBrick = entity_collision(jb->x, jb->y + 1, jb, F_JUMP);
        Entity *ahead = entity_collision(jb->x + jb->facing * 15, jb->y, jb, F_NOT_DROPLET);
        if (jumpBrick && jumpBrick->x <= jb->x &&
            jumpBrick->x + jumpBrick->w >= jb->x + jb->w && !ahead)
            activate_jump(jb, jumpBrick);
        return;
    }
    if (jb->anim % 5 == 4) {
        int frontX = jb->x + jb->facing * 15;
        Entity *crates[64];
        int n = rect_collision_all(frontX, jb->y, jb->w, jb->h + 1, F_CRATE, NULL, crates, 64);
        bool clear = true;
        for (int i = 0; i < n; i++) {
            Entity *crate = crates[i];
            if ((crate->x + crate->w <= jb->x || jb->x + jb->w <= crate->x) &&
                entity_collision(crate->x + jb->facing * 15, crate->y, crate, F_NOT_DROPLET))
                clear = false;
        }
        if (clear) for (int i = 0; i < n; i++) crates[i]->x += jb->facing * 15;
        int turned = walk(jb);
        for (int i = 0; i < g_level.entityCount && !turned; i++) {
            Entity *other = &g_level.entities[i];
            if (other->y != jb->y + jb->h || other->x > jb->x ||
                other->x + other->w < jb->x + jb->w) continue;
            if (other->type == ET_SWITCH) {
                other->on = !other->on;
                for (int j = 0; j < g_level.entityCount; j++) {
                    Entity *target = &g_level.entities[j];
                    if (target != other && target->switchID == other->switchID &&
                        (target->type == ET_FIRE || target->type == ET_FAN ||
                         target->type == ET_LASER || target->type == ET_SWITCH))
                        target->on = !target->on;
                }
                audio_play(SFX_SWITCH_CLICK);
                audio_play(other->on ? SFX_SWITCH_ON : SFX_SWITCH_OFF);
            } else if (other->type == ET_FIRE && other->on) {
                hurt_junkbot(jb, "fire");
            } else if (other->type == ET_SHIELD && !other->used &&
                       (jb->losingShield || !jb->armored)) {
                jb->anim = 0;
                jb->gettingShield = true;
                jb->losingShield = false;
                jb->losingShieldTime = 0;
                other->used = true;
                audio_play(SFX_GET_SHIELD);
                audio_play(SFX_GET_POWERUP);
            } else if (other->type == ET_JUMP) {
                activate_jump(jb, other);
            } else if (other->type == ET_TELEPORT && other->timer == 0 &&
                       jb->x == other->x + 15) {
                Entity *linked = find_linked_teleport(other);
                if (linked && !linked->teleportBlocked) {
                    jb->x = linked->x + 15;
                    jb->y = linked->y - jb->h;
                    linked->timer = 50;
                    other->timer = 50;
                    audio_play(SFX_TELEPORT);
                }
            }
        }
    }
    Entity *bin = entity_collision(jb->x + jb->facing * 15, jb->y, jb, F_BIN);
    if (bin) {
        jb->anim = 0;
        jb->collectingBin = true;
        bin->removeBeforeRender = true;
        audio_play(SFX_COLLECT_BIN);
        audio_play(SFX_COLLECT_BIN2);
    }
}

static void simulate_gravity(void) {
    Entity *sorted[1024];
    int n = 0;
    for (int i = 0; i < g_level.entityCount; i++) sorted[n++] = &g_level.entities[i];
    for (int i = 1; i < n; i++) {
        Entity *key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j]->y < key->y) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = key;
    }
    for (int i = 0; i < n; i++) {
        Entity *e = sorted[i];
        if (e->fixed || e->grabbed || e->floating || e->type == ET_DROPLET ||
            e->type == ET_JUNKBOT || e->type == ET_CLIMBBOT || e->type == ET_FLYBOT ||
            e->type == ET_EYEBOT) continue;
        if (bounds_hit(e->x, e->y + 1, e->w, e->h)) continue;
        int dir = (e->type == ET_JUNKBOT || e->type == ET_GEARBOT || e->type == ET_CRATE ||
                   e->type == ET_BIN) ? 1 : 0;
        if (connects_to_fixed(e, dir, NULL, 0)) continue;
        if (entity_collision(e->x, e->y, e, F_NOT_DROPLET)) continue;
        Entity *candidates[128];
        int cn = entity_collision_all(e->x, e->y + 18 + 1, e, F_NOT_DROPLET, candidates, 128);
        Entity *ground = NULL;
        for (int j = 0; j < cn; j++) if (!ground || candidates[j]->y < ground->y) ground = candidates[j];
        if (ground) e->y = ground->y - e->h;
        else e->y += 18;
    }
}

static void simulate_gearbot(Entity *e) {
    if (++e->anim <= 2) return;
    e->anim = 0;
    int aheadX = e->x + e->facing * 15;
    Entity *ahead = entity_collision(aheadX, e->y, e, F_NOT_DROPLET);
    int groundX = e->facing == -1 ? e->x - 15 : e->x + e->w;
    Entity *ground = rect_collision(groundX, e->y + 1, 15, e->h, F_NOT_DROPLET, e);
    if (ahead) {
        if (ahead->type == ET_JUNKBOT) hurt_junkbot(ahead, "bot");
        e->facing *= -1;
    } else if (ground) {
        e->x = aheadX;
    } else {
        e->facing *= -1;
    }
}

static void simulate_scaredy(Entity *bin) {
    if (++bin->anim <= 2) return;
    bin->anim = 0;
    Entity *candidates[128];
    int n = rect_collision_all(bin->x - 60, bin->y, bin->w + 120, bin->h, F_BIN, NULL, candidates, 128);
    Entity *junk = NULL;
    for (int i = 0; i < n; i++) if (candidates[i]->type == ET_JUNKBOT) { junk = candidates[i]; break; }
    if (!junk) { bin->facing = 0; return; }
    bin->facing = junk->x > bin->x ? -1 : 1;
    int aheadX = bin->x + bin->facing * 15;
    if (entity_collision(aheadX, bin->y, bin, F_NOT_DROPLET)) bin->facing = 0;
    else bin->x = aheadX;
}

static void simulate_flybot(Entity *e) {
    e->anim++;
    if (e->anim % 2 != 0) return;
    int aheadX = e->x + e->facing * 15;
    Entity *ahead = entity_collision(aheadX, e->y, e, F_NOT_DROPLET);
    if (ahead) {
        if (ahead->type == ET_JUNKBOT) hurt_junkbot(ahead, "bot");
        e->facing *= -1;
    } else e->x = aheadX;
}

static Entity *raycast(int sx, int sy, int dx, int dy, Entity *exclude) {
    int x = sx, y = sy;
    for (int step = 0; step < 50; step++) {
        Entity *hit = rect_collision(x, y, 15, 18, F_NOT_DROPLET, exclude);
        if (hit) return hit;
        x += dx * 15;
        y += dy * 18;
    }
    return NULL;
}

static void do_eyebot_targeting(Entity *e) {
    int dirs[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int d = 0; d < 4; d++) {
        int dx = dirs[d][0], dy = dirs[d][1];
        int offsets[2][2] = {{0,0},{dy != 0 ? 15 : 0, dy != 0 ? 0 : 18}};
        for (int o = 0; o < 2; o++) {
            Entity *hit = raycast(e->x + offsets[o][0], e->y + offsets[o][1], dx, dy, e);
            if (hit && hit->type == ET_JUNKBOT) {
                e->facing = dx;
                e->facingY = dy;
                e->active = 110;
            }
        }
    }
}

static void simulate_eyebot(Entity *e) {
    if (e->active > 0) e->active--;
    e->anim++;
    if (e->anim % ((e->active > 0) ? 1 : 2) != 0) return;
    int aheadX = e->x + e->facing * 15;
    int aheadY = e->y + e->facingY * 18;
    Entity *ahead = entity_collision(aheadX, aheadY, e, F_NOT_DROPLET);
    if (ahead) {
        if (ahead->type == ET_JUNKBOT) hurt_junkbot(ahead, "bot");
        e->facing *= -1;
        e->facingY *= -1;
    } else {
        e->x = aheadX;
        e->y = aheadY;
    }
}

static void simulate_climbbot(Entity *e) {
    e->anim++;
    if (e->anim <= 6) return;
    e->anim = 0;
    int asideX = e->x + e->facing * 15;
    int behindX = e->x - e->facing * 15;
    int aheadX = e->facingY == 0 ? asideX : e->x;
    int aheadY = e->facingY == 0 ? e->y : e->y + e->facingY * 18;
    Entity *aside = entity_collision(asideX, e->y, e, F_NOT_DROPLET);
    Entity *groundAside = entity_collision(asideX, e->y + 1, e, F_NOT_BIN_OR_DROPLET);
    Entity *ahead = entity_collision(aheadX, aheadY, e, F_NOT_DROPLET);
    Entity *behind = entity_collision(behindX, e->y, e, F_NOT_DROPLET);
    Entity *below = entity_collision(e->x, e->y + 18, e, F_NOT_DROPLET);
    if (ahead && ahead->type == ET_JUNKBOT) hurt_junkbot(ahead, "bot");
    if (e->facingY == -1) {
        if (!aside && groundAside) { e->facingY = 0; e->x = asideX; e->y = e->y; }
        else if (e->energy > 0 && !ahead) { e->energy--; e->x = aheadX; e->y = aheadY; }
        else e->facingY = 1;
    } else if (e->facingY == 1) {
        if (below) {
            if (aside && behind) { e->facingY = -1; e->energy = 3; }
            else {
                e->facingY = 0;
                if (aside) {
                    if (!behind) { e->facing *= -1; e->x = behindX; }
                } else e->x = asideX;
            }
        } else e->y += 18;
    } else {
        if (below) {
            if (aside) { e->facingY = -1; e->energy = 3; }
            else e->x = asideX;
        } else {
            if (aside) e->facingY = 1;
            else { e->facingY = 1; e->y += 18; }
        }
    }
    if (e->facingY != -1) e->energy = 0;
}

static void simulate_droplet(Entity *drop) {
    if (drop->splashing) {
        if (++drop->anim > 4) drop->removeBeforeRender = true;
        return;
    }
    for (int i = 0; i < 18; i++) {
        Entity *grounds[128];
        int n = rect_collision_all(drop->x, drop->y + drop->h, drop->w, 1,
                                   F_NOT_DROPLET, NULL, grounds, 128);
        drop->y += 1;
        bool hit = false;
        for (int j = 0; j < n; j++) {
            Entity *g = grounds[j];
            if (g->grabbed) continue;
            if (g->type == ET_JUNKBOT) hurt_junkbot(g, "water");
            if (g->type != ET_DROPLET) {
                drop->splashing = true;
                drop->anim = 0;
                audio_play(SFX_DRIP0 + (rand() % 3));
                hit = true;
                break;
            }
        }
        if (hit) break;
    }
}

static void simulate_pipe(Entity *pipe) {
    pipe->timer--;
    if (pipe->timer == 0 && g_level.entityCount < JB_MAX_ENTITIES) {
        Entity drop = make_entity(ET_DROPLET, pipe->x, pipe->y, 30, 18);
        add_entity(&drop);
    }
    if (pipe->timer <= 0) pipe->timer = (rand() % 30) + 20;
}

static void simulate_jump(Entity *e) {
    if (++e->anim >= 5) { e->anim = 0; e->active = false; }
}

static void simulate_teleport(Entity *e) {
    if (e->timer > 0) e->timer--;
    Entity *linked = find_linked_teleport(e);
    e->teleportBlocked = !linked ||
        rect_collision(e->x + 15, e->y - 72, 30, 72, F_NOT_DROPLET_OR_JUNKBOT, NULL) ||
        rect_collision(linked->x + 15, linked->y - 72, 30, 72, F_NOT_DROPLET_OR_JUNKBOT, NULL);
}

/* 0 = still playing, 1 = cleared, 2 = Junkbot destroyed. This must not return
   bool: both results are nonzero and would otherwise collapse into a clear. */
static int win_lose(void) {
    bool alive = false, living = false, bins = false, collecting = false;
    for (int i = 0; i < g_level.entityCount; i++) {
        Entity *e = &g_level.entities[i];
        if (e->type == ET_JUNKBOT && !e->dead) {
            alive = true;
            if (!e->dying) living = true;
        }
        if (e->type == ET_BIN && !e->removeBeforeRender) bins = true;
        if (e->collectingBin) collecting = true;
    }
    if (!alive) return 2;
    if (living && !bins && !collecting) return 1;
    return 0;
}

void game_simulate_tick(void) {
    if (g_paused || g_result) return;
    g_frameCounter++;
    simulate_gravity();
    for (int i = 0; i < g_level.entityCount; i++) {
        Entity *e = &g_level.entities[i];
        if (e->grabbed) continue;
        switch (e->type) {
            case ET_JUNKBOT: simulate_junkbot(e); break;
            case ET_GEARBOT: simulate_gearbot(e); break;
            case ET_CLIMBBOT: simulate_climbbot(e); break;
            case ET_FLYBOT: simulate_flybot(e); break;
            case ET_EYEBOT: simulate_eyebot(e); break;
            case ET_BIN: if (e->scaredy) simulate_scaredy(e); break;
            case ET_DROPLET: simulate_droplet(e); break;
            case ET_PIPE: simulate_pipe(e); break;
            case ET_JUMP: simulate_jump(e); break;
            case ET_TELEPORT: simulate_teleport(e); break;
            default: e->anim++; break;
        }
    }
    /* Collecting a recycling bin removes an entity mid-tick. The compaction
       below shifts every later entity in the array, so cached pointers (most
       importantly the grabbed group) would dangle and make the held piece
       jitter. Remember indices, compact, then restore pointers. */
    int dragIndex[64];
    for (int i = 0; i < s_dragCount; i++)
        dragIndex[i] = (int)(s_dragging[i] - g_level.entities);

    int oldToNew[JB_MAX_ENTITIES];
    int out = 0;
    for (int i = 0; i < g_level.entityCount; i++) {
        oldToNew[i] = -1;
        if (!g_level.entities[i].removeBeforeRender) {
            oldToNew[i] = out;
            g_level.entities[out++] = g_level.entities[i];
        }
    }
    g_level.entityCount = out;
    for (int i = 0; i < s_dragCount; i++) {
        int oi = dragIndex[i];
        if (oi >= 0 && oi < (int)(sizeof(oldToNew) / sizeof(oldToNew[0])) &&
            oldToNew[oi] >= 0)
            s_dragging[i] = &g_level.entities[oldToNew[oi]];
    }
    for (int i = 0; i < g_level.entityCount; i++) {
        Entity *e = &g_level.entities[i];
        e->wasFloating = e->floating;
        e->floating = false;
        if (e->type == ET_EYEBOT) do_eyebot_targeting(e);
    }
    g_windCount = 0;
    g_laserBeamCount = 0;
    for (int i = 0; i < g_level.entityCount; i++) {
        Entity *e = &g_level.entities[i];
        if (e->type == ET_FAN && e->on && g_windCount < 32) {
            WindColumn *w = &g_winds[g_windCount++];
            w->fan = e;
            w->count = 0;
            for (int x = e->x + 15; x < e->x + e->w - 15 && w->count < 4; x += 15) {
                int extent = 0;
                for (int y = e->y - 18; y > -200; y -= 18) {
                    bool collision = false;
                    for (int j = 0; j < g_level.entityCount; j++) {
                        Entity *other = &g_level.entities[j];
                        if (other->grabbed || !rects_intersect(x, y, 15, 18, other->x, other->y, other->w, other->h))
                            continue;
                        if (other->type == ET_JUNKBOT) {
                            if (!other->wasFloating) audio_play(SFX_FAN);
                            other->floating = true;
                        } else if (other->type != ET_DROPLET) {
                            collision = true;
                            break;
                        }
                    }
                    if (collision) break;
                    extent++;
                }
                w->extents[w->count++] = extent;
            }
        }
        if (e->type == ET_LASER && e->on && g_laserBeamCount < 32) {
            LaserBeam *beam = &g_laserBeams[g_laserBeamCount++];
            beam->laser = e;
            beam->extent = 0;
            beam->hit = NULL;
            for (; beam->extent < 200; beam->extent++) {
                int x = e->x + (e->facing == 1 ? e->w : -15) + 15 * beam->extent * e->facing;
                for (int j = 0; j < g_level.entityCount; j++) {
                    Entity *other = &g_level.entities[j];
                    if (other->grabbed || !rects_intersect(x, e->y, 15, 18, other->x, other->y, other->w, other->h))
                        continue;
                    if (other->type == ET_JUNKBOT) hurt_junkbot(other, "laser");
                    if (other->type != ET_DROPLET) { beam->hit = other; break; }
                }
                if (beam->hit) break;
            }
        }
    }
    int result = win_lose();
    if (result && !g_paused) {
        /* The title level has no bins, so win_lose() reports a win every tick.
           In ambient mode we keep animating and stay completely silent. */
        if (s_ambient) {
            g_result = 0;
        } else {
            g_paused = true;
            g_result = result;
            audio_play(result == 1 ? SFX_YEAH : SFX_OUCH);
        }
    }
}

/* Ambient/drag helpers live after can_release() at the end of this file. */

/* ---- pointer / drag handling ---- */

void game_set_pointer_world(int x, int y) {
    g_pointerWorldX = x;
    g_pointerWorldY = y;
}

static bool find_attached(Entity *brick, int dir, Entity **attached, int *count, int max, bool topLevel) {
    if (*count >= max) return true;
    for (int i = 0; i < g_level.entityCount; i++) {
        Entity *other = &g_level.entities[i];
        if (other == brick || other->grabbed || entity_in_list(other, attached, *count)) continue;
        int checkDir = other->type == ET_BRICK ? dir : -1;
        if (!connects(brick, other, checkDir)) continue;
        if (other->fixed || other->type != ET_BRICK) return false;
        if (*count >= max) return true;
        attached[(*count)++] = other;
        if (!find_attached(other, dir, attached, count, max, false)) return false;
    }
    if (topLevel) {
        for (int a = 0; a < *count; a++) {
            Entity *b = attached[a];
            for (int i = 0; i < g_level.entityCount; i++) {
                Entity *other = &g_level.entities[i];
                if (other->fixed || other->grabbed) continue;
                if (!(other->type == ET_BRICK || other->type == ET_JUMP || other->type == ET_SHIELD)) continue;
                if (!connects(b, other, 0)) continue;
                if (entity_in_list(other, attached, *count)) continue;
                if (connects_to_fixed(other, 0, attached, *count)) continue;
                for (int j = 0; j < g_level.entityCount; j++) {
                    Entity *junk = &g_level.entities[j];
                    if (junk->type == ET_BRICK || junk->grabbed) continue;
                    if (connects(other, junk, -1)) return false;
                }
                if (*count < max) attached[(*count)++] = other;
            }
        }
    }
    return true;
}

/* find_attached() already rejects a direction when Junkbot or an enemy is
   directly supported by one of the candidate bricks. It must not keep walking
   past pieces attached above the picked block: the original game still lets
   you grab the structure below a piece that is itself attached up there. */

static GrabOptions possible_grabs(void) {
    GrabOptions opts;
    memset(&opts, 0, sizeof(opts));
    if (g_paused) return opts;
    Entity *brick = draggable_at(g_pointerWorldX, g_pointerWorldY);
    if (!brick || brick->fixed ||
        (brick->type != ET_BRICK && brick->type != ET_JUMP && brick->type != ET_SHIELD))
        return opts;
    Entity *down[64], *up[64];
    int downCount = 0, upCount = 0;
    down[downCount++] = brick;
    up[upCount++] = brick;
    bool downOK = find_attached(brick, +1, down, &downCount, 64, true);
    bool upOK = find_attached(brick, -1, up, &upCount, 64, true);

    if (downOK) {
        for (int i = 0; i < downCount; i++) opts.items[opts.downwardCount++] = down[i];
        opts.total++;
    }
    if (upOK) {
        for (int i = 0; i < upCount; i++) opts.items[32 + i] = up[i];
        opts.upwardCount = upCount;
        opts.total++;
    }
    return opts;
}

static void start_grab(Entity **group, int count) {
    if (!group || count <= 0) return;
    /* Save the board before the first grab of this move. */
    if (s_dragCount == 0) {
        s_undoCount = g_level.entityCount;
        if (s_undoCount > JB_MAX_ENTITIES) s_undoCount = JB_MAX_ENTITIES;
        memcpy(s_undoEntities, g_level.entities, sizeof(Entity) * s_undoCount);
        s_hasUndo = true;
        /* The move is charged when the piece starts moving, not on release.
           Undo or a cancelled drag rolls this back through s_grabStartMoves;
           an invalid release keeps the piece held instead. */
        s_grabStartMoves = g_moves;
        if (!s_ambient) {
            g_moves++;
            if (g_level.par > 0 && g_moves == g_level.par + 1)
                audio_play(SFX_OUCH);   /* Junkbot's hurt cue when Par is lost */
        }
    }
    s_dragCount = 0;
    /* Clamp the grab anchor to the block we actually picked. A tolerant near-
       miss just outside the edge must not make the group jump a whole cell. */
    Entity *anchor = group[0];
    int px = g_pointerWorldX < anchor->x ? anchor->x : g_pointerWorldX;
    if (px >= anchor->x + anchor->w) px = anchor->x + anchor->w - 1;
    int py = g_pointerWorldY < anchor->y ? anchor->y : g_pointerWorldY;
    if (py >= anchor->y + anchor->h) py = anchor->y + anchor->h - 1;
    for (int i = 0; i < count && s_dragCount < 64; i++) {
        Entity *e = group[i];
        if (entity_in_list(e, s_dragging, s_dragCount)) continue;
        e->grabbed = true;
        e->grabOffX = snap(e->x, 15) - snap(px, 15);
        e->grabOffY = snap(e->y, 18) - snap(py, 18);
        s_dragging[s_dragCount++] = e;
    }
    audio_play(SFX_BLOCK_PICKUP);
}

static void update_drag(void) {
    if (s_dragCount == 0) return;
    int w = g_level.boundsW > 0 ? g_level.boundsW : 525;
    int h = g_level.boundsH > 0 ? g_level.boundsH : 396;

    /* Clamp the whole held group inside the level canvas. Touch input can
       report a finger anywhere on the 960x540 screen (including the HUD and
       footer), which maps to world coordinates outside the board. Without this
       clamp a piece can be dragged offscreen and left hovering there, where it
       is invisible and unreachable. */
    int minOffX = 0, maxOffX = 0, minOffY = 0, maxOffY = 0;
    for (int i = 0; i < s_dragCount; i++) {
        Entity *e = s_dragging[i];
        if (e->grabOffX < minOffX) minOffX = e->grabOffX;
        if (e->grabOffX + e->w > maxOffX) maxOffX = e->grabOffX + e->w;
        if (e->grabOffY < minOffY) minOffY = e->grabOffY;
        if (e->grabOffY + e->h > maxOffY) maxOffY = e->grabOffY + e->h;
    }
    int sx = snap(g_pointerWorldX, 15);
    int sy = snap(g_pointerWorldY, 18);
    if (sx + minOffX < 0) sx = -minOffX;
    if (sx + maxOffX > w) sx = w - maxOffX;
    if (sy + minOffY < 0) sy = -minOffY;
    if (sy + maxOffY > h) sy = h - maxOffY;

    for (int i = 0; i < s_dragCount; i++) {
        s_dragging[i]->x = sx + s_dragging[i]->grabOffX;
        s_dragging[i]->y = sy + s_dragging[i]->grabOffY;
    }
}

static bool can_release(void) {
    if (s_dragCount == 0) return false;
    /* Hard invariant: a piece must never be released partly outside the board,
       regardless of which input source moved it. */
    int w = g_level.boundsW > 0 ? g_level.boundsW : 525;
    int h = g_level.boundsH > 0 ? g_level.boundsH : 396;
    for (int i = 0; i < s_dragCount; i++) {
        Entity *e = s_dragging[i];
        if (e->x < 0 || e->y < 0 || e->x + e->w > w || e->y + e->h > h)
            return false;
    }
    for (int i = 0; i < s_dragCount; i++)
        if (rect_collision(s_dragging[i]->x, s_dragging[i]->y, s_dragging[i]->w, s_dragging[i]->h,
                           F_NOT_DROPLET, NULL))
            return false;
    bool allFixed = true;
    for (int i = 0; i < s_dragCount; i++) if (!s_dragging[i]->fixed) allFixed = false;
    if (allFixed) return true;
    bool connected[1024];
    compute_connected_fixed(connected);
    bool ceiling = false, floor = false;
    for (int i = 0; i < s_dragCount; i++) {
        Entity *d = s_dragging[i];
        for (int j = 0; j < g_level.entityCount; j++) {
            Entity *other = &g_level.entities[j];
            if (other->grabbed) continue;
            if ((other->type == ET_FIRE || other->type == ET_FAN) && connects(d, other, 0))
                return false;
            if (other->type != ET_BRICK || !connected[j]) continue;
            if (connects(d, other, -1)) ceiling = true;
            if (connects(d, other, +1)) floor = true;
        }
    }
    return ceiling != floor;
}

void game_pointer_down(void) {
    if (g_paused) return;
    if (s_dragCount) return;
    GrabOptions opts = possible_grabs();
    if (opts.total == 1) {
        if (opts.downwardCount) start_grab(opts.items, opts.downwardCount);
        else start_grab(opts.items + 32, opts.upwardCount);
        audio_play(SFX_BLOCK_CLICK);
    } else if (opts.total == 2) {
        s_pending = opts;
        s_pendingAtY = g_pointerWorldY;
        s_hasPending = true;
        audio_play(SFX_BLOCK_CLICK);
    }
}

void game_pointer_move(void) {
    if (g_result) {
        /* The clear/death popup already committed the score. Freeze any piece
           that was still held so it cannot keep sliding behind the popup. */
        s_hasPending = false;
        return;
    }
    if (s_hasPending) {
        int dy = g_pointerWorldY - s_pendingAtY;
        if (dy < -10) {
            start_grab(s_pending.items + 32, s_pending.upwardCount);
            s_hasPending = false;
        } else if (dy > 10) {
            start_grab(s_pending.items, s_pending.downwardCount);
            s_hasPending = false;
        }
    }
    if (s_dragCount) update_drag();
}

/* Put the board and the move counter back to their pre-grab state. Used by
   cancelled drags and the first step of an undo. An invalid release keeps the
   piece held and the charged move intact instead. */
static void discard_grab(void) {
    if (s_hasUndo && s_undoCount > 0) {
        memcpy(g_level.entities, s_undoEntities, sizeof(Entity) * s_undoCount);
        g_level.entityCount = s_undoCount;
        g_moves = s_grabStartMoves;
    }
    s_dragCount = 0;
    s_hasUndo = false;
    s_hasPending = false;
}

/* A result popup freezes scoring: the win/lose and save have already used the
   move count. Releasing a held piece there must not behave like an invalid drop
   (which would restore the whole board and rewind g_moves). Move only those
   held bricks back to their pre-grab coordinates and leave Junkbot, bins, and
   every other entity exactly where the simulation left them. */
static void settle_grab_for_result(void) {
    if (s_hasUndo && s_undoCount > 0) {
        for (int i = 0; i < s_dragCount; i++) {
            Entity *held = s_dragging[i];
            for (int j = 0; j < s_undoCount; j++) {
                const Entity *before = &s_undoEntities[j];
                if (before->id != held->id) continue;
                held->x = before->x;
                held->y = before->y;
                break;
            }
        }
    }
    for (int i = 0; i < s_dragCount; i++) {
        s_dragging[i]->grabbed = false;
        s_dragging[i]->grabOffX = 0;
        s_dragging[i]->grabOffY = 0;
    }
    s_dragCount = 0;
    s_hasUndo = false;
    s_hasPending = false;
}

void game_pointer_up(void) {
    s_hasPending = false;
    if (!s_dragCount) return;
    if (g_result) {
        settle_grab_for_result();
        return;
    }
    if (!can_release()) {
        /* Any placement that cannot be released keeps the group hovering. The
           next press/release can try another spot without charging a new move,
           just like dropping onto Junkbot. */
        audio_play(SFX_BLOCK_CLICK);
        return;
    }
    for (int i = 0; i < s_dragCount; i++) {
        s_dragging[i]->grabbed = false;
        s_dragging[i]->grabOffX = 0;
        s_dragging[i]->grabOffY = 0;
    }
    s_dragCount = 0;
    /* The history stores the count from before this move; the move itself was
       already charged on pickup. */
    if (s_hasUndo && !s_ambient)
        undo_history_push(s_undoEntities, s_undoCount, s_grabStartMoves);
    s_hasUndo = false;
    audio_play(SFX_BLOCK_DROP);
}

void game_cancel_drag(void) {
    if (g_result) {
        settle_grab_for_result();
        return;
    }
    for (int i = 0; i < s_dragCount; i++) s_dragging[i]->grabbed = false;
    discard_grab();
}

void game_undo(void) {
    /* A clear or death is final; no caller may rewind the saved score or the
       death itself through the undo snapshot. */
    if (g_result) return;
    bool hadPendingGrab = s_hasUndo;
    game_cancel_drag();
    int count = 0, moves = 0;
    if (!undo_history_pop(g_level.entities, JB_MAX_ENTITIES, &count, &moves)) {
        /* No completed move yet: discard an in-progress grab, including the
           move that was charged when it started. */
        if (!hadPendingGrab || s_undoCount <= 0) return;
        memcpy(g_level.entities, s_undoEntities, sizeof(Entity) * s_undoCount);
        count = s_undoCount;
        moves = g_moves;
    }
    g_level.entityCount = count;
    g_moves = moves;         /* undo subtracts from the move counter */
    s_hasUndo = false;
    s_dragCount = 0;
    g_result = 0;
    g_paused = false;
    g_windCount = 0;
    g_laserBeamCount = 0;
    audio_play(SFX_BLOCK_CLICK);
}

/* Undo is locked once Junkbot is already being collected by the last recycle
   bin. The title level has no bins, so it stays a pure control playground. */
bool game_undo_blocked(void) {
    if (s_ambient) return false;
    Entity *jb = NULL;
    int binsLeft = 0;
    for (int i = 0; i < g_level.entityCount; i++) {
        Entity *e = &g_level.entities[i];
        if (e->type == ET_JUNKBOT && !e->dead) jb = e;
        if (e->type == ET_BIN && !e->removeBeforeRender) binsLeft++;
    }
    return jb && jb->collectingBin && binsLeft == 0;
}

void game_set_ambient(bool ambient) {
    s_ambient = ambient;
    if (ambient) {
        g_paused = false;
        g_result = 0;
    }
}

bool game_is_dragging(void) { return s_dragCount > 0; }

bool game_drag_valid(void) { return s_dragCount > 0 && can_release(); }

void game_get_render_origin(int *ox, int *oy) {
    int w = g_level.boundsW > 0 ? g_level.boundsW : 525;
    int h = g_level.boundsH > 0 ? g_level.boundsH : 396;
    /* Centre the level canvas. The old +16 nudge exposed the green/white edge
       of the original level background down the left side of the screen. */
    *ox = (JB_SCREEN_W - w) / 2;
    *oy = (JB_SCREEN_H - h) / 2 + 12;
}

void game_get_level_clip(int *x, int *y, int *w, int *h) {
    int ox, oy;
    game_get_render_origin(&ox, &oy);
    *x = ox;
    *y = oy;
    *w = g_level.boundsW > 0 ? g_level.boundsW : 525;
    *h = g_level.boundsH > 0 ? g_level.boundsH : 396;
}

void game_screen_to_world(int sx, int sy, int *wx, int *wy) {
    int ox, oy;
    game_get_render_origin(&ox, &oy);
    *wx = sx - ox;
    *wy = sy - oy;
}

void game_world_to_screen(int wx, int wy, int *sx, int *sy) {
    int ox, oy;
    game_get_render_origin(&ox, &oy);
    *sx = wx + ox;
    *sy = wy + oy;
}
