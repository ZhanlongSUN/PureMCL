#include "internal.h"

bool nmc_download_retryable(NmcError e) {
    return e.code==NMC_HASH || (e.code==NMC_HTTP &&
        (e.detail<400 || e.detail>=500 || e.detail==408 || e.detail==429));
}
static bool source_failure(NmcError e) { return e.code==NMC_HTTP || e.code==NMC_HASH; }
NmcError nmc_transfer_route(NmcTask *t,unsigned group,NmcDownloadRegion region,NmcRouteAttempt run,void *ctx) {
    if (!group || region!=NMC_REGION_MAINLAND) return run(ctx,false);
    atomic_ullong *retry_after=&t->mirror_retry_after[group-1];
    bool mirror=GetTickCount64()>=atomic_load(retry_after);
    NmcError first=run(ctx,mirror);
    if (!source_failure(first)) return first;
    if (nmc_cancelled(t)) return ERR(NMC_CANCELLED,L"download source selection");
    NmcError second=run(ctx,!mirror);
    if (!second.code) {
        if (!mirror) atomic_store(retry_after,0);
        else if (nmc_download_retryable(first) || first.detail==403)
            atomic_store(retry_after,GetTickCount64()+30000ULL);
    }
    if (source_failure(second) && !nmc_download_retryable(second) && nmc_download_retryable(first))
        return first;
    return second;
}
NmcError nmc_file_meta(NmcFile *f,const wchar_t *base,const char *path,yyjson_val *v) {
    const char *sha=nmc_str(v,"sha1"), *url=nmc_str(v,"url");
    yyjson_val *sz=yyjson_obj_get(v,"size");
    if (!sha) sha=nmc_str(v,"sha256");
    if ((!nmc_hex(sha,40)&&!nmc_hex(sha,64)) || !nmc_content_url(url) ||
        !yyjson_is_uint(sz) || yyjson_get_uint(sz)>UINT64_C(4294967296))
        return ERR(NMC_JSON,L"download metadata");
    if (!nmc_path(f->path,base,path)) return ERR(NMC_PATH_ERROR,L"download metadata path");
    strcpy(f->url,url); strcpy(f->hash,sha); f->size=yyjson_get_uint(sz);
    return OK;
}
NmcError nmc_fetch(const NmcFile *f,bool verify,NmcTask *t) {
    if (nmc_cancelled(t)) return ERR(NMC_CANCELLED,L"file check");
    NmcFile resolved;
    if (f->url[0] && !f->hash[0]) {
        if (!verify && !nmc_check_file(f,false,t).code) return OK;
        char checksum[2048];
        int n=snprintf(checksum,sizeof(checksum),"%s.sha1",f->url);
        if (n<0 || n>=(int)sizeof(checksum)) return ERR(NMC_HTTP,L"Maven checksum URL");
        resolved=*f;
        CHECK(nmc_http_digest(checksum,resolved.hash,t));
        f=&resolved;
    }
    NmcError e=nmc_check_file(f,verify,t);
    if (!e.code || (e.code!=NMC_HASH && e.code!=NMC_MISSING)) return e;
    if (!f->url[0]) return e;
    for (unsigned i=0;i<3;++i) {
        e=nmc_http_file(f,t);
        if (!nmc_download_retryable(e)) break;
        if (i==2) break;
        for (unsigned j=0;j<(3u<<i);++j) {
            if (nmc_cancelled(t)) return ERR(NMC_CANCELLED,L"download retry");
            Sleep(100);
        }
    }
    return e;
}
typedef struct {
    NmcDownloadItem handle;
    void *data;
    size_t count;
    atomic_size_t index, finished;
    atomic_bool failed;
    NmcError error;
    NmcTask *task;
    NmcStage stage;
    size_t percent;
    SRWLOCK report;
} Batch;
static DWORD WINAPI download_worker(void *ptr) {
    Batch *b=ptr;
    while (!atomic_load(&b->failed) && !nmc_cancelled(b->task)) {
        size_t i=atomic_fetch_add(&b->index,1);
        if (i>=b->count) break;
        NmcError e=b->handle(b->data,i,b->task);
        if (e.code) {
            if (!atomic_exchange(&b->failed,true)) {
                b->error=e;
                atomic_store(&b->task->stop_workers,true);
            }
            break;
        }
        AcquireSRWLockExclusive(&b->report);
        size_t done=atomic_fetch_add(&b->finished,1)+1;
        size_t percent=done*100/b->count;
        if (percent!=b->percent || b->count-done<16) {
            b->percent=percent;
            nmc_progress(b->task,b->stage,done,b->count);
        }
        ReleaseSRWLockExclusive(&b->report);
    }
    return 0;
}
NmcError nmc_parallel(size_t count,unsigned jobs,NmcStage stage,NmcTask *t,NmcDownloadItem action,void *data) {
    NmcTask local={0};
    if (!t) { nmc_task_reset(&local); t=&local; }
    bool own_session=!t->http_session;
    if (own_session) t->http_session=nmc_http_pool_open();
    Batch b={.handle=action,.data=data,.count=count,.task=t,.stage=stage,.report=SRWLOCK_INIT};
    HANDLE workers[15]; DWORD n=0;
    atomic_init(&b.index,0); atomic_init(&b.finished,0); atomic_init(&b.failed,false);
    nmc_progress(t,stage,0,count);
    while (n<NMC_ARRAY(workers) && n+1<jobs && n+1<count) {
        HANDLE thread=CreateThread(NULL,0,download_worker,&b,0,NULL);
        if (!thread) break;
        workers[n++]=thread;
    }
    download_worker(&b);
    if (n) WaitForMultipleObjects(n,workers,TRUE,INFINITE);
    for (DWORD i=0;i<n;++i) CloseHandle(workers[i]);
    if (own_session) { nmc_http_pool_close(t->http_session); t->http_session=NULL; }
    atomic_store(&t->stop_workers,false);
    if (b.error.code) return b.error;
    return nmc_cancelled(t)?ERR(NMC_CANCELLED,L"downloads"):OK;
}
typedef struct { NmcFiles *files; bool verify; } FileBatch;
static NmcError download_file(void *data,size_t index,NmcTask *t) {
    FileBatch *b=data;
    return nmc_fetch(&b->files->v[index],b->verify,t);
}
NmcError nmc_batch(NmcFiles *files,unsigned jobs,bool verify,NmcStage stage,NmcTask *t) {
    FileBatch b={files,verify};
    return nmc_parallel(files->n,jobs,stage,t,download_file,&b);
}

