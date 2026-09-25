#!/usr/bin/env python3
# mesa-binpatch.py — libgallium 哨兵崩溃二进制补丁（多版本）
#
# 背景（FGOA-Deck-调试进度.md 0.37 修正版）：Mesa shader 磁盘缓存命中
# 恢复 program 元数据时，serialize.cpp 的 read_program_resource_data 会把
# UniformRemapTable 里的 INACTIVE_UNIFORM_EXPLICIT_LOCATION 哨兵（(void*)-1，
# "显式 location 但未激活的 uniform"）合法地装进 ProgramResourceList[i].Data。
# 随后 st_link_shader 无条件调 _mesa_create_program_resource_hash →
# _mesa_program_get_resource_name 对 GL_UNIFORM 资源做 *out = UNI(res)->name
# （24 字节 gl_resource_name 拷贝），直接解引用 -1 → SIGSEGV。
#
# 补丁语义（与源码级修复一致）：get_resource_name 的 GL_UNIFORM 共享分支
# 在解引用 Data 前检查 Data == -1 → return false（"该资源无名字"）。
# 两个调用方（create_program_resource_hash / find_name）都以
# if(!get_resource_name(...)) 跳过处理，false = 跳过该资源——未激活 uniform
# 本不该进哈希/被按名查到，语义正确。
#
# 支持版本（按输入文件 md5 自动识别）：
#   25.3.0  Deck runtime 25.08 分支，补丁点 vaddr 0x2f91cc，return-false 桩 0x2f9170
#   26.1.2  SteamOS 更新后（2026-09-25），代码逐字节相同仅位移 -0x1e60，
#           补丁点 vaddr 0x2f736c，return-false 桩 0x2f73e0（31c0c3）
#
# 用法: python3 mesa-binpatch.py <输入.so> <输出.so>
# 校验: 输入必须是未补丁的原版（md5 见下），输出重新反汇编人工核对。

import sys, hashlib

# (orig_bytes, patch_bytes) —— 补丁点 vaddr == 文件偏移（.text 段 vaddr==off）
# 两个版本原分支字节完全相同，patch 仅 je 位移不同（25.3.0: 9a / 26.1.2: 6a）
_ORIG = (
    '488b4708'      # +00: mov 0x8(%rdi),%rax
    'f30f6f00'      # +04: movdqu (%rax),%xmm0        <- 崩溃点
    '0f1106'        # +08: movups %xmm0,(%rsi)
    '488b4010'      # +0b: mov 0x10(%rax),%rax
    '48833e00'      # +0f: cmpq $0x0,(%rsi)
    '48894610'      # +13: mov %rax,0x10(%rsi)
    '0f95c0'        # +17: setne %al
    'c3'            # +1a: ret
    '660f1f840000000000'  # +1b: nopw 填充（9B，一并征用）
)

def _patch(je_rel):
    return (
        '488b4708'      # +00: mov 0x8(%rdi),%rax          ; rax = res->Data
        '4883f8ff'      # +04: cmp $-1,%rax                ; 哨兵检查
        f'74{je_rel:02x}'  # +08: je <return-false 桩>     ; -> xor eax,eax; ret
        'f30f6f00'      # +0a: movdqu (%rax),%xmm0         ; 原拷贝（重排：先存 +0x10 再 cmp，等效）
        '0f1106'        # +0e: movups %xmm0,(%rsi)
        '488b4010'      # +11: mov 0x10(%rax),%rax
        '48894610'      # +15: mov %rax,0x10(%rsi)
        '48833e00'      # +19: cmpq $0x0,(%rsi)            ; out->string != NULL ?
        '0f95c0'        # +1d: setne %al
        'c3'            # +20: ret
        '0f1f00'        # +21: nop 填充到 +0x24
    )

VERSIONS = {
    # md5: (版本名, 补丁点 vaddr, return-false 桩 vaddr)
    'c1a3e616b4697cea9ee69a1c120dec9a': ('25.3.0', 0x2f91cc, 0x2f9170),
    '3236bf4fe1b1e92117fbd2a882032ead': ('26.1.2', 0x2f736c, 0x2f73e0),
}

def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, 'rb').read())
    md5 = hashlib.md5(data).hexdigest()
    print(f'输入 md5: {md5}')
    if md5 not in VERSIONS:
        sys.exit('md5 不在已知版本表内——不是预期的原版 libgallium（已知: ' +
                 ', '.join(f'{v[0]}={k}' for k, v in VERSIONS.items()) + '）')
    name, vaddr, retfalse = VERSIONS[md5]
    orig = bytes.fromhex(_ORIG)
    # je 在补丁 +0x08（2 字节），下一条指令 vaddr+0x0a；位移 = 桩 - (vaddr+0x0a)
    je_rel = retfalse - (vaddr + 0x0a)
    assert 0 < je_rel < 0x80, f'je 位移超范围: {je_rel:#x}'
    patch = bytes.fromhex(_patch(je_rel))
    assert len(orig) == len(patch)
    print(f'识别版本: {name}（补丁点 {vaddr:#x}，return-false 桩 {retfalse:#x}，je rel {je_rel:#x}）')
    cur = bytes(data[vaddr:vaddr + len(orig)])
    if cur == patch:
        sys.exit('已经是打过补丁的版本，无需重复')
    if cur != orig:
        print('期望:', orig.hex())
        print('实际:', cur.hex())
        sys.exit('补丁点原始字节不匹配——.so 版本不对？')
    # 核对 return-false 桩确实是 xor eax,eax; ret
    stub = bytes(data[retfalse:retfalse + 3])
    if stub != bytes.fromhex('31c0c3'):
        sys.exit(f'return-false 桩字节异常: {stub.hex()}（期望 31c0c3），中止')
    data[vaddr:vaddr + len(patch)] = patch
    open(dst, 'wb').write(data)
    print(f'已写出: {dst}')
    print(f'输出 md5: {hashlib.md5(data).hexdigest()}')

if __name__ == '__main__':
    main()
