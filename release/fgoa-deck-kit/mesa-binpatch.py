#!/usr/bin/env python3
# mesa-binpatch.py — libgallium-25.3.0.so 哨兵崩溃二进制补丁
#
# 背景（FGOA-Deck-调试进度.md 0.37 修正版）：Mesa 25.3.0 shader 磁盘缓存命中
# 恢复 program 元数据时，serialize.cpp 的 read_program_resource_data 会把
# UniformRemapTable 里的 INACTIVE_UNIFORM_EXPLICIT_LOCATION 哨兵（(void*)-1，
# "显式 location 但未激活的 uniform"）合法地装进 ProgramResourceList[i].Data。
# 随后 st_link_shader 无条件调 _mesa_create_program_resource_hash →
# _mesa_program_get_resource_name 对 GL_UNIFORM 资源做 *out = UNI(res)->name
# （24 字节 gl_resource_name 拷贝），直接解引用 -1 → SIGSEGV（libgallium+0x2f91d0）。
#
# 补丁语义（与源码级修复一致）：get_resource_name 的 GL_UNIFORM 共享分支
# （vaddr 0x2f91cc，跳转表 idx0/1/4/5/7-12/19 + >0x92f4 的 0x957c..0x957d 共用）
# 在解引用 Data 前检查 Data == -1 → return false（"该资源无名字"）。
# 两个调用方（create_program_resource_hash:2208 / find_name:715）都以
# if(!get_resource_name(...)) 跳过处理，false = 跳过该资源——未激活 uniform
# 本不该进哈希/被按名查到，语义正确。
#
# 用法: python3 mesa-binpatch.py <输入.so> <输出.so>
# 校验: 输入必须是未补丁的原版（md5 见下），输出重新反汇编人工核对。

import sys, struct, hashlib

SRC_MD5 = 'c1a3e616b4697cea9ee69a1c120dec9a'  # 原版 libgallium-25.3.0.so（Deck runtime 25.08 分支）

# (vaddr, orig_bytes, patch_bytes) —— vaddr == 文件偏移（.text 段 vaddr==off）
REGION_VADDR = 0x2f91cc
ORIG = bytes.fromhex(
    '488b4708'      # 2f91cc: mov 0x8(%rdi),%rax
    'f30f6f00'      # 2f91d0: movdqu (%rax),%xmm0        <- 崩溃点
    '0f1106'        # 2f91d4: movups %xmm0,(%rsi)
    '488b4010'      # 2f91d7: mov 0x10(%rax),%rax
    '48833e00'      # 2f91db: cmpq $0x0,(%rsi)
    '48894610'      # 2f91df: mov %rax,0x10(%rsi)
    '0f95c0'        # 2f91e3: setne %al
    'c3'            # 2f91e6: ret
    '660f1f840000000000'  # 2f91e7: nopw 填充（9B，一并征用）
)
PATCH = bytes.fromhex(
    '488b4708'      # 2f91cc: mov 0x8(%rdi),%rax          ; rax = res->Data
    '4883f8ff'      # 2f91d0: cmp $-1,%rax                ; 哨兵检查
    '749a'          # 2f91d4: je 0x2f9170                 ; -> xor eax,eax; ret（return false）
    'f30f6f00'      # 2f91d6: movdqu (%rax),%xmm0         ; 原拷贝（重排：先存 +0x10 再 cmp，等效）
    '0f1106'        # 2f91da: movups %xmm0,(%rsi)
    '488b4010'      # 2f91dd: mov 0x10(%rax),%rax
    '48894610'      # 2f91e1: mov %rax,0x10(%rsi)
    '48833e00'      # 2f91e5: cmpq $0x0,(%rsi)            ; out->string != NULL ?
    '0f95c0'        # 2f91e9: setne %al
    'c3'            # 2f91ec: ret
    '0f1f00'        # 2f91ed: nop 填充到 2f91f0
)

assert len(ORIG) == len(PATCH) == 0x2f91f0 - 0x2f91cc

def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, 'rb').read())
    md5 = hashlib.md5(data).hexdigest()
    print(f'输入 md5: {md5}')
    if SRC_MD5 and md5 != SRC_MD5:
        sys.exit('md5 不匹配，不是预期的原版 libgallium-25.3.0.so')
    cur = bytes(data[REGION_VADDR:REGION_VADDR + len(ORIG)])
    if cur == PATCH:
        sys.exit('已经是打过补丁的版本，无需重复')
    if cur != ORIG:
        print('期望:', ORIG.hex())
        print('实际:', cur.hex())
        sys.exit('补丁点原始字节不匹配——.so 版本不对？')
    data[REGION_VADDR:REGION_VADDR + len(PATCH)] = PATCH
    open(dst, 'wb').write(data)
    print(f'已写出: {dst}')
    print(f'输出 md5: {hashlib.md5(data).hexdigest()}')

if __name__ == '__main__':
    main()
