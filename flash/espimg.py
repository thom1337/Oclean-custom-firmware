#!/usr/bin/env python3
"""Parse / verify / patch an ESP-IDF app image, fixing the 1-byte checksum and
appended SHA-256 so esp_ota_write accepts it.

The image served to the brush must be a valid ESP32-S3 app image: magic 0xE9,
a correct per-image XOR checksum byte, and (if the header says so) a correct
SHA-256 appended over the image. build/oclean_custom.bin already satisfies this;
you only need this tool if you hand-patch an image (e.g. bump a version string),
after which the checksum/hash must be recomputed or the brush shows "firmware
upgrade failed" and keeps the old image.

    python3 espimg.py verify <image>
    python3 espimg.py patch  <in> <out> <find_hex> <repl_hex>   # same-length splice, then re-hash

`verify` first round-trips the unmodified image to prove the recompute logic
matches the stored values before you trust a patch.
"""
import hashlib
import struct
import sys


def parse(data):
    assert data[0] == 0xE9, "not an ESP image (magic != 0xE9)"
    seg_count = data[1]
    hash_appended = data[23] == 1
    off = 24
    segs = []
    for _ in range(seg_count):
        load, dlen = struct.unpack("<II", data[off:off + 8])
        off += 8
        segs.append((load, off, dlen))
        off += dlen
    # checksum byte sits at the next position where (pos % 16 == 15)
    cksum_pos = off + (15 - (off % 16))
    return dict(seg_count=seg_count, hash_appended=hash_appended, segs=segs,
                data_end=off, cksum_pos=cksum_pos)


def recompute(data):
    """Return (checksum_byte, sha256_bytes_or_None, cksum_pos, info) from current bytes."""
    info = parse(data)
    ck = 0xEF
    for (_, o, l) in info["segs"]:
        for b in data[o:o + l]:
            ck ^= b
    cksum_pos = info["cksum_pos"]
    sha = hashlib.sha256(data[0:cksum_pos + 1]).digest() if info["hash_appended"] else None
    return ck & 0xFF, sha, cksum_pos, info


def verify(path):
    data = open(path, "rb").read()
    ck, sha, cpos, info = recompute(data)
    cur_ck = data[cpos]
    print(f"segments={info['seg_count']} hash_appended={info['hash_appended']}")
    print(f"checksum: computed=0x{ck:02x} stored=0x{cur_ck:02x}  {'MATCH' if ck == cur_ck else 'MISMATCH'}")
    if sha is not None:
        cur_sha = data[-32:]
        print(f"sha256 computed={sha.hex()}")
        print(f"sha256 stored  ={cur_sha.hex()}")
        print("sha256 " + ("MATCH" if sha == cur_sha else "MISMATCH"))
        print(f"file_len={len(data)}  cksum_pos+1={cpos + 1}  +32={cpos + 1 + 32}  (should equal file_len)")


def patch(inp, outp, find_hex, repl_hex):
    data = bytearray(open(inp, "rb").read())
    find = bytes.fromhex(find_hex)
    repl = bytes.fromhex(repl_hex)
    assert len(find) == len(repl), "patch must preserve length"
    i = data.find(find)
    assert i != -1, "pattern not found"
    assert data.find(find, i + 1) == -1, "pattern not unique — refine it"
    print(f"patching at offset 0x{i:x}: {find} -> {repl}")
    data[i:i + len(find)] = repl
    ck, sha, cpos, info = recompute(bytes(data))
    data[cpos] = ck
    if sha is not None:
        data[-32:] = sha
    open(outp, "wb").write(data)
    print("--- re-verify patched image ---")
    verify(outp)


def main(argv):
    if len(argv) >= 3 and argv[1] == "verify":
        verify(argv[2])
    elif len(argv) == 6 and argv[1] == "patch":
        patch(argv[2], argv[3], argv[4], argv[5])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
