"""tools/lwhash.py against tests/fnv_expected.txt, the same vectors as
tests/fnv_test.cpp computes with src/common/fnv1a.h.

    python tests/fnv_test.py tests/fnv_expected.txt
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import lwhash  # noqa: E402


def vectors():
    words = [0, 1, 0xFF, 0x100, 0xFFFF, 0x12345678, 0xFFFFFFFF, 0x80000000]
    dims = struct.pack("<HH", 64, 32)
    return {
        "empty": lwhash.fnv1a_bytes(b""),
        "a": lwhash.fnv1a_bytes(b"a"),
        "foobar": lwhash.fnv1a_bytes(b"foobar"),
        "bytes0-255": lwhash.fnv1a_bytes(bytes(range(256))),
        "words8": lwhash.fnv1a_words(words),
        "zeros64": lwhash.fnv1a_words([0] * 64),
        "dims+words": lwhash.fnv1a_words(words, lwhash.fnv1a_bytes(dims)),
        "mix": lwhash.fnv1a_mix(lwhash.fnv1a_mix(lwhash.BASIS ^ 0x1234, 7), 0xABCDEF),
    }


def main():
    if len(sys.argv) < 2:
        print("usage: fnv_test.py <expected.txt>", file=sys.stderr)
        return 2
    got = vectors()
    fails = seen = 0
    with open(sys.argv[1]) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            name, hexval = line.split()
            if name not in got:
                print(f"{name}: unknown vector")
                return 1
            seen += 1
            if got[name] != int(hexval, 16):
                print(f"{name}: expected {hexval}, got {got[name]:016X}")
                fails += 1
    if seen != len(got):
        print(f"the file has {seen} vectors, the test {len(got)}")
        return 1
    print(f"fnv (Python): {fails} failures" if fails else f"fnv (Python): {seen} vectors match")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
