#ifndef JUNKBOT_MUSIC_TIMING_H
#define JUNKBOT_MUSIC_TIMING_H

#include <stdbool.h>

/* Hand-editable music timing table.
 *
 * All positions are samples in a 44.1 kHz, 16-bit MONO timeline. The build
 * script trims and re-encodes each OGG to exactly [start, end) at 44.1 kHz and
 * plays it cached with no loop tags. (The local FFmpeg Vorbis encoder only
 * accepts two channels, so the file is duplicated to stereo; the timeline is
 * unchanged.)
 *
 * For normal playlist segments:
 *   start     = first audible sample after the source's leading silence
 *   loopStart = start (the loop begins immediately after the silence is cut)
 *   measures  = number of 4/4 bars in one loop (demo_4.1 is the 2-bar exception)
 *   end       = loopStart + measures * 4 * 60 / bpm samples
 *               (anacrusisBeats is reserved for future pickup tuning and is
 *               currently 0 for every track)
 * `bpm` and `measures` are authoritative and BPM accepts up to five decimal
 * places. `end` is derived from them at build time, so changing either one is
 * enough; rebuild to re-encode the trimmed OGG. To keep this table
 * self-documenting, run:
 *   python3 tools/music_timing.py recompute
 * or change one track directly:
 *   python3 tools/music_timing.py set demo_4.1 130.12345
 *   python3 tools/music_timing.py set-measures demo_4.1 2
 * `end` is exclusive. Anything after it in the source was the original's
 * baked-in trailing silence or an extra measure, so it is discarded.
 *
 * End stingers are one-shots: `oneShot = true` means trim only the outer
 * silence and play once after the current loop finishes. They keep BPM 0 and
 * measures 0, and use their hand-trimmed `end` unchanged.
 */

typedef struct {
    const char *file;       /* romfs audio filename */
    double bpm;             /* decimal BPM; 0 for one-shot stingers */
    int anacrusisBeats;     /* pickup beats before the loop, currently 0 */
    int measures;           /* 4/4 bars per loop; 0 for one-shot stingers */
    int start;              /* first audible sample @44.1k */
    int loopStart;          /* loop start @44.1k */
    int end;                /* exclusive loop/trim end @44.1k */
    bool oneShot;           /* end stinger, played once */
} JBMusicTiming;

#define JB_MUSIC_TIMING_COUNT 26

static const JBMusicTiming s_musicTiming[JB_MUSIC_TIMING_COUNT] = {
    { "intro_1.1.ogg",     110.56000, 0, 1,      0,      0,  95351, false },
    { "intro_1.2.ogg",     110.57000, 0, 1,      0,      0,  95351, false },
    { "intro_1.3.ogg",     110.53400, 0, 1,      0,      0,  95351, false },

    { "demo6_3.ogg",       110.67300, 0, 1,   2602,   2602,  97953, false },
    { "demo6_5.ogg",       110.65800, 0, 1,   2558,   2558,  97909, false },
    { "demo6_6.ogg",       110.63500, 0, 1,   3219,   3219,  98570, false },
    { "demo6_end.ogg",       0.00000, 0, 0,   2602,   2602,  61608, true  },

    { "lego1.1.ogg",       100.98000, 0, 1,   3043,   3043, 108883, false },
    { "lego1.2.ogg",       100.98000, 0, 1,   3087,   3087, 108927, false },
    { "lego1.3.ogg",       100.98000, 0, 1,   3087,   3087, 108927, false },
    { "lego1.end.ogg",       0.00000, 0, 0,   3043,   3043,  96800, true  },

    { "demo_4.1.ogg",      130.90000, 0, 2,   3175,   3175,  166006, false },
    { "demo_4.3.ogg",      130.99000, 0, 1,   3131,   3131,  84546, false },
    { "demo_4.5.ogg",      130.99000, 0, 1,   3175,   3175,  84590, false },
    { "demo_4.6.ogg",      130.90000, 0, 1,   3131,   3131,  84546, false },
    { "demo_4.end.ogg",      0.00000, 0, 0,   3087,   3087,  65444, true  },

    { "demo_5.1.ogg",      110.75500, 0, 1,   3087,   3087,  99305, false },
    { "demo_5.2.ogg",      110.7810, 0, 1,   2955,   2955,  99173, false },
    { "demo_5.7.ogg",      110.8010, 0, 1,   2690,   2690,  98908, false },
    { "demo_5.8.ogg",      110.8810, 0, 1,   2955,   2955,  99173, false },
    { "demo_5.end.ogg",      0.00000, 0, 0,   2911,   2911,  71486, true  },

    { "demo_6.1.ogg",       96.89000, 0, 1,   2690,   2690, 112940, false },
    { "demo_6.2.ogg",       96.89000, 0, 1,   2911,   2911, 113161, false },
    { "demo_6.4.ogg",       96.89000, 0, 1,   2955,   2955, 113205, false },
    { "demo_6.5.ogg",       96.89000, 0, 1,   3087,   3087, 113337, false },
    { "demo_6.end.ogg",      0.00000, 0, 0,   2955,   2955,  55301, true  },
};

#endif
