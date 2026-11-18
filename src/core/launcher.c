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
NmcError nmc_work_lock(const NmcConfig *c,HANDLE *lock) {
    wchar_t path[NMC_PATH];
    if (!nmc_join(path,c->root,L".pure-work.lock")) return ERR(NMC_PATH_ERROR,L"workspace lock path");
    return nmc_lock(path,lock);
}
static NmcError instance_idle(const NmcPlan *p,wchar_t mark[NMC_PATH]) {
    if (!nmc_join(mark,p->instance,L".pure-process")) return ERR(NMC_PATH_ERROR,L"game process lock path");
    return nmc_process_idle(mark);
}
static NmcError prepare(const NmcConfig *c,NmcPlan *p,bool repair,NmcTask *t,wchar_t java[NMC_PATH]) {
    CHECK(nmc_batch(&p->files,c->jobs,true,NMC_STAGE_FILES,t));
    CHECK(nmc_assets(c,p,repair,t));
    return nmc_runtime(c,p,true,repair,t,java);
}
NmcError nmc_install_locked(const NmcConfig *c,NmcTask *t,NmcPlan *p) {
    wchar_t java[NMC_PATH], mark[NMC_PATH];
    CHECK(nmc_plan(c,true,t,p));
    CHECK(instance_idle(p,mark));
    CHECK(prepare(c,p,false,t,java));
    CHECK(nmc_mkdir(p->game));
    return nmc_natives(p,false,t);
}
NmcError nmc_mark_installed(const NmcConfig *c,const NmcPlan *p) {
    NmcConfig pinned=*c;
    wchar_t receipt[NMC_PATH];
    if (!nmc_copy(pinned.version,sizeof(pinned.version),p->base_version) ||
        !nmc_join(receipt,p->instance,L".pure-installed"))
        return ERR(NMC_PATH_ERROR,L"installation receipt");
    pinned.loader_profile[0]=0;
    if (p->loader_version[0]) strcpy(pinned.loader_version,p->loader_version);
    CHECK(nmc_atomic(receipt,"1\n",2));
    return nmc_instance_create(&pinned);
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
    wchar_t game_mark[NMC_PATH], java[NMC_PATH];
    HANDLE lock=INVALID_HANDLE_VALUE, game=NULL; NmcError e=OK;
    TRY(nmc_work_lock(c,&lock));
    if (action==NMC_INSTALL) {
        NmcConfig previous=*c;
        e=nmc_instance_read(&previous,c->instance);
        if (!e.code && ((strcmp(previous.version,"latest") && _stricmp(previous.version,c->version)) ||
            previous.loader!=c->loader ||
            (c->loader_version[0] && strcmp(previous.loader_version,c->loader_version)))) {
            e=ERR(NMC_BUSY,L"此实例名已绑定版本和加载器，请使用新的实例名"); goto done;
        }
        if (e.code && e.code!=NMC_MISSING) goto done;
        e=OK;
    }
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
    TRY(instance_idle(&p,game_mark));
    if (maintenance) TRY(prepare(c,&p,repair,t,java));
    TRY(nmc_mkdir(p.game));
    TRY(nmc_natives(&p,repair,t));
    if (action!=NMC_PLAY) {
        TRY(nmc_mark_installed(c,&p));
        goto done;
    }
    TRY(nmc_session(c,t,&s));
    TRY(nmc_args_build(c,&p,&s,&args));
    if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"launch"); goto done; }
    nmc_progress(t,NMC_STAGE_LAUNCH,0,0);
    HANDLE *child=c->track_logs?&game:NULL;
    e=nmc_spawn(java,p.game,&args,game_mark,pid,child);
    if (e.code==NMC_UNSUPPORTED) {
        TRY(fallback_classpath(p.instance,&args,&p));
        TRY(nmc_spawn(java,p.game,&args,game_mark,pid,child));
    }
    if (!e.code && game) {
        t->watch_error=nmc_start_watch(game,p.game,NULL);
        if (t->watch_error.code) nmc_diagnostic(c,t->watch_error);
    }
done:
    if (game) CloseHandle(game);
    SecureZeroMemory(&s,sizeof(s)); nmc_args_free(&args); nmc_plan_free(&p);
    if (lock!=INVALID_HANDLE_VALUE) CloseHandle(lock);
    return e;
}

