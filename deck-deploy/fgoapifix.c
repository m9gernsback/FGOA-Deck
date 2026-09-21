/*
 * fgoapifix.dll — Steam Deck / Wine 补丁
 * 作用：ago.exe 静态导入了 user32.SetWindowFeedbackSetting，Wine 未实现该导出，
 *       IAT 槽被填成 "unimplemented function" 中止桩，游戏在触摸初始化时调用即死。
 *       本 DLL 在注入时（inject -k，游戏主线程恢复前）遍历主模块导入表，
 *       把该 IAT 槽改写为本 DLL 内的桩函数（返回 TRUE，触摸反馈设置纯属外观配置）。
 * 不修改 ago.exe 文件本身（fgohook 的版本校验不受影响），只做内存补丁。
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>

typedef int FEEDBACK_TYPE_T;

static BOOL WINAPI Stub_SetWindowFeedbackSetting(HWND hwnd, FEEDBACK_TYPE_T feedback,
                                                 DWORD dwFlags, UINT32 size,
                                                 const VOID *configuration)
{
    (void)hwnd; (void)feedback; (void)dwFlags; (void)size; (void)configuration;
    return TRUE;
}

static int patch_iat(const char *dll_name, const char *func_name, void *replacement)
{
    HMODULE base = GetModuleHandleA(NULL);   /* 主模块 = ago.exe */
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

            if (!VirtualProtect(&tiat->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
                return 0;
            tiat->u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&tiat->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        if (patch_iat("user32.dll", "SetWindowFeedbackSetting",
                      (void *)Stub_SetWindowFeedbackSetting)) {
            OutputDebugStringA("fgoapifix: SetWindowFeedbackSetting IAT patched (stub returns TRUE)\n");
        } else {
            OutputDebugStringA("fgoapifix: WARNING - import not found, patch skipped\n");
        }
    }
    return TRUE;
}
