#include "internal.h"
#include <miniz.h>

#define FABRIC_META "https://meta.fabricmc.net/v2/versions/loader/"
#define FORGE_MAVEN "https://maven.minecraftforge.net/"
#define NEO_MAVEN "https://maven.neoforged.net/releases/"

static NmcError get_json(const char *url,NmcTask *t,yyjson_doc **doc) {
    unsigned status=0;
    NmcError e=nmc_http_json(url,NULL,NULL,t,&status,doc);
    if (status && status!=200) return (NmcError){NMC_HTTP,status,L"loader metadata HTTP status"};
    return e;
}
static bool coordinate(const char *raw,char path[800]) {
    if (!raw || strlen(raw)>=400) return false;
    char copy[400]; strcpy(copy,raw);
    char *ext=strchr(copy,'@');
    if (ext) { *ext++=0; if (!nmc_id(ext)) return false; } else ext="jar";
    char *group=copy,*artifact=strchr(group,':');
    if (!artifact) return false;
    *artifact++=0;
    char *version=strchr(artifact,':');
    if (!version) return false;
    *version++=0;
    char *classifier=strchr(version,':');
    if (classifier) *classifier++=0;
    if (!nmc_id(group) || !nmc_id(artifact) || !nmc_id(version) || (classifier && !nmc_id(classifier))) return false;
    for (char *q=group;*q;++q) if (*q=='.') *q='/';
    int n=snprintf(path,800,"%s/%s/%s/%s-%s%s%s.%s",group,artifact,version,artifact,version,
                   classifier?"-":"",classifier?classifier:"",ext);
    return n>0 && n<800;
}
NmcError nmc_loader_library(yyjson_val *lib,const wchar_t *base,NmcFile *f) {
    char rel[800];
    const char *name=nmc_str(lib,"name");
    yyjson_val *artifact=yyjson_obj_get(yyjson_obj_get(lib,"downloads"),"artifact");
    const char *path=nmc_str(artifact,"path"),*url=nmc_str(artifact,"url"),*hash=nmc_str(artifact,"sha1");
    if (!path) {
        if (!coordinate(name,rel)) return ERR(NMC_JSON,L"loader Maven coordinates");
        path=rel;
    }
    if (!nmc_path(f->path,base,path)) return ERR(NMC_PATH_ERROR,L"loader library path");
    if (!artifact) {
        url=nmc_str(lib,"url");
        if (!url && name && strncmp(name,"optifine:",9)) url="https://libraries.minecraft.net/";
        if (url && *url) {
            size_t n=strlen(url);
            int written=snprintf(f->url,sizeof(f->url),"%s%s%s",url,url[n-1]=='/'?"":"/",path);
            if (written<0 || written>=(int)sizeof(f->url)) return ERR(NMC_HTTP,L"loader library URL");
        }
    } else if (url && !nmc_copy(f->url,sizeof(f->url),url)) return ERR(NMC_HTTP,L"loader library URL");
    if (f->url[0] && !nmc_content_url(f->url)) return ERR(NMC_HTTP,L"untrusted loader repository");
    if (hash && *hash && strspn(hash,"0")!=strlen(hash)) {
        if (!nmc_hex(hash,40)) return ERR(NMC_HASH,L"loader library hash");
        strcpy(f->hash,hash);
    }
    yyjson_val *size=yyjson_obj_get(artifact,"size");
    f->size=yyjson_is_uint(size) && yyjson_get_uint(size)>0 ? yyjson_get_uint(size) : NMC_SIZE_UNKNOWN;
    return OK;
}
static const char *loader_name(NmcLoader loader) {
    static const char *names[]={"","fabric","forge","neoforge","optifine"};
    return names[loader];
}
static bool profile_matches(yyjson_doc *d,NmcLoader loader,const char *base) {
    yyjson_val *r=yyjson_doc_get_root(d);
    const char *id=nmc_str(r,"id"),*parent=nmc_str(r,"inheritsFrom"),*main=nmc_str(r,"mainClass");
    if (!nmc_id(id) || !parent || strcmp(parent,base) || !main || !*main) return false;
    char lower[NMC_ID]; strcpy(lower,id); _strlwr(lower);
    if (loader==NMC_FORGE && strstr(lower,"neoforge")) return false;
    return strstr(lower,loader_name(loader))!=NULL;
}
static NmcError fabric_profile(const NmcConfig *c,const char *game,char chosen[NMC_ID],NmcTask *t,yyjson_doc **d) {
    char url[512]; NmcError e=OK; yyjson_doc *versions=NULL;
    if (c->loader_version[0]) strcpy(chosen,c->loader_version);
    else {
        snprintf(url,sizeof(url),FABRIC_META "%s",game);
        TRY(get_json(url,t,&versions));
        yyjson_val *item; size_t i,n;
        yyjson_arr_foreach(yyjson_doc_get_root(versions),i,n,item) {
            yyjson_val *loader=yyjson_obj_get(item,"loader");
            const char *v=nmc_str(loader,"version");
            if (nmc_id(v) && yyjson_is_true(yyjson_obj_get(loader,"stable"))) { strcpy(chosen,v); break; }
        }
        if (!chosen[0]) { e=ERR(NMC_MISSING,L"没有找到兼容的 Fabric 加载器"); goto done; }
    }
    snprintf(url,sizeof(url),FABRIC_META "%s/%s/profile/json",game,chosen);
    e=get_json(url,t,d);
done:
    yyjson_doc_free(versions); return e;
}
static NmcError preferred_loader(const NmcConfig *c,const char *game,char out[NMC_ID],NmcTask *t) {
    if (c->loader_version[0]) { strcpy(out,c->loader_version); return OK; }
    yyjson_doc *d=NULL; NmcError e=OK; char key[NMC_ID+16];
    if (c->loader==NMC_FORGE) {
        TRY(get_json("https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json",t,&d));
        yyjson_val *promos=yyjson_obj_get(yyjson_doc_get_root(d),"promos");
        snprintf(key,sizeof(key),"%s-recommended",game);
        const char *v=nmc_str(promos,key);
        if (!nmc_id(v)) { snprintf(key,sizeof(key),"%s-latest",game); v=nmc_str(promos,key); }
        if (nmc_id(v)) strcpy(out,v);
    } else {
        unsigned major=0,minor=0,patch=0; char tail;
        int parts=sscanf(game,"%u.%u.%u%c",&major,&minor,&patch,&tail);
        if ((parts==2 || parts==3) && ((major==1 && minor>=20) || major>=26)) {
            if (major==1) snprintf(key,sizeof(key),"%u.%u.",minor,patch);
            else snprintf(key,sizeof(key),"%u.%u.%u.",major,minor,patch);
            TRY(get_json("https://maven.neoforged.net/api/maven/versions/releases/net/neoforged/neoforge",t,&d));
            yyjson_val *versions=yyjson_obj_get(yyjson_doc_get_root(d),"versions");
            char beta[NMC_ID]={0};
            for (size_t i=yyjson_arr_size(versions);i>0;--i) {
                const char *v=yyjson_get_str(yyjson_arr_get(versions,i-1));
                if (!nmc_id(v) || strncmp(v,key,strlen(key))) continue;
                if (!strchr(v,'-')) { strcpy(out,v); break; }
                if (!beta[0]) strcpy(beta,v);
            }
            if (!out[0] && beta[0]) strcpy(out,beta);
        }
    }
    if (!out[0]) e=ERR(NMC_MISSING,L"未找到加载器版本，请填写版本号");
done:
    yyjson_doc_free(d); return e;
}
static NmcError find_profile(const wchar_t *versions,NmcLoader loader,const char *game,yyjson_doc **out) {
    wchar_t find_path[NMC_PATH],file[NMC_PATH]; WIN32_FIND_DATAW data;
    if (!nmc_join(find_path,versions,L"*")) return ERR(NMC_PATH_ERROR,L"installer versions");
    HANDLE find=FindFirstFileW(find_path,&data);
    if (find==INVALID_HANDLE_VALUE) return SYS(NMC_FILE,L"installer output missing");
    NmcError e=ERR(NMC_UNSUPPORTED,L"安装器没有生成兼容的客户端配置；可手动导入版本 JSON");
    do {
        char id[NMC_ID],rel[NMC_ID*2+12]; yyjson_doc *doc=NULL;
        if (!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
            !nmc_utf8(data.cFileName,id,sizeof(id)) || !nmc_id(id)) continue;
        snprintf(rel,sizeof(rel),"%s/%s.json",id,id);
        if (!nmc_path(file,versions,rel) || nmc_json(file,&doc).code) continue;
        if (profile_matches(doc,loader,game)) { *out=doc; e=OK; break; }
        yyjson_doc_free(doc);
    } while (FindNextFileW(find,&data));
    FindClose(find); return e;
}
static NmcError jar_json(mz_zip_archive *zip,const char *name,yyjson_doc **out) {
    int entry=mz_zip_reader_locate_file(zip,name,NULL,MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (entry<0) return ERR(NMC_UNSUPPORTED,L"此旧版安装器需要手动导入版本 JSON");
    mz_zip_archive_file_stat info;
    if (!mz_zip_reader_file_stat(zip,(mz_uint)entry,&info) || info.m_uncomp_size>JSON_LIMIT)
        return ERR(NMC_ZIP,L"installer metadata");
    NmcBytes b={0};
    b.data=mz_zip_reader_extract_to_heap(zip,(mz_uint)entry,&b.len,0);
    if (!b.data) return ERR(NMC_ZIP,L"installer metadata");
    NmcError e=nmc_json_mem(&b,out); mz_free(b.data); return e;
}
static NmcError installer_file(NmcFiles *files,const NmcFile *f) {
    for (size_t i=0;i<files->n;++i) if (!_wcsicmp(files->v[i].path,f->path)) {
        return !strcmp(files->v[i].hash,f->hash)?OK:ERR(NMC_JSON,L"installer library hash conflict");
    }
    if (!nmc_grow((void **)&files->v,&files->cap,files->n+1,sizeof(*files->v)))
        return ERR(NMC_MEMORY,L"installer dependencies");
    files->v[files->n++]=*f; return OK;
}
static NmcError prefetch_installer(const NmcConfig *c,const NmcPlan *p,const wchar_t *path,NmcTask *t) {
    NmcBytes bytes={0}; mz_zip_archive zip={0}; yyjson_doc *profile=NULL,*version=NULL;
    NmcFiles files={0}; NmcError e=OK; bool open=false;
    TRY(nmc_read(path,128u<<20,&bytes));
    open=mz_zip_reader_init_mem(&zip,bytes.data,bytes.len,0)!=0;
    if (!open) { e=ERR(NMC_ZIP,L"installer JAR"); goto done; }
    TRY(jar_json(&zip,"install_profile.json",&profile));
    yyjson_val *r=yyjson_doc_get_root(profile);
    const char *game=nmc_str(r,"minecraft"), *name=nmc_str(r,"json");
    if (!game || strcmp(p->base_version,game) || !name || *name!='/' || !name[1]) {
        e=ERR(NMC_UNSUPPORTED,L"此安装器未提供兼容的客户端版本，请手动导入版本 JSON"); goto done;
    }
    TRY(jar_json(&zip,name+1,&version));
    if (!profile_matches(version,c->loader,p->base_version)) { e=ERR(NMC_JSON,L"installer version profile"); goto done; }
    for (unsigned layer=0;layer<2;++layer) {
        yyjson_val *list=yyjson_obj_get(layer?yyjson_doc_get_root(version):r,"libraries"),*lib; size_t i,n;
        if (yyjson_arr_size(list)>1024) { e=ERR(NMC_JSON,L"too many installer libraries"); goto done; }
        yyjson_arr_foreach(list,i,n,lib) {
            if (!nmc_rules(yyjson_obj_get(lib,"rules"))) continue;
            NmcFile f={0}; TRY(nmc_loader_library(lib,p->loader_libs,&f));
            /* Empty URLs are embedded in the installer or produced by its processors. */
            if (f.url[0]) TRY(installer_file(&files,&f));
        }
    }
    const char *mappings=nmc_str(yyjson_obj_get(yyjson_obj_get(r,"data"),"MOJMAPS"),"client");
    if (mappings && *mappings=='[') {
        char coord[400], rel[800]; size_t n=strlen(mappings);
        yyjson_val *v=yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(p->chain[0]),"downloads"),"client_mappings");
        if (n<3 || n>=sizeof(coord) || mappings[n-1]!=']' || !v) {
            e=ERR(NMC_JSON,L"installer client mappings"); goto done;
        }
        memcpy(coord,mappings+1,n-2); coord[n-2]=0;
        if (!coordinate(coord,rel)) { e=ERR(NMC_JSON,L"installer mappings coordinate"); goto done; }
        NmcFile f={0}; TRY(nmc_file_meta(&f,p->loader_libs,rel,v)); TRY(installer_file(&files,&f));
    }
    nmc_progress(t,NMC_STAGE_LOADER,0,0);
    TRY(nmc_batch(&files,c->jobs,true,NMC_STAGE_LOADER,t));
done:
    yyjson_doc_free(profile); yyjson_doc_free(version);
    if (open) mz_zip_reader_end(&zip);
    free(files.v); free(bytes.data); return e;
}
static NmcError install_jar(const NmcConfig *c,NmcPlan *p,const wchar_t *workspace,
                           char chosen[NMC_ID],NmcTask *t,yyjson_doc **out) {
    CHECK(preferred_loader(c,p->base_version,chosen,t));
    wchar_t java[NMC_PATH],profiles[NMC_PATH],versions[NMC_PATH],dest[NMC_PATH],src[NMC_PATH];
    char rel[512]; NmcFile installer={0};
    const char *game=p->base_version;
    NmcError e=OK; NmcArgs args={0};
    if (!nmc_join(versions,workspace,L"versions") || !nmc_join(profiles,workspace,L"launcher_profiles.json"))
        return ERR(NMC_PATH_ERROR,L"installer staging");
    TRY(nmc_remove_tree(p->instance,versions));
    TRY(nmc_atomic(profiles,"{\"profiles\":{}}",15));
    snprintf(rel,sizeof(rel),"%s/%s.jar",game,game);
    if (!nmc_path(dest,versions,rel) || !nmc_path(src,c->versions_dir,rel)) return ERR(NMC_PATH_ERROR,L"vanilla installer JAR");
    TRY(nmc_fetch(&p->files.v[0],true,t));
    TRY(nmc_copy_file(src,dest,t));
    snprintf(rel,sizeof(rel),"%s/%s.json",game,game);
    if (!nmc_path(dest,versions,rel) || !nmc_path(src,c->versions_dir,rel)) return ERR(NMC_PATH_ERROR,L"vanilla installer JSON");
    TRY(nmc_copy_file(src,dest,t));
    if (c->loader==NMC_FORGE)
        snprintf(installer.url,sizeof(installer.url),FORGE_MAVEN "net/minecraftforge/forge/%s-%s/forge-%s-%s-installer.jar",
                 game,chosen,game,chosen);
    else
        snprintf(installer.url,sizeof(installer.url),NEO_MAVEN "net/neoforged/neoforge/%s/neoforge-%s-installer.jar",chosen,chosen);
    snprintf(rel,sizeof(rel),"installers/%s-%s-%s.jar",loader_name(c->loader),game,chosen);
    if (!nmc_path(installer.path,c->root,rel)) return ERR(NMC_PATH_ERROR,L"loader installer path");
    installer.size=NMC_SIZE_UNKNOWN;
    TRY(nmc_fetch(&installer,true,t));
    TRY(prefetch_installer(c,p,installer.path,t));
    TRY(nmc_runtime(c,p,true,false,t,java));
    if (!nmc_args_add(&args,L"-Djava.awt.headless=true") || !nmc_args_add(&args,L"-jar") ||
        !nmc_args_add(&args,installer.path) || !nmc_args_add(&args,L"--offline") ||
        !nmc_args_add(&args,L"--installClient") || !nmc_args_add(&args,workspace)) {
        e=ERR(NMC_MEMORY,L"loader installer arguments"); goto done;
    }
    nmc_progress(t,NMC_STAGE_LOADER,0,0);
    TRY(nmc_run_helper(java,workspace,&args,t));
    TRY(find_profile(versions,c->loader,game,out));
done:
    nmc_args_free(&args);
    /* The outer launcher already keeps the original JAR and version metadata. */
    (void)nmc_remove_tree(p->instance,versions);
    return e;
}
static NmcError import_profile(const NmcConfig *c,NmcPlan *p,NmcTask *t,yyjson_doc **d) {
    CHECK(nmc_json(c->loader_profile,d));
    if (!profile_matches(*d,c->loader,p->base_version)) return ERR(NMC_JSON,L"所选加载器配置与游戏版本不匹配");
    wchar_t source[NMC_PATH]; wcscpy(source,c->loader_profile);
    for (unsigned i=0;i<3;++i) {
        wchar_t *slash=wcsrchr(source,L'\\');
        if (!slash || slash-source<3) return ERR(NMC_PATH_ERROR,L"请选择 versions 目录下的加载器 JSON");
        *slash=0;
    }
    if (!nmc_join(source,source,L"libraries")) return ERR(NMC_PATH_ERROR,L"profile library root");
    yyjson_val *list=yyjson_obj_get(yyjson_doc_get_root(*d),"libraries"),*lib; size_t i,n;
    yyjson_arr_foreach(list,i,n,lib) {
        if (!nmc_rules(yyjson_obj_get(lib,"rules"))) continue;
        NmcFile dest={0},from={0};
        CHECK(nmc_loader_library(lib,p->loader_libs,&dest));
        CHECK(nmc_loader_library(lib,source,&from));
        if (!nmc_check_file(&dest,true,t).code) continue;
        if (!nmc_check_file(&from,true,t).code) CHECK(nmc_copy_file(from.path,dest.path,t));
        else if (!dest.url[0]) return ERR(NMC_MISSING,L"所选配置缺少本地加载器库");
    }
    return OK;
}
static NmcError write_profile(yyjson_doc *profile,NmcLoader loader,const char *version,const wchar_t *path) {
    yyjson_mut_doc *d=yyjson_doc_mut_copy(profile,NULL);
    if (!d) return ERR(NMC_MEMORY,L"loader profile");
    yyjson_mut_val *r=yyjson_mut_doc_get_root(d);
    NmcError e=ERR(NMC_MEMORY,L"loader profile");
    if (yyjson_mut_obj_add_uint(d,r,"pureLoader",loader) && yyjson_mut_obj_add_strcpy(d,r,"pureLoaderVersion",version)) {
        size_t n=0; char *bytes=yyjson_mut_write(d,0,&n);
        if (bytes) { e=nmc_atomic(path,bytes,n); free(bytes); }
    }
    yyjson_mut_doc_free(d); return e;
}
NmcError nmc_loader_plan(const NmcConfig *c,bool online,NmcTask *t,NmcPlan *p) {
    if (c->loader==NMC_VANILLA) return OK;
    wchar_t path[NMC_PATH],workspace[NMC_PATH],mark[NMC_PATH];
    if (p->count>=NMC_ARRAY(p->chain) || !nmc_join(path,p->instance,L".pure-loader.json") ||
        !nmc_join(workspace,p->instance,L"loader"))
        return ERR(NMC_PATH_ERROR,L"loader profile location");
    if (c->loader==NMC_FABRIC) {
        if (!nmc_join(p->loader_libs,c->root,L"libraries")) return ERR(NMC_PATH_ERROR,L"Fabric libraries");
    } else if (!nmc_join(p->loader_libs,workspace,L"libraries")) return ERR(NMC_PATH_ERROR,L"loader libraries");
    yyjson_doc *d=NULL;
    NmcError e=nmc_json(path,&d);
    if (!e.code) {
        yyjson_val *r=yyjson_doc_get_root(d);
        const char *stored=nmc_str(r,"pureLoaderVersion");
        if (yyjson_get_uint(yyjson_obj_get(r,"pureLoader"))!=(uint64_t)c->loader ||
            !profile_matches(d,c->loader,p->base_version) || !stored ||
            (c->loader_version[0] && strcmp(stored,c->loader_version))) {
            e=ERR(NMC_BUSY,L"该实例已有其他加载器，请使用新的实例名"); goto done;
        }
        if (!nmc_copy(p->loader_version,sizeof(p->loader_version),stored)) {
            e=ERR(NMC_JSON,L"loader version"); goto done;
        }
    } else {
        if (!online || e.code!=NMC_MISSING) goto done;
        if (!nmc_join(mark,p->instance,L".pure-process")) { e=ERR(NMC_PATH_ERROR,L"loader process path"); goto done; }
        TRY(nmc_process_idle(mark));
        TRY(nmc_mkdir(workspace));
        nmc_progress(t,NMC_STAGE_LOADER,0,0);
        if (c->loader_profile[0]) {
            TRY(import_profile(c,p,t,&d));
            if (c->loader_version[0]) strcpy(p->loader_version,c->loader_version);
        } else if (c->loader==NMC_FABRIC) {
            TRY(fabric_profile(c,p->base_version,p->loader_version,t,&d));
        } else if (c->loader==NMC_OPTIFINE) {
            e=ERR(NMC_MISSING,L"请导入官方 OptiFine 安装器生成的版本 JSON"); goto done;
        } else TRY(install_jar(c,p,workspace,p->loader_version,t,&d));
        if (!profile_matches(d,c->loader,p->base_version)) { e=ERR(NMC_JSON,L"loader profile mismatch"); goto done; }
        TRY(write_profile(d,c->loader,p->loader_version,path));
    }
    const char *main=nmc_str(yyjson_doc_get_root(d),"mainClass");
    if (!nmc_copy(p->main_class,sizeof(p->main_class),main)) { e=ERR(NMC_JSON,L"loader main class"); goto done; }
    memmove(p->chain+1,p->chain,p->count*sizeof(p->chain[0]));
    ++p->count; p->chain[0]=d; d=NULL;
done:
    yyjson_doc_free(d);
    return e;
}
