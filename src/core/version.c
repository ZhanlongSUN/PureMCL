#include "internal.h"
#include <miniz.h>

static NmcError json_to_file(yyjson_doc *d,const wchar_t *path) {
    size_t n=0; char *json=yyjson_write(d,0,&n);
    if (!json) return ERR(NMC_MEMORY,L"serialize catalog");
    NmcError e=nmc_atomic(path,json,n); free(json); return e;
}
NmcError nmc_catalog(const NmcConfig *c,bool refresh,NmcTask *t,yyjson_doc **out) {
    wchar_t path[NMC_PATH]; *out=NULL;
    if (!nmc_path(path,c->root,"metadata/catalog.json")) return ERR(NMC_PATH_ERROR,L"catalog path");
    if (!refresh) return nmc_json(path,out);
    nmc_progress(t,NMC_STAGE_CATALOG,0,0);
    unsigned status=0; yyjson_doc *d=NULL;
    NmcError e=nmc_http_json(VERSION_MANIFEST,NULL,NULL,t,&status,&d);
    if (!e.code && (status!=200 || !yyjson_is_arr(yyjson_obj_get(yyjson_doc_get_root(d),"versions"))))
        e=(NmcError){status==200?NMC_JSON:NMC_HTTP,status,L"official version catalog"};
    if (!e.code) e=json_to_file(d,path);
    if (e.code) yyjson_doc_free(d);
    else *out=d;
    return e;
}
NmcError nmc_versions(const NmcConfig *c,bool refresh,NmcTask *t,NmcVersionInfo **out,size_t *count) {
    yyjson_doc *d=NULL; *out=NULL; *count=0; size_t cap=0;
    NmcError e=nmc_catalog(c,refresh,t,&d);
    if (e.code) return e;
    yyjson_val *v; size_t i,max;
    yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(d),"versions"),i,max,v) {
        const char *id=nmc_str(v,"id"), *type=nmc_str(v,"type");
        if (!nmc_id(id) || !type || strlen(type)>=sizeof((*out)[0].type)) continue;
        if (!nmc_grow((void **)out,&cap,*count+1,sizeof(**out))) { e=ERR(NMC_MEMORY,L"version list"); break; }
        strcpy((*out)[*count].id,id); strcpy((*out)[(*count)++].type,type);
    }
    yyjson_doc_free(d);
    if (e.code) { free(*out); *out=NULL; *count=0; }
    return e;
}
bool nmc_rules(yyjson_val *rules) {
    if (!rules) return true;
    if (!yyjson_is_arr(rules)) return false;
    yyjson_val *v; size_t i,max; bool allow=false;
    yyjson_arr_foreach(rules,i,max,v) {
        const char *action=nmc_str(v,"action");
        yyjson_val *os=yyjson_obj_get(v,"os"), *features=yyjson_obj_get(v,"features");
        if (!action || (strcmp(action,"allow") && strcmp(action,"disallow"))) continue;
        if (os) {
            if (!yyjson_is_obj(os)) continue;
            const char *name=nmc_str(os,"name"), *arch=nmc_str(os,"arch"), *version=nmc_str(os,"version");
            if (name && strcmp(name,"windows")) continue;
            if (arch && strcmp(arch,"x86_64") && strcmp(arch,"amd64") && strcmp(arch,"x64")) continue;
            /* The supported OS is Windows 11 (NT 10.0); accept known upstream patterns only. */
            if (version && strcmp(version,"^10\\.") && strcmp(version,"^10\\.0") &&
                strcmp(version,"^10\\.0.*") && strcmp(version,"10.0")) continue;
            yyjson_val *range=yyjson_obj_get(os,"versionRange");
            if (range) {
                /* This app runs on Windows 11. Read the build through the documented registry
                   value only for rules that actually compare Windows versions. */
                DWORD build=0, bytes=0; wchar_t build_str[32];
                bytes=sizeof(build_str);
                if (RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                    L"CurrentBuildNumber",RRF_RT_REG_SZ,NULL,build_str,&bytes)!=ERROR_SUCCESS) continue;
                build=wcstoul(build_str,NULL,10);
                const char *bounds[]={nmc_str(range,"min"),nmc_str(range,"max")};
                bool in_range=yyjson_is_obj(range);
                for (size_t j=0;j<NMC_ARRAY(bounds) && in_range;++j) if (bounds[j]) {
                    unsigned major=0,minor=0,b=0; char tail;
                    int parsed=sscanf(bounds[j],"%u.%u.%u%c",&major,&minor,&b,&tail);
                    if (parsed<2 || parsed>3 || major>100 || minor>100) { in_range=false; break; }
                    int cmp=major!=10?(major<10?1:-1):minor?-1:build==b?0:build>b?1:-1;
                    in_range=j==0?cmp>=0:cmp<=0;
                }
                if (!in_range) continue;
            }
        }
        bool match=true; yyjson_val *k,*value; size_t j,count;
        yyjson_obj_foreach(features,j,count,k,value) {
            (void)k;
            if (!yyjson_is_false(value)) { match=false; break; }
        }
        if (match) allow=!strcmp(action,"allow");
    }
    return allow;
}
static yyjson_val *entry(yyjson_doc *catalog,const char *id) {
    yyjson_val *v; size_t i,n;
    yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(catalog),"versions"),i,n,v) {
        const char *name=nmc_str(v,"id");
        if (name && !strcmp(id,name)) return v;
    }
    return NULL;
}
static NmcError version_doc(const NmcConfig *c,yyjson_doc *catalog,const char *id,
                            bool online,NmcTask *t,yyjson_doc **out) {
    char rel[NMC_ID*2+24]; wchar_t path[NMC_PATH]; *out=NULL;
    if (!nmc_id(id)) return ERR(NMC_JSON,L"version ID");
    snprintf(rel,sizeof(rel),"%s/%s.json",id,id);
    if (!nmc_path(path,c->versions_dir,rel)) return ERR(NMC_PATH_ERROR,L"version JSON path");
    yyjson_val *v=entry(catalog,id);
    const char *sha=nmc_str(v,"sha1"), *url=nmc_str(v,"url");
    if (!nmc_hex(sha,40) || !nmc_content_url(url)) return ERR(NMC_MISSING,L"version not in cached catalog");
    if (online) {
        char got[65];
        NmcError e=nmc_hash_file(path,40,got,t);
        if (e.code==NMC_CANCELLED) return e;
        if (e.code || strcmp(got,sha)) {
            /* Preserve raw bytes for the catalog hash. The catalog has no size field. */
            CHECK(nmc_http_meta(url,path,sha,t));
        }
    }
    CHECK(nmc_json(path,out));
    const char *actual=nmc_str(yyjson_doc_get_root(*out),"id");
    if (!actual || strcmp(actual,id)) { yyjson_doc_free(*out); *out=NULL; return ERR(NMC_JSON,L"version ID mismatch"); }
    return OK;
}
static yyjson_val *field(const NmcPlan *p,const char *key) {
    for (size_t i=0;i<p->count;++i) {
        yyjson_val *v=yyjson_obj_get(yyjson_doc_get_root(p->chain[i]),key);
        if (v) return v;
    }
    return NULL;
}
static const char *string_field(const NmcPlan *p,const char *key) {
    yyjson_val *v=field(p,key);
    const char *s=yyjson_get_str(v);
    return s && strlen(s)==yyjson_get_len(v) ? s : NULL;
}
static NmcError add_file(NmcFiles *files,const NmcFile *f,bool dedup) {
    if (dedup) for (size_t i=0;i<files->n;++i) {
        if (_wcsicmp(files->v[i].path,f->path)) continue;
        return files->v[i].size==f->size && !strcmp(files->v[i].hash,f->hash)
            ? OK : ERR(NMC_JSON,L"conflicting dependency paths");
    }
    if (!nmc_grow((void **)&files->v,&files->cap,files->n+1,sizeof(*f))) return ERR(NMC_MEMORY,L"file list");
    files->v[files->n++]=*f; return OK;
}
static NmcError add_cp(NmcArgs *a,const wchar_t *path) {
    for (size_t i=0;i<a->n;++i) if (!_wcsicmp(a->v[i],path)) return OK;
    return nmc_args_add(a,path)?OK:ERR(NMC_MEMORY,L"classpath");
}
static bool library_key(const char *name,char out[512]) {
    if (!name || !*name || strlen(name)>=512) return false;
    const char *a=strchr(name,':'), *b=a?strchr(a+1,':'):NULL;
    if (!a || !b || a==name || b==a+1 || !b[1]) return false;
    size_t n=(size_t)(b-name);
    const char *classifier=strchr(b+1,':');
    memcpy(out,name,n); out[n]=0;
    if (classifier) strcat(out,classifier);
    return true;
}
static NmcError libraries(const NmcConfig *c,NmcPlan *p) {
    wchar_t dir[NMC_PATH]; NmcArgs overridden={0}; NmcError e=OK;
    if (!nmc_join(dir,c->root,L"libraries")) return ERR(NMC_PATH_ERROR,L"library base");
    for (size_t layer=0;layer<p->count;++layer) {
        yyjson_val *lib; size_t i,n;
        yyjson_val *list=yyjson_obj_get(yyjson_doc_get_root(p->chain[layer]),"libraries");
        size_t prior=overridden.n;
        yyjson_arr_foreach(list,i,n,lib) {
            if (!nmc_rules(yyjson_obj_get(lib,"rules"))) continue;
            const char *name=nmc_str(lib,"name"); char key[512]; wchar_t wkey[512];
            if (!library_key(name,key) || !nmc_utf16(key,wkey,NMC_ARRAY(wkey))) {
                e=ERR(NMC_JSON,L"library coordinates"); goto done;
            }
            bool overridden_by_child=false;
            for (size_t j=0;j<prior;++j) if (!wcscmp(overridden.v[j],wkey)) { overridden_by_child=true; break; }
            if (overridden_by_child) continue;
            if (!nmc_args_add(&overridden,wkey)) { e=ERR(NMC_MEMORY,L"library overrides"); goto done; }
            yyjson_val *downloads=yyjson_obj_get(lib,"downloads"), *artifact=yyjson_obj_get(downloads,"artifact");
            const char *classifier=nmc_str(yyjson_obj_get(lib,"natives"),"windows");
            if (!artifact && !classifier) { e=ERR(NMC_UNSUPPORTED,L"library without verified download"); goto done; }
            NmcFile f={0};
            if (artifact) {
                TRY(nmc_file_meta(&f,dir,nmc_str(artifact,"path"),artifact));
                TRY(add_file(&p->files,&f,true)); TRY(add_cp(&p->classpath,f.path));
            }
            if (classifier) {
                char resolved[128];
                const char *subst=strstr(classifier,"\x24{arch}");
                if (subst) {
                    int wrote=snprintf(resolved,sizeof(resolved),"%.*s64%s",(int)(subst-classifier),classifier,subst+7);
                    if (wrote<0 || wrote>=(int)sizeof(resolved)) { e=ERR(NMC_JSON,L"native classifier"); goto done; }
                    classifier=resolved;
                }
                yyjson_val *native=yyjson_obj_get(yyjson_obj_get(downloads,"classifiers"),classifier);
                if (!native) { e=ERR(NMC_UNSUPPORTED,L"missing Windows x64 natives"); goto done; }
                TRY(nmc_file_meta(&f,dir,nmc_str(native,"path"),native));
                TRY(add_file(&p->files,&f,true)); TRY(add_file(&p->native_zips,&f,true));
            }
        }
    }
done:
    nmc_args_free(&overridden); return e;
}
NmcError nmc_plan(const NmcConfig *c,bool online,NmcTask *t,NmcPlan *p) {
    *p=(NmcPlan){0}; yyjson_doc *catalog=NULL;
    NmcError e=nmc_catalog(c,false,t,&catalog);
    if (online && (e.code==NMC_MISSING || e.code==NMC_JSON ||
                   (!e.code && !entry(catalog,c->version) && strcmp(c->version,"latest")))) {
        yyjson_doc_free(catalog); catalog=NULL;
        e=nmc_catalog(c,true,t,&catalog);
    }
    if (e.code) goto done;
    const char *id=c->version;
    if (!strcmp(id,"latest")) id=nmc_str(yyjson_obj_get(yyjson_doc_get_root(catalog),"latest"),"release");
    char ids[12][NMC_ID];
    while (id) {
        if (!nmc_id(id) || p->count==NMC_ARRAY(p->chain)) { e=ERR(NMC_JSON,L"version inheritance depth"); goto done; }
        for (size_t i=0;i<p->count;++i)
            if (!strcmp(id,ids[i])) { e=ERR(NMC_JSON,L"version inheritance cycle"); goto done; }
        strcpy(ids[p->count],id);
        TRY(version_doc(c,catalog,id,online,t,&p->chain[p->count]));
        yyjson_val *root=yyjson_doc_get_root(p->chain[p->count++]);
        id=nmc_str(root,"inheritsFrom");
        if (yyjson_obj_get(root,"inheritsFrom") && !id) { e=ERR(NMC_JSON,L"invalid parent version"); goto done; }
    }
    if (!p->count || !nmc_copy(p->main_class,sizeof(p->main_class),string_field(p,"mainClass")) ||
        !p->main_class[0] || !nmc_copy(p->type,sizeof(p->type),string_field(p,"type"))) {
        e=ERR(NMC_JSON,L"launch metadata"); goto done;
    }
    yyjson_val *java=field(p,"javaVersion"), *asset=field(p,"assetIndex");
    uint64_t major=java?yyjson_get_uint(yyjson_obj_get(java,"majorVersion")):8;
    const char *component=java?nmc_str(java,"component"):"jre-legacy";
    const char *asset_id=nmc_str(asset,"id");
    if (major<8 || major>100 || !nmc_id(component) || !nmc_id(asset_id)) {
        e=ERR(NMC_JSON,L"Java or assets metadata"); goto done;
    }
    p->java_major=(unsigned)major; strcpy(p->component,component); strcpy(p->asset_id,asset_id);
    char rel[NMC_ID*2+32];
    TRY(nmc_game_instance(c,ids[0],p->instance));
    snprintf(rel,sizeof(rel),"%s/natives",ids[0]);
    if (!nmc_join(p->game,p->instance,L"game") || !nmc_join(p->natives,p->instance,L"natives") ||
        !nmc_path(p->unpacked,c->versions_dir,rel) || !nmc_join(p->assets,c->root,L"assets")) {
        e=ERR(NMC_PATH_ERROR,L"game paths"); goto done;
    }
    snprintf(rel,sizeof(rel),"indexes/%s.json",asset_id);
    TRY(nmc_file_meta(&p->asset_index,p->assets,rel,asset));
    p->legacy_assets=!strcmp(asset_id,"legacy") || !strcmp(asset_id,"pre-1.6");
    p->map_assets=!strcmp(asset_id,"pre-1.6");
    TRY(libraries(c,p));
    yyjson_val *client=NULL; const char *jar=string_field(p,"jar");
    for (size_t i=0;i<p->count;++i) {
        if (jar && strcmp(jar,ids[i])) continue;
        client=yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(p->chain[i]),"downloads"),"client");
        if (client) { id=ids[i]; break; }
    }
    if (!client || !id) { e=ERR(NMC_JSON,L"client download"); goto done; }
    snprintf(rel,sizeof(rel),"%s/%s.jar",id,id);
    NmcFile f={0};
    TRY(nmc_file_meta(&f,c->versions_dir,rel,client));
    TRY(add_file(&p->files,&f,true)); TRY(add_cp(&p->classpath,f.path));
    yyjson_val *logging=yyjson_obj_get(field(p,"logging"),"client");
    if (logging) {
        yyjson_val *log=yyjson_obj_get(logging,"file"); const char *log_id=nmc_str(log,"id");
        if (!nmc_id(log_id)) { e=ERR(NMC_JSON,L"logging file ID"); goto done; }
        snprintf(rel,sizeof(rel),"assets/log_configs/%s",log_id);
        TRY(nmc_file_meta(&f,c->root,rel,log)); TRY(add_file(&p->files,&f,true));
    }
done:
    yyjson_doc_free(catalog);
    if (e.code) nmc_plan_free(p);
    return e;
}
void nmc_plan_free(NmcPlan *p) {
    for (size_t i=0;i<p->count;++i) yyjson_doc_free(p->chain[i]);
    free(p->files.v); free(p->native_zips.v); nmc_args_free(&p->classpath); *p=(NmcPlan){0};
}
static bool stamp_matches(const wchar_t *stamp,const char *hash) {
    NmcBytes b={0}; NmcError e=nmc_read(stamp,4096,&b);
    bool ok=!e.code && b.len==strlen(hash) && !memcmp(b.data,hash,b.len);
    free(b.data); return ok;
}
static bool asset_stamp(const NmcPlan *p,wchar_t out[NMC_PATH]) {
    char name[NMC_ID+24];
    snprintf(name,sizeof(name),".pure-assets-%s",p->asset_id);
    return nmc_path(out,p->game,name);
}
NmcError nmc_assets_ready(const NmcPlan *p) {
    wchar_t stamp[NMC_PATH], objects[NMC_PATH];
    if (!asset_stamp(p,stamp) || !nmc_join(objects,p->assets,L"objects"))
        return ERR(NMC_PATH_ERROR,L"asset receipt path");
    CHECK(nmc_check_file(&p->asset_index,false,NULL));
    DWORD attr=GetFileAttributesW(objects);
    return attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_DIRECTORY) &&
        !(attr&FILE_ATTRIBUTE_REPARSE_POINT) && stamp_matches(stamp,p->asset_index.hash)
        ? OK : ERR(NMC_MISSING,L"asset receipt or objects");
}
NmcError nmc_assets(const NmcConfig *c,const NmcPlan *p,bool verify,NmcTask *t) {
    wchar_t stamp[NMC_PATH], virtual_dir[NMC_PATH], resources[NMC_PATH];
    char rel[NMC_ID+16];
    snprintf(rel,sizeof(rel),"virtual/%s",p->asset_id);
    if (!nmc_path(virtual_dir,p->assets,rel) || !nmc_join(resources,p->game,L"resources") ||
        !asset_stamp(p,stamp)) return ERR(NMC_PATH_ERROR,L"asset paths");
    CHECK(nmc_fetch(&p->asset_index,true,t));
    if (!verify && !nmc_assets_ready(p).code) return OK;
    NmcFiles files={0}; yyjson_doc *doc=NULL; NmcError e=OK;
    TRY(nmc_json(p->asset_index.path,&doc));
    yyjson_val *root=yyjson_doc_get_root(doc), *objs=yyjson_obj_get(root,"objects");
    bool virt=yyjson_is_true(yyjson_obj_get(root,"virtual")), mapped=yyjson_is_true(yyjson_obj_get(root,"map_to_resources"));
    if (!yyjson_is_obj(objs) || yyjson_obj_size(objs)>100000) { e=ERR(NMC_JSON,L"asset index"); goto done; }
    wchar_t objects[NMC_PATH];
    if (!nmc_join(objects,p->assets,L"objects")) { e=ERR(NMC_PATH_ERROR,L"asset objects directory"); goto done; }
    TRY(nmc_mkdir(objects));
    yyjson_val *k,*v; size_t i,n;
    size_t processed=0;
    nmc_progress(t,NMC_STAGE_ASSETS,0,yyjson_obj_size(objs));
    yyjson_obj_foreach(objs,i,n,k,v) {
        if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"asset index"); goto done; }
        const char *hash=nmc_str(v,"hash"); yyjson_val *size=yyjson_obj_get(v,"size");
        if (!nmc_hex(hash,40) || !yyjson_is_uint(size) || yyjson_get_uint(size)>UINT64_C(4294967296)) {
            e=ERR(NMC_JSON,L"asset object"); goto done;
        }
        char asset[64]; NmcFile f={0};
        snprintf(asset,sizeof(asset),"objects/%.2s/%s",hash,hash);
        if (!nmc_path(f.path,p->assets,asset)) { e=ERR(NMC_PATH_ERROR,L"asset path"); goto done; }
        snprintf(f.url,sizeof(f.url),"https://resources.download.minecraft.net/%.2s/%s",hash,hash);
        strcpy(f.hash,hash); f.size=yyjson_get_uint(size);
        if (virt || mapped) {
            const char *name=yyjson_get_str(k); wchar_t dest[NMC_PATH];
            if (!name || strlen(name)!=yyjson_get_len(k)) { e=ERR(NMC_JSON,L"legacy asset name"); goto done; }
            TRY(nmc_fetch(&f,verify,t));
            const wchar_t *bases[]={virt?virtual_dir:NULL,mapped?resources:NULL};
            for (size_t j=0;j<NMC_ARRAY(bases);++j) if (bases[j]) {
                if (!nmc_path(dest,bases[j],name)) { e=ERR(NMC_PATH_ERROR,L"legacy asset destination"); goto done; }
                NmcFile copy=f; wcscpy(copy.path,dest);
                if (!nmc_check_file(&copy,true,t).code) continue;
                NmcBytes b={0}; TRY(nmc_read(f.path,64*1024*1024,&b));
                e=nmc_atomic(dest,b.data,b.len); free(b.data);
                if (e.code) goto done;
            }
        } else {
            TRY(add_file(&files,&f,true));
            if (files.n==256 || i+1==n) {
                TRY(nmc_batch_part(&files,c->jobs,verify,NMC_STAGE_ASSETS,t,processed,n));
                files.n=0;
                processed=i+1;
                nmc_progress(t,NMC_STAGE_ASSETS,processed,n);
            }
        }
        if ((virt || mapped) && ((i+1)%64==0 || i+1==n))
            nmc_progress(t,NMC_STAGE_ASSETS,i+1,n);
    }
    TRY(nmc_atomic(stamp,p->asset_index.hash,strlen(p->asset_index.hash)));
done:
    free(files.v); yyjson_doc_free(doc); return e;
}
NmcError nmc_natives(const NmcPlan *p,bool verify,NmcTask *t) {
    if (!p->native_zips.n) return nmc_mkdir(p->natives);
    wchar_t stamp[NMC_PATH]; NmcBytes hashes={0}, receipt={0}, previous={0};
    NmcArgs extracted={0};
    NmcError e=OK;
    if (!nmc_join(stamp,p->unpacked,L".pure-natives"))
        return ERR(NMC_PATH_ERROR,L"natives path");
    TRY(nmc_mkdir(p->unpacked));
    if (!nmc_bytes_add(&hashes,"PURE-NATIVES-2\n",15)) {
        e=ERR(NMC_MEMORY,L"natives receipt"); goto done;
    }
    for (size_t i=0;i<p->native_zips.n;++i) {
        const char *h=p->native_zips.v[i].hash;
        if (!nmc_bytes_add(&hashes,h,strlen(h)) || !nmc_bytes_add(&hashes,"\n",1)) {
            e=ERR(NMC_MEMORY,L"natives receipt"); goto done;
        }
    }
    if (!nmc_bytes_add(&hashes,"\n",1)) { e=ERR(NMC_MEMORY,L"natives receipt"); goto done; }
    if (!verify && !nmc_read(stamp,1024*1024,&previous).code &&
        previous.len>=hashes.len && !memcmp(previous.data,hashes.data,hashes.len)) {
        char *context=NULL; bool ready=true;
        for (char *part=strtok_s(previous.data+hashes.len,"\n",&context);part;part=strtok_s(NULL,"\n",&context)) {
            wchar_t file[NMC_PATH]; uint64_t n;
            char *colon=strchr(part,':');
            if (!colon) { ready=false; break; }
            *colon=0; char *end=NULL;
            uint64_t expected=strtoull(part,&end,10);
            if (!*part || *end || !nmc_path(file,p->unpacked,colon+1) || !nmc_file_size(file,&n) || n!=expected) {
                ready=false; break;
            }
        }
        if (ready) goto done;
    }
    if (!nmc_bytes_add(&receipt,hashes.data,hashes.len)) { e=ERR(NMC_MEMORY,L"natives receipt"); goto done; }
    nmc_progress(t,NMC_STAGE_NATIVES,0,p->native_zips.n);
    uint64_t expanded=0;
    for (size_t i=0;i<p->native_zips.n;++i) {
        NmcBytes zip={0}; mz_zip_archive ar={0};
        TRY(nmc_read(p->native_zips.v[i].path,256*1024*1024,&zip));
        if (!mz_zip_reader_init_mem(&ar,zip.data,zip.len,0)) {
            free(zip.data); e=ERR(NMC_ZIP,L"native archive open"); goto done;
        }
        for (mz_uint j=0;j<mz_zip_reader_get_num_files(&ar);++j) {
            mz_zip_archive_file_stat st; wchar_t dest[NMC_PATH]; char full[NMC_PATH];
            if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"native archive"); break; }
            if (!mz_zip_reader_file_stat(&ar,j,&st) || !mz_zip_reader_get_filename(&ar,j,full,sizeof(full)) ||
                mz_zip_reader_get_filename(&ar,j,NULL,0)>=sizeof(full)) {
                e=ERR(NMC_ZIP,L"native entry metadata"); break;
            }
            if (st.m_is_directory) continue;
            if (!nmc_path(dest,p->unpacked,full) || ((st.m_external_attr>>16)&0170000)==0120000) {
                e=ERR(NMC_ZIP,L"unsafe native archive entry"); break;
            }
            size_t len=strlen(full);
            if (len<4 || _stricmp(full+len-4,".dll") || !strncmp(full,"META-INF/",9)) continue;
            bool duplicate=false;
            for (size_t k=0;k<extracted.n;++k) if (!_wcsicmp(extracted.v[k],dest)) { duplicate=true; break; }
            if (duplicate) { e=ERR(NMC_ZIP,L"duplicate native destination"); break; }
            if (st.m_uncomp_size>64*1024*1024 || expanded+st.m_uncomp_size>256*1024*1024) {
                e=ERR(NMC_ZIP,L"native expansion limit"); break;
            }
            size_t n=0;
            void *data=mz_zip_reader_extract_to_heap(&ar,j,&n,0);
            if (!data) { e=ERR(NMC_ZIP,L"native entry extraction"); break; }
            /* Avoid replacing an identical DLL that another instance may be using. */
            NmcBytes current={0};
            bool unchanged=!nmc_read(dest,n,&current).code && current.len==n && !memcmp(current.data,data,n);
            free(current.data);
            if (!unchanged) e=nmc_atomic(dest,data,n);
            mz_free(data);
            if (e.code) break;
            char line[64];
            int prefix=snprintf(line,sizeof(line),"%zu:",n);
            if (!nmc_args_add(&extracted,dest) || prefix<0 ||
                !nmc_bytes_add(&receipt,line,(size_t)prefix) || !nmc_bytes_add(&receipt,full,len) ||
                !nmc_bytes_add(&receipt,"\n",1)) { e=ERR(NMC_MEMORY,L"native entries receipt"); break; }
            expanded+=n;
        }
        mz_zip_reader_end(&ar); free(zip.data);
        if (e.code) goto done;
        nmc_progress(t,NMC_STAGE_NATIVES,i+1,p->native_zips.n);
    }
    e=nmc_atomic(stamp,receipt.data,receipt.len);
done:
    free(hashes.data); free(receipt.data); free(previous.data); nmc_args_free(&extracted); return e;
}

