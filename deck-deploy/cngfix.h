/* cngfix.h — soda/wine bcrypt 缺陷修复：BCryptDeriveKey(HASH KDF) 在 ECDH secret 上返回
 * 0xC0000002（构建时缺 ECC secret 支持："Compiled without ECC secret support"）。
 * 导致 amdipc Crypto 的 ECK1 密钥协商静默失败 → messenger 停在 state 2 → 4102 死锁。
 *
 * 修复策略（两端同代码，自洽即可）：
 *   钩 BCryptSecretAgreement：成功后导出双方 ECCPUBLICBLOB（ECK1 72B），按字典序拼接存档。
 *   钩 BCryptDeriveKey：先走真实现（未来 wine 修好就用真的），失败且句柄在册时
 *   合成 derived = SHA256(sort(pubA, pubB)) —— 两端输入相同故结果相同。
 *   钩 BCryptDestroySecret：注销。
 *
 * 依赖宿主已 IAT 钩/可解析 bcrypt 真实函数指针（patch_kb 之前先取 real_*）。
 * 用法：cngfix_install(log_fn) 安装钩子；log_fn 可为 NULL。
 */
#ifndef CNGFIX_H
#define CNGFIX_H

#include <windows.h>
#include <bcrypt.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((LONG)(s) >= 0)
#endif

typedef NTSTATUS (WINAPI *fn_BCryptSecretAgreement)(BCRYPT_KEY_HANDLE, BCRYPT_KEY_HANDLE, BCRYPT_SECRET_HANDLE *, ULONG);
typedef NTSTATUS (WINAPI *fn_BCryptDeriveKey)(BCRYPT_SECRET_HANDLE, LPCWSTR, BCryptBufferDesc *, PUCHAR, ULONG, ULONG *, ULONG);
typedef NTSTATUS (WINAPI *fn_BCryptDestroySecret)(BCRYPT_SECRET_HANDLE);
typedef NTSTATUS (WINAPI *fn_BCryptExportKey)(BCRYPT_KEY_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG *, ULONG);
typedef NTSTATUS (WINAPI *fn_BCryptOpenAlgorithmProvider)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
typedef NTSTATUS (WINAPI *fn_BCryptCreateHash)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
typedef NTSTATUS (WINAPI *fn_BCryptHashData)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS (WINAPI *fn_BCryptFinishHash)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS (WINAPI *fn_BCryptDestroyHash)(BCRYPT_HASH_HANDLE);
typedef NTSTATUS (WINAPI *fn_BCryptCloseAlgorithmProvider)(BCRYPT_ALG_HANDLE, ULONG);

static fn_BCryptSecretAgreement real_BCryptSecretAgreement;
static fn_BCryptDeriveKey real_BCryptDeriveKey;
static fn_BCryptDestroySecret real_BCryptDestroySecret;
static fn_BCryptExportKey real_BCryptExportKey;
static fn_BCryptOpenAlgorithmProvider real_BCryptOpenAlgorithmProvider;
static fn_BCryptCreateHash real_BCryptCreateHash;
static fn_BCryptHashData real_BCryptHashData;
static fn_BCryptFinishHash real_BCryptFinishHash;
static fn_BCryptDestroyHash real_BCryptDestroyHash;
static fn_BCryptCloseAlgorithmProvider real_BCryptCloseAlgorithmProvider;

static void (*cngfix_log)(const char *fmt, ...);

#define CNG_MAP_SLOTS 64
struct cng_ent { BCRYPT_SECRET_HANDLE h; unsigned char pubs[144]; int used; };
static struct cng_ent cng_map[CNG_MAP_SLOTS];
static CRITICAL_SECTION cng_cs;
static int cng_cs_init = 0;

static void cngfix_lock(void)   { if (cng_cs_init) EnterCriticalSection(&cng_cs); }
static void cngfix_unlock(void) { if (cng_cs_init) LeaveCriticalSection(&cng_cs); }

/* SHA256（走 bcrypt 真实实现，soda 上验证可用） */
static int cngfix_sha256(const void *data, ULONG len, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE ha = NULL;
    BCRYPT_HASH_HANDLE hh = NULL;
    int ok = 0;
    if (!real_BCryptOpenAlgorithmProvider) return 0;
    if (!NT_SUCCESS(real_BCryptOpenAlgorithmProvider(&ha, L"SHA256", NULL, 0))) return 0;
    if (NT_SUCCESS(real_BCryptCreateHash(ha, &hh, NULL, 0, NULL, 0, 0)) &&
        NT_SUCCESS(real_BCryptHashData(hh, (PUCHAR)data, len, 0)) &&
        NT_SUCCESS(real_BCryptFinishHash(hh, out, 32, 0)))
        ok = 1;
    if (hh) real_BCryptDestroyHash(hh);
    real_BCryptCloseAlgorithmProvider(ha, 0);
    return ok;
}

static NTSTATUS WINAPI hook_BCryptSecretAgreement(BCRYPT_KEY_HANDLE priv, BCRYPT_KEY_HANDLE pub,
                                                  BCRYPT_SECRET_HANDLE *out, ULONG flags)
{
    NTSTATUS st = real_BCryptSecretAgreement(priv, pub, out, flags);
    if (NT_SUCCESS(st) && out) {
        unsigned char p1[72], p2[72];
        ULONG cb1 = 0, cb2 = 0;
        /* 双方公钥：priv 句柄导出自身公钥 + 对端公钥句柄再导出（canonical 72B ECK1） */
        if (NT_SUCCESS(real_BCryptExportKey(priv, NULL, L"ECCPUBLICBLOB", p1, sizeof(p1), &cb1, 0)) && cb1 == 72 &&
            NT_SUCCESS(real_BCryptExportKey(pub,  NULL, L"ECCPUBLICBLOB", p2, sizeof(p2), &cb2, 0)) && cb2 == 72) {
            int i;
            cngfix_lock();
            for (i = 0; i < CNG_MAP_SLOTS; i++) if (!cng_map[i].used) {
                cng_map[i].h = *out;
                if (memcmp(p1, p2, 72) <= 0) { memcpy(cng_map[i].pubs, p1, 72); memcpy(cng_map[i].pubs + 72, p2, 72); }
                else                         { memcpy(cng_map[i].pubs, p2, 72); memcpy(cng_map[i].pubs + 72, p1, 72); }
                cng_map[i].used = 1;
                if (cngfix_log) cngfix_log("cngfix: secret %p recorded (ECDH pub pair)", *out);
                break;
            }
            cngfix_unlock();
        }
    }
    return st;
}

static NTSTATUS WINAPI hook_BCryptDeriveKey(BCRYPT_SECRET_HANDLE h, LPCWSTR kdf,
                                            BCryptBufferDesc *desc, PUCHAR out, ULONG outlen,
                                            ULONG *result, ULONG flags)
{
    NTSTATUS st = real_BCryptDeriveKey(h, kdf, desc, out, outlen, result, flags);
    if (!NT_SUCCESS(st) && kdf && !wcscmp(kdf, BCRYPT_KDF_HASH)) {
        int i, found = -1;
        unsigned char digest[32];
        cngfix_lock();
        for (i = 0; i < CNG_MAP_SLOTS; i++) if (cng_map[i].used && cng_map[i].h == h) { found = i; break; }
        if (found >= 0 && cngfix_sha256(cng_map[found].pubs, 144, digest)) {
            if (!out) { *result = 32; st = 0; }
            else {
                ULONG n = outlen < 32 ? outlen : 32;
                memcpy(out, digest, n);
                *result = n;
                st = 0;
            }
            if (cngfix_log) cngfix_log("cngfix: DeriveKey(HASH) synthesized for %p", h);
        }
        cngfix_unlock();
    }
    return st;
}

static NTSTATUS WINAPI hook_BCryptDestroySecret(BCRYPT_SECRET_HANDLE h)
{
    int i;
    cngfix_lock();
    for (i = 0; i < CNG_MAP_SLOTS; i++) if (cng_map[i].used && cng_map[i].h == h) cng_map[i].used = 0;
    cngfix_unlock();
    return real_BCryptDestroySecret(h);
}

/* 由宿主提供 patch 函数：int (*patch)(const char *dll, const char *func, void *hookfn, void **saved) */
static int cngfix_install(int (*patch)(const char *, const char *, void *, void **),
                          void (*logfn)(const char *, ...))
{
    int n = 0;
    cngfix_log = logfn;
    if (!cng_cs_init) { InitializeCriticalSection(&cng_cs); cng_cs_init = 1; }
    n += patch("bcrypt.dll", "BCryptSecretAgreement", hook_BCryptSecretAgreement, (void **)&real_BCryptSecretAgreement);
    n += patch("bcrypt.dll", "BCryptDeriveKey",       hook_BCryptDeriveKey,       (void **)&real_BCryptDeriveKey);
    n += patch("bcrypt.dll", "BCryptDestroySecret",   hook_BCryptDestroySecret,   (void **)&real_BCryptDestroySecret);
    /* 真实函数直取（不经过钩子，供内部 hash 用） */
    {
        HMODULE hb = GetModuleHandleA("bcrypt.dll");
        if (hb) {
            real_BCryptExportKey = (fn_BCryptExportKey)GetProcAddress(hb, "BCryptExportKey");
            real_BCryptOpenAlgorithmProvider = (fn_BCryptOpenAlgorithmProvider)GetProcAddress(hb, "BCryptOpenAlgorithmProvider");
            real_BCryptCreateHash = (fn_BCryptCreateHash)GetProcAddress(hb, "BCryptCreateHash");
            real_BCryptHashData = (fn_BCryptHashData)GetProcAddress(hb, "BCryptHashData");
            real_BCryptFinishHash = (fn_BCryptFinishHash)GetProcAddress(hb, "BCryptFinishHash");
            real_BCryptDestroyHash = (fn_BCryptDestroyHash)GetProcAddress(hb, "BCryptDestroyHash");
            real_BCryptCloseAlgorithmProvider = (fn_BCryptCloseAlgorithmProvider)GetProcAddress(hb, "BCryptCloseAlgorithmProvider");
        }
    }
    if (logfn) logfn("cngfix: %d/3 bcrypt hooks, helpers %s", n,
                     real_BCryptCreateHash ? "ok" : "MISSING");
    return n;
}

#endif /* CNGFIX_H */
