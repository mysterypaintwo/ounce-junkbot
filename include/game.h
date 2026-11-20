#ifndef JUNKBOT_GAME_H
#define JUNKBOT_GAME_H

#include <stdbool.h>

#define JB_GRID_X 15
#define JB_GRID_Y 18
#define JB_MAX_ENTITIES 1024
#define JB_MAX_DECALS 160
#define JB_MAX_LEVELS 128
#define JB_SIM_HZ 18
/* The original Junkbot level select has four buildings of fifteen levels. */
#define JB_LEVELS_PER_GROUP 15

typedef enum {
    ET_BOUNDS = 0,
    ET_BRICK,
    ET_JUNKBOT,
    ET_GEARBOT,
    ET_CLIMBBOT,
    ET_FLYBOT,
    ET_EYEBOT,
    ET_BIN,
    ET_CRATE,
    ET_FIRE,
    ET_FAN,
    ET_LASER,
    ET_SWITCH,
    ET_TELEPORT,
    ET_JUMP,
    ET_SHIELD,
    ET_PIPE,
    ET_DROPLET
} EntityType;

typedef enum {
    C_GRAY = 0,
    C_BLUE,
    C_RED,
    C_GREEN,
    C_YELLOW,
    C_WHITE
} BrickColor;

typedef struct {
    int x, y;
    char name[64];
} Decal;

typedef struct {
    int id;
    EntityType type;
    int x, y;
    int w, h;

    bool fixed;
    bool grabbed;
    bool floating, wasFloating;
    bool removeBeforeRender;

    int color;
    int studs;

    int facing, facingY;
    int anim;
    int energy;
    int active;
    int used;
    int on;
    int switchID;
    int teleportID;
    int timer;
    bool teleportBlocked;
    bool scaredy;

    int momentumX, momentumY;
    bool splashing;
    bool dead, dying, dyingWater, collectingBin, gettingShield;
    bool armored, losingShield;
    int losingShieldTime;
    bool headLoaded;

    int grabOffX, grabOffY;
} Entity;

typedef struct {
    char title[256];
    char hint[512];
    int par;
    int boundsW, boundsH;
    char backdrop[64];
    Decal decals[JB_MAX_DECALS];
    int decalCount;
    Decal bgdecals[JB_MAX_DECALS];
    int bgdecalCount;
    Entity entities[JB_MAX_ENTITIES];
    int entityCount;
} Level;

typedef struct {
    Entity *items[64];
    int upwardCount;
    int downwardCount;
    int total;
} GrabOptions;

typedef struct {
    Entity *fan;
    int extents[4];
    int count;
} WindColumn;

typedef struct {
    Entity *laser;
    int extent;
    Entity *hit;
} LaserBeam;

extern Level g_level;
extern int g_moves;
extern int g_frameCounter;
extern bool g_paused;
extern int g_result; /* 0 playing, 1 win, 2 lose */
extern int g_pointerWorldX, g_pointerWorldY;
extern char g_levelNames[JB_MAX_LEVELS][256];
extern int g_levelCount;
extern WindColumn g_winds[32];
extern int g_windCount;
extern LaserBeam g_laserBeams[32];
extern int g_laserBeamCount;

bool game_init(void);
void game_exit(void);

/* Scan romfs:/levels. Returns number of level titles. */
int game_scan_levels(void);
/* Load a level by its display title. Returns false if not found/parse failed. */
bool game_load_level(const char *title);
void game_reset_level(void);

Entity *game_junkbot(void);

void game_set_pointer_world(int x, int y);
void game_pointer_down(void);
void game_pointer_move(void);
void game_pointer_up(void);
void game_cancel_drag(void);
/* Restore the board and move count to a previous completed move. */
void game_undo(void);
/* Number of completed moves still available for undo (up to 500). */
int game_undo_depth(void);
/* True when Junkbot is being collected by the final recycle bin: undo is
   disabled at that point. Always false on the ambient title screen. */
bool game_undo_blocked(void);

/* Ambient mode (title screen): simulate for animation but never report a win
   or play the win/lose stingers. */
void game_set_ambient(bool ambient);

bool game_is_dragging(void);
/* True only while a group is held and the current position is a legal drop. */
bool game_drag_valid(void);

void game_simulate_tick(void);

/* Screen-space helpers in logical (960x540) pixels. */
void game_get_render_origin(int *ox, int *oy);
/* Playfield rectangle in logical pixels; art should be clipped to it. */
void game_get_level_clip(int *x, int *y, int *w, int *h);
void game_screen_to_world(int sx, int sy, int *wx, int *wy);
void game_world_to_screen(int wx, int wy, int *sx, int *sy);

#endif
