#!/usr/bin/env python3
"""Parse ELF coredump: find crash ucontext and walk the faulting thread's stack."""
import struct, sys

CORE = "/mnt/e/Games/FGOA/deck-logs/Hist/coredump/core.ago.14391"
GALLIUM_BASE = 0x7F1089600000
CRASH_RIP = GALLIUM_BASE + 0xA76034
TEXT_LO = GALLIUM_BASE + 0x15000
TEXT_HI = GALLIUM_BASE + 0x13B6000

f = open(CORE, "rb")
ehdr = f.read(64)
assert ehdr[:4] == b"\x7fELF"
e_phoff = struct.unpack("<Q", ehdr[0x20:0x28])[0]
e_phentsize, e_phnum = struct.unpack("<HH", ehdr[0x36:0x3A])

segs = []   # (vaddr, filesz, offset) for PT_LOAD
notes = []
for i in range(e_phnum):
    f.seek(e_phoff + i * e_phentsize)
    ph = f.read(e_phentsize)
    p_type = struct.unpack("<I", ph[0:4])[0]
    p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack("<QQQQQ", ph[8:8+40])
    if p_type == 1:
        segs.append((p_vaddr, p_filesz, p_offset, p_memsz))
    elif p_type == 4:
        notes.append((p_offset, p_filesz))

def read_vaddr(addr, size):
    for vaddr, filesz, off, memsz in segs:
        if vaddr <= addr < vaddr + filesz:
            avail = min(size, vaddr + filesz - addr)
            f.seek(off + (addr - vaddr))
            return f.read(avail)
    return None

# --- parse notes for NT_PRSTATUS (type=1) ---
threads = []
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
            # x86_64 pr_reg order: r15,r14,r13,r12,rbp,rbx,r11,r10,r9,r8,
            # rax,rcx,rdx,rsi,rdi,orig_rax,rip,cs,eflags,rsp,ss,fs_base,gs_base,ds,es,fs,gs
            threads.append({"rip": regs[16], "rsp": regs[19], "rbp": regs[4], "regs": regs})

print(f"threads: {len(threads)}")

# --- find thread whose stack contains crash RIP (ucontext copy by wine handler) ---
for idx, t in enumerate(threads):
    rsp = t["rsp"]
    stack = read_vaddr(rsp, 0x40000)
    if not stack:
        continue
    needle = struct.pack("<Q", CRASH_RIP)
    pos = 0
    while True:
        p = stack.find(needle, pos)
        if p < 0:
            break
        va = rsp + p
        print(f"thread {idx}: crash RIP found on stack at 0x{va:x} (thread rip=0x{t['rip']:x} rsp=0x{rsp:x})")
        # try interpret as ucontext: gregs at +40
        uc = read_vaddr(va - 40 - 16*8, 0x200)  # back up to start of gregs
        # find plausible gregs: RIP at idx16 equals CRASH_RIP when base = va-16*8?
        base = va - 16*8
        gregs_raw = read_vaddr(base - 40, 40 + 27*8)
        if gregs_raw:
            gregs = struct.unpack("<23Q", gregs_raw[40:40+23*8])
            names = ["R8","R9","R10","R11","R12","R13","R14","R15","RDI","RSI","RBP","RBX","RDX","RAX","RCX","RSP","RIP","EFL","CSGSFS","ERR","TRAPNO","OLDMASK","CR2"]
            print("  ucontext candidate:")
            for n, v in zip(names, gregs):
                print(f"    {n} = 0x{v:x}")
        pos = p + 8

# --- also scan ALL thread stacks for qwords pointing into gallium text near crash fn ---
print("\n--- return-address scan on each thread stack (libgallium text) ---")
for idx, t in enumerate(threads):
    stack = read_vaddr(t["rsp"], 0x30000)
    if not stack:
        continue
    hits = []
    for p in range(0, len(stack) - 8, 8):
        v = struct.unpack("<Q", stack[p:p+8])[0]
        if TEXT_LO <= v < TEXT_HI:
            hits.append((t["rsp"] + p, v - GALLIUM_BASE))
    if hits:
        interesting = [h for h in hits if 0xA70000 < h[1] < 0xA80000]
        if interesting:
            print(f"thread {idx} (rip=0x{t['rip']:x}):")
            for va, off in interesting[:10]:
                print(f"    stack 0x{va:x} -> libgallium+0x{off:x}")
f.close()
