/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Extra synthesis engines for the open (browser) instrument: WAVE (wavetable),
 * PAD (supersaw strings + chorus), CLOUD (granular soundscape) and KEYS
 * (electric piano / bell FM). Each is a plain mono voice bank like PSG/PLUCK:
 * caller-owned storage, 128-frame blocks, Q24 output, eight params. */
#ifndef FM1_EXT_ENGINES_H
#define FM1_EXT_ENGINES_H
#include "engine.h"
#include "fm1_param.h"

enum { EXT_FIRST = 6, EXT_COUNT = 4, EXT_PARAMS = 8, EXT_VOICES = 8 };
static inline unsigned ext_is(unsigned type) { return type >= EXT_FIRST && type < EXT_FIRST + EXT_COUNT; }
const char *ext_name(unsigned type);
size_t ext_size(unsigned type);
void ext_init(unsigned type, void *p);
void ext_panic(unsigned type, void *p);
void ext_note(unsigned type, void *p, unsigned note, unsigned velocity, int cents);
void ext_midi(unsigned type, void *p, const uint8_t m[3]);
void ext_render(unsigned type, void *p, int32_t *out, unsigned frames);
void ext_set(unsigned type, void *p, unsigned parameter, unsigned value);
unsigned ext_get(unsigned type, const void *p, unsigned parameter);
const fm1_param_t *ext_param(unsigned type, unsigned parameter);
unsigned ext_limit(unsigned type, void *p, unsigned voices);
unsigned ext_active(unsigned type, const void *p);
#endif
