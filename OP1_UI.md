# FM1 tape-deck UI (app/ui_op1.c)

Enabled by `FM1_UI_OP1` in `app/version.h` (delete the define to get the stock UI back).
Portable: only uses the `groove_*` API, so it runs in the web preview and on device builds.

Build the preview on Linux: `FM1_WASI_SDK=/path/to/wasi-sdk-27 tools/build-web.sh` → `build/preview/index.html`.
Headless screenshots: `node tools/headless.mjs build/web/simulator.wasm out "preset:2 btn:12 wait:600 shot:tape"`.

Controls: E0 track · E1 reel length (tape/seq) or level · E2–E5 the four coloured knobs · E6 scrub (tape) / page (synth) · SHIFT = SCL.
HOME tape · EDIT synth (again = next page) · ENV · LFO · SEQ (again = step detail) · GLO mixer · FX · ARP tap=on/off, hold=settings · SAVE album.
PLAY start/stop (SHIFT = stop + rewind) · REC arm · OCT-/OCT+ rewind/ffwd one splice (tape/seq) or octave.
SHIFT + white keys 1-8 select track; on TAPE keys 9-16 = lift splice, drop splice, cut splice, lift reel, drop reel, erase reel, undo, loop reel.
SHIFT+E6 on synth pages changes engine (clears incompatible pattern cells).
Album: keys/E6 pick slot, SAVE twice writes, SHIFT+SAVE loads, SHIFT+E6 toggles factory/user bank.

## New engines (browser build only)
`web/synth/ext_engines.c`, wired into `web/engine/sound.c`. Pick with SHIFT+E6 on a synth page. 8 voices each, 8 params (two pages), saved in projects.
- WAVE: 2-osc wavetable (8 morphing tables), phase warp, detune, SVF filter + envelope.
- PAD: 3 detuned band-limited saws + sub, slow filter sweep, built-in chorus.
- CLOUD: granular soundscape (40 grains): source, size, density, pitch spread, scatter, shimmer (octave/fifth grains).
- KEYS: electric piano / bell / clav / marimba FM with tremolo and velocity hardness.

Device note: on hardware the whole instrument (sequencer, sources, mixer, FX) is inside the sealed `libfm1_engine.a`,
which only knows the original six sources. These engines need the open engine, so they run in the preview only
(the device UI hides them via `SOURCE_COUNT`). Running them on hardware would need M-VAVE to expose a source hook.
