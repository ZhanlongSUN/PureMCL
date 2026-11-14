#include "internal.h"

typedef struct { const char *key; const wchar_t *value; } Variable;
static NmcError expand(NmcArgs *a,const char *s,const Variable *vars,size_t count) {
    if (!s || strlen(s)>32767) return ERR(NMC_JSON,L"argument text");
    int len=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,NULL,0);
    if (!len) return ERR(NMC_JSON,L"argument encoding");
    wchar_t *w=malloc((size_t)len*sizeof(wchar_t)); NmcWide result={0}; NmcError e=OK;
    if (!w) return ERR(NMC_MEMORY,L"argument text");
    if (!nmc_utf16(s,w,(size_t)len)) { e=ERR(NMC_JSON,L"argument encoding"); goto done; }
    const wchar_t *p=w;
    while (*p) {
        const wchar_t *next=wcsstr(p,L"\x24{");
        if (!next) {
            if (!nmc_wide_add(&result,p,wcslen(p))) e=ERR(NMC_MEMORY,L"expanded argument");
            break;
        }
        if (!nmc_wide_add(&result,p,(size_t)(next-p))) { e=ERR(NMC_MEMORY,L"expanded argument"); break; }
        const wchar_t *end=wcschr(next,L'}'); char key[80]; wchar_t name[80];
        size_t n=end?(size_t)(end-next-2):0;
        if (!n || n>=NMC_ARRAY(name)) { e=ERR(NMC_JSON,L"argument placeholder"); break; }
        wmemcpy(name,next+2,n); name[n]=0;
        if (!nmc_utf8(name,key,sizeof(key))) { e=ERR(NMC_JSON,L"argument placeholder encoding"); break; }
        const wchar_t *value=NULL;
        for (size_t i=0;i<count;++i) if (!strcmp(key,vars[i].key)) { value=vars[i].value; break; }
        if (!value) { e=ERR(NMC_UNSUPPORTED,L"unknown game argument placeholder"); break; }
        if (!nmc_wide_add(&result,value,wcslen(value))) { e=ERR(NMC_MEMORY,L"expanded argument"); break; }
        p=end+1;
    }
    if (!e.code && !nmc_args_add(a,result.data?result.data:L"")) e=ERR(NMC_MEMORY,L"argument list");
done:
    if (result.data) SecureZeroMemory(result.data,result.len*sizeof(wchar_t));
    free(result.data); free(w); return e;
}
static NmcError expand_val(NmcArgs *a,yyjson_val *v,const Variable *vars,size_t count) {
    const char *s=yyjson_get_str(v);
    return s && strlen(s)==yyjson_get_len(v)?expand(a,s,vars,count):ERR(NMC_JSON,L"argument value");
}
static NmcError expand_list(NmcArgs *a,yyjson_val *list,const Variable *vars,size_t count) {
    if (!list) return OK;
    if (!yyjson_is_arr(list)) return ERR(NMC_JSON,L"argument array");
    yyjson_val *v; size_t i,n;
    yyjson_arr_foreach(list,i,n,v) {
        if (yyjson_is_str(v)) { CHECK(expand_val(a,v,vars,count)); continue; }
        if (!yyjson_is_obj(v)) return ERR(NMC_JSON,L"argument rule object");
        if (!nmc_rules(yyjson_obj_get(v,"rules"))) continue;
        yyjson_val *value=yyjson_obj_get(v,"value"),*item; size_t j,m;
        if (yyjson_is_str(value)) { CHECK(expand_val(a,value,vars,count)); continue; }
        if (!yyjson_is_arr(value)) return ERR(NMC_JSON,L"argument rule value");
        yyjson_arr_foreach(value,j,m,item) { CHECK(expand_val(a,item,vars,count)); }
    }
    return OK;
}
static yyjson_val *field(const NmcPlan *p,const char *key) {
    for (size_t i=0;i<p->count;++i) {
        yyjson_val *v=yyjson_obj_get(yyjson_doc_get_root(p->chain[i]),key);
        if (v) return v;
    }
    return NULL;
}
static NmcError custom(NmcArgs *a,const wchar_t *lines) {
    while (*lines) {
        const wchar_t *e=wcschr(lines,L'\n'); size_t n=e?(size_t)(e-lines):wcslen(lines);
        while (n && (lines[n-1]==L'\r' || lines[n-1]==L' ' || lines[n-1]==L'\t')) --n;
        while (n && (*lines==L' ' || *lines==L'\t')) { ++lines; --n; }
        if (n) {
            wchar_t w[2048];
            if (n>=NMC_ARRAY(w) || *lines!=L'-') return ERR(NMC_PATH_ERROR,L"custom JVM argument");
            wmemcpy(w,lines,n); w[n]=0;
            if (!wcscmp(w,L"-jar") || !wcscmp(w,L"-cp") || !wcscmp(w,L"-classpath") ||
                !wcscmp(w,L"--class-path") || !wcsncmp(w,L"--class-path=",13) ||
                !wcscmp(w,L"-m") || !wcsncmp(w,L"--module",8) || !wcsncmp(w,L"-Xms",4) || !wcsncmp(w,L"-Xmx",4))
                return ERR(NMC_PATH_ERROR,L"use launcher memory / classpath settings");
            if (!nmc_args_add(a,w)) return ERR(NMC_MEMORY,L"custom JVM argument");
        }
        if (!e) break;
        lines=e+1;
    }
    return OK;
}
NmcError nmc_args_build(const NmcConfig *c,const NmcPlan *p,const NmcSession *s,NmcArgs *out) {
    wchar_t mem[48], name[64], uuid[33], access[8192], xuid[32], client[37], ver[NMC_ID], asset[NMC_ID],
        type[24], libraries[NMC_PATH], legacy[NMC_PATH], log[NMC_PATH]=L"";
    NmcWide cp={0}; NmcError e=OK;
    const char *id=nmc_str(yyjson_doc_get_root(p->chain[0]),"id"); char rel[128];
    if (!nmc_utf16(s->player.name,name,NMC_ARRAY(name)) || !nmc_utf16(s->player.id,uuid,NMC_ARRAY(uuid)) ||
        !nmc_utf16(s->token,access,NMC_ARRAY(access)) || !nmc_utf16(s->xuid,xuid,NMC_ARRAY(xuid)) ||
        !nmc_utf16(s->client,client,NMC_ARRAY(client)) || !nmc_utf16(id,ver,NMC_ARRAY(ver)) ||
        !nmc_utf16(p->asset_id,asset,NMC_ARRAY(asset)) || !nmc_utf16(p->type,type,NMC_ARRAY(type)) ||
        !nmc_join(libraries,c->root,L"libraries")) return ERR(NMC_JSON,L"launch variables");
    snprintf(rel,sizeof(rel),"virtual/%s",p->asset_id);
    if (!nmc_path(legacy,p->assets,rel)) { e=ERR(NMC_PATH_ERROR,L"legacy assets path"); goto done; }
    if (p->map_assets) {
        if (!nmc_join(legacy,p->game,L"resources")) { e=ERR(NMC_PATH_ERROR,L"mapped assets path"); goto done; }
    } else if (!p->legacy_assets) wcscpy(legacy,p->assets);
    for (size_t i=0;i<p->classpath.n;++i) {
        if ((i && !nmc_wide_add(&cp,L";",1)) || !nmc_wide_add(&cp,p->classpath.v[i],wcslen(p->classpath.v[i]))) {
            e=ERR(NMC_MEMORY,L"classpath string"); goto done;
        }
    }
    yyjson_val *logging=yyjson_obj_get(field(p,"logging"),"client");
    if (logging) {
        const char *file=nmc_str(yyjson_obj_get(logging,"file"),"id");
        if (!nmc_id(file)) { e=ERR(NMC_JSON,L"logging file"); goto done; }
        snprintf(rel,sizeof(rel),"assets/log_configs/%s",file);
        if (!nmc_path(log,c->root,rel)) { e=ERR(NMC_PATH_ERROR,L"logging path"); goto done; }
    }
    const Variable vars[]={
        {"auth_player_name",name},{"auth_uuid",uuid},{"auth_access_token",access},{"auth_session",access},
        {"auth_xuid",xuid},{"clientid",client},{"client_id",client},{"user_type",L"msa"},
        {"user_properties",L"{}"},{"version_name",ver},{"version_type",type},{"game_directory",p->game},
        {"assets_root",p->assets},{"assets_index_name",asset},{"game_assets",legacy},
        {"natives_directory",p->native_zips.n?p->unpacked:p->natives},
        {"library_directory",libraries},{"classpath",cp.data?cp.data:L""},{"classpath_separator",L";"},
        {"launcher_name",L"PureMCL"},{"launcher_version",L"0.1.0"},{"path",log}
    };
    swprintf(mem,NMC_ARRAY(mem),L"-Xms%um",c->xms);
    if (!nmc_args_add(out,mem)) goto memory;
    swprintf(mem,NMC_ARRAY(mem),L"-Xmx%um",c->xmx);
    if (!nmc_args_add(out,mem)) goto memory;
    if ((c->gc==1 && p->java_major<8) || (c->gc==2 && p->java_major<17)) {
        e=ERR(NMC_JAVA,L"GC incompatible with Java major version"); goto done;
    }
    if (c->gc && !nmc_args_add(out,c->gc==1?L"-XX:+UseG1GC":L"-XX:+UseZGC")) goto memory;
    TRY(custom(out,c->jvm));
    bool has_jvm=false;
    for (size_t i=p->count;i>0;--i) {
        yyjson_val *a=yyjson_obj_get(yyjson_doc_get_root(p->chain[i-1]),"arguments");
        yyjson_val *jvm=yyjson_obj_get(a,"jvm");
        if (jvm) { has_jvm=true; TRY(expand_list(out,jvm,vars,NMC_ARRAY(vars))); }
    }
    if (!has_jvm) {
        TRY(expand(out,"-Djava.library.path=\x24{natives_directory}",vars,NMC_ARRAY(vars)));
        if (!nmc_args_add(out,L"-cp") || !nmc_args_add(out,cp.data?cp.data:L"")) goto memory;
    }
    if (logging) TRY(expand(out,nmc_str(logging,"argument"),vars,NMC_ARRAY(vars)));
    wchar_t main_class[256];
    if (!nmc_utf16(p->main_class,main_class,NMC_ARRAY(main_class)) || !*main_class || *main_class==L'-' ||
        *main_class==L'@' || wcspbrk(main_class,L" \t\r\n")) { e=ERR(NMC_JSON,L"main class"); goto done; }
    if (!nmc_args_add(out,main_class)) goto memory;
    const char *old=yyjson_get_str(field(p,"minecraftArguments"));
    if (old) {
        /* Mojang's legacy metadata separates placeholders by spaces, before substitution. */
        for (const char *cur=old;*cur;) {
            while (*cur==' ') ++cur;
            if (!*cur) break;
            const char *end=strchr(cur,' '); size_t n=end?(size_t)(end-cur):strlen(cur);
            char word[4096];
            if (n>=sizeof(word)) { e=ERR(NMC_JSON,L"legacy argument"); goto done; }
            memcpy(word,cur,n); word[n]=0;
            TRY(expand(out,word,vars,NMC_ARRAY(vars))); cur+=n;
        }
    }
    for (size_t i=p->count;i>0;--i) {
        yyjson_val *a=yyjson_obj_get(yyjson_doc_get_root(p->chain[i-1]),"arguments");
        TRY(expand_list(out,yyjson_obj_get(a,"game"),vars,NMC_ARRAY(vars)));
    }
    goto done;
memory:
    e=ERR(NMC_MEMORY,L"launch argument list");
done:
    SecureZeroMemory(access,sizeof(access)); free(cp.data);
    if (e.code) nmc_args_free(out);
    return e;
}

