/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "ext_engines.h"
#include "dsp_math.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Shared voice bank                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t note, down, stage; /* stage: 1 attack, 2 hold, 3 release */
    uint32_t age;
    float env, gain, semis;    /* semis: cents offset captured at note-on, in semitones */
} ev_t;
struct ecommon {
    uint8_t pot[EXT_PARAMS], pedal, limit;
    float bend;                /* semitones */
    uint32_t age, rng;
};
struct engine {
    const char *name;
    size_t size, voice_off, voice_stride;
    const fm1_param_t *params;
    void (*init)(void *);
    void (*start)(void *, ev_t *, unsigned velocity);
    void (*render)(void *, int32_t *, unsigned);
};
#define COMMON(p) ((struct ecommon *)(p))
#define VOICE(e, p, i) ((ev_t *)((uint8_t *)(p) + (e)->voice_off + (i) * (e)->voice_stride))

static unsigned active_count(const struct engine *e, const void *p)
{
    unsigned n = 0;
    for (unsigned i = 0; i < EXT_VOICES; ++i) n += VOICE(e, (void *)p, i)->note != 255;
    return n;
}
static unsigned oldest(const struct engine *e, void *p)
{
    unsigned pick = EXT_VOICES, rank_best = 0;
    uint32_t longest = 0;
    for (unsigned i = 0; i < EXT_VOICES; ++i) {
        ev_t *v = VOICE(e, p, i);
        if (v->note == 255) continue;
        uint32_t age = COMMON(p)->age - v->age;
        unsigned rank = !v->down && !COMMON(p)->pedal ? 2U : !v->down ? 1U : 0U;
        if (pick == EXT_VOICES || rank > rank_best || (rank == rank_best && age > longest)) {
            pick = i; rank_best = rank; longest = age;
        }
    }
    return pick;
}
static float hz_of(const struct ecommon *c, const ev_t *v, float extra)
{
    return 440.0f * dsp_exp2(((float)v->note - 69.0f + v->semis + c->bend + extra) * (1.0f / 12.0f));
}
/* 0..127 -> seconds on an exponential scale. */
static float seconds(unsigned pot, float lo, float hi)
{
    return lo * dsp_exp2(dsp_log2(hi / lo) * (float)pot * (1.0f / 127.0f));
}
/* Amplitude envelope: linear attack, hold, exponential release. Returns env. */
static inline float env_step(ev_t *v, const struct ecommon *c, float attack, float release)
{
    if (v->stage == 1) { v->env += attack; if (v->env >= 1.0f) { v->env = 1.0f; v->stage = 2; } }
    else if (v->stage == 2) { if (!v->down && !c->pedal) v->stage = 3; }
    else { v->env *= release; if (v->env < 0.0002f) { v->note = 255; v->env = 0; } }
    if (v->stage < 3 && !v->down && !c->pedal && v->stage == 1) v->stage = 3;
    return v->env;
}

static unsigned generic_active(const struct engine *e, const void *p) { return active_count(e, p); }

/* ------------------------------------------------------------------ */
/* Wavetables (generated once, additive)                              */
/* ------------------------------------------------------------------ */
enum { WT_TABLES = 8, WT_SIZE = 256 };
static float wt[WT_TABLES][WT_SIZE + 1];
static unsigned wt_ready;
static void wt_build(void)
{
    if (wt_ready) return;
    for (unsigned k = 0; k < WT_TABLES; ++k) {
        float peak = 0.0001f;
        for (unsigned i = 0; i < WT_SIZE; ++i) {
            float x = (float)i * (2.0f * DSP_PI / WT_SIZE), s = 0;
            for (unsigned h = 1; h <= 32; ++h) {
                float fh = (float)h, w = 0;
                switch (k) {
                case 0: w = h == 1 ? 1.0f : 0; break;                                   /* sine */
                case 1: w = (h & 1U) ? ((h >> 1) & 1U ? -1.0f : 1.0f) / (fh * fh) : 0; break; /* triangle */
                case 2: w = 1.0f / fh; break;                                           /* saw */
                case 3: w = (h & 1U) ? 1.0f / fh : 0; break;                            /* square */
                case 4: w = dsp_sin(DSP_PI * fh * 0.12f) / fh; break;                   /* thin pulse */
                case 5: { float a = (fh - 5.0f) * 0.45f, b = (fh - 11.0f) * 0.35f;      /* vowel */
                          w = (dsp_exp(-a * a) + 0.6f * dsp_exp(-b * b)) / fh * 3.0f; break; }
                case 6: w = (h == 1 || h == 2 || h == 3 || h == 4 || h == 6 || h == 8) ? 1.0f / (0.6f + 0.4f * fh) : 0; break; /* organ */
                default: w = (h & 1U) ? (float)((h * 7919U) % 5U) / (5.0f * dsp_sqrt(fh)) : 0; break; /* metal */
                }
                s += w * dsp_sin(x * fh);
            }
            wt[k][i] = s;
            if (dsp_abs(s) > peak) peak = dsp_abs(s);
        }
        for (unsigned i = 0; i < WT_SIZE; ++i) wt[k][i] /= peak;
        wt[k][WT_SIZE] = wt[k][0];
    }
    wt_ready = 1;
}
/* morph in 0..1 across the eight tables, phase in 0..1 */
static inline float wt_read(float morph, float phase)
{
    float m = morph * (WT_TABLES - 1);
    unsigned a = (unsigned)m;
    if (a >= WT_TABLES - 1) a = WT_TABLES - 2;
    float mf = m - (float)a;
    float pos = phase * WT_SIZE;
    unsigned i = (unsigned)pos;
    float f = pos - (float)i;
    float x = wt[a][i] + (wt[a][i + 1] - wt[a][i]) * f;
    float y = wt[a + 1][i] + (wt[a + 1][i + 1] - wt[a + 1][i]) * f;
    return x + (y - x) * mf;
}

/* ------------------------------------------------------------------ */
/* WAVE: two-oscillator wavetable with phase warp                     */
/* ------------------------------------------------------------------ */
static FM1_DSP_DATA const fm1_param_t wave_params[EXT_PARAMS] = {
    {"TABLE", 127, 40, 0}, {"WARP", 127, 0, 0}, {"DETUNE", 127, 24, 0}, {"CUTOFF", 127, 100, 0},
    {"RESO", 127, 20, 0}, {"ENVAMT", 127, 50, 0}, {"ATTACK", 127, 2, 0}, {"RELEASE", 127, 40, 0}};
struct wave_voice { ev_t e; float p1, p2, fenv, vel; dsp_svf_t f; };
struct wave { struct ecommon c; struct wave_voice v[EXT_VOICES]; };
static void wave_init(void *p) { (void)p; wt_build(); }
static void wave_start(void *p, ev_t *e, unsigned vel)
{
    struct wave_voice *v = (struct wave_voice *)e; (void)p;
    v->p1 = v->p2 = 0.0f; v->fenv = 1.0f; v->vel = (float)vel / 127.0f; memset(&v->f, 0, sizeof v->f);
}
static void wave_render(void *p, int32_t *out, unsigned frames)
{
    struct wave *w = p; const uint8_t *pot = w->c.pot;
    float morph = pot[0] / 127.0f, warp = pot[1] / 127.0f, det = pot[2] / 127.0f * 0.5f;
    float k = 0.5f - 0.45f * warp, k1 = 0.5f / k, k2 = 0.5f / (1.0f - k);
    float attack = 1.0f / (44100.0f * seconds(pot[6], 0.002f, 3.0f));
    float release = dsp_decay(seconds(pot[7], 0.02f, 5.0f));
    float fdecay = dsp_decay(0.35f), q = 0.55f + pot[4] * 0.06f;
    float base = 40.0f * dsp_exp2(pot[3] * (1.0f / 127.0f) * 8.6f), amt = pot[5] / 127.0f * 6.0f;
    for (unsigned n = 0; n < EXT_VOICES; ++n) {
        struct wave_voice *v = &w->v[n];
        if (v->e.note == 255) continue;
        float hz = hz_of(&w->c, &v->e, 0), hz2 = hz_of(&w->c, &v->e, det);
        float i1 = hz / 44100.0f, i2 = hz2 / 44100.0f;
        for (unsigned i = 0; i < frames; ++i) {
            float env = env_step(&v->e, &w->c, attack, release);
            if (v->e.note == 255) break;
            float x1 = v->p1, x2 = v->p2;
            x1 = x1 < k ? x1 * k1 : 0.5f + (x1 - k) * k2;
            x2 = x2 < k ? x2 * k1 : 0.5f + (x2 - k) * k2;
            float s = (wt_read(morph, x1) + wt_read(morph, x2)) * 0.5f;
            v->p1 += i1; if (v->p1 >= 1.0f) v->p1 -= 1.0f;
            v->p2 += i2; if (v->p2 >= 1.0f) v->p2 -= 1.0f;
            v->fenv *= fdecay;
            if ((i & 15U) == 0) dsp_svf_set(&v->f, dsp_min(base * (1.0f + amt * v->fenv * v->vel * 2.0f), 16000.0f), q);
            s = dsp_svf(&v->f, s, 0);
            out[i] += (int32_t)(s * env * (0.12f + 0.12f * v->vel) * 16777216.0f);
        }
    }
}

/* ------------------------------------------------------------------ */
/* PAD: three detuned band-limited saws + sub, slow filter, chorus    */
/* ------------------------------------------------------------------ */
static FM1_DSP_DATA const fm1_param_t pad_params[EXT_PARAMS] = {
    {"DETUNE", 127, 40, 0}, {"CUTOFF", 127, 70, 0}, {"RESO", 127, 15, 0}, {"SWEEP", 127, 40, 0},
    {"ATTACK", 127, 70, 0}, {"RELEASE", 127, 80, 0}, {"CHORUS", 127, 80, 0}, {"SUB", 127, 40, 0}};
enum { CHORUS_LEN = 2048 };
struct pad_voice { ev_t e; float ph[3], sub; dsp_svf_t f; };
struct pad { struct ecommon c; struct pad_voice v[EXT_VOICES]; float lfo, c1, c2; float chorus[CHORUS_LEN]; unsigned cpos; };
static inline float blep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0;
}
static void pad_init(void *p) { struct pad *s = p; s->c1 = 0.0f; s->c2 = 0.33f; }
static void pad_start(void *p, ev_t *e, unsigned vel)
{
    struct pad_voice *v = (struct pad_voice *)e; (void)p;
    v->ph[0] = 0.0f; v->ph[1] = 0.31f; v->ph[2] = 0.67f; v->sub = 0; memset(&v->f, 0, sizeof v->f);
    e->gain = 0.2f + 0.8f * (float)vel / 127.0f;
}
static void pad_render(void *p, int32_t *out, unsigned frames)
{
    struct pad *s = p; const uint8_t *pot = s->c.pot;
    float det = pot[0] / 127.0f * 0.8f, q = 0.55f + pot[2] * 0.045f;
    float attack = 1.0f / (44100.0f * seconds(pot[4], 0.01f, 5.0f));
    float release = dsp_decay(seconds(pot[5], 0.03f, 7.0f));
    float base = 80.0f * dsp_exp2(pot[1] * (1.0f / 127.0f) * 7.2f), sweep = pot[3] / 127.0f * 2.5f;
    float chorus = pot[6] / 127.0f, sub = pot[7] / 127.0f * 0.6f;
    float mix[128]; memset(mix, 0, sizeof mix);
    if (frames > 128) frames = 128;
    for (unsigned n = 0; n < EXT_VOICES; ++n) {
        struct pad_voice *v = &s->v[n];
        if (v->e.note == 255) continue;
        float dt[3], hz = hz_of(&s->c, &v->e, 0);
        dt[0] = hz_of(&s->c, &v->e, -det) / 44100.0f; dt[1] = hz / 44100.0f; dt[2] = hz_of(&s->c, &v->e, det) / 44100.0f;
        float subdt = dt[1] * 0.5f;
        for (unsigned i = 0; i < frames; ++i) {
            float env = env_step(&v->e, &s->c, attack, release);
            if (v->e.note == 255) break;
            float x = 0;
            for (unsigned k = 0; k < 3; ++k) {
                float t = v->ph[k];
                x += (2.0f * t - 1.0f) - blep(t, dt[k]);
                v->ph[k] += dt[k]; if (v->ph[k] >= 1.0f) v->ph[k] -= 1.0f;
            }
            v->sub += subdt; if (v->sub >= 1.0f) v->sub -= 1.0f;
            x = x * (1.0f / 3.0f) + sub * (v->sub < 0.5f ? 1.0f : -1.0f) * 0.5f;
            if ((i & 15U) == 0) {
                float m = 1.0f + sweep * (0.5f + 0.5f * dsp_sin(s->lfo * 6.2831853f)) * env;
                dsp_svf_set(&v->f, dsp_min(base * m, 16000.0f), q);
            }
            mix[i] += dsp_svf(&v->f, x, 0) * env * v->e.gain * 0.16f;
        }
    }
    for (unsigned i = 0; i < frames; ++i) {
        s->lfo += 0.00004f; if (s->lfo >= 1.0f) s->lfo -= 1.0f;
        s->c1 += 0.0000091f; if (s->c1 >= 1.0f) s->c1 -= 1.0f;
        s->c2 += 0.0000143f; if (s->c2 >= 1.0f) s->c2 -= 1.0f;
        s->chorus[s->cpos] = mix[i];
        float d1 = 520.0f + 360.0f * dsp_sin(s->c1 * 6.2831853f), d2 = 700.0f + 420.0f * dsp_sin(s->c2 * 6.2831853f);
        float wet = 0;
        for (unsigned k = 0; k < 2; ++k) {
            float d = k ? d2 : d1, rp = (float)s->cpos - d;
            if (rp < 0) rp += CHORUS_LEN;
            unsigned a = (unsigned)rp & (CHORUS_LEN - 1U), b = (a + 1U) & (CHORUS_LEN - 1U);
            float f = rp - (float)(unsigned)rp;
            wet += s->chorus[a] + (s->chorus[b] - s->chorus[a]) * f;
        }
        s->cpos = (s->cpos + 1U) & (CHORUS_LEN - 1U);
        out[i] += (int32_t)((mix[i] * (1.0f - 0.35f * chorus) + wet * 0.5f * chorus * 0.9f) * 16777216.0f);
    }
}

/* ------------------------------------------------------------------ */
/* CLOUD: granular soundscape over the wavetables                     */
/* ------------------------------------------------------------------ */
static FM1_DSP_DATA const fm1_param_t cloud_params[EXT_PARAMS] = {
    {"SOURCE", 127, 50, 0}, {"SIZE", 127, 60, 0}, {"DENSITY", 127, 70, 0}, {"SPREAD", 127, 30, 0},
    {"SCATTER", 127, 30, 0}, {"SHIMMER", 127, 30, 0}, {"ATTACK", 127, 60, 0}, {"RELEASE", 127, 80, 0}};
enum { GRAINS = 40 };
struct grain { float ph, inc, w, winc, amp, morph; uint8_t on; };
struct cloud_voice { ev_t e; float timer; };
struct cloud { struct ecommon c; struct cloud_voice v[EXT_VOICES]; struct grain g[GRAINS]; };
static void cloud_init(void *p) { (void)p; wt_build(); }
static void cloud_start(void *p, ev_t *e, unsigned vel)
{
    struct cloud_voice *v = (struct cloud_voice *)e; (void)p;
    v->timer = 1.0f; e->gain = 0.25f + 0.75f * (float)vel / 127.0f;
}
static void cloud_render(void *p, int32_t *out, unsigned frames)
{
    struct cloud *s = p; const uint8_t *pot = s->c.pot;
    float morph = pot[0] / 127.0f, spread = pot[3] / 127.0f, scatter = pot[4] / 127.0f, shimmer = pot[5] / 127.0f;
    float size = seconds(pot[1], 0.012f, 0.6f), rate = seconds(pot[2], 4.0f, 120.0f) / 44100.0f;
    float attack = 1.0f / (44100.0f * seconds(pot[6], 0.01f, 5.0f));
    float release = dsp_decay(seconds(pot[7], 0.05f, 8.0f));
    if (frames > 128) frames = 128;
    float mix[128]; memset(mix, 0, sizeof mix);
    for (unsigned i = 0; i < frames; ++i) {
        for (unsigned n = 0; n < EXT_VOICES; ++n) {
            struct cloud_voice *v = &s->v[n];
            if (v->e.note == 255) continue;
            float env = env_step(&v->e, &s->c, attack, release);
            if (v->e.note == 255) continue;
            v->timer += rate * (0.5f + 0.5f * (dsp_noise(&s->c.rng) * scatter + 1.0f));
            if (v->timer >= 1.0f) {
                v->timer -= 1.0f;
                for (unsigned g = 0; g < GRAINS; ++g) {
                    struct grain *gr = &s->g[g];
                    if (gr->on) continue;
                    float semis = dsp_noise(&s->c.rng) * spread * 12.0f;
                    float r = dsp_noise(&s->c.rng);
                    if (r > 1.0f - 2.0f * shimmer * 0.5f) semis += r > 0.9f ? 19.0f : 12.0f;
                    float hz = hz_of(&s->c, &v->e, semis);
                    gr->ph = 0; gr->inc = hz / 44100.0f; gr->w = 0;
                    gr->winc = 1.0f / (44100.0f * size * (0.7f + 0.3f * (dsp_noise(&s->c.rng) * scatter + 1.0f)));
                    gr->amp = env * v->e.gain * 0.22f;
                    gr->morph = dsp_clamp(morph + dsp_noise(&s->c.rng) * scatter * 0.25f, 0.0f, 1.0f);
                    gr->on = 1;
                    break;
                }
            }
        }
        float acc = 0;
        for (unsigned g = 0; g < GRAINS; ++g) {
            struct grain *gr = &s->g[g];
            if (!gr->on) continue;
            float w = gr->w, win = 0.5f - 0.5f * dsp_cos(w * 6.2831853f);
            acc += wt_read(gr->morph, gr->ph) * win * gr->amp;
            gr->ph += gr->inc; if (gr->ph >= 1.0f) gr->ph -= 1.0f;
            gr->w += gr->winc; if (gr->w >= 1.0f) gr->on = 0;
        }
        mix[i] = acc;
    }
    for (unsigned i = 0; i < frames; ++i) out[i] += (int32_t)(mix[i] * 16777216.0f);
}

/* ------------------------------------------------------------------ */
/* KEYS: electric piano / bell / clav / marimba FM                    */
/* ------------------------------------------------------------------ */
static FM1_DSP_DATA const char *const keys_names[4] = {"EPIANO", "BELL", "CLAV", "MARIMBA"};
static FM1_DSP_DATA const fm1_param_t keys_params[EXT_PARAMS] = {
    {"VOICE", 3, 0, keys_names}, {"TONE", 127, 60, 0}, {"DECAY", 127, 70, 0}, {"BELL", 127, 40, 0},
    {"TREM", 127, 30, 0}, {"RATE", 127, 40, 0}, {"HARD", 127, 60, 0}, {"RELEASE", 127, 40, 0}};
struct keys_voice { ev_t e; float p1, m1, p2, m2, idx, vel, tail; };
struct keys { struct ecommon c; struct keys_voice v[EXT_VOICES]; float trem; };
static void keys_init(void *p) { struct keys *k = p; k->trem = 0; }
static void keys_start(void *p, ev_t *e, unsigned vel)
{
    struct keys_voice *v = (struct keys_voice *)e; struct keys *k = p;
    v->p1 = v->m1 = v->p2 = v->m2 = 0; v->vel = (float)vel / 127.0f;
    float hard = k->c.pot[6] / 127.0f;
    v->idx = 1.0f; e->gain = 0.35f + 0.65f * v->vel;
    v->tail = 0.4f + 0.6f * (1.0f - hard * v->vel);
}
static void keys_render(void *p, int32_t *out, unsigned frames)
{
    struct keys *k = p; const uint8_t *pot = k->c.pot;
    static const float ratio[4][2] = {{1.0f, 14.0f}, {3.5f, 7.0f}, {2.0f, 11.0f}, {4.0f, 10.0f}};
    unsigned vi = pot[0] & 3U;
    float tone = pot[1] / 127.0f * (vi == 2 ? 3.0f : 2.2f), bell = pot[3] / 127.0f;
    float decay = dsp_decay(seconds(pot[2], 0.15f, 9.0f)), fast = dsp_decay(seconds(pot[2], 0.04f, 0.6f));
    float release = dsp_decay(seconds(pot[7], 0.03f, 2.0f));
    float depth = pot[4] / 127.0f * 0.5f, trate = (0.5f + pot[5] / 127.0f * 7.0f) / 44100.0f;
    float attack = 1.0f / (44100.0f * 0.002f);
    if (frames > 128) frames = 128;
    for (unsigned i = 0; i < frames; ++i) {
        k->trem += trate; if (k->trem >= 1.0f) k->trem -= 1.0f;
        float tr = 1.0f - depth * (0.5f + 0.5f * dsp_sin(k->trem * 6.2831853f));
        float acc = 0;
        for (unsigned n = 0; n < EXT_VOICES; ++n) {
            struct keys_voice *v = &k->v[n];
            if (v->e.note == 255) continue;
            float hz = hz_of(&k->c, &v->e, 0);
            if (v->e.stage == 1) { v->e.env += attack; if (v->e.env >= 1.0f) { v->e.env = 1.0f; v->e.stage = 2; } }
            else if (v->e.stage == 2) {
                v->e.env *= decay;
                if (!v->e.down && !k->c.pedal) v->e.stage = 3;
            } else { v->e.env *= release * decay; if (v->e.env < 0.0002f) { v->e.note = 255; continue; } }
            if (v->e.stage == 1 && !v->e.down && !k->c.pedal) v->e.stage = 3;
            v->idx *= fast;
            float inc1 = hz / 44100.0f;
            float m1 = dsp_sin(v->m1 * 6.2831853f), c1 = dsp_sin((v->p1 + m1 * (tone * (0.15f + v->idx * v->tail * 1.2f))) * 6.2831853f);
            float m2 = dsp_sin(v->m2 * 6.2831853f), c2 = dsp_sin((v->p2 + m2 * 0.9f * v->idx) * 6.2831853f);
            v->p1 += inc1; if (v->p1 >= 1.0f) v->p1 -= 1.0f;
            v->m1 += inc1 * ratio[vi][0]; if (v->m1 >= 1.0f) v->m1 -= 1.0f;
            v->p2 += inc1 * ratio[vi][1] * 0.5f; if (v->p2 >= 1.0f) v->p2 -= 1.0f;
            v->m2 += inc1 * ratio[vi][1]; if (v->m2 >= 1.0f) v->m2 -= 1.0f;
            acc += (c1 + c2 * bell * v->idx * 0.7f) * v->e.env * v->e.gain * 0.22f;
        }
        out[i] += (int32_t)(acc * tr * 16777216.0f);
    }
}

/* ------------------------------------------------------------------ */
/* Factory presets (8 per engine, value order = parameter order)      */
/* ------------------------------------------------------------------ */
struct preset { const char *name; uint8_t v[EXT_PARAMS]; };
static const struct preset presets[EXT_COUNT][EXT_PRESETS] = {
    {{"INIT SAW", {40, 0, 24, 100, 20, 50, 2, 40}},    {"GLASS BELL", {10, 30, 30, 110, 25, 70, 0, 90}},
     {"VOWEL CHOIR", {85, 20, 40, 90, 30, 20, 60, 80}}, {"ORGAN", {100, 0, 10, 120, 0, 0, 0, 30}},
     {"ACID PULSE", {55, 70, 10, 60, 90, 100, 0, 25}},  {"METAL HIT", {127, 40, 60, 95, 60, 90, 0, 50}},
     {"SOFT SINE", {0, 0, 15, 120, 0, 0, 25, 70}},      {"WARP LEAD", {45, 100, 35, 105, 40, 40, 5, 45}}},
    {{"STRINGS", {45, 75, 10, 30, 70, 80, 90, 30}},     {"SLOW PAD", {60, 60, 20, 70, 100, 110, 100, 50}},
     {"BRASS SWELL", {30, 85, 30, 100, 60, 50, 40, 60}}, {"DARK DRONE", {80, 35, 50, 90, 110, 120, 110, 100}},
     {"SUPER LEAD", {90, 110, 10, 0, 0, 40, 60, 20}},   {"SOFT CHOIR", {35, 65, 5, 25, 85, 100, 127, 20}},
     {"ICE WASH", {100, 120, 60, 60, 90, 127, 100, 0}}, {"TAPE WARBLE", {55, 55, 30, 60, 40, 70, 127, 70}}},
    {{"SHIMMER", {50, 60, 70, 30, 30, 90, 60, 80}},     {"RAIN", {20, 15, 110, 60, 70, 0, 5, 40}},
     {"DUST", {127, 10, 60, 100, 100, 10, 0, 30}},      {"FROZEN LAKE", {10, 90, 40, 10, 20, 60, 100, 120}},
     {"GHOST CHOIR", {85, 95, 50, 25, 40, 50, 90, 110}}, {"BUBBLES", {0, 25, 90, 50, 60, 40, 10, 50}},
     {"SWARM", {127, 40, 127, 80, 90, 20, 40, 70}},     {"AURORA", {40, 110, 35, 15, 50, 110, 110, 127}}},
    {{"SUITCASE", {0, 60, 70, 40, 30, 40, 60, 40}},     {"TREM EP", {0, 70, 80, 30, 90, 60, 70, 50}},
     {"MUSIC BOX", {1, 40, 60, 90, 0, 0, 80, 60}},      {"TUBULAR", {1, 70, 100, 100, 0, 0, 60, 90}},
     {"CLAVINET", {2, 100, 35, 20, 0, 0, 100, 20}},     {"WAH CLAV", {2, 127, 30, 30, 40, 90, 127, 15}},
     {"MARIMBA", {3, 60, 45, 30, 0, 0, 90, 25}},        {"VIBES", {3, 45, 95, 60, 70, 50, 50, 70}}},
};
void ext_preset(unsigned type, void *p, unsigned program)
{
    if (!ext_is(type)) return;
    const struct preset *s = &presets[type - EXT_FIRST][program % EXT_PRESETS];
    for (unsigned i = 0; i < EXT_PARAMS; ++i) COMMON(p)->pot[i] = s->v[i];
}
const char *ext_preset_name(unsigned type, unsigned program)
{
    return ext_is(type) ? presets[type - EXT_FIRST][program % EXT_PRESETS].name : "";
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */
#define ENGINE(nm, type, params_, init_, start_, render_) \
    {nm, sizeof(struct type), offsetof(struct type, v), sizeof(((struct type *)0)->v[0]), params_, init_, start_, render_}
static const struct engine engines[EXT_COUNT] = {
    ENGINE("WAVE", wave, wave_params, wave_init, wave_start, wave_render),
    ENGINE("PAD", pad, pad_params, pad_init, pad_start, pad_render),
    ENGINE("CLOUD", cloud, cloud_params, cloud_init, cloud_start, cloud_render),
    ENGINE("KEYS", keys, keys_params, keys_init, keys_start, keys_render),
};
static const struct engine *eng(unsigned type) { return ext_is(type) ? &engines[type - EXT_FIRST] : NULL; }
const char *ext_name(unsigned type) { const struct engine *e = eng(type); return e ? e->name : ""; }
size_t ext_size(unsigned type) { const struct engine *e = eng(type); return e ? e->size : 0; }
void ext_panic(unsigned type, void *p)
{
    const struct engine *e = eng(type); if (!e) return;
    for (unsigned i = 0; i < EXT_VOICES; ++i) { ev_t *v = VOICE(e, p, i); v->note = 255; v->env = 0; v->down = 0; }
    if (type == EXT_FIRST + 2) memset(((struct cloud *)p)->g, 0, sizeof ((struct cloud *)p)->g);
    COMMON(p)->pedal = 0; COMMON(p)->bend = 0;
}
void ext_init(unsigned type, void *p)
{
    const struct engine *e = eng(type); if (!e) return;
    memset(p, 0, e->size);
    COMMON(p)->limit = EXT_VOICES; COMMON(p)->rng = 0x1234567U;
    for (unsigned i = 0; i < EXT_PARAMS; ++i) COMMON(p)->pot[i] = e->params[i].def;
    ext_panic(type, p);
    ext_preset(type, p, 0);
    if (e->init) e->init(p);
}
void ext_note(unsigned type, void *p, unsigned note, unsigned velocity, int cents)
{
    const struct engine *e = eng(type);
    if (!e || note > 127 || velocity > 127) return;
    if (!velocity) {
        for (unsigned i = 0; i < EXT_VOICES; ++i) if (VOICE(e, p, i)->note == note) VOICE(e, p, i)->down = 0;
        return;
    }
    unsigned slot = EXT_VOICES;
    for (unsigned i = 0; i < EXT_VOICES; ++i) if (VOICE(e, p, i)->note == note) { slot = i; break; }
    if (slot == EXT_VOICES && active_count(e, p) < COMMON(p)->limit)
        for (unsigned i = 0; i < EXT_VOICES; ++i) if (VOICE(e, p, i)->note == 255) { slot = i; break; }
    if (slot == EXT_VOICES) slot = oldest(e, p);
    ev_t *v = VOICE(e, p, slot);
    memset(v, 0, e->voice_stride);
    v->note = (uint8_t)note; v->down = 1; v->stage = 1; v->age = ++COMMON(p)->age;
    v->semis = (float)cents * 0.01f; v->env = 0;
    e->start(p, v, velocity);
}
void ext_midi(unsigned type, void *p, const uint8_t m[3])
{
    if (!eng(type)) return;
    unsigned st = m[0] & 0xf0U;
    if (st == 0xb0 && m[1] == 64) COMMON(p)->pedal = m[2] >= 64;
    else if (st == 0xe0) COMMON(p)->bend = (((int)m[1] + 128 * (int)m[2]) - 8192) * (2.0f / 8192.0f);
    else if (st == 0xb0 && m[1] == 121) { COMMON(p)->bend = 0; COMMON(p)->pedal = 0; }
    else if (st == 0xb0 && m[1] == 74) ext_set(type, p, type == EXT_FIRST + 3 ? 1 : 3, m[2]);
    else if (st == 0xb0 && m[1] == 72) ext_set(type, p, 7, m[2]);
    else if (st == 0xb0 && m[1] == 73) ext_set(type, p, 6, m[2]);
}
void ext_render(unsigned type, void *p, int32_t *out, unsigned frames)
{
    const struct engine *e = eng(type);
    memset(out, 0, frames * sizeof *out);
    if (e) e->render(p, out, frames);
}
void ext_set(unsigned type, void *p, unsigned parameter, unsigned value)
{
    const struct engine *e = eng(type);
    if (!e || parameter >= EXT_PARAMS || value > e->params[parameter].max) return;
    COMMON(p)->pot[parameter] = (uint8_t)value;
}
unsigned ext_get(unsigned type, const void *p, unsigned parameter)
{
    return eng(type) && parameter < EXT_PARAMS ? ((const struct ecommon *)p)->pot[parameter] : 0;
}
const fm1_param_t *ext_param(unsigned type, unsigned parameter)
{
    const struct engine *e = eng(type);
    return e && parameter < EXT_PARAMS ? &e->params[parameter] : NULL;
}
unsigned ext_active(unsigned type, const void *p) { const struct engine *e = eng(type); return e ? generic_active(e, p) : 0; }
unsigned ext_limit(unsigned type, void *p, unsigned voices)
{
    const struct engine *e = eng(type);
    if (!e || !voices || voices > EXT_VOICES) return 0;
    COMMON(p)->limit = (uint8_t)voices;
    unsigned retired = 0;
    while (active_count(e, p) > voices) { VOICE(e, p, oldest(e, p))->note = 255; ++retired; }
    return retired;
}
