#ifndef JUNKBOT_RENDER_H
#define JUNKBOT_RENDER_H

#include <SDL.h>
#include <SDL_ttf.h>
#include <stdbool.h>

#define JB_SCREEN_W 960
#define JB_SCREEN_H 540

typedef struct {
    char name[96];
    int x, y, w, h;
} Frame;

typedef struct {
    SDL_Texture *texture;
    Frame *frames;
    int frameCount;
} Atlas;

typedef struct {
    Atlas sprites;
    Atlas backgrounds;
    Atlas undercover;
    Atlas undercoverBackgrounds;
    SDL_Texture *titleArt;
    SDL_Texture *loadingPlaque;
    SDL_Texture *readyPlaque;
    SDL_Texture *creditsButton;
    SDL_Texture *creditsButtonHover;
    SDL_Texture *replayButton;
    SDL_Texture *replayButtonHover;
    SDL_Texture *skipButton;
    SDL_Texture *skipButtonHover;
    Atlas intro;
    TTF_Font *font;
    bool ready;
} Resources;

extern Resources g_res;

bool render_init(SDL_Renderer *renderer);
void render_exit(void);
void render_set_world_origin(int x, int y);
/* Grabbed groups draw at 0.8 alpha when placeable and 0.3 when not, matching
   the reference game's render loop. */
void render_set_drag_invalid(bool invalid);
void render_world(SDL_Renderer *renderer);
/* Pixel width of a string in the given font (for ticker layouts). */
int render_text_width(TTF_Font *font, const char *text);
int render_text_width_scaled(TTF_Font *font, const char *text, int scale);
void render_text(SDL_Renderer *renderer, TTF_Font *font, const char *text,
                 int x, int y, SDL_Color color, bool centered);
/* Same pixel font at an exact integer scale (3x for the jukebox timer). */
void render_text_scaled(SDL_Renderer *renderer, TTF_Font *font, const char *text,
                        int x, int y, SDL_Color color, bool centered, int scale);
void render_text_right(SDL_Renderer *renderer, TTF_Font *font, const char *text,
                       int rightX, int y, SDL_Color color);
void render_panel(SDL_Renderer *renderer, SDL_Rect rect, SDL_Color fill, SDL_Color border);
/* Nine-slice-style popup frame: fixed corner caps and rails, stretchable centre.
   Used for the quick centre-expanding modal/result/quit animations. */
void render_popup_frame(SDL_Renderer *renderer, SDL_Rect rect, SDL_Color fill,
                        SDL_Color border);
/* Slowly rotating vector-art gears and pulleys for menu backdrops. */
void render_factory_backdrop(SDL_Renderer *renderer, Uint64 nowUs);
void render_level_select_backdrop(SDL_Renderer *renderer, Uint64 nowUs);
/* Cached variants for menu screens: regenerate the animation at 20 Hz instead
   of re-issuing hundreds of draw calls every presented frame. */
void render_factory_backdrop_cached(SDL_Renderer *renderer, Uint64 nowUs);
void render_level_select_backdrop_cached(SDL_Renderer *renderer, Uint64 nowUs);
void render_title_art(SDL_Renderer *renderer);
/* Original "Download in progress" brick stack shown during boot. When
   `background` is false, only the plaque and bricks are drawn (used to keep
   the stack on the right side of the title screen after loading finishes). */
void render_progress_overlay(SDL_Renderer *renderer, Uint32 elapsedMs, bool ready,
                             bool background);
/* Original Credits button on the title screen (title-art coordinates). */
void render_credits_button(SDL_Renderer *renderer, bool hover);
/* Flipbook frame from the original intro SWF, letterboxed into the full screen. */
void render_intro_frame(SDL_Renderer *renderer, int frameIndex);
/* Original skip_intro / replay_intro buttons, drawn into dst. */
void render_intro_button(SDL_Renderer *renderer, bool replay, bool hover,
                         SDL_Rect dst);

#endif
