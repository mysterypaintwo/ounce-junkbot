#ifndef JUNKBOT_AUDIO_GAIN_H
#define JUNKBOT_AUDIO_GAIN_H

/* Hand-editable per-asset gain offsets (dB).
 *
 * At build time SFX are peak-mastered and each logical music track is mixed as
 * one song (shared pre-gain, peak limiter, 20Hz-20kHz EQ, make-up limiter) before
 * being split into runtime segments. This gain is the per-asset BGM fader after
 * that chain, at the 0 dBFS ceiling (-0.1 dB encoder headroom). 0 dB keeps the
 * asset at that ceiling. Negative values make one asset quieter relative to the
 * others; positive values are clamped to 0 dB so no asset clips.
 *
 * Runtime buses are separate: SFX plays at 100% and music at 80%, with a master
 * soft limiter protecting coincident peaks.
 *
 * Edit with: python3 tools/audio_gain.py set voice_ouch.wav -3.5
 * List with: python3 tools/audio_gain.py list
 */

typedef struct {
    const char *file;       /* romfs filename */
    float gainDb;           /* trim after mastering, in dB */
} JBAudioGain;

#define JB_AUDIO_GAIN_COUNT 55

static const JBAudioGain s_audioGains[JB_AUDIO_GAIN_COUNT] = {
    /* Sound effects (SFX bus: 100%). */
    { "turn1.wav",           -3.00f },   /* Junkbot wall turn */
    { "blockpickup.wav",      0.00f },
    { "blockdrop.wav",        0.00f },
    { "blockclick.wav",       0.00f },
    { "h_button1.wav",        -2.00f },
    { "fall.wav",             0.00f },
    { "headbonk1.wav",        0.00f },
    { "eat1.wav",             -2.00f },
    { "garbage1.wav",         -2.00f },
    { "switch_click.wav",     0.00f },
    { "switch_on.wav",        0.00f },
    { "switch_off.wav",       0.00f },
    { "fire.wav",             0.00f },
    { "electricity1.wav",     -1.50f },
    { "robottouch4.wav",      0.00f },
    { "shieldon2.wav",        0.00f },
    { "h_powerup1.wav",       0.00f },
    { "h_powerdown3.wav",     0.00f },
    { "h_powerup3.wav",       -3.00f },   /* original tabSwitch */
    { "voice_ohyeah.wav",     0.00f },
    { "voice_ouch.wav",       -2.00f },
    { "voice_uhoh.wav",       -2.00f },
    { "jump3.wav",            0.00f },
    { "fan.wav",              0.00f },
    { "drip1.wav",            -2.00f },
    { "drip2.wav",            -2.00f },
    { "drip3.wav",            -2.00f },
    { "undercover/laser_hit.wav", 0.00f },
    { "undercover/teleport.wav",  0.00f },

    /* Music segments (music bus: 80%). */
    { "intro_1.1.ogg",        0.00f },
    { "intro_1.2.ogg",        0.00f },
    { "intro_1.3.ogg",        0.00f },
    { "demo6_3.ogg",          0.00f },
    { "demo6_5.ogg",          0.00f },
    { "demo6_6.ogg",          0.00f },
    { "demo6_end.ogg",        0.00f },
    { "lego1.1.ogg",          0.00f },
    { "lego1.2.ogg",          0.00f },
    { "lego1.3.ogg",          0.00f },
    { "lego1.end.ogg",        0.00f },
    { "demo_4.1.ogg",         0.00f },
    { "demo_4.3.ogg",         0.00f },
    { "demo_4.5.ogg",         0.00f },
    { "demo_4.6.ogg",         0.00f },
    { "demo_4.end.ogg",       0.00f },
    { "demo_5.1.ogg",         0.00f },
    { "demo_5.2.ogg",         0.00f },
    { "demo_5.7.ogg",         0.00f },
    { "demo_5.8.ogg",         0.00f },
    { "demo_5.end.ogg",       0.00f },
    { "demo_6.1.ogg",         0.00f },
    { "demo_6.2.ogg",         0.00f },
    { "demo_6.4.ogg",         0.00f },
    { "demo_6.5.ogg",         0.00f },
    { "demo_6.end.ogg",       0.00f },
};

#endif
