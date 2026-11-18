#include "internal.h"
#include "monitor/watch.h"
#include <psapi.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return false; } } while (0)

static bool temp_dir(wchar_t dir[NMC_PATH]) {
    wchar_t base[NMC_PATH], name[64];
    DWORD len=GetTempPathW(NMC_PATH,base);
    if (!len || len>=NMC_PATH) return false;
    swprintf(name,NMC_ARRAY(name),L"PureMCL-watch-%lu-%llu",GetCurrentProcessId(),GetTickCount64());
    return nmc_join(dir,base,name) && CreateDirectoryW(dir,NULL);
}

static int fixture(const wchar_t *mode) {
    WNDCLASSW cls={.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(NULL),
                   .lpszClassName=!wcscmp(mode,L"log")?L"SDL_app":L"GLFW30"};
    if (!RegisterClassW(&cls)) return 10;
    HWND game=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"Minecraft fixture",
                             WS_POPUP,-1000,-1000,2,2,NULL,NULL,cls.hInstance,NULL);
    if (!game) return 11;
    ShowWindow(game,SW_SHOWNOACTIVATE);
    if (!wcscmp(mode,L"log")) {
        const char fatal[]="[Render thread/FATAL]: Unreported exception thrown!\n";
        wchar_t dir[NMC_PATH], path[NMC_PATH];
        DWORD len=GetCurrentDirectoryW(NMC_PATH,dir);
        if (!len || len>=NMC_PATH || !nmc_join(path,dir,L"logs\\latest.log") ||
            nmc_atomic(path,fatal,sizeof(fatal)-1).code) return 12;
    }
    Sleep(!wcscmp(mode,L"idle")?1600:500);
    DestroyWindow(game);
    return !wcscmp(mode,L"nonzero")?7:0;
}

static bool launch(const wchar_t *dir,const wchar_t *mode,HANDLE *game) {
    wchar_t exe[NMC_PATH];
    DWORD len=GetModuleFileNameW(NULL,exe,NMC_PATH);
    REQUIRE(len && len<NMC_PATH);
    NmcArgs args={0};
    bool ok=nmc_args_add(&args,L"--fixture") && nmc_args_add(&args,mode) &&
        !nmc_spawn(exe,dir,&args,NULL,NULL,game).code;
    nmc_args_free(&args);
    return ok;
}

static bool watch_check(void) {
    wchar_t dir[NMC_PATH], logs[NMC_PATH], log[NMC_PATH], report[NMC_PATH];
    REQUIRE(temp_dir(dir) && nmc_join(logs,dir,L"logs") && nmc_join(log,logs,L"latest.log") &&
            nmc_join(report,dir,L"crash-reports\\crash-old-client.txt"));
    const char stale[]="---- Minecraft Crash Report ----\n";
    REQUIRE(!nmc_atomic(log,stale,sizeof(stale)-1).code && !nmc_atomic(report,stale,sizeof(stale)-1).code);
    FILETIME old={.dwHighDateTime=0x01C00000};
    const wchar_t *const files[]={log,report};
    for (size_t i=0;i<NMC_ARRAY(files);++i) {
        HANDLE stale_file=CreateFileW(files[i],FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
        REQUIRE(stale_file!=INVALID_HANDLE_VALUE);
        BOOL stamped=SetFileTime(stale_file,NULL,NULL,&old);
        CloseHandle(stale_file);
        REQUIRE(stamped);
    }

    HANDLE game;
    NmcCrash crash;
    REQUIRE(launch(dir,L"normal",&game));
    REQUIRE(!nmc_watch_game(game,dir,&crash) && crash.window_seen && crash.exit_code==0);
    CloseHandle(game);
    REQUIRE(launch(dir,L"nonzero",&game));
    REQUIRE(nmc_watch_game(game,dir,&crash) && crash.window_seen && crash.exit_code==7 && !crash.log[0]);
    CloseHandle(game);
    REQUIRE(launch(dir,L"log",&game));
    REQUIRE(nmc_watch_game(game,dir,&crash) && crash.window_seen && crash.exit_code==0 && !wcscmp(crash.log,log));
    CloseHandle(game);

    HANDLE watcher;
    REQUIRE(launch(dir,L"idle",&game));
    REQUIRE(!nmc_start_watch(game,dir,&watcher).code && WaitForSingleObject(watcher,0)==WAIT_TIMEOUT);
    CloseHandle(game); /* Closing the launcher copy must not detach the monitor. */
    Sleep(450);
    PROCESS_MEMORY_COUNTERS_EX memory={.cb=sizeof(memory)};
    REQUIRE(K32GetProcessMemoryInfo(watcher,(PROCESS_MEMORY_COUNTERS *)&memory,sizeof(memory)));
    REQUIRE(WaitForSingleObject(watcher,6000)==WAIT_OBJECT_0);
    DWORD status=1;
    FILETIME start,end,kernel,user;
    REQUIRE(GetExitCodeProcess(watcher,&status) && status==0 && GetProcessTimes(watcher,&start,&end,&kernel,&user));
    double cpu_ms=(double)((((uint64_t)kernel.dwHighDateTime<<32)|kernel.dwLowDateTime)+
                          (((uint64_t)user.dwHighDateTime<<32)|user.dwLowDateTime))/10000.0;
    printf("Monitor fixture: %.2f MiB private commit, %.2f MiB working set, %.1f ms CPU during idle fixture\n",
           (double)memory.PrivateUsage/1048576.0,(double)memory.WorkingSetSize/1048576.0,cpu_ms);
    CloseHandle(watcher);
    REQUIRE(DeleteFileW(log) && DeleteFileW(report));
    wchar_t reports[NMC_PATH];
    REQUIRE(nmc_join(reports,dir,L"crash-reports") && RemoveDirectoryW(reports) &&
            RemoveDirectoryW(logs) && RemoveDirectoryW(dir));
    return true;
}

int wmain(int argc,wchar_t **argv) {
    if (argc==3 && !wcscmp(argv[1],L"--fixture")) return fixture(argv[2]);
    return watch_check()?0:1;
}
