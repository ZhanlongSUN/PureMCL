#include "internal.h"

static bool repairable(NmcError e) {
    return e.code==NMC_MISSING || e.code==NMC_HASH || e.code==NMC_JAVA || e.code==NMC_JSON;
}
static NmcError ready(const NmcPlan *p,NmcTask *t) {
    for (size_t i=0;i<p->files.n;++i) {
        if (nmc_cancelled(t)) return ERR(NMC_CANCELLED,L"check local files");
        CHECK(nmc_check_file(p->files.v+i,false,t));
    }
    return nmc_assets_ready(p);
}
static NmcError fallback_classpath(const wchar_t *inst,NmcArgs *args,const NmcPlan *p) {
    for (size_t i=0;i+1<args->n;++i)
        if (!wcscmp(args->v[i],L"-cp") || !wcscmp(args->v[i],L"-classpath") || !wcscmp(args->v[i],L"--class-path")) {
            wchar_t path[NMC_PATH];
            CHECK(nmc_classpath_jar(inst,&p->classpath,path));
            wchar_t *copy=_wcsdup(path);
            if (!copy) return ERR(NMC_MEMORY,L"classpath bridge");
            free(args->v[i+1]); args->v[i+1]=copy; return OK;
        }
    return ERR(NMC_UNSUPPORTED,L"long command has no explicit classpath");
}
NmcError nmc_run(const NmcConfig *c,NmcAction action,NmcTask *t,unsigned long *pid) {
    if (pid) *pid=0;
    CHECK(nmc_config_validate(c));
    if (action==NMC_PLAY) {
        CHECK(nmc_auth_configured());
        if (!c->account[0]) return ERR(NMC_AUTH,L"请先选择已验证的微软账号。");
    }
    NmcTask local={0};
    if (!t) { nmc_task_reset(&local); t=&local; }
    NmcPlan p={0}; NmcSession s={0}; NmcArgs args={0};
    wchar_t lock_path[NMC_PATH], game_mark[NMC_PATH], java[NMC_PATH];
    HANDLE lock=INVALID_HANDLE_VALUE; NmcError e=OK;
    if (!nmc_join(lock_path,c->root,L".pure-work.lock"))
        return ERR(NMC_PATH_ERROR,L"workspace / process lock paths");
    TRY(nmc_lock(lock_path,&lock));
    bool maintenance=action!=NMC_PLAY, repair=action==NMC_REPAIR;
    if (!maintenance) {
        e=nmc_plan(c,false,t,&p);
        if (!e.code) e=ready(&p,t);
        if (!e.code) e=nmc_runtime(c,&p,false,false,t,java);
        if (e.code) {
            if (!repairable(e)) goto done;
            nmc_plan_free(&p); maintenance=true;
        }
    }
    if (maintenance) {
        TRY(nmc_plan(c,true,t,&p));
    }
    if (!nmc_join(game_mark,p.instance,L".pure-process")) { e=ERR(NMC_PATH_ERROR,L"game process lock path"); goto done; }
    TRY(nmc_process_idle(game_mark));
    if (maintenance) {
        TRY(nmc_batch(&p.files,c->jobs,true,NMC_STAGE_FILES,t));
        TRY(nmc_assets(c,&p,true,t));
        TRY(nmc_runtime(c,&p,true,repair,t,java));
    }
    TRY(nmc_mkdir(p.game));
    TRY(nmc_natives(&p,repair,t));
    if (action!=NMC_PLAY) {
        NmcConfig pinned=*c;
        const char *actual=nmc_str(yyjson_doc_get_root(p.chain[0]),"id");
        if (!nmc_copy(pinned.version,sizeof(pinned.version),actual)) { e=ERR(NMC_JSON,L"installed version"); goto done; }
        TRY(nmc_instance_create(&pinned));
        goto done;
    }
    TRY(nmc_session(c,t,&s));
    TRY(nmc_args_build(c,&p,&s,&args));
    if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"launch"); goto done; }
    nmc_progress(t,NMC_STAGE_LAUNCH,0,0);
    e=nmc_spawn(java,p.game,&args,game_mark,pid);
    if (e.code==NMC_UNSUPPORTED) {
        TRY(fallback_classpath(p.instance,&args,&p));
        TRY(nmc_spawn(java,p.game,&args,game_mark,pid));
    }
done:
    SecureZeroMemory(&s,sizeof(s)); nmc_args_free(&args); nmc_plan_free(&p);
    if (lock!=INVALID_HANDLE_VALUE) CloseHandle(lock);
    return e;
}

