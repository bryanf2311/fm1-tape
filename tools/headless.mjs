// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
// Headless driver for the browser build: runs a script of panel actions and
// writes the 240x240 LCD to PNG files. Usage:
//   node tools/headless.mjs build/web/simulator.wasm out_dir "script"
// Script ops (space separated): btn:N  key:N  enc:I:D  wait:MS  shot:NAME  ... (btn/key toggle press+release;
// use down:btn:N / up:btn:N to hold).
import fs from 'node:fs'; import zlib from 'node:zlib';
const [wasmPath, outDir, script = ''] = process.argv.slice(2);
fs.mkdirSync(outDir, {recursive: true});
const wasi = new Proxy({}, {get: (_, n) => (n === 'proc_exit' ? () => { throw Error('exit'); } : () => 0)});
const {instance} = await WebAssembly.instantiate(fs.readFileSync(wasmPath), {wasi_snapshot_preview1: wasi});
const c = instance.exports; c._initialize?.();
c.sim_boot(1);
let audioPeak = 0;
const tick = ms => {
  for (let i = 0, n = Math.ceil(ms / 2.9); i < n; ++i) {
    const p = c.sim_render();
    const f = new Float32Array(c.memory.buffer, p, 256);
    for (let k = 0; k < 256; ++k) audioPeak = Math.max(audioPeak, Math.abs(f[k]));
  }
};
const crc = new Uint32Array(256).map((_, n) => { let x = n; for (let k = 0; k < 8; ++k) x = x & 1 ? 0xedb88320 ^ (x >>> 1) : x >>> 1; return x >>> 0; });
const crc32 = b => { let x = ~0; for (const v of b) x = crc[(x ^ v) & 255] ^ (x >>> 8); return ~x >>> 0; };
const chunk = (t, d) => { const b = Buffer.alloc(12 + d.length); b.writeUInt32BE(d.length, 0); b.write(t, 4); d.copy(b, 8);
  b.writeUInt32BE(crc32(b.subarray(4, 8 + d.length)), 8 + d.length); return b; };
const shot = (name, scale = 2) => {
  const px = new Uint16Array(c.memory.buffer, c.sim_pixels(), 240 * 240), W = 240 * scale;
  const raw = Buffer.alloc((W * 3 + 1) * W);
  for (let y = 0; y < W; ++y) { raw[y * (W * 3 + 1)] = 0;
    for (let x = 0; x < W; ++x) { const v = px[((y / scale) | 0) * 240 + ((x / scale) | 0)], o = y * (W * 3 + 1) + 1 + x * 3;
      raw[o] = (v >> 11) * 255 / 31; raw[o + 1] = ((v >> 5) & 63) * 255 / 63; raw[o + 2] = (v & 31) * 255 / 31; } }
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(W, 4); ihdr[8] = 8; ihdr[9] = 2;
  fs.writeFileSync(`${outDir}/${name}.png`, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
};
tick(2000);
for (const op of script.split(/\s+/).filter(Boolean)) {
  const a = op.split(':');
  if (a[0] === 'wait') tick(+a[1]);
  else if (a[0] === 'shot') shot(a[1]);
  else if (a[0] === 'btn') { c.sim_control(0, +a[1], 1); tick(60); c.sim_control(0, +a[1], 0); tick(60); }
  else if (a[0] === 'key') { c.sim_control(1, +a[1], 1); tick(120); c.sim_control(1, +a[1], 0); tick(60); }
  else if (a[0] === 'down') c.sim_control(a[1] === 'btn' ? 0 : 1, +a[2], 1), tick(60);
  else if (a[0] === 'up') c.sim_control(a[1] === 'btn' ? 0 : 1, +a[2], 0), tick(60);
  else if (a[0] === 'enc') { c.sim_control(2, +a[1], +a[2]); tick(60); }
  else if (a[0] === 'sel') { console.log('sel', c.sim_select(+a[1], +a[2])); tick(100); }
  else if (a[0] === 'rec') { // rec:name:ms -> WAV of master output
    const n = Math.ceil(+a[2] / 2.9), buf = new Int16Array(n * 256); let pk = 0, nan = 0;
    for (let i = 0; i < n; ++i) { const f = new Float32Array(c.memory.buffer, c.sim_render(), 256);
      for (let k = 0; k < 256; ++k) { const v = f[k]; if (!Number.isFinite(v)) { ++nan; continue; } pk = Math.max(pk, Math.abs(v)); buf[i * 256 + k] = Math.max(-1, Math.min(1, v)) * 32767; } }
    const h = Buffer.alloc(44); h.write('RIFF', 0); h.writeUInt32LE(36 + buf.byteLength, 4); h.write('WAVEfmt ', 8); h.writeUInt32LE(16, 16);
    h.writeUInt16LE(1, 20); h.writeUInt16LE(2, 22); h.writeUInt32LE(44100, 24); h.writeUInt32LE(176400, 28); h.writeUInt16LE(4, 32); h.writeUInt16LE(16, 34);
    h.write('data', 36); h.writeUInt32LE(buf.byteLength, 40);
    fs.writeFileSync(`${outDir}/${a[1]}.wav`, Buffer.concat([h, Buffer.from(buf.buffer)]));
    console.log('rec', a[1], 'peak', pk.toFixed(3), 'nan', nan); }
  else if (a[0] === 'preset') { c.sim_preset(+a[1]); tick(100); }
  else if (a[0] === 'peak') { console.log('peak', audioPeak.toFixed(3)); audioPeak = 0; }
  else if (a[0] === 'state') { const s = new Uint8Array(c.memory.buffer, c.sim_state(), 225); console.log('state', JSON.stringify(Array.from(s.subarray(0, 8))), 'types', JSON.stringify([0,1,2,3,4,5,6,7].map(t => s[8 + t * 20]))); }
  else console.log('unknown op', op);
}
