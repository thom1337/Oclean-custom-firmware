#!/usr/bin/env bash
set -e
export IDF_TOOLS_PATH=$HOME/.espressif
. $HOME/esp/esp-idf/export.sh >/dev/null 2>&1
OBJ=$HOME/.espressif/tools/xtensa-esp32s3-elf/esp-12.2.0_20230208/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-objdump
cd $HOME/oclean-custom-firmware/re

echo "== disassembling code segments =="
$OBJ -D -b binary -m xtensa --adjust-vma=0x40374000 seg2_40374000.bin > seg2.dis
$OBJ -D -b binary -m xtensa --adjust-vma=0x42000020 seg3_42000020.bin > seg3.dis
$OBJ -D -b binary -m xtensa --adjust-vma=0x4037aa08 seg4_4037aa08.bin > seg4.dis
wc -l seg2.dis seg3.dis seg4.dis

echo "== extracting strings with VMA (DROM@0x3c110020, DRAM@0x3fc99e00) =="
$HOME/.oclean/venv/bin/python - <<'PY'
import re
def dump(fn, base, out):
    d=open(fn,'rb').read(); res=[]; cur=b''; start=0
    for i,b in enumerate(d):
        if 32<=b<127:
            if not cur: start=i
            cur+=bytes([b])
        else:
            if len(cur)>=4: res.append((base+start, cur.decode('latin1')))
            cur=b''
    if len(cur)>=4: res.append((base+start, cur.decode('latin1')))
    with open(out,'w') as f:
        for a,s in res: f.write(f"0x{a:08x}\t{s}\n")
    return len(res)
n0=dump('seg0_3c110020.bin',0x3c110020,'strings_drom.txt')
n1=dump('seg1_3fc99e00.bin',0x3fc99e00,'strings_dram.txt')
print("drom strings",n0,"dram strings",n1)
PY
cat strings_drom.txt strings_dram.txt > strings_all.txt

echo "== source-path / driver tags (reveals which IDF drivers are compiled in) =="
grep -iE '\.(c|h)\b|/components/|esp-idf' strings_all.txt | sort -u -t$'\t' -k2 > srcpaths.txt
wc -l srcpaths.txt
echo "done"
