#ifndef JUNKBOT_AUDIO_H
#define JUNKBOT_AUDIO_H

#include <stdbool.h>

typedef enum {
    SFX_TURN = 0,
    SFX_BLOCK_PICKUP,
    SFX_BLOCK_DROP,
    SFX_BLOCK_CLICK,
    SFX_BUTTON_CLICK,
    SFX_FALL,
    SFX_HEAD_BONK,
    SFX_COLLECT_BIN,
    SFX_COLLECT_BIN2,
    SFX_SWITCH_CLICK,
    SFX_SWITCH_ON,
    SFX_SWITCH_OFF,
    SFX_DEATH_FIRE,
    SFX_DEATH_WATER,
    SFX_DEATH_LASER,
    SFX_DEATH_BOT,
    SFX_GET_SHIELD,
    SFX_GET_POWERUP,
    SFX_LOSE_POWERUP,
    SFX_TELEPORT,
    SFX_YEAH,
    SFX_OUCH,
    SFX_UHOH,
    SFX_JUMP,
    SFX_FAN,
    SFX_DRIP0,
    SFX_DRIP1,
    SFX_DRIP2,
    SFX_MENU_MOVE,
    SFX_REJECT,
    SFX_TAB_SWITCH,
    SFX_COUNT
} SoundID;

/* Music playlists, matching the original Lingo sound members:
   0 = title/intro, 1-5 = the five level music sets. */
#define MUSIC_TITLE 0
#define MUSIC_LEVEL_MIN 1
#define MUSIC_LEVEL_MAX 5

bool audio_init(void);
void audio_exit(void);
void audio_play(SoundID id);

/* Start a looping playlist. -1 picks a random level set (1-5). */
void audio_music_start(int playlist);
/* Stop any music immediately. */
void audio_music_stop(void);
/* Let the current one-measure loop finish, then play the playlist's short end
   stinger once (level clear / entering level select from the title). */
void audio_music_request_end(int playlist);
/* Entering File Select or Level Select always wraps up whatever is playing,
   no matter which screen/playlist/jukebox action started it. If the end
   stinger is already playing or queued, it is left alone to finish and die. */
void audio_music_wrap_up(void);
/* Immediate end stinger, kept for callers that need it synchronously. */
void audio_music_end_stinger(int playlist);
/* Playlist currently loaded (for the end stinger / title-music checks). */
int audio_current_playlist(void);
/* Advance the segmented BGM queue. Call once per frame from the main loop. */
void audio_tick(void);

/* Jukebox (unlocked from File Select). Tracks are indices into the same music
   timing table: 0..audio_music_track_count()-1. */
int  audio_music_track_count(void);
const char *audio_music_track_name(int track);
double audio_music_track_bpm(int track);     /* 0 for one-shot stingers */
int  audio_music_current_track(void);        /* -1 when stopped */
/* Queue one segment; it starts now if nothing is playing, otherwise after the
   current sound finishes. */
void audio_jukebox_queue_track(int track);
/* Play one BGM group the way the game does: start at the group's first segment,
   then keep feeding that group's own `random N` playlist with refill-at-5. It
   never crosses into another group and never ends on its own; R queues the
   stinger, B stops. Playback ending never changes the menu selection. */
void audio_jukebox_play_group(int first, int last, int playlist);
/* Toggle perpetual replay of a segment. Returns the new enabled state. */
bool audio_jukebox_toggle_loop(int track);
int  audio_jukebox_loop_track(void);         /* -1 when off */
bool audio_jukebox_looping(void);
/* Queue the matched end stinger after the current segment. */
void audio_jukebox_queue_end(int track);
bool audio_jukebox_end_queued(void);
/* Halt music immediately and clear the jukebox queue/loop/end state. */
void audio_jukebox_stop(void);

/* Playback info for the jukebox UI. The serial increments once every time a
   segment starts, so the sequence preview can animate exactly one step per
   real playback change instead of polling the current track. */
unsigned audio_music_play_serial(void);
unsigned audio_music_position_ms(void);   /* elapsed ms in the current segment */
unsigned audio_music_track_ms(int track); /* full segment length in ms, 0 if none */
int audio_jukebox_next_track(void);        /* next queued segment, -1 when idle */

void audio_set_muted(bool muted);
bool audio_is_muted(void);
/* HOME menu / sleep: freeze and resume all audio with the game. */
void audio_set_paused(bool paused);

#endif
