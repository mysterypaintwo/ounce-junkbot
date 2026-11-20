#include "render.h"
#include "game.h"
#include "jb_offsets.h"
#include "log.h"

#include <SDL_image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Resources g_res;
static int s_originX = 0, s_originY = 0;
static bool s_dragInvalid = false;
static int s_currentAlpha = -1;

/* Text textures are cached across frames. The menu screens redraw the same
   strings every frame; re-rasterising them with TTF and re-uploading a texture
   each time was a large part of the level-select/file-select slowdown. */
#define TEXT_CACHE_SLOTS 256
typedef struct {
    SDL_Texture *tex;
    char *text;
    TTF_Font *font;
    Uint32 color;
    int scale, w, h;
    Uint64 lastUsed;
} TextCacheSlot;
static TextCacheSlot s_textCache[TEXT_CACHE_SLOTS];
static Uint64 s_textClock = 0;

/* Cached animated menu backdrop (regenerated at 20 Hz). */
typedef struct {
    SDL_Texture *tex;
    Uint64 bucket;
    int kind;   /* 1 = factory, 2 = level select */
} BackdropCache;
static BackdropCache s_backdrop;

/* 04B_08 is an 8px pixel font. Rasterise at its native size and scale by an
   exact 2x with nearest-neighbour so the strokes stay whole pixels; loading it
   directly at 18px was producing uneven, blurry glyph weights. */
#define UI_FONT_SIZE 8
#define UI_FONT_SCALE 2

void render_set_world_origin(int x, int y) { s_originX = x; s_originY = y; }
void render_set_drag_invalid(bool invalid) { s_dragInvalid = invalid; }

/* Alpha applied to every sprite of a grabbed group, mirroring the JS render
   loop (0.8 when it can be dropped, 0.3 when it cannot). */
static int entity_alpha(bool grabbed) {
    if (!grabbed) return -1;
    return s_dragInvalid ? 77 : 204;   /* 0.3 and 0.8 of 255 */
}

static bool load_atlas(SDL_Renderer *renderer, const char *pngPath, const char *tsvPath, Atlas *atlas) {
    atlas->texture = IMG_LoadTexture(renderer, pngPath);
    if (!atlas->texture) {
        fprintf(stderr, "failed to load %s: %s\n", pngPath, IMG_GetError());
        return false;
    }
    atlas->frames = calloc(1024, sizeof(Frame));
    atlas->frameCount = 0;
    FILE *f = fopen(tsvPath, "r");
    if (!f) {
        fprintf(stderr, "failed to load %s\n", tsvPath);
        return false;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) && atlas->frameCount < 1024) {
        Frame fr;
        if (sscanf(line, "%95[^\t]\t%d\t%d\t%d\t%d", fr.name, &fr.x, &fr.y, &fr.w, &fr.h) == 5) {
            atlas->frames[atlas->frameCount++] = fr;
        }
    }
    fclose(f);
    return true;
}

static void free_atlas(Atlas *atlas) {
    if (atlas->texture) SDL_DestroyTexture(atlas->texture);
    free(atlas->frames);
    atlas->texture = NULL;
    atlas->frames = NULL;
    atlas->frameCount = 0;
}

bool render_init(SDL_Renderer *renderer) {
    memset(&g_res, 0, sizeof(g_res));
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) {
        LOG("IMG_Init FAILED: %s", IMG_GetError());
        return false;
    }
    LOG("IMG_Init ok");
    if (TTF_Init() != 0) {
        LOG("TTF_Init FAILED: %s", TTF_GetError());
        return false;
    }
    LOG("TTF_Init ok");
    g_res.font = TTF_OpenFont("romfs:/fonts/04B_08__.TTF", UI_FONT_SIZE);
    if (!g_res.font) LOG("font load FAILED: %s", TTF_GetError());
    else {
        TTF_SetFontHinting(g_res.font, TTF_HINTING_MONO);
        LOG("font loaded at native %dpx, drawn at %dx", UI_FONT_SIZE, UI_FONT_SCALE);
    }
    g_res.titleArt = IMG_LoadTexture(renderer, "romfs:/menus/neo_title.png");
    if (!g_res.titleArt) LOG("title art FAILED: %s", IMG_GetError());
    else LOG("title art loaded");
    g_res.loadingPlaque = IMG_LoadTexture(renderer, "romfs:/menus/download_in_progress.png");
    g_res.readyPlaque = IMG_LoadTexture(renderer, "romfs:/menus/ready_to_play.png");
    g_res.creditsButton = IMG_LoadTexture(renderer, "romfs:/menus/credit_up.png");
    g_res.creditsButtonHover = IMG_LoadTexture(renderer, "romfs:/menus/credit_up_x.png");
    g_res.replayButton = IMG_LoadTexture(renderer, "romfs:/menus/replay_intro.png");
    g_res.replayButtonHover = IMG_LoadTexture(renderer, "romfs:/menus/replay_intro_x.png");
    g_res.skipButton = IMG_LoadTexture(renderer, "romfs:/menus/skip_intro.png");
    g_res.skipButtonHover = IMG_LoadTexture(renderer, "romfs:/menus/skip_intro_x.png");
    LOG("boot plaques/buttons: %s",
        (g_res.loadingPlaque && g_res.readyPlaque && g_res.creditsButton &&
         g_res.creditsButtonHover && g_res.replayButton && g_res.skipButton)
        ? "loaded" : "some FAILED");

    bool ok = true;
    ok = load_atlas(renderer, "romfs:/sprites/sprites.png", "romfs:/sprites/sprites.tsv", &g_res.sprites) && ok;
    ok = load_atlas(renderer, "romfs:/sprites/backgrounds.png", "romfs:/sprites/backgrounds.tsv", &g_res.backgrounds) && ok;
    ok = load_atlas(renderer, "romfs:/sprites/undercover/sprites.png", "romfs:/sprites/undercover/sprites.tsv", &g_res.undercover) && ok;
    ok = load_atlas(renderer, "romfs:/sprites/undercover/backgrounds.png", "romfs:/sprites/undercover/backgrounds.tsv", &g_res.undercoverBackgrounds) && ok;
    if (!load_atlas(renderer, "romfs:/intro/atlas.png", "romfs:/intro/atlas.tsv",
                    &g_res.intro))
        LOG("intro flipbook unavailable");
    LOG("atlases: %s", ok ? "all loaded" : "one or more FAILED");
    g_res.ready = ok;
    return ok;
}

void render_exit(void) {
    if (s_backdrop.tex) SDL_DestroyTexture(s_backdrop.tex);
    memset(&s_backdrop, 0, sizeof(s_backdrop));
    for (int i = 0; i < TEXT_CACHE_SLOTS; i++) {
        if (s_textCache[i].tex) SDL_DestroyTexture(s_textCache[i].tex);
        free(s_textCache[i].text);
    }
    memset(s_textCache, 0, sizeof(s_textCache));
    free_atlas(&g_res.sprites);
    free_atlas(&g_res.backgrounds);
    free_atlas(&g_res.undercover);
    free_atlas(&g_res.undercoverBackgrounds);
    free_atlas(&g_res.intro);
    if (g_res.titleArt) SDL_DestroyTexture(g_res.titleArt);
    if (g_res.loadingPlaque) SDL_DestroyTexture(g_res.loadingPlaque);
    if (g_res.readyPlaque) SDL_DestroyTexture(g_res.readyPlaque);
    if (g_res.creditsButton) SDL_DestroyTexture(g_res.creditsButton);
    if (g_res.creditsButtonHover) SDL_DestroyTexture(g_res.creditsButtonHover);
    if (g_res.replayButton) SDL_DestroyTexture(g_res.replayButton);
    if (g_res.replayButtonHover) SDL_DestroyTexture(g_res.replayButtonHover);
    if (g_res.skipButton) SDL_DestroyTexture(g_res.skipButton);
    if (g_res.skipButtonHover) SDL_DestroyTexture(g_res.skipButtonHover);
    if (g_res.font) TTF_CloseFont(g_res.font);
    TTF_Quit();
    IMG_Quit();
    memset(&g_res, 0, sizeof(g_res));
}

static const Frame *find_frame(Atlas *atlas, const char *name) {
    if (!atlas) return NULL;
    for (int i = 0; i < atlas->frameCount; i++)
        if (strcasecmp(atlas->frames[i].name, name) == 0) return &atlas->frames[i];
    return NULL;
}

static void draw_frame_ex(SDL_Renderer *r, Atlas *atlas, const char *name,
                          int dx, int dy, bool flip, int alpha) {
    const Frame *fr = find_frame(atlas, name);
    if (!fr) return;
    int a = alpha >= 0 ? alpha : s_currentAlpha;
    SDL_Rect src = { fr->x, fr->y, fr->w, fr->h };
    SDL_Rect dst = { dx + s_originX, dy + s_originY, fr->w, fr->h };
    if (a >= 0) SDL_SetTextureAlphaMod(atlas->texture, (Uint8)a);
    SDL_RenderCopyEx(r, atlas->texture, &src, &dst, 0, NULL,
                     flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    if (a >= 0) SDL_SetTextureAlphaMod(atlas->texture, 255);
}

static void draw_frame_bottom_at(SDL_Renderer *r, const char *name, int x, int bottom,
                                 int offX, int offY, bool flip) {
    const Frame *fr = find_frame(&g_res.sprites, name);
    Atlas *atlas = &g_res.sprites;
    if (!fr) { fr = find_frame(&g_res.undercover, name); atlas = &g_res.undercover; }
    if (!fr) return;
    SDL_Rect src = { fr->x, fr->y, fr->w, fr->h };
    SDL_Rect dst = { x - offX + s_originX, bottom - fr->h - 1 - offY + s_originY, fr->w, fr->h };
    if (s_currentAlpha >= 0) SDL_SetTextureAlphaMod(atlas->texture, (Uint8)s_currentAlpha);
    SDL_RenderCopyEx(r, atlas->texture, &src, &dst, 0, NULL,
                     flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    if (s_currentAlpha >= 0) SDL_SetTextureAlphaMod(atlas->texture, 255);
}

static void draw_frame_bottom(SDL_Renderer *r, const char *name, int x, int bottom, bool flip) {
    draw_frame_bottom_at(r, name, x, bottom, 0, 0, flip);
}

static const char *color_name(int color) {
    switch (color) {
        case C_BLUE: return "blue";
        case C_RED: return "red";
        case C_GREEN: return "green";
        case C_YELLOW: return "yellow";
        case C_WHITE: return "white";
        default: return "immobile";
    }
}

static void draw_junkbot(SDL_Renderer *r, Entity *jb) {
    const char *anim = "walk_r";
    int len = 10;
    if (jb->dead) { anim = "dead"; len = 1; }
    else if (jb->dyingWater) { anim = "water_die"; len = 10; }
    else if (jb->dying) { anim = "die"; len = 10; }
    else if (jb->collectingBin) { anim = "eat_start"; len = 17; }
    else if (jb->gettingShield) { anim = jb->facing == 1 ? "shield_on_r" : "shield_on_l"; len = 11; }
    else if (jb->armored) { anim = jb->facing == 1 ? "shield_walk_r" : "shield_walk_l"; len = 10; }
    else anim = jb->facing == 1 ? "walk_r" : "walk_l";

    char name[96];
    if (strcmp(anim, "dead") == 0) {
        snprintf(name, sizeof(name), "minifig_dead");
    } else {
        int frame = 1 + (jb->anim % len);
        /* The original left-walk animations hold frame 6 for the seventh
           step; generating frame 7 from the number makes Junkbot warp/skew. */
        if (frame == 7 && (!strcmp(anim, "walk_l") ||
                           !strcmp(anim, "shield_walk_l")))
            frame = 6;
        snprintf(name, sizeof(name), "minifig_%s_%d", anim, frame);
    }
    const Frame *fr = find_frame(&g_res.sprites, name);
    if (!fr && strcmp(anim, "dead") == 0) snprintf(name, sizeof(name), "minifig_dead");
    if (!fr) snprintf(name, sizeof(name), "minifig_walk_r_1");

    int offX = 0, offY = 0;
    for (size_t i = 0; i < sizeof(jb_offsets) / sizeof(jb_offsets[0]); i++) {
        if (strcmp(jb_offsets[i].sprite, name) == 0) {
            offX = jb_offsets[i].x;
            offY = jb_offsets[i].y;
            break;
        }
    }
    draw_frame_bottom_at(r, name, jb->x, jb->y + jb->h, offX, offY, false);
}

static void draw_entity(SDL_Renderer *r, Entity *e) {
    s_currentAlpha = entity_alpha(e->grabbed);
    switch (e->type) {
        case ET_BRICK: {
            char name[64];
            snprintf(name, sizeof(name), "brick_%s_%d", color_name(e->color), e->studs ? e->studs : 2);
            draw_frame_bottom(r, name, e->x, e->y + e->h, false);
            break;
        }
        case ET_JUNKBOT: draw_junkbot(r, e); break;
        case ET_BIN:
            if (e->scaredy && e->facing) {
                char name[64];
                snprintf(name, sizeof(name), "SCAREDY_%s_%d_s3", e->facing == 1 ? "WALK_R" : "walk_l", 1 + (e->anim % 2));
                draw_frame_ex(r, &g_res.undercover, name, e->x + 4, e->y + e->h - 49, false, -1);
            } else draw_frame_ex(r, &g_res.sprites, "bin", e->x + 4, e->y + e->h - 52, false, -1);
            break;
        case ET_CRATE: draw_frame_bottom(r, "HAZ_SLICKCRATE", e->x, e->y + e->h, false); break;
        case ET_GEARBOT: {
            char name[64];
            snprintf(name, sizeof(name), "gearbot_walk_%s_%d", e->facing == 1 ? "r" : "l", 1 + (e->anim % 2));
            draw_frame_bottom(r, name, e->x, e->y + e->h, false);
            break;
        }
        case ET_FLYBOT: {
            char name[32];
            snprintf(name, sizeof(name), "flybot_%d", 1 + (e->anim % 2));
            draw_frame_bottom(r, name, e->x, e->y + e->h, false);
            break;
        }
        case ET_EYEBOT: {
            char name[48];
            snprintf(name, sizeof(name), "eyebot_%s%d", e->active > 0 ? "active_" : "", 1 + (e->anim % 2));
            if (!find_frame(&g_res.sprites, name) && e->active > 0)
                snprintf(name, sizeof(name), "eyebot_active_1fix");
            draw_frame_bottom(r, name, e->x, e->y + e->h, false);
            break;
        }
        case ET_CLIMBBOT: {
            const char *dir = e->facingY == -1 ? "u" : (e->facingY == 1 ? "d" : (e->facing == 1 ? "r" : "l"));
            char name[64];
            snprintf(name, sizeof(name), "climbbot_walk_%s_%d", dir, 1 + (e->anim % 6));
            draw_frame_ex(r, &g_res.sprites, name, e->x, e->y - 6, false, -1);
            break;
        }
        case ET_FIRE: {
            int idx = e->on ? (e->anim % 8 < 4 ? e->anim % 4 : 4 - (e->anim % 4)) : 0;
            char name[64];
            snprintf(name, sizeof(name), "haz_slickFire_%s_%d", e->on ? "on" : "off", 1 + idx);
            draw_frame_ex(r, &g_res.sprites, name, e->x + 1, e->y + e->h - 37, false, -1);
            break;
        }
        case ET_FAN: {
            int idx = e->on ? e->anim % 4 : 0;
            char name[64];
            snprintf(name, sizeof(name), "haz_slickFan_%s_%d",
                     e->on ? "on" : "off", 1 + idx);
            const Frame *fr = find_frame(&g_res.sprites, name);
            /* Match the JS: sit the sprite 4px above the entity's bottom. */
            int top = e->y + e->h - (fr ? fr->h : 26) - 4;
            draw_frame_ex(r, &g_res.sprites, name, e->x + 1, top, false, -1);
            break;
        }
        case ET_SWITCH: {
            char name[64];
            snprintf(name, sizeof(name), "haz_slickSwitch_%s_1", e->on ? "on" : "off");
            draw_frame_bottom(r, name, e->x, e->y + e->h, false);
            break;
        }
        case ET_JUMP: {
            char name[64];
            snprintf(name, sizeof(name), "%s_slickJump_%s_%d", e->fixed ? "haz" : "brick",
                     e->active ? "active" : "dormant", 1 + (e->active ? e->anim % 5 : 0));
            draw_frame_bottom(r, name, e->x, e->y + e->h, false);
            break;
        }
        case ET_SHIELD: {
            Atlas *atlas = e->fixed ? &g_res.sprites : &g_res.undercover;
            const char *prefix = e->fixed ? "HAZ" : "BRICK";
            char name[64];
            snprintf(name, sizeof(name), "%s_SLICKSHIELD_%s", prefix, e->used ? "OFF" : "ON");
            const Frame *fr = find_frame(atlas, name);
            if (fr) {
                SDL_Rect src = { fr->x, fr->y, fr->w, fr->h };
                SDL_Rect dst = { e->x + s_originX, e->y + e->h - fr->h - 1 + s_originY,
                                 fr->w, fr->h };
                if (s_currentAlpha >= 0) SDL_SetTextureAlphaMod(atlas->texture, (Uint8)s_currentAlpha);
                SDL_RenderCopyEx(r, atlas->texture, &src, &dst, 0, NULL, SDL_FLIP_NONE);
                if (s_currentAlpha >= 0) SDL_SetTextureAlphaMod(atlas->texture, 255);
            }
            break;
        }
        case ET_TELEPORT: {
            const char *name;
            if (e->timer > 30) name = e->timer % 2 ? "haz_slickTeleport_active_1" : "haz_slickTeleport_active_2";
            else name = (e->timer == 0 && !e->teleportBlocked) ? "haz_slickTeleport_on_1" : "haz_slickTeleport_off_1";
            draw_frame_bottom(r, name, e->x, e->y + e->h, false);
            break;
        }
        case ET_LASER: {
            draw_frame_bottom(r, e->facing == 1 ? "haz_slickLaser_L_ON_1" : "haz_slickLaser_R_ON_1",
                              e->x, e->y + e->h, false);
            break;
        }
        case ET_PIPE: {
            bool wet = e->timer <= 6 && e->timer > -1;
            char name[64];
            snprintf(name, sizeof(name), "haz_slickPipe_%s_%d", wet ? "wet" : "dry", wet ? 6 - e->timer + 1 : 1);
            draw_frame_ex(r, &g_res.sprites, name, e->x + 11, e->y - 12, false, -1);
            break;
        }
        case ET_DROPLET: {
            char name[64];
            snprintf(name, sizeof(name), "drip_%s_%d", e->splashing ? "splashing" : "falling",
                     e->splashing ? 1 + e->anim % 5 : 1);
            draw_frame_ex(r, &g_res.sprites, name, e->x + 15, e->y, false, -1);
            break;
        }
        default: break;
    }
    s_currentAlpha = -1;
}

static void draw_decal(SDL_Renderer *r, const char *name, int x, int y) {
    const Frame *fr = find_frame(&g_res.backgrounds, name);
    Atlas *atlas = &g_res.backgrounds;
    if (!fr) { fr = find_frame(&g_res.undercoverBackgrounds, name); atlas = &g_res.undercoverBackgrounds; }
    if (!fr) return;
    SDL_Rect src = { fr->x, fr->y, fr->w, fr->h };
    SDL_Rect dst = { x + s_originX, y + s_originY, fr->w, fr->h };
    SDL_RenderCopy(r, atlas->texture, &src, &dst);
}

void render_world(SDL_Renderer *r) {
    SDL_SetRenderDrawColor(r, 187, 187, 187, 255);
    SDL_RenderClear(r);

    draw_decal(r, *g_level.backdrop ? g_level.backdrop : "bkg1", -6, -25);
    for (int i = 0; i < g_level.bgdecalCount; i++)
        draw_decal(r, g_level.bgdecals[i].name, g_level.bgdecals[i].x - 3, g_level.bgdecals[i].y - 20);
    for (int i = 0; i < g_level.decalCount; i++)
        draw_decal(r, g_level.decals[i].name, g_level.decals[i].x - 30, g_level.decals[i].y - 64);

    Entity *sorted[JB_MAX_ENTITIES];
    int n = 0;
    for (int i = 0; i < g_level.entityCount; i++) sorted[n++] = &g_level.entities[i];
    for (int i = 1; i < n; i++) {
        Entity *key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j]->y < key->y) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = key;
    }
    /* The reference game follows the y sort with a bubble pass that swaps
       neighbours when one is wholly above or wholly left of the other. Without
       it, stacked bricks and bots render in the wrong order. */
    int end = n;
    while (end > 1) {
        int newEnd = 0;
        for (int i = 1; i < end; i++) {
            Entity *a = sorted[i - 1], *b = sorted[i];
            if (a->y + a->h < b->y || b->x + b->w <= a->x) {
                Entity *tmp = sorted[i - 1];
                sorted[i - 1] = b;
                sorted[i] = tmp;
                newEnd = i;
            }
        }
        end = newEnd;
    }
    for (int i = 0; i < n; i++) draw_entity(r, sorted[i]);

    for (int i = 0; i < g_windCount; i++) {
        WindColumn *w = &g_winds[i];
        /* The JS uses one frame for the whole gust, advanced once per 18 Hz
           simulation tick, with a frame-dependent vertical offset that makes
           the airflow appear to travel upward. */
        int frameIndex = ((w->fan->anim % 7) + 7) % 7;
        char name[32];
        snprintf(name, sizeof(name), "fanAir_1_%d", 1 + frameIndex);
        for (int c = 0; c < w->count; c++) {
            int x = w->fan->x + 15 + c * 15;
            int y = w->fan->y - 18;
            for (int e = 0; e < w->extents[c]; e++) {
                draw_frame_ex(r, &g_res.sprites, name, x + 4,
                              y - frameIndex * 2 + 8, false, -1);
                y -= 18;
            }
        }
    }
    for (int i = 0; i < g_laserBeamCount; i++) {
        LaserBeam *b = &g_laserBeams[i];
        for (int e = 0; e < b->extent; e++) {
            int x = b->laser->x + (b->laser->facing == 1 ? b->laser->w : -15) +
                    15 * e * b->laser->facing;
            char name[32];
            snprintf(name, sizeof(name), "laserbeam_1_%d", 1 + b->laser->anim % 3);
            draw_frame_ex(r, &g_res.undercover, name, x + 4, b->laser->y, false, -1);
        }
    }
}

static SDL_Texture *text_texture(SDL_Renderer *r, TTF_Font *font,
                                 const char *text, SDL_Color color,
                                 int scale, int *outW, int *outH) {
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    *outW = surf->w * scale;
    *outH = surf->h * scale;
    SDL_FreeSurface(surf);
    /* Nearest-neighbour is set globally with SDL_HINT_RENDER_SCALE_QUALITY in
       render_init(); this SDL2 build predates SDL_SetTextureScaleMode(). */
    return tex;
}

static unsigned text_hash(const char *s, Uint32 color, int scale) {
    unsigned h = 2166136261u;
    for (; *s; ++s) h = (h ^ (unsigned char)*s) * 16777619u;
    h = (h ^ color) * 16777619u;
    h = (h ^ (unsigned)scale) * 16777619u;
    return h;
}

/* Return a cached texture for this exact string/colour/scale. Textures live
   until render_exit(), so callers must NOT destroy the result. */
static SDL_Texture *text_cached(SDL_Renderer *r, TTF_Font *font, const char *text,
                                SDL_Color color, int scale, int *outW, int *outH) {
    Uint32 packed = ((Uint32)color.r << 24) | ((Uint32)color.g << 16) |
                    ((Uint32)color.b << 8) | (Uint32)color.a;
    unsigned idx = text_hash(text, packed, scale) & (TEXT_CACHE_SLOTS - 1);
    for (int attempt = 0; attempt < 8; attempt++) {
        TextCacheSlot *slot = &s_textCache[(idx + attempt) & (TEXT_CACHE_SLOTS - 1)];
        if (slot->tex && slot->font == font && slot->color == packed &&
            slot->scale == scale && strcmp(slot->text, text) == 0) {
            slot->lastUsed = ++s_textClock;
            *outW = slot->w;
            *outH = slot->h;
            return slot->tex;
        }
    }

    TextCacheSlot *victim = NULL;
    for (int attempt = 0; attempt < 8; attempt++) {
        TextCacheSlot *slot = &s_textCache[(idx + attempt) & (TEXT_CACHE_SLOTS - 1)];
        if (!slot->tex) { victim = slot; break; }
        if (!victim || slot->lastUsed < victim->lastUsed) victim = slot;
    }
    if (!victim) victim = &s_textCache[idx];
    if (victim->tex) {
        SDL_DestroyTexture(victim->tex);
        free(victim->text);
        victim->tex = NULL;
        victim->text = NULL;
    }
    victim->tex = text_texture(r, font, text, color, scale, &victim->w, &victim->h);
    if (!victim->tex) return NULL;
    victim->text = strdup(text);
    victim->font = font;
    victim->color = packed;
    victim->scale = scale;
    victim->lastUsed = ++s_textClock;
    *outW = victim->w;
    *outH = victim->h;
    return victim->tex;
}

void render_text_right(SDL_Renderer *r, TTF_Font *font, const char *text,
                       int rightX, int y, SDL_Color color) {
    if (!font || !text || !*text) return;
    int w = 0, h = 0;
    SDL_Texture *tex = text_cached(r, font, text, color, UI_FONT_SCALE, &w, &h);
    if (!tex) return;
    SDL_Rect dst = { rightX - w, y, w, h };
    SDL_RenderCopy(r, tex, NULL, &dst);
}

void render_panel(SDL_Renderer *r, SDL_Rect rect, SDL_Color fill, SDL_Color border) {
    SDL_SetRenderDrawColor(r, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(r, &rect);
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(r, &rect);
}

static SDL_Color popup_shade(SDL_Color c, int delta) {
    int v = c.r + delta;
    c.r = (Uint8)(v < 0 ? 0 : (v > 255 ? 255 : v));
    v = c.g + delta;
    c.g = (Uint8)(v < 0 ? 0 : (v > 255 ? 255 : v));
    v = c.b + delta;
    c.b = (Uint8)(v < 0 ? 0 : (v > 255 ? 255 : v));
    return c;
}

/* Fixed-size corners and rails with a stretchable centre, so the frame keeps
   its shape while a popup expands from the middle of the screen. */
void render_popup_frame(SDL_Renderer *r, SDL_Rect rect, SDL_Color fill, SDL_Color border) {
    if (rect.w < 6 || rect.h < 6) return;
    int shortest = rect.w < rect.h ? rect.w : rect.h;
    int cap = shortest / 4;
    if (cap > 8) cap = 8;
    if (cap < 2) cap = 2;

    SDL_Rect shadow = { rect.x + 4, rect.y + 4, rect.w, rect.h };
    SDL_SetRenderDrawColor(r, 0, 0, 0, 110);
    SDL_RenderFillRect(r, &shadow);

    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
    SDL_RenderFillRect(r, &rect);
    SDL_Rect inner = { rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4 };
    SDL_SetRenderDrawColor(r, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(r, &inner);

    /* Corner caps sit above the inner fill; the stretches between them are
       the nine-slice edges. */
    SDL_Rect corners[4] = {
        { rect.x, rect.y, cap, cap },
        { rect.x + rect.w - cap, rect.y, cap, cap },
        { rect.x, rect.y + rect.h - cap, cap, cap },
        { rect.x + rect.w - cap, rect.y + rect.h - cap, cap, cap }
    };
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
    for (int i = 0; i < 4; i++) SDL_RenderFillRect(r, &corners[i]);

    SDL_Color hi = popup_shade(border, 45);
    SDL_Color lo = popup_shade(border, -45);
    SDL_SetRenderDrawColor(r, hi.r, hi.g, hi.b, hi.a);
    SDL_RenderDrawLine(r, rect.x + cap, rect.y + 2,
                       rect.x + rect.w - cap, rect.y + 2);
    SDL_RenderDrawLine(r, rect.x + 2, rect.y + cap,
                       rect.x + 2, rect.y + rect.h - cap);
    SDL_SetRenderDrawColor(r, lo.r, lo.g, lo.b, lo.a);
    SDL_RenderDrawLine(r, rect.x + cap, rect.y + rect.h - 3,
                       rect.x + rect.w - cap, rect.y + rect.h - 3);
    SDL_RenderDrawLine(r, rect.x + rect.w - 3, rect.y + cap,
                       rect.x + rect.w - 3, rect.y + rect.h - cap);

    /* Fixed-interval rail notches make the stretched edges read as machinery
       rather than one long flat line. */
    SDL_SetRenderDrawColor(r, hi.r, hi.g, hi.b, hi.a);
    for (int x = rect.x + cap + 5; x < rect.x + rect.w - cap - 3; x += 24) {
        SDL_RenderDrawLine(r, x, rect.y + 3, x, rect.y + 5);
        SDL_RenderDrawLine(r, x, rect.y + rect.h - 5, x, rect.y + rect.h - 3);
    }
    for (int y = rect.y + cap + 5; y < rect.y + rect.h - cap - 3; y += 24) {
        SDL_RenderDrawLine(r, rect.x + 3, y, rect.x + 5, y);
        SDL_RenderDrawLine(r, rect.x + rect.w - 5, y, rect.x + rect.w - 3, y);
    }
}

/* Vector-art factory machinery for menu backdrops. Everything is drawn from
   primitives so it scales with the frame without extra assets, and it moves
   slowly enough to stay behind the UI rather than competing with it. */
static void draw_ring(SDL_Renderer *r, int cx, int cy, int radius, int segments) {
    const double pi = acos(-1.0);
    for (int i = 0; i < segments; i++) {
        double a0 = i * 2.0 * pi / segments;
        double a1 = (i + 1) * 2.0 * pi / segments;
        SDL_RenderDrawLine(r, cx + (int)(cos(a0) * radius),
                           cy + (int)(sin(a0) * radius),
                           cx + (int)(cos(a1) * radius),
                           cy + (int)(sin(a1) * radius));
    }
}

static void draw_gear(SDL_Renderer *r, int cx, int cy, int radius,
                      int teeth, double phase, SDL_Color color) {
    const double pi = acos(-1.0);
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    draw_ring(r, cx, cy, radius, 48);
    draw_ring(r, cx, cy, radius / 3, 24);
    for (int i = 0; i < teeth; i++) {
        double a = phase + i * 2.0 * pi / teeth;
        int x0 = cx + (int)(cos(a) * (radius - 3));
        int y0 = cy + (int)(sin(a) * (radius - 3));
        int x1 = cx + (int)(cos(a) * (radius + 6));
        int y1 = cy + (int)(sin(a) * (radius + 6));
        SDL_RenderDrawLine(r, x0, y0, x1, y1);
    }
    for (int i = 0; i < 4; i++) {
        double a = phase + i * pi / 2;
        SDL_RenderDrawLine(r, cx + (int)(cos(a) * radius / 3),
                           cy + (int)(sin(a) * radius / 3),
                           cx + (int)(cos(a) * (radius - 4)),
                           cy + (int)(sin(a) * (radius - 4)));
    }
}

static void draw_pulley(SDL_Renderer *r, int cx, int cy, int radius,
                        double phase, SDL_Color color) {
    const double pi = acos(-1.0);
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    draw_ring(r, cx, cy, radius, 36);
    draw_ring(r, cx, cy, radius / 4, 16);
    for (int i = 0; i < 6; i++) {
        double a = phase + i * 2.0 * pi / 6;
        SDL_RenderDrawLine(r, cx + (int)(cos(a) * radius / 4),
                           cy + (int)(sin(a) * radius / 4),
                           cx + (int)(cos(a) * (radius - 2)),
                           cy + (int)(sin(a) * (radius - 2)));
    }
}

void render_factory_backdrop(SDL_Renderer *r, Uint64 nowUs) {
    SDL_Rect screen = { 0, 0, JB_SCREEN_W, JB_SCREEN_H };
    SDL_SetRenderDrawColor(r, 18, 23, 30, 255);
    SDL_RenderFillRect(r, &screen);

    /* Faint structural beams and a belt line tie the machinery together. */
    SDL_SetRenderDrawColor(r, 34, 42, 52, 255);
    for (int y = 70; y < JB_SCREEN_H; y += 92)
        SDL_RenderDrawLine(r, 0, y, JB_SCREEN_W, y);
    SDL_RenderDrawLine(r, 90, 300, 870, 300);

    SDL_Color gear = { 44, 55, 68, 255 };
    SDL_Color pulley = { 52, 62, 74, 255 };
    double secs = (double)nowUs / 1000000.0;
    draw_gear(r, 92, 104, 62, 12, secs * 0.22, gear);
    draw_gear(r, 196, 156, 40, 10, -secs * 0.34, gear);
    draw_gear(r, 868, 96, 74, 14, -secs * 0.18, gear);
    draw_gear(r, 782, 172, 38, 9, secs * 0.30, gear);
    draw_gear(r, 60, 452, 54, 11, secs * 0.26, gear);
    draw_pulley(r, 420, 300, 34, secs * 0.28, pulley);
    draw_pulley(r, 560, 300, 34, secs * 0.28, pulley);
    draw_pulley(r, 700, 300, 34, secs * 0.28, pulley);
}

/* A scrolling conveyor band. Slat spacing is fixed while the whole pattern
   slides, so belts at different speeds read as separate depth layers. */
static void draw_conveyor(SDL_Renderer *r, int y, int width, int offset,
                          SDL_Color belt, SDL_Color slat) {
    SDL_Rect band = { 0, y, width, 9 };
    SDL_SetRenderDrawColor(r, belt.r, belt.g, belt.b, belt.a);
    SDL_RenderFillRect(r, &band);
    SDL_SetRenderDrawColor(r, slat.r, slat.g, slat.b, slat.a);
    const int spacing = 26;
    int shift = ((offset % spacing) + spacing) % spacing;
    for (int x = -shift; x < width; x += spacing)
        SDL_RenderDrawLine(r, x, y, x, y + 8);
    SDL_SetRenderDrawColor(r, belt.r + 12, belt.g + 12, belt.b + 12, 255);
    SDL_RenderDrawLine(r, 0, y, width, y);
    SDL_RenderDrawLine(r, 0, y + 8, width, y + 8);
}

/* Level Select echoes the original factory screens (grey-green bkg1/bkg2 and
   the metallic building tabs) with more machinery than File Select: three
   conveyor depths, slowly sliding pulley banks and counter-rotating gears. */
void render_level_select_backdrop(SDL_Renderer *r, Uint64 nowUs) {
    SDL_Rect screen = { 0, 0, JB_SCREEN_W, JB_SCREEN_H };
    SDL_SetRenderDrawColor(r, 22, 27, 25, 255);
    SDL_RenderFillRect(r, &screen);
    double secs = (double)nowUs / 1000000.0;

    /* Structural beams, dim enough to stay behind the level rows. */
    SDL_SetRenderDrawColor(r, 28, 34, 32, 255);
    for (int y = 24; y < JB_SCREEN_H; y += 76)
        SDL_RenderDrawLine(r, 0, y, JB_SCREEN_W, y);
    for (int x = 24; x < JB_SCREEN_W; x += 120)
        SDL_RenderDrawLine(r, x, 0, x, JB_SCREEN_H);

    /* Far layer: large, slow gears and the back conveyor. */
    SDL_Color far = { 30, 37, 34, 255 };
    draw_gear(r, 54, 70, 66, 14, secs * 0.10, far);
    draw_gear(r, 906, 84, 72, 15, -secs * 0.09, far);
    draw_gear(r, 46, 480, 58, 12, -secs * 0.12, far);
    draw_gear(r, 912, 470, 62, 13, secs * 0.11, far);
    draw_conveyor(r, 148, JB_SCREEN_W, (int)(secs * 8.0),
                  (SDL_Color){ 26, 32, 30, 255 },
                  (SDL_Color){ 38, 46, 42, 255 });
    draw_conveyor(r, 392, JB_SCREEN_W, (int)(secs * 11.0),
                  (SDL_Color){ 26, 32, 30, 255 },
                  (SDL_Color){ 38, 46, 42, 255 });

    /* Mid layer: opposing gears and a bank of pulleys that slowly push and
       pull apart, with taut cables riding over them. */
    SDL_Color mid = { 36, 44, 40, 255 };
    draw_gear(r, 116, 232, 34, 10, -secs * 0.30, mid);
    draw_gear(r, 848, 240, 38, 10, secs * 0.27, mid);
    draw_gear(r, 300, 512, 26, 8, secs * 0.36, mid);
    draw_gear(r, 660, 36, 28, 8, -secs * 0.33, mid);
    int py = 296;
    int p1 = 352 + (int)(24.0 * sin(secs * 0.22));
    int p2 = 492 + (int)(30.0 * sin(secs * 0.22 + 1.2));
    int p3 = 632 + (int)(24.0 * sin(secs * 0.22 + 2.4));
    SDL_SetRenderDrawColor(r, 34, 42, 38, 255);
    SDL_RenderDrawLine(r, p1, py - 14, p2, py - 14);
    SDL_RenderDrawLine(r, p2, py - 14, p3, py - 14);
    SDL_RenderDrawLine(r, p1, py + 14, p2, py + 14);
    SDL_RenderDrawLine(r, p2, py + 14, p3, py + 14);
    SDL_Color metal = { 58, 68, 62, 255 };
    draw_pulley(r, p1, py, 14, secs * 0.50, metal);
    draw_pulley(r, p2, py, 16, secs * 0.44, metal);
    draw_pulley(r, p3, py, 14, secs * 0.50, metal);

    /* Near layer: a faster front belt and small edge gears for parallax. */
    draw_conveyor(r, 470, JB_SCREEN_W, (int)(secs * 18.0),
                  (SDL_Color){ 30, 38, 34, 255 },
                  (SDL_Color){ 46, 56, 50, 255 });
    SDL_Color near = { 42, 50, 46, 255 };
    draw_gear(r, 18, 300, 18, 8, secs * 0.70, near);
    draw_gear(r, 942, 320, 20, 8, -secs * 0.65, near);
}

/* The animated menu backdrops are hundreds of SDL line calls per frame. Cache
   each into a target texture and only regenerate at 20 Hz; the gears move
   slowly enough that the difference is invisible, but the per-frame cost drops
   sharply. */
static void render_backdrop_cached(SDL_Renderer *r, int kind, Uint64 nowUs) {
    Uint64 bucket = nowUs / 50000ULL;   /* 20 fps */
    if (!s_backdrop.tex || s_backdrop.kind != kind || s_backdrop.bucket != bucket) {
        if (!s_backdrop.tex) {
            s_backdrop.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                               SDL_TEXTUREACCESS_TARGET,
                                               JB_SCREEN_W, JB_SCREEN_H);
        }
        SDL_Texture *prev = SDL_GetRenderTarget(r);
        SDL_SetRenderTarget(r, s_backdrop.tex);
        if (kind == 1) render_factory_backdrop(r, nowUs);
        else render_level_select_backdrop(r, nowUs);
        SDL_SetRenderTarget(r, prev);
        s_backdrop.kind = kind;
        s_backdrop.bucket = bucket;
    }
    SDL_RenderCopy(r, s_backdrop.tex, NULL, NULL);
}

void render_factory_backdrop_cached(SDL_Renderer *r, Uint64 nowUs) {
    render_backdrop_cached(r, 1, nowUs);
}

void render_level_select_backdrop_cached(SDL_Renderer *r, Uint64 nowUs) {
    render_backdrop_cached(r, 2, nowUs);
}

void render_title_art(SDL_Renderer *r) {
    if (!g_res.titleArt) return;
    SDL_Rect dst = { 155, 60, 650, 420 };
    SDL_RenderCopy(r, g_res.titleArt, NULL, &dst);
}

void render_progress_overlay(SDL_Renderer *r, Uint32 elapsedMs, bool ready,
                             bool background) {
    /* The web game's load indicator lives inside the title art at
       (496,24)/(506,50) relative to its top-left; our art is drawn at
       (TITLE_FRAME_X, TITLE_FRAME_Y) = (155,60). */
    if (background) {
        SDL_SetRenderDrawColor(r, 187, 187, 187, 255);
        SDL_RenderClear(r);
        render_title_art(r);
    }

    if (ready) {
        if (g_res.readyPlaque) {
            SDL_Rect dst = { 155 + 512, 60 + 24, 92, 25 };
            SDL_RenderCopy(r, g_res.readyPlaque, NULL, &dst);
        }
    } else if (g_res.loadingPlaque) {
        SDL_Rect dst = { 155 + 496, 60 + 24, 124, 25 };
        SDL_RenderCopy(r, g_res.loadingPlaque, NULL, &dst);
    }

    /* Fourteen immobile bricks, odd/even widths as in the original CSS, fall
       into a bottom-up stack. Each brick starts 90 ms after the previous one
       and takes 600 ms to drop, mirroring the original animation. */
    const int columnX = 155 + 506;
    const int columnW = 100;
    const int columnBottom = 60 + 50 + 265;
    const int stepY = 18, fallPx = 500, fallMs = 600;
    for (int i = 0; i < 14; i++) {
        Uint32 start = (Uint32)(i * 90);
        /* The title screen passes a large sentinel; keep this comparison
           unsigned so it never becomes -1 and skips the whole stack. */
        if (elapsedMs < start) break;
        int t = (int)((elapsedMs - start) * 1000 / fallMs);
        if (t > 1000) t = 1000;
        const char *name = (i % 2 == 0) ? "brick_immobile_4" : "brick_immobile_2";
        const Frame *fr = find_frame(&g_res.sprites, name);
        if (!fr) continue;
        int dx = columnX + (columnW - fr->w) / 2;
        /* The CSS brick divs are 18px tall while their sprites are 32px: the
           sprite starts at the top of its 18px slot and hangs below it. Anchor
           the sprite top to the slot top so a full stack sits inside the
           265px progress column and never climbs into the Ready plaque. */
        int dy = columnBottom - (i + 1) * stepY - (1000 - t) * fallPx / 1000;
        SDL_Rect src = { fr->x, fr->y, fr->w, fr->h };
        SDL_Rect dst = { dx, dy, fr->w, fr->h };
        SDL_RenderCopy(r, g_res.sprites.texture, &src, &dst);
    }
}

void render_credits_button(SDL_Renderer *r, bool hover) {
    SDL_Texture *tex = hover && g_res.creditsButtonHover ?
                       g_res.creditsButtonHover : g_res.creditsButton;
    if (!tex) {
        render_text(r, g_res.font, "Credits", 465, 219,
                    (SDL_Color){ 255, 235, 90, 255 }, true);
        return;
    }
    SDL_Rect dst = { 465, 219, 96, 26 };
    SDL_RenderCopy(r, tex, NULL, &dst);
}

void render_intro_frame(SDL_Renderer *r, int frameIndex) {
    SDL_Rect black = { 0, 0, JB_SCREEN_W, JB_SCREEN_H };
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderFillRect(r, &black);
    if (frameIndex < 0 || frameIndex >= g_res.intro.frameCount) return;
    const Frame *fr = &g_res.intro.frames[frameIndex];
    /* Original SWF is a square 200x200 stage; scale it 2x with nearest
       neighbour and centre it in the 960x540 frame. */
    const int size = 400;
    SDL_Rect dst = { (JB_SCREEN_W - size) / 2, (JB_SCREEN_H - size) / 2,
                     size, size };
    SDL_Rect src = { fr->x, fr->y, fr->w, fr->h };
    SDL_RenderCopy(r, g_res.intro.texture, &src, &dst);
}

void render_intro_button(SDL_Renderer *r, bool replay, bool hover, SDL_Rect dst) {
    SDL_Texture *tex = replay
        ? (hover && g_res.replayButtonHover ? g_res.replayButtonHover
                                            : g_res.replayButton)
        : (hover && g_res.skipButtonHover ? g_res.skipButtonHover
                                          : g_res.skipButton);
    if (!tex) return;
    SDL_RenderCopy(r, tex, NULL, &dst);
}

int render_text_width_scaled(TTF_Font *font, const char *text, int scale) {
    if (!font || !text || scale <= 0) return 0;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(font, text, &w, &h) != 0) return 0;
    return w * scale;
}

int render_text_width(TTF_Font *font, const char *text) {
    return render_text_width_scaled(font, text, UI_FONT_SCALE);
}

void render_text_scaled(SDL_Renderer *r, TTF_Font *font, const char *text,
                        int x, int y, SDL_Color color, bool centered, int scale) {
    if (!font || !text || !*text || scale <= 0) return;
    int w = 0, h = 0;
    SDL_Texture *tex = text_cached(r, font, text, color, scale, &w, &h);
    if (!tex) return;
    SDL_Rect dst = { centered ? x - w / 2 : x, y, w, h };
    SDL_RenderCopy(r, tex, NULL, &dst);
}

void render_text(SDL_Renderer *r, TTF_Font *font, const char *text,
                 int x, int y, SDL_Color color, bool centered) {
    render_text_scaled(r, font, text, x, y, color, centered, UI_FONT_SCALE);
}
