#include "watch.h"
#include <commctrl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

static bool open_log(const wchar_t *path) {
    wchar_t parent[WATCH_PATH];
    if (wcscpy_s(parent,_countof(parent),path)) return false;
    wchar_t *slash=wcsrchr(parent,L'\\');
    if (!slash) return false;
    *slash=0;
    DWORD parent_attr=GetFileAttributesW(parent);
    if (parent_attr==INVALID_FILE_ATTRIBUTES || !(parent_attr&FILE_ATTRIBUTE_DIRECTORY) ||
        (parent_attr&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    DWORD attr=GetFileAttributesW(path);
    if (attr==INVALID_FILE_ATTRIBUTES || (attr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    wchar_t exe[MAX_PATH], command[MAX_PATH+WATCH_PATH+8];
    UINT count=GetSystemDirectoryW(exe,_countof(exe));
    if (!count || count+_countof(L"\\notepad.exe")>_countof(exe)) return false;
    wcscat_s(exe,_countof(exe),L"\\notepad.exe");
    if (swprintf(command,_countof(command),L"\"%ls\" \"%ls\"",exe,path)<0) return false;
    STARTUPINFOW startup={.cb=sizeof(startup)};
    PROCESS_INFORMATION process={0};
    if (!CreateProcessW(exe,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

static void crash_dialog(const NmcCrash *crash) {
    const wchar_t *title=crash->window_seen?L"Minecraft 异常退出":L"Minecraft 启动时异常退出";
    wchar_t content[128];
    swprintf(content,_countof(content),L"退出代码：0x%08lX%ls",crash->exit_code,
        crash->log[0]?L"":L"\n本次运行没有找到可打开的日志。");
    const TASKDIALOG_BUTTON open={.nButtonID=100,.pszButtonText=L"打开最新日志"};
    TASKDIALOGCONFIG dialog={.cbSize=sizeof(dialog),.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION,
        .dwCommonButtons=TDCBF_CLOSE_BUTTON,.pszWindowTitle=L"PureMCL",.pszMainIcon=TD_ERROR_ICON,
        .pszMainInstruction=title,.pszContent=content,.cButtons=crash->log[0]?1u:0u,
        .pButtons=&open,.nDefaultButton=IDCLOSE};
    int chosen=0;
    HRESULT hr=TaskDialogIndirect(&dialog,&chosen,NULL,NULL);
    if (FAILED(hr)) {
        UINT flags=MB_SETFOREGROUND|MB_ICONERROR;
        if (crash->log[0]) {
            wcscat_s(content,_countof(content),L"\n要打开最新日志吗？");
            if (MessageBoxW(NULL,content,title,flags|MB_YESNO|MB_DEFBUTTON2)==IDYES) chosen=100;
        } else MessageBoxW(NULL,content,title,flags|MB_OK);
    }
    if (chosen==100 && !open_log(crash->log))
        MessageBoxW(NULL,L"无法打开本次运行的最新日志。",L"PureMCL",MB_OK|MB_ICONWARNING|MB_SETFOREGROUND);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show) {
    (void)instance; (void)previous; (void)show;
    if (*command<L'0' || *command>L'9') return 1;
    wchar_t *end;
    unsigned long long value=_wcstoui64(command,&end,10);
    if (!value || value==ULLONG_MAX || *end) return 1;
    HANDLE game=(HANDLE)(UINT_PTR)value;
    wchar_t directory[WATCH_PATH];
    DWORD count=GetCurrentDirectoryW(_countof(directory),directory);
    if (!count || count>=_countof(directory)) { CloseHandle(game); return 1; }
    SetHandleInformation(game,HANDLE_FLAG_INHERIT,0);
    bool background=SetPriorityClass(GetCurrentProcess(),PROCESS_MODE_BACKGROUND_BEGIN)!=0;
    NmcCrash crash;
    bool crashed=nmc_watch_game(game,directory,&crash);
    CloseHandle(game);
    if (background) SetPriorityClass(GetCurrentProcess(),PROCESS_MODE_BACKGROUND_END);
    if (crashed) crash_dialog(&crash);
    return 0;
}
