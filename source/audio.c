#include "audio.h"
#include "jb_music_timing.h"
#include "log.h"

#include <SDL.h>
#include <SDL_mixer.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *s_paths[SFX_COUNT] = {
    [SFX_TURN] = "romfs:/audio/sound-effects/turn1.wav",
    [SFX_BLOCK_PICKUP] = "romfs:/audio/sound-effects/blockpickup.wav",
    [SFX_BLOCK_DROP] = "romfs:/audio/sound-effects/blockdrop.wav",
    [SFX_BLOCK_CLICK] = "romfs:/audio/sound-effects/blockclick.wav",
    [SFX_BUTTON_CLICK] = "romfs:/audio/sound-effects/h_button1.wav",
    [SFX_FALL] = "romfs:/audio/sound-effects/fall.wav",
    [SFX_HEAD_BONK] = "romfs:/audio/sound-effects/headbonk1.wav",
    [SFX_COLLECT_BIN] = "romfs:/audio/sound-effects/eat1.wav",
    [SFX_COLLECT_BIN2] = "romfs:/audio/sound-effects/garbage1.wav",
    [SFX_SWITCH_CLICK] = "romfs:/audio/sound-effects/switch_click.wav",
    [SFX_SWITCH_ON] = "romfs:/audio/sound-effects/switch_on.wav",
    [SFX_SWITCH_OFF] = "romfs:/audio/sound-effects/switch_off.wav",
    [SFX_DEATH_FIRE] = "romfs:/audio/sound-effects/fire.wav",
    [SFX_DEATH_WATER] = "romfs:/audio/sound-effects/electricity1.wav",
    [SFX_DEATH_LASER] = "romfs:/audio/sound-effects/undercover/laser_hit.wav",
    [SFX_DEATH_BOT] = "romfs:/audio/sound-effects/robottouch4.wav",
    [SFX_GET_SHIELD] = "romfs:/audio/sound-effects/shieldon2.wav",
    [SFX_GET_POWERUP] = "romfs:/audio/sound-effects/h_powerup1.wav",
    [SFX_LOSE_POWERUP] = "romfs:/audio/sound-effects/h_powerdown3.wav",
    [SFX_TELEPORT] = "romfs:/audio/sound-effects/undercover/teleport.wav",
    [SFX_YEAH] = "romfs:/audio/sound-effects/voice_ohyeah.wav",
    [SFX_OUCH] = "romfs:/audio/sound-effects/voice_ouch.wav",
    [SFX_UHOH] = "romfs:/audio/sound-effects/voice_uhoh.wav",
    [SFX_JUMP] = "romfs:/audio/sound-effects/jump3.wav",
    [SFX_FAN] = "romfs:/audio/sound-effects/fan.wav",
    [SFX_DRIP0] = "romfs:/audio/sound-effects/drip1.wav",
    [SFX_DRIP1] = "romfs:/audio/sound-effects/drip2.wav",
    [SFX_DRIP2] = "romfs:/audio/sound-effects/drip3.wav",
    [SFX_MENU_MOVE] = "romfs:/audio/sound-effects/turn1.wav",
    [SFX_REJECT] = "romfs:/audio/sound-effects/headbonk1.wav",
    [SFX_TAB_SWITCH] = "romfs:/audio/sound-effects/h_powerup3.wav",
};

/* Playlist entries are filenames into the trimmed music cache. The original
   `random N` repeat counts are still applied queue-item by queue-item. */
static const char *const s_playlists[6][9] = {
    [MUSIC_TITLE] = {
        "intro_1.1.ogg", "intro_1.2.ogg", "intro_1.3.ogg", NULL },
    [1] = {
        "demo6_3.ogg", "demo6_5.ogg", "demo6_6.ogg", NULL },
    [2] = {
        "lego1.1.ogg", "lego1.2.ogg", "lego1.3.ogg", NULL },
    [3] = {
        "demo_4.1.ogg", "demo_4.3.ogg", "demo_4.5.ogg",
        "demo_4.6.ogg", NULL },
    [4] = {
        "demo_5.1.ogg", "demo_5.2.ogg", "demo_5.7.ogg",
        "demo_5.8.ogg", NULL },
    [5] = {
        "demo_6.1.ogg", "demo_6.2.ogg", "demo_6.4.ogg",
        "demo_6.5.ogg", NULL },
};

/* "random N" / "playlist N" from the original playlist text members. */
static const int s_repeat[6] = {
    [MUSIC_TITLE] = 2,  /* intro.txt:     random 2 */
    [1] = 4,            /* level1.txt:    random 4 */
    [2] = 4,            /* level2.txt:    random 4 */
    [3] = 2,            /* level3.txt:    random 2 */
    [4] = 4,            /* level4.txt:    random 4 */
    [5] = 2,            /* level5.txt:    random 2 */
};

static const char *const s_endings[6] = {
    [MUSIC_TITLE] = "demo6_end.ogg",  /* intro.end */
    [1] = "demo6_end.ogg",            /* level1.end.txt */
    [2] = "lego1.end.ogg",            /* level2.end.txt */
    [3] = "demo_4.end.ogg",           /* level3.end.txt */
    [4] = "demo_5.end.ogg",           /* level4.end.txt */
    [5] = "demo_6.end.ogg",           /* level5.end.txt */
};

/* Each logical music track is mixed as one song at build time (shared pre-gain,
   peak limiter, 20Hz-20kHz EQ, make-up limiter, per-asset fader). This fader is
   the runtime bus level, separate from the build-time chain. */
static const int SFX_VOLUME = MIX_MAX_VOLUME;             /* 100% */
static const int MUSIC_VOLUME = MIX_MAX_VOLUME * 4 / 5;   /* 80% BGM bus */
/* Cursor ticks fire constantly in menus, so use a short mechanical blip at a
   fraction of the normal UI volume instead of the loud button-confirm sound. */
static const int MENU_MOVE_VOLUME = MIX_MAX_VOLUME / 3;
/* A locked menu action gets a soft negative bonk rather than a loud warning. */
static const int REJECT_VOLUME = MIX_MAX_VOLUME * 3 / 4;
#define MUSIC_QUEUE_CAP 32
#define MUSIC_REFILL_AT 5  /* matches SndCheckPlaylist's "< 5" threshold */

static Mix_Chunk *s_chunks[SFX_COUNT];
/* Every sound effect keeps the channel it is playing on. Re-triggering the
   same effect stops that channel first, while other effects keep playing. */
static int s_sfxChannel[SFX_COUNT];

/* All music is decoded once at startup. Loading a fresh Music object at a
   segment boundary was the main source of audible gaps on hardware. */
static Mix_Music *s_musicCache[JB_MUSIC_TIMING_COUNT];
static bool s_musicCacheReady[JB_MUSIC_TIMING_COUNT];
static Mix_Music *s_music = NULL;
static int s_currentTrack = -1;

typedef struct {
    int track;
} MusicQueueItem;

static MusicQueueItem s_queue[MUSIC_QUEUE_CAP];
static int s_queueHead = 0;
static int s_queueCount = 0;
static int s_currentPlaylist = -1;
static int s_pendingEnd = -1;
/* Jukebox A starts a real playlist but ends on that group's own stinger, which
   is not always the same one s_endings[] uses for the level playlist. */
static int s_pendingEndTrack = -1;
static bool s_playlistActive = false;
/* Jukebox queues explicit one-shot segments instead of refilling a playlist.
   Its loop replays the same segment from the queue each time it finishes, and
   with the loop off playback simply advances through the timing table. */
static bool s_jukeboxMode = false;
static int s_jukeboxLoopTrack = -1;
static int s_jukeboxEndPending = -1;   /* matched *.end track index, or -1 */
static bool s_jukeboxAdvance = false;  /* keep walking the table when queue empties */
/* A plays one soundtrack group in table order, never crossing into the next. */
static bool s_jukeboxGroupSequence = false;
static int s_jukeboxGroupFirst = -1;
static int s_jukeboxGroupLast = -1;
/* SDL_mixer has no position getter, so the UI timer uses a clock tied to each
   play_track_now() call. It freezes with audio_set_paused(). */
static Uint32 s_musicClockBaseMs = 0;
static Uint32 s_musicClockAccumMs = 0;
static bool s_musicClockPaused = false;
static unsigned s_musicPlaySerial = 0;
static bool s_ready = false;
static bool s_muted = false;

static void sfx_finished(int channel) {
    for (int i = 0; i < SFX_COUNT; i++)
        if (s_sfxChannel[i] == channel) s_sfxChannel[i] = -1;
}

static int find_track(const char *file) {
    if (!file) return -1;
    for (int i = 0; i < JB_MUSIC_TIMING_COUNT; i++)
        if (s_musicTiming[i].file && strcmp(s_musicTiming[i].file, file) == 0)
            return i;
    return -1;
}

static bool load_music_cache(void) {
    char path[256];
    int loaded = 0;
    for (int i = 0; i < JB_MUSIC_TIMING_COUNT; i++) {
        snprintf(path, sizeof(path), "romfs:/audio/music/%s",
                 s_musicTiming[i].file);
        s_musicCache[i] = Mix_LoadMUS(path);
        s_musicCacheReady[i] = s_musicCache[i] != NULL;
        if (s_musicCacheReady[i]) loaded++;
        else LOG("music cache FAILED: %s: %s", path, Mix_GetError());
    }
    LOG("music cache: %d/%d tracks", loaded, JB_MUSIC_TIMING_COUNT);
    return loaded > 0;
}

static void clear_queue(void) {
    s_queueHead = 0;
    s_queueCount = 0;
}

static void clear_jukebox_state(void) {
    s_jukeboxMode = false;
    s_jukeboxLoopTrack = -1;
    s_jukeboxEndPending = -1;
    s_jukeboxAdvance = false;
    s_jukeboxGroupSequence = false;
    s_jukeboxGroupFirst = -1;
    s_jukeboxGroupLast = -1;
}

static void enqueue_track(int track) {
    if (track < 0 || track >= JB_MUSIC_TIMING_COUNT || !s_musicCacheReady[track])
        return;
    if (s_queueCount >= MUSIC_QUEUE_CAP) {
        /* A deliberate jukebox action should never be silently dropped. */
        s_queue[(s_queueHead + s_queueCount - 1) % MUSIC_QUEUE_CAP].track = track;
        return;
    }
    s_queue[(s_queueHead + s_queueCount) % MUSIC_QUEUE_CAP].track = track;
    s_queueCount++;
}

static int dequeue_track(void) {
    if (s_queueCount <= 0) return -1;
    int track = s_queue[s_queueHead].track;
    s_queueHead = (s_queueHead + 1) % MUSIC_QUEUE_CAP;
    s_queueCount--;
    return track;
}

/* Take over the scheduler without interrupting the segment already playing.
   Everything queued by the old playlist is discarded so an A press in the
   jukebox means "next", not "after the whole old batch". */
static void take_over_for_jukebox(void) {
    if (s_jukeboxMode) return;
    clear_queue();
    s_pendingEnd = -1;
    s_pendingEndTrack = -1;
    s_currentPlaylist = -1;
    s_jukeboxMode = true;
    s_jukeboxAdvance = true;
    s_playlistActive = true;
}

static int track_count(int playlist) {
    int n = 0;
    while (n < 9 && s_playlists[playlist][n]) n++;
    return n;
}

/* Queue one batch exactly like SndLevelQueue: for each track line, pick a
   random segment and queue it s_repeat[playlist] times consecutively. */
static void queue_batch(int playlist) {
    int n = track_count(playlist);
    if (n <= 0) return;
    int repeats = s_repeat[playlist] > 0 ? s_repeat[playlist] : 1;
    for (int i = 0; i < n; i++) {
        int track = find_track(s_playlists[playlist][rand() % n]);
        if (track < 0 || !s_musicCacheReady[track]) continue;
        for (int j = 0; j < repeats; j++) enqueue_track(track);
    }
}

static int ending_track_for(int track) {
    if (track < 0 || track >= JB_MUSIC_TIMING_COUNT) return -1;
    if (s_musicTiming[track].oneShot) return track;
    const char *file = s_musicTiming[track].file;
    if (!file) return -1;
    if (!strncmp(file, "intro_1.", 8) || !strncmp(file, "demo6_", 6))
        return find_track("demo6_end.ogg");
    if (!strncmp(file, "lego1.", 6)) return find_track("lego1.end.ogg");
    if (!strncmp(file, "demo_4.", 7)) return find_track("demo_4.end.ogg");
    if (!strncmp(file, "demo_5.", 7)) return find_track("demo_5.end.ogg");
    if (!strncmp(file, "demo_6.", 7)) return find_track("demo_6.end.ogg");
    return -1;
}

/* Natural jukebox progression: the next cached loop segment in table order,
   skipping the one-shot stingers. */
static int next_jukebox_track(int current) {
    if (current < 0 || current >= JB_MUSIC_TIMING_COUNT) current = 0;
    for (int step = 1; step <= JB_MUSIC_TIMING_COUNT; step++) {
        int idx = (current + step) % JB_MUSIC_TIMING_COUNT;
        if (!s_musicTiming[idx].oneShot && s_musicCacheReady[idx]) return idx;
    }
    return -1;
}

static bool play_track_now(int track, bool keepScheduler) {
    if (track < 0 || track >= JB_MUSIC_TIMING_COUNT || !s_musicCacheReady[track])
        return false;
    s_currentTrack = track;
    s_music = s_musicCache[track];
    Mix_VolumeMusic(s_muted ? 0 : MUSIC_VOLUME);
    if (Mix_PlayMusic(s_music, 0) != 0) {
        LOG("Mix_PlayMusic FAILED (%s): %s",
            s_musicTiming[track].file, Mix_GetError());
        s_music = NULL;
        s_currentTrack = -1;
        if (!keepScheduler) s_playlistActive = false;
        return false;
    }
    s_musicClockBaseMs = SDL_GetTicks();
    s_musicClockAccumMs = 0;
    s_musicClockPaused = false;
    s_musicPlaySerial++;
    return true;
}

static void play_end_stinger(int playlist) {
    if (playlist < 0 || playlist > 5) return;
    int track = find_track(s_endings[playlist]);
    Mix_HaltMusic();
    s_pendingEnd = -1;
    s_playlistActive = false;
    clear_queue();
    clear_jukebox_state();
    s_currentPlaylist = playlist;
    s_music = NULL;
    s_currentTrack = -1;
    if (track < 0 || !s_musicCacheReady[track]) {
        LOG("end stinger not cached: %s", s_endings[playlist]);
        return;
    }
    if (play_track_now(track, false))
        LOG("end stinger: %s", s_endings[playlist]);
}

/* The jukebox R action: play the group's stinger once, then stop. Clearing the
   scheduler here is what keeps a queued end from firing again for later songs. */
static void play_jukebox_end(int track) {
    if (track < 0 || track >= JB_MUSIC_TIMING_COUNT || !s_musicCacheReady[track])
        return;
    Mix_HaltMusic();
    s_pendingEnd = -1;
    s_pendingEndTrack = -1;
    s_currentPlaylist = -1;
    clear_queue();
    clear_jukebox_state();
    s_playlistActive = false;
    s_music = NULL;
    s_currentTrack = -1;
    if (play_track_now(track, false))
        LOG("jukebox end stinger: %s", s_musicTiming[track].file);
}

/* Play one group-specific end stinger once, then hand playback back to the UI.
   Deliberately does not touch any jukebox selection state: that lives in main.c
   and must not follow the green "now playing" cursor. */
static void play_group_end(int track) {
    if (track < 0 || track >= JB_MUSIC_TIMING_COUNT || !s_musicCacheReady[track])
        return;
    Mix_HaltMusic();
    s_pendingEnd = -1;
    s_pendingEndTrack = -1;
    s_playlistActive = false;
    clear_queue();
    clear_jukebox_state();
    s_music = NULL;
    s_currentTrack = -1;
    if (play_track_now(track, false))
        LOG("jukebox group end: %s", s_musicTiming[track].file);
}

static void play_next_segment(void) {
    if (s_jukeboxMode) {
        if (s_jukeboxEndPending >= 0) {
            play_jukebox_end(s_jukeboxEndPending);
            return;
        }
        if (s_queueCount == 0) {
            if (s_jukeboxLoopTrack >= 0) {
                enqueue_track(s_jukeboxLoopTrack);
            } else if (s_jukeboxGroupSequence) {
                /* Same random-N batch the level playlist uses, but this
                   playlist only names this group's segments. */
                queue_batch(s_currentPlaylist);
            } else if (s_jukeboxAdvance) {
                int next = next_jukebox_track(s_currentTrack);
                if (next >= 0) enqueue_track(next);
            }
        }
        int track = dequeue_track();
        if (track < 0) {
            s_playlistActive = false;
            return;
        }
        if (s_jukeboxGroupSequence && s_queueCount < MUSIC_REFILL_AT)
            queue_batch(s_currentPlaylist);
        if (!play_track_now(track, true)) s_playlistActive = false;
        return;
    }

    /* A pending end waits for the current multi-measure segment to finish. */
    if (s_pendingEndTrack >= 0) {
        play_group_end(s_pendingEndTrack);
        return;
    }
    if (s_pendingEnd >= 0) {
        play_end_stinger(s_pendingEnd);
        return;
    }
    if (s_queueCount == 0) queue_batch(s_currentPlaylist);
    if (s_queueCount == 0) {
        s_playlistActive = false;
        return;
    }
    int track = dequeue_track();
    if (s_queueCount < MUSIC_REFILL_AT) queue_batch(s_currentPlaylist);
    if (!play_track_now(track, true)) s_playlistActive = false;
}

bool audio_init(void) {
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) != 0) {
        fprintf(stderr, "Mix_OpenAudio failed: %s\n", Mix_GetError());
        return false;
    }
    for (int i = 0; i < SFX_COUNT; i++) s_sfxChannel[i] = -1;
    Mix_ChannelFinished(sfx_finished);
    Mix_Volume(-1, SFX_VOLUME);
    Mix_VolumeMusic(MUSIC_VOLUME);
    load_music_cache();
    s_ready = true;
    s_muted = false;
    return true;
}

void audio_exit(void) {
    audio_music_stop();
    Mix_ChannelFinished(NULL);
    for (int i = 0; i < SFX_COUNT; i++) {
        if (s_chunks[i]) Mix_FreeChunk(s_chunks[i]);
        s_chunks[i] = NULL;
        s_sfxChannel[i] = -1;
    }
    for (int i = 0; i < JB_MUSIC_TIMING_COUNT; i++) {
        if (s_musicCache[i]) Mix_FreeMusic(s_musicCache[i]);
        s_musicCache[i] = NULL;
        s_musicCacheReady[i] = false;
    }
    s_music = NULL;
    if (s_ready) Mix_CloseAudio();
    s_ready = false;
}

void audio_play(SoundID id) {
    if (!s_ready || s_muted || id < 0 || id >= SFX_COUNT) return;
    if (!s_chunks[id]) {
        s_chunks[id] = Mix_LoadWAV(s_paths[id]);
        if (!s_chunks[id]) return;
        if (id == SFX_MENU_MOVE) Mix_VolumeChunk(s_chunks[id], MENU_MOVE_VOLUME);
        if (id == SFX_REJECT) Mix_VolumeChunk(s_chunks[id], REJECT_VOLUME);
    }
    /* One instance of each unique effect: stop the previous one first. */
    int old = s_sfxChannel[id];
    if (old >= 0) Mix_HaltChannel(old);
    int ch = Mix_PlayChannel(-1, s_chunks[id], 0);
    s_sfxChannel[id] = ch;
}

void audio_music_start(int playlist) {
    if (!s_ready) return;
    if (playlist == -1)
        playlist = MUSIC_LEVEL_MIN + rand() % (MUSIC_LEVEL_MAX - MUSIC_LEVEL_MIN + 1);
    if (playlist < 0 || playlist > 5) return;
    audio_music_stop();
    s_currentPlaylist = playlist;
    s_playlistActive = true;
    queue_batch(playlist);
    play_next_segment();
    LOG("music playlist started: %d", playlist);
}

void audio_music_stop(void) {
    if (!s_ready) return;
    Mix_HaltMusic();
    s_music = NULL;                 /* owned by s_musicCache, never freed here */
    s_currentTrack = -1;
    s_currentPlaylist = -1;
    s_pendingEnd = -1;
    s_pendingEndTrack = -1;
    s_playlistActive = false;
    s_musicClockBaseMs = SDL_GetTicks();
    s_musicClockAccumMs = 0;
    s_musicClockPaused = false;
    clear_queue();
    clear_jukebox_state();
}

void audio_music_request_end(int playlist) {
    if (!s_ready || playlist < 0 || playlist > 5) return;
    if (!s_playlistActive) return;
    if (s_pendingEnd >= 0) return;
    s_pendingEnd = playlist;
    LOG("music end requested for playlist %d (after current loop)", playlist);
}

void audio_music_wrap_up(void) {
    if (!s_ready || !s_playlistActive) return;
    /* Already at the end position: let the stinger finish and stop instead of
       queueing another one or restarting a loop behind it. */
    if (s_currentTrack >= 0 && s_musicTiming[s_currentTrack].oneShot) return;
    if (s_pendingEnd >= 0 || s_pendingEndTrack >= 0 ||
        s_jukeboxEndPending >= 0) return;

    if (s_jukeboxMode) {
        int end = s_pendingEndTrack >= 0 ? s_pendingEndTrack
                                         : ending_track_for(s_currentTrack);
        /* Stop the group sequence, the table walk and any loop before the
           stinger; otherwise playback would resume behind the end. */
        s_jukeboxAdvance = false;
        s_jukeboxGroupSequence = false;
        s_jukeboxLoopTrack = -1;
        clear_queue();
        if (end >= 0) {
            s_jukeboxEndPending = end;
            LOG("jukebox wrap-up: end %s", s_musicTiming[end].file);
        } else {
            LOG("jukebox wrap-up: no end stinger, stop after current segment");
        }
        return;
    }

    if (s_currentPlaylist >= 0) {
        s_pendingEnd = s_currentPlaylist;
        clear_queue();
        LOG("music wrap-up: end requested for playlist %d", s_currentPlaylist);
    }
}

void audio_tick(void) {
    if (!s_ready || !s_playlistActive) return;
    /* Poll instead of relying on the mixer callback: the next cached track
       starts in the same frame the previous one finishes. */
    if (Mix_PlayingMusic()) return;
    play_next_segment();
}

int audio_music_track_count(void) { return JB_MUSIC_TIMING_COUNT; }

const char *audio_music_track_name(int track) {
    if (track < 0 || track >= JB_MUSIC_TIMING_COUNT) return "";
    return s_musicTiming[track].file;
}

double audio_music_track_bpm(int track) {
    if (track < 0 || track >= JB_MUSIC_TIMING_COUNT) return 0.0;
    return s_musicTiming[track].bpm;
}

int audio_music_current_track(void) { return s_currentTrack; }

void audio_jukebox_queue_track(int track) {
    if (!s_ready || track < 0 || track >= JB_MUSIC_TIMING_COUNT ||
        !s_musicCacheReady[track])
        return;
    take_over_for_jukebox();
    enqueue_track(track);
    LOG("jukebox queued: %s", s_musicTiming[track].file);
    if (!Mix_PlayingMusic()) play_next_segment();
}

/* Reload one cached track from disk so its decoder state is brand new. Used
   when A starts a group: the old song must be completely gone from memory
   before the new playlist is initialised, not merely stopped. */
static void refresh_music_cache(int track) {
    char path[256];
    snprintf(path, sizeof(path), "romfs:/audio/music/%s",
             s_musicTiming[track].file);
    Mix_Music *fresh = Mix_LoadMUS(path);
    if (!fresh) {
        LOG("jukebox refresh FAILED: %s: %s", path, Mix_GetError());
        return;
    }
    if (s_musicCache[track]) Mix_FreeMusic(s_musicCache[track]);
    s_musicCache[track] = fresh;
    s_musicCacheReady[track] = true;
}

void audio_jukebox_play_group(int first, int last, int playlist) {
    if (!s_ready || first < 0 || last < first ||
        last >= JB_MUSIC_TIMING_COUNT || playlist < 0 || playlist > 5)
        return;

    /* A means "play this soundtrack" with the same sequencer as the game:
       start at the group's first segment, then `random N` over that group's
       playlist lines with refill-at-5, forever. The playlist only names this
       group's segments, so nothing can bleed in from a neighbouring set. */
    audio_music_stop();
    /* audio_music_stop() clears the scheduler and detaches the active music;
       also reload the first segment so no decoder state from the previous song
       survives into it. */
    refresh_music_cache(first);
    s_jukeboxMode = true;
    s_jukeboxAdvance = true;
    s_jukeboxGroupSequence = true;
    s_jukeboxGroupFirst = first;
    s_jukeboxGroupLast = last;
    s_currentPlaylist = playlist;
    s_playlistActive = true;
    enqueue_track(first);
    queue_batch(playlist);
    int track = dequeue_track();
    if (track < 0 || !play_track_now(track, true)) {
        s_playlistActive = false;
        return;
    }
    LOG("jukebox playlist %d from %s", playlist, s_musicTiming[track].file);
}

bool audio_jukebox_toggle_loop(int track) {
    if (!s_ready || track < 0 || track >= JB_MUSIC_TIMING_COUNT ||
        !s_musicCacheReady[track] || s_musicTiming[track].oneShot)
        return false;
    bool wasLooping = s_jukeboxLoopTrack == track;
    /* L means "repeat this exact segment": restart from the top of the clip so
       the first play and every replay are the same single track. */
    audio_music_stop();
    if (wasLooping) {
        LOG("jukebox loop off: %s", s_musicTiming[track].file);
        return false;
    }
    s_jukeboxMode = true;
    s_jukeboxAdvance = false;
    s_jukeboxLoopTrack = track;
    s_playlistActive = true;
    if (!play_track_now(track, true)) {
        s_playlistActive = false;
        return false;
    }
    LOG("jukebox loop on: %s", s_musicTiming[track].file);
    return true;
}

int audio_jukebox_loop_track(void) { return s_jukeboxLoopTrack; }

bool audio_jukebox_looping(void) { return s_jukeboxLoopTrack >= 0; }

void audio_jukebox_queue_end(int track) {
    int end = ending_track_for(track);
    if (!s_ready || end < 0 || !s_musicCacheReady[end]) return;
    if (!s_jukeboxMode) take_over_for_jukebox();
    /* Nothing may play between the current segment and its end stinger, and
       the pending end is cleared once it plays (see play_jukebox_end). */
    clear_queue();
    s_jukeboxEndPending = end;
    LOG("jukebox end queued: %s", s_musicTiming[end].file);
    if (!Mix_PlayingMusic()) play_next_segment();
}

bool audio_jukebox_end_queued(void) { return s_jukeboxEndPending >= 0; }

void audio_jukebox_stop(void) {
    if (!s_ready) return;
    audio_music_stop();
    LOG("jukebox stopped: sound halted and queue cleared");
}

void audio_music_end_stinger(int playlist) {
    if (!s_ready || playlist < 0 || playlist > 5) return;
    play_end_stinger(playlist);
}

int audio_current_playlist(void) { return s_currentPlaylist; }

unsigned audio_music_play_serial(void) { return s_musicPlaySerial; }

unsigned audio_music_position_ms(void) {
    if (!s_ready || s_currentTrack < 0 || !Mix_PlayingMusic()) return 0;
    Uint32 elapsed = s_musicClockPaused
                         ? s_musicClockAccumMs
                         : s_musicClockAccumMs + (SDL_GetTicks() - s_musicClockBaseMs);
    return (unsigned)elapsed;
}

unsigned audio_music_track_ms(int track) {
    if (track < 0 || track >= JB_MUSIC_TIMING_COUNT) return 0;
    const JBMusicTiming *t = &s_musicTiming[track];
    int samples = t->end - t->start;   /* build trims OGG to exactly [start,end) */
    if (samples <= 0) return 0;
    return (unsigned)(((double)samples * 1000.0) / 44100.0 + 0.5);
}

int audio_jukebox_next_track(void) {
    if (s_queueCount > 0) return s_queue[s_queueHead].track;
    if (s_jukeboxLoopTrack >= 0) return s_jukeboxLoopTrack;
    if (s_jukeboxGroupSequence) return s_jukeboxGroupFirst;
    return -1;
}

void audio_set_paused(bool paused) {
    if (!s_ready) return;
    if (paused) {
        if (!s_musicClockPaused) {
            s_musicClockAccumMs += SDL_GetTicks() - s_musicClockBaseMs;
            s_musicClockPaused = true;
        }
        Mix_PauseMusic();
        Mix_Pause(-1);
    } else {
        if (s_musicClockPaused) {
            s_musicClockBaseMs = SDL_GetTicks();
            s_musicClockPaused = false;
        }
        Mix_ResumeMusic();
        Mix_Resume(-1);
    }
}

void audio_set_muted(bool muted) {
    s_muted = muted;
    if (!s_ready) return;
    Mix_Volume(-1, muted ? 0 : SFX_VOLUME);
    Mix_VolumeMusic(muted ? 0 : MUSIC_VOLUME);
}

bool audio_is_muted(void) { return s_muted; }
