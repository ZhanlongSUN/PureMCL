#include "internal.h"

static const wchar_t CONFIG[] = L"settings.json";
static NmcError config_path(wchar_t out[NMC_PATH]) {
    CHECK(nmc_home(out));
    return nmc_join(out,out,CONFIG)?OK:ERR(NMC_PATH_ERROR,L"settings path");
}
static unsigned read_uint(yyjson_val *r,const char *key,unsigned fallback) {
    yyjson_val *v=yyjson_obj_get(r,key);
    return yyjson_is_uint(v) && yyjson_get_uint(v)<=UINT_MAX ? (unsigned)yyjson_get_uint(v) : fallback;
}
static bool read_wide(yyjson_val *r,const char *key,wchar_t *out,size_t cap) {
    return !yyjson_obj_get(r,key) || nmc_utf16(nmc_str(r,key),out,cap);
}
static bool read_str(yyjson_val *r,const char *key,char *out,size_t cap) {
    return !yyjson_obj_get(r,key) || nmc_copy(out,cap,nmc_str(r,key));
}
static bool safe_id(const char *id) {
    wchar_t wide[NMC_PATH]; return nmc_id(id) && nmc_relative(id,wide);
}
NmcError nmc_config_validate(const NmcConfig *c) {
    if (!safe_id(c->instance) || !safe_id(c->version) ||
        (unsigned)c->loader>NMC_OPTIFINE || (c->loader_version[0] && !safe_id(c->loader_version)) ||
        (c->account[0] && !nmc_hex(c->account,32)) ||
        c->jobs<1 || c->jobs>16 || c->gc>2 || c->xms<128 || c->xms>c->xmx || c->xmx>262144)
        return ERR(NMC_PATH_ERROR,L"instance, version, memory, GC or concurrency");
    if (wcslen(c->root)<4 || c->root[1]!=L':' || c->root[2]!=L'\\' ||
        wcslen(c->versions_dir)<4 || c->versions_dir[1]!=L':' || c->versions_dir[2]!=L'\\' ||
        wcschr(c->versions_dir,L';') ||
        wcschr(c->root,L';') || (c->java[0] && (wcslen(c->java)<4 || c->java[1]!=L':' || c->java[2]!=L'\\')))
        return ERR(NMC_PATH_ERROR,L"data, versions or Java path");
    return OK;
}
static NmcError read_instance_json(NmcConfig *c,yyjson_val *r) {
    c->loader=(NmcLoader)read_uint(r,"loader",NMC_VANILLA);
    c->loader_version[0]=0;
    c->loader_profile[0]=0;
    c->xms=read_uint(r,"xms",c->xms); c->xmx=read_uint(r,"xmx",c->xmx);
    c->gc=read_uint(r,"gc",c->gc);
    if (!yyjson_is_obj(r) || !read_str(r,"version",c->version,sizeof(c->version)) ||
        !read_str(r,"loaderVersion",c->loader_version,sizeof(c->loader_version)) ||
        !read_wide(r,"java",c->java,NMC_ARRAY(c->java)) || !read_wide(r,"jvm",c->jvm,NMC_ARRAY(c->jvm)))
        return ERR(NMC_JSON,L"instance configuration");
    return nmc_config_validate(c);
}
NmcError nmc_config_load(NmcConfig *c) {
    wchar_t path[NMC_PATH];
    *c=(NmcConfig){.instance="default",.version="latest",.xms=256,.xmx=2048,.jobs=6,.exit_on_launch=true};
    MEMORYSTATUSEX mem={.dwLength=sizeof(mem)};
    if (GlobalMemoryStatusEx(&mem) && mem.ullTotalPhys>=UINT64_C(12000000000)) c->xmx=4096;
    CHECK(nmc_home(c->root)); CHECK(nmc_launcher_versions(c->versions_dir)); CHECK(config_path(path));
    yyjson_doc *doc=NULL;
    NmcError e=nmc_json(path,&doc);
    if (e.code==NMC_MISSING) return OK;
    if (e.code) return e;
    yyjson_val *r=yyjson_doc_get_root(doc);
    c->jobs=read_uint(r,"jobs",c->jobs);
    if (yyjson_is_bool(yyjson_obj_get(r,"exitOnLaunch"))) c->exit_on_launch=yyjson_get_bool(yyjson_obj_get(r,"exitOnLaunch"));
    c->track_logs=yyjson_is_true(yyjson_obj_get(r,"trackLogs"));
    if (!read_str(r,"instance",c->instance,sizeof(c->instance)) ||
        !read_str(r,"account",c->account,sizeof(c->account)))
        e=ERR(NMC_JSON,L"launcher settings");
    else e=read_instance_json(c,r);
    yyjson_doc_free(doc);
    return e;
}
static bool fields(yyjson_mut_doc *d,yyjson_mut_val *r,const NmcConfig *c) {
    char java[NMC_PATH*3], jvm[NMC_ARRAY(c->jvm)*3];
    return nmc_utf8(c->java,java,sizeof(java)) && nmc_utf8(c->jvm,jvm,sizeof(jvm)) &&
        yyjson_mut_obj_add_strcpy(d,r,"java",java) && yyjson_mut_obj_add_strcpy(d,r,"jvm",jvm) &&
        yyjson_mut_obj_add_strcpy(d,r,"version",c->version) && yyjson_mut_obj_add_uint(d,r,"xms",c->xms) &&
        yyjson_mut_obj_add_uint(d,r,"loader",c->loader) &&
        yyjson_mut_obj_add_strcpy(d,r,"loaderVersion",c->loader_version) &&
        yyjson_mut_obj_add_uint(d,r,"xmx",c->xmx) && yyjson_mut_obj_add_uint(d,r,"gc",c->gc);
}
static NmcError write_config(const NmcConfig *c,const wchar_t *path,bool global) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL); yyjson_mut_val *r=d?yyjson_mut_obj(d):NULL;
    NmcError e=ERR(NMC_MEMORY,L"settings JSON");
    if (r && fields(d,r,c) && (!global ||
        (yyjson_mut_obj_add_strcpy(d,r,"instance",c->instance) && yyjson_mut_obj_add_strcpy(d,r,"account",c->account) &&
         yyjson_mut_obj_add_uint(d,r,"jobs",c->jobs) &&
         yyjson_mut_obj_add_bool(d,r,"exitOnLaunch",c->exit_on_launch) &&
         yyjson_mut_obj_add_bool(d,r,"trackLogs",c->track_logs)))) {
        yyjson_mut_doc_set_root(d,r);
        size_t n=0; char *json=yyjson_mut_write(d,YYJSON_WRITE_PRETTY,&n);
        if (json) { e=nmc_atomic(path,json,n); free(json); }
    }
    yyjson_mut_doc_free(d); return e;
}
NmcError nmc_config_save(const NmcConfig *c) {
    wchar_t p[NMC_PATH]; CHECK(nmc_config_validate(c)); CHECK(config_path(p));
    return write_config(c,p,true);
}
NmcError nmc_instance_dir(const NmcConfig *c,wchar_t out[NMC_PATH]) {
    char rel[NMC_ID+16];
    if (!safe_id(c->instance)) return ERR(NMC_PATH_ERROR,L"instance name");
    snprintf(rel,sizeof(rel),"instances/%s",c->instance);
    return nmc_path(out,c->root,rel)?OK:ERR(NMC_PATH_ERROR,L"instance path");
}
NmcError nmc_game_instance(const NmcConfig *c,const char *version,wchar_t out[NMC_PATH]) {
    char rel[NMC_ID*2+16];
    if (!safe_id(version) || !safe_id(c->instance) || !_stricmp(version,"latest"))
        return ERR(NMC_PATH_ERROR,L"resolved version / instance ID");
    snprintf(rel,sizeof(rel),"%s/instances/%s",version,c->instance);
    return nmc_path(out,c->versions_dir,rel)?OK:ERR(NMC_PATH_ERROR,L"isolated game directory");
}
static NmcError instance_file(const NmcConfig *c,wchar_t p[NMC_PATH]) {
    CHECK(nmc_instance_dir(c,p));
    return nmc_join(p,p,L"instance.json")?OK:ERR(NMC_PATH_ERROR,L"instance config path");
}
NmcError nmc_instance_create(const NmcConfig *c) {
    wchar_t p[NMC_PATH]; CHECK(nmc_config_validate(c)); CHECK(instance_file(c,p));
    return write_config(c,p,false);
}
NmcError nmc_instance_remove(const NmcConfig *c) {
    wchar_t p[NMC_PATH]; uint64_t size;
    CHECK(instance_file(c,p));
    if (!nmc_file_size(p,&size)) return ERR(NMC_MISSING,L"instance registration");
    /* Keep game files and worlds recoverable; only remove the registration. */
    return DeleteFileW(p)?OK:SYS(NMC_FILE,L"remove instance registration");
}
NmcError nmc_instance_read(NmcConfig *c,const char *id) {
    NmcConfig tmp=*c; wchar_t p[NMC_PATH]; yyjson_doc *d=NULL;
    if (!nmc_copy(tmp.instance,sizeof(tmp.instance),id)) return ERR(NMC_PATH_ERROR,L"instance ID");
    CHECK(instance_file(&tmp,p)); CHECK(nmc_json(p,&d));
    NmcError e=read_instance_json(&tmp,yyjson_doc_get_root(d)); yyjson_doc_free(d);
    if (!e.code) *c=tmp;
    return e;
}
NmcError nmc_instances(const NmcConfig *c,NmcVersionInfo **out,size_t *count) {
    wchar_t p[NMC_PATH]; *out=NULL; *count=0;
    if (!nmc_join(p,c->root,L"instances\\*")) return ERR(NMC_PATH_ERROR,L"instance list path");
    WIN32_FIND_DATAW entry;
    HANDLE f=FindFirstFileW(p,&entry);
    if (f==INVALID_HANDLE_VALUE) {
        DWORD error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND ? OK : nmc_winerr(NMC_FILE,error,L"instance list");
    }
    size_t cap=0; NmcError e=OK;
    do {
        char id[NMC_ID]; NmcConfig tmp=*c;
        if (!(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
            !nmc_utf8(entry.cFileName,id,sizeof(id)) || !safe_id(id) || nmc_instance_read(&tmp,id).code ||
            !nmc_installed(&tmp)) continue;
        if (!nmc_grow((void **)out,&cap,*count+1,sizeof(**out))) { e=ERR(NMC_MEMORY,L"instance list"); break; }
        NmcVersionInfo *v=&(*out)[(*count)++];
        strcpy(v->id,id); strcpy(v->type,tmp.version);
    } while (FindNextFileW(f,&entry));
    FindClose(f);
    if (e.code) { free(*out); *out=NULL; *count=0; }
    return e;
}
void nmc_diagnostic(const NmcConfig *c,NmcError e) {
    wchar_t path[NMC_PATH], line[256]; SYSTEMTIME now;
    (void)c;
    if (!e.code || nmc_home(path).code || !nmc_join(path,path,L"last-error.txt")) return;
    GetLocalTime(&now);
    int n=swprintf(line,NMC_ARRAY(line),L"%04u-%02u-%02u %02u:%02u:%02u  code=%u detail=0x%08lX  %ls\r\n",
        (unsigned)now.wYear,(unsigned)now.wMonth,(unsigned)now.wDay,(unsigned)now.wHour,(unsigned)now.wMinute,
        (unsigned)now.wSecond,(unsigned)e.code,e.detail,e.where?e.where:L"-");
    char utf8[1024];
    if (n>0 && nmc_utf8(line,utf8,sizeof(utf8))) (void)nmc_atomic(path,utf8,strlen(utf8));
}

