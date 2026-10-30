#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Linux/macOS local preview build (equivalent of Build-Web.ps1 -LocalPreview).
# Usage: FM1_WASI_SDK=/path/to/wasi-sdk-27.0-* tools/build-web.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${FM1_WASI_SDK:?set FM1_WASI_SDK to an extracted WASI SDK 27}"
out="$root/build/web"; mkdir -p "$out" "$root/build/preview"
cd "$root"
mapfile -t src < <(find app web third_party/synth -name '*.c' ! -path 'app/board/*' | sort)
"$sdk/bin/clang" --sysroot="$sdk/share/wasi-sysroot" -std=c11 -O2 -fwrapv -fno-fast-math \
  -ffp-contract=off -DFM1_HOST_TEST -DFM1_OPEN_WEB -Wall -Wextra -Werror -Wno-unused-function \
  -Wno-array-parameter -mexec-model=reactor \
  -Iweb/engine -Iweb/synth -Ithird_party/synth -Isdk/include -Iapp/synth -Iapp \
  "${src[@]}" -Wl,--strip-all -Wl,-z,stack-size=131072 -Wl,--initial-memory=4194304 \
  -Wl,--max-memory=33554432 -o "$out/simulator.wasm"
node --input-type=module - "$root" <<'EOF'
import fs from 'node:fs'; import crypto from 'node:crypto'; import path from 'node:path';
const root = process.argv[2], rd = f => fs.readFileSync(path.join(root, f), 'utf8');
const wasm = fs.readFileSync(path.join(root, 'build/web/simulator.wasm'));
const version = /#define FM1_RELEASE_VERSION "(V\d+)"/.exec(rd('app/version.h'))[1];
const sha = crypto.createHash('sha256').update(wasm).digest('hex');
const sim = JSON.stringify({schema: 1, release_version: version, sha256: sha, source_sha256: sha,
  sample_bank: 'synthesized-demo', data: wasm.toString('base64')});
let web = rd('sdk/Update.html.in');
web = web.replace(/<a\b[^>]*href="\/\*FM1_SOURCE_URL\*\/"[^>]*>(.*?)<\/a>/g, '<span>$1 (local preview)</span>');
const esc = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const rep = {
  '/*FM1_SOURCE_URL*/': '', '/*FM1_PAYLOAD*/': 'const FM1_PACKAGE = null;',
  '/*FM1_SIMULATOR*/': 'const FM1_SIMULATOR = ' + sim + ';',
  '/*FM1_I18N*/': rd('sdk/I18n.js'), '/*FM1_SCRIPT*/': rd('sdk/WebUpdate.js'),
  '/*FM1_SIM_SCRIPT*/': rd('sdk/Simulator.js'), '/*FM1_SAMPLE_SCRIPT*/': rd('sdk/Samples.js'),
  '/*FM1_NOTICES*/': esc(rd('THIRD_PARTY.md') + '\n' + rd('LICENSE.md')),
};
for (const [k, v] of Object.entries(rep)) web = web.split(k).join(v);
fs.writeFileSync(path.join(root, 'build/preview/index.html'), web);
console.log('wrote build/preview/index.html', web.length);
EOF
