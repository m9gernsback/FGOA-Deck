/*
 * ipcdump.dll v6 — FGOA Wine 诊断：ago.exe/amdaemon.exe 的 amdipc IPC 全观测
 * v6 新增（v5 Deck 双进程 transcript 定位对称死锁后，追杀 Deck 独有的
 *       amdaemon 握手后两次 WaitForSingleObject(h=NULL)→0xffffffff）：
 *   - CreateThread（记 start addr/flags/返回句柄/tid/err——怀疑有线程没建成）
 *   - _beginthreadex（msvcrt.dll/ucrtbase.dll 双名尝试）
 *   - OpenProcess（记 pid/access/返回值/err——客户端 liveness watch 嫌疑）
 *   - CreateEventW 改记无名事件（每线程防洪 100 条；命名事件照旧）
 * v5 已有：ReadFileEx/WriteFileEx+完成例程蹦床、alertable 泵点（0xC0）、
 *   管道创建/连接/读写钩子、命名对象/shmdump（daemon 门控）、原子追加
 * 输出：C:\FGOA\logs\ipc-dump.log（追加）。只改内存，不动文件。
 */
/* v5 原文档注释：ago.exe/amdaemon.exe 的 amdipc IPC 全观测
 * v5 新增（v4 Deck 实测发现 amdaemon 的管道读写零 ReadFile/WriteFile 命中，
 *       objdump 证实 amdipc 走 ReadFileEx/WriteFileEx + 完成例程）：
 *   - ReadFileEx/WriteFileEx（仅管道句柄）+ **完成例程蹦床**：替换 app 的
 *     LPOVERLAPPED_COMPLETION_ROUTINE 为我们的 trampoline，先记 err/done +
 *     dump 读到的 64B，再调回原例程（OVERLAPPED 指针不变，按指针建映射表）
 *   - SleepEx / WaitForSingleObjectEx 的 alertable 等待记录（防洪）——
 *     完成例程只在 alertable 等待里触发；返回值 0xC0=WAIT_IO_COMPLETION 是
 *     "APC 泵活着"的直接证据
 *   - DeviceIoControl（仅管道句柄）
 * v4 已有：CRYPT32 报文转储、命名对象/MapViewOfFile/shmdump（daemon 门控）、
 *   管道创建/连接/等待钩子、ReadFile/WriteFile、无名句柄 INFINITE 等待、
 *   FILE_APPEND_DATA 原子追加
 * 输出：C:\FGOA\logs\ipc-dump.log（追加）。只改内存，不动文件。
 */
/* v4 原文档注释：ago.exe/amdaemon.exe 的 amdipc IPC 全观测
 * 背景：ago↔amdaemon 的 amdipc 握手成功（256B 往返）后，post-handshake 消息流
 *       在 Deck 上从未启动（PC 对照组握手后 +31ms 起有数百条 16B 管道消息）。
 *       v3 已证实 Deck 上 amdaemon 建好了 daemon_backup shm 并写了首个 272B 块，
 *       但 ago 从不读。断点在管道层，而 v3 对管道 I/O 全盲 → v4 加管道钩子。
 * 钩子：
 *   - CRYPT32!CryptProtectMemory/UnprotectMemory（amdipc 报文缓冲）
 *   - KERNEL32 命名对象（FileMapping/Event/Mutex + MapViewOfFile + Wait + SetEvent）
 *   - KERNEL32 管道 I/O（v4 新增）：CreateFileW/A（仅 \\.\pipe\ 路径）、
 *     CreateNamedPipeW、ConnectNamedPipe、DisconnectNamedPipe、WaitNamedPipeW、
 *     TransactNamedPipe、PeekNamedPipe、SetNamedPipeHandleState、
 *     ReadFile/WriteFile（仅已知管道句柄，dump 前 64B）、GetOverlappedResult、
 *     FlushFileBuffers（仅管道）、CloseHandle（注销句柄跟踪）
 *   - WaitForSingleObject/Ex：命名句柄全记；未知名句柄仅记 ms==INFINITE
 *     （每个句柄最多 5 条，防洪）→ 抓"泵线程卡在无名事件上"
 *   - WaitForMultipleObjects（v4 新增，同款防洪）
 * v4 修复：
 *   - 日志打开改用 FILE_APPEND_DATA（原子追加）——v3 双进程写同一文件有交错损坏
 *   - shmdump 只在 daemon/amdipc 互斥锁上触发——v3 被 amdaemon 的
 *     SystemProperty 轮询刷屏（73 万行 / 680MB）
 * 输出：C:\FGOA\logs\ipc-dump.log（追加）。只改内存，不动文件。
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <bcrypt.h>
#include "cngfix.h"

static HANDLE log_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION log_cs;

static void log_line(const char *fmt, ...);   /* 前向声明 */

/* ---- 句柄→名字 表（命名对象：mutex/event/mapping） ---- */
#define MAX_NAMED 512
static struct { HANDLE h; WCHAR name[128]; } named[MAX_NAMED];
static int named_count;

/* ---- 管道句柄表（句柄→管道名） ---- */
#define MAX_PIPES 64
static struct { HANDLE h; WCHAR name[128]; } pipes[MAX_PIPES];
static int pipe_count;

/* ---- 命名 mapping 的视图表 ---- */
#define MAX_VIEWS 64
static struct { LPVOID addr; SIZE_T size; WCHAR name[128]; } views[MAX_VIEWS];
static int view_count;

/* ---- 无名句柄等待的防洪计数 ---- */
#define MAX_UNK 128
static struct { HANDLE h; int n; } unk_waits[MAX_UNK];
static int unk_count;

static int wcs_icontains(const WCHAR *hay, const char *needle)
{
    /* hay 宽串里找 ASCII needle（大小写不敏感） */
    int i, j;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++) {
            WCHAR c = hay[i + j];
            if (!c) return 0;
            if (c >= 'A' && c <= 'Z') c += 32;
            if ((char)c != (char)tolower((unsigned char)needle[j])) break;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

static void view_register(HANDLE mapping, LPCWSTR name, LPVOID addr, SIZE_T size)
{
    if (!addr || !name || !name[0]) return;
    if (view_count < MAX_VIEWS) {
        views[view_count].addr = addr;
        views[view_count].size = size;
        wcsncpy(views[view_count].name, name, 127);
        views[view_count].name[127] = 0;
        view_count++;
    }
}

/* 在 daemon/amdipc 互斥锁 获取/释放 时转储所有命名 shm 视图内容（各最多 1536B） */
static void dump_views(const WCHAR *mtx_name)
{
    int i;
    if (!mtx_name) return;
    if (!wcs_icontains(mtx_name, "daemon") && !wcs_icontains(mtx_name, "amdipc"))
        return;   /* v4：SystemProperty 等轮询互斥锁不再触发转储 */
    for (i = 0; i < view_count; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T n, j;
        char *hex;
        const unsigned char *p;
        if (VirtualQuery(views[i].addr, &mbi, sizeof(mbi)) == 0) continue;
        if (mbi.State != MEM_COMMIT) continue;
        n = views[i].size > 1536 ? 1536 : views[i].size;
        p = (const unsigned char *)views[i].addr;
        hex = malloc(n * 2 + 1);
        if (!hex) return;
        for (j = 0; j < n; j++) sprintf(hex + j * 2, "%02x", p[j]);
        log_line("shmdump [%S] (%S) size=%lu | %s", mtx_name, views[i].name,
                 (unsigned long)views[i].size, hex);
        free(hex);
    }
}

static void name_register(HANDLE h, const WCHAR *name)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE || !name || !name[0]) return;
    for (i = 0; i < named_count; i++)
        if (named[i].h == h) return;
    if (named_count < MAX_NAMED) {
        named[named_count].h = h;
        wcsncpy(named[named_count].name, name, 127);
        named[named_count].name[127] = 0;
        named_count++;
    }
}

static const WCHAR *name_lookup(HANDLE h)
{
    int i;
    for (i = 0; i < named_count; i++)
        if (named[i].h == h) return named[i].name;
    return NULL;
}

static void name_unregister(HANDLE h)
{
    int i;
    for (i = 0; i < named_count; i++)
        if (named[i].h == h) {
            named[i] = named[--named_count];
            return;
        }
}

static void pipe_register(HANDLE h, const WCHAR *name)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE || !name || !name[0]) return;
    for (i = 0; i < pipe_count; i++)
        if (pipes[i].h == h) return;
    if (pipe_count < MAX_PIPES) {
        pipes[pipe_count].h = h;
        wcsncpy(pipes[pipe_count].name, name, 127);
        pipes[pipe_count].name[127] = 0;
        pipe_count++;
    }
}

static const WCHAR *pipe_lookup(HANDLE h)
{
    int i;
    for (i = 0; i < pipe_count; i++)
        if (pipes[i].h == h) return pipes[i].name;
    return NULL;
}

static void pipe_unregister(HANDLE h)
{
    int i;
    for (i = 0; i < pipe_count; i++)
        if (pipes[i].h == h) {
            pipes[i] = pipes[--pipe_count];
            return;
        }
}

static void log_line(const char *fmt, ...)
{
    char line[4096];
    char *o = line;
    va_list ap;
    DWORD written;
    /* v8: IPCDUMP_QUIET=1 安静模式——只放行 cngfix 日志（4102 修复链的存活证据），
     * 报文转储/句柄事件全静默。cngfix.h 所有日志均以 "cngfix" 前缀开头。 */
    static int g_quiet = -1;
    if (g_quiet < 0) {
        char ev[8];
        g_quiet = (GetEnvironmentVariableA("IPCDUMP_QUIET", ev, sizeof(ev)) > 0 && ev[0] == '1');
    }
    if (g_quiet && strncmp(fmt, "cngfix", 6) != 0)
        return;
    o += sprintf(o, "[%lu] pid=%lu tid=%lu ", GetTickCount(),
                 GetCurrentProcessId(), GetCurrentThreadId());
    va_start(ap, fmt);
    o += vsprintf(o, fmt, ap);
    va_end(ap);
    *o++ = '\r'; *o++ = '\n';
    EnterCriticalSection(&log_cs);
    if (log_file != INVALID_HANDLE_VALUE)
        WriteFile(log_file, line, (DWORD)(o - line), &written, NULL);
    LeaveCriticalSection(&log_cs);
}

static void dump_buf(const char *tag, LPVOID pData, DWORD cbData)
{
    char hex[520];
    char txt[270];
    DWORD n = cbData > 256 ? 256 : cbData;
    DWORD i;
    const unsigned char *p = (const unsigned char *)pData;
    for (i = 0; i < n; i++) sprintf(hex + i * 2, "%02x", p[i]);
    for (i = 0; i < n; i++) txt[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
    txt[n] = 0;
    log_line("%s len=%lu | %s | %s", tag, (unsigned long)cbData, hex, txt);
}

/* 管道读写的 64B 行内转储 */
static void fmt_hex64(const void *data, DWORD avail, char *hex, char *txt)
{
    DWORD n = avail > 64 ? 64 : avail, i;
    const unsigned char *p = (const unsigned char *)data;
    hex[0] = 0; txt[0] = 0;
    if (!data || !avail) return;
    for (i = 0; i < n; i++) sprintf(hex + i * 2, "%02x", p[i]);
    for (i = 0; i < n; i++) txt[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
    txt[n] = 0;
}

/* ---- CRYPT32 ---- */
typedef BOOL(WINAPI *CryptMemFn)(LPVOID, DWORD, DWORD);
static CryptMemFn real_protect, real_unprotect;

static BOOL WINAPI hook_protect(LPVOID p, DWORD cb, DWORD f)
{
    dump_buf("protect  ", p, cb);
    return real_protect(p, cb, f);
}
static BOOL WINAPI hook_unprotect(LPVOID p, DWORD cb, DWORD f)
{
    dump_buf("unprotect", p, cb);
    return real_unprotect(p, cb, f);
}

/* ---- KERNEL32 命名对象 ---- */
typedef HANDLE(WINAPI *CreateFileMappingWFn)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
typedef HANDLE(WINAPI *OpenFileMappingWFn)(DWORD, BOOL, LPCWSTR);
typedef HANDLE(WINAPI *CreateEventWFn)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
typedef HANDLE(WINAPI *OpenEventWFn)(DWORD, BOOL, LPCWSTR);
typedef HANDLE(WINAPI *CreateMutexWFn)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
typedef HANDLE(WINAPI *OpenMutexWFn)(DWORD, BOOL, LPCWSTR);
typedef DWORD(WINAPI *WaitFn)(HANDLE, DWORD);
typedef DWORD(WINAPI *WaitExFn)(HANDLE, DWORD, BOOL);
typedef DWORD(WINAPI *WaitMultiFn)(DWORD, const HANDLE *, BOOL, DWORD);
typedef BOOL(WINAPI *SetEventFn)(HANDLE);
typedef LPVOID(WINAPI *MapViewOfFileFn)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
typedef BOOL(WINAPI *ReleaseMutexFn)(HANDLE);

static CreateFileMappingWFn real_CreateFileMappingW;
static OpenFileMappingWFn real_OpenFileMappingW;
static CreateEventWFn real_CreateEventW;
static OpenEventWFn real_OpenEventW;
static CreateMutexWFn real_CreateMutexW;
static OpenMutexWFn real_OpenMutexW;
static WaitFn real_WaitForSingleObject;
static WaitExFn real_WaitForSingleObjectEx;
static WaitMultiFn real_WaitForMultipleObjects;
static SetEventFn real_SetEvent;
static MapViewOfFileFn real_MapViewOfFile;
static ReleaseMutexFn real_ReleaseMutex;

static LPVOID WINAPI hook_MapViewOfFile(HANDLE mapping, DWORD acc, DWORD hi, DWORD lo, SIZE_T size)
{
    LPVOID p = real_MapViewOfFile(mapping, acc, hi, lo, size);
    const WCHAR *n = name_lookup(mapping);
    if (p && n) {
        log_line("MapViewOfFile %S -> %p size=%lu", n, p, (unsigned long)size);
        view_register(mapping, n, p, size);
    }
    return p;
}
static BOOL WINAPI hook_ReleaseMutex(HANDLE h)
{
    const WCHAR *n = name_lookup(h);
    if (n) {
        log_line("ReleaseMutex %S", n);
        dump_views(n);
    }
    return real_ReleaseMutex(h);
}

static HANDLE WINAPI hook_CreateFileMappingW(HANDLE hf, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR name)
{
    HANDLE h = real_CreateFileMappingW(hf, sa, prot, hi, lo, name);
    if (name && name[0]) {
        log_line("CreateFileMappingW %S -> %p err=%lu", name, h, GetLastError());
        name_register(h, name);
    }
    return h;
}
static HANDLE WINAPI hook_OpenFileMappingW(DWORD acc, BOOL inh, LPCWSTR name)
{
    HANDLE h = real_OpenFileMappingW(acc, inh, name);
    if (name && name[0]) {
        log_line("OpenFileMappingW %S -> %p err=%lu", name, h, GetLastError());
        name_register(h, name);
    }
    return h;
}
/* v6：无名 CreateEvent 防洪（每线程最多 100 条） */
static int anon_event_count;

static HANDLE WINAPI hook_CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL mr, BOOL is_, LPCWSTR name)
{
    HANDLE h = real_CreateEventW(sa, mr, is_, name);
    DWORD e = GetLastError();
    if (name && name[0]) {
        log_line("CreateEventW %S -> %p err=%lu", name, h, e);
        name_register(h, name);
    } else if (anon_event_count < 100) {
        anon_event_count++;
        log_line("CreateEventW (anon) mr=%d init=%d -> %p err=%lu", mr, is_, h, e);
    }
    SetLastError(e);
    return h;
}
static HANDLE WINAPI hook_OpenEventW(DWORD acc, BOOL inh, LPCWSTR name)
{
    HANDLE h = real_OpenEventW(acc, inh, name);
    if (name && name[0]) {
        log_line("OpenEventW %S -> %p err=%lu", name, h, GetLastError());
        name_register(h, name);
    }
    return h;
}
static HANDLE WINAPI hook_CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCWSTR name)
{
    HANDLE h = real_CreateMutexW(sa, own, name);
    if (name && name[0]) {
        log_line("CreateMutexW %S -> %p err=%lu", name, h, GetLastError());
        name_register(h, name);
    }
    return h;
}
static HANDLE WINAPI hook_OpenMutexW(DWORD acc, BOOL inh, LPCWSTR name)
{
    HANDLE h = real_OpenMutexW(acc, inh, name);
    if (name && name[0]) {
        log_line("OpenMutexW %S -> %p err=%lu", name, h, GetLastError());
        name_register(h, name);
    }
    return h;
}

/* 无名句柄等待防洪：每个句柄只记前 5 次 */
static int unk_wait_allow(HANDLE h)
{
    int i;
    for (i = 0; i < unk_count; i++)
        if (unk_waits[i].h == h) {
            if (unk_waits[i].n >= 5) return 0;
            unk_waits[i].n++;
            return 1;
        }
    if (unk_count < MAX_UNK) {
        unk_waits[unk_count].h = h;
        unk_waits[unk_count].n = 1;
        unk_count++;
        return 1;
    }
    return 0;
}

static DWORD WINAPI hook_WaitForSingleObject(HANDLE h, DWORD ms)
{
    DWORD r = real_WaitForSingleObject(h, ms);
    const WCHAR *n = name_lookup(h);
    const WCHAR *pn;
    if (n) {
        log_line("WaitForSingleObject %S ms=%lu -> 0x%lx", n, (unsigned long)ms, (unsigned long)r);
        if (r == 0) dump_views(n);
    } else if ((pn = pipe_lookup(h)) != NULL) {
        log_line("WaitForSingleObject PIPE %S ms=%lu -> 0x%lx", pn, (unsigned long)ms, (unsigned long)r);
    } else if (ms == 4294967295UL && unk_wait_allow(h)) {
        log_line("WaitForSingleObject UNNAMED h=%p ms=INFINITE -> 0x%lx", h, (unsigned long)r);
    }
    return r;
}
static DWORD WINAPI hook_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alert)
{
    DWORD r = real_WaitForSingleObjectEx(h, ms, alert);
    const WCHAR *n = name_lookup(h);
    if (n) {
        log_line("WaitForSingleObjectEx %S ms=%lu alert=%d -> 0x%lx", n, (unsigned long)ms, alert, (unsigned long)r);
    } else if ((alert || r == 0xC0) && unk_wait_allow(h)) {
        /* v5：alertable 等待 = 完成例程泵点；0xC0=WAIT_IO_COMPLETION 是 APC 触发证据 */
        log_line("WaitForSingleObjectEx UNNAMED h=%p ms=%lu alert=%d -> 0x%lx%s",
                 h, (unsigned long)ms, alert, (unsigned long)r,
                 r == 0xC0 ? " (APC fired)" : "");
    }
    return r;
}
static DWORD WINAPI hook_WaitForMultipleObjects(DWORD cnt, const HANDLE *hs, BOOL all, DWORD ms)
{
    DWORD r = real_WaitForMultipleObjects(cnt, hs, all, ms);
    DWORD i;
    int any_named = 0;
    for (i = 0; i < cnt && i < 16; i++)
        if (name_lookup(hs[i]) || pipe_lookup(hs[i])) any_named = 1;
    if (any_named || (ms == 4294967295UL && cnt > 0 && unk_wait_allow(hs[0]))) {
        log_line("WaitForMultipleObjects cnt=%lu all=%d ms=%lu -> 0x%lx",
                 (unsigned long)cnt, all, (unsigned long)ms, (unsigned long)r);
        for (i = 0; i < cnt && i < 16; i++) {
            const WCHAR *n = name_lookup(hs[i]);
            const WCHAR *pn = pipe_lookup(hs[i]);
            if (n) log_line("  h[%lu]=%p named %S", (unsigned long)i, hs[i], n);
            else if (pn) log_line("  h[%lu]=%p pipe %S", (unsigned long)i, hs[i], pn);
        }
    }
    return r;
}
static BOOL WINAPI hook_SetEvent(HANDLE h)
{
    const WCHAR *n = name_lookup(h);
    if (n) log_line("SetEvent %S", n);
    return real_SetEvent(h);
}

/* ---- KERNEL32 管道 I/O（v4） ---- */
typedef HANDLE(WINAPI *CreateFileWFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI *CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI *CreateNamedPipeWFn)(LPCWSTR, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPSECURITY_ATTRIBUTES);
typedef BOOL(WINAPI *ConnectNamedPipeFn)(HANDLE, LPOVERLAPPED);
typedef BOOL(WINAPI *DisconnectNamedPipeFn)(HANDLE);
typedef BOOL(WINAPI *WaitNamedPipeWFn)(LPCWSTR, DWORD);
typedef BOOL(WINAPI *TransactNamedPipeFn)(HANDLE, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI *PeekNamedPipeFn)(HANDLE, LPVOID, DWORD, LPDWORD, LPDWORD, LPDWORD);
typedef BOOL(WINAPI *SetNamedPipeHandleStateFn)(HANDLE, LPDWORD, LPDWORD, LPDWORD);
typedef BOOL(WINAPI *ReadFileFn)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI *WriteFileFn)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI *GetOverlappedResultFn)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
typedef BOOL(WINAPI *FlushFileBuffersFn)(HANDLE);
typedef BOOL(WINAPI *CloseHandleFn)(HANDLE);
typedef BOOL(WINAPI *ReadFileExFn)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
typedef BOOL(WINAPI *WriteFileExFn)(HANDLE, LPCVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
typedef DWORD(WINAPI *SleepExFn)(DWORD, BOOL);
typedef BOOL(WINAPI *DeviceIoControlFn)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);

static CreateFileWFn real_CreateFileW;
static CreateFileAFn real_CreateFileA;
static CreateNamedPipeWFn real_CreateNamedPipeW;
static ConnectNamedPipeFn real_ConnectNamedPipe;
static DisconnectNamedPipeFn real_DisconnectNamedPipe;
static WaitNamedPipeWFn real_WaitNamedPipeW;
static TransactNamedPipeFn real_TransactNamedPipe;
static PeekNamedPipeFn real_PeekNamedPipe;
static SetNamedPipeHandleStateFn real_SetNamedPipeHandleState;
static ReadFileFn real_ReadFile;
static WriteFileFn real_WriteFile;
static GetOverlappedResultFn real_GetOverlappedResult;
static FlushFileBuffersFn real_FlushFileBuffers;
static CloseHandleFn real_CloseHandle;
static ReadFileExFn real_ReadFileEx;
static WriteFileExFn real_WriteFileEx;
static SleepExFn real_SleepEx;
static DeviceIoControlFn real_DeviceIoControl;

/* ---- ReadFileEx/WriteFileEx 完成例程蹦床（v5） ---- */
#define MAX_OVL 64
static struct {
    LPOVERLAPPED ovl;
    LPOVERLAPPED_COMPLETION_ROUTINE orig;
    LPVOID buf;
    DWORD req;
    int is_write;
    WCHAR pname[64];
} ovls[MAX_OVL];
static CRITICAL_SECTION ovl_cs;

static void WINAPI ovl_cb_trampoline(DWORD err, DWORD done, LPOVERLAPPED ovl)
{
    LPOVERLAPPED_COMPLETION_ROUTINE orig = NULL;
    LPVOID buf = NULL;
    DWORD req = 0;
    int is_write = 0, i;
    WCHAR pname[64]; pname[0] = 0;
    EnterCriticalSection(&ovl_cs);
    for (i = 0; i < MAX_OVL; i++)
        if (ovls[i].ovl == ovl) {
            orig = ovls[i].orig;
            buf = ovls[i].buf;
            req = ovls[i].req;
            is_write = ovls[i].is_write;
            wcsncpy(pname, ovls[i].pname, 63);
            ovls[i].ovl = NULL;   /* 释放槽位 */
            break;
        }
    LeaveCriticalSection(&ovl_cs);
    if (orig) {
        char hex[129], txt[65];
        hex[0] = 0; txt[0] = 0;
        if (!is_write && done && buf) fmt_hex64(buf, done, hex, txt);
        log_line("%s %S err=%lu done=%lu req=%lu | %s | %s",
                 is_write ? "pipeWriteDone" : "pipeReadDone",
                 pname[0] ? pname : L"?", (unsigned long)err,
                 (unsigned long)done, (unsigned long)req, hex, txt);
        orig(err, done, ovl);
    }
}

static BOOL ovl_register(LPOVERLAPPED ovl, LPOVERLAPPED_COMPLETION_ROUTINE orig,
                         LPVOID buf, DWORD req, int is_write, const WCHAR *pname)
{
    int i;
    BOOL ok = FALSE;
    EnterCriticalSection(&ovl_cs);
    for (i = 0; i < MAX_OVL; i++)
        if (!ovls[i].ovl) {
            ovls[i].ovl = ovl;
            ovls[i].orig = orig;
            ovls[i].buf = buf;
            ovls[i].req = req;
            ovls[i].is_write = is_write;
            wcsncpy(ovls[i].pname, pname ? pname : L"?", 63);
            ovls[i].pname[63] = 0;
            ok = TRUE;
            break;
        }
    LeaveCriticalSection(&ovl_cs);
    return ok;
}

static BOOL WINAPI hook_ReadFileEx(HANDLE h, LPVOID buf, DWORD len, LPOVERLAPPED ovl,
                                   LPOVERLAPPED_COMPLETION_ROUTINE cb)
{
    const WCHAR *n = pipe_lookup(h);
    BOOL r;
    DWORD e;
    if (!n) return real_ReadFileEx(h, buf, len, ovl, cb);
    if (cb && ovl && ovl_register(ovl, cb, buf, len, 0, n)) {
        r = real_ReadFileEx(h, buf, len, ovl, ovl_cb_trampoline);
        e = GetLastError();
        log_line("pipeReadEx %S req=%lu ovl=%p cb=tramp -> %d err=%lu",
                 n, (unsigned long)len, ovl, r, e);
    } else {
        r = real_ReadFileEx(h, buf, len, ovl, cb);
        e = GetLastError();
        log_line("pipeReadEx %S req=%lu ovl=%p cb=%p(NOTRAMP) -> %d err=%lu",
                 n, (unsigned long)len, ovl, cb, r, e);
    }
    SetLastError(e);
    return r;
}
static BOOL WINAPI hook_WriteFileEx(HANDLE h, LPCVOID buf, DWORD len, LPOVERLAPPED ovl,
                                    LPOVERLAPPED_COMPLETION_ROUTINE cb)
{
    const WCHAR *n = pipe_lookup(h);
    BOOL r;
    DWORD e;
    char hex[129], txt[65];
    if (!n) return real_WriteFileEx(h, buf, len, ovl, cb);
    fmt_hex64(buf, len, hex, txt);
    if (cb && ovl && ovl_register(ovl, cb, (LPVOID)buf, len, 1, n)) {
        r = real_WriteFileEx(h, buf, len, ovl, ovl_cb_trampoline);
        e = GetLastError();
        log_line("pipeWriteEx %S len=%lu ovl=%p cb=tramp -> %d err=%lu | %s | %s",
                 n, (unsigned long)len, ovl, r, e, hex, txt);
    } else {
        r = real_WriteFileEx(h, buf, len, ovl, cb);
        e = GetLastError();
        log_line("pipeWriteEx %S len=%lu ovl=%p cb=%p(NOTRAMP) -> %d err=%lu | %s | %s",
                 n, (unsigned long)len, ovl, cb, r, e, hex, txt);
    }
    SetLastError(e);
    return r;
}

/* SleepEx：只记 alertable 的（完成例程泵点），按 ms 值防洪 */
#define MAX_SleepEx_SEEN 32
static struct { DWORD ms; int n; } sleepex_seen[MAX_SleepEx_SEEN];
static int sleepex_seen_count;

static DWORD WINAPI hook_SleepEx(DWORD ms, BOOL alert)
{
    DWORD r = real_SleepEx(ms, alert);
    if (alert) {
        int i, allow = 0;
        for (i = 0; i < sleepex_seen_count; i++)
            if (sleepex_seen[i].ms == ms) {
                if (sleepex_seen[i].n < 5) { sleepex_seen[i].n++; allow = 1; }
                break;
            }
        if (i == sleepex_seen_count && sleepex_seen_count < MAX_SleepEx_SEEN) {
            sleepex_seen[sleepex_seen_count].ms = ms;
            sleepex_seen[sleepex_seen_count].n = 1;
            sleepex_seen_count++;
            allow = 1;
        }
        if (allow)
            log_line("SleepEx ms=%lu alert=1 -> 0x%lx%s", (unsigned long)ms,
                     (unsigned long)r, r == 0xC0 ? " (APC fired)" : "");
    }
    return r;
}
static BOOL WINAPI hook_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD inLen,
                                        LPVOID out, DWORD outLen, LPDWORD done, LPOVERLAPPED ovl)
{
    const WCHAR *n = pipe_lookup(h);
    BOOL r;
    DWORD e;
    if (!n) return real_DeviceIoControl(h, code, in, inLen, out, outLen, done, ovl);
    r = real_DeviceIoControl(h, code, in, inLen, out, outLen, done, ovl);
    e = GetLastError();
    log_line("DeviceIoControl %S code=%lx in=%lu out=%lu ovl=%p -> %d err=%lu",
             n, (unsigned long)code, (unsigned long)inLen, (unsigned long)outLen,
             ovl, r, e);
    SetLastError(e);
    return r;
}

/* ---- v6：线程/进程/无名事件创建追踪 ---- */
typedef HANDLE(WINAPI *CreateThreadFn)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
typedef HANDLE(WINAPI *OpenProcessFn)(DWORD, BOOL, DWORD);
typedef uintptr_t(WINAPI *beginthreadexFn)(void *, unsigned, unsigned (__stdcall *)(void *), void *, unsigned, unsigned *);

static CreateThreadFn real_CreateThread;
static OpenProcessFn real_OpenProcess;
static beginthreadexFn real_beginthreadex;

static HANDLE WINAPI hook_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack,
                                       LPTHREAD_START_ROUTINE start, LPVOID param,
                                       DWORD flags, LPDWORD outTid)
{
    HANDLE h = real_CreateThread(sa, stack, start, param, flags, outTid);
    DWORD e = GetLastError();
    log_line("CreateThread start=%p flags=%lx -> h=%p tid=%lu err=%lu",
             start, (unsigned long)flags, h,
             (unsigned long)(outTid ? *outTid : 0), e);
    SetLastError(e);
    return h;
}
static HANDLE WINAPI hook_OpenProcess(DWORD acc, BOOL inh, DWORD pid)
{
    HANDLE h = real_OpenProcess(acc, inh, pid);
    DWORD e = GetLastError();
    log_line("OpenProcess pid=%lu acc=%lx -> %p err=%lu",
             (unsigned long)pid, (unsigned long)acc, h, e);
    SetLastError(e);
    return h;
}
static uintptr_t WINAPI hook_beginthreadex(void *sa, unsigned stack,
                                           unsigned (__stdcall *start)(void *), void *param,
                                           unsigned flags, unsigned *outTid)
{
    uintptr_t h = real_beginthreadex(sa, stack, start, param, flags, outTid);
    log_line("_beginthreadex start=%p flags=%x -> h=%Ix tid=%u",
             start, flags, h, outTid ? *outTid : 0);
    return h;
}

static HANDLE WINAPI hook_CreateFileW(LPCWSTR path, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD attr, HANDLE tmpl)
{
    HANDLE h = real_CreateFileW(path, acc, share, sa, disp, attr, tmpl);
    if (path && wcs_icontains(path, "pipe")) {
        log_line("CreateFileW %S acc=%lx share=%lx disp=%lu -> %p err=%lu",
                 path, acc, share, (unsigned long)disp, h, GetLastError());
        if (h != INVALID_HANDLE_VALUE) pipe_register(h, path);
    }
    return h;
}
static HANDLE WINAPI hook_CreateFileA(LPCSTR path, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD attr, HANDLE tmpl)
{
    HANDLE h = real_CreateFileA(path, acc, share, sa, disp, attr, tmpl);
    if (path && strstr(path, "pipe") || path && strstr(path, "PIPE") || path && strstr(path, "Pipe")) {
        WCHAR wn[128];
        MultiByteToWideChar(CP_ACP, 0, path, -1, wn, 127);
        log_line("CreateFileA %s acc=%lx share=%lx disp=%lu -> %p err=%lu",
                 path, acc, share, (unsigned long)disp, h, GetLastError());
        if (h != INVALID_HANDLE_VALUE) pipe_register(h, wn);
    }
    return h;
}
static HANDLE WINAPI hook_CreateNamedPipeW(LPCWSTR name, DWORD openMode, DWORD pipeMode, DWORD maxInst,
                                           DWORD outBuf, DWORD inBuf, DWORD timeout, LPSECURITY_ATTRIBUTES sa)
{
    HANDLE h = real_CreateNamedPipeW(name, openMode, pipeMode, maxInst, outBuf, inBuf, timeout, sa);
    log_line("CreateNamedPipeW %S open=%lx pipe=%lx max=%lu out=%lu in=%lu -> %p err=%lu",
             name, openMode, pipeMode, (unsigned long)maxInst,
             (unsigned long)outBuf, (unsigned long)inBuf, h, GetLastError());
    if (h != INVALID_HANDLE_VALUE) pipe_register(h, name);
    return h;
}
static BOOL WINAPI hook_ConnectNamedPipe(HANDLE h, LPOVERLAPPED ovl)
{
    BOOL r = real_ConnectNamedPipe(h, ovl);
    DWORD e = GetLastError();
    const WCHAR *n = pipe_lookup(h);
    if (n) log_line("ConnectNamedPipe %S ovl=%p -> %d err=%lu", n, ovl, r, e);
    else log_line("ConnectNamedPipe h=%p ovl=%p -> %d err=%lu", h, ovl, r, e);
    SetLastError(e);
    return r;
}
static BOOL WINAPI hook_DisconnectNamedPipe(HANDLE h)
{
    BOOL r = real_DisconnectNamedPipe(h);
    const WCHAR *n = pipe_lookup(h);
    if (n) log_line("DisconnectNamedPipe %S -> %d err=%lu", n, r, GetLastError());
    return r;
}
static BOOL WINAPI hook_WaitNamedPipeW(LPCWSTR name, DWORD timeout)
{
    BOOL r = real_WaitNamedPipeW(name, timeout);
    DWORD e = GetLastError();
    log_line("WaitNamedPipeW %S timeout=%lu -> %d err=%lu", name, (unsigned long)timeout, r, e);
    SetLastError(e);
    return r;
}
static BOOL WINAPI hook_TransactNamedPipe(HANDLE h, LPVOID in, DWORD inLen, LPVOID out, DWORD outLen, LPDWORD done, LPOVERLAPPED ovl)
{
    BOOL r = real_TransactNamedPipe(h, in, inLen, out, outLen, done, ovl);
    DWORD e = GetLastError();
    const WCHAR *n = pipe_lookup(h);
    log_line("TransactNamedPipe %S in=%lu out=%lu ovl=%p -> %d err=%lu done=%lu",
             n ? n : L"?", (unsigned long)inLen, (unsigned long)outLen, ovl, r, e,
             (unsigned long)(done ? *done : 0));
    SetLastError(e);
    return r;
}
static BOOL WINAPI hook_PeekNamedPipe(HANDLE h, LPVOID buf, DWORD len, LPDWORD done, LPDWORD avail, LPDWORD left)
{
    BOOL r = real_PeekNamedPipe(h, buf, len, done, avail, left);
    const WCHAR *n = pipe_lookup(h);
    if (n) log_line("PeekNamedPipe %S -> %d avail=%lu", n, r,
                    (unsigned long)(avail ? *avail : 0));
    return r;
}
static BOOL WINAPI hook_SetNamedPipeHandleState(HANDLE h, LPDWORD mode, LPDWORD coll, LPDWORD data)
{
    BOOL r = real_SetNamedPipeHandleState(h, mode, coll, data);
    const WCHAR *n = pipe_lookup(h);
    if (n) log_line("SetNamedPipeHandleState %S mode=%lx -> %d err=%lu",
                    n, (unsigned long)(mode ? *mode : 0), r, GetLastError());
    return r;
}
static BOOL WINAPI hook_ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD done, LPOVERLAPPED ovl)
{
    const WCHAR *n = pipe_lookup(h);
    BOOL r;
    DWORD e;
    if (!n) return real_ReadFile(h, buf, len, done, ovl);
    r = real_ReadFile(h, buf, len, done, ovl);
    e = GetLastError();
    if (r && !ovl && done && *done) {
        char hex[129], txt[65];
        fmt_hex64(buf, *done, hex, txt);
        log_line("pipeRead %S req=%lu -> %d done=%lu | %s | %s",
                 n, (unsigned long)len, r, (unsigned long)*done, hex, txt);
    } else {
        log_line("pipeRead %S req=%lu ovl=%p -> %d err=%lu done=%lu",
                 n, (unsigned long)len, ovl, r, e,
                 (unsigned long)(done && !ovl ? *done : 0));
    }
    SetLastError(e);
    return r;
}
static BOOL WINAPI hook_WriteFile(HANDLE h, LPCVOID buf, DWORD len, LPDWORD done, LPOVERLAPPED ovl)
{
    const WCHAR *n = pipe_lookup(h);
    BOOL r;
    DWORD e;
    char hex[129], txt[65];
    if (!n) return real_WriteFile(h, buf, len, done, ovl);
    hex[0] = 0; txt[0] = 0;
    if (!ovl) fmt_hex64(buf, len, hex, txt);
    r = real_WriteFile(h, buf, len, done, ovl);
    e = GetLastError();
    log_line("pipeWrite %S len=%lu ovl=%p -> %d err=%lu done=%lu | %s | %s",
             n, (unsigned long)len, ovl, r, e,
             (unsigned long)(done && !ovl ? *done : 0), hex, txt);
    SetLastError(e);
    return r;
}
static BOOL WINAPI hook_GetOverlappedResult(HANDLE h, LPOVERLAPPED ovl, LPDWORD done, BOOL wait)
{
    BOOL r = real_GetOverlappedResult(h, ovl, done, wait);
    DWORD e = GetLastError();
    const WCHAR *n = pipe_lookup(h);
    if (n) log_line("GetOverlappedResult %S wait=%d -> %d err=%lu done=%lu",
                    n, wait, r, e, (unsigned long)(done ? *done : 0));
    SetLastError(e);
    return r;
}
static BOOL WINAPI hook_FlushFileBuffers(HANDLE h)
{
    const WCHAR *n = pipe_lookup(h);
    BOOL r = real_FlushFileBuffers(h);
    if (n) log_line("FlushFileBuffers %S -> %d err=%lu", n, r, GetLastError());
    return r;
}
static BOOL WINAPI hook_CloseHandle(HANDLE h)
{
    name_unregister(h);
    pipe_unregister(h);
    return real_CloseHandle(h);
}

/* ---- 通用 IAT 改写 ---- */
static int patch_iat(const char *dll_name, const char *func_name, void *replacement, void **saved)
{
    HMODULE base = GetModuleHandleA(NULL);
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_IMPORT_DESCRIPTOR desc;

    if (!base) return 0;
    dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (PIMAGE_NT_HEADERS)((BYTE *)base + dos->e_lfanew);
    if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) return 0;

    desc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE *)base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    for (; desc->Name; desc++) {
        const char *name = (const char *)((BYTE *)base + desc->Name);
        PIMAGE_THUNK_DATA tname, tiat;
        if (_stricmp(name, dll_name) != 0) continue;

        tname = (PIMAGE_THUNK_DATA)((BYTE *)base +
            (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        tiat  = (PIMAGE_THUNK_DATA)((BYTE *)base + desc->FirstThunk);

        for (; tname->u1.AddressOfData; tname++, tiat++) {
            PIMAGE_IMPORT_BY_NAME ibn;
            DWORD old;
            if (tname->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            ibn = (PIMAGE_IMPORT_BY_NAME)((BYTE *)base + tname->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func_name) != 0) continue;

            *saved = (void *)tiat->u1.Function;
            if (!VirtualProtect(&tiat->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
                return 0;
            tiat->u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&tiat->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

/* KERNEL32 与 KERNELBASE 双名尝试（wine 下导入名可能任一） */
static int patch_kb(const char *func, void *hookfn, void **saved)
{
    void *s = NULL;
    if (patch_iat("KERNEL32.dll", func, hookfn, &s)) { *saved = s; return 1; }
    if (patch_iat("KERNELBASE.dll", func, hookfn, &s)) { *saved = s; return 1; }
    return 0;
}

BOOL ipcdump_attach(void)
{
    int n = 0;
    InitializeCriticalSection(&log_cs);
    InitializeCriticalSection(&ovl_cs);
    /* v4：FILE_APPEND_DATA = 原子追加，多进程写同一文件不再交错损坏 */
    log_file = CreateFileA("C:\\FGOA\\logs\\ipc-dump.log", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE) {
        /* 回退：写到主模块同目录（Windows 对照组没有 C:\FGOA） */
        char dir[MAX_PATH], *sep;
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        sep = strrchr(dir, '\\');
        if (sep) { sep[1] = 0; strcat(dir, "ipc-dump.log"); }
        log_file = CreateFileA(dir, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    n += patch_iat("CRYPT32.dll", "CryptProtectMemory", hook_protect, (void **)&real_protect);
    n += patch_iat("CRYPT32.dll", "CryptUnprotectMemory", hook_unprotect, (void **)&real_unprotect);
    n += patch_kb("CreateFileMappingW", hook_CreateFileMappingW, (void **)&real_CreateFileMappingW);
    n += patch_kb("OpenFileMappingW", hook_OpenFileMappingW, (void **)&real_OpenFileMappingW);
    n += patch_kb("CreateEventW", hook_CreateEventW, (void **)&real_CreateEventW);
    n += patch_kb("OpenEventW", hook_OpenEventW, (void **)&real_OpenEventW);
    n += patch_kb("CreateMutexW", hook_CreateMutexW, (void **)&real_CreateMutexW);
    n += patch_kb("OpenMutexW", hook_OpenMutexW, (void **)&real_OpenMutexW);
    n += patch_kb("WaitForSingleObject", hook_WaitForSingleObject, (void **)&real_WaitForSingleObject);
    n += patch_kb("WaitForSingleObjectEx", hook_WaitForSingleObjectEx, (void **)&real_WaitForSingleObjectEx);
    n += patch_kb("WaitForMultipleObjects", hook_WaitForMultipleObjects, (void **)&real_WaitForMultipleObjects);
    n += patch_kb("SetEvent", hook_SetEvent, (void **)&real_SetEvent);
    n += patch_kb("MapViewOfFile", hook_MapViewOfFile, (void **)&real_MapViewOfFile);
    n += patch_kb("ReleaseMutex", hook_ReleaseMutex, (void **)&real_ReleaseMutex);
    /* v4 管道钩子 */
    n += patch_kb("CreateFileW", hook_CreateFileW, (void **)&real_CreateFileW);
    n += patch_kb("CreateFileA", hook_CreateFileA, (void **)&real_CreateFileA);
    n += patch_kb("CreateNamedPipeW", hook_CreateNamedPipeW, (void **)&real_CreateNamedPipeW);
    n += patch_kb("ConnectNamedPipe", hook_ConnectNamedPipe, (void **)&real_ConnectNamedPipe);
    n += patch_kb("DisconnectNamedPipe", hook_DisconnectNamedPipe, (void **)&real_DisconnectNamedPipe);
    n += patch_kb("WaitNamedPipeW", hook_WaitNamedPipeW, (void **)&real_WaitNamedPipeW);
    n += patch_kb("TransactNamedPipe", hook_TransactNamedPipe, (void **)&real_TransactNamedPipe);
    n += patch_kb("PeekNamedPipe", hook_PeekNamedPipe, (void **)&real_PeekNamedPipe);
    n += patch_kb("SetNamedPipeHandleState", hook_SetNamedPipeHandleState, (void **)&real_SetNamedPipeHandleState);
    n += patch_kb("ReadFile", hook_ReadFile, (void **)&real_ReadFile);
    n += patch_kb("WriteFile", hook_WriteFile, (void **)&real_WriteFile);
    n += patch_kb("GetOverlappedResult", hook_GetOverlappedResult, (void **)&real_GetOverlappedResult);
    n += patch_kb("FlushFileBuffers", hook_FlushFileBuffers, (void **)&real_FlushFileBuffers);
    n += patch_kb("CloseHandle", hook_CloseHandle, (void **)&real_CloseHandle);
    /* v5：ReadFileEx/WriteFileEx（amdipc 真实管道读写通道）+ alertable 泵点 */
    n += patch_kb("ReadFileEx", hook_ReadFileEx, (void **)&real_ReadFileEx);
    n += patch_kb("WriteFileEx", hook_WriteFileEx, (void **)&real_WriteFileEx);
    n += patch_kb("SleepEx", hook_SleepEx, (void **)&real_SleepEx);
    n += patch_kb("DeviceIoControl", hook_DeviceIoControl, (void **)&real_DeviceIoControl);
    /* v6：追踪 h=NULL 等待的来源 */
    n += patch_kb("CreateThread", hook_CreateThread, (void **)&real_CreateThread);
    n += patch_kb("OpenProcess", hook_OpenProcess, (void **)&real_OpenProcess);
    if (patch_iat("msvcrt.dll", "_beginthreadex", hook_beginthreadex, (void **)&real_beginthreadex) ||
        patch_iat("ucrtbase.dll", "_beginthreadex", hook_beginthreadex, (void **)&real_beginthreadex) ||
        patch_iat("api-ms-win-crt-runtime-l1-1-0.dll", "_beginthreadex", hook_beginthreadex, (void **)&real_beginthreadex))
        n++;
    /* v7：cngfix — 修复 soda bcrypt 缺 ECC secret 派生（BCryptDeriveKey/HASH
     * 在 ECDH secret 上 0xC0000002），amdipc ECK1 协商因此静默失败、Messenger
     * 状态机停在 state 2、putMention 被 [state]!=3 门死 = 4102 对称死锁根因。
     * 详见 cngfix.h。IPCDUMP_CNGFIX=0 可关。 */
    {
        char ev[8];
        if (!GetEnvironmentVariableA("IPCDUMP_CNGFIX", ev, sizeof(ev)) || ev[0] != '0')
            cngfix_install(patch_iat, log_line);
        else
            log_line("cngfix: disabled by IPCDUMP_CNGFIX=0");
    }
    log_line("attach v7: %d/31 IAT hooks installed", n);
    return n > 0;
}

#ifdef IPCDUMP_STANDALONE
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        ipcdump_attach();
    if (reason == DLL_PROCESS_DETACH) {
        if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    }
    return TRUE;
}
#endif
