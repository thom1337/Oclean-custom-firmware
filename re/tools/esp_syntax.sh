#!/usr/bin/env bash
# Syntax-check ESP-side source files with the real cross compiler and the project's
# include paths / flags, without running a full build:
#   re/tools/esp_syntax.sh main/hw_led.c [more.c ...]
# Uses the flags recorded for main/config_store.c in build/compile_commands.json
# (run `idf.py build` once so that file exists).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC_JSON=$ROOT/build/compile_commands.json
[ -f "$CC_JSON" ] || { echo "no $CC_JSON; build the project once first" >&2; exit 2; }
export IDF_TOOLS_PATH=${IDF_TOOLS_PATH:-$HOME/.espressif}
CMD=$(python3 - "$CC_JSON" <<'PY'
import json, sys, shlex
for e in json.load(open(sys.argv[1])):
    if e['file'].endswith('/main/config_store.c'):
        a = shlex.split(e['command'])
        out = []; skip = False
        for i, t in enumerate(a):
            if skip: skip = False; continue
            if t in ('-o', '-c', '-MF', '-MT'): skip = True if t != '-c' else False; continue
            if t == '-MD' or t.endswith('config_store.c'): continue
            out.append(t)
        print(' '.join(shlex.quote(t) for t in out)); break
PY
)
rc=0
for f in "$@"; do
  (cd "$ROOT/build" && eval "$CMD -fsyntax-only -Wno-error=format-truncation $(printf %q "$ROOT/$f")") || rc=1
done
exit $rc
