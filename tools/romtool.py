"""Extracts the memory regions of Sega Rally 2 (srally2, Model 3 Step 2.0)
from the original zip, without modifying it. Writes flat binaries to data/.

Usage: python tools/romtool.py <path to srally2.zip> [output folder]
"""
import os
import sys
import zipfile
import zlib

# (file name, crc32, offset) per region. Layout taken from the
# hardware interleaving format: stride/chunk/byte_swap. Checked against the
# boot code: the reset vector at 0xFFF00100 decodes and copies CROM[0:2 MB] to
# RAM. dsb_* is the Digital Sound Board 2 (68000 + MPEG music and voices);
# drive_program the Z80 of the force feedback board.
REGIONS = {
    "crom": dict(stride=8, chunk=2, swap=True, files=[
        ("epr-20635.20", 0x7937473F, 0x0000000),
        ("epr-20634.19", 0x45A09245, 0x0000002),
        ("epr-20633.18", 0xF5A24F24, 0x0000004),
        ("epr-20632.17", 0x6829A801, 0x0000006)]),
    "banked_crom": dict(stride=8, chunk=2, swap=True, files=[
        ("mpr-20605.4", 0x00513401, 0x0000000),
        ("mpr-20604.3", 0x99C5F396, 0x0000002),
        ("mpr-20603.2", 0xAD0D8EB8, 0x0000004),
        ("mpr-20602.1", 0x60CFA72A, 0x0000006),
        ("mpr-20609.8", 0xC03CC0E5, 0x1000000),
        ("mpr-20608.7", 0x0C9B0571, 0x1000002),
        ("mpr-20607.6", 0x6DA85AA3, 0x1000004),
        ("mpr-20606.5", 0x072498FD, 0x1000006),
        ("mpr-20613.12", 0x2938C0D9, 0x2000000),
        ("mpr-20612.11", 0x721A44B6, 0x2000002),
        ("mpr-20611.10", 0x5D9F8BA2, 0x2000004),
        ("mpr-20610.9", 0xB6E0FF4E, 0x2000006)]),
    "vrom": dict(stride=32, chunk=2, swap=False, files=[
        ("mpr-20616.26", 0xE11DCF8B, 0x0000000),
        ("mpr-20617.27", 0x96ACEF3F, 0x0000002),
        ("mpr-20618.28", 0x6C281281, 0x0000004),
        ("mpr-20619.29", 0x0FA65819, 0x0000006),
        ("mpr-20620.30", 0xEE79585F, 0x0000008),
        ("mpr-20621.31", 0x3A99148F, 0x000000A),
        ("mpr-20622.32", 0x0618F056, 0x000000C),
        ("mpr-20623.33", 0xCCF31B85, 0x000000E),
        ("mpr-20624.34", 0x90F30936, 0x0000010),
        ("mpr-20625.35", 0x04F804FA, 0x0000012),
        ("mpr-20626.36", 0x2D6C97D6, 0x0000014),
        ("mpr-20627.37", 0xA14EE871, 0x0000016),
        ("mpr-20628.38", 0xBBA829A3, 0x0000018),
        ("mpr-20629.39", 0xEAD2EB31, 0x000001A),
        ("mpr-20630.40", 0xCC5881B8, 0x000001C),
        ("mpr-20631.41", 0x5CB69FFD, 0x000001E)]),
    "sound_program": dict(stride=1, chunk=1, swap=True, files=[
        ("epr-20636.21", 0x7139EBF8, 0x0000000)]),
    "sound_samples": dict(stride=1, chunk=1, swap=True, files=[
        ("mpr-20614.22", 0xA3930E4A, 0x0000000),
        ("mpr-20615.24", 0x62E8A94A, 0x0400000)]),
    "dsb_program": dict(stride=1, chunk=1, swap=True, files=[
        ("epr-20641.2", 0xC9B82035, 0x0000000)]),
    "dsb_mpeg": dict(stride=1, chunk=1, swap=False, files=[
        ("mpr-20637.57", 0xD66E8A02, 0x0000000),
        ("mpr-20638.58", 0xD1513382, 0x0400000),
        ("mpr-20639.59", 0xF6603B7B, 0x0800000),
        ("mpr-20640.60", 0x9EEA07B7, 0x0C00000)]),
    "drive_program": dict(stride=1, chunk=1, swap=False, files=[
        ("epr-20512.bin", 0xCF64350D, 0x0000000)]),
}


def load_by_crc(zf):
    return {info.CRC: info for info in zf.infolist()}


def byteswap16(data):
    b = bytearray(data)
    b[0::2], b[1::2] = data[1::2], data[0::2]
    return bytes(b)


def build_region(zf, by_crc, spec):
    parts = []
    for name, crc, off in spec["files"]:
        info = by_crc.get(crc)
        if info is None:
            raise SystemExit(f"{name} (crc {crc:08x}) missing from the zip")
        data = zf.read(info)
        if zlib.crc32(data) != crc:
            raise SystemExit(f"Bad CRC in {info.filename}")
        if spec["swap"]:
            data = byteswap16(data)
        parts.append((off, data))
    stride, chunk = spec["stride"], spec["chunk"]
    size = max(off - off % stride + len(d) * stride // chunk for off, d in parts)
    out = bytearray(size)
    for off, data in parts:
        base, lane = off - off % stride, off % stride
        if stride == chunk:
            out[base:base + len(data)] = data
            continue
        for i in range(0, len(data), chunk):
            p = base + (i // chunk) * stride + lane
            out[p:p + chunk] = data[i:i + chunk]
    return bytes(out)


# Chip -> what it is, for the error when a set lacks it
NOTES = {
    "epr-20512.bin": "program of the force feedback board of the steering wheel",
}


def check_set(zf, by_crc):
    """Every chip of every region, before anything is written: a set with a
    chip missing must not leave a half data folder that build.bat then takes
    as extracted."""
    missing = [name for spec in REGIONS.values() for name, crc, _ in spec["files"] if crc not in by_crc]
    if not missing:
        return
    lines = [f"{zf.filename} is not a complete Sega Rally 2 ROM (srally2).",
             "Missing (looked up by CRC, so the file names in the zip do not matter):"]
    total = sum(len(spec["files"]) for spec in REGIONS.values())
    if len(missing) > 8:
        lines[1] = f"{len(missing)} of its {total} chips are missing: this is another game or another version."
        missing = []
    for name in missing:
        note = NOTES.get(name)
        lines.append(f"  {name}" + (f"  ({note})" if note else ""))
    lines.append("Get a complete srally2.zip and run build.bat again.")
    raise SystemExit("\n".join(lines))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "data")
    with zipfile.ZipFile(sys.argv[1]) as zf:
        by_crc = load_by_crc(zf)
        check_set(zf, by_crc)
        os.makedirs(outdir, exist_ok=True)
        # crom.bin last: build.bat takes data\crom.bin as "already extracted"
        for region in sorted(REGIONS, key=lambda r: r == "crom"):
            spec = REGIONS[region]
            data = build_region(zf, by_crc, spec)
            with open(os.path.join(outdir, region + ".bin"), "wb") as f:
                f.write(data)
            print(f"{region:14} {len(data):>10} bytes")


if __name__ == "__main__":
    main()
