/*
 * wlanapi.dll shim — amdaemon.exe 侧 ipcdump 注入载体
 * 原理：amdaemon.exe 静态导入 wlanapi.dll 的 8 个函数（无线网卡自检，实机/ wine 下
 *       都是摆设——Deck 日志里 wlanapi 零调用）。wine 的 DLL 搜索序会让应用目录下的
 *       原生 wlanapi.dll 优先于内建加载 → 本 shim 随之进入 amdaemon 进程，
 *       DllMain 里跑 ipcdump_attach() 给 amdaemon.exe 的 IAT 装钩。
 * 8 个导出全是错误返回桩（日志证实这些函数从未被真正调用）。
 * 部署：放到 bottle drive_c/FGOA/App/am/（amdaemon.exe 同目录）。不改任何现有文件。
 */
#include <windows.h>

BOOL ipcdump_attach(void);   /* ipcdump.c */

/* 镜像 wine 内建 wlanapi 的实测行为：OpenHandle 成功(h=1,nv=2)、
 * EnumInterfaces 成功但空列表、CloseHandle 成功（wine 11 实测值） */
static HANDLE fake_handle = (HANDLE)1;

DWORD WINAPI WlanOpenHandle(DWORD v, PVOID r, PDWORD nv, PHANDLE ph)
{ (void)r; if (nv) *nv = v; if (ph) *ph = fake_handle; return 0; }
DWORD WINAPI WlanCloseHandle(HANDLE h, PVOID r)
{ (void)h; (void)r; return 0; }
DWORD WINAPI WlanEnumInterfaces(HANDLE h, PVOID r, PVOID *pp)
{
    /* 返回空 WLAN_INTERFACE_INFO_LIST（dwNumberOfItems=0），调用方会 WlanFreeMemory */
    (void)h; (void)r;
    if (pp) {
        DWORD *buf = HeapAlloc(GetProcessHeap(), 0, 16);
        if (buf) { buf[0] = 0; buf[1] = 0; }   /* dwNumberOfItems=0, dwIndex=0 */
        *pp = buf;
    }
    return 0;
}
DWORD WINAPI WlanQueryInterface(HANDLE h, PVOID p, int t, PVOID r, PVOID a, PVOID b)
{ (void)h; (void)p; (void)t; (void)r; (void)a; (void)b; return ERROR_NOT_FOUND; }
DWORD WINAPI WlanDisconnect(HANDLE h, PVOID p, PVOID r)
{ (void)h; (void)p; (void)r; return ERROR_NOT_FOUND; }
DWORD WINAPI WlanSetProfile(HANDLE h, PVOID p, DWORD f, PVOID x, PVOID y, BOOL z, PVOID r, PDWORD e)
{ (void)h; (void)p; (void)f; (void)x; (void)y; (void)z; (void)r; (void)e; return ERROR_NOT_FOUND; }
DWORD WINAPI WlanConnect(HANDLE h, PVOID p, PVOID c, PVOID r)
{ (void)h; (void)p; (void)c; (void)r; return ERROR_NOT_FOUND; }
VOID WINAPI WlanFreeMemory(PVOID p)
{ if (p) HeapFree(GetProcessHeap(), 0, p); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        ipcdump_attach();
    return TRUE;
}
