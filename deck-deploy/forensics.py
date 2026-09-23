#!/usr/bin/env python3
"""Post-crash forensics: dump object at rbx, its entry array, and stack call chain."""
import struct

CORE = "/mnt/e/Games/FGOA/deck-logs/Hist/coredump/core.ago.14391"
GALLIUM_BASE = 0x7F1089600000
TEXT_LO = GALLIUM_BASE + 0x15000
TEXT_HI = GALLIUM_BASE + 0x13B6000

RBX = 0x7f10386f2a68      # object under inspection (arg2)
CRASH_RSP = 0x7f103dffa790
R15 = 0x7f1038595610
R10 = 0x78                # maybe array index?

f = open(CORE, "rb")
ehdr = f.read(64)
e_phoff = struct.unpack("<Q", ehdr[0x20:0x28])[0]
e_phentsize, e_phnum = struct.unpack("<HH", ehdr[0x36:0x3A])
segs = []
for i in range(e_phnum):
    f.seek(e_phoff + i * e_phentsize)
    ph = f.read(e_phentsize)
    p_type = struct.unpack("<I", ph[0:4])[0]
    p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack("<QQQQQ", ph[8:48])
    if p_type == 1:
        segs.append((p_vaddr, p_filesz, p_offset))

def rv(addr, size):
    for vaddr, filesz, off in segs:
        if vaddr <= addr < vaddr + filesz:
            avail = min(size, vaddr + filesz - addr)
            f.seek(off + (addr - vaddr))
            return f.read(avail)
    return None

def q(addr):
    b = rv(addr, 8)
    return struct.unpack("<Q", b)[0] if b else None

def d(addr):
    b = rv(addr, 4)
    return struct.unpack("<I", b)[0] if b else None

print("=== object at RBX 0x%x ===" % RBX)
hdr = rv(RBX, 0x100)
if hdr:
    for i in range(0, 0x100, 16):
        print("  +0x%02x: %s" % (i, " ".join("%02x" % c for c in hdr[i:i+16])))
typ = hdr[0x18] if hdr else None
print("type byte (+0x18):", typ)
print("dword +0x20: 0x%x" % d(RBX + 0x20))
arr = q(RBX + 0x50); cnt = d(RBX + 0x58)
print("array (+0x50) = 0x%x, count (+0x58) = %d" % (arr or 0, cnt or 0))

if arr and cnt and cnt < 4096:
    print("=== entry array scan (stride 0x28, key@+0x20, link@+0x18) ===")
    for i in range(min(cnt, 64)):
        e = arr + i * 0x28
        key = d(e + 0x20)
        link = q(e + 0x18)
        flag = rv(e + 0x1c, 1)
        flag = flag[0] if flag else None
        mark = ""
        if key == 0:
            mark = " <== first key==0 (crash entry)"
            print("  [%d] @0x%x key=0x%x link=0x%x flag=0x%x%s" % (i, e, key or 0, link or 0, flag or 0, mark))
            break
        if i < 8:
            print("  [%d] @0x%x key=0x%x link=0x%x flag=0x%x" % (i, e, key, link or 0, flag or 0))

print("\n=== stack call chain (gallium text pointers above crash RSP) ===")
stack = rv(CRASH_RSP, 0x20000)
if stack:
    seen = []
    for p in range(0, len(stack) - 8, 8):
        v = struct.unpack("<Q", stack[p:p+8])[0]
        if TEXT_LO <= v < TEXT_HI:
            off = v - GALLIUM_BASE
            seen.append((CRASH_RSP + p, off))
    # dedupe consecutive and print first 40
    prev = None
    n = 0
    for va, off in seen:
        if off != prev:
            print("  stack 0x%x -> libgallium+0x%x" % (va, off))
            n += 1
        prev = off
        if n >= 40:
            break
f.close()
