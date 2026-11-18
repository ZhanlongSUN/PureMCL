#include "internal.h"

/* Read only through the Windows archive engine; never give it an extraction path. */
typedef struct {
    HMODULE module;
    void *(__cdecl *create)(void);
    int (__cdecl *open)(void *,const wchar_t *,size_t);
    int (__cdecl *next)(void *,void **);
    SSIZE_T (__cdecl *read)(void *,void *,size_t);
    int (__cdecl *close)(void *);
    int (__cdecl *skip)(void *);
    const wchar_t *(__cdecl *path)(void *), *(__cdecl *hardlink)(void *), *(__cdecl *symlink)(void *);
    unsigned (__cdecl *kind)(void *);
    int64_t (__cdecl *size)(void *);
} Archive;

#define PACK_TOTAL (32ULL<<30)
#define PACK_FILE (8ULL<<30)
#define PACK_ENTRIES 250000
#define PACK_REG 0100000u
#define PACK_DIR 0040000u

static NmcError archive_api(Archive *a) {
    a->module=LoadLibraryExW(L"archiveint.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!a->module) return SYS(NMC_UNSUPPORTED,L"Windows 压缩包组件不可用");
#define IMPORT(field,name) do { FARPROC p=GetProcAddress(a->module,name); \
    if (!p) return SYS(NMC_UNSUPPORTED,L"Windows 压缩包组件缺少所需接口"); \
    _Static_assert(sizeof(p)==sizeof(a->field),"Windows function pointer size"); \
    memcpy(&a->field,&p,sizeof(p)); } while (0)
    IMPORT(create,"archive_read_new"); IMPORT(open,"archive_read_open_filename_w");
    IMPORT(next,"archive_read_next_header"); IMPORT(read,"archive_read_data");
    IMPORT(close,"archive_read_free"); IMPORT(skip,"archive_read_data_skip");
    IMPORT(path,"archive_entry_pathname_w"); IMPORT(hardlink,"archive_entry_hardlink_w");
    IMPORT(symlink,"archive_entry_symlink_w"); IMPORT(kind,"archive_entry_filetype");
    IMPORT(size,"archive_entry_size");
#undef IMPORT
    return OK;
}
static NmcError open_pack(const Archive *a,const wchar_t *path,void **out) {
    static const char *const codecs[]={
        "archive_read_support_format_zip", "archive_read_support_format_rar", "archive_read_support_format_rar5",
        "archive_read_support_format_7zip", "archive_read_support_format_tar",
        "archive_read_support_filter_gzip", "archive_read_support_filter_bzip2",
        "archive_read_support_filter_xz", "archive_read_support_filter_zstd"
    };
    *out=a->create();
    if (!*out) return ERR(NMC_MEMORY,L"archive reader");
    for (size_t i=0;i<NMC_ARRAY(codecs);++i) {
        FARPROC p=GetProcAddress(a->module,codecs[i]); int (__cdecl *enable)(void *);
        if (p) { memcpy(&enable,&p,sizeof(p)); (void)enable(*out); }
    }
    return a->open(*out,path,65536)==0?OK:ERR(NMC_ZIP,L"无法打开整合包");
}
static bool member_path(const wchar_t *path,wchar_t output[NMC_PATH],bool dir) {
    if (!path || wcslen(path)>=NMC_PATH) return false;
    char utf8[NMC_PATH*3];
    if (!nmc_utf8(path,utf8,sizeof(utf8))) return false;
    for (char *p=utf8;*p;++p) if (*p=='\\') *p='/';
    const char *name=utf8;
    if (!strncmp(name,"./",2)) name+=2;
    size_t n=strlen(name);
    if (dir && n && name[n-1]=='/') utf8[(name-utf8)+--n]=0;
    if (dir && (!strcmp(name,".") || (!n && name!=utf8))) { *output=0; return true; }
    return nmc_relative(name,output);
}
static bool segment(const wchar_t *path,const wchar_t *word) {
    size_t n=wcslen(word); return !_wcsnicmp(path,word,n) && (path[n]==0 || path[n]==L'\\');
}
static bool allowed_destination(const wchar_t *p) {
    if (!*p) return false;
    do {
        if (!_wcsnicmp(p,L".pure-",6)) return false;
        p=wcschr(p,L'\\');
        if (p) ++p;
    } while (p);
    return true;
}
static bool game_path(const wchar_t *path) {
    static const wchar_t *const roots[]={L"mods",L"config",L"defaultconfigs",L"resourcepacks",L"shaderpacks",
        L"saves",L"kubejs",L"scripts",L"datapacks",L"schematics",L"patchouli_books",L"journeymap",
        L"local",L"global_packs",L"openloader",L"options.txt",L"optionsof.txt",L"servers.dat"};
    for (size_t i=0;i<NMC_ARRAY(roots);++i) if (segment(path,roots[i])) return true;
    return false;
}
static const wchar_t *plain_target(const wchar_t *rel,const wchar_t *top) {
    if (top[0] && segment(rel,top) && rel[wcslen(top)]==L'\\') rel+=wcslen(top)+1;
    if (segment(rel,L"overrides") || segment(rel,L".minecraft")) {
        const wchar_t *slash=wcschr(rel,L'\\'); if (!slash) return NULL; rel=slash+1;
    }
    return game_path(rel)?rel:NULL;
}
static NmcError read_member(Archive *a,void *r,uint64_t size,HANDLE file,NmcBytes *text,NmcTask *t) {
    char buffer[65536]; uint64_t count=0;
    for (;;) {
        if (nmc_cancelled(t)) return ERR(NMC_CANCELLED,L"archive import");
        SSIZE_T n=a->read(r,buffer,sizeof(buffer));
        if (n<0 || (uint64_t)n>size-count) return ERR(NMC_ZIP,L"archive entry data");
        if (!n) return count==size?OK:ERR(NMC_ZIP,L"archive entry length");
        if (text && !nmc_bytes_add(text,buffer,(size_t)n)) return ERR(NMC_MEMORY,L"pack manifest");
        if (file!=INVALID_HANDLE_VALUE) CHECK(nmc_write(file,buffer,(size_t)n));
        count+=(size_t)n;
    }
}
static NmcError extract(Archive *a,void *r,uint64_t size,const wchar_t *target,NmcTask *t) {
    wchar_t tmp[NMC_PATH]; HANDLE file;
    CHECK(nmc_temp(target,tmp,&file));
    NmcError e=read_member(a,r,size,file,NULL,t);
    CloseHandle(file);
    if (!e.code) e=nmc_promote(tmp,target);
    if (e.code) DeleteFileW(tmp);
    return e;
}

typedef enum { SCAN, PLAIN, OVERRIDES, CLIENT } Pass;
typedef struct { wchar_t top[256]; bool top_set, multiple, curse; NmcBytes index; } Pack;
static NmcError walk(Archive *a,const wchar_t *source,Pass pass,Pack *pack,const wchar_t *game,NmcTask *t) {
    void *r=NULL; NmcError e; uint64_t total=0; size_t entries=0, extracted=0;
    TRY(open_pack(a,source,&r));
    for (;;) {
        if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"archive import"); break; }
        void *entry; int rc=a->next(r,&entry);
        if (rc==1) break;
        if (rc!=0 || ++entries>PACK_ENTRIES) { e=ERR(NMC_ZIP,L"archive header or entry limit"); break; }
        unsigned kind=a->kind(entry); bool dir=kind==PACK_DIR;
        wchar_t rel[NMC_PATH], target[NMC_PATH];
        if ((kind!=PACK_REG && !dir) || a->hardlink(entry) || a->symlink(entry) ||
            !member_path(a->path(entry),rel,dir)) {
            e=ERR(NMC_PATH_ERROR,L"整合包含有不安全的路径或链接"); break;
        }
        if (dir) { if (a->skip(r)!=0) e=ERR(NMC_ZIP,L"archive directory"); if (e.code) break; continue; }
        int64_t bytes=a->size(entry);
        if (bytes<0 || (uint64_t)bytes>PACK_FILE || (uint64_t)bytes>PACK_TOTAL-total) {
            e=ERR(NMC_ZIP,L"整合包超出解压大小限制"); break;
        }
        total+=(uint64_t)bytes;
        if (pass==SCAN) {
            const wchar_t *sep=wcschr(rel,L'\\'); size_t n=sep?(size_t)(sep-rel):wcslen(rel);
            if (sep && !plain_target(rel,L"") && plain_target(sep+1,L"")) {
                if (!pack->top_set) { wmemcpy(pack->top,rel,n); pack->top[n]=0; pack->top_set=true; }
                else if (n!=wcslen(pack->top) || _wcsnicmp(pack->top,rel,n)) pack->multiple=true;
            }
            if (!wcscmp(rel,L"modrinth.index.json")) {
                if (pack->index.data || !bytes || bytes>JSON_LIMIT) { e=ERR(NMC_ZIP,L"Modrinth index"); break; }
                TRY(read_member(a,r,(uint64_t)bytes,INVALID_HANDLE_VALUE,&pack->index,t));
                continue;
            }
            const wchar_t *name= wcsrchr(rel,L'\\'); name=name?name+1:rel;
            if (!_wcsicmp(name,L"manifest.json") && bytes>0 && bytes<=JSON_LIMIT) {
                NmcBytes b={0}; yyjson_doc *d=NULL;
                e=read_member(a,r,(uint64_t)bytes,INVALID_HANDLE_VALUE,&b,t);
                if (!e.code && !nmc_json_mem(&b,&d).code) {
                    const char *type=nmc_str(yyjson_doc_get_root(d),"manifestType");
                    if (type && !strcmp(type,"minecraftModpack")) pack->curse=true;
                }
                yyjson_doc_free(d); free(b.data); if (e.code) break;
                continue;
            }
        } else {
            const wchar_t *tail=NULL;
            if (pass==PLAIN) {
                tail=plain_target(rel,L"");
                if (!tail && !pack->multiple) tail=plain_target(rel,pack->top);
            } else {
                const wchar_t *prefix=pass==OVERRIDES?L"overrides":L"client-overrides";
                size_t n=wcslen(prefix);
                if (segment(rel,prefix) && rel[n]==L'\\') tail=rel+n+1;
            }
            if (tail) {
                if (!allowed_destination(tail) || !nmc_join(target,game,tail)) {
                    e=ERR(NMC_PATH_ERROR,L"整合包目标路径无效"); break;
                }
                TRY(extract(a,r,(uint64_t)bytes,target,t)); ++extracted;
                continue;
            }
        }
        if (a->skip(r)!=0) { e=ERR(NMC_ZIP,L"archive member data"); break; }
    }
    if (!e.code && pass==PLAIN && !extracted) e=ERR(NMC_UNSUPPORTED,L"未找到整合包的 mods、config 或其他游戏文件");
done:
    if (r) a->close(r);
    return e;
}

typedef struct { yyjson_val **items; size_t count; const wchar_t *game; } ModFiles;
static NmcError mod_file(yyjson_val *v,const wchar_t *game,NmcFile *f) {
    yyjson_val *hashes=yyjson_obj_get(v,"hashes"), *size=yyjson_obj_get(v,"fileSize"), *urls=yyjson_obj_get(v,"downloads");
    const char *sha1=nmc_str(hashes,"sha1"), *sha512=nmc_str(hashes,"sha512");
    wchar_t relative[NMC_PATH];
    if (!nmc_hex(sha1,40) || !nmc_hex(sha512,128) ||
        !yyjson_is_uint(size) || yyjson_get_uint(size)>PACK_FILE || !yyjson_arr_size(urls) ||
        !nmc_relative(nmc_str(v,"path"),relative) || !allowed_destination(relative) ||
        !nmc_join(f->path,game,relative))
        return ERR(NMC_JSON,L"Modrinth 文件路径、哈希或大小无效");
    f->size=yyjson_get_uint(size); f->pack=true;
    strcpy(f->hash,sha1); strcpy(f->hash512,sha512);
    size_t i,m; yyjson_val *url;
    yyjson_arr_foreach(urls,i,m,url) {
        const char *s=yyjson_get_str(url);
        if (!s || strlen(s)!=yyjson_get_len(url) || strlen(s)>=sizeof(f->url) || !nmc_pack_url(s))
            return ERR(NMC_HTTP,L"Modrinth 下载源不在允许范围内");
    }
    return OK;
}
static NmcError mod_download(void *ctx,size_t i,NmcTask *t) {
    ModFiles *m=ctx; yyjson_val *v=m->items[i]; NmcFile f={0};
    CHECK(mod_file(v,m->game,&f));
    yyjson_val *url; size_t index,count; NmcError e=ERR(NMC_HTTP,L"Modrinth 下载源");
    yyjson_arr_foreach(yyjson_obj_get(v,"downloads"),index,count,url) {
        strcpy(f.url,yyjson_get_str(url)); e=nmc_fetch(&f,true,t);
        if (!e.code || e.code==NMC_CANCELLED) break;
    }
    return e;
}
static NmcError mrpack_config(yyjson_val *root,NmcConfig *c,ModFiles *files) {
    const char *game=nmc_str(root,"game");
    if (!game || strcmp(game,"minecraft") || yyjson_get_uint(yyjson_obj_get(root,"formatVersion"))!=1)
        return ERR(NMC_UNSUPPORTED,L"仅支持 Minecraft 的 Modrinth 整合包格式 1");
    yyjson_val *deps=yyjson_obj_get(root,"dependencies"), *ls=yyjson_obj_get(root,"files");
    const char *version=nmc_str(deps,"minecraft");
    if (!nmc_id(version) || !nmc_copy(c->version,sizeof(c->version),version) || !yyjson_is_arr(ls) ||
        yyjson_arr_size(ls)>PACK_ENTRIES)
        return ERR(NMC_JSON,L"Modrinth Minecraft 版本或文件列表");
    c->loader=NMC_VANILLA; c->loader_version[0]=0; c->loader_profile[0]=0;
    static const char *const names[]={"","fabric-loader","forge","neoforge"};
    size_t i,n; yyjson_val *key,*value;
    yyjson_obj_foreach(deps,i,n,key,value) {
        const char *name=yyjson_get_str(key);
        if (strlen(name)!=yyjson_get_len(key)) return ERR(NMC_JSON,L"Modrinth loader key");
        if (!strcmp(name,"minecraft")) continue;
        unsigned j;
        for (j=1;j<NMC_ARRAY(names) && strcmp(names[j],name);++j) {}
        const char *v=yyjson_get_str(value);
        if (j==NMC_ARRAY(names) || c->loader!=NMC_VANILLA)
            return ERR(NMC_UNSUPPORTED,L"整合包含有不受支持的加载器");
        if (!nmc_id(v) || strlen(v)!=yyjson_get_len(value)) return ERR(NMC_JSON,L"Modrinth loader version");
        c->loader=(NmcLoader)j; strcpy(c->loader_version,v);
    }
    files->items=calloc(yyjson_arr_size(ls)+1,sizeof(*files->items));
    if (!files->items) return ERR(NMC_MEMORY,L"Modrinth files list");
    yyjson_arr_foreach(ls,i,n,value) {
        const char *client=nmc_str(yyjson_obj_get(value,"env"),"client");
        if (client && !strcmp(client,"unsupported")) continue;
        files->items[files->count++]=value;
    }
    return OK;
}
static int compare_paths(const void *a,const void *b) {
    return CompareStringOrdinal(*(wchar_t *const *)a,-1,*(wchar_t *const *)b,-1,TRUE)-CSTR_EQUAL;
}
static NmcError preflight(const ModFiles *files,const wchar_t *game) {
    wchar_t **paths=calloc(files->count+1,sizeof(*paths));
    if (!paths) return ERR(NMC_MEMORY,L"Modrinth paths");
    NmcError e=OK; uint64_t total=0;
    for (size_t i=0;i<files->count;++i) {
        NmcFile f={0}; TRY(mod_file(files->items[i],game,&f));
        if (f.size>PACK_TOTAL-total) { e=ERR(NMC_ZIP,L"Modrinth 下载总大小超出限制"); goto done; }
        total+=f.size;
        paths[i]=_wcsdup(f.path);
        if (!paths[i]) { e=ERR(NMC_MEMORY,L"Modrinth path"); goto done; }
    }
    qsort(paths,files->count,sizeof(*paths),compare_paths);
    for (size_t i=1;i<files->count;++i) if (!compare_paths(paths+i-1,paths+i)) {
        e=ERR(NMC_PATH_ERROR,L"Modrinth 文件路径重复"); break;
    }
done:
    for (size_t i=0;i<files->count;++i) free(paths[i]);
    free(paths); return e;
}

NmcError nmc_import_pack(const NmcConfig *c,const wchar_t *source,NmcTask *t) {
    CHECK(nmc_config_validate(c));
    uint64_t size;
    if (!nmc_file_size(source,&size) || !size) return ERR(NMC_MISSING,L"整合包文件");
    if (size>PACK_TOTAL) return ERR(NMC_ZIP,L"整合包文件过大");
    HANDLE input=CreateFileW(source,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (input==INVALID_HANDLE_VALUE) return SYS(NMC_FILE,L"打开整合包");
    Archive a={0}; Pack pack={0}; NmcConfig config=*c; NmcPlan p={0}; ModFiles files={0};
    yyjson_doc *index=NULL; HANDLE lock=INVALID_HANDLE_VALUE; NmcError e=OK;
    bool cleanup=false;
    wchar_t instance[NMC_PATH], game[NMC_PATH];
    nmc_progress(t,NMC_STAGE_PACK,0,0);
    TRY(archive_api(&a));
    TRY(walk(&a,source,SCAN,&pack,NULL,t));
    if (pack.curse) { e=ERR(NMC_UNSUPPORTED,L"此 CurseForge 包需从官方客户端导出实际游戏文件后再导入"); goto done; }
    const wchar_t *ext=wcsrchr(source,L'.');
    if (pack.index.data) {
        TRY(nmc_json_mem(&pack.index,&index));
        TRY(mrpack_config(yyjson_doc_get_root(index),&config,&files));
    } else if (ext && !_wcsicmp(ext,L".mrpack")) {
        e=ERR(NMC_JSON,L"缺少 modrinth.index.json"); goto done;
    }
    if (!strcmp(config.version,"latest")) {
        yyjson_doc *catalog=NULL;
        e=nmc_catalog(&config,false,t,&catalog);
        if (e.code==NMC_MISSING || e.code==NMC_JSON) {
            yyjson_doc_free(catalog); catalog=NULL; e=nmc_catalog(&config,true,t,&catalog);
        }
        if (e.code) { yyjson_doc_free(catalog); goto done; }
        const char *v=nmc_str(yyjson_obj_get(yyjson_doc_get_root(catalog),"latest"),"release");
        bool valid=nmc_id(v) && nmc_copy(config.version,sizeof(config.version),v);
        yyjson_doc_free(catalog);
        if (!valid) { e=ERR(NMC_JSON,L"latest release"); goto done; }
    }
    TRY(nmc_game_instance(&config,config.version,instance));
    if (!nmc_join(game,instance,L"game")) { e=ERR(NMC_PATH_ERROR,L"pack instance game path"); goto done; }
    if (index) TRY(preflight(&files,game));
    TRY(nmc_work_lock(&config,&lock));
    NmcConfig existing=config;
    e=nmc_instance_read(&existing,config.instance);
    if (!e.code) {
        e=ERR(NMC_BUSY,L"请为整合包选择一个新的实例名"); goto done;
    }
    if (e.code!=NMC_MISSING) goto done;
    if (GetFileAttributesW(instance)!=INVALID_FILE_ATTRIBUTES) {
        e=ERR(NMC_BUSY,L"请为整合包选择一个新的实例名"); goto done;
    }
    DWORD missing=GetLastError();
    if (missing!=ERROR_FILE_NOT_FOUND && missing!=ERROR_PATH_NOT_FOUND) {
        e=nmc_winerr(NMC_FILE,missing,L"pack instance directory"); goto done;
    }
    cleanup=true;
    TRY(nmc_install_locked(&config,t,&p));
    nmc_progress(t,NMC_STAGE_PACK,0,0);
    if (index) {
        files.game=p.game;
        TRY(nmc_parallel(files.count,config.jobs,NMC_STAGE_PACK,t,mod_download,&files));
        TRY(walk(&a,source,OVERRIDES,&pack,p.game,t));
        TRY(walk(&a,source,CLIENT,&pack,p.game,t));
    } else TRY(walk(&a,source,PLAIN,&pack,p.game,t));
    if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"pack import"); goto done; }
    TRY(nmc_mark_installed(&config,&p));
    cleanup=false;
done:
    if (cleanup) (void)nmc_remove_tree(config.versions_dir,instance);
    if (lock!=INVALID_HANDLE_VALUE) CloseHandle(lock);
    if (a.module) FreeLibrary(a.module);
    CloseHandle(input); nmc_plan_free(&p); yyjson_doc_free(index); free(files.items); free(pack.index.data);
    return e;
}
