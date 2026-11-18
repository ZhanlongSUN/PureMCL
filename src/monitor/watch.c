#include "watch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CHANGE_FILTER (FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_SIZE)
#define TAIL_BYTES 8192
#define DEBOUNCE_MS 350

typedef struct {
    HWND window;
    DWORD pid;
    bool seen, closed;
} GameWindow;

/* The out-of-context hook calls back on the thread that installed it. */
static GameWindow *active_window;

static BOOL CALLBACK find_window(HWND hwnd,LPARAM context) {
    GameWindow *game=(GameWindow *)context;
    DWORD owner=0;
    GetWindowThreadProcessId(hwnd,&owner);
    if (owner!=game->pid || !IsWindowVisible(hwnd) || GetAncestor(hwnd,GA_ROOT)!=hwnd ||
        GetWindow(hwnd,GW_OWNER)) return TRUE;
    wchar_t cls[64];
    if (!GetClassNameW(hwnd,cls,(int)_countof(cls))) return TRUE;
    if (wcscmp(cls,L"GLFW30") && wcscmp(cls,L"LWJGL") && wcscmp(cls,L"SDL_app")) {
        wchar_t title[128];
        if (!wcscmp(cls,L"#32770") || !GetWindowTextW(hwnd,title,(int)_countof(title)) ||
            !wcsstr(title,L"Minecraft")) return TRUE;
    }
    game->window=hwnd;
    game->seen=true;
    return FALSE;
}

static void CALLBACK window_event(HWINEVENTHOOK hook,DWORD event,HWND hwnd,LONG object,LONG child,
                                  DWORD thread,DWORD time) {
    (void)hook; (void)thread; (void)time;
    if (!active_window || !hwnd || object!=OBJID_WINDOW || child!=CHILDID_SELF) return;
    if (event==EVENT_OBJECT_SHOW) find_window(hwnd,(LPARAM)active_window);
    else if (event==EVENT_OBJECT_DESTROY && hwnd==active_window->window) {
        active_window->window=NULL;
        active_window->closed=true;
    }
}

static bool path_join(wchar_t out[WATCH_PATH],const wchar_t *dir,const wchar_t *name) {
    size_t n=wcslen(dir);
    if (!n || n+wcslen(name)+2>WATCH_PATH) return false;
    if (out!=dir) wcscpy_s(out,WATCH_PATH,dir);
    if (out[n-1]!=L'\\') out[n++]=L'\\';
    wcscpy_s(out+n,WATCH_PATH-n,name);
    return true;
}

static bool directory_ok(const wchar_t *path) {
    DWORD attr=GetFileAttributesW(path);
    return attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_DIRECTORY) &&
        !(attr&FILE_ATTRIBUTE_REPARSE_POINT);
}

static HANDLE regular_file(const wchar_t *path,FILETIME start,BY_HANDLE_FILE_INFORMATION *info) {
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (f!=INVALID_HANDLE_VALUE && (!GetFileInformationByHandle(f,info) ||
        (info->dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
        CompareFileTime(&info->ftLastWriteTime,&start)<0)) {
        CloseHandle(f);
        return INVALID_HANDLE_VALUE;
    }
    return f;
}

typedef struct {
    BY_HANDLE_FILE_INFORMATION info;
    ULONGLONG offset;
    char suffix[96];
    size_t suffix_len;
    bool fatal;
} LogTail;

static void read_tail(const wchar_t *dir,const wchar_t *name,FILETIME start,LogTail *tail,bool final) {
    wchar_t path[WATCH_PATH];
    if (!directory_ok(dir) || !path_join(path,dir,name)) return;
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE f=regular_file(path,start,&info);
    if (f==INVALID_HANDLE_VALUE) return;
    ULONGLONG length=((ULONGLONG)info.nFileSizeHigh<<32)|info.nFileSizeLow;
    if (info.dwVolumeSerialNumber!=tail->info.dwVolumeSerialNumber ||
        info.nFileIndexHigh!=tail->info.nFileIndexHigh || info.nFileIndexLow!=tail->info.nFileIndexLow ||
        length<tail->offset || (length==tail->offset && CompareFileTime(&info.ftLastWriteTime,&tail->info.ftLastWriteTime))) {
        tail->offset=0;
        tail->suffix_len=0;
    }
    tail->info=info;

    ULONGLONG scan_limit=final?8*TAIL_BYTES:TAIL_BYTES;
    if (length-tail->offset>scan_limit) {
        tail->offset=length-scan_limit;
        tail->suffix_len=0;
    }
    LARGE_INTEGER position={.QuadPart=(LONGLONG)tail->offset};
    if (length>tail->offset && SetFilePointerEx(f,position,NULL,FILE_BEGIN)) {
        char text[TAIL_BYTES+sizeof(tail->suffix)+1];
        while (tail->offset<length) {
            ULONGLONG left=length-tail->offset;
            DWORD got=0, want=(DWORD)(left<TAIL_BYTES?left:TAIL_BYTES);
            memcpy(text,tail->suffix,tail->suffix_len);
            if (!ReadFile(f,text+tail->suffix_len,want,&got,NULL) || !got) break;
            size_t n=tail->suffix_len+got;
            text[n]=0;
            if (strstr(text,"---- Minecraft Crash Report ----") ||
                strstr(text,"FATAL]: Unreported exception thrown!") ||
                strstr(text,"# A fatal error has been detected by the Java Runtime Environment")) tail->fatal=true;
            tail->suffix_len=n<sizeof(tail->suffix)?n:sizeof(tail->suffix);
            memcpy(tail->suffix,text+n-tail->suffix_len,tail->suffix_len);
            tail->offset+=got;
        }
    }
    CloseHandle(f);
}

static bool consider_log(const wchar_t *dir,const wchar_t *name,FILETIME start,
                         wchar_t newest[WATCH_PATH],FILETIME *time) {
    wchar_t path[WATCH_PATH];
    if (!directory_ok(dir) || !path_join(path,dir,name)) return false;
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE f=regular_file(path,start,&info);
    if (f==INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    if (!newest[0] || CompareFileTime(&info.ftLastWriteTime,time)>0) {
        *time=info.ftLastWriteTime;
        wcscpy_s(newest,WATCH_PATH,path);
    }
    return true;
}

static bool crash_reports(const wchar_t *game,FILETIME start,wchar_t newest[WATCH_PATH],FILETIME *time) {
    wchar_t dir[WATCH_PATH], pattern[WATCH_PATH];
    if (!path_join(dir,game,L"crash-reports") || !directory_ok(dir) ||
        !path_join(pattern,dir,L"crash-*-client.txt")) return false;
    WIN32_FIND_DATAW data;
    HANDLE find=FindFirstFileExW(pattern,FindExInfoBasic,&data,FindExSearchNameMatch,NULL,0);
    if (find==INVALID_HANDLE_VALUE) return false;
    bool report=false;
    do {
        if (CompareFileTime(&data.ftLastWriteTime,&start)>=0 &&
            consider_log(dir,data.cFileName,start,newest,time)) report=true;
    } while (FindNextFileW(find,&data));
    FindClose(find);
    return report;
}

static HANDLE watch_dir(const wchar_t *dir) {
    return directory_ok(dir)?FindFirstChangeNotificationW(dir,FALSE,CHANGE_FILTER):INVALID_HANDLE_VALUE;
}

static void rearm(HANDLE *watch) {
    if (!FindNextChangeNotification(*watch)) {
        FindCloseChangeNotification(*watch);
        *watch=INVALID_HANDLE_VALUE;
    }
}

static bool monitor(HANDLE game,const wchar_t *dir,const wchar_t *logs,FILETIME start,GameWindow *window,
                    LogTail *latest,LogTail *legacy) {
    HANDLE changes=watch_dir(dir), log_changes=watch_dir(logs);
    bool pending=true, exited=false;
    ULONGLONG next_read=0;
    for (;;) {
        ULONGLONG now=GetTickCount64();
        if (window->closed) { window->closed=false; pending=true; }
        if (pending && now>=next_read) {
            read_tail(logs,L"latest.log",start,latest,false);
            read_tail(dir,L"output-client.log",start,legacy,false);
            pending=false;
            next_read=GetTickCount64()+DEBOUNCE_MS;
            now=GetTickCount64();
        }
        HANDLE waits[3]={game}; DWORD count=1, dir_slot=MAXDWORD, log_slot=MAXDWORD;
        if (changes!=INVALID_HANDLE_VALUE) { dir_slot=count; waits[count++]=changes; }
        if (log_changes!=INVALID_HANDLE_VALUE) { log_slot=count; waits[count++]=log_changes; }
        DWORD timeout=pending?(DWORD)(next_read-now):INFINITE;
        DWORD wake=MsgWaitForMultipleObjectsEx(count,waits,timeout,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if (wake==WAIT_OBJECT_0) { exited=true; break; }
        if (wake==WAIT_FAILED) break;
        if (wake==WAIT_TIMEOUT) continue;
        if (wake-WAIT_OBJECT_0==dir_slot) {
            rearm(&changes);
            if (log_changes!=INVALID_HANDLE_VALUE) FindCloseChangeNotification(log_changes);
            log_changes=watch_dir(logs);
            pending=true;
        } else if (wake-WAIT_OBJECT_0==log_slot) {
            rearm(&log_changes);
            pending=true;
        } else if (wake==WAIT_OBJECT_0+count) {
            MSG msg;
            while (PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) DispatchMessageW(&msg);
        }
    }
    if (log_changes!=INVALID_HANDLE_VALUE) FindCloseChangeNotification(log_changes);
    if (changes!=INVALID_HANDLE_VALUE) FindCloseChangeNotification(changes);
    return exited;
}

bool nmc_watch_game(HANDLE game,const wchar_t *directory,NmcCrash *crash) {
    *crash=(NmcCrash){0};
    FILETIME start, end, kernel, user;
    wchar_t logs[WATCH_PATH];
    GameWindow window={.pid=GetProcessId(game)};
    if (!window.pid || !GetProcessTimes(game,&start,&end,&kernel,&user) ||
        !path_join(logs,directory,L"logs")) return false;

    active_window=&window;
    HWINEVENTHOOK hook=SetWinEventHook(EVENT_OBJECT_DESTROY,EVENT_OBJECT_SHOW,NULL,window_event,
                                      window.pid,0,WINEVENT_OUTOFCONTEXT);
    EnumWindows(find_window,(LPARAM)&window);
    LogTail latest={0}, legacy={0};
    bool exited=monitor(game,directory,logs,start,&window,&latest,&legacy);
    if (hook) UnhookWinEvent(hook);
    active_window=NULL;
    if (!exited || !GetExitCodeProcess(game,&crash->exit_code)) return false;
    crash->window_seen=window.seen;
    read_tail(logs,L"latest.log",start,&latest,true);
    read_tail(directory,L"output-client.log",start,&legacy,true);

    wchar_t name[64];
    swprintf(name,_countof(name),L"hs_err_pid%lu.log",window.pid);
    FILETIME last={0};
    bool jvm_crash=consider_log(directory,name,start,crash->log,&last);
    bool report=crash_reports(directory,start,crash->log,&last);
    if (!crash->exit_code && !jvm_crash && !report && !latest.fatal && !legacy.fatal) return false;
    consider_log(logs,L"latest.log",start,crash->log,&last);
    consider_log(logs,L"debug.log",start,crash->log,&last);
    consider_log(directory,L"output-client.log",start,crash->log,&last);
    consider_log(directory,L"ForgeModLoader-client-0.log",start,crash->log,&last);
    return true;
}
