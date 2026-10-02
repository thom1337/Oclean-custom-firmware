#!/usr/bin/env bash
# Turn the stock ota.bin into readable C (per-function files under re/work/decomp/f2).
#
#   re/tools/decompile.sh /path/to/stock/ota.bin
#
# Needs: re/prep.sh already run (segments in re/), ESP-IDF v5.1.1 (IDF_PATH, exported),
# a JDK 21 and Ghidra >= 11 (GHIDRA_HOME). Steps:
#   1. wrap the image segments in an ELF (build_elf.py)
#   2. build a reference blufi example at -Os and name the stock's IDF library functions
#      by matching its unlinked objects against the stock code (match_names.py)
#   3. Ghidra headless: import, mark function entries, apply names, decompile the app range
#   4. resolve literal-pool pointers into names / strings / globals (postproc.py)
set -euo pipefail
OTA=${1:?usage: decompile.sh ota.bin}
T=$(cd "$(dirname "$0")" && pwd); RE=$(dirname "$T"); W=$RE/work
: "${GHIDRA_HOME:?set GHIDRA_HOME}"; : "${IDF_PATH:?export ESP-IDF first}"
mkdir -p "$W"; cd "$W"
python3 "$T/build_elf.py" "$OTA" stock.elf

if [ ! -d ref_Os/build/esp-idf ]; then
  cp -r "$IDF_PATH/examples/bluetooth/blufi" ref_Os
  rm -f ref_Os/sdkconfig.defaults.esp32*
  cat >> ref_Os/sdkconfig.defaults <<CFG
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_ULP_COPROC_ENABLED=y
CONFIG_ULP_COPROC_TYPE_RISCV=y
CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CFG
  (cd ref_Os && idf.py set-target esp32s3 && idf.py build) > ref_build.log 2>&1
fi
TC=$(dirname "$(dirname "$(command -v xtensa-esp32s3-elf-gcc)")")
python3 "$T/match_names.py" --rebuild ref_Os/build/esp-idf \
  "$IDF_PATH/components/esp_wifi/lib/esp32s3" "$IDF_PATH/components/esp_phy/lib/esp32s3/libphy.a" \
  "$IDF_PATH/components/esp_phy/lib/esp32s3/libbtbb.a" \
  "$IDF_PATH/components/bt/controller/lib_esp32c3_family/esp32s3/libbtdm_app.a" \
  "$IDF_PATH/components/esp_coex/lib/esp32s3/libcoexist.a" "$IDF_PATH/components/xtensa/esp32s3/libxt_hal.a" \
  "$TC/xtensa-esp32s3-elf/lib/no-rtti/libc.a" "$TC/xtensa-esp32s3-elf/lib/no-rtti/libm.a" \
  "$TC/lib/gcc/xtensa-esp32s3-elf/12.2.0/no-rtti/libgcc.a"
sort -o names.txt names.txt

# ROM labels (implementation addresses from the ROM ELF, entry stubs from the ld scripts)
ROMELF=$(ls ~/.espressif/tools/esp-rom-elfs/*/esp32s3_rev0_rom.elf | head -1)
xtensa-esp32s3-elf-nm "$ROMELF" | awk '$2 ~ /^[TtDdBbRrAW]$/ {print $1, $3}' | grep -v -E ' (_|\.)' > rom_syms.txt
cat "$IDF_PATH"/components/esp_rom/esp32s3/ld/*.ld | python3 -c '
import sys,re
seen={}
for line in sys.stdin:
    m=re.search(r"(?:PROVIDE\s*\(\s*)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(0x[0-9a-fA-F]+)\s*\)?\s*;",line)
    if m:
        a=int(m.group(2),16)
        if 0x40000000<=a<0x40060000 or 0x3ff00000<=a<0x3ff20000: seen.setdefault(a,m.group(1))
for a in sorted(seen): print("%08x %s"%(a,seen[a]))' > rom_ld.txt
# function entries: every "entry a1, N" in the code segments
python3 - <<'PY' > entries.txt
import os
RE=os.path.dirname(os.getcwd())
for base,f in ((0x40374000,'seg2_40374000.bin'),(0x42000020,'seg3_42000020.bin'),(0x4037aa08,'seg4_4037aa08.bin')):
    d=open(os.path.join(RE,f),'rb').read(); i=d.find(b'\x36')
    while i>=0:
        if i+3<=len(d) and (d[i+1]&0xF)==1 and d[i+2]<0x10: print('%08x'%(base+i))
        i=d.find(b'\x36',i+1)
PY
"$GHIDRA_HOME/support/analyzeHeadless" "$W/ghproj" oclean -import stock.elf -processor "Xtensa:LE:32:default" \
  -scriptPath "$T/ghidra_scripts" -preScript OcleanSetup.java "$W" -overwrite > ghidra_import.log 2>&1
# app code: 0x4200b800..0x4202b74c (flash) and 0x40377bd8..0x40378868 (IRAM)
"$GHIDRA_HOME/support/analyzeHeadless" "$W/ghproj" oclean -process stock.elf -noanalysis \
  -scriptPath "$T/ghidra_scripts" -postScript OcleanNames.java "$W/names.txt" \
  -postScript OcleanExport.java "$W/decomp" 4200b800 4202b74c 40377bd8 40378868 > ghidra_export.log 2>&1
python3 "$T/postproc.py" "$W"
echo "decompiled functions: $W/decomp/f2/<addr>.c   (single file: $W/decomp/all2.c)"
