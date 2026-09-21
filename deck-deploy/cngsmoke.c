/* cngsmoke: amdipc Crypto.cpp 关键路径复现测试
 * ECDH_P256 密钥对生成 -> ECK1 导出/导入 -> BCryptSecretAgreement -> BCryptDeriveKey(HASH)
 * 双方各算一遍，比较派生密钥是否一致。
 * 构建: x86_64-w64-mingw32-gcc -O2 -o cngsmoke.exe cngsmoke.c -lbcrypt
 */
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>

#ifdef CNGFIX_SELFTEST
/* 自钩模式：把 cngfix 钩到自己的 IAT 上，验证修复后双端派生一致 */
#include "cngfix.h"
static int self_patch_iat(const char *dll_name, const char *func_name, void *replacement, void **saved)
{
    HMODULE base = GetModuleHandleA(NULL);
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE *)base + dos->e_lfanew);
    PIMAGE_IMPORT_DESCRIPTOR desc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE *)base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; desc->Name; desc++) {
        const char *name = (const char *)((BYTE *)base + desc->Name);
        PIMAGE_THUNK_DATA tname, tiat;
        if (_stricmp(name, dll_name) != 0) continue;
        tname = (PIMAGE_THUNK_DATA)((BYTE *)base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        tiat  = (PIMAGE_THUNK_DATA)((BYTE *)base + desc->FirstThunk);
        for (; tname->u1.AddressOfData; tname++, tiat++) {
            PIMAGE_IMPORT_BY_NAME ibn;
            DWORD old;
            if (tname->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            ibn = (PIMAGE_IMPORT_BY_NAME)((BYTE *)base + tname->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func_name) != 0) continue;
            *saved = (void *)tiat->u1.Function;
            if (!VirtualProtect(&tiat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return 0;
            tiat->u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&tiat->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((LONG)(s) >= 0)
#endif

static int step(const char *name, NTSTATUS st)
{
    printf("%-44s -> 0x%08lx %s\n", name, (unsigned long)st,
           NT_SUCCESS(st) ? "OK" : "FAIL");
    return NT_SUCCESS(st) ? 0 : 1;
}

/* 模拟 amdipc: DeriveKey(HASH, label=L"amdipc"... 这里 label 可选) */
static int derive(BCRYPT_SECRET_HANDLE h, const wchar_t *label,
                  unsigned char *out, DWORD *outlen)
{
    BCryptBufferDesc desc;
    BCryptBuffer buf;
    NTSTATUS st;
    DWORD result = 0;

    if (label) {
        /* amdipc 真实形态：cBuffers=1，单 buffer BufferType=0(KDF_HASH_ALGORITHM)，内容=哈希名 */
        buf.cbBuffer = (ULONG)(wcslen(label) * 2 + 2);
        buf.BufferType = 0; /* KDF_HASH_ALGORITHM */
        buf.pvBuffer = (void *)label;
        desc.ulVersion = 0;
        desc.cBuffers = 1;
        desc.pBuffers = &buf;
        /* 先取长度 */
        st = BCryptDeriveKey(h, L"HASH", &desc, NULL, 0, &result, 0);
        if (step("BCryptDeriveKey(HASH) query-len", st)) return 1;
        st = BCryptDeriveKey(h, L"HASH", &desc, out, result, &result, 0);
        if (step("BCryptDeriveKey(HASH) derive", st)) return 1;
        *outlen = result;
    } else {
        st = BCryptDeriveKey(h, L"HASH", NULL, NULL, 0, &result, 0);
        if (step("BCryptDeriveKey(HASH,nolabel) query-len", st)) return 1;
        st = BCryptDeriveKey(h, L"HASH", NULL, out, result, &result, 0);
        if (step("BCryptDeriveKey(HASH,nolabel) derive", st)) return 1;
        *outlen = result;
    }
    return 0;
}

static int extra_tests(BCRYPT_ALG_HANDLE alg, BCRYPT_KEY_HANDLE privA, BCRYPT_KEY_HANDLE pubB);

int main(void)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE kA = NULL, kB = NULL, impB = NULL, impA = NULL;
    BCRYPT_SECRET_HANDLE sA = NULL, sB = NULL;
    unsigned char pubA[72], pubB[72]; /* ECK1 blob: 8 + 32 + 32 */
    DWORD cbA = 0, cbB = 0;
    unsigned char derA[64], derB[64];
    DWORD ldA = 0, ldB = 0;
    int fails = 0;

#ifdef CNGFIX_SELFTEST
    printf("== CNGFIX_SELFTEST: installing IAT hooks ==\n");
    cngfix_install(self_patch_iat, NULL);
#endif

    fails += step("OpenAlgorithmProvider(ECDH_P256)",
        BCryptOpenAlgorithmProvider(&alg, L"ECDH_P256", NULL, 0));
    if (fails) goto out;

    fails += step("GenerateKeyPair A", BCryptGenerateKeyPair(alg, &kA, 256, 0));
    fails += step("FinalizeKeyPair A", BCryptFinalizeKeyPair(kA, 0));
    fails += step("GenerateKeyPair B", BCryptGenerateKeyPair(alg, &kB, 256, 0));
    fails += step("FinalizeKeyPair B", BCryptFinalizeKeyPair(kB, 0));
    if (fails) goto out;

    fails += step("ExportKey A (ECCKEY_BLOB/ECK1)",
        BCryptExportKey(kA, NULL, L"ECCPUBLICBLOB", pubA, sizeof(pubA), &cbA, 0));
    fails += step("ExportKey B (ECCPUBLICBLOB)",
        BCryptExportKey(kB, NULL, L"ECCPUBLICBLOB", pubB, sizeof(pubB), &cbB, 0));
    printf("  blob sizes: A=%lu B=%lu (expect 72, magic ECK1=%02x%02x%02x%02x)\n",
           cbA, cbB, pubA[0], pubA[1], pubA[2], pubA[3]);
    if (fails) goto out;

    fails += step("ImportKeyPair B-pub into A-side",
        BCryptImportKeyPair(alg, NULL, L"ECCPUBLICBLOB", &impB, pubB, cbB, 0));
    fails += step("ImportKeyPair A-pub into B-side",
        BCryptImportKeyPair(alg, NULL, L"ECCPUBLICBLOB", &impA, pubA, cbA, 0));
    if (fails) goto out;

    fails += step("SecretAgreement A(privA, pubB)",
        BCryptSecretAgreement(kA, impB, &sA, 0));
    fails += step("SecretAgreement B(privB, pubA)",
        BCryptSecretAgreement(kB, impA, &sB, 0));
    if (fails) goto out;

    /* amdipc 带 label 路径 */
    if (!derive(sA, L"SHA256", derA, &ldA) && !derive(sB, L"SHA256", derB, &ldB)) {
        printf("  derived len A=%lu B=%lu\n", ldA, ldB);
        if (ldA == ldB && !memcmp(derA, derB, ldA))
            printf("  DERIVED KEYS MATCH\n");
        else {
            printf("  DERIVED KEYS MISMATCH !!!\n");
            fails++;
        }
        printf("  A=%02x%02x%02x%02x... B=%02x%02x%02x%02x...\n",
               derA[0], derA[1], derA[2], derA[3], derB[0], derB[1], derB[2], derB[3]);
    }

    /* 无 label 路径对照 */
    {
        DWORD l2a = 0, l2b = 0;
        unsigned char d2a[64], d2b[64];
        if (!derive(sA, NULL, d2a, &l2a) && !derive(sB, NULL, d2b, &l2b)) {
            if (l2a == l2b && !memcmp(d2a, d2b, l2a))
                printf("  (nolabel) DERIVED KEYS MATCH\n");
            else { printf("  (nolabel) MISMATCH !!!\n"); fails++; }
        }
    }

    if (!fails) fails += extra_tests(alg, kA, impB);
out:
    printf(fails ? "RESULT: FAIL (%d)\n" : "RESULT: ALL OK\n", fails);
    return fails;
}

/* ===== 追加：缺陷范围界定 =====
 * 从 main() 尾部 return 前调用 extra_tests(alg, kA, impB)
 */
static int extra_tests(BCRYPT_ALG_HANDLE alg, BCRYPT_KEY_HANDLE privA, BCRYPT_KEY_HANDLE pubB)
{
    unsigned char pub[72];
    DWORD cb = 0;
    int fails = 0;

    /* 1. 从私钥句柄导出公钥（shim 方案需要） */
    fails += step("ExportKey pub from PRIVKEY handle",
        BCryptExportKey(privA, NULL, L"ECCPUBLICBLOB", pub, sizeof(pub), &cb, 0));

    /* 2. 从导入的公钥句柄再导出（对端公钥规范化用） */
    cb = 0;
    fails += step("Re-export pub from IMPORTED handle",
        BCryptExportKey(pubB, NULL, L"ECCPUBLICBLOB", pub, sizeof(pub), &cb, 0));

    /* 3. AES-256-CBC 加解密回环 */
    {
        BCRYPT_ALG_HANDLE aes = NULL;
        BCRYPT_KEY_HANDLE sk = NULL;
        unsigned char key[32], iv[16], pt[32], ct[64], dt[64];
        DWORD l1 = 0, l2 = 0;
        memset(key, 0x11, 32); memset(iv, 0x22, 16);
        memset(pt, 0x41, 32);
        fails += step("OpenAlgorithmProvider(AES)", BCryptOpenAlgorithmProvider(&aes, L"AES", NULL, 0));
        if (!fails) {
            fails += step("GenerateSymmetricKey(32B)", BCryptGenerateSymmetricKey(aes, &sk, NULL, 0, key, 32, 0));
            fails += step("SetProperty ChainingModeCBC",
                BCryptSetProperty(sk, L"ChainingMode", (PUCHAR)L"ChainingModeCBC", 28, 0));
            fails += step("BCryptEncrypt CBC", BCryptEncrypt(sk, pt, 32, NULL, iv, 16, ct, 64, &l1, 0));
            memset(iv, 0x22, 16);
            fails += step("BCryptDecrypt CBC", BCryptDecrypt(sk, ct, l1, NULL, iv, 16, dt, 64, &l2, 0));
            if (l2 == 32 && !memcmp(pt, dt, 32)) printf("  AES-CBC roundtrip MATCH\n");
            else { printf("  AES-CBC roundtrip MISMATCH\n"); fails++; }
        }
    }

    /* 4. SHA256 已知向量 */
    {
        BCRYPT_ALG_HANDLE ha = NULL; BCRYPT_HASH_HANDLE hh = NULL;
        unsigned char dg[32];
        static const unsigned char expect[32] = { /* sha256("abc") */
            0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
            0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
        fails += step("OpenAlgorithmProvider(SHA256)", BCryptOpenAlgorithmProvider(&ha, L"SHA256", NULL, 0));
        if (!fails) {
            fails += step("CreateHash", BCryptCreateHash(ha, &hh, NULL, 0, NULL, 0, 0));
            fails += step("HashData", BCryptHashData(hh, (PUCHAR)"abc", 3, 0));
            fails += step("FinishHash", BCryptFinishHash(hh, dg, 32, 0));
            if (!memcmp(dg, expect, 32)) printf("  SHA256(abc) MATCH\n");
            else { printf("  SHA256(abc) MISMATCH\n"); fails++; }
        }
    }
    return fails;
}
