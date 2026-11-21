#include <switch.h>

#include <SDL.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "assets.h"
#include "audio.h"
#include "dirprep.h"
#include "game.h"
#include "log.h"
#include "render.h"
#include "save.h"

typedef enum {
    SCREEN_TITLE = 0, SCREEN_SLOTS, SCREEN_JUKEBOX,
    SCREEN_SELECT, SCREEN_LEVEL, SCREEN_INTRO
} Screen;
typedef enum { INPUT_MOUSE = 0, INPUT_TOUCH } InputMode;
typedef enum {
    MODAL_NONE = 0, MODAL_HINT, MODAL_CREDITS,
    MODAL_LEVEL_EXIT, MODAL_LEVEL_RESTART, MODAL_ERASE_SAVE,
    MODAL_COPY_SLOT, MODAL_KONAMI, MODAL_CLEAR_DATA
} Modal;
/* File Select actions are two-step flows driven entirely from the file rows:
   COPY picks a source slot then a destination; ERASE picks one slot to wipe. */
typedef enum {
    FILE_ACTION_NONE = 0, FILE_ACTION_COPY_SOURCE,
    FILE_ACTION_COPY_DEST, FILE_ACTION_ERASE
} FileAction;

static const SDL_Color WHITE = { 255, 255, 255, 255 };
static const SDL_Color BLACK = { 0, 0, 0, 220 };
static const SDL_Color YELLOW = { 255, 235, 90, 255 };
static const SDL_Color LIGHT = { 210, 225, 240, 255 };
static const SDL_Color MUTED = { 135, 145, 155, 255 };  /* subdued header/footer text */
static const SDL_Color DIM = { 100, 105, 110, 255 };
static const SDL_Color GREEN = { 150, 205, 150, 255 };  /* subtle par-achieved text */
static const SDL_Color RULE = { 115, 45, 50, 220 };      /* subtle red separator */
static const SDL_Color RED = { 255, 95, 95, 255 };       /* locked-action warning flash */
/* Popup frame colours: each panel type gets its own border so the meaning is
   readable at a glance. */
static const SDL_Color FRAME_RED = { 224, 72, 72, 255 };     /* homebrew / quit menu */
static const SDL_Color FRAME_GOLD = { 255, 202, 64, 255 };   /* gold award earned */
static const SDL_Color FRAME_GREEN = { 96, 208, 112, 255 };  /* normal level clear */
static const SDL_Color FRAME_ORANGE = { 240, 152, 56, 255 }; /* warnings / confirmation */
static const SDL_Color FRAME_BLUE = { 88, 142, 224, 255 };   /* game over */

/* Native 1x framebuffer, nearest-neighbour upscaled to the display. s_world
   holds the level before it is composited, so the touch magnifier can sample
   it without reading from the texture currently being drawn into. */
static SDL_Texture *s_frame = NULL;
static SDL_Texture *s_world = NULL;
static int s_outW = 1920, s_outH = 1080;
static bool s_noVsync = false;
static Event s_resEvent;
static bool s_resEventOpen = false;

/* The real Title Screen level is simulated and drawn through the transparent
   window in the title art, exactly like the JS reference. */
static SDL_Texture *s_titleWorld = NULL;
#define TITLE_FRAME_X 155
/* The title art is 650x420 centred in the 960x540 frame: (540-420)/2 = 60. */
#define TITLE_FRAME_Y 60
#define TITLE_WIN_X (TITLE_FRAME_X + 36)
#define TITLE_WIN_Y (TITLE_FRAME_Y + 201)
#define TITLE_WIN_W 414
#define TITLE_WIN_H 184
#define TITLE_WORLD_TOP 212  /* world Y shown at the top of the window */
#define START_BOX_X 260
#define START_BOX_Y 486
#define START_BOX_W 440
#define START_BOX_H 54
/* Credits button in title-art coordinates (310,159,96,26), like the JS game. */
#define CREDITS_BOX_X (TITLE_FRAME_X + 310)
#define CREDITS_BOX_Y (TITLE_FRAME_Y + 159)
#define CREDITS_BOX_W 96
#define CREDITS_BOX_H 26
/* Replay Intro sits to the left of Credits in the title-art header. */
#define REPLAY_BOX_X (TITLE_FRAME_X + 210)
#define REPLAY_BOX_Y (TITLE_FRAME_Y + 159)
#define REPLAY_BOX_W 96
#define REPLAY_BOX_H 26
/* Title-screen destructive option. It sits in the bottom-left corner, clear of
   the centred Start box, and asks for the full Konami sequence before wiping.
   File select is reached simply by starting the game. */
#define CLEAR_DATA_BOX_X 20
#define CLEAR_DATA_BOX_Y (JB_SCREEN_H - 38)
#define CLEAR_DATA_BOX_W 210
#define CLEAR_DATA_BOX_H 28

/* File-select screen. */
#define SLOT_ROW_X 150
#define SLOT_ROW_Y 92
#define SLOT_ROW_W 660
#define SLOT_ROW_H 82
#define SLOT_ROW_GAP 12
#define SLOT_BTN_Y 458
#define SLOT_BTN_W 160
#define SLOT_BTN_H 38
#define SLOT_COPY_X 220
#define SLOT_JUKEBOX_X 400
#define SLOT_ERASE_X 580

/* Jukebox: rows are BGM groups (the whole soundtrack top to bottom), columns
   are the segments within each group, with wrapping on both axes. */
#define JUKEBOX_LIST_X 35
#define JUKEBOX_COL_W 170
#define JUKEBOX_COL_GAP 10
#define JUKEBOX_LIST_Y 66
#define JUKEBOX_ROW_H 56
#define JUKEBOX_ROW_HGT 50
/* Sequence preview strip directly below the grid, above the yellow status. */
#define JUKEBOX_PREVIEW_X 35
#define JUKEBOX_PREVIEW_Y 408
#define JUKEBOX_PREVIEW_CELL 118
#define JUKEBOX_PREVIEW_COUNT 5
#define JUKEBOX_TIMER_X 795

/* Level select: one page per building (15 levels each). */
#define SELECT_X 150
#define SELECT_W 660
#define SELECT_ROW_Y 52
#define SELECT_ROW_H 27
#define SELECT_ARROW_Y 459
#define SELECT_ARROW_W 44
#define SELECT_ARROW_H 26

/* Modal geometry: taller than the old panel so hints and credits fit. */
#define MODAL_X 120
#define MODAL_Y 40
#define MODAL_W 720
#define MODAL_H 460
#define MODAL_ARROW_Y 350
#define MODAL_ARROW_W 44
#define MODAL_ARROW_H 30
#define MODAL_BTN_Y 410
#define MODAL_BTN_H 38

/* Input source state. Pointer and touch are mutually exclusive; only the
   active source feeds the game. */
static InputMode s_inputMode = INPUT_MOUSE;
static bool s_touchActive = false;
static u32 s_touchFinger = 0;
static int s_lastTouchX = 0, s_lastTouchY = 0;
static bool s_useStickCursor = true;

/* One physical button owns a click/drag until it is released, so left/right
   mouse buttons, L/ZL and R/ZR can never fight over the same drag. */
typedef enum {
    CLICK_NONE = 0, CLICK_MOUSE_LEFT, CLICK_MOUSE_RIGHT,
    CLICK_PAD_L, CLICK_PAD_ZL, CLICK_PAD_R, CLICK_PAD_ZR
} ClickOwner;
static ClickOwner s_clickOwner = CLICK_NONE;

/* Each controller input source gets its own PadState and pointer. Exactly one
   source is consumed per frame: the one that last showed real activity. This
   lets a lone right Joy-Con drive the reticle and R/ZR click without a left
   Joy-Con stealing the same frame, and vice versa. */
#define PAD_SOURCES 9
#define PAD_SOURCE_HANDHELD 8
static PadState s_pads[PAD_SOURCES];
static PadPointer s_pointers[PAD_SOURCES];
static bool s_pointerReady[PAD_SOURCES];
static int s_activePad = -1;
static int s_reticleStick = 0;   /* 0 = left stick, 1 = right stick */

/* Menus deliberately ignore controller arbitration: the D-pad may be on the
   non-mouse Joy-Con, and A/B/X/Y may be on the other one. Gameplay still uses
   exactly one active source, but menu navigation/confirmation is the union of
   every connected controller's D-pad and face buttons. */
#define MENU_BUTTON_MASK (HidNpadButton_Up | HidNpadButton_Down | \
                          HidNpadButton_Left | HidNpadButton_Right | \
                          HidNpadButton_A | HidNpadButton_B | \
                          HidNpadButton_X | HidNpadButton_Y | \
                          HidNpadButton_Minus | HidNpadButton_Plus)
/* Shoulders/system keys used by the clear popup are also read from every pad,
   so a left Joy-Con in mouse mode never loses them. */
#define POPUP_BUTTON_MASK (HidNpadButton_L | HidNpadButton_R | \
                           HidNpadButton_ZL | HidNpadButton_ZR)

static int s_lastPX = -1, s_lastPY = -1;
static int s_cursorWX = 0, s_cursorWY = 0;   /* world units */
static int s_reticleSX = 0, s_reticleSY = 0; /* logical screen units */
static int s_inputSX = 0, s_inputSY = 0;     /* last active pointer position */
static s32 s_touchW = 1280, s_touchH = 720;
static bool s_resultMusicPlayed = false;

/* Menu / progression state. */
static int s_levelIndex = -1;      /* -1 on the title screen */
static int s_selected = 0;
static const u64 s_konami[] = {
    HidNpadButton_Up, HidNpadButton_Up,
    HidNpadButton_Down, HidNpadButton_Down,
    HidNpadButton_Left, HidNpadButton_Right,
    HidNpadButton_Left, HidNpadButton_Right,
    HidNpadButton_B, HidNpadButton_A,
};
#define KONAMI_LEN (int)(sizeof(s_konami) / sizeof(s_konami[0]))
static int s_konamiPos = 0;
static bool s_konamiPending = false;

/* Credits text from the Janitorial README (the original game's credit list). */
static const char *const s_credits[] = {
    "Web Remake: Isaiah Odhner",
    "Resource ripping: Rory Fewell, TommytheJerk",
    "Tools: CastRipperTool, SharpExport,",
    "swfExport Xtra, MP3 Xtra",
    "Extra sounds: Lego Creator,",
    "Lego Star Wars: Force Awakens",
    "",
    "Junkbot & Junkbot Undercover",
    "Art: Peter Lee / Frank Lantz",
    "Code: Ranjit Bhatnagar / Peter Lee",
    "Game Design: Frank Lantz /",
    "Eric Zimmerman / Nick Fortugno",
    "Project Management: Peter Lee / Frank Lantz",
    "Sound: Michael Sweet - Blister Media",
    "Lego Producer: Tomas Clark",
    "",
    "Switch 2 port: mysterypaint",
};
#define CREDITS_LINES (int)(sizeof(s_credits) / sizeof(s_credits[0]))
#define CREDITS_VISIBLE 10

/* Original intro cinematic: 15 fps flipbook built from the SWF at build time. */
#define INTRO_STEP_US 66667u
#define INTRO_BTN_X (JB_SCREEN_W - 116)
#define INTRO_BTN_Y (JB_SCREEN_H - 38)
#define INTRO_BTN_W 96
#define INTRO_BTN_H 26
static Uint16 *s_introSeq = NULL;
static int s_introSeqCount = 0;
static int s_introStep = 0;
static Uint32 s_introAccumUs = 0;

/* In-game modal dialogs (level hint / credits / confirmations). The quit
   prompt is deliberately NOT one of these: it has its own state so it can sit
   above every other popup and take input priority at any time. */
static Modal s_modal = MODAL_NONE;
static int s_modalChoice = 0;
static bool s_quitPrompt = false;
static int s_quitChoice = 0;   /* 0 = No, 1 = Yes */
static SDL_Rect s_quitNoRect = { 0, 0, 0, 0 };
static SDL_Rect s_quitYesRect = { 0, 0, 0, 0 };
static int s_modalScroll = 0;
static int s_eraseStep = 0;   /* 1 = first warning, 2 = final warning */
static int s_selectedSlot = 0;
/* File-select focus walks the three file rows plus COPY, JUKEBOX and ERASE,
   so a controller can reach every action without switching to pointer mode. */
#define SLOT_FOCUS_COUNT (JB_SAVE_SLOTS + 3)
#define SLOT_FOCUS_COPY JB_SAVE_SLOTS
#define SLOT_FOCUS_JUKEBOX (JB_SAVE_SLOTS + 1)
#define SLOT_FOCUS_ERASE (JB_SAVE_SLOTS + 2)
static int s_slotFocus = 0;
/* Up/down walks four rows: slot 1, slot 2, slot 3, then the button row.
   Left/right walks the three buttons on that row and remembers the column. */
static int s_slotButtonCol = 0;
static FileAction s_fileAction = FILE_ACTION_NONE;
static int s_fileActionOrigin = 0;  /* focus restored when an action is cancelled */
static int s_jukeboxTrack = 0;
static int s_jukeboxHint = 0;   /* frames left on the locked-button explanation */
/* Jukebox sequence preview: one entry slides in per real playback change. */
static int s_previewTracks[JUKEBOX_PREVIEW_COUNT] = { -1, -1, -1, -1, -1 };
static unsigned s_previewSerial = 0;
static float s_previewScroll = 0.0f;
static int s_eraseSlot = 0;
static int s_copySource = -1;
static int s_copyDest = -1;
static int s_copyStep = 0;    /* 2 = confirm (source/dest picked from the rows) */
/* Title-screen CLEAR ALL DATA confirmation. The full Konami sequence is the
   confirmation gesture; any wrong key restarts it, X cancels. */
static int s_clearKeyPos = 0;
static int s_clearWrongTimer = 0;
static int s_clearDoneTimer = 0;
static bool s_clearComplete = false;
/* Building (1-based) newly unlocked by the current clear, 0 when none. */
static int s_unlockedBuilding = 0;
/* A B press that changes screen must be released before another B counts. */
static bool s_bNeedsRelease = false;
static char s_modalHeading[200];
static char s_modalBody[8][180];
static int s_modalBodyCount = 0;
static SDL_Rect s_modalPrevRect = { 0, 0, 0, 0 };
static SDL_Rect s_modalNextRect = { 0, 0, 0, 0 };
static SDL_Rect s_hintRect = { 0, 0, 0, 0 };
static SDL_Rect s_resultHintRect = { 0, 0, 0, 0 };
static SDL_Rect s_selPrevRect = { 0, 0, 0, 0 };
static SDL_Rect s_selNextRect = { 0, 0, 0, 0 };

/* Cached level-select canvas: text is expensive to rasterise every frame. */
static SDL_Texture *s_selectCache = NULL;
static bool s_selectDirty = true;
static void select_mark_dirty(void) { s_selectDirty = true; }

/* Menu navigation: D-pad and both analogue sticks, with capped key repeat.
   Analog movement moves once on deflection, waits at least half a second, then
   repeats at a fixed rate instead of accelerating with the frame rate. */
#define MENU_ANALOG_DELAY_US 500000ULL
#define MENU_ANALOG_REPEAT_US 110000ULL
typedef struct {
    int keyRepeat;
    int analogDir;
    Uint64 analogSinceUs;
    Uint64 analogLastUs;
} MenuRepeat;

static MenuRepeat s_navRepeat;
static MenuRepeat s_pageRepeat;
static MenuRepeat s_modalRepeat;
static MenuRepeat s_modalPageRepeat;
static MenuRepeat s_modalScrollRepeat;

static void menu_analog_reset(MenuRepeat *r) {
    r->analogDir = 0;
    r->analogSinceUs = 0;
    r->analogLastUs = 0;
}

static void menu_repeat_reset(MenuRepeat *r) {
    r->keyRepeat = 0;
    menu_analog_reset(r);
}

static void nav_timers_reset(void) {
    menu_repeat_reset(&s_navRepeat);
    menu_repeat_reset(&s_pageRepeat);
    menu_repeat_reset(&s_modalRepeat);
    menu_repeat_reset(&s_modalPageRepeat);
    menu_repeat_reset(&s_modalScrollRepeat);
}

/* Popup frames expand from the centre and collapse back the same way. The
   timings stay under ~0.1s so the flourish never feels sluggish; while a frame
   is moving, its contents (text, buttons, hitboxes) stay hidden and inert. */
typedef enum {
    POPUP_CLOSED = 0, POPUP_OPENING, POPUP_OPEN, POPUP_CLOSING
} PopupPhase;

typedef struct {
    PopupPhase phase;
    Uint64 startUs;
} PopupAnim;

#define POPUP_OPEN_US 105000ULL
#define POPUP_CLOSE_US 85000ULL

static PopupAnim s_modalAnim = { POPUP_CLOSED, 0 };
static PopupAnim s_quitAnim = { POPUP_CLOSED, 0 };
static PopupAnim s_resultAnim = { POPUP_CLOSED, 0 };
static Modal s_prevModal = MODAL_NONE;
static bool s_prevQuit = false;
static int s_prevResult = 0;

static Uint64 popup_now_us(void) {
    return (Uint64)SDL_GetTicks() * 1000ULL;
}

static void popup_start(PopupAnim *a, Uint64 nowUs) {
    a->phase = POPUP_OPENING;
    a->startUs = nowUs;
}

static void popup_finish(PopupAnim *a, Uint64 nowUs) {
    if (a->phase == POPUP_CLOSED) return;
    a->phase = POPUP_CLOSING;
    a->startUs = nowUs;
}

static void popup_reset(PopupAnim *a) {
    a->phase = POPUP_CLOSED;
    a->startUs = 0;
}

static void popup_tick(PopupAnim *a, Uint64 nowUs) {
    Uint64 duration = a->phase == POPUP_CLOSING ? POPUP_CLOSE_US : POPUP_OPEN_US;
    if ((a->phase == POPUP_OPENING || a->phase == POPUP_CLOSING) &&
        nowUs >= a->startUs && nowUs - a->startUs >= duration) {
        a->phase = a->phase == POPUP_OPENING ? POPUP_OPEN : POPUP_CLOSED;
        a->startUs = 0;
    }
}

static bool popup_busy(const PopupAnim *a) {
    return a->phase == POPUP_OPENING || a->phase == POPUP_CLOSING;
}

static bool popup_drawable(const PopupAnim *a) {
    return a->phase != POPUP_CLOSED;
}

static bool popup_content_ready(const PopupAnim *a) {
    return a->phase == POPUP_OPEN;
}

/* Slight overshoot on the way in, cubic ease-in on the way out. */
static float popup_open_ease(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

static float popup_scale(const PopupAnim *a, Uint64 nowUs) {
    if (a->phase == POPUP_CLOSED) return 0.0f;
    if (a->phase == POPUP_OPEN) return 1.0f;
    Uint64 duration = a->phase == POPUP_CLOSING ? POPUP_CLOSE_US : POPUP_OPEN_US;
    float t = nowUs > a->startUs ? (float)(nowUs - a->startUs) / (float)duration : 0.0f;
    if (t > 1.0f) t = 1.0f;
    if (a->phase == POPUP_OPENING) return 0.08f + 0.92f * popup_open_ease(t);
    return 1.0f - 0.92f * t * t * t;
}

static SDL_Rect popup_scaled_rect(SDL_Rect full, float scale) {
    int w = (int)(full.w * scale);
    int h = (int)(full.h * scale);
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    SDL_Rect out = { full.x + (full.w - w) / 2,
                     full.y + (full.h - h) / 2, w, h };
    return out;
}

/* Called once before input and once before drawing so a popup opened or closed
   in this same frame still gets the full animation. */
static void popup_sync(Uint64 nowUs, Screen screen) {
    bool modalActive = s_modal != MODAL_NONE;
    if (modalActive && s_prevModal == MODAL_NONE) popup_start(&s_modalAnim, nowUs);
    else if (!modalActive && s_prevModal != MODAL_NONE) popup_finish(&s_modalAnim, nowUs);
    s_prevModal = s_modal;

    if (s_quitPrompt && !s_prevQuit) popup_start(&s_quitAnim, nowUs);
    else if (!s_quitPrompt && s_prevQuit) popup_finish(&s_quitAnim, nowUs);
    s_prevQuit = s_quitPrompt;

    int resultActive = g_result != 0 && screen == SCREEN_LEVEL ? 1 : 0;
    if (resultActive && !s_prevResult) popup_start(&s_resultAnim, nowUs);
    else if (!resultActive && s_prevResult) {
        if (screen == SCREEN_LEVEL) popup_finish(&s_resultAnim, nowUs);
        else popup_reset(&s_resultAnim);
    }
    s_prevResult = resultActive;

    popup_tick(&s_modalAnim, nowUs);
    popup_tick(&s_quitAnim, nowUs);
    popup_tick(&s_resultAnim, nowUs);
}

static bool popup_input_locked(void) {
    return popup_busy(&s_modalAnim) || popup_busy(&s_quitAnim) ||
           popup_busy(&s_resultAnim);
}

/* One cue for every real cursor/selection change in every menu and popup. It
   is deliberately quieter than the button-confirm click. */
static void menu_cursor_moved(void) {
    audio_play(SFX_MENU_MOVE);
}

/* A/B need a fresh physical press after a screen transition, otherwise the
   same button can back out of two screens in a row. Menu buttons are collected
   from every controller, so edges are tracked per source: a face button on a
   second Joy-Con must not be swallowed by a held button on the mouse Joy-Con. */
static int s_screenCooldown = 0;
static u64 s_menuPrev[PAD_SOURCES];
/* Confirmed warnings, handled by the main loop after modal input. */
static bool s_leaveLevel = false;
static bool s_restartLevel = false;

/* Sleep / HOME menu handling. HOME and sleep are never blocked at the system
   level (that can hang the console); instead loading defers the freeze until
   assets are ready, and play freezes immediately after that. */
static AppletHookCookie s_focusCookie;
static volatile bool s_suspended = false;
static volatile bool s_resetTiming = false;
static volatile bool s_loadingAssets = true;
static volatile bool s_freezePending = false;
static bool s_audioReady = false;

static int min_i(int a, int b) { return a < b ? a : b; }

static bool point_in(int x, int y, SDL_Rect r) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Hitboxes are defined here as well as drawn so the first frame is clickable. */
static SDL_Rect select_prev_rect(void) {
    return (SDL_Rect){ SELECT_X, SELECT_ARROW_Y, SELECT_ARROW_W, SELECT_ARROW_H };
}

static SDL_Rect select_next_rect(void) {
    return (SDL_Rect){ SELECT_X + SELECT_W - SELECT_ARROW_W,
                       SELECT_ARROW_Y, SELECT_ARROW_W, SELECT_ARROW_H };
}

static SDL_Rect slot_row_rect(int row) {
    return (SDL_Rect){ SLOT_ROW_X, SLOT_ROW_Y + row * (SLOT_ROW_H + SLOT_ROW_GAP),
                       SLOT_ROW_W, SLOT_ROW_H };
}

static SDL_Rect slot_copy_rect(void) {
    return (SDL_Rect){ SLOT_COPY_X, SLOT_BTN_Y, SLOT_BTN_W, SLOT_BTN_H };
}

static SDL_Rect slot_jukebox_rect(void) {
    return (SDL_Rect){ SLOT_JUKEBOX_X, SLOT_BTN_Y, SLOT_BTN_W, SLOT_BTN_H };
}

static SDL_Rect slot_erase_rect(void) {
    return (SDL_Rect){ SLOT_ERASE_X, SLOT_BTN_Y, SLOT_BTN_W, SLOT_BTN_H };
}

/* Any actual movement of the file-select cursor gets the same quiet menu cue.
   Up/down treats the three buttons as one row below slot 3, wrapping from that
   row back to slot 1. During a COPY/ERASE action focus stays on the slot rows. */
static bool slot_focus_move(int delta) {
    int next;
    if (s_fileAction != FILE_ACTION_NONE) {
        next = (s_slotFocus + delta + JB_SAVE_SLOTS) % JB_SAVE_SLOTS;
    } else {
        int row = s_slotFocus < JB_SAVE_SLOTS ? s_slotFocus : JB_SAVE_SLOTS;
        next = (row + delta + JB_SAVE_SLOTS + 1) % (JB_SAVE_SLOTS + 1);
        if (next == JB_SAVE_SLOTS) next = JB_SAVE_SLOTS + s_slotButtonCol;
    }
    if (next == s_slotFocus) return false;
    s_slotFocus = next;
    if (s_slotFocus < JB_SAVE_SLOTS) s_selectedSlot = s_slotFocus;
    menu_cursor_moved();
    return true;
}

/* Left/right only walks COPY -> JUKEBOX -> ERASE on the button row. It does
   nothing on a slot row, so horizontal input never changes the open file. */
static bool slot_focus_button_move(int delta) {
    if (s_fileAction != FILE_ACTION_NONE || s_slotFocus < JB_SAVE_SLOTS)
        return false;
    int next = (s_slotButtonCol + delta + 3) % 3;
    if (next == s_slotButtonCol) return false;
    s_slotButtonCol = next;
    s_slotFocus = JB_SAVE_SLOTS + next;
    menu_cursor_moved();
    return true;
}

static void slot_focus_set_quiet(int focus) {
    if (focus < 0 || focus >= SLOT_FOCUS_COUNT) return;
    s_slotFocus = focus;
    if (focus < JB_SAVE_SLOTS) s_selectedSlot = focus;
    else s_slotButtonCol = focus - JB_SAVE_SLOTS;
}

static void slot_focus_set(int focus) {
    if (focus < 0 || focus >= SLOT_FOCUS_COUNT || focus == s_slotFocus) return;
    slot_focus_set_quiet(focus);
    menu_cursor_moved();
}

/* The jukebox is a reward, not a starting-screen toy: one file needs 40 gold
   awards, or any file needs the Konami unlock flag. */
static bool jukebox_unlocked(void) {
    for (int i = 0; i < save_slot_count(); i++) {
        if (save_slot_par_count(i) >= 40 || save_slot_unlock_all(i)) return true;
    }
    return false;
}

/* The timing table is ordered by BGM group and then by segment; mirror the
   groups used by the build's MUSIC_GROUPS so navigation follows the soundtrack
   instead of an arbitrary flat list. */
static const char *const s_bgmGroups[] = {
    "intro_", "demo6_", "lego1.", "demo_4.", "demo_5.", "demo_6."
};
#define BGM_GROUP_COUNT ((int)(sizeof(s_bgmGroups) / sizeof(s_bgmGroups[0])))

static int jukebox_group(int track) {
    const char *name = audio_music_track_name(track);
    for (int i = 0; i < BGM_GROUP_COUNT; i++)
        if (strncmp(name, s_bgmGroups[i], strlen(s_bgmGroups[i])) == 0) return i;
    return 0;
}

static int jukebox_group_first(int group) {
    int count = audio_music_track_count();
    for (int i = 0; i < count; i++)
        if (jukebox_group(i) == group) return i;
    return 0;
}

static int jukebox_group_last(int group) {
    int count = audio_music_track_count();
    int last = jukebox_group_first(group);
    for (int i = last + 1; i < count && jukebox_group(i) == group; i++) last = i;
    return last;
}

static int jukebox_group_cols(int group) {
    return jukebox_group_last(group) - jukebox_group_first(group) + 1;
}

static SDL_Rect jukebox_item_rect(int index) {
    int group = jukebox_group(index);
    int col = index - jukebox_group_first(group);
    return (SDL_Rect){
        JUKEBOX_LIST_X + col * (JUKEBOX_COL_W + JUKEBOX_COL_GAP),
        JUKEBOX_LIST_Y + group * JUKEBOX_ROW_H,
        JUKEBOX_COL_W, JUKEBOX_ROW_HGT
    };
}

/* Up/down walks whole groups; left/right walks the segments in one group.
   Both axes wrap so no edge of the grid is a dead end. */
static void jukebox_move_group(int delta) {
    int group = jukebox_group(s_jukeboxTrack);
    int next = (group + delta + BGM_GROUP_COUNT) % BGM_GROUP_COUNT;
    int col = s_jukeboxTrack - jukebox_group_first(group);
    int lastCol = jukebox_group_cols(next) - 1;
    if (col > lastCol) col = lastCol;
    int target = jukebox_group_first(next) + col;
    if (target == s_jukeboxTrack) return;
    s_jukeboxTrack = target;
    menu_cursor_moved();
}

static void jukebox_move_track(int delta) {
    int group = jukebox_group(s_jukeboxTrack);
    int first = jukebox_group_first(group);
    int cols = jukebox_group_cols(group);
    if (cols <= 1) return;
    int col = (s_jukeboxTrack - first + delta + cols) % cols;
    s_jukeboxTrack = first + col;
    menu_cursor_moved();
}

static bool jukebox_hit_test(int x, int y, int *index) {
    int count = audio_music_track_count();
    if (count <= 0) return false;
    int row = (y - JUKEBOX_LIST_Y) / JUKEBOX_ROW_H;
    if (row < 0 || row >= BGM_GROUP_COUNT) return false;
    int col = (x - JUKEBOX_LIST_X) / (JUKEBOX_COL_W + JUKEBOX_COL_GAP);
    if (col < 0) return false;
    int idx = jukebox_group_first(row) + col;
    if (idx >= count || jukebox_group(idx) != row) return false;
    if (!point_in(x, y, jukebox_item_rect(idx))) return false;
    if (index) *index = idx;
    return true;
}

static void jukebox_preview_reset(void) {
    for (int i = 0; i < JUKEBOX_PREVIEW_COUNT; i++) s_previewTracks[i] = -1;
    s_previewSerial = audio_music_play_serial();
    s_previewScroll = 0.0f;
}

/* One entry per real playback start: the strip shifts left and the new segment
   slides in from the right, eased every frame so it feels responsive. */
static void jukebox_preview_sync(void) {
    unsigned serial = audio_music_play_serial();
    if (serial == s_previewSerial) return;
    s_previewSerial = serial;
    int track = audio_music_current_track();
    if (track < 0) return;
    /* Even a looping single segment is a fresh play: slide one step per loop. */
    for (int i = 0; i < JUKEBOX_PREVIEW_COUNT - 1; i++)
        s_previewTracks[i] = s_previewTracks[i + 1];
    s_previewTracks[JUKEBOX_PREVIEW_COUNT - 1] = track;
    s_previewScroll = 1.0f;
}

static void jukebox_preview_tick(void) {
    if (s_previewScroll <= 0.0f) return;
    s_previewScroll -= s_previewScroll * 0.18f;
    if (s_previewScroll < 0.003f) s_previewScroll = 0.0f;
}

static Screen open_jukebox(void) {
    int count = audio_music_track_count();
    int current = audio_music_current_track();
    s_jukeboxTrack = (current >= 0 && current < count) ? current : 0;
    s_jukeboxHint = 0;
    jukebox_preview_reset();
    s_bNeedsRelease = true;
    s_screenCooldown = 6;
    nav_timers_reset();
    audio_play(SFX_BUTTON_CLICK);
    LOG("screen -> JUKEBOX (unlocked)");
    return SCREEN_JUKEBOX;
}

static Screen close_jukebox(void) {
    s_slotFocus = SLOT_FOCUS_JUKEBOX;
    /* Leaving the jukebox back to File Select wraps up whatever it queued. */
    audio_music_wrap_up();
    s_bNeedsRelease = true;
    s_screenCooldown = 6;
    nav_timers_reset();
    audio_play(SFX_BUTTON_CLICK);
    LOG("screen -> SLOTS from JUKEBOX");
    return SCREEN_SLOTS;
}

static SDL_Rect clear_data_rect(void) {
    return (SDL_Rect){ CLEAR_DATA_BOX_X, CLEAR_DATA_BOX_Y,
                       CLEAR_DATA_BOX_W, CLEAR_DATA_BOX_H };
}

static SDL_Rect credits_prev_rect(void) {
    return (SDL_Rect){ MODAL_X + 40, MODAL_ARROW_Y, MODAL_ARROW_W, MODAL_ARROW_H };
}

static SDL_Rect credits_next_rect(void) {
    return (SDL_Rect){ MODAL_X + MODAL_W - 84, MODAL_ARROW_Y,
                       MODAL_ARROW_W, MODAL_ARROW_H };
}

static void level_id(int index, char *out, size_t size) {
    if (index < 0 || index >= g_levelCount) {
        snprintf(out, size, "--");
        return;
    }
    snprintf(out, size, "%d-%d", index / JB_LEVELS_PER_GROUP + 1,
             index % JB_LEVELS_PER_GROUP + 1);
}

static int page_count(void) {
    int pages = (g_levelCount + JB_LEVELS_PER_GROUP - 1) / JB_LEVELS_PER_GROUP;
    return pages < 1 ? 1 : pages;
}

static void clamp_selection(void) {
    if (s_selected < 0) s_selected = 0;
    if (s_selected >= g_levelCount) s_selected = g_levelCount - 1;
}

/* Up/Down wraps within the current building. Page changes are strictly a
   Left/Right (or shoulder button /-arrow click) action. */
static void select_move_row(int dir) {
    int first = (s_selected / JB_LEVELS_PER_GROUP) * JB_LEVELS_PER_GROUP;
    int count = min_i(JB_LEVELS_PER_GROUP, g_levelCount - first);
    if (count <= 0) { clamp_selection(); return; }
    int row = s_selected - first;
    row = (row + dir) % count;
    if (row < 0) row += count;
    int next = first + row;
    if (next == s_selected) return;
    s_selected = next;
    select_mark_dirty();
    menu_cursor_moved();            /* level-select cursor moved */
}

static void select_page(int dir) {
    int pages = page_count();
    int page = s_selected / JB_LEVELS_PER_GROUP;
    int next = page + dir;
    if (next < 0 || next >= pages) return;
    s_selected = next * JB_LEVELS_PER_GROUP;
    clamp_selection();
    select_mark_dirty();
    audio_play(SFX_TAB_SWITCH);     /* original's dedicated building-switch cue */
    LOG("level select page -> %d/%d", next + 1, pages);
}

static void credits_clamp_scroll(void) {
    int max = CREDITS_LINES - CREDITS_VISIBLE;
    if (max < 0) max = 0;
    if (s_modalScroll < 0) s_modalScroll = 0;
    if (s_modalScroll > max) s_modalScroll = max;
}

static void credits_scroll_lines(int dir) {
    int before = s_modalScroll;
    s_modalScroll += dir;
    credits_clamp_scroll();
    if (s_modalScroll != before) menu_cursor_moved();
}

static int credits_page(void) {
    int max = CREDITS_LINES - CREDITS_VISIBLE;
    if (max < 0) max = 0;
    int pages = (CREDITS_LINES + CREDITS_VISIBLE - 1) / CREDITS_VISIBLE;
    if (pages < 1) pages = 1;
    int page = 1;
    for (int p = 1; p < pages; p++) {
        int start = p * CREDITS_VISIBLE;
        if (start > max) start = max;
        if (s_modalScroll >= start) page = p + 1;
    }
    return page;
}

static void credits_scroll_pages(int dir) {
    int max = CREDITS_LINES - CREDITS_VISIBLE;
    if (max < 0) max = 0;
    int before = s_modalScroll;
    if (max > 0 && dir > 0 && s_modalScroll < max)
        s_modalScroll = min_i(s_modalScroll + CREDITS_VISIBLE, max);
    else if (max > 0 && dir < 0 && s_modalScroll > 0)
        s_modalScroll = (s_modalScroll - CREDITS_VISIBLE) > 0 ?
                        s_modalScroll - CREDITS_VISIBLE : 0;
    credits_clamp_scroll();
    if (s_modalScroll != before) menu_cursor_moved();
}

/* Returns true when the press belongs to an in-progress Konami combo and must
   be hidden from navigation. Only Left/Right/A/B are locked: Up/Down must keep
   working so level select never feels like it drops inputs. */
static bool feed_konami(u64 kDown) {
    if (!kDown) return false;
    u64 want = s_konami[s_konamiPos];
    if (kDown & want) {
        s_konamiPos++;
        if (s_konamiPos == KONAMI_LEN) {
            s_konamiPos = 0;
            save_set_unlock_all();
            audio_play(SFX_YEAH);
            select_mark_dirty();
            /* The last A (or B) is part of the cheat, never a menu confirm.
               A separate OK prompt makes the unlock explicit. */
            s_konamiPending = true;
            return true;
        }
        return (want == HidNpadButton_Left || want == HidNpadButton_Right ||
                want == HidNpadButton_A || want == HidNpadButton_B);
    }
    s_konamiPos = (kDown & s_konami[0]) ? 1 : 0;
    return false;
}

static const char *pad_source_name(int i) {
    if (i == PAD_SOURCE_HANDHELD) return "Handheld";
    if (i >= 0 && i < 8) {
        static char name[24];
        snprintf(name, sizeof(name), "Npad %d", i + 1);
        return name;
    }
    return "none";
}

static bool stick_active(HidAnalogStickState s, int dz) {
    return (s.x > dz || s.x < -dz || s.y > dz || s.y < -dz);
}

/* Menu sticks: SDK positive Y is up, so invert it for menu "down" (the world
   reticle applies its own inversion separately). A diagonal is resolved to
   its dominant axis so two directions do not fire from one stick at once. */
static void stick_menu_dirs(const PadState *pad, int *upDown, int *leftRight) {
    const int dz = 5000;
    *upDown = *leftRight = 0;
    for (unsigned i = 0; i < 2; i++) {
        HidAnalogStickState s = padGetStickPos(pad, i);
        int ax = s.x < 0 ? -s.x : s.x;
        int ay = s.y < 0 ? -s.y : s.y;
        if (ay >= ax) {
            if (ay > dz && !*upDown) *upDown = s.y < 0 ? 1 : -1;
        } else if (ax > dz && !*leftRight) {
            *leftRight = s.x < 0 ? -1 : 1;
        }
    }
}

/* Either stick can drive the reticle. A right Joy-Con reports its stick in
   slot 1, while left/handheld/pro controllers normally use slot 0. */
static HidAnalogStickState reticle_stick(const PadState *pad) {
    const int dz = 1200;
    HidAnalogStickState l = padGetStickPos(pad, 0);
    HidAnalogStickState r = padGetStickPos(pad, 1);
    bool lm = stick_active(l, dz), rm = stick_active(r, dz);
    if (s_reticleStick == 1 && rm) return r;
    if (s_reticleStick == 0 && lm) return l;
    if (rm) { s_reticleStick = 1; return r; }
    if (lm) { s_reticleStick = 0; return l; }
    return s_reticleStick == 1 ? r : l;
}

static u64 click_button_mask(void) {
    return HidNpadButton_L | HidNpadButton_ZL |
           HidNpadButton_R | HidNpadButton_ZR;
}

static u32 click_pointer_mask(ClickOwner owner) {
    if (owner == CLICK_MOUSE_LEFT) return HidMouseButton_Left;
    if (owner == CLICK_MOUSE_RIGHT) return HidMouseButton_Right;
    return 0;
}

static u64 click_pad_mask(ClickOwner owner) {
    switch (owner) {
        case CLICK_PAD_L:  return HidNpadButton_L;
        case CLICK_PAD_ZL: return HidNpadButton_ZL;
        case CLICK_PAD_R:  return HidNpadButton_R;
        case CLICK_PAD_ZR: return HidNpadButton_ZR;
        default: return 0;
    }
}

static bool click_owner_down(ClickOwner owner, u32 pDown, u64 kDown) {
    return (click_pointer_mask(owner) && (pDown & click_pointer_mask(owner))) ||
           (click_pad_mask(owner) && (kDown & click_pad_mask(owner)));
}

static bool click_owner_up(ClickOwner owner, u32 pUp, u64 kUp) {
    return (click_pointer_mask(owner) && (pUp & click_pointer_mask(owner))) ||
           (click_pad_mask(owner) && (kUp & click_pad_mask(owner)));
}

static bool click_owner_held(ClickOwner owner, u32 pHeld, u64 kHeld) {
    return (click_pointer_mask(owner) && (pHeld & click_pointer_mask(owner))) ||
           (click_pad_mask(owner) && (kHeld & click_pad_mask(owner)));
}

static ClickOwner click_owner_from_down(u32 pDown, u64 kDown) {
    if (pDown & HidMouseButton_Left) return CLICK_MOUSE_LEFT;
    if (pDown & HidMouseButton_Right) return CLICK_MOUSE_RIGHT;
    if (kDown & HidNpadButton_L) return CLICK_PAD_L;
    if (kDown & HidNpadButton_ZL) return CLICK_PAD_ZL;
    if (kDown & HidNpadButton_R) return CLICK_PAD_R;
    if (kDown & HidNpadButton_ZR) return CLICK_PAD_ZR;
    return CLICK_NONE;
}

/* Touch HID samples can report an End (or otherwise stale) entry with zeroed
   coordinates. Treating that as a live finger sent both the drag anchor and the
   finger magnifier to 0,0. Track one finger id, ignore End entries, and never
   let a suspicious zero-origin sample overwrite the last real position. */
static void read_touch_input(bool *down, bool *held, bool *up, int *lx, int *ly) {
    *down = false;
    *held = false;
    *up = false;
    *lx = s_lastTouchX;
    *ly = s_lastTouchY;

    HidTouchScreenState state = {0};
    size_t states = hidGetTouchScreenStates(&state, 1);
    const int maxTouches = (int)(sizeof(state.touches) / sizeof(state.touches[0]));
    const HidTouchState *t = NULL;

    if (states > 0) {
        if (s_touchActive) {
            /* A second finger must never steal a drag already in progress. */
            for (int i = 0; i < state.count && i < maxTouches; i++) {
                const HidTouchState *candidate = &state.touches[i];
                if (candidate->finger_id != s_touchFinger) continue;
                if (candidate->attributes & HidTouchAttribute_End) continue;
                t = candidate;
                break;
            }
        } else {
            for (int i = 0; i < state.count && i < maxTouches; i++) {
                const HidTouchState *candidate = &state.touches[i];
                if (candidate->attributes & HidTouchAttribute_End) continue;
                t = candidate;
                break;
            }
        }
    }

    if (t) {
        bool fresh = !s_touchActive;
        int w = s_touchW > 0 ? s_touchW : 1280;
        int h = s_touchH > 0 ? s_touchH : 720;
        int x = (int)((long long)t->x * JB_SCREEN_W / w);
        int y = (int)((long long)t->y * JB_SCREEN_H / h);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= JB_SCREEN_W) x = JB_SCREEN_W - 1;
        if (y >= JB_SCREEN_H) y = JB_SCREEN_H - 1;

        /* A tracked finger teleporting to exactly (0,0) without a new Start is
           a bad sample, not a real move. Keep the last good coordinates so the
           held group stays where the player put it. */
        if (!fresh && t->x == 0 && t->y == 0) {
            x = s_lastTouchX;
            y = s_lastTouchY;
        } else {
            s_lastTouchX = x;
            s_lastTouchY = y;
        }

        if (fresh) {
            s_touchActive = true;
            s_touchFinger = t->finger_id;
            *down = true;
        }
        *held = true;
        *lx = x;
        *ly = y;
    } else if (s_touchActive) {
        s_touchActive = false;
        *up = true;
        *lx = s_lastTouchX;
        *ly = s_lastTouchY;
    }
}

/* How much a controller did this frame. Only the highest-scoring (or the
   already active tied) source is consumed, so two Joy-Cons never both drive
   the game on the same frame. */
static int pad_activity(const PadState *pad, const PadPointer *ptr, bool pointerReady) {
    bool pointerActive = false;
    if (pointerReady && ptr->active) {
        pointerActive = ptr->delta_x || ptr->delta_y || ptr->wheel_delta_y ||
                        padPointerGetButtonsDown(ptr) || padPointerGetButtonsUp(ptr) ||
                        padPointerGetButtons(ptr);
    }
    if (!padIsConnected(pad) && !pointerActive) return -1;

    int score = 0;
    if (padGetButtonsDown(pad)) score += 6;
    if (padGetButtons(pad)) score += 1;
    if (stick_active(padGetStickPos(pad, 0), 1200) ||
        stick_active(padGetStickPos(pad, 1), 1200)) score += 4;
    if (pointerActive) score += 5;
    return score;
}

static int choose_active_pad(void) {
    int scores[PAD_SOURCES];
    for (int i = 0; i < PAD_SOURCES; i++)
        scores[i] = pad_activity(&s_pads[i], &s_pointers[i], s_pointerReady[i]);

    int cur = s_activePad;
    /* A live grab owns the input source until release. Touch drags have no pad
       owner at all, so without this a stray pad sample can flip s_activePad,
       cancel the drag, and rebuild it from stale pointer state. */
    if (game_is_dragging()) return cur;
    /* A latched click owns its controller until release. Without this, a tiny
       movement on a second Joy-Con can steal the active pad mid-grab, which
       cancels the drag and makes L/ZL/R/ZR grabbing feel unreliable. */
    if (cur >= 0 && cur < PAD_SOURCES && s_clickOwner != CLICK_NONE)
        return cur;
    if (cur >= 0 && cur < PAD_SOURCES && scores[cur] >= 0) {
        for (int i = 0; i < PAD_SOURCES; i++)
            if (i != cur && scores[i] > scores[cur]) cur = i;
        return cur;
    }
    int best = -1;
    for (int i = 0; i < PAD_SOURCES; i++)
        if (scores[i] > 0 && (best < 0 || scores[i] > scores[best])) best = i;
    return best;
}

/* D-pad navigation repeats after a short delay. Analog deflection moves once
   immediately, then waits half a second before settling into a fixed repeat
   rate; it never accelerates with the display frame rate. */
static int menu_move_axis(u64 kDown, u64 kHeld, u64 negMask, u64 posMask,
                          int stickDir, MenuRepeat *repeat) {
    if (kDown & negMask) {
        repeat->keyRepeat = 14;
        menu_analog_reset(repeat);
        return -1;
    }
    if (kDown & posMask) {
        repeat->keyRepeat = 14;
        menu_analog_reset(repeat);
        return 1;
    }

    if (stickDir != 0) {
        Uint64 now = SDL_GetPerformanceCounter();
        if (repeat->analogDir != stickDir) {
            repeat->analogDir = stickDir;
            repeat->analogSinceUs = now;
            repeat->analogLastUs = now;
            return stickDir;
        }
        Uint64 freq = SDL_GetPerformanceFrequency();
        Uint64 delay = freq ? freq * MENU_ANALOG_DELAY_US / 1000000ULL
                            : MENU_ANALOG_DELAY_US;
        Uint64 interval = freq ? freq * MENU_ANALOG_REPEAT_US / 1000000ULL
                               : MENU_ANALOG_REPEAT_US;
        if (now - repeat->analogSinceUs >= delay &&
            now - repeat->analogLastUs >= interval) {
            repeat->analogLastUs = now;
            return stickDir;
        }
        return 0;
    }

    menu_analog_reset(repeat);
    if (repeat->keyRepeat > 0 && --repeat->keyRepeat <= 0) {
        if (kHeld & negMask) { repeat->keyRepeat = 6; return -1; }
        if (kHeld & posMask) { repeat->keyRepeat = 6; return 1; }
    }
    return 0;
}

static int menu_move(u64 kDown, u64 kHeld, int stickDir, MenuRepeat *repeat) {
    return menu_move_axis(kDown, kHeld, HidNpadButton_Up, HidNpadButton_Down,
                          stickDir, repeat);
}

static void title_screen_to_world(int sx, int sy, int *wx, int *wy) {
    *wx = sx - TITLE_WIN_X;
    *wy = sy - TITLE_WIN_Y + TITLE_WORLD_TOP;
}

/* Word-wrap a hint into the modal's fixed-width lines. */
static int wrap_text(const char *text, char out[][180], int maxLines, int maxWidth) {
    if (!text || !*text || maxLines <= 0) return 0;
    int lines = 0;
    char line[180];
    line[0] = '\0';
    size_t used = 0;
    char word[120];
    const char *p = text;
    while (*p && lines < maxLines - 1) {
        while (*p == ' ' || *p == '\t') p++;
        size_t w = 0;
        while (p[w] && p[w] != ' ' && p[w] != '\t') {
            if (w < sizeof(word) - 1) word[w] = p[w];
            w++;
            if (w >= sizeof(word) - 1) break;
        }
        word[w < sizeof(word) ? w : sizeof(word) - 1] = '\0';
        p += w;
        if (!w) break;
        if (used && render_text_width(g_res.font, line) + 8 +
                    render_text_width(g_res.font, word) > maxWidth) {
            snprintf(out[lines++], 180, "%s", line);
            line[0] = '\0';
            used = 0;
        }
        if (used)
            snprintf(line + used, sizeof(line) - used, " %.119s", word);
        else
            snprintf(line, sizeof(line), "%.119s", word);
        used = strlen(line);
    }
    if (used && lines < maxLines) snprintf(out[lines++], 180, "%s", line);
    return lines;
}

/* Moves 0 keeps the normal HUD colour. Under Par it eases from muted green to
   yellow; exactly Par is orange; over Par is red. */
static SDL_Color moves_color(int moves, int par) {
    if (moves <= 0) return LIGHT;
    if (par <= 0) return (SDL_Color){ 95, 180, 120, 255 };
    if (moves > par) return (SDL_Color){ 205, 100, 95, 255 };
    if (moves == par) return (SDL_Color){ 240, 150, 60, 255 };

    float t = par > 1 ? (float)(moves - 1) / (float)(par - 1) : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float g[3] = { 95.0f, 180.0f, 120.0f };
    float y[3] = { 215.0f, 190.0f, 90.0f };
    SDL_Color c;
    c.r = (Uint8)(g[0] + (y[0] - g[0]) * t);
    c.g = (Uint8)(g[1] + (y[1] - g[1]) * t);
    c.b = (Uint8)(g[2] + (y[2] - g[2]) * t);
    c.a = 255;
    return c;
}

static void draw_hud(SDL_Renderer *r) {
    SDL_Rect bar = { 0, 0, JB_SCREEN_W, 40 };
    render_panel(r, bar, BLACK, (SDL_Color){ 80, 95, 110, 255 });

    char id[16];
    level_id(s_levelIndex, id, sizeof(id));
    char line[320];
    snprintf(line, sizeof(line), "%s  %s", id, g_level.title);

    /* Line 1: level id + title, scrolling like a news ticker when too long. */
    int maxW = JB_SCREEN_W - 24;
    int w = render_text_width(g_res.font, line);
    if (w <= maxW) {
        render_text(r, g_res.font, line, 12, 2, WHITE, false);
    } else {
        SDL_Rect clip = { 0, 0, maxW + 12, 20 };
        SDL_RenderSetClipRect(r, &clip);
        int gap = 48, total = w + gap;
        int off = (g_frameCounter * 2) % total;
        render_text(r, g_res.font, line, 12 - off, 2, WHITE, false);
        render_text(r, g_res.font, line, 12 - off + total, 2, WHITE, false);
        SDL_RenderSetClipRect(r, NULL);
    }

    /* Line 2: Moves/Par, the yellow/black square immediately after it, then
       "| Best: N", and the Hint button off to the far right. */
    char second[96];
    snprintf(second, sizeof(second), "Moves: %d / Par: %d", g_moves, g_level.par);
    int x = 12;
    render_text(r, g_res.font, second, x, 21,
                moves_color(g_moves, g_level.par), false);
    x += render_text_width(g_res.font, second) + 10;

    SDL_Rect sq = { x, 23, 14, 14 };
    bool underPar = (g_level.par <= 0 || g_moves <= g_level.par);
    SDL_SetRenderDrawColor(r, underPar ? 255 : 0, underPar ? 235 : 0,
                           underPar ? 90 : 0, 255);
    SDL_RenderFillRect(r, &sq);
    SDL_SetRenderDrawColor(r, 190, 190, 190, 255);
    SDL_RenderDrawRect(r, &sq);
    x += 14 + 12;

    s_hintRect = (SDL_Rect){ JB_SCREEN_W - 88, 20, 74, 18 };
    if (s_levelIndex >= 0 && save_is_beaten(s_levelIndex)) {
        char best[64];
        snprintf(best, sizeof(best), "| Best: %d", save_best_moves(s_levelIndex));
        int bw = render_text_width(g_res.font, best);
        if (x + bw > s_hintRect.x - 12) x = s_hintRect.x - 12 - bw;
        render_text(r, g_res.font, best, x, 21, MUTED, false);
    }

    bool hintDisabled = s_modal != MODAL_NONE || s_quitPrompt;
    render_panel(r, s_hintRect,
                 hintDisabled ? (SDL_Color){ 28, 32, 38, 255 }
                              : (SDL_Color){ 40, 50, 65, 255 },
                 hintDisabled ? (SDL_Color){ 70, 75, 80, 255 }
                              : (SDL_Color){ 120, 130, 140, 255 });
    render_text(r, g_res.font, "Hint", s_hintRect.x + s_hintRect.w / 2, 20,
                hintDisabled ? DIM : YELLOW, true);

    /* The result panel owns the button prompts once the level ends; leaving the
       play footer up would advertise A as Reset on the clear screen. */
    if (!g_result)
        render_text(r, g_res.font,
                    "Click / Tap / LR Grab   A Reset   B Levels   Y Undo   X Mute   + Quit",
                    JB_SCREEN_W / 2, JB_SCREEN_H - 22, MUTED, true);
}

/* Only a clear can advance, and the final level in the whole game has no
   next destination. Building boundaries are not ends: the next building's
   first level follows the last level of the previous one. */
static bool result_has_next_level(void) {
    return g_result == 1 && s_levelIndex >= 0 &&
           s_levelIndex + 1 < g_levelCount;
}

static void draw_result(SDL_Renderer *r) {
    Uint64 nowUs = popup_now_us();
    if (g_result == 0 && !popup_drawable(&s_resultAnim)) return;
    float scale = popup_scale(&s_resultAnim, nowUs);
    if (scale <= 0.0f) return;
    SDL_Rect full = { 210, 185, 540, 165 };
    SDL_Rect panel = popup_scaled_rect(full, scale);
    SDL_Color frame = FRAME_BLUE;   /* Junkbot destroyed */
    if (g_result == 1)
        frame = (g_level.par > 0 && g_moves <= g_level.par) ? FRAME_GOLD : FRAME_GREEN;
    render_popup_frame(r, panel, (SDL_Color){ 25, 30, 40, 245 }, frame);
    if (!popup_content_ready(&s_resultAnim)) {
        s_resultHintRect = (SDL_Rect){ 0, 0, 0, 0 };
        return;
    }
    render_text(r, g_res.font, g_result == 1 ? "LEVEL CLEAR!" : "JUNKBOT DESTROYED",
                JB_SCREEN_W / 2, 220, YELLOW, true);
    char line[128];
    snprintf(line, sizeof(line), "Moves: %d", g_moves);
    SDL_Color moveColor = moves_color(g_moves, g_level.par);
    bool gotPar = g_result == 1 && g_level.par > 0 && g_moves <= g_level.par;
    if (gotPar) {
        const char *parText = "GOLD AWARD!";
        const int gap = 10;
        int moveW = render_text_width(g_res.font, line);
        int sepW = render_text_width(g_res.font, "|");
        int parW = render_text_width(g_res.font, parText);
        int totalW = moveW + gap + sepW + gap + parW;
        int x = JB_SCREEN_W / 2 - totalW / 2;
        render_text(r, g_res.font, line, x, 255, moveColor, false);
        render_text(r, g_res.font, "|", x + moveW + gap, 255, WHITE, false);
        render_text(r, g_res.font, parText, x + moveW + gap + sepW + gap,
                    255, YELLOW, false);
    } else {
        render_text(r, g_res.font, line, JB_SCREEN_W / 2, 255, moveColor, true);
    }
    if (g_result == 1) {
        if (result_has_next_level()) {
            render_text(r, g_res.font, "Next: A / ZL / ZR   Levels: B / L / R",
                        JB_SCREEN_W / 2, 282, LIGHT, true);
            render_text(r, g_res.font, "Retry: Y / - / +",
                        JB_SCREEN_W / 2, 300, LIGHT, true);
        } else {
            render_text(r, g_res.font, "Levels: B / L / R   Retry: Y / - / +",
                        JB_SCREEN_W / 2, 286, LIGHT, true);
        }
    } else {
        render_text(r, g_res.font, "A Retry   B Level select",
                    JB_SCREEN_W / 2, 286, LIGHT, true);
    }
    if (s_unlockedBuilding > 0) {
        char unlock[96];
        snprintf(unlock, sizeof(unlock), "BUILDING %d UNLOCKED!", s_unlockedBuilding);
        render_text(r, g_res.font, unlock, JB_SCREEN_W / 2, 316, YELLOW, true);
        render_text(r, g_res.font, "Available in Level Select", JB_SCREEN_W / 2,
                    334, MUTED, true);
    }
    if (g_result == 2 && *g_level.hint) {
        s_resultHintRect = (SDL_Rect){ 390, 312, 180, 30 };
        render_panel(r, s_resultHintRect, (SDL_Color){ 45, 55, 70, 255 },
                     (SDL_Color){ 120, 130, 140, 255 });
        render_text(r, g_res.font, "Get Hint", 480, 313, YELLOW, true);
    } else {
        s_resultHintRect = (SDL_Rect){ 0, 0, 0, 0 };
    }
}

static void draw_arrow_button(SDL_Renderer *r, SDL_Rect rect, const char *label,
                              bool disabled) {
    render_panel(r, rect,
                 disabled ? (SDL_Color){ 28, 34, 42, 255 }
                          : (SDL_Color){ 45, 58, 75, 255 },
                 disabled ? (SDL_Color){ 85, 90, 96, 255 } : YELLOW);
    /* Two pixels lower: the font's ascent was sitting the glyph too high. */
    render_text(r, g_res.font, label, rect.x + rect.w / 2, rect.y + 5,
                disabled ? DIM : YELLOW, true);
}

static bool credits_at_first_page(void) { return s_modalScroll <= 0; }

static bool credits_at_last_page(void) {
    int max = CREDITS_LINES - CREDITS_VISIBLE;
    return max < 0 || s_modalScroll >= max;
}

static void draw_select_content(SDL_Renderer *r) {
    int page = s_selected / JB_LEVELS_PER_GROUP;
    if (page >= page_count()) page = page_count() - 1;
    bool unlocked = save_building_unlocked(page);
    int cleared = save_building_cleared(page);
    int first = page * JB_LEVELS_PER_GROUP;
    int total = min_i(JB_LEVELS_PER_GROUP, g_levelCount - first);

    char head[160];
    snprintf(head, sizeof(head), "SLOT %d   BUILDING %d   -   Page %d/%d%s",
             save_active_slot() + 1, page + 1, page + 1, page_count(),
             unlocked ? "" : "   [LOCKED]");
    render_text(r, g_res.font, head, JB_SCREEN_W / 2, 6, MUTED, true);
    char sub[180];
    snprintf(sub, sizeof(sub),
             "Cleared %d/%d   -   white tick = done, yellow = Par achieved",
             cleared, total);
    render_text(r, g_res.font, sub, JB_SCREEN_W / 2, 26, MUTED, true);

    SDL_Rect rule = { SELECT_X, 46, SELECT_W, 2 };
    SDL_SetRenderDrawColor(r, RULE.r, RULE.g, RULE.b, RULE.a);
    SDL_RenderFillRect(r, &rule);

    int playable = save_max_playable_index();
    for (int row = 0; row < JB_LEVELS_PER_GROUP; row++) {
        int idx = first + row;
        if (idx >= g_levelCount) break;
        SDL_Rect rect = { SELECT_X, SELECT_ROW_Y + row * SELECT_ROW_H, SELECT_W, 25 };
        bool locked = !unlocked || idx > playable;
        if (idx == s_selected)
            render_panel(r, rect, (SDL_Color){ 50, 65, 85, 255 }, YELLOW);

        /* Completion tickbox, left of the par square. The leftmost box keeps
           the original 10px inset. Its outline uses the same grey as the par
           box so neither stands out more than the other, and a 3px gap keeps
           their borders from touching. */
        SDL_Rect tick = { SELECT_X + 10, rect.y + 6, 13, 13 };
        SDL_SetRenderDrawColor(r, 140, 140, 140, 255);
        SDL_RenderDrawRect(r, &tick);
        if (save_is_beaten(idx)) {
            SDL_SetRenderDrawColor(r, WHITE.r, WHITE.g, WHITE.b, WHITE.a);
            SDL_RenderDrawLine(r, tick.x + 3, tick.y + 7,
                               tick.x + 5, tick.y + 9);
            SDL_RenderDrawLine(r, tick.x + 5, tick.y + 9,
                               tick.x + 10, tick.y + 3);
        }

        SDL_Rect icon = { tick.x + tick.w + 3, rect.y + 6, 13, 13 };
        bool par = save_got_par(idx);
        SDL_SetRenderDrawColor(r, par ? 255 : 0, par ? 235 : 0, par ? 90 : 0, 255);
        SDL_RenderFillRect(r, &icon);
        SDL_SetRenderDrawColor(r, 140, 140, 140, 255);
        SDL_RenderDrawRect(r, &icon);

        char id[16];
        level_id(idx, id, sizeof(id));
        char label[320];
        if (locked)
            snprintf(label, sizeof(label), "%s  ???   [LOCKED]", id);
        else
            snprintf(label, sizeof(label), "%s  %s", id, g_levelNames[idx]);
        SDL_Color color = locked ? DIM : (par ? GREEN : LIGHT);
        render_text(r, g_res.font, label, rect.x + 45, rect.y + 4, color, false);

        if (!locked && save_is_beaten(idx)) {
            char best[48];
            snprintf(best, sizeof(best), "Best %d", save_best_moves(idx));
            render_text_right(r, g_res.font, best, rect.x + rect.w - 10,
                              rect.y + 4, MUTED);
        }
    }

    /* Tap targets for flipping building pages, matching the keyboard/keys. */
    s_selPrevRect = select_prev_rect();
    s_selNextRect = select_next_rect();
    draw_arrow_button(r, s_selPrevRect, "<", page <= 0);
    draw_arrow_button(r, s_selNextRect, ">", page >= page_count() - 1);

    SDL_Rect rule2 = { SELECT_X, 493, SELECT_W, 2 };
    SDL_SetRenderDrawColor(r, RULE.r, RULE.g, RULE.b, RULE.a);
    SDL_RenderFillRect(r, &rule2);
    render_text(r, g_res.font, "Up/Down select   Left/Right (or sticks) building   A play   B back",
                JB_SCREEN_W / 2, 500, MUTED, true);
    render_text(r, g_res.font, "Konami code unlocks every building",
                JB_SCREEN_W / 2, 520, MUTED, true);
}

static void draw_select(SDL_Renderer *r) {
    if (!s_selectCache) {
        s_selectCache = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_TARGET,
                                          JB_SCREEN_W, JB_SCREEN_H);
        /* The animated backdrop is drawn every frame underneath this cached
           UI, so the cache itself must stay transparent. */
        if (s_selectCache)
            SDL_SetTextureBlendMode(s_selectCache, SDL_BLENDMODE_BLEND);
        s_selectDirty = true;
    }
    render_level_select_backdrop_cached(r, (Uint64)SDL_GetTicks() * 1000ULL);
    if (s_selectCache) {
        SDL_Texture *prev = SDL_GetRenderTarget(r);
        if (s_selectDirty) {
            SDL_SetRenderTarget(r, s_selectCache);
            SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
            SDL_RenderClear(r);
            draw_select_content(r);
            SDL_SetRenderTarget(r, prev);
            s_selectDirty = false;
        }
        SDL_RenderCopy(r, s_selectCache, NULL, NULL);
    } else {
        draw_select_content(r);
    }
}

static bool modal_has_two_buttons(void) {
    return s_modal == MODAL_LEVEL_EXIT || s_modal == MODAL_LEVEL_RESTART ||
           s_modal == MODAL_ERASE_SAVE ||
           (s_modal == MODAL_COPY_SLOT && s_copyStep == 2);
}

static int modal_button_count(void) {
    if (s_modal == MODAL_CLEAR_DATA) return 0;   /* keyboard gesture, no buttons */
    if (s_modal == MODAL_COPY_SLOT && s_copyStep == 1) return JB_SAVE_SLOTS;
    return modal_has_two_buttons() ? 2 : 1;
}

static const char *konami_key_label(int i) {
    static const char *const labels[KONAMI_LEN] = {
        "UP", "UP", "DOWN", "DOWN", "LEFT", "RIGHT", "LEFT", "RIGHT", "B", "A"
    };
    if (i < 0 || i >= KONAMI_LEN) return "?";
    return labels[i];
}

static const char *modal_button_label(int i) {
    if (s_modal == MODAL_LEVEL_EXIT) return i == 0 ? "Stay" : "Leave";
    if (s_modal == MODAL_LEVEL_RESTART) return i == 0 ? "Cancel" : "Restart";
    if (s_modal == MODAL_ERASE_SAVE)
        return i == 0 ? "Cancel" : (s_eraseStep == 1 ? "Continue" : "ERASE SLOT");
    if (s_modal == MODAL_COPY_SLOT) {
        if (s_copyStep == 1) {
            static char label[32];
            snprintf(label, sizeof(label), i == s_copySource ? "SLOT %d*" : "SLOT %d",
                     i + 1);
            return label;
        }
        return i == 0 ? "Cancel" : "COPY";
    }
    if (s_modal == MODAL_HINT) return "OK - Restart level";
    if (s_modal == MODAL_KONAMI) return "OK";
    return "OK";
}

/* Buttons are sized from the rendered text, so nothing overflows the box. */
static SDL_Rect modal_button_rect(int i) {
    if (s_modal == MODAL_COPY_SLOT && s_copyStep == 1) {
        int w = 180;
        int cx = JB_SCREEN_W / 2 + (i - 1) * 200;
        return (SDL_Rect){ cx - w / 2, MODAL_BTN_Y - 10, w, MODAL_BTN_H + 6 };
    }
    const char *label = modal_button_label(i);
    int w = render_text_width(g_res.font, label) + 40;
    if (w < 90) w = 90;
    int cx = JB_SCREEN_W / 2;
    if (modal_has_two_buttons())
        cx = i == 0 ? 350 : 610;
    SDL_Rect b = { cx - w / 2, MODAL_BTN_Y, w, MODAL_BTN_H };
    return b;
}

static void draw_modal(SDL_Renderer *r) {
    if (s_modal == MODAL_NONE && !popup_drawable(&s_modalAnim)) return;
    Uint64 nowUs = popup_now_us();
    float scale = popup_scale(&s_modalAnim, nowUs);
    if (scale <= 0.0f) return;
    SDL_Rect dim = { 0, 0, JB_SCREEN_W, JB_SCREEN_H };
    SDL_SetRenderDrawColor(r, 0, 0, 0, (Uint8)(185 * scale));
    SDL_RenderFillRect(r, &dim);
    SDL_Rect full = { MODAL_X, MODAL_Y, MODAL_W, MODAL_H };
    SDL_Rect panel = popup_scaled_rect(full, scale);
    render_popup_frame(r, panel, (SDL_Color){ 25, 30, 40, 255 }, FRAME_ORANGE);
    if (!popup_content_ready(&s_modalAnim)) return;

    if (s_modal == MODAL_CLEAR_DATA) {
        render_text(r, g_res.font, "CLEAR ALL DATA?", JB_SCREEN_W / 2, 70,
                    YELLOW, true);
        if (s_clearComplete) {
            render_text(r, g_res.font, "ALL SAVE DATA ERASED", JB_SCREEN_W / 2,
                        140, GREEN, true);
            render_text(r, g_res.font, "Press A, B or X to return to the title screen.",
                        JB_SCREEN_W / 2, 180, LIGHT, true);
        } else {
            render_text(r, g_res.font,
                        "Enter the entire Konami code to erase every save file.",
                        JB_SCREEN_W / 2, 115, LIGHT, true);
            render_text(r, g_res.font, "X cancels. A wrong key restarts the sequence.",
                        JB_SCREEN_W / 2, 150, MUTED, true);
        }
        const int boxW = 96, boxH = 34, gap = 16;
        int totalW = 5 * boxW + 4 * gap;
        int startX = (JB_SCREEN_W - totalW) / 2;
        bool wrong = s_clearWrongTimer > 0 && !s_clearComplete;
        for (int i = 0; i < KONAMI_LEN; i++) {
            int col = i % 5, row = i / 5;
            SDL_Rect box = { startX + col * (boxW + gap),
                             210 + row * (boxH + 18), boxW, boxH };
            bool lit = i < s_clearKeyPos || s_clearComplete;
            bool next = i == s_clearKeyPos && !lit;
            SDL_Color fill = wrong ? (SDL_Color){ 90, 30, 35, 255 }
                                   : (lit ? (SDL_Color){ 55, 75, 105, 255 }
                                          : (SDL_Color){ 34, 42, 54, 255 });
            SDL_Color edge = wrong ? (SDL_Color){ 220, 70, 70, 255 }
                                   : (lit ? YELLOW
                                          : (next ? (SDL_Color){ 170, 180, 195, 255 }
                                                  : (SDL_Color){ 95, 105, 115, 255 }));
            render_panel(r, box, fill, edge);
            render_text(r, g_res.font, konami_key_label(i),
                        box.x + box.w / 2, box.y + 7,
                        lit && !wrong ? YELLOW : WHITE, true);
        }
        if (wrong)
            render_text(r, g_res.font, "WRONG - START OVER", JB_SCREEN_W / 2,
                        320, (SDL_Color){ 235, 95, 95, 255 }, true);
        return;   /* no ordinary modal buttons */
    }

    if (s_modal == MODAL_ERASE_SAVE) {
        char heading[80];
        snprintf(heading, sizeof(heading), "Erase SLOT %d?", s_eraseSlot + 1);
        if (s_eraseStep == 1) {
            render_text(r, g_res.font, heading, JB_SCREEN_W / 2,
                        100, YELLOW, true);
            render_text(r, g_res.font, "Every beaten level and best score in this file",
                        JB_SCREEN_W / 2, 140, LIGHT, true);
            render_text(r, g_res.font, "will be removed. Other files are untouched.",
                        JB_SCREEN_W / 2, 170, LIGHT, true);
        } else {
            render_text(r, g_res.font, heading, JB_SCREEN_W / 2,
                        100, YELLOW, true);
            render_text(r, g_res.font, "This cannot be undone.",
                        JB_SCREEN_W / 2, 145, LIGHT, true);
            render_text(r, g_res.font, "Cancel to keep the file, ERASE SLOT to wipe it.",
                        JB_SCREEN_W / 2, 180, MUTED, true);
        }
    } else if (s_modal == MODAL_COPY_SLOT) {
        char heading[96];
        if (s_copyStep == 1) {
            snprintf(heading, sizeof(heading), "Copy SLOT %d", s_copySource + 1);
            render_text(r, g_res.font, heading, JB_SCREEN_W / 2, 95, YELLOW, true);
            render_text(r, g_res.font, "Choose a destination file:", JB_SCREEN_W / 2,
                        145, LIGHT, true);
            render_text(r, g_res.font, "The destination will be overwritten.",
                        JB_SCREEN_W / 2, 180, MUTED, true);
            render_text(r, g_res.font, "Left/Right and A also work.",
                        JB_SCREEN_W / 2, 215, MUTED, true);
        } else {
            snprintf(heading, sizeof(heading), "Copy SLOT %d to SLOT %d?",
                     s_copySource + 1, s_copyDest + 1);
            render_text(r, g_res.font, heading, JB_SCREEN_W / 2, 115, YELLOW, true);
            render_text(r, g_res.font, "Everything in the destination file is replaced.",
                        JB_SCREEN_W / 2, 165, LIGHT, true);
            render_text(r, g_res.font, "Source file is left unchanged.",
                        JB_SCREEN_W / 2, 200, MUTED, true);
        }
    } else if (s_modal == MODAL_LEVEL_EXIT) {
        render_text(r, g_res.font, "Leave this level?", JB_SCREEN_W / 2,
                    100, YELLOW, true);
        render_text(r, g_res.font, "Progress in this level is not saved and will be lost.",
                    JB_SCREEN_W / 2, 140, LIGHT, true);
        render_text(r, g_res.font, "Stay to keep playing, Leave to go back to level select.",
                    JB_SCREEN_W / 2, 170, MUTED, true);
    } else if (s_modal == MODAL_LEVEL_RESTART) {
        render_text(r, g_res.font, "Restart this level?", JB_SCREEN_W / 2,
                    100, YELLOW, true);
        render_text(r, g_res.font, "Your progress in this level will be lost.",
                    JB_SCREEN_W / 2, 140, LIGHT, true);
        render_text(r, g_res.font, "Cancel to keep playing, Restart to begin again.",
                    JB_SCREEN_W / 2, 170, MUTED, true);
    } else if (s_modal == MODAL_HINT) {
        render_text(r, g_res.font, s_modalHeading, JB_SCREEN_W / 2, 70, YELLOW, true);
        for (int i = 0; i < s_modalBodyCount; i++)
            render_text(r, g_res.font, s_modalBody[i], JB_SCREEN_W / 2,
                        110 + i * 24, LIGHT, true);
        render_text(r, g_res.font, "A or OK restarts the level. B will not dismiss this hint.",
                    JB_SCREEN_W / 2, 360, MUTED, true);
    } else if (s_modal == MODAL_KONAMI) {
        render_text(r, g_res.font, "ALL BUILDINGS UNLOCKED!", JB_SCREEN_W / 2,
                    100, YELLOW, true);
        render_text(r, g_res.font, "Konami code accepted.", JB_SCREEN_W / 2,
                    150, LIGHT, true);
        render_text(r, g_res.font, "Press A or tap OK to continue.", JB_SCREEN_W / 2,
                    185, MUTED, true);
    } else {
        credits_clamp_scroll();
        int pages = (CREDITS_LINES + CREDITS_VISIBLE - 1) / CREDITS_VISIBLE;
        int page = credits_page();
        char heading[80];
        snprintf(heading, sizeof(heading), "Credits   %d/%d", page, pages);
        render_text(r, g_res.font, heading, JB_SCREEN_W / 2, 70, YELLOW, true);
        int last = min_i(s_modalScroll + CREDITS_VISIBLE, CREDITS_LINES);
        for (int i = s_modalScroll; i < last; i++)
            render_text(r, g_res.font, s_credits[i], JB_SCREEN_W / 2,
                        105 + (i - s_modalScroll) * 22, LIGHT, true);

        s_modalPrevRect = credits_prev_rect();
        s_modalNextRect = credits_next_rect();
        draw_arrow_button(r, s_modalPrevRect, "<", credits_at_first_page());
        draw_arrow_button(r, s_modalNextRect, ">", credits_at_last_page());
        render_text(r, g_res.font, "Up/Down to scroll", JB_SCREEN_W / 2,
                    358, MUTED, true);
    }

    for (int i = 0; i < modal_button_count(); i++) {
        SDL_Rect b = modal_button_rect(i);
        if (i == s_modalChoice)
            render_panel(r, b, (SDL_Color){ 55, 75, 105, 255 }, YELLOW);
        else
            render_panel(r, b, (SDL_Color){ 40, 50, 65, 255 },
                         (SDL_Color){ 120, 130, 140, 255 });
        SDL_Rect labelBox = b;
        render_text(r, g_res.font, modal_button_label(i),
                    labelBox.x + labelBox.w / 2, labelBox.y + 9, WHITE, true);
    }
}

/* The homebrew quit prompt is independent of s_modal: it has its own dimmer,
   buttons and input pass so no hint, warning or credits window can trap the
   player. It is drawn after every other overlay. */
static SDL_Rect quit_button_rect(int i) {
    const char *label = i == 0 ? "No" : "Yes";
    int w = render_text_width(g_res.font, label) + 48;
    if (w < 100) w = 100;
    int cx = i == 0 ? 350 : 610;
    return (SDL_Rect){ cx - w / 2, MODAL_BTN_Y, w, MODAL_BTN_H };
}

static void draw_quit_prompt(SDL_Renderer *r) {
    if (!s_quitPrompt && !popup_drawable(&s_quitAnim)) return;
    Uint64 nowUs = popup_now_us();
    float scale = popup_scale(&s_quitAnim, nowUs);
    if (scale <= 0.0f) return;
    SDL_Rect dim = { 0, 0, JB_SCREEN_W, JB_SCREEN_H };
    SDL_SetRenderDrawColor(r, 0, 0, 0, (Uint8)(210 * scale));
    SDL_RenderFillRect(r, &dim);
    SDL_Rect full = { MODAL_X, MODAL_Y, MODAL_W, MODAL_H };
    SDL_Rect panel = popup_scaled_rect(full, scale);
    render_popup_frame(r, panel, (SDL_Color){ 25, 30, 40, 255 }, FRAME_RED);
    /* Keep hitboxes current even while the frame is moving; input is gated
       separately, so the first fully open frame is immediately live. */
    s_quitNoRect = quit_button_rect(0);
    s_quitYesRect = quit_button_rect(1);
    if (!popup_content_ready(&s_quitAnim)) return;
    render_text(r, g_res.font, "Quit to homebrew menu?", JB_SCREEN_W / 2,
                100, YELLOW, true);
    render_text(r, g_res.font, "Progress is only saved when you clear a level.",
                JB_SCREEN_W / 2, 145, LIGHT, true);

    for (int i = 0; i < 2; i++) {
        SDL_Rect b = quit_button_rect(i);
        if (i == s_quitChoice)
            render_panel(r, b, (SDL_Color){ 55, 75, 105, 255 }, YELLOW);
        else
            render_panel(r, b, (SDL_Color){ 40, 50, 65, 255 },
                         (SDL_Color){ 120, 130, 140, 255 });
        render_text(r, g_res.font, i == 0 ? "No" : "Yes",
                    b.x + b.w / 2, b.y + 9, WHITE, true);
    }
}

static void draw_clear_data_button(SDL_Renderer *r) {
    SDL_Rect rect = clear_data_rect();
    bool hover = point_in(s_inputSX, s_inputSY, rect);
    render_panel(r, rect,
                 hover ? (SDL_Color){ 92, 38, 42, 255 }
                       : (SDL_Color){ 48, 30, 34, 255 },
                 (SDL_Color){ 180, 80, 80, 255 });
    render_text(r, g_res.font, "CLEAR ALL DATA",
                rect.x + rect.w / 2, rect.y + 6,
                hover ? WHITE : (SDL_Color){ 235, 140, 140, 255 }, true);
}

static void show_title_screen(SDL_Renderer *renderer) {
    render_title_art(renderer);
    if (s_titleWorld) {
        SDL_SetRenderTarget(renderer, s_titleWorld);
        /* Clip in the 414x184 target's own coordinates so no level art can
           spill past the transparent window in the title bitmap. */
        SDL_Rect titleClip = { 0, 0, TITLE_WIN_W, TITLE_WIN_H };
        SDL_RenderSetClipRect(renderer, &titleClip);
        render_set_world_origin(0, -TITLE_WORLD_TOP);
        render_world(renderer);
        SDL_RenderSetClipRect(renderer, NULL);
        SDL_SetRenderTarget(renderer, s_frame);
        SDL_Rect win = { TITLE_WIN_X, TITLE_WIN_Y, TITLE_WIN_W, TITLE_WIN_H };
        SDL_RenderCopy(renderer, s_titleWorld, NULL, &win);
    }
    SDL_Rect creditsBox = { CREDITS_BOX_X, CREDITS_BOX_Y,
                            CREDITS_BOX_W, CREDITS_BOX_H };
    render_credits_button(renderer, point_in(s_inputSX, s_inputSY, creditsBox));
    if (s_introSeqCount > 0) {
        SDL_Rect replayBox = { REPLAY_BOX_X, REPLAY_BOX_Y,
                               REPLAY_BOX_W, REPLAY_BOX_H };
        render_intro_button(renderer, true,
                            point_in(s_inputSX, s_inputSY, replayBox), replayBox);
    }
    draw_clear_data_button(renderer);
    /* The original keeps the load indicator on the right of the title screen. */
    render_progress_overlay(renderer, 0xFFFFFFFFu, true, false);
    render_text(renderer, g_res.font, "JUNKBOT", JB_SCREEN_W / 2, 505, YELLOW, true);
    render_text(renderer, g_res.font, "A / Tap Here to Start",
                JB_SCREEN_W / 2, 524, WHITE, true);
}

static void draw_slot_button(SDL_Renderer *r, SDL_Rect rect, const char *label,
                             bool focused, bool disabled) {
    /* Even a locked entry must acknowledge the pointer, otherwise the cursor
       appears to disappear while it is over the button. */
    bool hover = point_in(s_inputSX, s_inputSY, rect);
    bool lit = focused || hover;
    if (disabled) {
        render_panel(r, rect,
                     lit ? (SDL_Color){ 48, 52, 60, 255 }
                         : (SDL_Color){ 26, 30, 36, 255 },
                     lit ? (SDL_Color){ 168, 128, 96, 255 }
                         : (SDL_Color){ 70, 76, 84, 255 });
        render_text(r, g_res.font, label, rect.x + rect.w / 2, rect.y + 10,
                    lit ? LIGHT : DIM, true);
    } else {
        render_panel(r, rect,
                     lit ? (SDL_Color){ 55, 78, 108, 255 }
                         : (SDL_Color){ 42, 58, 78, 255 },
                     lit ? YELLOW : (SDL_Color){ 120, 130, 140, 255 });
        render_text(r, g_res.font, label, rect.x + rect.w / 2, rect.y + 10,
                    lit ? WHITE : YELLOW, true);
    }
}

static void draw_slot_select(SDL_Renderer *r) {
    bool unlocked = jukebox_unlocked();
    render_factory_backdrop_cached(r, (Uint64)SDL_GetTicks() * 1000ULL);
    render_text(r, g_res.font, "SELECT SAVE FILE", JB_SCREEN_W / 2, 16,
                YELLOW, true);
    render_text(r, g_res.font,
                "A opens a file   COPY/JUKEBOX/ERASE buttons or L/Y   B title",
                JB_SCREEN_W / 2, 42, MUTED, true);
    if (s_fileAction == FILE_ACTION_COPY_SOURCE)
        render_text(r, g_res.font, "COPY: choose the SOURCE slot   (B cancels)",
                    JB_SCREEN_W / 2, 68, YELLOW, true);
    else if (s_fileAction == FILE_ACTION_COPY_DEST)
        render_text(r, g_res.font, "COPY: choose the DESTINATION slot   (B cancels)",
                    JB_SCREEN_W / 2, 68, YELLOW, true);
    else if (s_fileAction == FILE_ACTION_ERASE)
        render_text(r, g_res.font, "ERASE: choose the slot to erase   (B cancels)",
                    JB_SCREEN_W / 2, 68, YELLOW, true);

    for (int i = 0; i < save_slot_count(); i++) {
        SDL_Rect rect = slot_row_rect(i);
        bool selected = i == s_slotFocus;
        render_panel(r, rect,
                     selected ? (SDL_Color){ 48, 66, 92, 255 }
                              : (SDL_Color){ 28, 34, 44, 255 },
                     selected ? YELLOW : (SDL_Color){ 90, 100, 112, 255 });
        char heading[64];
        snprintf(heading, sizeof(heading), "SLOT %d%s%s", i + 1,
                 i == save_active_slot() ? "  (ACTIVE)" : "",
                 save_slot_unlock_all(i) ? "  [KONAMI]" : "");
        render_text(r, g_res.font, heading, rect.x + 18, rect.y + 12,
                    selected ? YELLOW : LIGHT, false);
        char summary[160];
        save_slot_summary(i, summary, sizeof(summary));
        render_text(r, g_res.font, summary, rect.x + 18, rect.y + 42,
                    save_slot_used(i) ? LIGHT : MUTED, false);
        char building[64];
        snprintf(building, sizeof(building), "LATEST: BUILDING %d",
                 save_slot_max_building(i));
        render_text_right(r, g_res.font, building, rect.x + rect.w - 18,
                          rect.y + 12, selected ? YELLOW : MUTED);
    }

    draw_slot_button(r, slot_copy_rect(), "COPY",
                     s_slotFocus == SLOT_FOCUS_COPY, false);
    draw_slot_button(r, slot_jukebox_rect(), "JUKEBOX",
                     s_slotFocus == SLOT_FOCUS_JUKEBOX, !unlocked);
    draw_slot_button(r, slot_erase_rect(), "ERASE",
                     s_slotFocus == SLOT_FOCUS_ERASE, false);

    if (!unlocked && (s_jukeboxHint > 0 || s_slotFocus == SLOT_FOCUS_JUKEBOX)) {
        /* A rejected press briefly flashes the requirement in red and gives it
           a small, decaying shake, then it settles back to the normal muted
           explanation in its original spot. */
        SDL_Color color = MUTED;
        int shakeX = 0;
        if (s_jukeboxHint > 0) {
            int age = 180 - s_jukeboxHint;   /* frames since the rejection */
            if (age < 90 && (age / 12) % 2 == 0)
                color = RED;
            if (age < 45) {
                shakeX = ((age / 3) % 2 == 0) ? -3 : 3;
                shakeX = shakeX * (45 - age) / 45;
            }
        }
        render_text(r, g_res.font,
                    "Unlock JUKEBOX: 40 GOLD awards in one file, or Konami on any file",
                    JB_SCREEN_W / 2 + shakeX, 500, color, true);
    }
    render_text(r, g_res.font,
                "Up/down rows   left/right buttons   A open   L copy   Y erase   B title",
                JB_SCREEN_W / 2, JB_SCREEN_H - 18, MUTED, true);
}

/* There are no official track titles, so the cast filename (minus .ogg) is
   the clearest label available. Keep the group prefix: at 2x the widest name
   is ~112px, so it still fits a 170px cell with the tempo on its own line. */
static void jukebox_track_label(int track, char out[24]) {
    const char *file = audio_music_track_name(track);
    size_t n = strlen(file);
    if (n > 4 && strcmp(file + n - 4, ".ogg") == 0) n -= 4;
    if (n > 23) n = 23;
    memcpy(out, file, n);
    out[n] = '\0';
}

static void draw_jukebox(SDL_Renderer *r) {
    jukebox_preview_sync();
    jukebox_preview_tick();
    render_text(r, g_res.font, "JUKEBOX", JB_SCREEN_W / 2, 14, YELLOW, true);
    render_text(r, g_res.font,
                "A plays a soundtrack   L loops a segment   R queues its end   B stop   X back",
                JB_SCREEN_W / 2, 38, MUTED, true);

    int count = audio_music_track_count();
    int current = audio_music_current_track();
    int loop = audio_jukebox_loop_track();

    /* Each horizontal row is a different soundtrack. Fill the whole 6px gap
       with a bright bevel so the boundary is unmistakable. */
    for (int g = 1; g < BGM_GROUP_COUNT; g++) {
        int y = JUKEBOX_LIST_Y + g * JUKEBOX_ROW_H - 6;
        SDL_Rect band = { 12, y, JB_SCREEN_W - 24, 4 };
        SDL_SetRenderDrawColor(r, 225, 235, 245, 255);
        SDL_RenderFillRect(r, &band);
        band.y += 4;
        band.h = 2;
        SDL_SetRenderDrawColor(r, 70, 80, 94, 255);
        SDL_RenderFillRect(r, &band);
    }
    int gridBottom = JUKEBOX_LIST_Y + BGM_GROUP_COUNT * JUKEBOX_ROW_H - 6;
    SDL_Rect bottomBand = { 12, gridBottom, JB_SCREEN_W - 24, 4 };
    SDL_SetRenderDrawColor(r, 225, 235, 245, 255);
    SDL_RenderFillRect(r, &bottomBand);
    bottomBand.y += 4;
    bottomBand.h = 2;
    SDL_SetRenderDrawColor(r, 70, 80, 94, 255);
    SDL_RenderFillRect(r, &bottomBand);

    for (int i = 0; i < count; i++) {
        SDL_Rect rect = jukebox_item_rect(i);
        bool selected = i == s_jukeboxTrack;
        bool playing = i == current;
        bool looping = i == loop;
        render_panel(r, rect,
                     selected ? (SDL_Color){ 50, 68, 94, 255 }
                              : (playing ? (SDL_Color){ 35, 66, 52, 255 }
                                         : (looping ? (SDL_Color){ 60, 55, 35, 255 }
                                                    : (SDL_Color){ 25, 30, 40, 255 })),
                     selected ? YELLOW
                              : (playing ? GREEN
                                         : (looping ? (SDL_Color){ 220, 180, 90, 255 }
                                                    : (SDL_Color){ 70, 78, 88, 255 })));
        char label[24];
        jukebox_track_label(i, label);
        SDL_Color textColor = selected ? WHITE
                                       : (playing ? GREEN : (looping ? YELLOW : LIGHT));
        render_text(r, g_res.font, label, rect.x + 8, rect.y + 5, textColor, false);
        char tempo[20];
        double bpm = audio_music_track_bpm(i);
        if (bpm > 0.0) snprintf(tempo, sizeof(tempo), "%.5g BPM", bpm);
        else snprintf(tempo, sizeof(tempo), "END");
        render_text(r, g_res.font, tempo, rect.x + 10, rect.y + 26,
                    bpm > 0.0 ? MUTED : (SDL_Color){ 220, 150, 150, 255 }, false);
        if (selected)
            render_text(r, g_res.font, "<", rect.x + rect.w - 24, rect.y + 5,
                        YELLOW, false);
        if (playing)
            render_text(r, g_res.font, ">", rect.x + rect.w - 24, rect.y + 26,
                        GREEN, false);
    }

    /* Sequence preview: clipped so a sliding cell never bleeds into the
       timer. Every real playback change shifts the strip one cell left. */
    SDL_Rect previewClip = {
        JUKEBOX_PREVIEW_X, JUKEBOX_PREVIEW_Y,
        JUKEBOX_PREVIEW_COUNT * JUKEBOX_PREVIEW_CELL, 34
    };
    SDL_RenderSetClipRect(r, &previewClip);
    int shift = (int)(s_previewScroll * JUKEBOX_PREVIEW_CELL);
    for (int i = 0; i < JUKEBOX_PREVIEW_COUNT; i++) {
        SDL_Rect cell = {
            JUKEBOX_PREVIEW_X + i * JUKEBOX_PREVIEW_CELL + shift,
            JUKEBOX_PREVIEW_Y, JUKEBOX_PREVIEW_CELL - 6, 30
        };
        int t = s_previewTracks[i];
        bool isNow = t >= 0 && t == current;
        render_panel(r, cell,
                     isNow ? (SDL_Color){ 35, 66, 52, 255 }
                           : (SDL_Color){ 22, 27, 36, 255 },
                     isNow ? GREEN : (SDL_Color){ 70, 78, 88, 255 });
        if (t >= 0) {
            char label[24];
            jukebox_track_label(t, label);
            render_text(r, g_res.font, label, cell.x + 4, cell.y + 7,
                        isNow ? GREEN : LIGHT, false);
        }
    }
    SDL_RenderSetClipRect(r, NULL);

    /* Playback timer: one exact integer step larger than the rest of the UI. */
    unsigned posMs = audio_music_position_ms();
    unsigned totalMs = audio_music_track_ms(current);
    unsigned posSec = posMs / 1000;
    unsigned totalSec = totalMs / 1000;
    char timer[40];
    snprintf(timer, sizeof(timer), "%u:%02u / %u:%02u",
             posSec / 60, posSec % 60, totalSec / 60, totalSec % 60);
    render_text_scaled(r, g_res.font, timer, JUKEBOX_TIMER_X,
                       JUKEBOX_PREVIEW_Y + 2, YELLOW, true, 3);
    SDL_Rect bar = { JUKEBOX_TIMER_X - 120, JUKEBOX_PREVIEW_Y + 30, 240, 6 };
    render_panel(r, bar, (SDL_Color){ 30, 35, 45, 255 },
                 (SDL_Color){ 90, 98, 108, 255 });
    if (totalMs > 0) {
        unsigned capped = posMs <= totalMs ? posMs : totalMs;
        int fill = (int)(240u * capped / totalMs);
        if (fill > 0) {
            SDL_Rect prog = { bar.x + 1, bar.y + 1, fill, bar.h - 2 };
            SDL_SetRenderDrawColor(r, 150, 205, 150, 255);
            SDL_RenderFillRect(r, &prog);
        }
    }

    char now[64], looping[64];
    int nowTrack = audio_music_current_track();
    snprintf(now, sizeof(now), "%s",
             nowTrack >= 0 ? audio_music_track_name(nowTrack) : "-");
    snprintf(looping, sizeof(looping), "%s",
             loop >= 0 ? audio_music_track_name(loop) : "OFF");
    char status[256];
    snprintf(status, sizeof(status), "NOW: %s   LOOP: %s   END: %s",
             now, looping, audio_jukebox_end_queued() ? "QUEUED" : "OFF");
    render_text(r, g_res.font, status, JB_SCREEN_W / 2, 462, YELLOW, true);
    render_text(r, g_res.font,
                "A starts at segment 1, then follows the game's playlist",
                JB_SCREEN_W / 2, 488, MUTED, true);
    render_text(r, g_res.font,
                "Up/down: soundtrack groups   left/right: segments in a group",
                JB_SCREEN_W / 2, 510, MUTED, true);
}

/* Always the last overlay: a black box keeps MUTED legible over HUD text,
   modals, credits and the level-select list alike. */
static void draw_muted_overlay(SDL_Renderer *r) {
    if (!audio_is_muted()) return;
    const char *label = "MUTED";
    int tw = render_text_width(g_res.font, label);
    SDL_Rect box = { JB_SCREEN_W - tw - 28, JB_SCREEN_H - 54, tw + 18, 26 };
    render_panel(r, box, (SDL_Color){ 0, 0, 0, 245 },
                 (SDL_Color){ 150, 60, 60, 255 });
    render_text(r, g_res.font, label, box.x + box.w / 2, box.y + 6,
                YELLOW, true);
}

static void draw_magnifier(SDL_Renderer *r) {
    if (!s_world || s_inputMode != INPUT_TOUCH || !game_is_dragging()) return;
    const int sw = 110, sh = 70, scale = 2;
    SDL_Rect src = { s_inputSX - sw / 2, s_inputSY - sh / 2, sw, sh };
    if (src.x < 0) src.x = 0;
    if (src.y < 0) src.y = 0;
    if (src.x + src.w > JB_SCREEN_W) src.x = JB_SCREEN_W - src.w;
    if (src.y + src.h > JB_SCREEN_H) src.y = JB_SCREEN_H - src.h;

    int dw = sw * scale, dh = sh * scale;
    /* Put the loupe on the opposite side of the screen from the finger, with
       a real gap, so it never covers the bricks being dragged. Centering it
       above the finger made the whole work area disappear behind the zoom. */
    const int gapX = 76, gapY = 72;
    int dx = s_inputSX < JB_SCREEN_W / 2 ? s_inputSX + gapX
                                         : s_inputSX - dw - gapX;
    int dy = s_inputSY < JB_SCREEN_H / 2 ? s_inputSY + gapY
                                         : s_inputSY - dh - gapY;
    if (dx < 4) dx = 4;
    if (dx + dw > JB_SCREEN_W - 4) dx = JB_SCREEN_W - dw - 4;
    if (dy < 44) dy = 44;
    if (dy + dh > JB_SCREEN_H - 4) dy = JB_SCREEN_H - dh - 4;

    SDL_Rect dst = { dx, dy, dw, dh };
    SDL_RenderCopy(r, s_world, &src, &dst);
    SDL_SetRenderDrawColor(r, 255, 235, 90, 255);
    SDL_RenderDrawRect(r, &dst);
    /* Marker for the finger's exact point inside the magnified view. */
    int mx = dx + (s_inputSX - src.x) * scale;
    int my = dy + (s_inputSY - src.y) * scale;
    SDL_Rect mark = { mx - 5, my - 5, 10, 10 };
    SDL_RenderDrawRect(r, &mark);
}

static void updateOutput(SDL_Renderer *renderer, SDL_Window *window) {
    if (s_resEventOpen && R_SUCCEEDED(eventWait(&s_resEvent, 0))) {
        s32 nw = 0, nh = 0;
        if (R_SUCCEEDED(appletGetDefaultDisplayResolution(&nw, &nh)) && nw > 0 && nh > 0) {
            LOG("display changed to %dx%d", nw, nh);
            SDL_SetWindowSize(window, nw, nh);
            s_touchW = nw;
            s_touchH = nh;
            s_resetTiming = true;  /* a swapchain resize must not cause catch-up */
        }
    }
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    if (w > 0 && h > 0 && (w != s_outW || h != s_outH)) {
        LOG("renderer output %dx%d (scale %.3fx x %.3fy)", w, h,
            (float)w / JB_SCREEN_W, (float)h / JB_SCREEN_H);
        s_outW = w;
        s_outH = h;
    }
}

/* Match the original SndMusicEnd(): whatever is queued jumps to its end
   stinger after the current measure, from any screen or jukebox action. */
static void music_wrap_up(void) {
    audio_music_wrap_up();
}

/* Screen transitions return the Screen enum value so callers cannot forget to
   switch state after a helper has reset the world/music for that screen. */
static Screen return_to_title(void) {
    game_cancel_drag();
    s_bNeedsRelease = true;
    s_unlockedBuilding = 0;
    s_levelIndex = -1;
    if (!game_load_level("Title Screen")) LOG("failed to reload Title Screen level");
    game_set_ambient(true);
    s_resultMusicPlayed = false;
    /* Coming back from the file screen should restore title music even if a
       title end stinger already played. */
    audio_music_start(MUSIC_TITLE);
    s_screenCooldown = 6;
    nav_timers_reset();
    LOG("screen -> TITLE");
    return SCREEN_TITLE;
}

static Screen prepare_select_screen(void) {
    game_cancel_drag();
    s_bNeedsRelease = true;
    s_unlockedBuilding = 0;
    s_levelIndex = -1;
    game_set_ambient(false);
    s_resultMusicPlayed = false;
    music_wrap_up();
    select_mark_dirty();
    s_screenCooldown = 6;
    nav_timers_reset();
    LOG("screen -> SELECT (slot %d)", save_active_slot() + 1);
    return SCREEN_SELECT;
}

static Screen return_to_slots(void) {
    game_cancel_drag();
    s_bNeedsRelease = true;
    s_unlockedBuilding = 0;
    s_levelIndex = -1;
    game_set_ambient(false);
    s_resultMusicPlayed = false;
    s_slotFocus = s_selectedSlot;
    s_fileAction = FILE_ACTION_NONE;
    /* Entering the file/level-select chain lets the current loop finish and
       hand off to its end stinger instead of cutting it off. Coming from a
       level, its own end stinger is already queued by then. */
    music_wrap_up();
    select_mark_dirty();
    s_screenCooldown = 6;
    nav_timers_reset();
    LOG("screen -> SLOTS");
    return SCREEN_SLOTS;
}

static Screen start_level(int index) {
    if (index < 0 || index >= g_levelCount || index > save_max_playable_index()) {
        audio_play(SFX_BLOCK_CLICK);
        LOG("level %d is locked", index);
        return SCREEN_SELECT;
    }
    game_cancel_drag();
    s_unlockedBuilding = 0;
    if (!game_load_level(g_levelNames[index])) {
        LOG("game_load_level FAILED: %s", g_levelNames[index]);
        return SCREEN_SELECT;
    }
    game_set_ambient(false);
    s_levelIndex = index;
    s_resultMusicPlayed = false;
    audio_play(SFX_BUTTON_CLICK);
    audio_music_start(-1);
    LOG("screen -> LEVEL: %s (entities=%d, music playlist %d)",
        g_levelNames[index], g_level.entityCount, audio_current_playlist());
    s_screenCooldown = 6;
    nav_timers_reset();
    return SCREEN_LEVEL;
}

static void quit_game(void) {
    game_cancel_drag();
    game_set_ambient(false);
    /* Deliberately no save here: progress is written only on level clear. */
    audio_music_stop();
    LOG("quit: game state normalised");
}

static void open_hint_modal(void) {
    if (s_levelIndex < 0) return;
    char id[16];
    level_id(s_levelIndex, id, sizeof(id));
    snprintf(s_modalHeading, sizeof(s_modalHeading), "Level %s hint:", id);
    if (*g_level.hint)
        s_modalBodyCount = wrap_text(g_level.hint, s_modalBody, 8, 500);
    else {
        snprintf(s_modalBody[0], sizeof(s_modalBody[0]), "No hint for this level.");
        s_modalBodyCount = 1;
    }
    s_modalChoice = 0;
    s_modal = MODAL_HINT;
    nav_timers_reset();
    audio_play(SFX_BUTTON_CLICK);
    LOG("hint opened for level %s", id);
}

static void open_credits_modal(void) {
    s_modalScroll = 0;
    s_modalChoice = 0;
    s_modal = MODAL_CREDITS;
    nav_timers_reset();
    audio_play(SFX_BUTTON_CLICK);
    LOG("credits opened");
}

static void open_konami_modal(void) {
    s_modalChoice = 0;
    s_modal = MODAL_KONAMI;
    nav_timers_reset();
    LOG("Konami confirmation opened");
}

static void open_erase_slot_modal(int slot) {
    if (slot < 0 || slot >= save_slot_count()) return;
    s_eraseSlot = slot;
    s_eraseStep = 1;
    s_modalChoice = 0;
    s_modal = MODAL_ERASE_SAVE;
    nav_timers_reset();
    audio_play(SFX_UHOH);
    LOG("erase slot %d warning 1/2 opened", slot + 1);
}

/* File Select COPY/ERASE are row-driven: selecting the command moves focus to
   the files, and each A press picks one file. No double-confirm on a row. */
static void start_copy_action(void) {
    s_fileActionOrigin = s_slotFocus;
    s_fileAction = FILE_ACTION_COPY_SOURCE;
    s_copySource = s_slotFocus < save_slot_count() ? s_slotFocus : s_selectedSlot;
    if (s_copySource < 0 || s_copySource >= save_slot_count()) s_copySource = 0;
    s_slotFocus = s_copySource;
    s_selectedSlot = s_copySource;
    nav_timers_reset();
    audio_play(SFX_BUTTON_CLICK);
    LOG("copy: choose source slot (B cancels)");
}

static void start_erase_action(void) {
    s_fileActionOrigin = s_slotFocus;
    s_fileAction = FILE_ACTION_ERASE;
    s_slotFocus = (s_selectedSlot >= 0 && s_selectedSlot < save_slot_count())
                      ? s_selectedSlot : 0;
    nav_timers_reset();
    audio_play(SFX_BUTTON_CLICK);
    LOG("erase: choose a slot (B cancels)");
}

static void cancel_file_action(void) {
    s_fileAction = FILE_ACTION_NONE;
    s_slotFocus = s_fileActionOrigin;
    if (s_slotFocus < 0 || s_slotFocus >= SLOT_FOCUS_COUNT) s_slotFocus = 0;
    s_screenCooldown = 6;
    nav_timers_reset();
    audio_play(SFX_BUTTON_CLICK);
    LOG("file action cancelled");
}

static void confirm_copy_destination(int dest) {
    s_fileAction = FILE_ACTION_NONE;
    s_copyDest = dest;
    s_copyStep = 2;                 /* source/dest already picked from rows */
    s_modalChoice = 0;
    s_modal = MODAL_COPY_SLOT;
    nav_timers_reset();
    audio_play(SFX_BUTTON_CLICK);
    LOG("copy SLOT %d -> SLOT %d confirmation", s_copySource + 1, dest + 1);
}

static void open_clear_data_modal(void) {
    s_konamiPos = 0;
    s_konamiPending = false;
    s_clearKeyPos = 0;
    s_clearWrongTimer = 0;
    s_clearDoneTimer = 0;
    s_clearComplete = false;
    s_modalChoice = 0;
    s_modal = MODAL_CLEAR_DATA;
    nav_timers_reset();
    audio_play(SFX_UHOH);
    LOG("CLEAR ALL DATA gate opened (Konami confirmation required)");
}

static void cancel_clear_data_modal(void) {
    s_clearKeyPos = 0;
    s_clearWrongTimer = 0;
    s_clearDoneTimer = 0;
    s_clearComplete = false;
    s_modal = MODAL_NONE;
    nav_timers_reset();
    audio_play(SFX_BUTTON_CLICK);
    LOG("CLEAR ALL DATA cancelled");
}

/* Consume one fresh menu-button edge. Correct keys advance the lit sequence;
   any other press (X is handled by the caller as Cancel) wipes the sequence. */
static void feed_clear_data(u64 pressed) {
    if (s_clearComplete) return;
    u64 key = pressed & ~((u64)HidNpadButton_X);
    if (!key) return;
    if (key == s_konami[s_clearKeyPos]) {
        int step = s_clearKeyPos++;
        menu_cursor_moved();
        LOG("clear-data key %d/%d: %s", s_clearKeyPos, KONAMI_LEN,
            konami_key_label(step));
        if (s_clearKeyPos == KONAMI_LEN) {
            save_erase_all();
            select_mark_dirty();
            s_selected = 0;
            s_selectedSlot = 0;
            s_slotFocus = 0;
            s_clearComplete = true;
            s_clearDoneTimer = 45;
            s_screenCooldown = 6;
            audio_play(SFX_GET_POWERUP);
            LOG("CLEAR ALL DATA confirmed: every save erased");
        }
    } else {
        s_clearKeyPos = 0;
        s_clearWrongTimer = 45;
        audio_play(SFX_BLOCK_CLICK);
        LOG("clear-data sequence wrong: restarting");
    }
}

static void open_warning_modal(Modal modal) {
    s_modalChoice = 0;   /* default to the safe option */
    s_modal = modal;
    nav_timers_reset();
    audio_play(SFX_UHOH);
    LOG("warning modal %d opened", modal);
}

/* The quit prompt is intentionally outside the normal modal stack. + opens it
   on every screen, including while another popup is open, except during the
   short window where assets are still being loaded. */
static void open_quit_prompt(void) {
    if (s_loadingAssets) {
        LOG("quit prompt ignored: loading assets");
        return;
    }
    if (s_quitPrompt) return;
    s_quitChoice = 0;   /* default to No */
    s_quitPrompt = true;
    game_cancel_drag();
    nav_timers_reset();
    audio_play(SFX_BUTTON_CLICK);
    LOG("quit prompt opened");
}

static void close_quit_prompt(bool *quitRequested) {
    if (!s_quitPrompt) return;
    s_quitPrompt = false;
    /* No click on the final Yes: the mixer unloads immediately and the tail of
       the sound is very audible. Cancelling still gets the normal menu cue. */
    if (s_quitChoice != 1) audio_play(SFX_BUTTON_CLICK);
    if (s_quitChoice == 1) {
        quit_game();
        if (quitRequested) *quitRequested = true;
        LOG("quit confirmed");
    } else {
        LOG("quit declined");
    }
}

static void confirm_modal(void) {
    if (s_modal == MODAL_LEVEL_EXIT) {
        s_modal = MODAL_NONE;
        audio_play(SFX_BUTTON_CLICK);
        if (s_modalChoice == 1) {
            s_leaveLevel = true;        /* main loop switches to level select */
        }
    } else if (s_modal == MODAL_LEVEL_RESTART) {
        s_modal = MODAL_NONE;
        audio_play(SFX_BUTTON_CLICK);
        if (s_modalChoice == 1) {
            s_restartLevel = true;      /* main loop restarts the level */
        }
    } else if (s_modal == MODAL_ERASE_SAVE) {
        audio_play(SFX_BUTTON_CLICK);
        if (s_modalChoice == 0) {
            s_modal = MODAL_NONE;
            LOG("erase slot %d cancelled", s_eraseSlot + 1);
        } else if (s_eraseStep == 1) {
            s_eraseStep = 2;
            s_modalChoice = 0;
            nav_timers_reset();
            audio_play(SFX_UHOH);       /* second, stronger warning */
            LOG("erase slot %d warning 2/2 opened", s_eraseSlot + 1);
        } else {
            s_modal = MODAL_NONE;
            save_erase_slot(s_eraseSlot);
            select_mark_dirty();
            s_screenCooldown = 6;
            LOG("slot %d erased", s_eraseSlot + 1);
        }
    } else if (s_modal == MODAL_COPY_SLOT) {
        audio_play(SFX_BUTTON_CLICK);
        if (s_copyStep == 1) {
            if (s_modalChoice == s_copySource) {
                audio_play(SFX_BLOCK_CLICK);
                LOG("copy destination matches source");
                return;
            }
            s_copyDest = s_modalChoice;
            s_copyStep = 2;
            s_modalChoice = 0;
            nav_timers_reset();
            LOG("copy confirmation SLOT %d -> SLOT %d opened",
                s_copySource + 1, s_copyDest + 1);
        } else {
            s_modal = MODAL_NONE;
            if (s_modalChoice == 1 && s_copyDest >= 0)
                save_copy_slot(s_copySource, s_copyDest);
            else
                LOG("slot copy cancelled");
            select_mark_dirty();
            s_screenCooldown = 6;
        }
    } else if (s_modal == MODAL_HINT) {
        s_modal = MODAL_NONE;
        audio_play(SFX_BUTTON_CLICK);
        game_reset_level();              /* original reloads the level on hint OK */
        audio_music_start(-1);           /* every reset picks a new random track */
        s_resultMusicPlayed = false;
        s_screenCooldown = 6;
        nav_timers_reset();
        LOG("hint dismissed: level restarted (moves=%d)", g_moves);
    } else if (s_modal == MODAL_CREDITS || s_modal == MODAL_KONAMI) {
        s_modal = MODAL_NONE;
        audio_play(SFX_BUTTON_CLICK);
        LOG("confirmation closed");
    }
}

/* HOME menu / sleep: freeze the simulation and audio and discard accumulated
   frame time when we come back. Loading is the one exception: the freeze is
   deferred until assets are ready, mirroring the known-good gravityrush
   Switch 2 host. */
static void suspend_game(void) {
    if (s_suspended) return;
    s_suspended = true;
    game_cancel_drag();
    if (s_audioReady) audio_set_paused(true);
    LOG("suspended: HOME menu or sleep");
}

static void resume_game(void) {
    if (!s_suspended) return;
    s_suspended = false;
    s_resetTiming = true;
    if (s_audioReady) audio_set_paused(false);
    LOG("resumed from HOME menu / sleep");
}

static void focus_hook(AppletHookType hook, void *param) {
    (void)param;
    if (hook != AppletHookType_OnFocusState && hook != AppletHookType_OnResume)
        return;
    AppletFocusState state = appletGetFocusState();
    if (s_loadingAssets) {
        if (state == AppletFocusState_Background) {
            if (!s_freezePending)
                LOG("HOME or sleep during asset loading: freeze deferred");
            s_freezePending = true;
        } else if (state == AppletFocusState_InFocus) {
            s_freezePending = false;
        }
        return;
    }
    if (state == AppletFocusState_Background) suspend_game();
    else if (state == AppletFocusState_InFocus) resume_game();
}

static void finish_asset_loading(void) {
    /* Dispatch focus messages while the loading flag is still true, then
       apply any deferred freeze before gameplay starts. */
    appletMainLoop();
    s_loadingAssets = false;
    if (s_freezePending || appletGetFocusState() == AppletFocusState_Background) {
        s_freezePending = false;
        suspend_game();
    }
}

static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) { remove(path); return; }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
        else remove(child);
    }
    closedir(dir);
    remove(path);
}

static void prep_progress(const char *stage, int done, int total, void *user) {
    (void)user;
    if (total > 0 && (done == 0 || done == total || done % 200 == 0))
        LOG("prep: %s (%d/%d)", stage, done, total);
}

static void show_missing_assets(SDL_Renderer *renderer) {
    PadState pad;
    padInitializeAny(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & (HidNpadButton_Plus | HidNpadButton_A)) break;
        SDL_SetRenderTarget(renderer, s_frame);
        SDL_SetRenderDrawColor(renderer, 18, 20, 28, 255);
        SDL_RenderClear(renderer);
        if (g_res.font) {
            render_text(renderer, g_res.font, "ASSET PACKAGE MISSING",
                        JB_SCREEN_W / 2, 170, WHITE, true);
            render_text(renderer, g_res.font,
                        "Put junkbot.otr beside junkbot.nro",
                        JB_SCREEN_W / 2, 230, YELLOW, true);
            render_text(renderer, g_res.font,
                        "Or put junkbot.dir there for one-time prep",
                        JB_SCREEN_W / 2, 262, WHITE, true);
            const char *detail = assets_error();
            if (detail && *detail)
                render_text(renderer, g_res.font, detail,
                            JB_SCREEN_W / 2, 294, YELLOW, true);
            render_text(renderer, g_res.font, "Press + to exit",
                        JB_SCREEN_W / 2, 350, WHITE, true);
        }
        SDL_SetRenderTarget(renderer, NULL);
        if (s_frame) SDL_RenderCopy(renderer, s_frame, NULL, NULL);
        SDL_RenderPresent(renderer);
    }
}

static bool run_onconsole_prep(const char *arg0) {
    if (assets_has_package()) return false;

    static const char *dirCandidates[] = {
        NULL, /* filled with assets_dir_path() below */
        JB_DIR_FILENAME,
        "sdmc:/switch/junkbot/" JB_DIR_FILENAME,
        NULL
    };
    dirCandidates[0] = assets_dir_path();
    const char *dirPath = NULL;
    for (int i = 0; dirCandidates[i]; i++) {
        FILE *dirFile = fopen(dirCandidates[i], "rb");
        if (dirFile) {
            fclose(dirFile);
            dirPath = dirCandidates[i];
            break;
        }
    }
    if (!dirPath) return false;

    const char *packagePath = assets_package_path();
    const char *stage = "sdmc:/switch/junkbot/otr-stage";
    remove_tree(stage);
    mkdir("sdmc:/switch/junkbot", 0777);
    mkdir(stage, 0777);

    char error[256] = {0};
    LOG("on-console prep: extracting %s", dirPath);
    if (!dirprep_extract(dirPath, stage, prep_progress, NULL,
                         error, sizeof(error))) {
        LOG("on-console prep FAILED: %s", error);
        return false;
    }
    if (!otr_pack_directory(stage, packagePath, "dir:on-console",
                            error, sizeof(error))) {
        LOG("on-console package FAILED: %s", error);
        return false;
    }
    remove_tree(stage);
    if (remove(dirPath) == 0)
        LOG("on-console prep complete; removed %s", dirPath);
    else
        LOG("on-console prep complete; could not remove %s", dirPath);
    assets_exit();
    assets_init(arg0);
    return assets_has_package();
}

int main(int argc, char **argv) {
    (void)argc;
    const char *arg0 = (argc > 0 && argv && argv[0]) ? argv[0] : NULL;

    log_init();
    LOG("entered main");

    s_loadingAssets = true;
    /* Like gravityrush_switch2, keep the applet thread running out of focus so
       the hook can freeze the game itself. Blocking HOME or holding a sleep
       lock can hang the console; deferring the freeze during loading cannot. */
    Result focusModeRc =
        appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    LOG("focus handling mode: 0x%x (%s)", R_VALUE(focusModeRc),
        R_SUCCEEDED(focusModeRc) ? "NoSuspend" : "default");

    assets_init(arg0);
    Result rc;

    appletHook(&s_focusCookie, focus_hook, NULL);
    LOG("focus hook registered (HOME / sleep)");

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        LOG("SDL video init FAILED: %s", SDL_GetError());
        return 1;
    }
    LOG("SDL video init ok");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        LOG("SDL audio init FAILED: %s (continuing silent)", SDL_GetError());
    else
        LOG("SDL audio init ok");

    if (R_SUCCEEDED(appletGetDefaultDisplayResolutionChangeEvent(&s_resEvent))) {
        eventClear(&s_resEvent);
        s_resEventOpen = true;
        LOG("resolution change event opened");
    }

    s32 dispW = 0, dispH = 0;
    rc = appletGetDefaultDisplayResolution(&dispW, &dispH);
    LOG("display resolution: 0x%x %dx%d", R_VALUE(rc), dispW, dispH);
    if (dispW <= 0 || dispH <= 0) { dispW = 1920; dispH = 1080; }
    s_outW = dispW;
    s_outH = dispH;

    SDL_Window *window = SDL_CreateWindow("Junkbot", SDL_WINDOWPOS_UNDEFINED,
                                          SDL_WINDOWPOS_UNDEFINED, dispW, dispH, 0);
    if (!window) {
        LOG("SDL_CreateWindow FAILED: %s", SDL_GetError());
        return 1;
    }
    LOG("window created %dx%d", dispW, dispH);

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        LOG("accelerated+vsync renderer FAILED: %s; trying software", SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        s_noVsync = true;
    }
    if (!renderer) {
        LOG("software renderer FAILED: %s", SDL_GetError());
        return 1;
    }
    if (SDL_RenderSetVSync(renderer, 1) < 0) {
        LOG("SDL_RenderSetVSync unsupported: %s", SDL_GetError());
        s_noVsync = true;
    }
    SDL_RendererInfo renderInfo;
    memset(&renderInfo, 0, sizeof(renderInfo));
    if (SDL_GetRendererInfo(renderer, &renderInfo) == 0) {
        bool vsync = (renderInfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
        if (!vsync) s_noVsync = true;
        LOG("renderer=%s flags=0x%x present_vsync=%d", renderInfo.name,
            renderInfo.flags, vsync ? 1 : 0);
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* Secondary setup path: if there is no user-generated package but the
       original movie is next to the NRO, build the package once on-console. */
    run_onconsole_prep(arg0);
    if (!assets_has_package())
        LOG("no asset package available; game assets will be missing");

    s_frame = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_TARGET, JB_SCREEN_W, JB_SCREEN_H);
    s_world = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_TARGET, JB_SCREEN_W, JB_SCREEN_H);
    if (!s_frame) {
        LOG("framebuffer texture FAILED (%s); using SDL logical-size fallback", SDL_GetError());
        SDL_RenderSetLogicalSize(renderer, JB_SCREEN_W, JB_SCREEN_H);
    }
    s_titleWorld = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                     SDL_TEXTUREACCESS_TARGET, TITLE_WIN_W, TITLE_WIN_H);
    if (!s_titleWorld) LOG("title world texture FAILED: %s (title will be static)", SDL_GetError());
    LOG("framebuffer %dx%d, title world %dx%d", JB_SCREEN_W, JB_SCREEN_H, TITLE_WIN_W, TITLE_WIN_H);

    if (!render_init(renderer))
        LOG("render_init FAILED (continuing with whatever loaded)");
    else
        LOG("render_init ok");

    if (!assets_has_package()) {
        LOG("blocking: no asset package found");
        show_missing_assets(renderer);
        goto cleanup;
    }

    /* Flipbook sequence: 480 little-endian uint16 indices into the intro atlas. */
    {
        size_t size = 0;
        void *seq = assets_read("intro/seq.bin", &size);
        if (seq) {
            if (size > 0 && size % 2 == 0) {
                s_introSeq = (Uint16 *)malloc(size);
                if (s_introSeq) {
                    memcpy(s_introSeq, seq, size);
                    s_introSeqCount = (int)(size / 2);
                }
            }
            free(seq);
        }
        LOG("intro flipbook: %d steps, %d atlas frames",
            s_introSeqCount, g_res.intro.frameCount);
        if (s_introSeqCount && !g_res.intro.frameCount) s_introSeqCount = 0;
    }

    if (!audio_init())
        LOG("audio_init FAILED (continuing silent)");
    else {
        s_audioReady = true;
        LOG("audio_init ok");
        audio_music_start(MUSIC_TITLE);
        LOG("title music started (playlist %d)", audio_current_playlist());
    }

    padConfigureInput(8, HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadHandheld);
    LOG("pad configured (all controller styles, one source consumed per frame)");

    /* The short boot animation accepts any controller. */
    PadState bootPad;
    padInitializeAny(&bootPad);

    for (int i = 0; i < 8; i++) padInitializeWithMask(&s_pads[i], BIT(i));
    padInitializeWithMask(&s_pads[PAD_SOURCE_HANDHELD],
                          1UL << HidNpadIdType_Handheld);
    for (int i = 0; i < PAD_SOURCES; i++) {
        rc = padPointerInitialize(&s_pointers[i], dispW, dispH);
        s_pointerReady[i] = R_SUCCEEDED(rc);
        if (i == 0)
            LOG("padPointerInitialize: 0x%x (%s)", R_VALUE(rc),
                R_SUCCEEDED(rc) ? "ok" : "FAILED");
    }
    if (R_SUCCEEDED(hidSetTouchScreenOutputRanges(dispW, dispH))) {
        s_touchW = dispW;
        s_touchH = dispH;
    }
    hidInitializeTouchScreen();
    LOG("touchscreen initialized (range %dx%d)", s_touchW, s_touchH);

    int levels = game_scan_levels();
    save_init();
    LOG("level scan: %d levels", levels);
    game_load_level("Title Screen");
    game_set_ambient(true);
    LOG("title level loaded (entities=%d)", g_level.entityCount);

    /* All romfs, textures, audio and level data are resident now. Any HOME or
       sleep press that arrived during this window freezes from here on. */
    finish_asset_loading();
    LOG("asset loading finished (suspended=%d)", (int)s_suspended);

    /* Boot indicator from the original web game: fourteen immobile bricks fall
       into place under a "Download in progress" plaque, then "Ready to play".
       Everything is already loaded locally, so this is a short cosmetic nod to
       the original startup; A / + skips it. */
    {
        const Uint32 brickSettle = 13 * 90 + 600;
        const Uint32 bootDone = brickSettle + 500;
        Uint64 bootFreq = SDL_GetPerformanceFrequency();
        Uint64 bootLast = SDL_GetPerformanceCounter();
        Uint64 bootElapsedUs = 0;
        LOG("boot progress animation started (%u ms)", bootDone);
        while (appletMainLoop()) {
            Uint64 now = SDL_GetPerformanceCounter();
            Uint64 frameUs = (now - bootLast) * 1000000ULL / bootFreq;
            bootLast = now;
            if (s_suspended) {
                /* HOME or sleep freezes the boot animation too. */
                SDL_Delay(16);
                continue;
            }
            bootElapsedUs += frameUs;
            audio_tick();
            Uint32 elapsed = (Uint32)(bootElapsedUs / 1000);
            bool ready = elapsed >= brickSettle;
            padUpdate(&bootPad);
            bool skip = (padGetButtonsDown(&bootPad) &
                         (HidNpadButton_A | HidNpadButton_Plus)) && elapsed > 200;
            if (elapsed >= bootDone || skip) break;

            SDL_SetRenderTarget(renderer, s_frame ? s_frame : NULL);
            render_progress_overlay(renderer, elapsed, ready, true);
            SDL_SetRenderTarget(renderer, NULL);
            if (s_frame) {
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, s_frame, NULL, NULL);
            }
            SDL_RenderPresent(renderer);
        }
        LOG("boot progress animation finished");
    }

    Screen screen = SCREEN_TITLE;
    if (s_introSeqCount > 0) {
        screen = SCREEN_INTRO;
        s_introStep = 0;
        s_introAccumUs = 0;
        LOG("intro cinematic playing at startup");
    }
    int titleClickCooldown = 0;
    Uint32 frames = 0;

    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 last = SDL_GetPerformanceCounter();
    Uint64 accumulator = 0;
    Uint64 nextPresent = 0;   /* only used when SDL cannot provide vsync */
    const Uint64 step = 1000000ULL / JB_SIM_HZ;
    LOG("entering main loop (1x %dx%d -> output %dx%d, %llu us/step)",
        JB_SCREEN_W, JB_SCREEN_H, s_outW, s_outH, (unsigned long long)step);

    while (appletMainLoop()) {
        audio_tick();
        frames++;
        Uint64 popupNowUs = popup_now_us();
        popup_sync(popupNowUs, screen);
        bool popupLocked = popup_input_locked();
        if (frames % 300 == 0)
            LOG("heartbeat frame=%u screen=%d mode=%d levels=%d playable=%d moves=%d result=%d modal=%d suspended=%d",
                frames, screen, s_inputMode, g_levelCount, save_max_playable_index(),
                g_moves, g_result, s_modal, s_suspended);

        updateOutput(renderer, window);

        if (s_resetTiming) {
            last = SDL_GetPerformanceCounter();
            accumulator = 0;
            s_resetTiming = false;
        }

        for (int i = 0; i < PAD_SOURCES; i++) {
            padUpdate(&s_pads[i]);
            if (s_pointerReady[i]) padPointerUpdate(&s_pointers[i], &s_pads[i]);
        }

        int newPad = choose_active_pad();
        bool padJustChanged = (newPad != s_activePad);
        if (padJustChanged) {
            if (game_is_dragging()) game_cancel_drag();
            s_clickOwner = CLICK_NONE;
            nav_timers_reset();
            if (newPad >= 0)
                LOG("active controller -> %s (styles 0x%x)", pad_source_name(newPad),
                    padGetStyleSet(&s_pads[newPad]));
            else
                LOG("active controller -> none");
            s_activePad = newPad;
        }

        PadState *pad = s_activePad >= 0 ? &s_pads[s_activePad] : NULL;
        PadPointer *pointer = s_activePad >= 0 ? &s_pointers[s_activePad] : NULL;
        bool pointerReady = s_activePad >= 0 && s_pointerReady[s_activePad];
        u64 kDown = pad ? padGetButtonsDown(pad) : 0;
        u64 kHeld = pad ? padGetButtons(pad) : 0;
        u64 kUp = pad ? padGetButtonsUp(pad) : 0;
        u32 pointerDown = pointerReady ? padPointerGetButtonsDown(pointer) : 0;
        u32 pointerUp = pointerReady ? padPointerGetButtonsUp(pointer) : 0;
        u32 pointerHeld = pointerReady ? padPointerGetButtons(pointer) : 0;
        int pointerX = pointerReady ? pointer->x : 0;
        int pointerY = pointerReady ? pointer->y : 0;
        if (kDown) LOG("buttons down: 0x%016llx", (unsigned long long)kDown);

        /* Menu buttons come from every pad, not just s_activePad. This keeps
           D-pad navigation alive while a right Joy-Con is in mouse mode, and
           keeps A/B/X/Y alive while a left Joy-Con is in mouse mode. */
        u64 menuDown = 0, menuHeld = 0, popupDown = 0;
        bool aFresh = false, bFresh = false;
        for (int i = 0; i < PAD_SOURCES; i++) {
            u64 cur = 0, down = 0;
            if (padIsConnected(&s_pads[i])) {
                u64 rawDown = padGetButtonsDown(&s_pads[i]);
                popupDown |= rawDown & POPUP_BUTTON_MASK;
                cur = padGetButtons(&s_pads[i]) & MENU_BUTTON_MASK;
                down = rawDown & MENU_BUTTON_MASK;
            }
            menuDown |= down;
            menuHeld |= cur;
            if ((down & HidNpadButton_A) && !(s_menuPrev[i] & HidNpadButton_A))
                aFresh = true;
            if ((down & HidNpadButton_B) && !(s_menuPrev[i] & HidNpadButton_B))
                bFresh = true;
            s_menuPrev[i] = cur;
        }
        if (!(menuHeld & HidNpadButton_B)) s_bNeedsRelease = false;

        /* While a panel is moving, its controls do not exist yet (or any more).
           Keep + alive so the top-priority quit prompt still opens immediately;
           the clear popup owns + while it is the active result panel. */
        if (popupLocked) {
            u64 plus = 0;
            if (!(g_result == 1 && popup_busy(&s_resultAnim)))
                plus = menuDown & HidNpadButton_Plus;
            menuDown = plus;
            menuHeld = 0;
            popupDown = 0;
            aFresh = false;
            bFresh = false;
        }

        if (titleClickCooldown > 0) titleClickCooldown--;
        if (s_jukeboxHint > 0) s_jukeboxHint--;
        if (s_screenCooldown > 0) s_screenCooldown--;

        /* The CLEAR ALL DATA gate uses the same key sequence for a destructive
           confirmation; do not let it also trigger the unlock-all shortcut. */
        bool konamiConsumed = feed_konami(s_modal == MODAL_CLEAR_DATA ? 0 : menuDown);
        if (s_konamiPending) {
            s_konamiPending = false;
            open_konami_modal();  /* final cheat press is never a menu confirm */
            konamiConsumed = true;
        }

        /* --- Touch input --- */
        bool touchDown = false, touchHeld = false, touchUp = false;
        int touchLX = 0, touchLY = 0;
        read_touch_input(&touchDown, &touchHeld, &touchUp, &touchLX, &touchLY);

        /* --- One latched click source across mouse, shoulders and touch --- */
        ClickOwner owner = s_clickOwner;
        if (owner == CLICK_NONE) owner = click_owner_from_down(pointerDown, kDown);
        bool clickDown = click_owner_down(owner, pointerDown, kDown);
        bool clickUp = click_owner_up(owner, pointerUp, kUp);
        bool clickHeld = click_owner_held(owner, pointerHeld, kHeld);
        if (clickDown && s_clickOwner == CLICK_NONE) s_clickOwner = owner;
        if (clickUp && s_clickOwner == owner) s_clickOwner = CLICK_NONE;
        if (touchHeld) s_clickOwner = CLICK_NONE;
        u32 activeMouseBit = click_pointer_mask(s_clickOwner);

        HidAnalogStickState stick = pad ? reticle_stick(pad) : (HidAnalogStickState){ 0, 0 };
        bool stickMoved = stick_active(stick, 1200);
        /* A controller switch should not look like a huge pointer jump. */
        if (padJustChanged) { s_lastPX = pointerX; s_lastPY = pointerY; }
        bool pointerMoved = (pointerX != s_lastPX || pointerY != s_lastPY);
        /* A shoulder press is a click, but it is not pointer motion. Counting
           it as motion made a held L/ZL/R/ZR snap the reticle back to a stale
           padPointer every frame, so the dragged piece vanished. Only real
           pointer motion (or a mouse-button click) switches away from the
           stick reticle; shoulder clicks leave the current mode untouched. */
        bool pointerClick = click_pointer_mask(owner) != 0 ||
                            click_pointer_mask(s_clickOwner) != 0;
        if (stickMoved) s_useStickCursor = true;
        else if (pointerMoved || pointerClick) s_useStickCursor = false;
        s_lastPX = pointerX;
        s_lastPY = pointerY;

        InputMode oldMode = s_inputMode;
        if (touchDown || touchHeld) s_inputMode = INPUT_TOUCH;
        if (!touchHeld && (stickMoved || pointerMoved || activeMouseBit ||
                           clickHeld || (kDown & click_button_mask())))
            s_inputMode = INPUT_MOUSE;
        if (oldMode != s_inputMode)
            LOG("input mode -> %s", s_inputMode == INPUT_TOUCH ? "TOUCH" : "POINTER");

        /* A/B use per-controller edges computed above, so a fresh press on the
           other Joy-Con still works in either mouse mode. s_bNeedsRelease only
           blocks a B that is still held after a screen transition. */
        aFresh = aFresh && s_screenCooldown == 0;
        bFresh = bFresh && !s_bNeedsRelease && s_screenCooldown == 0;

        u32 inputDown = 0, inputUp = 0;
        int logicalX, logicalY;

        if (s_inputMode == INPUT_TOUCH) {
            logicalX = touchLX;
            logicalY = touchLY;
            if (touchDown) inputDown |= HidMouseButton_Left;
            if (touchUp) inputUp |= HidMouseButton_Left;
        } else {
            if (s_useStickCursor) {
                /* Apply a deadzone to EACH axis, not just to the stick as a
                   whole. Holding left on the left stick can report a small
                   vertical offset; without this it accumulates and drags the
                   reticle downward once X has hit the level boundary. */
                const int axisDz = 2500;
                int sx = (stick.x > axisDz || stick.x < -axisDz) ?
                         (int)(stick.x / 3277) : 0;
                int sy = (stick.y > axisDz || stick.y < -axisDz) ?
                         (int)(stick.y / 3277) : 0;
                s_cursorWX += sx;
                /* Analog Y is inverted here; Joy-Con mouse-mode pointer Y is
                   read from padPointer below and deliberately left unchanged. */
                s_cursorWY -= sy;
                if (screen == SCREEN_TITLE) {
                    /* The title bitmap only shows a short slice near the bottom
                       of the level. Clamp the stick cursor to that window in
                       world units; using the full level bounds pins Y to the
                       top of the visible window. */
                    if (s_cursorWX < 0) s_cursorWX = 0;
                    if (s_cursorWY < TITLE_WORLD_TOP) s_cursorWY = TITLE_WORLD_TOP;
                    if (s_cursorWX >= TITLE_WIN_W) s_cursorWX = TITLE_WIN_W - 1;
                    if (s_cursorWY >= TITLE_WORLD_TOP + TITLE_WIN_H)
                        s_cursorWY = TITLE_WORLD_TOP + TITLE_WIN_H - 1;
                    s_reticleSX = TITLE_WIN_X + s_cursorWX;
                    s_reticleSY = TITLE_WIN_Y + (s_cursorWY - TITLE_WORLD_TOP);
                } else {
                    if (s_cursorWX < 0) s_cursorWX = 0;
                    if (s_cursorWY < 0) s_cursorWY = 0;
                    if (s_cursorWX >= g_level.boundsW) s_cursorWX = g_level.boundsW - 1;
                    if (s_cursorWY >= g_level.boundsH) s_cursorWY = g_level.boundsH - 1;
                    game_world_to_screen(s_cursorWX, s_cursorWY, &s_reticleSX, &s_reticleSY);
                }
                logicalX = s_reticleSX;
                logicalY = s_reticleSY;
            } else {
                logicalX = (int)((long long)pointerX * JB_SCREEN_W / (s_outW > 0 ? s_outW : 1920));
                logicalY = (int)((long long)pointerY * JB_SCREEN_H / (s_outH > 0 ? s_outH : 1080));
                s_reticleSX = logicalX;
                s_reticleSY = logicalY;
            }
            if (clickDown) inputDown |= HidMouseButton_Left;
            if (clickUp) inputUp |= HidMouseButton_Left;
        }

        logicalX = (logicalX < 0) ? 0 : (logicalX >= JB_SCREEN_W ? JB_SCREEN_W - 1 : logicalX);
        logicalY = (logicalY < 0) ? 0 : (logicalY >= JB_SCREEN_H ? JB_SCREEN_H - 1 : logicalY);
        s_inputSX = logicalX;
        s_inputSY = logicalY;
        if (popupLocked) {
            inputDown = 0;
            inputUp = 0;
        }

        /* X is the global mute everywhere except the jukebox, where it is the
           dedicated "back to File Select" key requested for that menu. */
        if ((menuDown & HidNpadButton_X) && screen != SCREEN_JUKEBOX &&
            s_modal != MODAL_CLEAR_DATA)
            audio_set_muted(!audio_is_muted());

        /* --- Quit prompt has absolute priority over every other popup --- */
        int stickUD = 0, stickLR = 0;
        if (pad) stick_menu_dirs(pad, &stickUD, &stickLR);
        int wheel = pointerReady ? pointer->wheel_delta_y : 0;
        if (popupLocked) {
            stickUD = 0;
            stickLR = 0;
            wheel = 0;
        }
        bool quitRequested = false;
        if (s_quitPrompt) {
            if (menuDown & HidNpadButton_Plus) {
                close_quit_prompt(&quitRequested);   /* + toggles it closed */
            } else {
                int choice = menu_move(menuDown, menuHeld, stickUD, &s_modalRepeat);
                if (!choice)
                    choice = menu_move_axis(menuDown, menuHeld, HidNpadButton_Left,
                                            HidNpadButton_Right, stickLR,
                                            &s_modalPageRepeat);
                if (!konamiConsumed && choice) {
                    s_quitChoice = (s_quitChoice + choice + 2) % 2;
                    menu_cursor_moved();
                }
                if (aFresh && !konamiConsumed) {
                    close_quit_prompt(&quitRequested);
                } else if (bFresh && !konamiConsumed) {
                    s_quitChoice = 0;   /* B always declines */
                    close_quit_prompt(&quitRequested);
                } else if (inputDown & HidMouseButton_Left) {
                    if (point_in(logicalX, logicalY, s_quitNoRect)) {
                        s_quitChoice = 0;
                        close_quit_prompt(&quitRequested);
                    } else if (point_in(logicalX, logicalY, s_quitYesRect)) {
                        s_quitChoice = 1;
                        close_quit_prompt(&quitRequested);
                    }
                }
            }
            if (quitRequested) break;
        } else if (menuDown & HidNpadButton_Plus) {
            /* On the clear popup + is the Retry key, and the homebrew menu
               cannot open while that popup is up, so don't steal it here. */
            if (g_result != 1 || screen != SCREEN_LEVEL)
                open_quit_prompt();
        } else if (s_modal == MODAL_CLEAR_DATA) {
            if (s_clearWrongTimer > 0) s_clearWrongTimer--;
            if (s_clearDoneTimer > 0) s_clearDoneTimer--;
            if (menuDown & HidNpadButton_X) {
                cancel_clear_data_modal();
            } else if (s_clearComplete) {
                /* The final A key must not also dismiss the success panel. */
                if (s_clearDoneTimer <= 0 && (aFresh || bFresh))
                    cancel_clear_data_modal();
            } else {
                feed_clear_data(menuDown);
            }
        } else if (s_modal != MODAL_NONE) {
            int count = modal_button_count();
            if (s_modal == MODAL_CREDITS) {
                int scroll = menu_move(menuDown, menuHeld, stickUD, &s_modalScrollRepeat);
                if (!konamiConsumed && scroll) credits_scroll_lines(scroll);
                /* padPointer reports positive wheel delta as upward. */
                if (wheel > 0) credits_scroll_lines(-1);
                if (wheel < 0) credits_scroll_lines(1);

                int page = menu_move_axis(menuDown, menuHeld, HidNpadButton_Left,
                                          HidNpadButton_Right, stickLR,
                                          &s_modalPageRepeat);
                int clickPage = 0;
                if (inputDown & HidMouseButton_Left) {
                    if (!credits_at_first_page() &&
                        point_in(logicalX, logicalY, credits_prev_rect())) clickPage = -1;
                    else if (!credits_at_last_page() &&
                             point_in(logicalX, logicalY, credits_next_rect())) clickPage = 1;
                }
                if (clickPage) credits_scroll_pages(clickPage);
                else if (!konamiConsumed && page) credits_scroll_pages(page);
            } else {
                int choice = menu_move(menuDown, menuHeld, stickUD, &s_modalRepeat);
                if (!choice)
                    choice = menu_move_axis(menuDown, menuHeld, HidNpadButton_Left,
                                            HidNpadButton_Right, stickLR,
                                            &s_modalPageRepeat);
                if (!konamiConsumed && choice) {
                    s_modalChoice = (s_modalChoice + choice + count) % count;
                    menu_cursor_moved();
                }
            }

            if (aFresh && !konamiConsumed) {
                confirm_modal();
            } else if (bFresh && !konamiConsumed && s_modal != MODAL_HINT) {
                /* A hint is mandatory: only its OK button restarts the level.
                   The quit prompt above still works because it is separate. */
                s_modal = MODAL_NONE;   /* B declines ordinary popups */
                audio_play(SFX_BUTTON_CLICK);
            } else if (inputDown & HidMouseButton_Left) {
                for (int i = 0; i < count; i++) {
                    if (point_in(logicalX, logicalY, modal_button_rect(i))) {
                        s_modalChoice = i;
                        confirm_modal();
                        break;
                    }
                }
            }
        } else {
            /* --- Screens --- */
            if (screen == SCREEN_INTRO) {
                SDL_Rect skip = { INTRO_BTN_X, INTRO_BTN_Y,
                                  INTRO_BTN_W, INTRO_BTN_H };
                bool tapSkip = (inputDown & HidMouseButton_Left) &&
                    point_in(logicalX, logicalY, skip);
                if (tapSkip || aFresh || bFresh) {
                    screen = SCREEN_TITLE;
                    titleClickCooldown = 12;
                    audio_play(SFX_BUTTON_CLICK);
                    LOG("intro cinematic skipped");
                }
            } else if (screen == SCREEN_TITLE) {
                int wx, wy;
                title_screen_to_world(logicalX, logicalY, &wx, &wy);
                bool inside = point_in(logicalX, logicalY,
                    (SDL_Rect){ TITLE_WIN_X, TITLE_WIN_Y, TITLE_WIN_W, TITLE_WIN_H });
                if (inside || game_is_dragging()) {
                    game_set_pointer_world(wx, wy);
                    if (inside && (inputDown & HidMouseButton_Left)) game_pointer_down();
                    game_pointer_move();
                    if (inputUp & HidMouseButton_Left) game_pointer_up();
                }
                render_set_drag_invalid(game_is_dragging() && !game_drag_valid());

                if (audio_current_playlist() != MUSIC_TITLE) {
                    audio_music_start(MUSIC_TITLE);
                    LOG("title music resumed");
                }
                bool tapCredits = (inputDown & HidMouseButton_Left) &&
                    point_in(logicalX, logicalY,
                        (SDL_Rect){ CREDITS_BOX_X, CREDITS_BOX_Y,
                                    CREDITS_BOX_W, CREDITS_BOX_H });
                bool tapClearData = (inputDown & HidMouseButton_Left) &&
                    point_in(logicalX, logicalY, clear_data_rect());
                SDL_Rect replayBox = { REPLAY_BOX_X, REPLAY_BOX_Y,
                                       REPLAY_BOX_W, REPLAY_BOX_H };
                bool tapReplay = s_introSeqCount > 0 && (inputDown & HidMouseButton_Left) &&
                    point_in(logicalX, logicalY, replayBox);
                if (tapCredits || (kDown & (HidNpadButton_R | HidNpadButton_ZR))) {
                    open_credits_modal();
                }
                if (tapClearData || (menuDown & HidNpadButton_Y)) {
                    game_cancel_drag();
                    open_clear_data_modal();
                    titleClickCooldown = 12;
                }
                if (tapReplay && !titleClickCooldown) {
                    screen = SCREEN_INTRO;
                    s_introStep = 0;
                    s_introAccumUs = 0;
                    audio_play(SFX_BUTTON_CLICK);
                    titleClickCooldown = 12;
                    LOG("replay intro from title");
                }
                bool tapStart = (inputDown & HidMouseButton_Left) &&
                    point_in(logicalX, logicalY,
                        (SDL_Rect){ START_BOX_X, START_BOX_Y, START_BOX_W, START_BOX_H });
                if (!tapReplay && ((!konamiConsumed && (aFresh || bFresh)) ||
                    (tapStart && !titleClickCooldown && !tapCredits &&
                     !tapClearData))) {
                    game_cancel_drag();
                    music_wrap_up();
                    s_slotFocus = s_selectedSlot;
                    s_fileAction = FILE_ACTION_NONE;
                    screen = SCREEN_SLOTS;
                    audio_play(SFX_BUTTON_CLICK);
                    titleClickCooldown = 12;
                    s_screenCooldown = 6;
                    nav_timers_reset();
                    LOG("screen -> SLOTS");
                }
            } else if (screen == SCREEN_SLOTS) {
                if (!konamiConsumed) {
                    /* Up/down walks the three slot rows and the button row,
                       wrapping from the buttons back to slot 1. Left/right
                       walks COPY/JUKEBOX/ERASE without leaving that row. */
                    int move = menu_move(menuDown, menuHeld, stickUD, &s_navRepeat);
                    if (move) slot_focus_move(move);
                    int side = menu_move_axis(menuDown, menuHeld,
                                              HidNpadButton_Left, HidNpadButton_Right,
                                              stickLR, &s_pageRepeat);
                    if (side) slot_focus_button_move(side);
                }
                if (wheel > 0) slot_focus_move(-1);
                if (wheel < 0) slot_focus_move(1);

                bool tapCopy = (inputDown & HidMouseButton_Left) &&
                    point_in(logicalX, logicalY, slot_copy_rect());
                bool tapJukebox = (inputDown & HidMouseButton_Left) &&
                    point_in(logicalX, logicalY, slot_jukebox_rect());
                bool tapErase = (inputDown & HidMouseButton_Left) &&
                    point_in(logicalX, logicalY, slot_erase_rect());
                int tapRow = -1;
                if ((inputDown & HidMouseButton_Left) && !tapCopy &&
                    !tapJukebox && !tapErase) {
                    for (int i = 0; i < save_slot_count(); i++) {
                        if (point_in(logicalX, logicalY, slot_row_rect(i))) {
                            tapRow = i;
                            break;
                        }
                    }
                }

                bool shortcutCopy = (kDown & HidNpadButton_L) != 0;
                bool shortcutErase = (menuDown & HidNpadButton_Y) != 0;
                /* A pointer tap always acknowledges the button, including the
                   locked Jukebox; the hover highlight is drawn independently. */
                if (s_fileAction == FILE_ACTION_NONE) {
                    if (tapCopy) slot_focus_set_quiet(SLOT_FOCUS_COPY);
                    if (tapErase) slot_focus_set_quiet(SLOT_FOCUS_ERASE);
                    if (tapJukebox) slot_focus_set_quiet(SLOT_FOCUS_JUKEBOX);
                }

                if (s_fileAction != FILE_ACTION_NONE) {
                    int picked = tapRow >= 0 ? tapRow
                               : ((!konamiConsumed && aFresh &&
                                   s_slotFocus < save_slot_count())
                                      ? s_slotFocus : -1);
                    if (!konamiConsumed && bFresh) {
                        cancel_file_action();
                    } else if (picked >= 0 && !konamiConsumed) {
                        if (s_fileAction == FILE_ACTION_COPY_SOURCE) {
                            s_copySource = picked;
                            s_fileAction = FILE_ACTION_COPY_DEST;
                            s_slotFocus = (picked + 1) % save_slot_count();
                            s_selectedSlot = s_slotFocus;
                            nav_timers_reset();
                            audio_play(SFX_BUTTON_CLICK);
                            LOG("copy source slot %d; choose destination", picked + 1);
                        } else if (s_fileAction == FILE_ACTION_COPY_DEST) {
                            if (picked == s_copySource) {
                                audio_play(SFX_REJECT);
                                LOG("copy destination matches source");
                            } else {
                                confirm_copy_destination(picked);
                            }
                        } else {
                            open_erase_slot_modal(picked);
                            s_fileAction = FILE_ACTION_NONE;
                        }
                    }
                } else {
                    bool didErase = false, didCopy = false, didJukebox = false;
                    if (tapErase || shortcutErase ||
                        (!konamiConsumed && aFresh && s_slotFocus == SLOT_FOCUS_ERASE)) {
                        start_erase_action();
                        didErase = true;
                    } else if (tapCopy || shortcutCopy ||
                               (!konamiConsumed && aFresh && s_slotFocus == SLOT_FOCUS_COPY)) {
                        start_copy_action();
                        didCopy = true;
                    }
                    if (!didErase && !didCopy &&
                        ((tapJukebox && !titleClickCooldown) ||
                         (!konamiConsumed && aFresh && s_slotFocus == SLOT_FOCUS_JUKEBOX))) {
                        if (jukebox_unlocked()) {
                            screen = open_jukebox();
                        } else {
                            audio_play(SFX_REJECT);
                            s_jukeboxHint = 180;
                            LOG("jukebox still locked (needs 40 GOLD or Konami)");
                        }
                        didJukebox = true;
                    }

                    /* One A press on a file row opens it: no second confirm. */
                    bool openFile = !didErase && !didCopy && !didJukebox &&
                        ((!konamiConsumed && aFresh &&
                          s_slotFocus < save_slot_count()) ||
                         (tapRow >= 0 && !titleClickCooldown));
                    if (openFile) {
                        if (tapRow >= 0) slot_focus_set(tapRow);
                        audio_play(SFX_BUTTON_CLICK);
                        save_set_active_slot(s_selectedSlot);
                        s_selected = 0;
                        screen = prepare_select_screen();
                        titleClickCooldown = 12;
                        LOG("opened save slot %d", s_selectedSlot + 1);
                    }
                    if (!konamiConsumed && bFresh) screen = return_to_title();
                }
                render_set_drag_invalid(false);
            } else if (screen == SCREEN_JUKEBOX) {
                if (!konamiConsumed) {
                    int move = menu_move(menuDown, menuHeld, stickUD, &s_navRepeat);
                    if (move) jukebox_move_group(move);
                    int side = menu_move_axis(menuDown, menuHeld, HidNpadButton_Left,
                                              HidNpadButton_Right, stickLR,
                                              &s_pageRepeat);
                    if (side) jukebox_move_track(side);
                }
                if (wheel > 0) jukebox_move_group(-1);
                if (wheel < 0) jukebox_move_group(1);

                int tapTrack = -1;
                if ((inputDown & HidMouseButton_Left) &&
                    jukebox_hit_test(logicalX, logicalY, &tapTrack) &&
                    tapTrack != s_jukeboxTrack) {
                    s_jukeboxTrack = tapTrack;
                    audio_play(SFX_BUTTON_CLICK);
                }

                if (!konamiConsumed && aFresh) {
                    /* A on any cell starts that group's real playlist at its
                       first segment and keeps it going until R or B. The
                       yellow selection stays put while the green cursor
                       follows the game's random-N sequence. */
                    int group = jukebox_group(s_jukeboxTrack);
                    /* A always jumps to the first segment of the horizontal
                       group; never start from the highlighted column. L is the
                       action that plays/loops exactly the highlighted cell. */
                    int first = jukebox_group_first(group);
                    audio_jukebox_play_group(first, jukebox_group_last(group),
                                             group);
                    audio_play(SFX_BUTTON_CLICK);
                    LOG("jukebox A: play group %d from first track %d "
                        "(highlighted %d)", group, first, s_jukeboxTrack);
                }
                if (kDown & HidNpadButton_L) {
                    bool looping = audio_jukebox_toggle_loop(s_jukeboxTrack);
                    audio_play(SFX_BUTTON_CLICK);
                    LOG("jukebox L: track %d loop %s", s_jukeboxTrack,
                        looping ? "on" : "off");
                }
                if (kDown & HidNpadButton_R) {
                    /* R ends what is actually playing, not the yellow hover. */
                    int playing = audio_music_current_track();
                    if (playing >= 0) {
                        audio_jukebox_queue_end(playing);
                        audio_play(SFX_BUTTON_CLICK);
                        LOG("jukebox R: queued end for playing track %d", playing);
                    }
                }
                if (!konamiConsumed && bFresh) {
                    audio_jukebox_stop();
                    jukebox_preview_reset();
                    audio_play(SFX_BUTTON_CLICK);
                    s_bNeedsRelease = true;
                    LOG("jukebox B: stopped and queue cleared");
                }
                if (menuDown & HidNpadButton_X) screen = close_jukebox();
                render_set_drag_invalid(false);
            } else if (screen == SCREEN_SELECT) {
                /* L/ZL and R/ZR double as page buttons here; they are grab
                   buttons only while actually playing a level. */
                bool shoulderNav = (kDown & (HidNpadButton_L | HidNpadButton_ZL |
                                             HidNpadButton_R | HidNpadButton_ZR)) != 0;
                if (!konamiConsumed) {
                    int move = menu_move(menuDown, menuHeld, stickUD, &s_navRepeat);
                    if (move) select_move_row(move);
                    int page = menu_move_axis(menuDown, menuHeld, HidNpadButton_Left,
                                              HidNpadButton_Right, stickLR,
                                              &s_pageRepeat);
                    if (!page && shoulderNav)
                        page = (kDown & (HidNpadButton_L | HidNpadButton_ZL)) ? -1 : 1;
                    if (page) select_page(page);
                }
                if (wheel > 0) select_move_row(-1);
                if (wheel < 0) select_move_row(1);

                if ((inputDown & HidMouseButton_Left) && !shoulderNav) {
                    int pg = s_selected / JB_LEVELS_PER_GROUP;
                    if (pg > 0 && point_in(logicalX, logicalY, select_prev_rect())) {
                        select_page(-1);
                    } else if (pg < page_count() - 1 &&
                               point_in(logicalX, logicalY, select_next_rect())) {
                        select_page(1);
                    } else {
                        int page = s_selected / JB_LEVELS_PER_GROUP;
                        for (int row = 0; row < JB_LEVELS_PER_GROUP; row++) {
                            int idx = page * JB_LEVELS_PER_GROUP + row;
                            if (idx >= g_levelCount) break;
                            SDL_Rect rect = { SELECT_X,
                                SELECT_ROW_Y + row * SELECT_ROW_H, SELECT_W, 25 };
                            if (point_in(logicalX, logicalY, rect)) {
                                s_selected = idx;
                                screen = start_level(idx);
                                break;
                            }
                        }
                    }
                }
                if (!konamiConsumed && aFresh) {
                    screen = start_level(s_selected);
                }
                if (!konamiConsumed && bFresh) {
                    screen = return_to_slots();
                }
                render_set_drag_invalid(false);
            } else {
                int wx, wy;
                game_screen_to_world(logicalX, logicalY, &wx, &wy);
                if (s_inputMode == INPUT_MOUSE && s_useStickCursor)
                    game_set_pointer_world(s_cursorWX, s_cursorWY);
                else
                    game_set_pointer_world(wx, wy);
                if (inputDown & HidMouseButton_Left)
                    LOG("grab click: screen %d,%d world %d,%d stick=%d pad=%d owner=%d",
                        logicalX, logicalY, g_pointerWorldX, g_pointerWorldY,
                        s_useStickCursor ? 1 : 0, s_activePad, (int)s_clickOwner);
                /* HUD/footer clicks belong to the UI, not to the board. */
                bool uiClick = (inputDown & HidMouseButton_Left) &&
                               (logicalY < 40 || logicalY >= JB_SCREEN_H - 32);
                if ((inputDown & HidMouseButton_Left) && !uiClick) game_pointer_down();
                game_pointer_move();
                if ((inputUp & HidMouseButton_Left) && !uiClick) game_pointer_up();
                render_set_drag_invalid(game_is_dragging() && !g_result &&
                                        !game_drag_valid());

                /* Commit the clear before handling A. start_level() checks the
                   active file's unlock state, and this clear may have just
                   opened the next building. */
                if (g_result && !s_resultMusicPlayed) {
                    s_resultMusicPlayed = true;
                    if (g_result == 1) {
                        int building = s_levelIndex / JB_LEVELS_PER_GROUP;
                        int next = building + 1;
                        int buildings = (g_levelCount + JB_LEVELS_PER_GROUP - 1) /
                                        JB_LEVELS_PER_GROUP;
                        bool wasUnlocked = next < buildings && save_building_unlocked(next);
                        save_record_win(s_levelIndex, g_moves, g_level.par);
                        if (next < buildings && !wasUnlocked &&
                            save_building_unlocked(next)) {
                            s_unlockedBuilding = next + 1;
                            audio_play(SFX_GET_POWERUP);
                            LOG("building %d unlocked by clearing level %d",
                                s_unlockedBuilding, s_levelIndex);
                        }
                        /* The end stinger waits for this measure to finish. */
                        audio_music_request_end(audio_current_playlist());
                    }
                }
                if (!g_result) s_resultMusicPlayed = false;

                bool popupActive = (s_modal != MODAL_NONE) || s_quitPrompt;
                bool hintClicked = !popupActive &&
                                   (inputDown & HidMouseButton_Left) &&
                                   point_in(logicalX, logicalY, s_hintRect);
                if (hintClicked) open_hint_modal();
                bool retryRequested = false;
                if (menuDown & HidNpadButton_Y) {
                    if (g_result == 1) {
                        retryRequested = true;
                        s_restartLevel = true;
                        LOG("retry from clear requested with Y");
                    } else if (g_result) {
                        LOG("undo ignored: result screen shown");
                    } else if (game_undo_blocked()) {
                        audio_play(SFX_BLOCK_CLICK);
                        LOG("undo ignored: Junkbot is entering the last bin");
                    } else {
                        game_undo();
                        LOG("undo (%d move(s) left, moves now %d)",
                            game_undo_depth(), g_moves);
                    }
                }
                if (!popupActive && g_result == 2 &&
                    (inputDown & HidMouseButton_Left) &&
                    point_in(logicalX, logicalY, s_resultHintRect))
                    open_hint_modal();

                /* The clear popup accepts the face keys from every pad plus
                   shoulder/system shortcuts. A is inert after the final level. */
                bool clearPopup = g_result == 1;
                bool nextRequested = false, leaveRequested = false;
                if (clearPopup && !retryRequested && !konamiConsumed) {
                    /* Read -/+ straight from the pads as well as the filtered
                       menu edges. While any popup is animating the shared
                       menuDown mask is intentionally stripped down to just +
                       (and cleared entirely while the result panel opens), so
                       a retry press during that window was silently lost. */
                    u64 retryKeys = menuDown;
                    for (int i = 0; i < PAD_SOURCES; i++) {
                        if (padIsConnected(&s_pads[i]))
                            retryKeys |= padGetButtonsDown(&s_pads[i]) &
                                         (HidNpadButton_Minus | HidNpadButton_Plus);
                    }
                    if (retryKeys & (HidNpadButton_Minus | HidNpadButton_Plus)) {
                        retryRequested = true;
                        s_restartLevel = true;
                        LOG("retry from clear requested with -/+");
                    } else if (popupDown & (HidNpadButton_ZL | HidNpadButton_ZR)) {
                        nextRequested = true;
                    } else if (popupDown & (HidNpadButton_L | HidNpadButton_R)) {
                        leaveRequested = true;
                    } else if (aFresh) {
                        nextRequested = true;
                    } else if (bFresh) {
                        leaveRequested = true;
                    }
                }
                if (nextRequested) {
                    if (result_has_next_level()) {
                        LOG("advancing from level %d to %d", s_levelIndex,
                            s_levelIndex + 1);
                        screen = start_level(s_levelIndex + 1);
                    } else {
                        LOG("final level cleared: next has no destination");
                    }
                } else if (leaveRequested) {
                    s_leaveLevel = true;
                } else if (!retryRequested && aFresh && !konamiConsumed) {
                    /* A destroyed Junkbot still retries with A; during play A
                       is the ordinary Reset warning. */
                    if (g_result == 1) {
                        if (result_has_next_level()) {
                            LOG("advancing from level %d to %d", s_levelIndex,
                                s_levelIndex + 1);
                            screen = start_level(s_levelIndex + 1);
                        } else {
                            LOG("final level cleared: A has no next level");
                        }
                    } else if (g_result == 2) {
                        s_restartLevel = true;
                    } else {
                        open_warning_modal(MODAL_LEVEL_RESTART);
                    }
                } else if (!retryRequested && bFresh && !konamiConsumed) {
                    if (g_result) s_leaveLevel = true;
                    else open_warning_modal(MODAL_LEVEL_EXIT);
                }
            }
        }

        /* Warnings are confirmed by the modal handler; apply them here so the
           rest of this frame never also sees the same A/B press. */
        if (s_leaveLevel) {
            s_leaveLevel = false;
            /* Leaving a level/result screen returns to THIS file's level list.
               B on the level list itself is still the way back to file select. */
            screen = prepare_select_screen();
        }
        if (s_restartLevel) {
            s_restartLevel = false;
            game_reset_level();
            audio_music_start(-1);       /* retry after clear picks a fresh track */
            s_unlockedBuilding = 0;
            s_resultMusicPlayed = false;
            LOG("level restarted (moves=%d)", g_moves);
            titleClickCooldown = 12;
            s_screenCooldown = 6;
            nav_timers_reset();
        }

        Uint64 now = SDL_GetPerformanceCounter();
        Uint64 dtUs = (now - last) * 1000000ULL / freq;
        last = now;
        if (dtUs > 250000ULL) dtUs = 250000ULL;

        /* Advance the intro flipbook at the SWF's native 15 fps. */
        if (screen == SCREEN_INTRO && s_introSeqCount > 0 && !s_suspended &&
            !s_quitPrompt && s_modal == MODAL_NONE) {
            s_introAccumUs += (Uint32)dtUs;
            while (s_introAccumUs >= INTRO_STEP_US) {
                s_introAccumUs -= INTRO_STEP_US;
                if (++s_introStep >= s_introSeqCount) {
                    s_introStep = 0;
                    screen = SCREEN_TITLE;
                    titleClickCooldown = 12;
                    LOG("intro cinematic finished");
                    break;
                }
            }
        }

        accumulator += dtUs;
        int simSteps = 0;
        bool frozen = s_modal != MODAL_NONE || s_quitPrompt || s_suspended;
        /* One tick max: a stalled frame must never turn into a burst of sprite
           teleports on the next present. The 18 Hz simulation intentionally
           holds sprites for 3-4 display frames at 60 Hz. */
        while (accumulator >= step && simSteps < 1) {
            if (!frozen && (screen == SCREEN_LEVEL || screen == SCREEN_TITLE))
                game_simulate_tick();  /* title is ambient: animation only */
            accumulator -= step;
            simSteps++;
        }
        if (accumulator >= step) accumulator = 0;  /* drop debt, never catch up */
        if (screen == SCREEN_SLOTS || screen == SCREEN_JUKEBOX ||
            screen == SCREEN_SELECT || frozen)
            accumulator = 0;

        /* Catch popup state changes made by this frame's input handling. */
        popup_sync(popup_now_us(), screen);

        /* --- Draw at 1x into s_frame --- */
        SDL_SetRenderTarget(renderer, s_frame);
        SDL_SetRenderDrawColor(renderer, 20, 24, 32, 255);
        SDL_RenderClear(renderer);
        if (screen == SCREEN_TITLE) {
            show_title_screen(renderer);
            if (s_inputMode == INPUT_MOUSE &&
                point_in(s_reticleSX, s_reticleSY,
                    (SDL_Rect){ TITLE_WIN_X, TITLE_WIN_Y, TITLE_WIN_W, TITLE_WIN_H })) {
                SDL_Rect ring = { s_reticleSX - 7, s_reticleSY - 7, 14, 14 };
                SDL_SetRenderDrawColor(renderer, 255, 235, 90, 220);
                SDL_RenderDrawRect(renderer, &ring);
                SDL_Rect dot = { s_reticleSX - 1, s_reticleSY - 1, 3, 3 };
                SDL_RenderFillRect(renderer, &dot);
            }
            /* The magnifier samples s_world. Mirror the finished title frame
               into it while a touch drag is active so the title screen gets the
               same zoomed grab view as a level. */
            if (s_world && s_inputMode == INPUT_TOUCH && game_is_dragging()) {
                SDL_SetRenderTarget(renderer, s_world);
                SDL_RenderCopy(renderer, s_frame, NULL, NULL);
                SDL_SetRenderTarget(renderer, s_frame);
            }
        } else if (screen == SCREEN_SLOTS) {
            draw_slot_select(renderer);
        } else if (screen == SCREEN_JUKEBOX) {
            draw_jukebox(renderer);
        } else if (screen == SCREEN_SELECT) {
            draw_select(renderer);
        } else if (screen == SCREEN_INTRO) {
            int idx = s_introSeqCount > 0 ? s_introSeq[s_introStep] : 0;
            render_intro_frame(renderer, idx);
            SDL_Rect skip = { INTRO_BTN_X, INTRO_BTN_Y,
                              INTRO_BTN_W, INTRO_BTN_H };
            render_intro_button(renderer, false,
                                point_in(s_inputSX, s_inputSY, skip), skip);
            render_text(renderer, g_res.font, "A / Tap to Skip",
                        JB_SCREEN_W / 2, JB_SCREEN_H - 24, WHITE, true);
        } else {
            int ox, oy;
            game_get_render_origin(&ox, &oy);
            render_set_world_origin(ox, oy);
            int clipX, clipY, clipW, clipH;
            game_get_level_clip(&clipX, &clipY, &clipW, &clipH);
            SDL_Rect levelClip = { clipX, clipY, clipW, clipH };
            if (s_world) {
                SDL_SetRenderTarget(renderer, s_world);
                SDL_RenderSetClipRect(renderer, &levelClip);
                render_world(renderer);
                SDL_RenderSetClipRect(renderer, NULL);
                SDL_SetRenderTarget(renderer, s_frame);
                SDL_RenderCopy(renderer, s_world, NULL, NULL);
            } else {
                SDL_RenderSetClipRect(renderer, &levelClip);
                render_world(renderer);
                SDL_RenderSetClipRect(renderer, NULL);
            }
            if (s_inputMode == INPUT_MOUSE) {
                SDL_Rect ring = { s_reticleSX - 7, s_reticleSY - 7, 14, 14 };
                SDL_SetRenderDrawColor(renderer, 255, 235, 90, 220);
                SDL_RenderDrawRect(renderer, &ring);
                SDL_Rect dot = { s_reticleSX - 1, s_reticleSY - 1, 3, 3 };
                SDL_RenderFillRect(renderer, &dot);
            }
            draw_hud(renderer);
            if (g_result || popup_drawable(&s_resultAnim)) draw_result(renderer);
        }
        draw_modal(renderer);
        draw_magnifier(renderer);
        draw_muted_overlay(renderer);
        draw_quit_prompt(renderer);   /* always the topmost overlay */

        SDL_SetRenderTarget(renderer, NULL);
        if (s_frame) {
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, s_frame, NULL, NULL);
        }
        if (s_noVsync && freq > 0) {
            /* Drift-free 60 Hz fallback only when SDL could not give us real
               vsync; never double-throttle an accelerated/vsynced renderer. */
            const Uint64 period = freq / 60;
            Uint64 nowTicks = SDL_GetPerformanceCounter();
            if (!nextPresent) nextPresent = nowTicks;
            Uint64 target = nextPresent + period;
            nowTicks = SDL_GetPerformanceCounter();
            if (target > nowTicks) {
                SDL_Delay((Uint32)((target - nowTicks) * 1000 / freq));
                nextPresent = target;
            } else {
                nextPresent = nowTicks;  /* drop missed slots, no debt */
            }
        }
        SDL_RenderPresent(renderer);
    }

cleanup:
    LOG("leaving main loop");
    quit_game();
    audio_exit();
    render_exit();
    if (s_titleWorld) SDL_DestroyTexture(s_titleWorld);
    if (s_world) SDL_DestroyTexture(s_world);
    if (s_frame) SDL_DestroyTexture(s_frame);
    free(s_introSeq);
    s_introSeq = NULL;
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    for (int i = 0; i < PAD_SOURCES; i++)
        if (s_pointerReady[i]) padPointerExit(&s_pointers[i]);
    appletUnhook(&s_focusCookie);
    assets_exit();
    if (s_resEventOpen) eventClose(&s_resEvent);
    log_exit();
    return 0;
}
