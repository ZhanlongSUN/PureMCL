#include "internal.h"
#include <miniz.h>

static bool quote(NmcWide *cmd,const wchar_t *s) {
    if (!nmc_wide_add(cmd,L"\"",1)) return false;
    while (*s) {
        size_t n=0;
        while (s[n]==L'\\') ++n;
        size_t emit=n;
        if (!s[n] || s[n]==L'"') emit=n*2+(s[n]==L'"');
        while (emit--) if (!nmc_wide_add(cmd,L"\\",1)) return false;
        s+=n;
        if (*s && !nmc_wide_add(cmd,s++,1)) return false;
    }
    return nmc_wide_add(cmd,L"\"",1);
}
NmcError nmc_command(const wchar_t *exe,const NmcArgs *args,NmcWide *out) {
    if (!quote(out,exe)) return ERR(NMC_MEMORY,L"Windows command");
    for (size_t i=0;i<args->n;++i)
        if (!nmc_wide_add(out,L" ",1) || !quote(out,args->v[i])) return ERR(NMC_MEMORY,L"Windows command");
    return OK;
}
typedef struct { DWORD pid; FILETIME created; } ProcessMark;
NmcError nmc_process_idle(const wchar_t *path) {
    NmcBytes b={0}; ProcessMark mark;
    NmcError e=nmc_read(path,sizeof(mark),&b);
    if (e.code==NMC_MISSING) return OK;
    if (e.code) return e;
    if (b.len!=sizeof(mark)) { free(b.data); return ERR(NMC_FILE,L"game process receipt"); }
    memcpy(&mark,b.data,sizeof(mark)); free(b.data);
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,mark.pid);
    if (!p) return GetLastError()==ERROR_INVALID_PARAMETER ? OK : SYS(NMC_PROCESS,L"inspect game process");
    FILETIME created,exit,kernel,user;
    DWORD wait=WaitForSingleObject(p,0);
    if (wait==WAIT_TIMEOUT) {
        if (!GetProcessTimes(p,&created,&exit,&kernel,&user)) e=SYS(NMC_PROCESS,L"read game process identity");
        else if (!CompareFileTime(&created,&mark.created)) e=ERR(NMC_BUSY,L"game instance is running");
    } else if (wait==WAIT_FAILED) e=SYS(NMC_PROCESS,L"wait for game status");
    CloseHandle(p); return e;
}
NmcError nmc_spawn(const wchar_t *exe,const wchar_t *dir,const NmcArgs *args,
                   const wchar_t *lock_path,unsigned long *pid) {
    NmcWide cmd={0}; STARTUPINFOW si={.cb=sizeof(si)}; PROCESS_INFORMATION pi={0};
    NmcError e=nmc_command(exe,args,&cmd);
    if (!e.code && cmd.len>=32767) e=ERR(NMC_UNSUPPORTED,L"command line exceeds Windows limit");
    if (!e.code && !CreateProcessW(exe,cmd.data,NULL,NULL,FALSE,CREATE_UNICODE_ENVIRONMENT|NORMAL_PRIORITY_CLASS|CREATE_NO_WINDOW|CREATE_SUSPENDED,
                                  NULL,dir,&si,&pi))
        e=SYS(NMC_PROCESS,L"CreateProcessW");
    if (!e.code) {
        if (lock_path) {
            ProcessMark mark={.pid=pi.dwProcessId}; FILETIME exit,kernel,user;
            if (!GetProcessTimes(pi.hProcess,&mark.created,&exit,&kernel,&user))
                e=SYS(NMC_PROCESS,L"game process creation time");
            else e=nmc_atomic(lock_path,&mark,sizeof(mark));
        }
        if (!e.code && ResumeThread(pi.hThread)==(DWORD)-1) e=SYS(NMC_PROCESS,L"resume game process");
        if (e.code) TerminateProcess(pi.hProcess,1); /* Never leave our suspended child behind. */
        else if (pid) *pid=pi.dwProcessId;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    if (cmd.data) SecureZeroMemory(cmd.data,cmd.len*sizeof(wchar_t));
    free(cmd.data); return e;
}
static bool uri(NmcBytes *b,const wchar_t *path) {
    char s[NMC_PATH*3]; static const char hex[]="0123456789ABCDEF";
    if (!nmc_utf8(path,s,sizeof(s)) || !nmc_bytes_add(b,"file:/",6)) return false;
    for (const unsigned char *p=(const unsigned char *)s;*p;++p) {
        unsigned char c=*p;
        if (c=='\\') c='/';
        if (strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/:-._~",c)) {
            if (!nmc_bytes_add(b,&c,1)) return false;
        } else {
            char esc[]={'%',hex[c>>4],hex[c&15]};
            if (!nmc_bytes_add(b,esc,sizeof(esc))) return false;
        }
    }
    return true;
}
NmcError nmc_classpath_jar(const wchar_t *inst,const NmcArgs *cp,wchar_t out[NMC_PATH]) {
    NmcBytes line={0}, manifest={0}; NmcError e=OK;
    mz_zip_archive zip={0}; void *blob=NULL; size_t len=0;
    if (!nmc_join(out,inst,L"classpath.jar")) return ERR(NMC_PATH_ERROR,L"classpath bridge path");
    if (!nmc_bytes_add(&line,"Class-Path:",11) || !nmc_bytes_add(&manifest,"Manifest-Version: 1.0\r\n",23)) goto memory;
    for (size_t i=0;i<cp->n;++i)
        if (!nmc_bytes_add(&line," ",1) || !uri(&line,cp->v[i])) goto memory;
    for (size_t i=0;i<line.len;) {
        size_t width=i?69:70; if (width>line.len-i) width=line.len-i;
        if ((i && !nmc_bytes_add(&manifest," ",1)) ||
            !nmc_bytes_add(&manifest,line.data+i,width) || !nmc_bytes_add(&manifest,"\r\n",2)) goto memory;
        i+=width;
    }
    if (!nmc_bytes_add(&manifest,"\r\n",2)) goto memory;
    if (!mz_zip_writer_init_heap(&zip,0,0) ||
        !mz_zip_writer_add_mem(&zip,"META-INF/MANIFEST.MF",manifest.data,manifest.len,0) ||
        !mz_zip_writer_finalize_heap_archive(&zip,&blob,&len)) {
        e=ERR(NMC_ZIP,L"classpath JAR"); goto done;
    }
    e=nmc_atomic(out,blob,len); goto done;
memory:
    e=ERR(NMC_MEMORY,L"classpath manifest");
done:
    if (zip.m_pState) mz_zip_writer_end(&zip);
    mz_free(blob); free(line.data); free(manifest.data); return e;
}

