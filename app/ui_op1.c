/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* TAPE-deck front panel for FM1: a tape-centred, four-colour-knob workflow
 * (tape / synth / sequencer / mixer / fx) built on the portable groove_* API.
 * Replaces screen.c when FM1_UI_OP1 is defined in version.h. Everything here
 * runs in the control callback; rendering is banded (16 rows) exactly like
 * screen.c so it fits the device's DMA buffer. */
#include "version.h"
#ifdef FM1_UI_OP1
#include "instrument.h"
#include "fm1_engine_control.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Palette                                                            */
/* ------------------------------------------------------------------ */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
enum {
    BG = 0x0000,
    INK = RGB(236, 236, 228),
    DIM = RGB(104, 108, 116),
    PANEL = RGB(26, 28, 34),
    PANEL2 = RGB(44, 47, 56),
    RED = RGB(255, 64, 56),
    /* the four encoder colours */
    C_BLUE = RGB(52, 120, 255),
    C_GREEN = RGB(60, 214, 120),
    C_WHITE = RGB(240, 240, 240),
    C_ORANGE = RGB(255, 140, 30)
};
static const uint16_t knob_color[4] = {C_BLUE, C_GREEN, C_WHITE, C_ORANGE};
static const uint16_t track_color[8] = {
    RGB(80, 150, 255), RGB(70, 220, 200), RGB(120, 230, 100), RGB(220, 230, 80),
    RGB(255, 170, 60), RGB(255, 110, 90), RGB(240, 100, 190), RGB(170, 120, 255)};

/* ------------------------------------------------------------------ */
/* Banded rasteriser                                                  */
/* ------------------------------------------------------------------ */
enum { ROWS = 16, BANDS = FM1_SCREEN_HEIGHT / ROWS, W = FM1_SCREEN_WIDTH };
_Static_assert(W * ROWS <= FM1_SCREEN_DMA_PIXELS, "LCD band capacity");
static uint16_t band_px[W * ROWS];
static unsigned band_top;

static void fill(int x, int y, int w, int h, uint16_t c)
{
    int y1 = y + h, x1 = x + w;
    if (x < 0) x = 0;
    if (x1 > W) x1 = W;
    if (y < (int)band_top) y = (int)band_top;
    if (y1 > (int)(band_top + ROWS)) y1 = (int)(band_top + ROWS);
    if (x >= x1 || y >= y1) return;
    for (int r = y; r < y1; ++r) {
        uint16_t *p = band_px + (r - (int)band_top) * W + x;
        for (int i = x; i < x1; ++i) *p++ = c;
    }
}
static void pix(int x, int y, uint16_t c) { fill(x, y, 1, 1, c); }
static void hline(int x, int y, int w, uint16_t c) { fill(x, y, w, 1, c); }
static void vline(int x, int y, int h, uint16_t c) { fill(x, y, 1, h, c); }
static void frame(int x, int y, int w, int h, uint16_t c)
{
    hline(x, y, w, c); hline(x, y + h - 1, w, c); vline(x, y, h, c); vline(x + w - 1, y, h, c);
}
static int iabs(int v) { return v < 0 ? -v : v; }
static unsigned isqrt(unsigned v)
{
    unsigned r = 0, bit = 1U << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}
/* Filled disc / ring through per-row spans; rows outside the band are skipped. */
static void disc(int cx, int cy, int r, uint16_t c)
{
    for (int y = cy - r; y <= cy + r; ++y) {
        if (y < (int)band_top || y >= (int)(band_top + ROWS)) continue;
        int dy = y - cy, dx = (int)isqrt((unsigned)(r * r - dy * dy));
        hline(cx - dx, y, 2 * dx + 1, c);
    }
}
static void ring(int cx, int cy, int r, uint16_t c)
{
    for (int y = cy - r; y <= cy + r; ++y) {
        if (y < (int)band_top || y >= (int)(band_top + ROWS)) continue;
        int dy = y - cy, dx = (int)isqrt((unsigned)(r * r - dy * dy));
        int dn = r > 0 ? (int)isqrt((unsigned)((r - 1) * (r - 1) - (dy * dy > (r - 1) * (r - 1) ? (r - 1) * (r - 1) : dy * dy))) : 0;
        if (iabs(dy) >= r - 1) hline(cx - dx, y, 2 * dx + 1, c);
        else { hline(cx - dx, y, dx - dn + 1, c); hline(cx + dn, y, dx - dn + 1, c); }
    }
}
static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    if ((y0 < y1 ? y0 : y1) >= (int)(band_top + ROWS) || (y0 > y1 ? y0 : y1) < (int)band_top) return;
    if (x0 == x1) { vline(x0, y0 < y1 ? y0 : y1, iabs(y1 - y0) + 1, c); return; }
    if (y0 == y1) { hline(x0 < x1 ? x0 : x1, y0, iabs(x1 - x0) + 1, c); return; }
    int dx = iabs(x1 - x0), dy = -iabs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) {
        pix(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

/* 5x7 font: digits, upper-case, a little punctuation. */
static const uint8_t font[36][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0, 0x42, 0x7f, 0x40, 0},       {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1e}, {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41},
    {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0, 0x41, 0x7f, 0x41, 0},       {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e},
    {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}};
static const uint8_t punct[][6] = {
    {'-', 8, 8, 8, 8, 8}, {'/', 0x60, 0x18, 6, 1, 0}, {'+', 8, 8, 0x3e, 8, 8},
    {'#', 0x14, 0x7f, 0x14, 0x7f, 0x14}, {'%', 0x63, 0x13, 8, 0x64, 0x63}, {'>', 0x41, 0x22, 0x14, 8, 0},
    {'<', 8, 0x14, 0x22, 0x41, 0}, {'.', 0, 0x60, 0x60, 0, 0}, {':', 0, 0x36, 0x36, 0, 0},
    {'!', 0, 0, 0x5f, 0, 0}, {'(', 0, 0x1c, 0x22, 0x41, 0}, {')', 0, 0x41, 0x22, 0x1c, 0},
    {'=', 0x14, 0x14, 0x14, 0x14, 0x14}, {'_', 0x40, 0x40, 0x40, 0x40, 0x40}, {'*', 0x14, 8, 0x3e, 8, 0x14}};
enum { NOBG = 0x10000 };
static int text(int x, int y, const char *s, int scale, uint16_t fg, uint32_t bg)
{
    int w = 6 * scale, h = 7 * scale;
    if (y + h <= (int)band_top || y >= (int)(band_top + ROWS)) return x + (int)strlen(s) * w;
    for (; *s; ++s, x += w) {
        if (x >= W) break;
        char l = *s >= 'a' && *s <= 'z' ? (char)(*s - 'a' + 'A') : *s;
        static const uint8_t blank[5] = {0};
        const uint8_t *shape = blank;
        if (l >= '0' && l <= '9') shape = font[l - '0'];
        else if (l >= 'A' && l <= 'Z') shape = font[10 + l - 'A'];
        else if (l != ' ')
            for (unsigned i = 0; i < sizeof punct / sizeof punct[0]; ++i)
                if (punct[i][0] == (uint8_t)l) { shape = punct[i] + 1; break; }
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row) {
                if (shape[col] & (1 << row)) fill(x + col * scale, y + row * scale, scale, scale, fg);
                else if (bg != NOBG) fill(x + col * scale, y + row * scale, scale, scale, (uint16_t)bg);
            }
        if (bg != NOBG) fill(x + 5 * scale, y, scale, h, (uint16_t)bg);
    }
    return x;
}
static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }
static void text_c(int cx, int y, const char *s, int scale, uint16_t fg) { text(cx - text_w(s, scale) / 2, y, s, scale, fg, NOBG); }
static void text_r(int rx, int y, const char *s, int scale, uint16_t fg) { text(rx - text_w(s, scale), y, s, scale, fg, NOBG); }
static void dec(char *out, int v)
{
    char t[8]; unsigned n = 0, neg = v < 0; unsigned u = (unsigned)(neg ? -v : v);
    do { t[n++] = (char)('0' + u % 10U); u /= 10U; } while (u && n < 7);
    unsigned o = 0; if (neg) out[o++] = '-';
    while (n) out[o++] = t[--n];
    out[o] = 0;
}
static void cat(char *dst, const char *s) { strcat(dst, s); }

/* Integer sine: phase 0..255 -> -127..127 (parabolic approximation). */
static int isin(unsigned phase)
{
    phase &= 255U;
    int x = (int)(phase & 127U) - 64;       /* -64..63 around the peak of a half wave */
    int v = 127 - ((x * x * 127) >> 12);   /* 0..127 parabola */
    return phase < 128U ? v : -v;
}

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */
enum page { P_TAPE, P_SYNTH, P_ENV, P_LFO, P_SEQ, P_MIX, P_FX, P_ARP, P_PROJECT, P_COUNT };
enum { OCT_DOWN, OCT_UP, FX, SCL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC };
#define BUTTON(b) (1U << (b))
static const uint8_t white_keys[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const char *const source_names[GROOVE_SOURCES] = {"DX7 FM", "BASS 303", "DRUM 808", "PSG CHIP", "PLUCK", "SAMPLER",
                                                         "WAVE", "PAD", "CLOUD", "KEYS"};
/* The sealed device engine only has the first six sources. */
#ifdef FM1_HOST_TEST
#define SOURCE_COUNT GROOVE_SOURCES
#else
#define SOURCE_COUNT GROOVE_SAMPLE + 1
#endif
static const char *const page_names[P_COUNT] = {"TAPE", "SYNTH", "ENVELOPE", "LFO", "SEQUENCER", "MIXER", "EFFECTS", "ARPEGGIO", "ALBUM"};

static unsigned page, track, cursor, writing, octave, drum, selected_note = 60, step_page;
static unsigned syn_page[GROOVE_TRACKS], drum_part[GROOVE_TRACKS];
static unsigned seq_sub, shift_down, tape_follow = 1;
static uint32_t prev_buttons, prev_keys, blocked_keys, boot_at, frame_at, notice_until, arp_ms;
static unsigned boot_active, arp_down, arp_long, retry_reset;
static const char *notice;
static uint8_t key_note[27], key_track[27];
static unsigned project_slot, project_bank, project_confirm;
static char project_names[GROOVE_PROJECT_SLOTS][13];
static int project_state[GROOVE_PROJECT_SLOTS];
static uint32_t hit_until[GROOVE_TRACKS];
static unsigned hit_seen[GROOVE_TRACKS], hit_level[GROOVE_TRACKS];
static int8_t encoder_dir[7];
static uint8_t encoder_speed[7];
static uint32_t encoder_time[7];
static char web_knobs[4][2][16];

/* Everything the painter needs, captured once per frame so all bands match. */
struct frame_state {
    struct groove_view view[GROOVE_TRACKS];
    uint64_t lanes[GROOVE_TRACKS];
    uint8_t tone[GROOVE_TONE_PARAMS], lfo[4], patch[156], fx[4], sends[2];
    struct groove_fm_controls fmc;
    uint8_t step_note[16];
    uint32_t now;
    unsigned page, track, cursor, writing, play_step, playing, tempo, length, octave, drum, step_page;
    unsigned part, syn_page, shift, boot, seq_sub, notice_on, source, arp_on, slot, bank, confirm;
    char notice[40];
    char names[GROOVE_PROJECT_SLOTS][13];
    int slot_state[GROOVE_PROJECT_SLOTS];
    uint8_t step_set[5], arp[6], project_dirty, project_slot_now;
    char project[13];
};
static struct frame_state fs;

static int post(uint8_t command, uint8_t a, uint8_t b, unsigned bytes)
{
    const uint8_t message[3] = {command, a, b};
    if (!groove_midi_local(message, bytes)) return 1;
    retry_reset = 1;
    return 0;
}
static void say(const char *s, uint32_t now) { notice = s; notice_until = now + 1800U; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static unsigned tone_part(void) { return groove_pad_count(track) != 0 ? drum_part[track] : 0; }

/* ------------------------------------------------------------------ */
/* DX7 parameter pages (offset (5-op)*21 applies to per-operator ids) */
/* ------------------------------------------------------------------ */
struct fm_slot { const char *label; uint8_t id, op, max; };
struct fm_page { const char *title; struct fm_slot s[4]; };
static const struct fm_page fm_pages[] = {
    {"ALGORITHM", {{"ALGO", 134, 0, 31}, {"FEEDBK", 135, 0, 7}, {"TRANS", 144, 0, 48}, {"LFO SPD", 137, 0, 99}}},
    {"OP LEVEL 1-4", {{"OP1", 16, 1, 99}, {"OP2", 16, 2, 99}, {"OP3", 16, 3, 99}, {"OP4", 16, 4, 99}}},
    {"OP LEVEL 5-6", {{"OP5", 16, 5, 99}, {"OP6", 16, 6, 99}, {"PMD", 139, 0, 99}, {"AMD", 140, 0, 99}}},
    {"RATIO 1-4", {{"OP1", 18, 1, 31}, {"OP2", 18, 2, 31}, {"OP3", 18, 3, 31}, {"OP4", 18, 4, 31}}},
    {"RATIO 5-6", {{"OP5", 18, 5, 31}, {"OP6", 18, 6, 31}, {"FINE5", 19, 5, 99}, {"FINE6", 19, 6, 99}}},
    {"DETUNE", {{"DET 1", 20, 1, 14}, {"DET 2", 20, 2, 14}, {"DET 3", 20, 3, 14}, {"DET 4", 20, 4, 14}}}};
enum { FM_PAGES = sizeof fm_pages / sizeof fm_pages[0] };
static unsigned fm_id(const struct fm_slot *s) { return s->id + (s->op ? (unsigned)(6 - s->op) * 21U : 0); }

static unsigned page_count(void)
{
    if (groove_source_type(track) == GROOVE_DX7) return FM_PAGES;
    return (groove_tone_count(track, tone_part()) + 3U) / 4U;
}

/* ------------------------------------------------------------------ */
/* Actions                                                            */
/* ------------------------------------------------------------------ */
static int encoder_step(unsigned i, int v, uint32_t now)
{
    if (!v) return 0;
    int dir = v > 0 ? 1 : -1;
    encoder_speed[i] = dir == encoder_dir[i] && now - encoder_time[i] <= 40U ? (uint8_t)clampi(encoder_speed[i] + 1, 1, 4) : 1;
    encoder_dir[i] = (int8_t)dir; encoder_time[i] = now;
    int a = iabs(v), gain = a < 2 ? 1 : a < 4 ? 2 : a == 4 ? 3 : 4;
    if (gain < encoder_speed[i]) gain = encoder_speed[i];
    return v * gain;
}
static void select_track(unsigned t)
{
    if (t >= GROOVE_TRACKS || t == track) return;
    track = t;
    cursor %= groove_sequence_length(track);
    drum = drum_part[track] < groove_pad_count(track) ? drum_part[track] : 0;
    if (groove_pad_count(track)) selected_note = groove_pad_note(track, drum);
    else if (selected_note < 24) selected_note = 60;
    if (syn_page[track] >= page_count()) syn_page[track] = 0;
    if (step_page * 16U >= groove_sequence_length(track)) step_page = 0;
}
static int release_keys(void)
{
    for (unsigned key = 0; key < 27; ++key)
        if (key_note[key] != 0xff) {
            if (!post((uint8_t)(0x80 + groove_midi_input_channel(key_track[key])), key_note[key], 0, 3)) return 0;
            key_note[key] = 0xff;
        }
    return 1;
}
static void go(unsigned target)
{
    if (target == page) return;
    page = target; blocked_keys |= prev_keys; release_keys();
    if (page == P_SEQ) step_page = (cursor / 16U) % 4U;
    if (page == P_PROJECT) {
        project_confirm = 0;
        for (unsigned i = 0; i < GROOVE_PROJECT_SLOTS; ++i)
            project_state[i] = project_bank ? 0 : groove_project_info(i, project_names[i]);
        for (unsigned i = 0; project_bank && i < GROOVE_PROJECT_SLOTS; ++i) {
            const char *n = groove_factory_name(i); unsigned k = 0;
            while (n && n[k] && k < 12) { project_names[i][k] = n[k]; ++k; }
            project_names[i][k] = 0;
        }
    }
}
static void toggle_step(unsigned step)
{
    struct groove_view v = {0};
    groove_get_view(track, step, &v);
    unsigned on = groove_pad_count(track) ? (v.step.drums & (1U << drum)) != 0 : v.step.velocity != 0;
    if (!groove_pad_count(track) && on) selected_note = v.step.note;
    groove_sequence_edit(track, step, selected_note, on ? 0 : 100);
    cursor = step;
}
/* Lift / drop / cut on the tape: copy, paste, clear a 16-step splice or the whole reel. */
static void tape_function(unsigned f, uint32_t now)
{
    int r = 0;
    switch (f) {
    case 0: r = groove_edit_copy(track, cursor, 1); say(r ? "LIFT FAILED" : "LIFTED SPLICE", now); break;
    case 1: r = groove_edit_paste(track, cursor, 1); say(r ? "NOTHING TO DROP" : "DROPPED SPLICE", now); break;
    case 2: r = groove_edit_clear(track, cursor, 1); say(r ? "CUT FAILED" : "CUT SPLICE", now); break;
    case 3: r = groove_edit_copy(track, cursor, 2); say(r ? "LIFT FAILED" : "LIFTED REEL", now); break;
    case 4: r = groove_edit_paste(track, cursor, 2); say(r ? "NOTHING TO DROP" : "DROPPED REEL", now); break;
    case 5: r = groove_edit_clear(track, cursor, 2); say(r ? "ERASE FAILED" : "REEL ERASED", now); break;
    case 6: r = groove_edit_undo(); say(r ? "NOTHING TO UNDO" : "UNDO / REDO", now); break;
    case 7: {
        unsigned len = groove_sequence_length(track);
        r = groove_edit_repeat(track, len < 64U ? len * 2U : 64U);
        say(r ? "CANNOT LOOP" : "REEL LOOPED", now); break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */
static void adjust_knob(unsigned k, int d, uint32_t now)
{
    unsigned type = groove_source_type(track);
    unsigned col = k;
    switch (page) {
    case P_TAPE:
        if (k == 0) groove_control_edit(track, 0, d, 0);
        else if (k == 1) groove_sends_edit(track, 0, d);
        else if (k == 2) groove_sends_edit(track, 1, d);
        else groove_tempo_edit(d);
        break;
    case P_MIX:
        if (k == 0) groove_control_edit(track, 0, d, 0);
        else if (k == 1) groove_sends_edit(track, 0, d);
        else if (k == 2) groove_sends_edit(track, 1, d);
        else groove_control_edit(track, 0, 0, d);
        break;
    case P_SYNTH:
        if (type == GROOVE_DX7) {
            const struct fm_slot *s = &fm_pages[syn_page[track]].s[col];
            groove_fm_edit(track, fm_id(s), d);
        } else {
            unsigned p = syn_page[track] * 4U + col;
            if (groove_tone_parameter(track, tone_part(), p)) groove_tone_edit(track, tone_part(), p, d);
        }
        break;
    case P_ENV:
        if (type == GROOVE_DX7) groove_adsr_edit(track, col, d);
        break;
    case P_LFO:
        if (type == GROOVE_DX7) {
            static const uint8_t lfo_id[4] = {142, 137, 139, 140};
            groove_fm_edit(track, lfo_id[col], d);
        } else groove_lfo_edit(track, col, d);
        break;
    case P_FX:
        if (col < 3) groove_effects_edit(col, d); else groove_effects_edit(3, d);
        break;
    case P_SEQ:
        if (col < 3) groove_step_setting_edit(track, cursor, col == 0 ? 1U : col == 1 ? 2U : 3U, d);
        else groove_sequence_setting_edit(0, d);
        break;
    case P_ARP:
        groove_arp_edit(track, col, d);
        break;
    default: break;
    }
    (void)now;
}

static int apply_controls(const struct fm1_controls *c, uint32_t now)
{
    uint32_t pressed = c->buttons & ~prev_buttons, key_down = c->keys & ~prev_keys;
    uint32_t old_keys = prev_keys;
    prev_buttons = c->buttons; prev_keys = c->keys;
    blocked_keys &= c->keys;
    shift_down = (c->buttons & BUTTON(SCL)) != 0;
    if (retry_reset) {
        blocked_keys = c->keys;
        if (!post(0xff, 0, 0, 1)) return 0;
        retry_reset = 0; memset(key_note, 0xff, sizeof key_note);
    }
    if (pressed || c->encoders[0] || c->encoders[6]) notice = notice && now < notice_until ? notice : NULL;
    /* panic */
    if ((c->buttons & (BUTTON(HOME) | BUTTON(PLAY))) == (BUTTON(HOME) | BUTTON(PLAY)) &&
        (pressed & (BUTTON(HOME) | BUTTON(PLAY)))) {
        blocked_keys |= c->keys;
        if (!post(0xff, 0, 0, 1)) return 0;
        memset(key_note, 0xff, sizeof key_note);
        writing = 0; page = P_TAPE; groove_edit_record(0, track, cursor);
        say("ALL STOP", now);
        return 1;
    }
    /* ARP: tap toggles, hold opens settings */
    if (pressed & BUTTON(ARP)) { arp_down = 1; arp_long = 0; arp_ms = now; }
    if (arp_down && !(c->buttons & BUTTON(ARP))) {
        if (!arp_long) {
            groove_arp_set(track, !groove_arp_enabled(track));
            say(groove_arp_enabled(track) ? "ARP ON  HOLD ARP FOR SETTINGS" : "ARP OFF", now);
        }
        arp_down = 0;
    }
    if (arp_down && !arp_long && now - arp_ms >= 600U) { arp_long = 1; go(page == P_ARP ? P_TAPE : P_ARP); }

    /* page buttons */
    if (pressed & BUTTON(HOME)) go(P_TAPE);
    if (pressed & BUTTON(EDIT)) {
        if (page == P_SYNTH) { if (page_count()) syn_page[track] = (syn_page[track] + 1U) % page_count(); }
        else go(P_SYNTH);
    }
    if (pressed & BUTTON(ENV)) go(page == P_ENV ? P_SYNTH : P_ENV);
    if (pressed & BUTTON(LFO)) go(page == P_LFO ? P_SYNTH : P_LFO);
    if (pressed & BUTTON(SEQ)) {
        if (page == P_SEQ) seq_sub ^= 1U; else go(P_SEQ);
    }
    if ((pressed & BUTTON(GLO))) go(page == P_MIX ? P_TAPE : P_MIX);
    if ((pressed & BUTTON(FX))) go(page == P_FX ? P_TAPE : P_FX);
    if ((pressed & BUTTON(SAVE))) {
        if (page != P_PROJECT) go(P_PROJECT);
        else if (shift_down) {
            int r = project_bank ? groove_factory_load(project_slot) : groove_project_load_slot(project_slot);
            say(r ? "LOAD FAILED" : "LOADED", now);
            if (!r) { cursor = 0; page = P_TAPE; }
        } else if (project_bank) say("FACTORY IS READ ONLY", now);
        else if (!project_confirm) { project_confirm = 1; say("SAVE AGAIN TO OVERWRITE", now); }
        else {
            int r = groove_project_save_slot(project_slot);
            say(r ? "SAVE FAILED" : "SAVED", now);
            project_confirm = 0; page = P_TAPE;
        }
    }

    /* transport */
    if ((pressed & BUTTON(REC))) { writing ^= 1U; if (writing && page == P_SYNTH) {} }
    groove_edit_record(writing, track, cursor);
    if (pressed & BUTTON(PLAY)) {
        struct groove_view v = {0};
        groove_get_view(track, cursor, &v);
        if (shift_down) {
            if (!post(0xfc, 0, 0, 1)) return 0;
            cursor = 0; groove_edit_seek(track, 0); writing = 0; groove_edit_record(0, track, 0);
            say("REWOUND", now);
        } else {
            uint8_t command = v.playing ? 0xfc : (writing && groove_sequence_setting(2) && groove_sequence_rewound() ? 0xfa : 0xfb);
            if (!post(command, 0, 0, 1)) return 0;
        }
    }
    /* octave / reel transport */
    if (pressed & (BUTTON(OCT_DOWN) | BUTTON(OCT_UP))) {
        int dir = (pressed & BUTTON(OCT_UP)) ? 1 : -1;
        if (page == P_TAPE || page == P_SEQ) {
            unsigned len = groove_sequence_length(track);
            int next = clampi((int)(cursor / 16U) + dir, 0, (int)(len / 16U) - 1);
            cursor = (unsigned)next * 16U; step_page = (unsigned)next;
            groove_edit_seek(track, cursor);
            tape_follow = 0;
        } else octave = (unsigned)clampi((int)octave + dir, -3 + 3, 6);
    }

    /* encoders */
    unsigned type = groove_source_type(track);
    if (c->encoders[0]) {
        if (page == P_SEQ) cursor = (unsigned)clampi((int)cursor + c->encoders[0], 0, (int)groove_sequence_length(track) - 1), step_page = cursor / 16U;
        else select_track((unsigned)clampi((int)track + c->encoders[0], 0, 7));
    }
    if (c->encoders[1]) {
        if (page == P_TAPE || page == P_SEQ) {
            unsigned len = groove_sequence_length(track) / 16U;
            len = (unsigned)clampi((int)len + (c->encoders[1] > 0 ? 1 : -1), 1, 4);
            groove_sequence_length_set(track, len * 16U);
        } else groove_control_edit(track, 0, c->encoders[1], 0);
    }
    for (unsigned k = 0; k < 4; ++k) {
        int d = encoder_step(2 + k, c->encoders[2 + k], now);
        if (d) adjust_knob(k, d, now);
    }
    if (c->encoders[6]) {
        int d = c->encoders[6];
        switch (page) {
        case P_TAPE: {
            struct groove_view v = {0}; groove_get_view(track, cursor, &v);
            cursor = (unsigned)clampi((int)cursor + d, 0, (int)v.sequence_length - 1);
            groove_edit_seek(track, cursor); tape_follow = 0; step_page = cursor / 16U;
            break;
        }
        case P_SYNTH: case P_ENV: case P_LFO:
            if (shift_down) {
                unsigned next = (unsigned)((int)type + (d > 0 ? 1 : -1) + SOURCE_COUNT) % SOURCE_COUNT;
                if (!release_keys()) return 0;
                if (groove_source_set(track, next)) say("ENGINE CHANGE FAILED", now);
                else {
                    syn_page[track] = 0; drum = 0; drum_part[track] = 0;
                    selected_note = groove_pad_count(track) ? groove_pad_note(track, 0) : 60;
                    say(source_names[next], now);
                }
            } else if (page == P_SYNTH) {
                if (page_count()) syn_page[track] = (syn_page[track] + (d > 0 ? 1U : page_count() - 1U)) % page_count();
            } else if (type == GROOVE_DX7) groove_adsr_enable(track, d > 0);
            break;
        case P_SEQ: step_page = (unsigned)clampi((int)step_page + (d > 0 ? 1 : -1), 0, (int)groove_sequence_length(track) / 16 - 1); break;
        case P_ARP: groove_arp_edit(track, shift_down ? 5U : 4U, d); break;
        case P_PROJECT:
            if (shift_down) { project_bank ^= 1U; page = P_TAPE; go(P_PROJECT); }
            else project_slot = (unsigned)clampi((int)project_slot + (d > 0 ? 1 : -1), 0, GROOVE_PROJECT_SLOTS - 1);
            project_confirm = 0;
            break;
        case P_FX: groove_tempo_edit(d); break;
        default: break;
        }
    }

    /* keys */
    uint32_t keys = c->keys & ~blocked_keys;
    if (shift_down) {
        for (unsigned i = 0; i < 16; ++i)
            if (key_down & keys & (1U << white_keys[i])) {
                if (i < 8) select_track(i);
                else if (page == P_TAPE) tape_function(i - 8U, now);
            }
        keys = 0;
        blocked_keys |= c->keys;
    } else if (page == P_SEQ && !writing) {
        for (unsigned i = 0; i < 16; ++i)
            if (key_down & keys & (1U << white_keys[i])) {
                if (seq_sub) cursor = step_page * 16U + i; else toggle_step(step_page * 16U + i);
            }
        if (!seq_sub) keys = 0;
    } else if (page == P_MIX) {
        for (unsigned i = 0; i < 16; ++i)
            if (key_down & keys & (1U << white_keys[i])) groove_mix_toggle(i & 7U, i >= 8);
        keys = 0;
    } else if (page == P_PROJECT) {
        for (unsigned i = 0; i < 16; ++i)
            if (key_down & keys & (1U << white_keys[i])) { project_slot = i; project_confirm = 0; }
        keys = 0;
    }
    for (unsigned key = 0; key < 27; ++key) {
        unsigned note = groove_key_note(track, key, octave);
        if (key_note[key] != 0xff && (!(keys & (1U << key)) || key_note[key] != note || key_track[key] != track)) {
            if (!post((uint8_t)(0x80 + groove_midi_input_channel(key_track[key])), key_note[key], 0, 3)) return 0;
            key_note[key] = 0xff;
        }
        if (note < 128 && (keys & (1U << key)) && key_note[key] == 0xff) {
            if (!post((uint8_t)(0x90 + groove_midi_input_channel(track)), (uint8_t)note, 100, 3)) return 0;
            key_note[key] = (uint8_t)note; key_track[key] = (uint8_t)track;
            selected_note = note;
            if (groove_pad_count(track)) {
                int idx = groove_pad_index(track, note);
                if (idx >= 0) { drum = (unsigned)idx; drum_part[track] = (uint8_t)idx; }
            }
        }
    }
    (void)old_keys;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Frame capture                                                      */
/* ------------------------------------------------------------------ */
static const char *const fx_sync[9] = {"1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/8D", "1/4"};
static void delay_label(unsigned v, char *out)
{
    out[0] = 0;
    if (v >= GROOVE_DELAY_SYNC && v <= GROOVE_DELAY_LAST) { cat(out, fx_sync[v - GROOVE_DELAY_SYNC]); return; }
    unsigned ms = v < 128 ? v * 8U : v <= 201 ? 360U + (v - 137U) * 10U : 1020U + (v - 202U) * 20U;
    dec(out, (int)ms); cat(out, "MS");
}
static void capture(uint32_t now)
{
    memset(&fs, 0, sizeof fs);
    fs.now = now; fs.page = page; fs.track = track; fs.cursor = cursor; fs.writing = writing;
    fs.octave = octave; fs.drum = drum; fs.step_page = step_page; fs.shift = shift_down;
    fs.boot = boot_active; fs.seq_sub = seq_sub; fs.part = tone_part(); fs.syn_page = syn_page[track];
    fs.source = groove_source_type(track); fs.arp_on = groove_arp_enabled(track);
    fs.slot = project_slot; fs.bank = project_bank; fs.confirm = project_confirm;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        groove_get_view(t, t == track ? cursor : 0, &fs.view[t]);
        uint64_t lane = fs.view[t].active_steps;
        if (groove_pad_count(t))
            for (unsigned d = 0; d < GROOVE_DRUMS; ++d) lane |= groove_drum_steps(t, d);
        fs.lanes[t] = lane;
        unsigned n = groove_midi_activity(t);
        if (n != hit_seen[t]) { hit_seen[t] = n; hit_until[t] = now + 160U; }
        hit_level[t] = (int32_t)(hit_until[t] - now) > 0 ? (hit_until[t] - now) : 0;
    }
    struct groove_view *v = &fs.view[track];
    fs.play_step = v->play_step; fs.playing = v->playing; fs.tempo = v->tempo; fs.length = v->sequence_length;
    if (notice && (int32_t)(notice_until - now) > 0) {
        strncpy(fs.notice, notice, sizeof fs.notice - 1); fs.notice_on = 1;
    }
    groove_project_name(fs.project);
    fs.project_dirty = (uint8_t)groove_project_dirty();
    groove_effects_get(fs.fx); groove_sends_get(track, fs.sends);
    for (unsigned i = 0; i < 5; ++i) fs.step_set[i] = (uint8_t)groove_step_setting(track, cursor, i);
    for (unsigned i = 0; i < 6; ++i) fs.arp[i] = (uint8_t)groove_arp_setting(track, i);
    if (page == P_SEQ) {
        struct groove_step cells[16];
        groove_sequence_read(track, step_page * 16U, cells, 16);
        for (unsigned i = 0; i < 16; ++i) fs.step_note[i] = cells[i].note;
    }
    if (fs.source == GROOVE_DX7) groove_fm_copy(track, fs.patch, &fs.fmc);
    else {
        groove_tone_copy(track, fs.part, fs.tone);
        groove_lfo_copy(track, fs.lfo);
    }
    if (page == P_PROJECT) {
        memcpy(fs.names, project_names, sizeof fs.names);
        memcpy(fs.slot_state, project_state, sizeof fs.slot_state);
    }
}

/* ------------------------------------------------------------------ */
/* Drawing helpers                                                    */
/* ------------------------------------------------------------------ */
static void note_name(unsigned note, char *out)
{
    static const char *const n[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    out[0] = 0; cat(out, n[note % 12U]);
    char t[8]; dec(t, (int)(note / 12U) - 1); cat(out, t);
}
static void header(const char *title, uint16_t color)
{
    fill(0, 0, W, 20, PANEL);
    fill(0, 20, W, 1, color);
    text(6, 6, title, 1, color, NOBG);
    char s[24]; s[0] = 0; cat(s, "T"); char t[4]; dec(t, (int)fs.track + 1); cat(s, t);
    cat(s, " "); cat(s, source_names[fs.source]);
    text_c(120, 6, s, 1, track_color[fs.track]);
    char b[12]; dec(b, (int)fs.tempo); cat(b, "BPM");
    text_r(W - 6 - (fs.writing ? 30 : 0), 6, b, 1, INK);
    if (fs.writing) {
        disc(W - 10, 10, 4, ((fs.now / 300U) & 1U) ? RED : RGB(120, 20, 20));
    }
}
/* One of the four coloured encoder read-outs along the bottom edge. */
static void knob_cell(unsigned i, const char *label, const char *value, unsigned fraction)
{
    int x = 4 + (int)i * 59;
    if (i < 4) {
        memset(web_knobs[i], 0, sizeof web_knobs[i]);
        strncpy(web_knobs[i][0], label, 15); strncpy(web_knobs[i][1], value, 15);
    }
    fill(x, 198, 54, 38, PANEL);
    fill(x, 198, 54, 3, knob_color[i]);
    text(x + 3, 204, label, 1, DIM, NOBG);
    text(x + 3, 214, value, strlen(value) <= 4 ? 2 : 1, INK, NOBG);
    fill(x + 3, 230, 48, 3, PANEL2);
    fill(x + 3, 230, (int)(fraction * 48U / 100U), 3, knob_color[i]);
}
static void hints(const char *s) { text_c(120, 186, s, 1, DIM); }
static void pad_bar(unsigned i, const char *label, unsigned value, unsigned max, int shown)
{
    char v[12]; dec(v, shown);
    knob_cell(i, label, v, max ? value * 100U / max : 0);
}

/* --- Tape page --------------------------------------------------- */
static void reel(int cx, int cy, int hub_r, int wound_r, unsigned angle, uint16_t col)
{
    disc(cx, cy, wound_r, PANEL2);
    ring(cx, cy, wound_r, col);
    disc(cx, cy, hub_r, BG);
    ring(cx, cy, hub_r, INK);
    for (unsigned i = 0; i < 3; ++i) {
        unsigned a = angle + i * 85U;
        int sx = (isin(a + 64) * (hub_r - 2)) / 127, sy = (isin(a) * (hub_r - 2)) / 127;
        line(cx, cy, cx + sx, cy + sy, INK);
    }
}
static void draw_tape(void)
{
    header("TAPE", C_ORANGE);
    unsigned len = fs.length ? fs.length : 16;
    unsigned pos = fs.playing || fs.writing ? fs.play_step : fs.cursor;
    int wl = 12 + (int)((len - pos) * 24U / len), wr = 12 + (int)(pos * 24U / len);
    unsigned angle = fs.playing ? (fs.now / 6U) & 255U : (fs.now / 1200U) & 255U;
    reel(52, 62, 11, wl, angle, track_color[fs.track]);
    reel(188, 62, 11, wr, angle, track_color[fs.track]);
    /* tape path */
    line(52 + wl - 2, 62 + 4, 96, 96, DIM); line(188 - wr + 2, 62 + 4, 144, 96, DIM);
    hline(96, 96, 48, DIM);
    /* counter */
    unsigned bpm = fs.tempo ? fs.tempo : 120U;
    unsigned tenths = pos * 150U / bpm;                /* 16th note = 15/bpm seconds */
    char c[16]; c[0] = 0; char t[8];
    dec(t, (int)(tenths / 600U)); cat(c, t); cat(c, ":");
    dec(t, (int)((tenths / 10U) % 60U)); if (((tenths / 10U) % 60U) < 10U) cat(c, "0"); cat(c, t);
    text_c(120, 54, c, 2, INK);
    char bar[16]; bar[0] = 0; dec(t, (int)(pos / 16U) + 1); cat(bar, t); cat(bar, ".");
    dec(t, (int)((pos % 16U) / 4U) + 1); cat(bar, t);
    text_c(120, 74, bar, 1, DIM);
    text_c(120, 26, fs.project, 1, fs.project_dirty ? C_ORANGE : DIM);
    /* lanes */
    const int x0 = 18, lw = 218;
    for (unsigned t8 = 0; t8 < GROOVE_TRACKS; ++t8) {
        int y = 102 + (int)t8 * 11;
        const struct groove_view *v = &fs.view[t8];
        unsigned l = v->sequence_length ? v->sequence_length : 16;
        uint16_t col = track_color[t8];
        if (v->muted || (!v->audible && v->solo == 0)) col = DIM;
        char n[2] = {(char)('1' + t8), 0};
        fill(0, y, W, 10, t8 == fs.track ? PANEL2 : PANEL);
        if (t8 == fs.track) fill(0, y, 2, 10, track_color[t8]);
        text(5, y + 1, n, 1, t8 == fs.track ? track_color[t8] : DIM, NOBG);
        for (unsigned s = 0; s < l; ++s) {
            int sx = x0 + (int)(s * (unsigned)lw / l);
            int sw = (int)((s + 1) * (unsigned)lw / l) - (int)(s * (unsigned)lw / l);
            if ((s & 15U) == 0 && s) vline(sx, y, 10, DIM);
            if ((fs.lanes[t8] >> s) & 1U) fill(sx, y + 2, sw > 1 ? sw - 1 : 1, 6, col);
        }
        if (v->playing) {
            int hx = x0 + (int)(v->play_step * (unsigned)lw / l);
            vline(hx, y, 10, INK);
        }
        if (hit_level[t8]) disc(W - 5, y + 5, 2, col);
        if (t8 == fs.track && fs.writing) disc(W - 12, y + 5, 3, RED);
    }
    /* cursor marker when parked */
    if (!fs.playing) {
        int hx = x0 + (int)(fs.cursor * (unsigned)lw / (len ? len : 1));
        fill(hx - 1, 98, 3, 4, C_ORANGE);
    }
    char v1[12], v2[12], v3[12], v4[12];
    dec(v1, fs.view[track].volume);
    dec(v2, fs.sends[0]); dec(v3, fs.sends[1]); dec(v4, (int)fs.tempo);
    knob_cell(0, "LEVEL", v1, fs.view[track].volume * 100U / 127U);
    knob_cell(1, "REVERB", v2, fs.sends[0] * 100U / 15U);
    knob_cell(2, "DELAY", v3, fs.sends[1] * 100U / 15U);
    knob_cell(3, "TEMPO", v4, (fs.tempo > 30 ? fs.tempo - 30U : 0U) * 100U / 270U);
    if (fs.shift) {
        fill(0, 186, W, 11, PANEL);
        text(2, 188, "1-8 TRK 9 LIFT 10 DROP 11 CUT 12-14 REEL 15 UNDO 16 LOOP", 1, C_ORANGE, NOBG);
    }
}

/* --- Engine visuals ---------------------------------------------- */
static void wave_draw(int x, int y, int w, int h, unsigned kind, unsigned amount, unsigned phase, unsigned energy, uint16_t col)
{
    int prev = 0;
    int mid = y + h / 2, amp = (int)(h / 2 - 2) * (int)(25 + energy * 3 / 4) / 100;
    for (int i = 0; i < w; ++i) {
        unsigned ph = phase + (unsigned)i * (2U + amount / 40U);
        int v;
        switch (kind) {
        case 0: v = isin(ph + (unsigned)(isin(ph * 2U + phase) * (int)amount / 800)); break;           /* FM */
        case 1: v = (int)(ph & 255U) - 128; v = v * (int)(255 - amount) / 255 + isin(ph) * (int)amount / 255; break; /* acid saw */
        case 2: v = ((ph & 255U) < 128U ? 127 : -127) * (int)(255 - amount / 2) / 255; break;           /* square */
        default: v = isin(ph); break;
        }
        int py = mid - v * amp / 127;
        if (i) line(x + i - 1, prev, x + i, py, col);
        prev = py;
    }
}
static void draw_engine_visual(int x, int y, int w, int h, unsigned type, const uint8_t *tone, const uint8_t *patch,
                               unsigned level, unsigned hit, uint16_t col, uint32_t now)
{
    fill(x, y, w, h, PANEL);
    frame(x, y, w, h, PANEL2);
    unsigned energy = level > 100 ? 100 : level;
    if (hit > energy) energy = hit * 100U / 160U;
    unsigned ph = (now / 8U) & 255U;
    int cx = x + w / 2, cy = y + h / 2;
    switch (type) {
    case GROOVE_DX7: {
        unsigned fb = patch[135], alg = patch[134];
        for (unsigned k = 0; k < 3; ++k)
            wave_draw(x + 4, y + 4 + (int)k * 2, w - 8, h - 8 - (int)k * 4, 0, 40U + fb * 20U + alg, ph + k * 21U, energy, k == 0 ? col : (k == 1 ? C_BLUE : DIM));
        break;
    }
    case GROOVE_303: {
        unsigned cutoff = tone[0] > 100 ? 100 : tone[0];
        wave_draw(x + 4, y + 4, w - 8, h - 8, 1, 255 - cutoff * 2U, ph, energy, col);
        int sweep = x + 6 + (int)(cutoff * (unsigned)(w - 12) / 100U);
        vline(sweep, y + 3, h - 6, C_ORANGE);
        break;
    }
    case GROOVE_808: {
        for (unsigned r = 0; r < 4; ++r) {
            int rad = 8 + (int)(r * 12U) + (int)(energy * (r + 1U) / 12U);
            if (rad < h / 2) ring(cx, cy, rad, r == 0 ? col : (r == 1 ? C_BLUE : PANEL2));
        }
        disc(cx, cy, 4 + (int)(energy / 10U), col);
        break;
    }
    case GROOVE_PSG: {
        for (unsigned ch = 0; ch < 3; ++ch) {
            int yy = y + 8 + (int)ch * (h - 14) / 3, hh = (h - 20) / 3;
            unsigned period = 14U + ch * 9U + (tone[1] & 7U) * 2U;
            for (int i = 0; i < w - 8; i += 2) {
                unsigned p = ((unsigned)i + now / (10U + ch * 3U)) % period;
                int up = p < period / 2U;
                fill(x + 4 + i, yy + (up ? 0 : hh * (int)(100 - energy / 2) / 100 / 2 + 2), 2, 2 + (int)(hh * energy / 150U), ch == 0 ? col : (ch == 1 ? C_GREEN : C_BLUE));
            }
        }
        break;
    }
    case GROOVE_PLUCK: {
        int decay = (int)(100 - tone[0] % 100);
        int amp = (int)(h / 2 - 6) * (int)energy / 100;
        for (int i = 0; i < w - 8; ++i) {
            int env = isin((unsigned)(i * 128 / (w - 8))) ;
            int v = isin(ph * 4U + (unsigned)i * 6U) * env / 127;
            int py = cy - v * amp / 127 * decay / 100 - 0;
            if (i) pix(x + 4 + i, py, col);
        }
        hline(x + 4, cy, w - 8, PANEL2);
        break;
    }
    case GROOVE_WAVE: {
        unsigned morph = tone[0], warp = tone[1];
        wave_draw(x + 4, y + 4, w - 8, h - 8, morph < 20 ? 3 : morph < 50 ? 1 : morph < 90 ? 2 : 0, 30U + morph, ph + warp, energy, col);
        wave_draw(x + 4, y + 4, w - 8, h - 8, 2, 20U + warp * 2U, ph * 2U, energy / 2U, C_BLUE);
        break;
    }
    case GROOVE_PAD: {
        for (unsigned k = 0; k < 5; ++k) {
            int yy = y + 14 + (int)k * 14;
            for (int i = 0; i < w - 8; i += 2) {
                int off = (isin((unsigned)(i * 3) + ph + k * 50U) * (int)(2 + tone[0] / 24U)) / 127;
                int amp = (int)(4 + energy / 12U);
                fill(x + 4 + i, yy + off - amp / 2, 2, amp, k & 1 ? col : C_GREEN);
            }
        }
        break;
    }
    case GROOVE_CLOUD: {
        for (unsigned n = 0; n < 7U + tone[2] / 8U; ++n) {
            unsigned sd = n * 2654435761U + (now / (180U + (127U - tone[1]) * 4U)) * 40503U;
            int gx = x + 6 + (int)((sd >> 8) % (unsigned)(w - 12)), gy = y + 6 + (int)((sd >> 20) % (unsigned)(h - 12));
            int gr = 3 + (int)(((sd >> 4) & 7U) * (1U + tone[1] / 32U));
            if (energy || n < 5) ring(gx, gy, gr, (n & 3U) == 0 ? C_WHITE : (n & 1U) ? col : C_BLUE);
        }
        break;
    }
    case GROOVE_KEYS: {
        for (int i = 0; i < 12; ++i) {
            int kx = x + 8 + i * ((w - 16) / 12);
            int down = (int)((energy * 12U / 100U) > (unsigned)i && ((i * 5 + (int)(now / 90U)) & 3) != 0);
            fill(kx, y + 10, (w - 16) / 12 - 2, h - 22, down ? col : INK);
            if (i % 7 != 2 && i % 7 != 6) fill(kx + (w - 16) / 24, y + 10, (w - 16) / 18, (h - 22) * 6 / 10, BG);
        }
        break;
    }
    default: {
        for (int i = 0; i < 20; ++i) {
            int hgt = 6 + (isin((unsigned)(i * 13) + ph * 2U) * (int)(h / 2 - 10) / 127 * (int)(20 + energy) / 120);
            if (hgt < 3) hgt = 3 + (hgt < 0 ? -hgt : 0);
            fill(x + 6 + i * ((w - 12) / 20), cy - hgt / 2, (w - 12) / 20 - 2, hgt, i & 1 ? col : C_BLUE);
        }
        break;
    }
    }
}

/* --- Synth / env / lfo -------------------------------------------- */
static void draw_synth_like(void)
{
    uint16_t col = track_color[fs.track];
    header(page_names[fs.page], col);
    const struct groove_view *v = &fs.view[fs.track];
    char t[24];
    text(6, 26, source_names[fs.source], 2, col, NOBG);
    unsigned pages = fs.source == GROOVE_DX7 ? (unsigned)FM_PAGES : (groove_tone_count(fs.track, fs.part) + 3U) / 4U;
    if (fs.page == P_SYNTH) {
        t[0] = 0; dec(t, (int)fs.syn_page + 1); cat(t, "/"); char u[8]; dec(u, (int)pages); cat(t, u);
        text_r(W - 6, 28, t, 1, INK);
    }
    draw_engine_visual(6, 46, W - 12, 96, fs.source, fs.tone, fs.patch, v->level, hit_level[fs.track], col, fs.now);
    if (groove_pad_count(fs.track) && fs.source != GROOVE_DX7) {
        t[0] = 0; cat(t, groove_pad_name(fs.track, fs.part));
        text_c(120, 148, t, 1, INK);
    } else if (fs.page == P_SYNTH && fs.source == GROOVE_DX7) {
        text_c(120, 148, fm_pages[fs.syn_page % FM_PAGES].title, 1, INK);
    }
    char names[24];
    if (fs.page == P_SYNTH) {
        note_name(selected_note, names);
        text(6, 162, names, 1, DIM, NOBG);
        t[0] = 0; cat(t, "OCT "); char u[8]; dec(u, (int)fs.octave); cat(t, u);
        text_r(W - 6, 162, t, 1, DIM);
    }
    hints(fs.page == P_SYNTH ? "E6 PAGE  SHIFT+E6 ENGINE"
          : fs.page == P_ENV ? "E6 BYPASS  SHIFT+E6 ENGINE" : "E2-E5 EDIT");
    for (unsigned i = 0; i < 4; ++i) {
        char lab[16], val[16];
        unsigned frac = 0;
        lab[0] = val[0] = 0;
        if (fs.page == P_SYNTH && fs.source == GROOVE_DX7) {
            const struct fm_slot *s = &fm_pages[fs.syn_page % FM_PAGES].s[i];
            unsigned raw = fs.patch[fm_id(s)];
            cat(lab, s->label); dec(val, (int)raw); frac = raw * 100U / s->max;
        } else if (fs.page == P_SYNTH) {
            unsigned p = fs.syn_page * 4U + i;
            const fm1_param_t *pp = groove_tone_parameter(fs.track, fs.part, p);
            if (pp) {
                unsigned raw = fs.tone[p];
                cat(lab, pp->name);
                if (pp->names) cat(val, pp->names[raw]); else dec(val, (int)raw);
                frac = pp->max ? raw * 100U / pp->max : 0;
            }
        } else if (fs.page == P_ENV && fs.source == GROOVE_DX7) {
            static const char *const en[4] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"};
            cat(lab, en[i]); dec(val, fs.fmc.envelope[i]); frac = fs.fmc.envelope[i] * 100U / 99U;
        } else if (fs.page == P_ENV) {
            if (i == 0) { cat(lab, "SOURCE"); cat(val, "SHARED"); }
        } else if (fs.page == P_LFO && fs.source == GROOVE_DX7) {
            static const char *const ln[4] = {"WAVE", "SPEED", "PMD", "AMD"};
            static const uint8_t id[4] = {142, 137, 139, 140};
            cat(lab, ln[i]); dec(val, fs.patch[id[i]]); frac = fs.patch[id[i]] * 100U / (i ? 99U : 5U);
        } else if (fs.page == P_LFO) {
            static const char *const ln[4] = {"RATE", "DEPTH", "TARGET", "WAVE"};
            static const char *const tgt[2] = {"VOL", "PAN"}, *const wav[4] = {"TRI", "SQR", "UP", "DOWN"};
            cat(lab, ln[i]);
            if (i == 2) cat(val, tgt[fs.lfo[2] & 1U]); else if (i == 3) cat(val, wav[fs.lfo[3] & 3U]); else dec(val, fs.lfo[i]);
            frac = i < 2 ? fs.lfo[i] : 50U;
        }
        knob_cell(i, lab, val, frac);
    }
}

/* --- Sequencer ---------------------------------------------------- */
static void draw_seq(void)
{
    uint16_t col = track_color[fs.track];
    header(fs.seq_sub ? "STEP DETAIL" : "SEQUENCER", col);
    const int cx = 120, cy = 98, R = 62;
    ring(cx, cy, R + 14, PANEL2);
    for (unsigned i = 0; i < 16; ++i) {
        unsigned step = fs.step_page * 16U + i;
        int sx = cx + isin(i * 16U) * R / 127, sy = cy - isin(i * 16U + 64U) * R / 127;
        if (step >= fs.length) { ring(sx, sy, 6, PANEL2); continue; }
        int on = (int)((fs.lanes[fs.track] >> step) & 1U);
        int head = fs.playing && fs.play_step == step;
        if (on) disc(sx, sy, 9, head ? INK : col); else ring(sx, sy, 8, head ? INK : DIM);
        if (step == fs.cursor) ring(sx, sy, 11, C_ORANGE);
        if ((i & 3U) == 0) pix(sx, sy, BG);
    }
    char t[24]; char u[12];
    note_name(fs.cursor < 64 ? fs.view[fs.track].step.note : 60, t);
    if (groove_pad_count(fs.track)) { t[0] = 0; cat(t, groove_pad_label(fs.track, fs.drum)); }
    text_c(cx, cy - 14, t, 2, INK);
    t[0] = 0; cat(t, "STEP "); dec(u, (int)fs.cursor + 1); cat(t, u); text_c(cx, cy + 6, t, 1, DIM);
    t[0] = 0; cat(t, "PAGE "); dec(u, (int)fs.step_page + 1); cat(t, u); cat(t, "/"); dec(u, (int)fs.length / 16); cat(t, u);
    text_c(cx, cy + 18, t, 1, col);
    hints(fs.seq_sub ? "KEYS PICK STEP  SEQ BACK" : "KEYS TOGGLE  OCT PAGE  SEQ DETAIL");
    static const char *const prob[8] = {"ALWAYS", "1/8", "2/8", "3/8", "4/8", "5/8", "6/8", "7/8"};
    char v1[12], v2[12], v3[12], v4[12];
    v1[0] = 0; dec(v1, fs.step_set[1] ? (int)(fs.step_set[1] * 100U / 15U) : 50); cat(v1, "%");
    v2[0] = 0; cat(v2, prob[fs.step_set[2] & 7U]);
    v3[0] = 0; if (groove_pad_count(fs.track)) cat(v3, "-"); else dec(v3, fs.step_set[3]);
    v4[0] = 0; dec(v4, (int)groove_sequence_setting(0)); cat(v4, "%");
    knob_cell(0, "GATE", v1, fs.step_set[1] * 100U / 15U);
    knob_cell(1, "PROB", v2, fs.step_set[2] * 100U / 7U);
    knob_cell(2, "CHORD", v3, fs.step_set[3] * 100U / 16U);
    knob_cell(3, "SWING", v4, groove_sequence_setting(0) * 2U);
}

/* --- Mixer -------------------------------------------------------- */
static void draw_mix(void)
{
    header("MIXER", C_GREEN);
    for (unsigned t = 0; t < 8; ++t) {
        const struct groove_view *v = &fs.view[t];
        int x = 8 + (int)t * 29, top = 30, hh = 120;
        uint16_t col = v->muted ? DIM : track_color[t];
        fill(x, top, 22, hh, PANEL);
        int level = (int)(v->volume * (unsigned)hh / 127U);
        fill(x + 2, top + hh - level, 18, level, col);
        int meter = (int)((v->level > 100 ? 100U : v->level) * (unsigned)hh / 100U);
        fill(x + 8, top + hh - meter, 6, meter, INK);
        if (t == fs.track) frame(x - 2, top - 2, 26, hh + 4, INK);
        char n[2] = {(char)('1' + t), 0};
        text_c(x + 11, top + hh + 6, n, 1, col);
        fill(x + 2, top + hh + 18, 8, 8, v->muted ? RED : PANEL2);
        text(x + 4, top + hh + 19, "M", 1, v->muted ? BG : DIM, NOBG);
        fill(x + 12, top + hh + 18, 8, 8, v->solo ? C_ORANGE : PANEL2);
        text(x + 14, top + hh + 19, "S", 1, v->solo ? BG : DIM, NOBG);
    }
    char v1[12], v2[12], v3[12]; dec(v1, fs.view[fs.track].volume); dec(v2, fs.sends[0]); dec(v3, fs.sends[1]);
    knob_cell(0, "LEVEL", v1, fs.view[fs.track].volume * 100U / 127U);
    knob_cell(1, "REVERB", v2, fs.sends[0] * 100U / 15U);
    knob_cell(2, "DELAY", v3, fs.sends[1] * 100U / 15U);
    knob_cell(3, "CUTOFF", "", 50);
    hints("KEYS 1-8 MUTE  9-16 SOLO");
}

/* --- FX ---------------------------------------------------------- */
static void draw_fx(void)
{
    header("EFFECTS", C_BLUE);
    unsigned decay = fs.fx[0], fb = fs.fx[2], ret = fs.fx[3];
    int cx = 120, cy = 92;
    for (unsigned r = 0; r < 6; ++r) {
        unsigned ph = (fs.now / 30U + r * 40U) % 240U;
        int rad = 6 + (int)(ph * (10U + decay / 4U) / 100U);
        if (rad < 66) ring(cx, cy, rad, ph < 120U ? C_BLUE : PANEL2);
    }
    /* echo trail */
    for (unsigned e = 0; e < 6; ++e) {
        int x = 30 + (int)e * 30;
        unsigned amp = 40U - e * (45U - fb / 4U < 40U ? 6U : 5U);
        if (amp > 40U) amp = 4U;
        fill(x, cy - (int)amp / 2, 8, (int)amp, e == 0 ? C_ORANGE : PANEL2);
    }
    disc(cx, cy, 5 + (int)ret / 20, C_GREEN);
    char d[12], dl[12], f[12], r[12];
    d[0] = 0; dec(d, fs.fx[0]);
    delay_label(fs.fx[1], dl);
    f[0] = 0; dec(f, fs.fx[2]); r[0] = 0; dec(r, fs.fx[3]);
    knob_cell(0, "DECAY", d, fs.fx[0]);
    knob_cell(1, "D TIME", dl, fs.fx[1] > 127 ? 50U : fs.fx[1] * 100U / 127U);
    knob_cell(2, "FEEDBK", f, fs.fx[2]);
    knob_cell(3, "RETURN", r, fs.fx[3]);
    hints("E6 TEMPO");
}

/* --- Arp --------------------------------------------------------- */
static void draw_arp(void)
{
    header("ARPEGGIO", C_GREEN);
    text_c(120, 40, fs.arp_on ? "ARP ON" : "ARP OFF - TAP ARP", 2, fs.arp_on ? C_GREEN : DIM);
    static const char *const rates[] = {"1/4", "1/8", "1/16", "1/32"}, *const dirs[] = {"UP", "DOWN", "UPDN", "RAND"};
    char r[12], g[12];
    r[0] = 0; dec(r, fs.arp[2] + 1); cat(r, " OCT");
    g[0] = 0; dec(g, (fs.arp[3] + 1) * 25); cat(g, "%");
    for (unsigned i = 0; i < 6; ++i) {
        int a = 40 + (int)i * 9;
        fill(30 + (int)i * 30, 100 - (int)((isin(((fs.now / 12U) + i * 40U)) + 127) * (int)(8 + fs.arp[2] * 8) / 254), 20, 6, i % 2 ? C_GREEN : C_BLUE);
        (void)a;
    }
    knob_cell(0, "RATE", rates[fs.arp[0] & 3U], fs.arp[0] * 33U);
    knob_cell(1, "MODE", dirs[fs.arp[1] & 3U], fs.arp[1] * 33U);
    knob_cell(2, "RANGE", r, fs.arp[2] * 33U);
    knob_cell(3, "GATE", g, (fs.arp[3] + 1U) * 25U);
    hints("E6 SWING  SHIFT+E6 HOLD");
}

/* --- Album (projects) ------------------------------------------- */
static void draw_project(void)
{
    header(fs.bank ? "FACTORY" : "ALBUM", C_ORANGE);
    for (unsigned i = 0; i < GROOVE_PROJECT_SLOTS; ++i) {
        int x = 6 + (int)(i % 4U) * 58, y = 28 + (int)(i / 4U) * 36;
        uint16_t bg = i == fs.slot ? C_ORANGE : PANEL;
        uint16_t fg = i == fs.slot ? BG : INK;
        fill(x, y, 54, 32, bg);
        disc(x + 14, y + 12, 6, i == fs.slot ? BG : PANEL2); disc(x + 40, y + 12, 6, i == fs.slot ? BG : PANEL2);
        char n[4]; dec(n, (int)i + 1);
        text_c(x + 27, y + 9, n, 1, fg);
        char name[10]; strncpy(name, fs.names[i], 8); name[8] = 0;
        if (!fs.bank && fs.slot_state[i]) { name[0] = 0; cat(name, "EMPTY"); }
        text(x + 3, y + 23, name, 1, fg, NOBG);
    }
    hints(fs.confirm ? "SAVE AGAIN TO OVERWRITE" : "SAVE WRITE  S+SAVE LOAD  S+E6 BANK");
    text_c(120, 150, fs.names[fs.slot], 2, INK);
}

/* --- Boot -------------------------------------------------------- */
static void draw_boot(void)
{
    unsigned t = fs.now - boot_at;
    reel(70, 100, 12, 28, (t / 4U) & 255U, C_ORANGE);
    reel(170, 100, 12, 28, (t / 4U) & 255U, C_BLUE);
    line(98, 100, 142, 100, DIM);
    text_c(120, 150, "FM1 TAPE", 3, INK);
    text_c(120, 180, "8 TRACK  6 ENGINES", 1, DIM);
}

static void draw_frame(void)
{
    if (band_top == 0 || band_top >= 0) fill(0, (int)band_top, W, ROWS, BG);
    if (fs.boot) { draw_boot(); return; }
    switch (fs.page) {
    case P_TAPE: draw_tape(); break;
    case P_SYNTH: case P_ENV: case P_LFO: draw_synth_like(); break;
    case P_SEQ: draw_seq(); break;
    case P_MIX: draw_mix(); break;
    case P_FX: draw_fx(); break;
    case P_ARP: draw_arp(); break;
    case P_PROJECT: draw_project(); break;
    default: break;
    }
    if (fs.notice_on) {
        int w = text_w(fs.notice, 1) + 12;
        fill(120 - w / 2, 22, w, 13, C_ORANGE);
        text_c(120, 25, fs.notice, 1, BG);
    }
}

/* ------------------------------------------------------------------ */
/* Frame pacing                                                       */
/* ------------------------------------------------------------------ */
static unsigned painting, paint_band;
static uint32_t last_signature;
static uint32_t signature(void)
{
    uint32_t h = 2166136261U;
    const uint8_t *p = (const uint8_t *)&fs;
    for (unsigned i = offsetof(struct frame_state, page); i < sizeof fs; ++i) h = (h ^ p[i]) * 16777619U;
    return h;
}
static void paint_step(void)
{
    uint32_t started = fm1_millis();
    while (painting) {
        int pending = fm1_screen_pending();
        if (pending) break;
        band_top = paint_band * ROWS;
        draw_frame();
        if (fm1_screen_submit(0, band_top, W, ROWS, band_px, W * ROWS)) { painting = 0; last_signature = ~0U; break; }
        if (++paint_band >= BANDS) { painting = 0; break; }
        if (fm1_millis() - started >= 4U) break;
    }
}

void groove_screen_init(void)
{
    page = P_TAPE; track = cursor = writing = drum = step_page = seq_sub = 0;
    octave = 0; selected_note = 60; tape_follow = 1;
    memset(syn_page, 0, sizeof syn_page); memset(drum_part, 0, sizeof drum_part);
    memset(key_note, 0xff, sizeof key_note);
    prev_buttons = prev_keys = blocked_keys = retry_reset = 0;
    shift_down = arp_down = arp_long = 0;
    project_slot = project_bank = project_confirm = 0;
    notice = NULL; painting = 0; last_signature = ~0U;
    memset(hit_seen, 0, sizeof hit_seen); memset(hit_until, 0, sizeof hit_until);
    memset(encoder_dir, 0, sizeof encoder_dir); memset(encoder_speed, 0, sizeof encoder_speed);
    fm1_controls_init();
    for (unsigned key = 0; key < 41; ++key) fm1_led_set(key, 0);
    fm1_screen_init();
    boot_at = fm1_millis(); boot_active = 1;
    capture(boot_at);
    painting = 1; paint_band = 0; frame_at = boot_at;
    paint_step();
}
static void update_leds(void)
{
    static uint32_t cache, button_cache;
    uint32_t bits = 0;
    if (page == P_SEQ && !writing) {
        uint64_t lane = fs.lanes[track];
        for (unsigned i = 0; i < 16; ++i) {
            unsigned step = step_page * 16U + i;
            if ((lane >> step) & 1U) bits |= 1U << white_keys[i];
            if (fs.playing && fs.play_step == step) bits ^= 1U << white_keys[i];
        }
    } else if (page == P_MIX) {
        for (unsigned t = 0; t < 8; ++t) {
            if (fs.view[t].muted) bits |= 1U << white_keys[t];
            if (fs.view[t].solo) bits |= 1U << white_keys[t + 8];
        }
    } else if (page == P_PROJECT) bits = 1U << white_keys[project_slot];
    else for (unsigned k = 0; k < 27; ++k) if (key_note[k] != 0xff) bits |= 1U << k;
    uint32_t ch = bits ^ cache;
    for (unsigned k = 0; k < 27; ++k) if (ch & (1U << k)) fm1_led_set(k + 14, (bits >> k) & 1U);
    cache = bits;
    bits = ((page == P_TAPE) << HOME) | ((page == P_FX) << FX) | ((page == P_ENV) << ENV) | ((page == P_LFO) << LFO)
         | ((page == P_SYNTH) << EDIT) | ((page == P_SEQ) << SEQ) | ((page == P_MIX) << GLO)
         | ((page == P_PROJECT) << SAVE) | ((groove_arp_enabled(track) != 0) << ARP)
         | ((writing != 0) << REC) | ((fs.playing != 0) << PLAY) | ((shift_down != 0) << SCL);
    ch = bits ^ button_cache;
    for (unsigned k = 0; k < 14; ++k) if (ch & (1U << k)) fm1_led_set(k, (bits >> k) & 1U);
    button_cache = bits;
}
void groove_screen_control(void)
{
    struct fm1_controls controls;
    fm1_controls_read(&controls);
    uint32_t now = fm1_millis();
    unsigned master = groove_master_update(controls.knob);
    if (master) {
        static char m[24]; unsigned level = fm1_engine_output_level();
        m[0] = 0; cat(m, "MASTER "); char t[8]; dec(t, (int)((level * 100U + 63U) / 127U)); cat(m, t);
        notice = m; notice_until = now + 1200U;
    }
    unsigned input = master || controls.buttons != prev_buttons || controls.keys != prev_keys;
    for (unsigned i = 0; i < 7; ++i) input |= controls.encoders[i] != 0;
    if (boot_active && (now - boot_at >= 1500U || controls.buttons || controls.keys || input)) { boot_active = 0; input = 1; }
    writing = groove_edit_recording();
    if (writing) cursor = groove_edit_cursor();
    unsigned err = groove_edit_error();
    if (err && !notice) say(err == 1 ? "RECORD STOPPED - MIDI FULL" : "CHORD LIMIT", now);
    struct groove_view tv = {0};
    groove_get_view(track, cursor, &tv);
    cursor %= tv.sequence_length ? tv.sequence_length : 1U;
    if (tv.playing && tape_follow) cursor = tv.play_step;
    if (!tv.playing && !writing) tape_follow = 1;
    if (!apply_controls(&controls, now)) return;
    int animated = boot_active || fs.page == P_TAPE || fs.page == P_SYNTH || fs.page == P_ENV || fs.page == P_LFO ||
                   fs.page == P_FX || fs.page == P_ARP || fs.page == P_MIX || fs.playing;
    if (!painting && (input || now - frame_at >= (animated ? 50U : 250U))) {
        capture(now);
        uint32_t sig = signature();
        if (animated || sig != last_signature || input) {
            last_signature = sig; frame_at = now; painting = 1; paint_band = 0;
        }
        update_leds();
    }
    paint_step();
}
void groove_screen_tap(uint32_t now)
{
    int bpm = groove_tempo_tap(now);
    say(bpm < 0 ? "EXTERNAL CLOCK" : bpm ? "TAP TEMPO SET" : "TAP AGAIN", now);
}
void groove_screen_mmc(unsigned command)
{
    unsigned transport = command == 1 || command == 9 ? 0xfc : command == 2 || command == 3 || command == 6 ? 0xfb : 0;
    if (!transport && command != 7) return;
    writing = command == 6;
    groove_edit_record(writing, track, cursor);
    if (transport) (void)post((uint8_t)transport, 0, 0, 1);
}

#ifdef FM1_HOST_TEST
/* Browser workbench hooks. The tape UI has no clickable LCD targets; the
 * panel controls, keyboard and project tools keep working. */
int groove_screen_preset(unsigned slot)
{
    if (slot >= GROOVE_PROJECT_SLOTS) return GROOVE_PROJECT_INVALID;
    int r = groove_factory_load(slot);
    if (!r) { cursor = 0; page = P_TAPE; project_slot = slot; project_bank = 1; }
    return r;
}
unsigned groove_screen_preset_position(void) { return project_slot | (project_bank << 8); }
int groove_screen_open(unsigned target)
{
    static const uint8_t map[] = {P_TAPE, P_SEQ, P_SYNTH, P_ENV, P_LFO, P_SYNTH, P_MIX, P_TAPE, P_TAPE, P_TAPE, P_FX};
    boot_active = 0;
    go(target < sizeof map ? map[target] : P_TAPE);
    return 0;
}
unsigned groove_screen_web_context(void) { return page | (track << 4) | (syn_page[track] << 8); }
unsigned groove_screen_web_mask(void) { return 0; }
const void *groove_screen_web_targets(void) { return &fs; }
unsigned groove_screen_web_target_count(void) { return 0; }
int groove_screen_web_adjust(unsigned context, unsigned action, unsigned index, unsigned owner, int delta)
{
    (void)context; (void)action; (void)index; (void)owner; (void)delta;
    return -1;
}
const char *groove_screen_web_knobs(void) { return &web_knobs[0][0][0]; }
void groove_screen_web_fine(unsigned fine) { (void)fine; }
unsigned groove_screen_web_first(void) { return step_page * 16U; }
int groove_screen_web_step(unsigned step, unsigned toggle)
{
    if (step >= 64) return -1;
    cursor = step; step_page = step / 16U;
    if (toggle) toggle_step(step);
    return 0;
}
void groove_screen_snapshot(uint8_t state[8])
{
    state[0] = (uint8_t)track; state[1] = (uint8_t)page; state[2] = (uint8_t)cursor; state[3] = (uint8_t)drum;
    state[4] = (uint8_t)writing; state[5] = 0; state[6] = (uint8_t)syn_page[track]; state[7] = (uint8_t)(octave + 24);
}
#endif
#endif /* FM1_UI_OP1 */
