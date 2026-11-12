"""Replays a function captured with SegaRally2.exe --diff on an independent
PowerPC emulator (Unicorn/QEMU) and compares the result with the recompiled
code. Any difference in registers or RAM points at a translation bug.

Usage: python tools/diff_unicorn.py [out] [data]
Needs: pip install unicorn
"""
import os
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED, UC_PROT_ALL
from unicorn import UC_HOOK_INTR, UC_HOOK_BLOCK
import unicorn.ppc_const as P

RAM_SIZE = 8 * 1024 * 1024
SENTINEL = 0x7FFF0000


def load_state(path):
    d = open(path, "rb").read()
    hdr = struct.unpack_from("<8I", d, 0)
    off = 32
    gpr = list(struct.unpack_from("<32I", d, off))
    off += 128
    fpr = list(struct.unpack_from("<32Q", d, off))
    off += 256
    bank = d[off]
    off += 1
    ram = d[off:off + RAM_SIZE]
    keys = ["magic", "fn", "cr", "xer", "lr", "ctr", "msr", "fpscr"]
    st = dict(zip(keys, hdr))
    st.update(gpr=gpr, fpr=fpr, bank=bank, ram=ram)
    return st


def reg(name):
    return getattr(P, name)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "out"
    data = sys.argv[2] if len(sys.argv) > 2 else "data"
    before = load_state(os.path.join(out, "diff_before.bin"))
    after = load_state(os.path.join(out, "diff_after.bin"))
    mmio = struct.unpack("<%dI" % (os.path.getsize(os.path.join(out, "diff_mmio.bin")) // 4),
                         open(os.path.join(out, "diff_mmio.bin"), "rb").read())
    mmio = [mmio[i:i + 3] for i in range(0, len(mmio), 3)]
    crom = open(os.path.join(data, "crom.bin"), "rb").read()
    banked = open(os.path.join(data, "banked_crom.bin"), "rb").read()

    uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
    uc.mem_map(0, RAM_SIZE, UC_PROT_ALL)
    uc.mem_write(0, bytes(before["ram"]))
    fixed = banked[:0x600000] + crom   # 8 MB at 0xFF800000
    uc.mem_map(0xFF800000, 0x800000, UC_PROT_ALL)
    uc.mem_write(0xFF800000, fixed)
    bank = (~before["bank"]) & 7
    uc.mem_map(0xFF000000, 0x800000, UC_PROT_ALL)
    uc.mem_write(0xFF000000, banked[bank * 0x800000:(bank + 1) * 0x800000])
    uc.mem_map(SENTINEL, 0x1000, UC_PROT_ALL)

    # Devices: pages are mapped on demand; reads are served from the log
    mapped = set()
    pos = [0]
    mismatches = []

    def map_page(addr):
        page = addr & ~0xFFF
        if page not in mapped:
            uc.mem_map(page, 0x1000, UC_PROT_ALL)
            mapped.add(page)

    def on_unmapped(uc_, access, addr, size, value, user):
        map_page(addr)
        if (addr + size - 1) & ~0xFFF != addr & ~0xFFF:
            map_page(addr + size - 1)
        return True

    def on_read(uc_, access, addr, size, value, user):
        if addr < RAM_SIZE or addr >= 0xFF000000:
            return
        if pos[0] < len(mmio):
            a, sz, v = mmio[pos[0]]
            if a != addr:
                mismatches.append((addr, a))
            pos[0] += 1
            uc_.mem_write(addr, v.to_bytes(sz, "big")[-size:].rjust(size, b"\0"))

    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, on_unmapped)
    uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=RAM_SIZE, end=0xFEFFFFFF)

    for i in range(32):
        uc.reg_write(reg("UC_PPC_REG_%d" % i), before["gpr"][i])
    if hasattr(P, "UC_PPC_REG_FPR0"):
        for i in range(32):
            uc.reg_write(reg("UC_PPC_REG_FPR%d" % i), before["fpr"][i])
    uc.reg_write(P.UC_PPC_REG_CR, before["cr"])
    uc.reg_write(P.UC_PPC_REG_XER, before["xer"])
    uc.reg_write(P.UC_PPC_REG_CTR, before["ctr"])
    uc.reg_write(P.UC_PPC_REG_LR, before["lr"])   # stop when the function returns to its caller
    if hasattr(P, "UC_PPC_REG_MSR"):
        uc.reg_write(P.UC_PPC_REG_MSR, (before["msr"] | 0x2000) & ~0x8000 & ~0x30)
    if hasattr(P, "UC_PPC_REG_FPSCR"):
        uc.reg_write(P.UC_PPC_REG_FPSCR, before["fpscr"])

    # Remember the last blocks to locate a CPU exception
    trail = []

    def on_block(uc_, addr, size, user):
        trail.append(addr)
        if len(trail) > 16:
            trail.pop(0)

    exceptions = []

    def on_intr(uc_, intno, user):
        exceptions.append((intno, uc_.reg_read(P.UC_PPC_REG_PC)))
        uc_.emu_stop()

    uc.hook_add(UC_HOOK_BLOCK, on_block)
    if os.environ.get("LW_TRACE_RANGE"):   # e.g. 800-900: print each instruction in that range
        lo_t, hi_t = (int(x, 16) for x in os.environ["LW_TRACE_RANGE"].split("-"))
        from unicorn import UC_HOOK_CODE

        def on_code(uc_, addr, size, user):
            print(f"   {addr:08X}  r2={uc_.reg_read(P.UC_PPC_REG_2):08X} r4={uc_.reg_read(P.UC_PPC_REG_4):08X} "
                  f"msr={uc_.reg_read(P.UC_PPC_REG_MSR):08X}")

        uc.hook_add(UC_HOOK_CODE, on_code, begin=lo_t, end=hi_t)
    uc.hook_add(UC_HOOK_INTR, on_intr)

    fn = before["fn"]
    print(f"Running fn_{fn:08X} on Unicorn ({len(mmio)} device reads)...")
    pc = fn
    for attempt in range(1000):   # spurious CPU exceptions: resume at the same PC
        try:
            uc.emu_start(pc, before["lr"])
        except UcError as e:
            print(f"Unicorn stopped with error {e} at {uc.reg_read(P.UC_PPC_REG_PC):08X}")
            break
        pc = uc.reg_read(P.UC_PPC_REG_PC)
        if pc == before["lr"] or not exceptions:
            break
        exceptions.clear()
    if attempt:
        print(f"{attempt} resumes after internal Unicorn exceptions")
    pc = uc.reg_read(P.UC_PPC_REG_PC)
    print(f"Ended at {pc:08X}; reads consumed {pos[0]}/{len(mmio)}; address mismatches {len(mismatches)}")
    for m in mismatches[:5]:
        print(f"   read {m[0]:08X}, expected {m[1]:08X}")

    # Registers
    diffs = 0
    for i in range(32):
        a, b = uc.reg_read(reg("UC_PPC_REG_%d" % i)), after["gpr"][i]
        if a != b:
            print(f"  r{i}: unicorn {a:08X}  recompiled {b:08X}")
            diffs += 1
    if hasattr(P, "UC_PPC_REG_FPR0"):
        for i in range(32):
            a, b = uc.reg_read(reg("UC_PPC_REG_FPR%d" % i)), after["fpr"][i]
            if a != b:
                print(f"  f{i}: unicorn {a:016X}  recompiled {b:016X}")
                diffs += 1
    for name, key in (("CR", "cr"), ("XER", "xer"), ("CTR", "ctr")):
        a, b = uc.reg_read(getattr(P, "UC_PPC_REG_" + name)), after[key]
        if (a & 0xFFFFFFFF) != b:
            print(f"  {name}: unicorn {a:08X}  recompiled {b:08X}")
            diffs += 1

    # RAM
    ram_u = uc.mem_read(0, RAM_SIZE)
    ram_r = after["ram"]
    regions = []
    i = 0
    while i < RAM_SIZE:
        if ram_u[i] != ram_r[i]:
            j = i
            while j < RAM_SIZE and (ram_u[j] != ram_r[j] or (j + 16 < RAM_SIZE and ram_u[j:j + 16] != ram_r[j:j + 16])):
                j += 1
            regions.append((i, j))
            i = j
        i += 1
    print(f"Differences: {diffs} registers, {len(regions)} RAM regions")
    for a, b in regions[:30]:
        print(f"  RAM {a:08X}-{b:08X}: unicorn {bytes(ram_u[a:min(b, a + 16)]).hex()}  recompiled {ram_r[a:min(b, a + 16)].hex()}")


if __name__ == "__main__":
    main()
