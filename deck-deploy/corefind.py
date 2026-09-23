#!/usr/bin/env python3
"""Generic ELF coredump analyzer: module map from NT_FILE, thread regs from
NT_PRSTATUS, find SIGSEGV ucontext on stacks, report crash module+offset."""
import struct, sys

def load(path):
    f = open(path, "rb")
    eh = f.read(64)
    assert eh[:4] == b"\x7fELF"
    phoff = struct.unpack("<Q", eh[0x20:0x28])[0]
    phentsize, phnum = struct.unpack("<HH", eh[0x36:0x3A])
    segs, notes = [], []
    for i in range(phnum):
        f.seek(phoff + i * phentsize)
        ph = f.read(phentsize)
        p_type = struct.unpack("<I", ph[0:4])[0]
        p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack("<QQQQQ", ph[8:48])
        if p_type == 1:
            segs.append((p_vaddr, p_filesz, p_offset))
        elif p_type == 4:
            notes.append((p_offset, p_filesz))
    return f, segs, notes

def make_reader(f, segs):
    def rv(addr, size):
        for vaddr, filesz, off in segs:
            if vaddr <= addr < vaddr + filesz:
                avail = min(size, vaddr + filesz - addr)
                f.seek(off + (addr - vaddr))
                return f.read(avail)
        return None
    return rv

def parse_notes(f, notes):
    threads, files = [], []
    for noff, nsz in notes:
        f.seek(noff)
        data = f.read(nsz)
        p = 0
        while p + 12 <= nsz:
            namesz, descsz, ntype = struct.unpack("<III", data[p:p+12])
            p += 12
            name = data[p:p+namesz]; p += (namesz + 3) & ~3
            desc = data[p:p+descsz]; p += (descsz + 3) & ~3
            if ntype == 1 and name.startswith(b"CORE") and descsz >= 0x70 + 27*8:
                regs = struct.unpack("<27Q", desc[0x70:0x70+27*8])
                threads.append(regs)
            elif ntype == 0x46494c45 and name.startswith(b"CORE"):  # NT_FILE
                n = struct.unpack("<Q", desc[0:8])[0]
                pgsz = struct.unpack("<Q", desc[8:16])[0]
                starts = struct.unpack("<%dQ" % n, desc[16:16+8*n])
                ends = struct.unpack("<%dQ" % n, desc[16+8*n:16+16*n])
                ofs = struct.unpack("<%dQ" % n, desc[16+16*n:16+24*n])
                names = desc[16+24*n:].split(b"\0")
                for i in range(min(n, len(names))):
                    files.append((starts[i], ends[i], ofs[i]*pgsz, names[i].decode("utf-8", "replace")))
    return threads, files

def main(path):
    f, segs, notes = load(path)
    rv = make_reader(f, segs)
    threads, files = parse_notes(f, notes)
    print("threads=%d mappings=%d" % (len(threads), len(files)))
    mods = {}
    for s, e, o, n in files:
        mods.setdefault(n, []).append((s, e, o))
    for n in sorted(mods):
        if "gallium" in n or "radeon" in n.lower() or "libGL" in n:
            print("  module: %s base=0x%x" % (n, min(m[0] for m in mods[n])))

    # find signal ucontexts on thread stacks: look for TRAPNO=0xe (page fault)
    # gregs in ucontext: scan stacks for plausible sigframes: ERR in {4,6,7}, TRAPNO=0xe/0xd
    for ti, regs in enumerate(threads):
        rsp = regs[19]
        stack = rv(rsp, 0x60000)
        if not stack:
            continue
        for p in range(0, len(stack) - 8, 8):
            v = struct.unpack("<Q", stack[p:p+8])[0]
            if v == 0xe:  # TRAPNO candidate; check neighbors
                # layout: ... RIP[-3] EFL[-2] CSGSFS[-1] ERR[0] TRAPNO[1] ... CR2[3]
                q = [struct.unpack("<Q", stack[p+8*k:p+8*k+8])[0] for k in range(-4, 5) if 0 <= p+8*k < len(stack)-8]
                if len(q) < 9: continue
                efl, cs, err, trapno, oldmask, cr2 = q[2], q[3], q[4], q[5], q[6], q[7]
                if trapno != 0xe or err > 0x10 or efl == 0 or (cs & 0xffff) != 0x33:
                    continue
                rip = q[0]
                mod = None
                for n, ranges in mods.items():
                    for s, e, o in ranges:
                        if s <= rip < e:
                            mod = (n, rip - s + o)
                            break
                    if mod: break
                print("\n=== SIGSEGV frame on thread %d stack @0x%x ===" % (ti, rsp + p - 32))
                print("  RIP = 0x%x  %s" % (rip, "+0x%x %s" % (mod[1], mod[0]) if mod else "(unmapped/unknown)"))
                print("  CR2 = 0x%x ERR = 0x%x" % (cr2, err))
                # dump full gregs (23 qwords ending at CR2): base = p-32-16*8
                base = rsp + p - 32 - 16*8
                raw = rv(base, 40 + 23*8)
                if raw:
                    g = struct.unpack("<23Q", raw[40:40+23*8])
                    names = ["R8","R9","R10","R11","R12","R13","R14","R15","RDI","RSI","RBP","RBX","RDX","RAX","RCX","RSP","RIP"]
                    for nm, val in zip(names, g[:17]):
                        tag = ""
                        for n2, ranges in mods.items():
                            for s, e, o in ranges:
                                if s <= val < e:
                                    tag = "  <%s+0x%x>" % (n2.split("/")[-1], val - s + o)
                                    break
                            if tag: break
                        print("    %s = 0x%x%s" % (nm, val, tag))
                return
    print("\nno SIGSEGV ucontext found")

if __name__ == "__main__":
    main(sys.argv[1])
