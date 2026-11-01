/* Sound: a stand-in for the ARM7's NitroSDK sound driver.
 *
 * The ARM9 sends lists of SNDCommand (next, id, arg[4]) over PXI tag 7. Sequences (SSEQ) play from the sequence data,
 * instrument bank (SBNK) and wave archives (SWAR) the game has loaded into main RAM; the bank's wave-archive links
 * point at those archives. Direct channel commands (cries, streams) play samples the same way the hardware channels
 * would. Everything is mixed to 32768 Hz stereo, one video frame at a time.
 *
 * Command layouts were read from what the game sends (B2W2_LOG_SND=1), e.g. SETUP_CHANNEL_PCM:
 *   arg0 = channel | timer << 16, arg1 = data, arg2 = loop length (words) | shift << 22 | volume << 24,
 *   arg3 = loop start (words) | pan << 16 | format << 24 | loop mode << 26
 * Sequencer behaviour (event set, envelope curves, tempo of 240 ticks per beat counter at 192 Hz) follows the SDAT
 * format as documented by the community (VGMTrans, SSEQPlayer). Volumes are in tenths of a decibel like the SDK's. */
#include "internal.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#define RATE 32768
#define SEQ_HZ 192.0                     /* the driver's update rate: every 64 * 2728 bus cycles */
#define ENV_MIN (-92544)                 /* -723 dB*10 in the envelope's 1/128 steps */

/* ------------------------------------------------------------------ tables */

static int db_sq(int v) {                /* volume 0-127 -> tenths of a dB, squared curve like SNDi_DecibelSquareTable */
    if (v <= 0) return -723;
    if (v >= 127) return 0;
    int d = (int)lround(400.0 * log10(v / 127.0));
    return d < -723 ? -723 : d;
}

static int attack_rate(int a) {
    static const uint8_t t[19] = {0x00, 0x01, 0x05, 0x0e, 0x1a, 0x26, 0x33, 0x3f, 0x49, 0x54, 0x5c, 0x64, 0x6d, 0x74,
                                  0x7b, 0x7f, 0x84, 0x89, 0x8f};
    return a >= 0x6d ? t[0x7f - a] : 0xff - a;
}
static int fall_rate(int f) {
    if (f == 0x7f) return 0xffff;
    if (f == 0x7e) return 0x3c00;
    if (f < 0x32) return f * 2 + 1;
    return 0x1e00 / (0x7e - f);
}
static int sustain_level(int s) { return s == 127 ? 0 : db_sq(s) * 128; }

/* ------------------------------------------------------------------ state */

typedef struct {
    int active;
    int kind;                            /* 0 PCM, 1 PSG, 2 noise */
    int format;                          /* PCM: 0 8-bit, 1 16-bit, 2 IMA-ADPCM */
    int loop;                            /* loop mode: 1 repeat, 2 one-shot */
    uint32_t data;                       /* first sample byte (after the ADPCM header) */
    uint32_t loop_start, length;         /* in samples */
    double pos, step;
    int duty;                            /* PSG duty 0-7 */
    uint16_t lfsr;
    int adpcm_sample, adpcm_index, adpcm_pos;      /* decoder state at adpcm_pos */
    int loop_sample, loop_index, loop_saved;
    float gain_l, gain_r;
    /* sequencer voices */
    int track, player, note, base_note, prio;
    int velocity_db, env_state, env, attack, decay, sustain, release;
    int length_ticks;                    /* remaining note length in sequence ticks, -1 = held (tie) */
    int pan;
    double timer_base;                   /* sample rate at the base note */
    double sweep, sweep_step;            /* portamento: semitones left, change per tick */
    int lfo_delay, lfo_phase;
} Voice;

typedef struct {
    int active, player;
    uint32_t pc;
    uint32_t stack[3];
    int loops[3], sp;
    int wait;
    int program, volume, expression, pan, transpose, bend, bend_range, prio;
    int attack, decay, sustain, release;
    int note_wait, tie, porta, porta_key, porta_time, sweep;
    int mod_depth, mod_speed, mod_type, mod_range, mod_delay;
    int cond, muted;
    Voice *tied;
} Track;

typedef struct {
    int active, prepared, paused;
    uint32_t base, bank;
    int tempo, tempo_ratio, counter;
    int volume_db, seq_volume, prio;
    int tracks[16], ntracks;
    uint32_t ticks;
} Player;

static Voice voices[64];
static Voice channels[16];               /* channels the game drives directly */
static int ch_running;                   /* START_TIMER mask */
static Track tracks[64];
static Player players[16];
static int master_volume = 127;
static uint32_t shared;
static struct { int tick, period, id, count, on; } alarms[8];
static double seq_clock;

void snd_set_shared(uint32_t a) { shared = a; }

/* Sequence and bank data is a byte stream: its halfwords can sit at odd addresses, where rd16 (like the bus) would
 * read the aligned halfword instead */
static uint16_t rd16u(uint32_t a) { return (uint16_t)(rd8(a) | rd8(a + 1) << 8); }

/* ------------------------------------------------------------------ sample sources */

static void adpcm_step(Voice *v, uint8_t nib) {
    static const int idx_tab[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    static const int steps[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767};
    int s = steps[v->adpcm_index], d = s >> 3;
    if (nib & 1) d += s >> 2;
    if (nib & 2) d += s >> 1;
    if (nib & 4) d += s;
    v->adpcm_sample += nib & 8 ? -d : d;
    if (v->adpcm_sample > 32767) v->adpcm_sample = 32767;     /* the DS clamps to +-0x7fff */
    if (v->adpcm_sample < -32767) v->adpcm_sample = -32767;
    v->adpcm_index += idx_tab[nib & 7];
    if (v->adpcm_index < 0) v->adpcm_index = 0;
    if (v->adpcm_index > 88) v->adpcm_index = 88;
}

static int fetch(Voice *v, uint32_t i) {
    switch (v->format) {
    case 0: return (int8_t)rd8(v->data + i) * 256;
    case 1: return (int16_t)rd16(v->data + i * 2);
    default:
        if ((int)i < v->adpcm_pos - 1) {
            /* went back: to the loop point if its decoder state is known, else to the header */
            if (v->loop_saved && i >= v->loop_start) {
                v->adpcm_sample = v->loop_sample; v->adpcm_index = v->loop_index; v->adpcm_pos = (int)v->loop_start;
            } else {
                v->adpcm_sample = (int16_t)rd16(v->data - 4); v->adpcm_index = rd8(v->data - 2) & 0x7f;
                v->adpcm_pos = 0;
            }
        }
        while ((uint32_t)v->adpcm_pos <= i) {
            if ((uint32_t)v->adpcm_pos == v->loop_start && !v->loop_saved) {
                v->loop_sample = v->adpcm_sample; v->loop_index = v->adpcm_index; v->loop_saved = 1;
            }
            uint8_t b = rd8(v->data + (uint32_t)v->adpcm_pos / 2);
            adpcm_step(v, v->adpcm_pos & 1 ? b >> 4 : b & 15);
            v->adpcm_pos++;
        }
        return v->adpcm_sample;
    }
}

static int next_sample(Voice *v) {
    if (v->kind == 1) {                                  /* PSG square: 8 steps a period */
        int phase = (int)v->pos & 7;
        v->pos += v->step;
        return phase <= v->duty ? 0x7fff : -0x7fff;
    }
    if (v->kind == 2) {                                  /* noise: 15-bit LFSR */
        double before = v->pos;
        v->pos += v->step;
        for (int n = (int)v->pos - (int)before; n > 0; n--) {
            int bit = v->lfsr & 1;
            v->lfsr >>= 1;
            if (bit) v->lfsr ^= 0x6000;
        }
        return v->lfsr & 1 ? -0x7fff : 0x7fff;
    }
    uint32_t i = (uint32_t)v->pos;
    if (i >= v->length) {
        if (v->loop != 1 || v->length <= v->loop_start) { v->active = 0; return 0; }
        double over = v->pos - v->length;
        v->pos = v->loop_start + fmod(over, (double)(v->length - v->loop_start));
        i = (uint32_t)v->pos;
    }
    int s = fetch(v, i);
    v->pos += v->step;
    return s;
}

/* ------------------------------------------------------------------ banks */

typedef struct { int type, wave, archive, base_note, attack, decay, sustain, release, pan; } Inst;

static int read_inst_params(uint32_t p, int type, Inst *in) {
    in->type = type;
    in->wave = rd16u(p); in->archive = rd16u(p + 2); in->base_note = rd8(p + 4);
    in->attack = rd8(p + 5); in->decay = rd8(p + 6); in->sustain = rd8(p + 7); in->release = rd8(p + 8);
    in->pan = rd8(p + 9);
    return type >= 1 && type <= 4;
}

static int find_inst(uint32_t bank, int program, int note, Inst *in) {
    if (!bank || program >= (int)rd32(bank + 0x38)) return 0;
    uint32_t rec = rd32(bank + 0x3c + 4 * (uint32_t)program);
    int type = rec & 0xff;
    uint32_t p = bank + ((rec >> 8) & 0xffff);
    if (type >= 1 && type <= 4) return read_inst_params(p, type, in);
    if (type == 16) {                                    /* drum set: a range of notes, one instrument each */
        int lo = rd8(p), hi = rd8(p + 1);
        if (note < lo || note > hi) return 0;
        uint32_t e = p + 2 + (uint32_t)(note - lo) * 12;
        return read_inst_params(e + 2, rd16u(e), in);
    }
    if (type == 17) {                                    /* key split: up to 8 ranges */
        int i = 0;
        while (i < 8 && rd8(p + (uint32_t)i) && note > rd8(p + (uint32_t)i)) i++;
        if (i == 8 || !rd8(p + (uint32_t)i)) return 0;
        uint32_t e = p + 8 + (uint32_t)i * 12;
        return read_inst_params(e + 2, rd16u(e), in);
    }
    return 0;
}

/* ------------------------------------------------------------------ voices */

/* The DS has 16 channels, shared with the ones the game drives directly; when all are busy the driver takes the
 * quietest released note, else the lowest priority one. Without this limit release tails pile up and the mix clips. */
static Voice *alloc_voice(int prio) {
    Voice *best = NULL, *free_v = NULL;
    int busy = 0;
    for (int c = 0; c < 16; c++) busy += channels[c].active && (ch_running & (1 << c));
    for (int i = 0; i < 64; i++) {
        Voice *v = &voices[i];
        if (!v->active) { if (!free_v) free_v = v; continue; }
        busy++;
        if (v->env_state == 3 && (!best || best->env_state != 3 || v->env < best->env)) best = v;
        else if (!best || (best->env_state != 3 && v->prio < best->prio)) best = v;
    }
    if (busy < 16 && free_v) return free_v;
    if (best && (best->env_state == 3 || best->prio <= prio)) return best;
    return NULL;
}

static double note_rate(const Voice *v, double semis) { return v->timer_base * pow(2.0, semis / 12.0); }

static void note_on(Track *t, Player *pl, int note, int velocity, int length) {
    Inst in;
    if (!find_inst(pl->bank, t->program, note, &in)) return;
    if (t->tie && t->tied && t->tied->active && t->tied->track == (int)(t - tracks)) {
        Voice *v = t->tied;                              /* tie: keep the voice, change its note */
        v->sweep = t->porta ? v->note - note + v->sweep : 0;
        v->sweep_step = v->sweep / (t->porta_time > 0 ? t->porta_time : 1);
        v->note = note;
        v->velocity_db = db_sq(velocity);
        v->length_ticks = length;
        return;
    }
    Voice *v = alloc_voice(t->prio + pl->prio);
    if (!v) return;
    memset(v, 0, sizeof *v);
    v->track = (int)(t - tracks);
    v->player = t->player;
    v->note = note;
    v->base_note = in.base_note;
    v->prio = t->prio + pl->prio;
    v->velocity_db = db_sq(velocity);
    v->length_ticks = t->tie ? -1 : length;
    v->attack = attack_rate(t->attack != 0xff ? t->attack : in.attack);
    v->decay = fall_rate(t->decay != 0xff ? t->decay : in.decay);
    v->sustain = sustain_level(t->sustain != 0xff ? t->sustain : in.sustain);
    v->release = fall_rate(t->release != 0xff ? t->release : in.release);
    v->env = ENV_MIN;
    v->pan = in.pan;
    v->lfo_delay = t->mod_delay;
    if (in.type == 1 || in.type == 4) {
        uint32_t arc = rd32(pl->bank + 0x18 + 8 * (uint32_t)in.archive);
        if (!arc || (uint32_t)in.wave >= rd32(arc + 0x38)) return;
        uint32_t w = arc + rd32(arc + 0x3c + 4 * (uint32_t)in.wave);
        v->format = rd8(w);
        v->loop = rd8(w + 1) ? 1 : 2;
        uint16_t timer = rd16(w + 4);
        uint32_t loop_words = rd16(w + 6), len_words = rd32(w + 8);
        int per_word = v->format == 0 ? 4 : v->format == 1 ? 2 : 8;
        v->data = w + 12;
        if (v->format == 2) {                            /* ADPCM: 4-byte header before the nibbles */
            v->data += 4;
            v->adpcm_sample = (int16_t)rd16(w + 12);
            v->adpcm_index = rd8(w + 14) & 0x7f;
            loop_words = loop_words ? loop_words - 1 : 0;
        }
        v->loop_start = loop_words * (uint32_t)per_word;
        v->length = v->loop_start + len_words * (uint32_t)per_word;
        v->timer_base = timer ? 16756991.0 / timer : 32768;
        v->kind = 0;
    } else if (in.type == 2) {
        v->kind = 1;
        v->duty = in.wave & 7;
        v->timer_base = 440.0 * 8;                       /* the square's 8 steps at A4 */
        v->base_note = 69;
    } else {
        v->kind = 2;
        v->lfsr = 0x7fff;
        v->timer_base = 440.0 * 8;
        v->base_note = 69;
    }
    if (t->porta) {
        v->sweep = (t->porta_key - note) + t->sweep / 64.0;
        int time = t->porta_time ? t->porta_time : length;
        v->sweep_step = time > 0 ? v->sweep / time : v->sweep;
        t->porta_key = note;
    } else if (t->sweep) {
        v->sweep = t->sweep / 64.0;
        v->sweep_step = length > 0 ? v->sweep / length : 0;
    }
    v->active = 1;
    if (t->tie) t->tied = v;
    {   /* B2W2_LOG_NOTES=from-to: the notes started between two frames */
        static int lo = -2, hi, count;
        if (lo == -2) { const char *e = getenv("B2W2_LOG_NOTES"); lo = -1; if (e) sscanf(e, "%d-%d", &lo, &hi); }
        if (lo >= 0 && (int)rc_frames >= lo && (int)rc_frames < hi && count++ < 400)
            rc_logf("note p%d t%d prog %d type %d kind %d fmt %d note %d base %d len %d vel %d rate %.0f loop %d %u/%u "
                    "vol %d expr %d pan %d bend %d/%d mod %d/%d/%d/%d sweep %d porta %d tie %d A%d D%d S%d R%d\n",
                    t->player, (int)(t - tracks), t->program, in.type, v->kind, v->format, note, v->base_note, length,
                    velocity, v->timer_base, v->loop, v->loop_start, v->length, t->volume, t->expression, t->pan,
                    t->bend, t->bend_range, t->mod_type, t->mod_depth, t->mod_speed, t->mod_range, t->sweep, t->porta,
                    t->tie, in.attack, in.decay, in.sustain, in.release);
    }
}

/* Volume, pan and pitch from the voice's track and player, once a driver frame */
static void update_voice(Voice *v) {
    Track *t = &tracks[v->track];
    Player *pl = &players[v->player];
    switch (v->env_state) {                              /* ADSR, in 1/128 tenth-dB steps */
    case 0:
        v->env = (v->attack * v->env) / 256;
        if (v->env >= -1) { v->env = 0; v->env_state = 1; }
        break;
    case 1:
        v->env -= v->decay;
        if (v->env <= v->sustain) { v->env = v->sustain; v->env_state = 2; }
        break;
    case 3:
        v->env -= v->release;
        if (v->env <= ENV_MIN) { v->active = 0; return; }
        break;
    }
    double lfo = 0;
    if (t->mod_depth && v->lfo_delay <= 0) {
        /* As the SDK's LFO: the counter moves speed << 6 a frame around a 32768-step sine (32 frames, about 6 Hz,
         * at the default speed of 16); depth * range scales it to at most about a semitone */
        v->lfo_phase = (v->lfo_phase + t->mod_speed * 64) & 0x7fff;
        lfo = sin(v->lfo_phase * (2 * 3.14159265358979 / 0x8000)) * t->mod_depth * t->mod_range / 128.0;
    } else if (v->lfo_delay > 0) v->lfo_delay--;
    int db = v->velocity_db + db_sq(t->volume) + db_sq(t->expression) + db_sq(pl->seq_volume) + pl->volume_db +
             db_sq(master_volume) + v->env / 128;
    if (t->mod_type == 1) db += (int)(lfo * 60);
    if (db < -723) db = -723;
    double gain = pow(10.0, db / 200.0) * (t->muted ? 0 : 1);
    int pan = t->pan + v->pan - 64;
    if (t->mod_type == 2) pan += (int)(lfo * 64);
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    v->gain_l = (float)(gain * (127 - pan) / 127.0);
    v->gain_r = (float)(gain * pan / 127.0);
    double semis = v->note - v->base_note + t->bend * t->bend_range / 128.0 + v->sweep;
    if (t->mod_type == 0) semis += lfo;
    v->sweep -= v->sweep_step;
    if ((v->sweep_step > 0 && v->sweep < 0) || (v->sweep_step < 0 && v->sweep > 0)) { v->sweep = 0; v->sweep_step = 0; }
    v->step = note_rate(v, semis) / RATE;
}

/* ------------------------------------------------------------------ sequencer */

static uint32_t read_varlen(Track *t, uint32_t *pc) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = rd8((*pc)++);
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    (void)t;
    return v;
}

/* Sequence variables live in the SDK's shared work, where the game can read them: 0-15 the player's own
 * (+0x20 + player * 0x24), 16-31 global (+0x260) */
static int16_t local_vars[16][16], global_vars[16];
static int get_var(Track *t, int n) {
    n &= 31;
    if (shared) return (int16_t)rd16(n < 16 ? shared + 0x20 + (uint32_t)t->player * 0x24 + (uint32_t)n * 2
                                            : shared + 0x260 + (uint32_t)(n - 16) * 2);
    return n < 16 ? local_vars[t->player][n] : global_vars[n - 16];
}
static void write_var(Track *t, int n, int v) {
    n &= 31;
    if (shared) wr16(n < 16 ? shared + 0x20 + (uint32_t)t->player * 0x24 + (uint32_t)n * 2
                            : shared + 0x260 + (uint32_t)(n - 16) * 2, (uint16_t)(int16_t)v);
    else if (n < 16) local_vars[t->player][n] = (int16_t)v;
    else global_vars[n - 16] = (int16_t)v;
}

static void track_init(Track *t, int player, uint32_t pc) {
    memset(t, 0, sizeof *t);
    t->active = 1;
    t->player = player;
    t->pc = pc;
    t->volume = 127; t->expression = 127; t->pan = 64; t->bend_range = 2; t->prio = 64;
    t->attack = t->decay = t->sustain = t->release = 0xff;
    t->note_wait = 1; t->mod_range = 1; t->mod_speed = 16; t->porta_key = 60;
}

static Track *new_track(void) {
    for (int i = 0; i < 64; i++) if (!tracks[i].active) return &tracks[i];
    return NULL;
}

static void stop_player(int p, int now) {
    Player *pl = &players[p];
    for (int i = 0; i < pl->ntracks; i++) tracks[pl->tracks[i]].active = 0;
    for (int i = 0; i < 64; i++)
        if (voices[i].active && voices[i].player == p) {
            if (now) voices[i].active = 0; else voices[i].env_state = 3;
        }
    pl->active = pl->prepared = 0;
    pl->ntracks = 0;
}

/* B2W2_SND_STATS=from-to: count each player's events between two frames, logged at the end */
static uint32_t stats[16][256];
static void snd_stat(int player, uint8_t cmd) {
    static int lo = -2, hi;
    if (lo == -2) {
        const char *e = getenv("B2W2_SND_STATS");
        lo = -1;
        if (e) sscanf(e, "%d-%d", &lo, &hi);
    }
    if (lo < 0) return;
    if ((int)rc_frames >= lo && (int)rc_frames < hi) stats[player][cmd]++;
    if ((int)rc_frames >= hi) {
        lo = -1;
        for (int p = 0; p < 16; p++) {
            int any = 0;
            for (int c = 0; c < 256; c++) any |= stats[p][c] != 0;
            if (!any) continue;
            rc_logf("snd events player %d:", p);
            for (int c = 0; c < 256; c++) if (stats[p][c] && c >= 0x80) rc_logf(" %02x:%u", c, stats[p][c]);
            uint32_t notes = 0;
            for (int c = 0; c < 0x80; c++) notes += stats[p][c];
            rc_logf(" notes:%u\n", notes);
        }
    }
}

/* Runs one event; returns 0 when the track has to wait */
static void track_step(Track *t) {
    Player *pl = &players[t->player];
    uint32_t base = pl->base;
    for (int guard = 0; guard < 10000 && t->active && t->wait <= 0; guard++) {
        uint32_t pc = t->pc;
        uint8_t cmd = rd8(pc++);
        snd_stat(t->player, cmd);
        int use_random = 0, use_var = 0, use_if = 0;
        if (cmd == 0xa0) { use_random = 1; cmd = rd8(pc++); }
        else if (cmd == 0xa1) { use_var = 1; cmd = rd8(pc++); }
        else if (cmd == 0xa2) { use_if = 1; cmd = rd8(pc++); }
        int skip = use_if && !t->cond;
        /* the last argument of the command, from random, a variable or the stream */
        int16_t lo = 0, hi = 0;
        if (cmd < 0x80) {
            int vel = rd8(pc++);
            int len;
            if (use_random) { lo = (int16_t)rd16u(pc); hi = (int16_t)rd16u(pc + 2); pc += 4; len = lo + (hi > lo ? rand() % (hi - lo + 1) : 0); }
            else if (use_var) len = get_var(t, rd8(pc++));
            else len = (int)read_varlen(t, &pc);
            t->pc = pc;
            if (!skip) {
                note_on(t, pl, cmd + t->transpose, vel, len);
                if (t->note_wait) t->wait = len;
            }
            continue;
        }
        switch (cmd) {
        case 0x80: case 0x81: {
            int v;
            if (use_random) { lo = (int16_t)rd16u(pc); hi = (int16_t)rd16u(pc + 2); pc += 4; v = lo + (hi > lo ? rand() % (hi - lo + 1) : 0); }
            else if (use_var) v = get_var(t, rd8(pc++));
            else v = (int)read_varlen(t, &pc);
            t->pc = pc;
            if (skip) break;
            if (cmd == 0x80) t->wait = v;
            else t->program = v;
            break;
        }
        case 0x93: {
            int n = rd8(pc);
            uint32_t off = rd8(pc + 1) | rd8(pc + 2) << 8 | (uint32_t)rd8(pc + 3) << 16;
            t->pc = pc + 4;
            if (skip) break;
            Track *nt = new_track();
            if (nt && pl->ntracks < 16) {
                track_init(nt, t->player, base + off);
                pl->tracks[pl->ntracks++] = (int)(nt - tracks);
            }
            (void)n;
            break;
        }
        case 0x94: case 0x95: {
            uint32_t off = rd8(pc) | rd8(pc + 1) << 8 | (uint32_t)rd8(pc + 2) << 16;
            pc += 3;
            if (skip) { t->pc = pc; break; }
            if (cmd == 0x95 && t->sp < 3) { t->stack[t->sp] = pc; t->loops[t->sp] = 0; t->sp++; }
            t->pc = base + off;
            break;
        }
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6:
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: {
            int var = rd8(pc++);
            int v;
            if (use_random) { lo = (int16_t)rd16u(pc); hi = (int16_t)rd16u(pc + 2); pc += 4; v = lo + (hi > lo ? rand() % (hi - lo + 1) : 0); }
            else if (use_var) v = get_var(t, rd8(pc++));
            else { v = (int16_t)rd16u(pc); pc += 2; }
            t->pc = pc;
            if (skip) break;
            int cur = get_var(t, var);
            switch (cmd) {
            case 0xb0: write_var(t, var, v); break;
            case 0xb1: write_var(t, var, cur + v); break;
            case 0xb2: write_var(t, var, cur - v); break;
            case 0xb3: write_var(t, var, cur * v); break;
            case 0xb4: if (v) write_var(t, var, cur / v); break;
            case 0xb5: write_var(t, var, v >= 0 ? cur << v : cur >> -v); break;
            case 0xb6: write_var(t, var, v < 0 ? -(rand() % (-v + 1)) : rand() % (v + 1)); break;
            case 0xb8: t->cond = cur == v; break;
            case 0xb9: t->cond = cur >= v; break;
            case 0xba: t->cond = cur > v; break;
            case 0xbb: t->cond = cur <= v; break;
            case 0xbc: t->cond = cur < v; break;
            case 0xbd: t->cond = cur != v; break;
            }
            break;
        }
        case 0xe0: case 0xe1: case 0xe3: {
            int v;
            if (use_random) { lo = (int16_t)rd16u(pc); hi = (int16_t)rd16u(pc + 2); pc += 4; v = lo + (hi > lo ? rand() % (hi - lo + 1) : 0); }
            else if (use_var) v = get_var(t, rd8(pc++));
            else { v = (int16_t)rd16u(pc); pc += 2; }
            t->pc = pc;
            if (skip) break;
            if (cmd == 0xe0) t->mod_delay = v;
            else if (cmd == 0xe1) pl->tempo = v;
            else t->sweep = v;
            break;
        }
        case 0xfc:
            t->pc = pc;
            if (skip || !t->sp) break;
            if (t->loops[t->sp - 1] == 0) { t->pc = t->stack[t->sp - 1]; break; }      /* infinite loop */
            if (--t->loops[t->sp - 1] > 0) t->pc = t->stack[t->sp - 1];
            else t->sp--;
            break;
        case 0xfd:
            t->pc = pc;
            if (!skip && t->sp) t->pc = t->stack[--t->sp];
            break;
        case 0xfe: t->pc = pc + 2; break;
        case 0xff:
            t->pc = pc;
            if (!skip) { t->active = 0; return; }
            break;
        default:
            if (cmd >= 0xc0 && cmd <= 0xdf) {
                int v;
                if (use_random) { lo = (int16_t)rd16u(pc); hi = (int16_t)rd16u(pc + 2); pc += 4; v = lo + (hi > lo ? rand() % (hi - lo + 1) : 0); }
                else if (use_var) v = get_var(t, rd8(pc++));
                else v = rd8(pc++);
                t->pc = pc;
                if (skip) break;
                switch (cmd) {
                case 0xc0: t->pan = v; break;
                case 0xc1: t->volume = v; break;
                case 0xc2: pl->seq_volume = v; break;
                case 0xc3: t->transpose = (int8_t)v; break;
                case 0xc4: t->bend = (int8_t)v; break;
                case 0xc5: t->bend_range = v; break;
                case 0xc6: t->prio = v; break;
                case 0xc7: t->note_wait = v; break;
                case 0xc8: t->tie = v; if (!v && t->tied) { t->tied->env_state = 3; t->tied = NULL; } break;
                case 0xc9: t->porta_key = v + t->transpose; t->porta = 1; break;
                case 0xca: t->mod_depth = v; break;
                case 0xcb: t->mod_speed = v; break;
                case 0xcc: t->mod_type = v; break;
                case 0xcd: t->mod_range = v; break;
                case 0xce: t->porta = v; break;
                case 0xcf: t->porta_time = v; break;
                case 0xd0: t->attack = v; break;
                case 0xd1: t->decay = v; break;
                case 0xd2: t->sustain = v; break;
                case 0xd3: t->release = v; break;
                case 0xd4:
                    if (t->sp < 3) { t->stack[t->sp] = t->pc; t->loops[t->sp] = v; t->sp++; }
                    break;
                case 0xd5: t->expression = v; break;
                case 0xd6: break;
                default: break;
                }
                break;
            }
            /* Unknown event: end the track rather than run off into data */
            t->active = 0;
            return;
        }
    }
}

/* One sequence tick for a player: note lengths, then the tracks' events */
static void player_tick(int p) {
    Player *pl = &players[p];
    for (int i = 0; i < 64; i++) {
        Voice *v = &voices[i];
        if (v->active && v->player == p && v->length_ticks > 0 && --v->length_ticks == 0 && v->env_state != 3)
            v->env_state = 3;
    }
    for (int i = 0; i < pl->ntracks; i++) {
        Track *t = &tracks[pl->tracks[i]];
        if (!t->active) continue;
        if (t->wait > 0) t->wait--;
        track_step(t);
    }
    pl->ticks++;
}

/* A player is done once all its tracks have ended and its notes have died away, whether or not it still ticks
 * (a sequence can end on its first tick, or set tempo 0); the game waits on the status bit for this */
static void check_finished(int p) {
    Player *pl = &players[p];
    for (int i = 0; i < pl->ntracks; i++) if (tracks[pl->tracks[i]].active) return;
    for (int i = 0; i < 64; i++) if (voices[i].active && voices[i].player == p) return;
    pl->active = pl->prepared = 0;
    pl->ntracks = 0;
}

/* The driver's 192 Hz frame: tempo, then envelopes and the status the game reads */
static void driver_frame(void) {
    for (int p = 0; p < 16; p++) {
        Player *pl = &players[p];
        if (!pl->active || pl->paused) continue;
        pl->counter += pl->tempo * pl->tempo_ratio / 256;
        while (pl->counter >= 240 && pl->active) { pl->counter -= 240; player_tick(p); }
        check_finished(p);
    }
    for (int i = 0; i < 64; i++) if (voices[i].active) update_voice(&voices[i]);
    static int every = -2;
    static uint64_t last_dump;
    if (every == -2) { const char *e = getenv("B2W2_SND_STATE"); every = e ? atoi(e) : 0; }
    if (every > 0 && rc_frames % (uint64_t)every == 0 && rc_frames != last_dump) {   /* B2W2_SND_STATE=n */
        last_dump = rc_frames;
        for (int p = 0; p < 16; p++) {
            Player *pl = &players[p];
            if (!pl->active && !pl->prepared) continue;
            int nv = 0;
            for (int i = 0; i < 64; i++) nv += voices[i].active && voices[i].player == p;
            rc_logf("frame %llu player %d: active %d paused %d base %08x tempo %d*%d ticks %u voices %d tracks",
                    (unsigned long long)rc_frames, p, pl->active, pl->paused, pl->base, pl->tempo, pl->tempo_ratio,
                    pl->ticks, nv);
            for (int i = 0; i < pl->ntracks; i++) {
                Track *t = &tracks[pl->tracks[i]];
                rc_logf(" [%d %08x w%d]", t->active, t->pc - pl->base, t->wait);
            }
            rc_logf("\n");
            if (pl->ticks < 4) {
                rc_logf("  seq:");
                for (uint32_t i = 0; i < 0x30; i++) rc_logf(" %02x", rd8(pl->base + i));
                rc_logf("\n");
            }
        }
        if (shared) rc_logf("frame %llu shared: tag %u players %08x channels %04x\n", (unsigned long long)rc_frames,
                            rd32(shared), rd32(shared + 4), rd16(shared + 8));
    }
    if (shared) {
        uint32_t ps = 0;
        uint16_t cs = 0;
        for (int p = 0; p < 16; p++) {
            if (players[p].active) ps |= 1u << p;
            wr32(shared + 0x20 + (uint32_t)p * 0x24 + 0x20, players[p].ticks);
        }
        for (int c = 0; c < 16; c++) if (channels[c].active && (ch_running & (1 << c))) cs |= (uint16_t)(1 << c);
        wr32(shared + 4, ps);
        wr16(shared + 8, cs);
    }
}

/* ------------------------------------------------------------------ commands */

static void set_channel_gain(Voice *v, int volume, int shift, int pan) {
    static const int shifts[4] = {0, 1, 2, 4};
    double g = volume / 127.0 / (1 << shifts[shift & 3]) * pow(10.0, db_sq(master_volume) / 200.0);
    v->gain_l = (float)(g * (127 - pan) / 127.0);
    v->gain_r = (float)(g * pan / 127.0);
    v->pan = pan;
}

static void start_seq(int p, uint32_t base, uint32_t offset, uint32_t bank, int start) {
    if (p < 0 || p > 15) return;
    stop_player(p, 1);
    Player *pl = &players[p];
    int keep_vol = pl->volume_db, keep_prio = pl->prio;
    memset(pl, 0, sizeof *pl);
    pl->volume_db = keep_vol; pl->prio = keep_prio;
    pl->base = base; pl->bank = bank;
    pl->tempo = 120; pl->tempo_ratio = 256; pl->seq_volume = 127;
    Track *t = new_track();
    if (!t) return;
    uint32_t pc = base + offset;
    track_init(t, p, pc);
    if (rd8(pc) == 0xfe) t->pc = pc + 3;                 /* the track allocation header */
    pl->tracks[pl->ntracks++] = (int)(t - tracks);
    pl->prepared = 1;
    pl->active = start;
}

static void invalidate(uint32_t lo, uint32_t hi) {
    for (int p = 0; p < 16; p++) {
        Player *pl = &players[p];
        if ((pl->active || pl->prepared) &&
            ((pl->base >= lo && pl->base < hi) || (pl->bank >= lo && pl->bank < hi))) stop_player(p, 1);
    }
    for (int i = 0; i < 64; i++)
        if (voices[i].active && voices[i].kind == 0 && voices[i].data >= lo && voices[i].data < hi) voices[i].active = 0;
}

/* The menu sounds, for what the runtime adds to the game's menus. They stay loaded in the game's sound heap (the same
 * addresses on the title menu and in the field): cursor, decide, cancel, with the volumes the game gives them. A
 * sound plays only while its bank is there and (once the game has played it) its sequence starts as it did then. */
static const struct { uint32_t seq, bank; int volume; } menu_sounds[3] = {
    {0x020d123c, 0x020d0620, -30}, {0x020d44fc, 0x020d2700, -5}, {0x020d9e5c, 0x020d8f80, -21}};
static uint8_t menu_sound_sig[3][16];
static int menu_sound_seen[3];

static void note_menu_sound(uint32_t seq) {
    for (int k = 0; k < 3; k++)
        if (seq == menu_sounds[k].seq) {
            for (int i = 0; i < 16; i++) menu_sound_sig[k][i] = rd8(seq + i);
            menu_sound_seen[k] = 1;
        }
}

void snd_menu_sound(int k) {
    if (k < 0 || k > 2) return;
    uint32_t seq = menu_sounds[k].seq, bank = menu_sounds[k].bank;
    if (rd32(bank) != 0x4b4e4253) return;                              /* "SBNK" */
    if (menu_sound_seen[k]) for (int i = 0; i < 16; i++) if (rd8(seq + i) != menu_sound_sig[k][i]) return;
    int p = 15;
    while (p > 0 && (players[p].active || players[p].prepared)) p--;
    start_seq(p, seq, 0, bank, 1);
    players[p].prio = 0x72;
    players[p].volume_db = menu_sounds[k].volume;
    if (getenv("B2W2_LOG_SND")) rc_logf("menu sound %d on player %d (frame %llu)\n", k, p, (unsigned long long)rc_frames);
}

int fe_reporting(void), fe_loading(void);  /* frontend.c */
extern int snd_hush_cursor;               /* topscreen.c: the game's cursor sound unheard (the runtime plays its own) */

void snd_command(uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    if ((id == 0 || id == 2) && a1 + a2 == menu_sounds[0].seq && snd_hush_cursor) return;
    /* while a report is under way, or a save file chosen on the list is loading through the title menu's Continue,
     * the runtime presses A for the player: the list made its own decide sound */
    if ((id == 0 || id == 2) && a1 + a2 == menu_sounds[1].seq && (fe_reporting() || fe_loading())) return;
    switch (id) {
    case 0: note_menu_sound(a1 + a2); start_seq((int)a0, a1, a2, a3, 1); break;   /* START_SEQ */
    case 1: if (a0 < 16) stop_player((int)a0, 0); break;              /* STOP_SEQ */
    case 2: note_menu_sound(a1 + a2); start_seq((int)a0, a1, a2, a3, 0); break;   /* PREPARE_SEQ */
    case 3: if (a0 < 16 && players[a0].prepared && !players[a0].active) { players[a0].active = 1; check_finished((int)a0); } break;
    case 4: if (a0 < 16) players[a0].paused = (int)a1; break;         /* PAUSE_SEQ */
    case 5:                                                            /* SKIP_SEQ */
        if (a0 < 16) for (uint32_t i = 0; i < a1 && players[a0].active; i++) player_tick((int)a0);
        break;
    case 6:                                                            /* PLAYER_PARAM: player, offset, value, size */
        if (a0 < 16) {
            if (a1 == 4) players[a0].prio = (int)(a2 & 0xff);
            else if (a1 == 6) players[a0].volume_db = (int16_t)a2;
            else if (a1 == 0x18) players[a0].tempo = (int)(a2 & 0xffff);
            else if (a1 == 0x1a) players[a0].tempo_ratio = (int)(a2 & 0xffff);
            if (getenv("B2W2_LOG_SND")) rc_logf("player %u param %02x = %08x size %u (frame %llu)\n", a0, a1, a2, a3,
                                                (unsigned long long)rc_frames);
        }
        break;
    case 8:                                                            /* MUTE_TRACK: player, track mask, flag */
        if (a0 < 16)
            for (int i = 0; i < players[a0].ntracks; i++)
                if (a1 & (1u << i)) tracks[players[a0].tracks[i]].muted = (int)a2;
        break;
    case 10: if (a0 < 16 && shared) wr16(shared + 0x20 + a0 * 0x24 + (a1 & 15) * 2, (uint16_t)a2); break;
    case 11: if (shared) wr16(shared + 0x260 + (a0 & 15) * 2, (uint16_t)a1); break;
    case 12:                                                           /* START_TIMER: channels, capture, alarms */
        ch_running |= (int)(a0 & 0xffff);
        for (int c = 0; c < 16; c++)
            if (a0 & (1u << c)) {
                Voice *v = &channels[c];
                v->pos = 0; v->loop_saved = 0; v->adpcm_pos = 0;
                if (v->format == 2 && v->data) { v->adpcm_sample = (int16_t)rd16(v->data - 4); v->adpcm_index = rd8(v->data - 2) & 0x7f; }
                if (v->data || v->kind) v->active = 1;
            }
        for (int i = 0; i < 8; i++) if (a2 & (1u << i)) { alarms[i].on = 1; alarms[i].count = alarms[i].tick; }
        break;
    case 13:                                                           /* STOP_TIMER */
        ch_running &= ~(int)(a0 & 0xffff);
        for (int c = 0; c < 16; c++) if (a0 & (1u << c)) channels[c].active = 0;
        for (int i = 0; i < 8; i++) if (a2 & (1u << i)) alarms[i].on = 0;
        break;
    case 14: {                                                         /* SETUP_CHANNEL_PCM */
        int c = (int)(a0 & 15);
        Voice *v = &channels[c];
        memset(v, 0, sizeof *v);
        v->format = (int)((a3 >> 24) & 3);
        v->loop = (int)((a3 >> 26) & 3);
        int per_word = v->format == 0 ? 4 : v->format == 1 ? 2 : 8;
        uint32_t timer = (a0 >> 16) & 0xffff;
        v->data = a1 & 0x7ffffff;
        v->loop_start = (a3 & 0xffff) * (uint32_t)per_word;
        v->length = v->loop_start + (a2 & 0x3fffff) * (uint32_t)per_word;
        if (v->format == 2) {
            v->adpcm_sample = (int16_t)rd16(v->data); v->adpcm_index = rd8(v->data + 2) & 0x7f;
            v->data += 4;
            v->loop_start = v->loop_start >= 8 ? v->loop_start - 8 : 0;
            v->length -= 8;
        }
        v->step = timer ? 16756991.0 / timer / RATE : 0;
        set_channel_gain(v, (int)((a2 >> 24) & 0x7f), (int)((a2 >> 22) & 3), (int)((a3 >> 16) & 0x7f));
        v->active = 1;
        break;
    }
    case 15: case 16: {                                                /* SETUP_CHANNEL_PSG / NOISE */
        int c = (int)(a0 & 15);
        Voice *v = &channels[c];
        memset(v, 0, sizeof *v);
        v->kind = id == 15 ? 1 : 2;
        v->duty = (int)(a1 & 7);
        v->lfsr = 0x7fff;
        uint32_t timer = (a0 >> 16) & 0xffff;
        v->step = timer ? 16756991.0 / timer / RATE : 0;
        set_channel_gain(v, (int)((a2 >> 24) & 0x7f), (int)((a2 >> 22) & 3), (int)((a3 >> 16) & 0x7f));
        v->active = 1;
        break;
    }
    case 18:                                                           /* SETUP_ALARM: no, tick, period, id */
        if (a0 < 8) { alarms[a0].tick = (int)a1; alarms[a0].period = (int)a2; alarms[a0].id = (int)(a3 & 0xff); }
        break;
    case 19:                                                           /* CHANNEL_TIMER: mask, timer */
        for (int c = 0; c < 16; c++) if (a0 & (1u << c)) channels[c].step = a1 ? 16756991.0 / (a1 & 0xffff) / RATE : 0;
        break;
    case 20:                                                           /* CHANNEL_VOLUME: mask, volume, shift */
        for (int c = 0; c < 16; c++) if (a0 & (1u << c)) set_channel_gain(&channels[c], (int)(a1 & 0x7f), (int)a2, channels[c].pan ? channels[c].pan : 64);
        break;
    case 21:                                                           /* CHANNEL_PAN: mask, pan */
        for (int c = 0; c < 16; c++)
            if (a0 & (1u << c)) {
                float g = channels[c].gain_l + channels[c].gain_r;
                int pan = (int)(a1 & 0x7f);
                channels[c].gain_l = g * (127 - pan) / 127.0f; channels[c].gain_r = g * pan / 127.0f;
                channels[c].pan = pan;
            }
        break;
    case 23: master_volume = (int)(a0 & 0x7f); break;
    case 30: case 31: case 32: invalidate(a0, a1); break;             /* INVALIDATE_SEQ/BANK/WAVE */
    default: break;
    }
}

/* ------------------------------------------------------------------ mixing */

void arm7_reply_sound_alarm(int no, int id);

/* Mixes n stereo samples into out (may be NULL: then the state still advances) */
void snd_mix(int16_t *out, int n) {
    const double ticks_per_sample = RC_CLOCK / 64.0 / RATE;          /* alarms count OS ticks */
    for (int s = 0; s < n; s++) {
        seq_clock += SEQ_HZ / RATE;
        if (seq_clock >= 1) { seq_clock -= 1; driver_frame(); }
        for (int i = 0; i < 8; i++) {
            if (!alarms[i].on) continue;
            alarms[i].count -= (int)(ticks_per_sample + 0.5);
            if (alarms[i].count <= 0) {
                arm7_reply_sound_alarm(i, alarms[i].id);
                if (alarms[i].period) alarms[i].count += alarms[i].period; else alarms[i].on = 0;
            }
        }
        float l = 0, r = 0;
        for (int i = 0; i < 64; i++) {
            Voice *v = &voices[i];
            if (!v->active) continue;
            float x = (float)next_sample(v);
            l += x * v->gain_l; r += x * v->gain_r;
        }
        for (int c = 0; c < 16; c++) {
            Voice *v = &channels[c];
            if (!v->active || !(ch_running & (1 << c))) continue;
            float x = (float)next_sample(v);
            l += x * v->gain_l; r += x * v->gain_r;
        }
        if (out) {
            l *= 0.5f; r *= 0.5f;
            out[2 * s] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            out[2 * s + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
    }
}
