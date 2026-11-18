#include "internal.h"
#include <shlobj.h>
#include <limits.h>

NmcError nmc_winerr(NmcCode code, DWORD detail, const wchar_t *where) {
    switch (detail) {
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: code = NMC_ACCESS; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: code = NMC_DISK_FULL; break;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND:
        if (code == NMC_FILE) code = NMC_MISSING;
        break;
    }
    return (NmcError){code, detail, where};
}
const wchar_t *nmc_error_text(NmcCode code) {
    static const wchar_t *const labels[] = {
        L"完成", L"内存不足", L"文件操作失败", L"路径无效或不安全", L"权限不足",
        L"磁盘空间不足", L"网络请求失败", L"文件校验失败", L"元数据无效",
        L"解压失败", L"找不到兼容的 x64 Java", L"账号验证失败", L"无法创建游戏进程",
        L"已取消", L"实例或共享目录正在使用", L"文件缺失", L"暂不支持此配置"
    };
    return (unsigned)code < NMC_ARRAY(labels) ? labels[code] : L"未知错误";
}
bool nmc_cancelled(NmcTask *task) { return task && (atomic_load(&task->cancel) || atomic_load(&task->stop_workers)); }
void nmc_task_reset(NmcTask *t) {
    atomic_init(&t->cancel,false);
    atomic_init(&t->stop_workers,false);
    atomic_init(&t->download_region,NMC_REGION_UNCHECKED);
    for (size_t i=0;i<NMC_ARRAY(t->mirror_retry_after);++i) atomic_init(&t->mirror_retry_after[i],0);
    atomic_init(&t->mirror_announced,false);
    atomic_init(&t->received_bytes,0);
    atomic_init(&t->next_transfer_notice,0);
    t->http_session=NULL;
    t->watch_error=OK;
}
void nmc_progress(NmcTask *t, NmcStage s, size_t i, size_t n) {
    if (t && t->progress) t->progress(t->user, s, i, n);
}
bool nmc_grow(void **p, size_t *cap, size_t count, size_t item) {
    if (count <= *cap) return true;
    if (count > SIZE_MAX / item) return false;
    size_t n = *cap && *cap <= SIZE_MAX / 2 ? *cap * 2 : 8;
    if (n < count || n > SIZE_MAX / item) n = count;
    void *next = realloc(*p, n * item);
    if (!next) return false;
    *p = next; *cap = n;
    return true;
}
bool nmc_bytes_add(NmcBytes *b, const void *data, size_t n) {
    if (n >= SIZE_MAX - b->len || !nmc_grow((void **)&b->data, &b->cap, b->len+n+1, 1)) return false;
    if (n) memcpy(b->data+b->len, data, n);
    b->len += n; b->data[b->len] = 0;
    return true;
}
bool nmc_wide_add(NmcWide *b, const wchar_t *data, size_t n) {
    if (n >= SIZE_MAX - b->len || !nmc_grow((void **)&b->data, &b->cap, b->len+n+1, sizeof(wchar_t))) return false;
    if (n) memcpy(b->data+b->len, data, n*sizeof(wchar_t));
    b->len += n; b->data[b->len] = 0;
    return true;
}
bool nmc_args_take(NmcArgs *a,wchar_t *owned) {
    if (!nmc_grow((void **)&a->v, &a->cap, a->n+1, sizeof(*a->v))) return false;
    a->v[a->n++]=owned;
    return true;
}
bool nmc_args_add(NmcArgs *a, const wchar_t *s) {
    wchar_t *copy=_wcsdup(s);
    if (!copy) return false;
    if (nmc_args_take(a,copy)) return true;
    SecureZeroMemory(copy,wcslen(copy)*sizeof(wchar_t));
    free(copy);
    return false;
}
void nmc_args_free(NmcArgs *a) {
    for (size_t i=0; i<a->n; ++i) {
        SecureZeroMemory(a->v[i], wcslen(a->v[i])*sizeof(wchar_t));
        free(a->v[i]);
    }
    free(a->v); *a = (NmcArgs){0};
}
bool nmc_utf16(const char *s, wchar_t *w, size_t n) {
    return s && n <= INT_MAX && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, (int)n) != 0;
}
bool nmc_utf8(const wchar_t *w, char *s, size_t n) {
    return w && n <= INT_MAX && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, s, (int)n, NULL, NULL) != 0;
}
bool nmc_id(const char *s) {
    if (!s || !*s || strlen(s) >= NMC_ID || *s == '.' || s[strlen(s)-1] == '.') return false;
    for (; *s; ++s) if (!strchr("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._-+", *s)) return false;
    return true;
}
bool nmc_hex(const char *s, size_t n) {
    if (!s || strlen(s) != n) return false;
    for (; *s; ++s) if (!strchr("0123456789abcdef", *s)) return false;
    return true;
}
static bool part(const wchar_t *s, size_t n) {
    if (!n || n>255 || s[n-1]==L'.' || s[n-1]==L' ') return false;
    size_t base=0;
    while (base<n && s[base]!=L'.') ++base;
    while (base && s[base-1]==L' ') --base;
    for (size_t i=0; i<n; ++i)
        if (s[i]<32 || wcschr(L"<>:\"|?*\\", s[i])) return false;
    if ((base==3 && (!_wcsnicmp(s,L"CON",3) || !_wcsnicmp(s,L"PRN",3) ||
                     !_wcsnicmp(s,L"AUX",3) || !_wcsnicmp(s,L"NUL",3))) ||
        (base==4 && (!_wcsnicmp(s,L"COM",3) || !_wcsnicmp(s,L"LPT",3)) &&
         wcschr(L"0123456789¹²³",s[3])) ||
        (base==6 && !_wcsnicmp(s,L"CONIN$",6)) ||
        (base==7 && !_wcsnicmp(s,L"CONOUT$",7))) return false;
    return true;
}
bool nmc_relative(const char *s, wchar_t out[NMC_PATH]) {
    if (!nmc_utf16(s,out,NMC_PATH) || !*out) return false;
    wchar_t *start=out;
    for (wchar_t *p=out;;++p) {
        if (*p!=L'/' && *p) continue;
        if (!part(start,(size_t)(p-start))) return false;
        if (!*p) return true;
        *p=L'\\'; start=p+1;
    }
}
bool nmc_join(wchar_t out[NMC_PATH], const wchar_t *base, const wchar_t *tail) {
    size_t a=wcslen(base), b=wcslen(tail);
    if (!a || !b || a+b+2>NMC_PATH) return false;
    bool slash=base[a-1]!=L'\\' && base[a-1]!=L'/';
    if (out!=base) wmemcpy(out,base,a);
    if (slash) out[a++]=L'\\';
    wmemcpy(out+a,tail,b+1);
    return true;
}
bool nmc_path(wchar_t out[NMC_PATH], const wchar_t *base, const char *tail) {
    wchar_t sub[NMC_PATH];
    return nmc_relative(tail,sub) && nmc_join(out,base,sub);
}
NmcError nmc_home(wchar_t out[NMC_PATH]) {
    PWSTR p=NULL;
    HRESULT hr=SHGetKnownFolderPath(&FOLDERID_LocalAppData,0,NULL,&p);
    if (FAILED(hr)) return (NmcError){NMC_PATH_ERROR,(DWORD)hr,L"LocalAppData"};
    bool ok=nmc_join(out,p,L"PureMCL");
    CoTaskMemFree(p);
    return ok ? OK : ERR(NMC_PATH_ERROR,L"LocalAppData/PureMCL");
}
NmcError nmc_launcher_versions(wchar_t out[NMC_PATH]) {
    DWORD n=GetModuleFileNameW(NULL,out,NMC_PATH);
    if (!n || n>=NMC_PATH) return SYS(NMC_PATH_ERROR,L"launcher executable path");
    wchar_t *slash=wcsrchr(out,L'\\');
    if (!slash || slash-out<2 || out[1]!=L':' || out[2]!=L'\\' || wcschr(out,L';'))
        return ERR(NMC_PATH_ERROR,L"launcher must be on a local drive; no semicolon in path");
    if (slash-out==2) ++slash;
    *slash=0;
    return nmc_join(out,out,L"versions")?OK:ERR(NMC_PATH_ERROR,L"fixed versions directory");
}
static size_t drive_prefix(const wchar_t *p) {
    return wcslen(p)>=3 && ((p[0]>=L'A'&&p[0]<=L'Z') || (p[0]>=L'a'&&p[0]<=L'z')) &&
        p[1]==L':' && p[2]==L'\\' ? 3 : 0;
}
static NmcError directory(const wchar_t *p, bool create) {
    if (create && !CreateDirectoryW(p,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS)
        return SYS(NMC_FILE,L"CreateDirectoryW");
    DWORD attr=GetFileAttributesW(p);
    if (attr==INVALID_FILE_ATTRIBUTES) return SYS(NMC_FILE,L"directory attributes");
    if (!(attr&FILE_ATTRIBUTE_DIRECTORY) || (attr&FILE_ATTRIBUTE_REPARSE_POINT))
        return ERR(NMC_PATH_ERROR,L"directory or reparse point");
    return OK;
}
static NmcError walk(const wchar_t *p, bool create) {
    wchar_t buf[NMC_PATH];
    size_t root=drive_prefix(p), n=wcslen(p);
    if (!root || n>=NMC_PATH) return ERR(NMC_PATH_ERROR,L"absolute local path");
    wcscpy(buf,p);
    wchar_t *segment=buf+root;
    for (wchar_t *q=segment;;++q) {
        wchar_t c=*q;
        if (c!=L'\\' && c) continue;
        if (q==segment && !c) return OK;
        if (!part(segment,(size_t)(q-segment))) return ERR(NMC_PATH_ERROR,L"path component");
        *q=0;
        NmcError e=directory(buf,create);
        *q=c;
        if (e.code || !c) return e;
        segment=q+1;
    }
}
NmcError nmc_mkdir(const wchar_t *p) { return walk(p,true); }
static NmcError parent(const wchar_t *p, bool create) {
    const wchar_t *slash=wcsrchr(p,L'\\');
    wchar_t dir[NMC_PATH];
    if (!slash || slash-p<2 || (size_t)(slash-p)>=NMC_PATH ||
        !part(slash+1,wcslen(slash+1))) return ERR(NMC_PATH_ERROR,L"file path");
    size_t n=(size_t)(slash-p); if (n==2) ++n;
    wmemcpy(dir,p,n); dir[n]=0;
    return walk(dir,create);
}
NmcError nmc_parent(const wchar_t *p) { return parent(p,true); }
bool nmc_file_size(const wchar_t *p, uint64_t *size) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (parent(p,false).code || !GetFileAttributesExW(p,GetFileExInfoStandard,&data) ||
        (data.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) return false;
    *size=((uint64_t)data.nFileSizeHigh<<32)|data.nFileSizeLow;
    return true;
}
NmcError nmc_read(const wchar_t *p, size_t limit, NmcBytes *out) {
    *out=(NmcBytes){0};
    CHECK(parent(p,false));
    HANDLE f=CreateFileW(p,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if (f==INVALID_HANDLE_VALUE) return SYS(NMC_FILE,L"open local file");
    LARGE_INTEGER sz;
    BY_HANDLE_FILE_INFORMATION info;
    NmcError e=OK;
    if (!GetFileInformationByHandle(f,&info) || !GetFileSizeEx(f,&sz)) e=SYS(NMC_FILE,L"read file attributes");
    else if ((info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) ||
             sz.QuadPart<0 || (uint64_t)sz.QuadPart>limit || (uint64_t)sz.QuadPart>=MAXDWORD)
        e=ERR(NMC_PATH_ERROR,L"local file type or size");
    else {
        size_t n=(size_t)sz.QuadPart; DWORD got=0;
        if (!nmc_grow((void **)&out->data,&out->cap,n+1,1)) e=ERR(NMC_MEMORY,L"read local file");
        else if (!ReadFile(f,out->data,(DWORD)n,&got,NULL)) e=SYS(NMC_FILE,L"ReadFile");
        else if (got!=n) e=ERR(NMC_FILE,L"short read");
        else { out->len=n; out->data[n]=0; }
    }
    CloseHandle(f);
    if (e.code) { free(out->data); *out=(NmcBytes){0}; }
    return e;
}
NmcError nmc_temp(const wchar_t *target, wchar_t tmp[NMC_PATH], HANDLE *f) {
    CHECK(nmc_parent(target));
    static atomic_uint serial;
    for (unsigned i=0;i<16;++i) {
        if (swprintf(tmp,NMC_PATH,L"%ls.pure-%lu-%u.tmp",target,GetCurrentProcessId(),atomic_fetch_add(&serial,1))<0)
            return ERR(NMC_PATH_ERROR,L"temporary path length");
        *f=CreateFileW(tmp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if (*f!=INVALID_HANDLE_VALUE) return OK;
        if (GetLastError()!=ERROR_FILE_EXISTS) return SYS(NMC_FILE,L"create temporary file");
    }
    return ERR(NMC_FILE,L"temporary file collision");
}
NmcError nmc_promote(const wchar_t *tmp, const wchar_t *target) {
    DWORD a=GetFileAttributesW(target);
    if (a!=INVALID_FILE_ATTRIBUTES && (a&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))
        return ERR(NMC_PATH_ERROR,L"replace unsafe target");
    return MoveFileExW(tmp,target,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)
        ? OK : SYS(NMC_FILE,L"atomic replace");
}
NmcError nmc_write(HANDLE f, const void *data, size_t len) {
    const char *p=data;
    while (len) {
        DWORD n=len>1048576 ? 1048576 : (DWORD)len, wrote=0;
        if (!WriteFile(f,p,n,&wrote,NULL)) return SYS(NMC_FILE,L"WriteFile");
        if (!wrote) return ERR(NMC_FILE,L"short write");
        len-=wrote; p+=wrote;
    }
    return OK;
}
NmcError nmc_copy_file(const wchar_t *source,const wchar_t *target,NmcTask *t) {
    CHECK(parent(source,false));
    HANDLE in=CreateFileW(source,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if (in==INVALID_HANDLE_VALUE) return SYS(NMC_FILE,L"open copy source");
    BY_HANDLE_FILE_INFORMATION info; HANDLE out=INVALID_HANDLE_VALUE;
    wchar_t tmp[NMC_PATH]; bool owned=false;
    unsigned char buf[65536];
    NmcError e=OK;
    if (!GetFileInformationByHandle(in,&info) ||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) {
        e=ERR(NMC_PATH_ERROR,L"unsafe copy source"); goto done;
    }
    TRY(nmc_temp(target,tmp,&out)); owned=true;
    for (;;) {
        DWORD got=0;
        if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"copy cancelled"); goto done; }
        if (!ReadFile(in,buf,sizeof(buf),&got,NULL)) { e=SYS(NMC_FILE,L"copy read"); goto done; }
        if (!got) break;
        TRY(nmc_write(out,buf,got));
    }
    CloseHandle(out); out=INVALID_HANDLE_VALUE;
    TRY(nmc_promote(tmp,target));
done:
    if (out!=INVALID_HANDLE_VALUE) CloseHandle(out);
    if (owned && e.code) DeleteFileW(tmp);
    CloseHandle(in); return e;
}
static NmcError remove_entry(const wchar_t *path,unsigned depth) {
    DWORD attr=GetFileAttributesW(path);
    if (attr==INVALID_FILE_ATTRIBUTES) {
        DWORD error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND?OK:nmc_winerr(NMC_FILE,error,L"uninstall attributes");
    }
    if (depth>512) return ERR(NMC_PATH_ERROR,L"uninstall directory depth");
    if ((attr&FILE_ATTRIBUTE_DIRECTORY) && !(attr&FILE_ATTRIBUTE_REPARSE_POINT)) {
        wchar_t *child=malloc(NMC_PATH*sizeof(*child)); WIN32_FIND_DATAW data;
        if (!child) return ERR(NMC_MEMORY,L"uninstall directory");
        if (!nmc_join(child,path,L"*")) { free(child); return ERR(NMC_PATH_ERROR,L"uninstall directory"); }
        HANDLE find=FindFirstFileW(child,&data);
        NmcError e=OK;
        if (find==INVALID_HANDLE_VALUE && GetLastError()!=ERROR_FILE_NOT_FOUND)
            e=SYS(NMC_FILE,L"enumerate uninstall directory");
        if (find!=INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(data.cFileName,L".") || !wcscmp(data.cFileName,L"..")) continue;
                if (!nmc_join(child,path,data.cFileName)) { e=ERR(NMC_PATH_ERROR,L"uninstall child"); break; }
                e=remove_entry(child,depth+1);
                if (e.code) break;
            } while (FindNextFileW(find,&data));
            if (!e.code && GetLastError()!=ERROR_NO_MORE_FILES) e=SYS(NMC_FILE,L"enumerate uninstall directory");
            FindClose(find);
        }
        free(child);
        CHECK(e);
    }
    if (attr&FILE_ATTRIBUTE_DIRECTORY)
        return RemoveDirectoryW(path)?OK:SYS(NMC_FILE,L"remove game directory");
    return DeleteFileW(path)?OK:SYS(NMC_FILE,L"remove game file");
}
NmcError nmc_remove_tree(const wchar_t *root,const wchar_t *path) {
    size_t n=wcslen(root);
    if (!n || n>=wcslen(path) || _wcsnicmp(root,path,n) || path[n]!=L'\\' ||
        wcsstr(path,L"\\..\\") || wcsstr(path,L"\\.\\"))
        return ERR(NMC_PATH_ERROR,L"uninstall path outside its base");
    NmcError e=parent(path,false);
    if (e.code==NMC_MISSING) return OK;
    CHECK(e);
    return remove_entry(path,0);
}
NmcError nmc_atomic(const wchar_t *path, const void *data, size_t len) {
    wchar_t tmp[NMC_PATH]; HANDLE f;
    CHECK(nmc_temp(path,tmp,&f));
    NmcError e=nmc_write(f,data,len);
    if (!e.code && !FlushFileBuffers(f)) e=SYS(NMC_FILE,L"flush local file");
    CloseHandle(f);
    if (!e.code) e=nmc_promote(tmp,path);
    if (e.code) DeleteFileW(tmp);
    return e;
}
NmcError nmc_json_mem(NmcBytes *b, yyjson_doc **out) {
    yyjson_read_err err={0};
    *out=yyjson_read_opts(b->data,b->len,0,NULL,&err);
    return *out ? OK : (NmcError){NMC_JSON,(DWORD)err.code,L"JSON syntax"};
}
NmcError nmc_json(const wchar_t *p, yyjson_doc **out) {
    NmcBytes b={0}; *out=NULL;
    CHECK(nmc_read(p,JSON_LIMIT,&b));
    NmcError e=nmc_json_mem(&b,out);
    free(b.data);
    return e;
}
const char *nmc_str(yyjson_val *obj, const char *key) {
    yyjson_val *v=yyjson_obj_get(obj,key);
    const char *s=yyjson_get_str(v);
    return s && strlen(s)==yyjson_get_len(v) ? s : NULL;
}
bool nmc_copy(char *dst, size_t cap, const char *s) {
    if (!s || strlen(s)>=cap) return false;
    strcpy(dst,s); return true;
}
NmcError nmc_lock(const wchar_t *path, HANDLE *out) {
    CHECK(nmc_parent(path));
    *out=CreateFileW(path,GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (*out==INVALID_HANDLE_VALUE)
        return SYS(GetLastError()==ERROR_SHARING_VIOLATION ? NMC_BUSY : NMC_FILE,L"workspace lock");
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(*out,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(*out); *out=INVALID_HANDLE_VALUE; return ERR(NMC_PATH_ERROR,L"lock reparse point");
    }
    return OK;
}

