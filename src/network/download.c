#include "internal.h"

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
    NmcError e=nmc_check_file(f,verify,t);
    if (!e.code || (e.code!=NMC_HASH && e.code!=NMC_MISSING)) return e;
    for (unsigned i=0;i<3;++i) {
        e=nmc_http_file(f,t);
        if (e.code!=NMC_HTTP && e.code!=NMC_HASH) break;
        if (e.detail>=400 && e.detail<500 && e.detail!=408 && e.detail!=429) break;
        if (i==2) break;
        for (unsigned j=0;j<(3u<<i);++j) {
            if (nmc_cancelled(t)) return ERR(NMC_CANCELLED,L"download retry");
            Sleep(100);
        }
    }
    return e;
}
typedef struct {
    NmcFiles *files;
    atomic_size_t index, finished;
    atomic_bool failed;
    NmcError error;
    NmcTask *task;
    bool verify;
    NmcStage stage;
    size_t offset,total;
    SRWLOCK report;
} Batch;
static DWORD WINAPI download_worker(void *ptr) {
    Batch *b=ptr;
    while (!atomic_load(&b->failed) && !nmc_cancelled(b->task)) {
        size_t i=atomic_fetch_add(&b->index,1);
        if (i>=b->files->n) break;
        NmcError e=nmc_fetch(b->files->v+i,b->verify,b->task);
        if (e.code) {
            if (!atomic_exchange(&b->failed,true)) b->error=e;
            break;
        }
        AcquireSRWLockExclusive(&b->report);
        size_t done=atomic_fetch_add(&b->finished,1)+1;
        if (done==b->files->n || done%16==0) nmc_progress(b->task,b->stage,b->offset+done,b->total);
        ReleaseSRWLockExclusive(&b->report);
    }
    return 0;
}
NmcError nmc_batch_part(NmcFiles *files,unsigned jobs,bool verify,NmcStage stage,NmcTask *t,size_t offset,size_t total) {
    Batch b={.files=files,.task=t,.verify=verify,.stage=stage,.offset=offset,.total=total,.report=SRWLOCK_INIT};
    HANDLE workers[15]; DWORD n=0;
    atomic_init(&b.index,0); atomic_init(&b.finished,0); atomic_init(&b.failed,false);
    nmc_progress(t,stage,offset,total);
    while (n<NMC_ARRAY(workers) && n+1<jobs && n+1<files->n) {
        HANDLE thread=CreateThread(NULL,0,download_worker,&b,0,NULL);
        if (!thread) break;
        workers[n++]=thread;
    }
    download_worker(&b);
    if (n) WaitForMultipleObjects(n,workers,TRUE,INFINITE);
    for (DWORD i=0;i<n;++i) CloseHandle(workers[i]);
    if (b.error.code) return b.error;
    return nmc_cancelled(t)?ERR(NMC_CANCELLED,L"downloads"):OK;
}
NmcError nmc_batch(NmcFiles *files,unsigned jobs,bool verify,NmcStage stage,NmcTask *t) {
    return nmc_batch_part(files,jobs,verify,stage,t,0,files->n);
}

