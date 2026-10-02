#!/usr/bin/env bash
# Cross-compile every main/*.c and list the project symbols no object defines (needs a
# build/compile_commands.json from an earlier idf.py build). With the port complete this
# prints only what main/oem_glue.c has to provide.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd); OUT=$(mktemp -d)
export IDF_TOOLS_PATH=${IDF_TOOLS_PATH:-$HOME/.espressif}
CMD=$(python3 - "$ROOT/build/compile_commands.json" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['file'].endswith('/main/config_store.c'):
        a = shlex.split(e['command']); out = []; skip = False
        for t in a:
            if skip: skip = False; continue
            if t in ('-o', '-MF', '-MT'): skip = True; continue
            if t in ('-c', '-MD') or t.endswith('config_store.c'): continue
            out.append(t)
        print(' '.join(shlex.quote(t) for t in out)); break
PY
)
for f in "$ROOT"/main/*.c; do
  (cd "$ROOT/build" && eval "$CMD -Wno-error=format-truncation -c $(printf %q "$f") -o $OUT/$(basename "$f" .c).o")
done
NM=$(command -v xtensa-esp32s3-elf-nm || echo "$HOME/.espressif/tools/xtensa-esp32s3-elf/esp-12.2.0_20230208/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-nm")
"$NM" "$OUT"/*.o | awk 'NF==2 && $1=="U"{u[$2]=1} NF==3 && $2 ~ /^[TDBRWVC]$/{d[$3]=1} END{for(k in u) if(!(k in d)) print k}' \
  | { grep -E "^(hal_|oem_|hw_|brush_app|ui_|metrics_|boot_guard|weblog|fs_|config_|wifi_mgr|mqtt_ha|web_server|ble_server)" || true; } | sort
rm -rf "$OUT"
