/* pipesmoke.exe — ipcdump v4 本地冒烟：走一遍管道/命名对象 API，人工核对日志 */
#include <windows.h>
#include <stdio.h>

static HANDLE evt;
static DWORD WINAPI setter(void *p)
{
    (void)p;
    Sleep(200);
    SetEvent(evt);
    return 0;
}

static char ex_rbuf[64];
static VOID CALLBACK ex_cb(DWORD err, DWORD done, LPOVERLAPPED ovl)
{
    (void)ovl;
    printf("ex_cb err=%lu done=%lu data=%.16s\n", err, done, ex_rbuf);
}

int main(void)
{
    HANDLE hpipe, hcli, hmtx, hmap, t;
    DWORD mode, done;
    char wbuf[32] = "ping-0123456789";
    char rbuf[64];
    LPVOID v;
    BOOL r;

    HMODULE m = LoadLibraryA("ipcdump_smoke.dll");
    printf("load=%p err=%lu\n", m, GetLastError());
    if (!m) return 1;

    /* 命名管道：server + client 同进程回环 */
    hpipe = CreateNamedPipeW(L"\\\\.\\pipe\\smoke_amdmsg_SC",
        PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 4096, 4096, 0, NULL);
    printf("pipe=%p err=%lu\n", hpipe, GetLastError());

    hcli = CreateFileW(L"\\\\.\\pipe\\smoke_amdmsg_SC",
        GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    printf("cli=%p err=%lu\n", hcli, GetLastError());

    r = WriteFile(hcli, wbuf, 16, &done, NULL);
    printf("write=%d done=%lu\n", r, done);
    r = ReadFile(hpipe, rbuf, sizeof(rbuf), &done, NULL);
    printf("read=%d done=%lu data=%.16s\n", r, done, rbuf);

    /* WaitNamedPipe 对不存在的管道（应 err=2） */
    r = WaitNamedPipeW(L"\\\\.\\pipe\\smoke_no_such", 100);
    printf("waitnamed=%d err=%lu\n", r, GetLastError());

    /* ReadFileEx/WriteFileEx + alertable 等待（v5 蹦床验证）
     * 必须用 FILE_FLAG_OVERLAPPED 管道（与 amdipc 一致）——非 overlapped
     * 句柄上 wine 的 ReadFileEx 会退化成同步等待数据而挂住 */
    {
        OVERLAPPED o1, o2;
        HANDLE hp2 = CreateNamedPipeW(L"\\\\.\\pipe\\smoke_amdmsg_EX",
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 4096, 4096, 0, NULL);
        HANDLE hc2 = CreateFileW(L"\\\\.\\pipe\\smoke_amdmsg_EX",
            GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, NULL);
        printf("pipe2=%p cli2=%p err=%lu\n", hp2, hc2, GetLastError()); fflush(stdout);
        memset(&o1, 0, sizeof(o1));
        memset(&o2, 0, sizeof(o2));
        r = ReadFileEx(hp2, ex_rbuf, sizeof(ex_rbuf), &o1, ex_cb);
        printf("readex=%d err=%lu\n", r, GetLastError()); fflush(stdout);
        r = WriteFileEx(hc2, wbuf, 16, &o2, ex_cb);
        printf("writeex=%d err=%lu\n", r, GetLastError()); fflush(stdout);
        SleepEx(2000, TRUE);   /* alertable：完成例程应在此触发 */
        printf("after sleepex\n"); fflush(stdout);
        CloseHandle(hc2);
        CloseHandle(hp2);
    }

    /* 命名互斥 + 无名事件 INFINITE 等待 */
    hmtx = CreateMutexW(NULL, FALSE, L"smoke_daemon_test_123");
    WaitForSingleObject(hmtx, 1000);
    ReleaseMutex(hmtx);

    evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    t = CreateThread(NULL, 0, setter, NULL, 0, NULL);
    WaitForSingleObject(evt, INFINITE);   /* 应被 UNNAMED 防洪逻辑记录 */
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);

    /* 命名 shm + 视图 */
    hmap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 256, L"smoke_shm_test");
    v = MapViewOfFile(hmap, FILE_MAP_ALL_ACCESS, 0, 0, 256);
    if (v) memset(v, 0x41, 256);
    WaitForSingleObject(hmtx, 1000);   /* 非 daemon 名不触发 shmdump */
    ReleaseMutex(hmtx);

    CloseHandle(hcli);
    CloseHandle(hpipe);
    CloseHandle(hmap);
    CloseHandle(hmtx);
    CloseHandle(evt);
    printf("done\n");
    return 0;
}
