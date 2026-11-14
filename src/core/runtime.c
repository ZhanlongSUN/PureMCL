#include "internal.h"
#include <wctype.h>

static bool x64_pe(const wchar_t *exe) {
    uint64_t ignored;
    if (!nmc_file_size(exe,&ignored)) return false;
    HANDLE f=CreateFileW(exe,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (f==INVALID_HANDLE_VALUE) return false;
    IMAGE_DOS_HEADER dos; DWORD sig=0,n=0; IMAGE_FILE_HEADER header;
    bool ok=ReadFile(f,&dos,sizeof(dos),&n,NULL) && n==sizeof(dos) && dos.e_magic==IMAGE_DOS_SIGNATURE &&
        dos.e_lfanew>0 && dos.e_lfanew<1024*1024 && SetFilePointer(f,dos.e_lfanew,NULL,FILE_BEGIN)!=INVALID_SET_FILE_POINTER &&
        ReadFile(f,&sig,sizeof(sig),&n,NULL) && n==sizeof(sig) && sig==IMAGE_NT_SIGNATURE &&
        ReadFile(f,&header,sizeof(header),&n,NULL) && n==sizeof(header) && header.Machine==IMAGE_FILE_MACHINE_AMD64 &&
        !(header.Characteristics&IMAGE_FILE_DLL);
    CloseHandle(f); return ok;
}
static unsigned release_major(const wchar_t *exe) {
    wchar_t path[NMC_PATH]; wcscpy(path,exe);
    wchar_t *last=wcsrchr(path,L'\\');
    if (!last) return 0;
    *last=0; last=wcsrchr(path,L'\\');
    if (!last) return 0;
    *last=0;
    for (unsigned i=0;i<2;++i) {
        wchar_t release[NMC_PATH]; NmcBytes b={0};
        if (nmc_join(release,path,L"release") && !nmc_read(release,65536,&b).code) {
            const char *s=b.data;
            if (strncmp(s,"JAVA_VERSION=\"",14)) s=strstr(s,"\nJAVA_VERSION=\""), s=s?s+1:NULL;
            unsigned major=0;
            if (s) { s+=14; if (!strncmp(s,"1.",2)) s+=2; major=(unsigned)strtoul(s,NULL,10); }
            free(b.data);
            if (major) return major;
        }
        last=wcsrchr(path,L'\\');
        if (!last || last-path<3 || _wcsicmp(last+1,L"jre")) break;
        *last=0;
    }
    return 0;
}
static bool candidate(const wchar_t *path,unsigned want,wchar_t out[NMC_PATH]) {
    uint64_t size; wchar_t exe[NMC_PATH], canonical[NMC_PATH];
    DWORD n=GetFullPathNameW(path,NMC_PATH,canonical,NULL);
    if (!n || n>=NMC_PATH) return false;
    if (nmc_file_size(canonical,&size)) {
        const wchar_t *name=wcsrchr(canonical,L'\\');
        if (!name || (_wcsicmp(name+1,L"java.exe") && _wcsicmp(name+1,L"javaw.exe"))) return false;
        wcscpy(exe,canonical);
    } else if (!nmc_join(exe,canonical,L"bin\\javaw.exe") || !nmc_file_size(exe,&size)) return false;
    if (!x64_pe(exe) || release_major(exe)!=want) return false;
    wcscpy(out,exe); return true;
}
static bool subdirs(const wchar_t *base,unsigned major,wchar_t out[NMC_PATH]) {
    wchar_t search[NMC_PATH]; WIN32_FIND_DATAW data;
    if (!nmc_join(search,base,L"*")) return false;
    HANDLE f=FindFirstFileW(search,&data);
    if (f==INVALID_HANDLE_VALUE) return false;
    bool found=false;
    do {
        if (!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
            !wcscmp(data.cFileName,L".") || !wcscmp(data.cFileName,L"..")) continue;
        if (nmc_join(search,base,data.cFileName) && candidate(search,major,out)) { found=true; break; }
    } while (FindNextFileW(f,&data));
    FindClose(f); return found;
}
static bool installed(unsigned major,wchar_t out[NMC_PATH]) {
    wchar_t p[NMC_PATH], dir[NMC_PATH];
    DWORD n=GetEnvironmentVariableW(L"JAVA_HOME",p,NMC_PATH);
    if (n && n<NMC_PATH && candidate(p,major,out)) return true;
    n=GetEnvironmentVariableW(L"PATH",NULL,0);
    if (n && n<=32768) {
        wchar_t *env=malloc((size_t)n*sizeof(wchar_t));
        if (env) {
            DWORD len=GetEnvironmentVariableW(L"PATH",env,n); wchar_t *ctx=NULL;
            if (len && len<n) for (wchar_t *v=wcstok_s(env,L";",&ctx);v;v=wcstok_s(NULL,L";",&ctx)) {
                /* Relative PATH entries must never select a Java executable. */
                if (wcslen(v)<3 || v[1]!=L':' || (v[2]!=L'\\' && v[2]!=L'/')) continue;
                if ((nmc_join(p,v,L"javaw.exe") && candidate(p,major,out)) ||
                    (nmc_join(p,v,L"java.exe") && candidate(p,major,out))) { free(env); return true; }
            }
            free(env);
        }
    }
    n=GetEnvironmentVariableW(L"ProgramFiles",p,NMC_PATH);
    static const wchar_t *const vendors[]={L"Java",L"Eclipse Adoptium",L"Microsoft",L"Amazon Corretto",L"BellSoft",L"Zulu"};
    if (n && n<NMC_PATH) for (size_t i=0;i<NMC_ARRAY(vendors);++i)
        if (nmc_join(dir,p,vendors[i]) && subdirs(dir,major,out)) return true;
    return false;
}
static NmcError install_runtime(const NmcConfig *c,const NmcPlan *p,const wchar_t *dir,bool repair,NmcTask *t) {
    yyjson_doc *all=NULL,*manifest=NULL; NmcFiles files={0}; unsigned status=0; NmcError e=OK;
    wchar_t manifest_path[NMC_PATH], stamp[NMC_PATH]; char rel[128];
    nmc_progress(t,NMC_STAGE_JAVA,0,0);
    TRY(nmc_http_json(RUNTIME_MANIFEST,NULL,NULL,t,&status,&all));
    if (status!=200) { e=(NmcError){NMC_HTTP,status,L"Java runtime catalog"}; goto done; }
    yyjson_val *platform=yyjson_obj_get(yyjson_doc_get_root(all),"windows-x64");
    yyjson_val *chosen=yyjson_arr_get_first(yyjson_obj_get(platform,p->component));
    snprintf(rel,sizeof(rel),"metadata/runtime-%s.json",p->component);
    NmcFile file={0};
    TRY(nmc_file_meta(&file,c->root,rel,yyjson_obj_get(chosen,"manifest")));
    wcscpy(manifest_path,file.path);
    TRY(nmc_fetch(&file,true,t)); TRY(nmc_json(manifest_path,&manifest));
    yyjson_val *items=yyjson_obj_get(yyjson_doc_get_root(manifest),"files"),*key,*entry;
    size_t i,n;
    if (!yyjson_is_obj(items) || yyjson_obj_size(items)>10000) { e=ERR(NMC_JSON,L"Java package manifest"); goto done; }
    yyjson_obj_foreach(items,i,n,key,entry) {
        const char *name=yyjson_get_str(key), *type=nmc_str(entry,"type"); wchar_t sub[NMC_PATH];
        if (!name || strlen(name)!=yyjson_get_len(key) || !nmc_path(sub,dir,name) || !type) {
            e=ERR(NMC_JSON,L"Java package path"); goto done;
        }
        if (!strcmp(type,"directory")) { TRY(nmc_mkdir(sub)); continue; }
        if (strcmp(type,"file")) { e=ERR(NMC_UNSUPPORTED,L"Java package links"); goto done; }
        TRY(nmc_file_meta(&file,dir,name,yyjson_obj_get(yyjson_obj_get(entry,"downloads"),"raw")));
        if (!nmc_grow((void **)&files.v,&files.cap,files.n+1,sizeof(file))) { e=ERR(NMC_MEMORY,L"Java package files"); goto done; }
        files.v[files.n++]=file;
    }
    if (!files.n) { e=ERR(NMC_JSON,L"empty Java package"); goto done; }
    /* Verify all existing files before recognizing an interrupted package. */
    TRY(nmc_batch(&files,c->jobs,true,NMC_STAGE_JAVA,t));
    (void)repair;
    if (!nmc_join(stamp,dir,L".pure-runtime")) { e=ERR(NMC_PATH_ERROR,L"Java completion path"); goto done; }
    e=nmc_atomic(stamp,"1",1);
done:
    yyjson_doc_free(all); yyjson_doc_free(manifest); free(files.v); return e;
}
NmcError nmc_runtime(const NmcConfig *c,const NmcPlan *p,bool online,bool repair,NmcTask *t,wchar_t out[NMC_PATH]) {
    if (c->java[0]) return candidate(c->java,p->java_major,out)?OK:ERR(NMC_JAVA,L"configured Java x64 / major version");
    wchar_t dir[NMC_PATH], marker[NMC_PATH]; char rel[NMC_ID+16]; uint64_t size;
    snprintf(rel,sizeof(rel),"runtime/%s",p->component);
    if (!nmc_path(dir,c->root,rel) || !nmc_join(marker,dir,L".pure-runtime")) return ERR(NMC_PATH_ERROR,L"runtime path");
    bool managed=nmc_file_size(marker,&size);
    if (managed && !repair && candidate(dir,p->java_major,out)) return OK;
    if ((!managed || !repair) && installed(p->java_major,out)) return OK;
    if (!online) return ERR(NMC_JAVA,L"compatible Java unavailable");
    CHECK(install_runtime(c,p,dir,repair,t));
    return candidate(dir,p->java_major,out)?OK:ERR(NMC_JAVA,L"downloaded runtime x64 / major version");
}

