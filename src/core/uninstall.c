#include "internal.h"

bool nmc_installed(const NmcConfig *c) {
    wchar_t instance[NMC_PATH], path[NMC_PATH];
    char relative[NMC_ID*2+8];
    uint64_t size;
    if (nmc_game_instance(c,c->version,instance).code) return false;
    snprintf(relative,sizeof(relative),"%s/%s.jar",c->version,c->version);
    if (!nmc_path(path,c->versions_dir,relative) || !nmc_file_size(path,&size) || !size) return false;
    snprintf(relative,sizeof(relative),"%s/%s.json",c->version,c->version);
    if (!nmc_path(path,c->versions_dir,relative) || !nmc_file_size(path,&size) || !size) return false;
    if (!nmc_join(path,instance,L"game")) return false;
    DWORD attrs=GetFileAttributesW(path);
    if (attrs==INVALID_FILE_ATTRIBUTES || !(attrs&FILE_ATTRIBUTE_DIRECTORY) || (attrs&FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    if (c->loader!=NMC_VANILLA &&
        (!nmc_join(path,instance,L".pure-loader.json") || !nmc_file_size(path,&size))) return false;
    if (nmc_join(path,instance,L".pure-installed") && nmc_file_size(path,&size)) return size==2;
    /* Installations made by older PureMCL builds used a per-game asset receipt. */
    if (c->loader!=NMC_VANILLA) return false;
    snprintf(relative,sizeof(relative),"%s/%s.json",c->version,c->version);
    yyjson_doc *doc=NULL;
    if (!nmc_path(path,c->versions_dir,relative) || nmc_json(path,&doc).code) return false;
    const char *asset=nmc_str(yyjson_obj_get(yyjson_doc_get_root(doc),"assetIndex"),"id");
    bool complete=false;
    if (nmc_id(asset)) {
        snprintf(relative,sizeof(relative),"game/.pure-assets-%s",asset);
        complete=nmc_path(path,instance,relative) && nmc_file_size(path,&size) && size==40;
    }
    yyjson_doc_free(doc); return complete;
}

NmcError nmc_uninstall(const NmcConfig *c,NmcTask *t) {
    CHECK(nmc_config_validate(c));
    HANDLE lock;
    CHECK(nmc_work_lock(c,&lock));
    wchar_t instance[NMC_PATH], mark[NMC_PATH], removed[NMC_PATH], base[NMC_PATH], files[NMC_PATH];
    NmcConfig saved=*c;
    NmcError e=OK;
    TRY(nmc_instance_read(&saved,c->instance));
    if (!nmc_installed(&saved)) { e=ERR(NMC_MISSING,L"没有找到已安装的实例"); goto done; }
    TRY(nmc_game_instance(&saved,saved.version,instance));
    if (!nmc_join(mark,instance,L".pure-process") ||
        swprintf(removed,NMC_PATH,L"%ls.removing-%lu-%llu",instance,GetCurrentProcessId(),GetTickCount64())<0) {
        e=ERR(NMC_PATH_ERROR,L"uninstall instance path"); goto done;
    }
    TRY(nmc_process_idle(mark));
    if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"uninstall"); goto done; }
    if (!MoveFileExW(instance,removed,MOVEFILE_WRITE_THROUGH)) { e=SYS(NMC_FILE,L"detach installed instance"); goto done; }
    e=nmc_instance_remove(&saved);
    if (e.code) { MoveFileExW(removed,instance,MOVEFILE_WRITE_THROUGH); goto done; }
    TRY(nmc_remove_tree(c->versions_dir,removed));
    if (!nmc_path(base,c->versions_dir,saved.version) || !nmc_join(files,base,L"instances"))
        goto done;
    if (RemoveDirectoryW(files)) {
        char name[NMC_ID+8];
        snprintf(name,sizeof(name),"%s.jar",saved.version);
        if (!nmc_path(files,base,name)) goto done;
        TRY(nmc_remove_tree(c->versions_dir,files));
        snprintf(name,sizeof(name),"%s.json",saved.version);
        if (nmc_path(files,base,name)) TRY(nmc_remove_tree(c->versions_dir,files));
        if (nmc_join(files,base,L"natives")) TRY(nmc_remove_tree(c->versions_dir,files));
        RemoveDirectoryW(base);
    }
done:
    CloseHandle(lock);
    return e;
}
