#ifndef NMC_MONITOR_H
#define NMC_MONITOR_H

#include <windows.h>
#include <stdbool.h>

#define WATCH_PATH 4096

typedef struct {
    DWORD exit_code;
    bool window_seen;
    wchar_t log[WATCH_PATH];
} NmcCrash;

bool nmc_watch_game(HANDLE game,const wchar_t *directory,NmcCrash *crash);

#endif
