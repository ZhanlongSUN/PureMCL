#include "internal.h"
#include <winhttp.h>

typedef struct { HINTERNET session, conn, req; unsigned status; bool mirrored; } Http;
static void close_http(Http *h) {
    if (h->req) WinHttpCloseHandle(h->req);
    if (h->conn) WinHttpCloseHandle(h->conn);
    if (h->session) WinHttpCloseHandle(h->session);
    *h=(Http){0};
}
#define BMCL "https://bmclapi2.bangbang93.com"
#define MIRROR_LATENCY_MS 1500
#define MIRROR_COOLDOWN_MS 60000
static const char *const content_hosts[]={
    "piston-meta.mojang.com","launchermeta.mojang.com","piston-data.mojang.com",
    "launcher.mojang.com","libraries.minecraft.net","resources.download.minecraft.net",
    "launchercontent.mojang.com"
};
static bool host_is(const char *url, const char *host) {
    size_t n=strlen(host);
    return strncmp(url,"https://",8)==0 && strncmp(url+8,host,n)==0 && url[8+n]=='/';
}
bool nmc_content_url(const char *url) {
    if (!url || strlen(url)>=2048) return false;
    for (size_t i=0;i<NMC_ARRAY(content_hosts);++i) if (host_is(url,content_hosts[i])) return true;
    return false;
}
unsigned nmc_mirror_url(const char *url,char out[2048]) {
    if (!nmc_content_url(url)) return 0;
    const char *path=strchr(url+8,'/'), *prefix="";
    unsigned group=4; /* Mojang object and package endpoints. */
    if (host_is(url,"resources.download.minecraft.net")) { group=1; prefix="/assets"; }
    else if (host_is(url,"libraries.minecraft.net")) { group=2; prefix="/maven"; }
    else if (!host_is(url,"piston-data.mojang.com") && !host_is(url,"launcher.mojang.com") &&
             !host_is(url,"piston-meta.mojang.com") && !host_is(url,"launchermeta.mojang.com")) return 0;
    if (group==4 && strncmp(path,"/v1/objects/",12) && strncmp(path,"/v1/packages/",13)) return 0;
    int n=snprintf(out,2048,BMCL "%s%s",prefix,path);
    return n>0 && n<2048 ? group : 0;
}
static bool mirror_node(const char *url) {
    return host_is(url,"bmclapi2.bangbang93.com") || host_is(url,"mirrors.cernet.edu.cn") ||
        host_is(url,"mirror.nju.edu.cn") || host_is(url,"mirrors.nju.edu.cn") ||
        host_is(url,"mirrors.qlu.edu.cn") || host_is(url,"mirrors.tuna.tsinghua.edu.cn") ||
        host_is(url,"mirrors.ustc.edu.cn") || host_is(url,"mirrors.sjtug.sjtu.edu.cn");
}
static bool cluster_node(const char *url,const char *digest) {
    if (strncmp(url,"https://",8) || !nmc_hex(digest,40)) return false;
    const char *host=url+8, *path=strchr(host,'/');
    if (!path || strncmp(path,"/download/",10) || strncmp(path+10,digest,40) ||
        path[50]!='?' || !strstr(path+51,"s=") || !strstr(path+51,"e=")) return false;
    const char *end=path, *port=memchr(host,':',(size_t)(path-host));
    if (port) { if (path-port!=4 || memcmp(port,":443",4)) return false; end=port; }
    if (end-host<4 || end-host>253) return false;
    const char *label=host,*dot=NULL;
    for (const char *p=host;p<end;++p) {
        char c=*p;
        if (c=='.') {
            if (p==label || p[-1]=='-' || p-label>63) return false;
            dot=p; label=p+1;
        } else if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-",c) ||
                   (p==label && c=='-')) return false;
    }
    if (!dot || end-label<2 || end-label>63) return false;
    for (const char *p=label;p<end;++p)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ",*p)) return false;
    return _strnicmp(label,"local",(size_t)(end-label)) &&
        _strnicmp(label,"localhost",(size_t)(end-label)) && _strnicmp(label,"internal",(size_t)(end-label));
}
static bool auth_url(const char *s) {
    return host_is(s,"login.microsoftonline.com") || host_is(s,"user.auth.xboxlive.com") ||
        host_is(s,"xsts.auth.xboxlive.com") || host_is(s,"api.minecraftservices.com");
}
static NmcError request(Http *h,const char *url,const wchar_t *headers,const char *body,NmcTask *t,bool mirror,
                        const char *cluster_digest) {
    wchar_t wurl[2048], server[256], target[2048];
    URL_COMPONENTS parts={.dwStructSize=sizeof(parts),.lpszHostName=server,.dwHostNameLength=NMC_ARRAY(server),
        .dwUrlPathLength=(DWORD)-1,.dwExtraInfoLength=(DWORD)-1,.dwUserNameLength=(DWORD)-1,.dwPasswordLength=(DWORD)-1};
    if (nmc_cancelled(t)) return ERR(NMC_CANCELLED,L"HTTPS request");
    if ((!nmc_content_url(url) && !auth_url(url) &&
         !(mirror && (mirror_node(url) || (cluster_digest && cluster_node(url,cluster_digest))))) ||
        (mirror && (body || auth_url(url))) || !nmc_utf16(url,wurl,NMC_ARRAY(wurl)) ||
        !WinHttpCrackUrl(wurl,0,0,&parts) || parts.nScheme!=INTERNET_SCHEME_HTTPS ||
        parts.nPort!=443 || parts.dwUserNameLength || parts.dwPasswordLength ||
        parts.dwUrlPathLength+parts.dwExtraInfoLength>=NMC_ARRAY(target) || wcschr(wurl,L'#') || wcschr(wurl,L'\\'))
        return ERR(NMC_HTTP,L"untrusted HTTPS URL");
    wmemcpy(target,parts.lpszUrlPath,parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) wmemcpy(target+parts.dwUrlPathLength,parts.lpszExtraInfo,parts.dwExtraInfoLength);
    target[parts.dwUrlPathLength+parts.dwExtraInfoLength]=0;
    h->session=WinHttpOpen(L"PureMCL/0.1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if (!h->session) return SYS(NMC_HTTP,L"WinHttpOpen");
    if (!WinHttpSetTimeouts(h->session,mirror?3500:8000,mirror?3500:8000,10000,mirror?5000:8000))
        return SYS(NMC_HTTP,L"HTTPS timeouts");
    h->conn=WinHttpConnect(h->session,server,443,0);
    if (!h->conn) return SYS(NMC_HTTP,L"WinHttpConnect");
    h->req=WinHttpOpenRequest(h->conn,body?L"POST":L"GET",target,NULL,WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    if (!h->req) return SYS(NMC_HTTP,L"WinHttpOpenRequest");
    DWORD disabled=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(h->req,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled)))
        return SYS(NMC_HTTP,L"HTTPS request options");
    size_t len=body?strlen(body):0;
    if (len>65536) return ERR(NMC_HTTP,L"HTTPS request length");
    if (!WinHttpSendRequest(h->req,headers,headers?(DWORD)-1:0,(void *)body,(DWORD)len,(DWORD)len,0) ||
        !WinHttpReceiveResponse(h->req,NULL)) return SYS(NMC_HTTP,L"HTTPS response");
    DWORD code=0, size=sizeof(code);
    if (!WinHttpQueryHeaders(h->req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX,&code,&size,WINHTTP_NO_HEADER_INDEX))
        return SYS(NMC_HTTP,L"HTTPS status");
    h->status=code;
    return OK;
}
static bool redirect(unsigned status) {
    return status==301 || status==302 || status==303 || status==307 || status==308;
}
static NmcError request_mirror(Http *h,const char *first,const wchar_t *headers,NmcTask *t,const char *digest) {
    char url[2048];
    strcpy(url,first);
    bool cluster=false;
    for (unsigned i=0;i<5;++i) {
        NmcError e=request(h,url,headers,NULL,t,true,cluster?digest:NULL);
        if (e.code || !redirect(h->status)) return e;
        bool from_bmcl=host_is(url,"bmclapi2.bangbang93.com");
        wchar_t value[2048]; DWORD bytes=sizeof(value);
        char location[2048];
        if (!WinHttpQueryHeaders(h->req,WINHTTP_QUERY_LOCATION,NULL,value,&bytes,NULL) ||
            !nmc_utf8(value,location,sizeof(location))) return ERR(NMC_HTTP,L"BMCLAPI redirect");
        if (!strncmp(location,"https://",8)) strcpy(url,location);
        else if (location[0]=='/' && location[1]!='/' && location[1]!='\\') {
            char *path=strchr(url+8,'/');
            if (!path || (size_t)(path-url)+strlen(location)>=sizeof(url))
                return ERR(NMC_HTTP,L"BMCLAPI redirect path");
            strcpy(path,location);
        } else return ERR(NMC_HTTP,L"BMCLAPI redirect protocol");
        cluster=from_bmcl && cluster_node(url,digest);
        if (!mirror_node(url) && !nmc_content_url(url) && !cluster) return ERR(NMC_HTTP,L"BMCLAPI redirect host");
        close_http(h);
    }
    return ERR(NMC_HTTP,L"BMCLAPI redirect limit");
}
static bool content_ok(const Http *h,NmcError e) {
    return !e.code && (h->status==200 || h->status==206 || h->status==416);
}
static bool mirror_allowed(NmcTask *t) {
    return !t || GetTickCount64()>=atomic_load(&t->mirror_retry_after);
}
static bool try_mirror(Http *h,const char *url,const wchar_t *headers,NmcTask *t,const char *digest) {
    NmcError e=request_mirror(h,url,headers,t,digest);
    if (content_ok(h,e)) {
        h->mirrored=true;
        return true;
    }
    if (t && (h->status==429 || h->status>=500 || e.detail>=12000))
        atomic_store(&t->mirror_retry_after,GetTickCount64()+MIRROR_COOLDOWN_MS);
    close_http(h);
    return false;
}
static NmcError request_content(Http *h,const char *url,const wchar_t *headers,NmcTask *t,const char *digest) {
    /* Only the digest-verified file and version metadata paths call this function. */
    char mirror[2048];
    unsigned group=nmc_mirror_url(url,mirror);
    bool preferred=group && t && (atomic_load(&t->mirror_hosts)&group);
    bool attempted=preferred && mirror_allowed(t);
    if (attempted && try_mirror(h,mirror,headers,t,digest)) return OK;
    ULONGLONG start=GetTickCount64();
    NmcError e=request(h,url,headers,NULL,t,false,NULL);
    bool slow=GetTickCount64()-start>=MIRROR_LATENCY_MS;
    if (group && !attempted && !nmc_cancelled(t) && mirror_allowed(t) && (slow || !content_ok(h,e))) {
        if (t && (slow || e.detail>=12000)) atomic_fetch_or(&t->mirror_hosts,group);
        Http alternative={0};
        if (try_mirror(&alternative,mirror,headers,t,digest)) { close_http(h); *h=alternative; return OK; }
    }
    return e;
}
NmcError nmc_http_json(const char *url,const wchar_t *headers,const char *body,NmcTask *t,
                       unsigned *status,yyjson_doc **doc) {
    Http h={0}; NmcBytes b={0}; char buf[16384];
    *status=0; *doc=NULL;
    NmcError e=request(&h,url,headers,body,t,false,NULL);
    if (e.code) goto done;
    *status=h.status;
    size_t limit=auth_url(url)?(size_t)1024*1024:JSON_LIMIT;
    for (;;) {
        DWORD got=0;
        if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"HTTPS body"); goto done; }
        if (!WinHttpReadData(h.req,buf,sizeof(buf),&got)) { e=SYS(NMC_HTTP,L"HTTPS read"); goto done; }
        if (!got) break;
        if (got>limit-b.len) { e=ERR(NMC_HTTP,L"HTTPS body limit"); goto done; }
        if (!nmc_bytes_add(&b,buf,got)) { e=ERR(NMC_MEMORY,L"HTTPS body"); goto done; }
    }
    e=nmc_json_mem(&b,doc);
    if (e.code && h.status!=200) e=(NmcError){NMC_HTTP,h.status,L"HTTP response status"};
done:
    if (auth_url(url)) { if (b.data) SecureZeroMemory(b.data,b.len); SecureZeroMemory(buf,sizeof(buf)); }
    free(b.data); close_http(&h); return e;
}
static NmcError http_file(const NmcFile *file,NmcTask *t,bool mirror,bool *used_mirror) {
    Http h={0}; wchar_t tmp[NMC_PATH]={0}, headers[128]; HANDLE f=INVALID_HANDLE_VALUE;
    uint64_t total=0; char *buf=NULL; bool own_tmp=false, resumable=file->size>=1048576;
    if (!nmc_content_url(file->url)) return ERR(NMC_HTTP,L"file source");
    if (!nmc_hex(file->hash,40) && !nmc_hex(file->hash,64)) return ERR(NMC_JSON,L"file digest");
    NmcError e=OK;
    if (resumable) {
        TRY(nmc_parent(file->path));
        wchar_t digest[65]; const wchar_t *slash=wcsrchr(file->path,L'\\');
        if (!slash || !nmc_utf16(file->hash,digest,NMC_ARRAY(digest)) ||
            swprintf(tmp,NMC_PATH,L"%.*ls\\.pure-%ls.part",(int)(slash-file->path),file->path,digest)<0) {
            e=ERR(NMC_PATH_ERROR,L"partial download path"); goto done;
        }
        f=CreateFileW(tmp,GENERIC_WRITE|GENERIC_READ,0,NULL,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if (f==INVALID_HANDLE_VALUE) { e=SYS(NMC_FILE,L"open partial download"); goto done; }
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(f,&info)) { e=SYS(NMC_FILE,L"partial download attributes"); goto done; }
        if (info.nNumberOfLinks!=1 || (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) {
            e=ERR(NMC_PATH_ERROR,L"unsafe partial download file"); goto done;
        }
        own_tmp=true;
        total=((uint64_t)info.nFileSizeHigh<<32)|info.nFileSizeLow;
        if (total==file->size) goto validate;
        if (total>file->size) total=0;
    }
    if (total) swprintf(headers,NMC_ARRAY(headers),L"Range: bytes=%llu-", (unsigned long long)total);
    e=mirror?request_content(&h,file->url,total?headers:NULL,t,file->hash):
        request(&h,file->url,total?headers:NULL,NULL,t,false,NULL);
    *used_mirror=h.mirrored;
    if (e.code) goto done;
    if (h.status==206 && total) {
        wchar_t range[128], expected[128]; DWORD length=sizeof(range);
        swprintf(expected,NMC_ARRAY(expected),L"bytes %llu-%llu/%llu",(unsigned long long)total,
            (unsigned long long)(file->size-1),(unsigned long long)file->size);
        if (!WinHttpQueryHeaders(h.req,WINHTTP_QUERY_CONTENT_RANGE,NULL,range,&length,NULL) ||
            wcscmp(range,expected)) { e=ERR(NMC_HASH,L"partial response range"); goto done; }
    } else if (h.status==200) total=0;
    else if (h.status==416 && total) { e=ERR(NMC_HASH,L"server refused partial range"); goto done; }
    else { e=(NmcError){NMC_HTTP,h.status,L"download HTTP status"}; goto done; }
    if (resumable) {
        LARGE_INTEGER offset={.QuadPart=(LONGLONG)total};
        if (!SetFilePointerEx(f,offset,NULL,FILE_BEGIN) || !SetEndOfFile(f)) {
            e=SYS(NMC_FILE,L"position partial download"); goto done;
        }
    } else {
        TRY(nmc_temp(file->path,tmp,&f));
        own_tmp=true;
    }
    buf=malloc(65536);
    if (!buf) { e=ERR(NMC_MEMORY,L"download buffer"); goto done; }
    for (;;) {
        DWORD n=0;
        if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"download"); goto done; }
        if (!WinHttpReadData(h.req,buf,65536,&n)) {
            e=SYS(NMC_HTTP,L"download read");
            if (t && !h.mirrored) {
                char alternative[2048];
                atomic_fetch_or(&t->mirror_hosts,nmc_mirror_url(file->url,alternative));
            }
            goto done;
        }
        if (!n) break;
        if (n>file->size-total) { e=ERR(NMC_HASH,L"download size exceeds metadata"); goto done; }
        TRY(nmc_write(f,buf,n));
        total+=n;
    }
    if (total!=file->size) { e=ERR(NMC_HTTP,L"short download"); goto done; }
validate:
    if (!FlushFileBuffers(f)) { e=SYS(NMC_FILE,L"download flush"); goto done; }
    CloseHandle(f); f=INVALID_HANDLE_VALUE;
    char hash[65];
    TRY(nmc_hash_file(tmp,strlen(file->hash),hash,t));
    if (strcmp(hash,file->hash)) { e=ERR(NMC_HASH,L"download digest"); goto done; }
    TRY(nmc_promote(tmp,file->path));
done:
    if (f!=INVALID_HANDLE_VALUE) CloseHandle(f);
    if (own_tmp && e.code && (!resumable || (e.code!=NMC_HTTP && e.code!=NMC_CANCELLED))) DeleteFileW(tmp);
    free(buf); close_http(&h); return e;
}
NmcError nmc_http_file(const NmcFile *file,NmcTask *t) {
    bool mirrored=false;
    NmcError e=http_file(file,t,true,&mirrored);
    if (mirrored && (e.code==NMC_HASH || e.code==NMC_HTTP))
        e=http_file(file,t,false,&mirrored);
    if (!e.code && mirrored && t && t->mirror && !atomic_exchange(&t->mirror_announced,true))
        t->mirror(t->user);
    return e;
}
static NmcError http_meta(const char *url,const wchar_t *path,const char *hash,NmcTask *t,bool use_mirror) {
    Http h={0}; wchar_t tmp[NMC_PATH]={0}; HANDLE f=INVALID_HANDLE_VALUE;
    char buf[16384], digest[65]; size_t total=0; bool own_tmp=false;
    if (!nmc_content_url(url) || !nmc_hex(hash,40)) return ERR(NMC_JSON,L"metadata source");
    NmcError e=use_mirror?request_content(&h,url,NULL,t,hash):request(&h,url,NULL,NULL,t,false,NULL);
    if (e.code) goto done;
    if (h.status!=200) { e=(NmcError){NMC_HTTP,h.status,L"metadata HTTP status"}; goto done; }
    TRY(nmc_temp(path,tmp,&f));
    own_tmp=true;
    for (;;) {
        DWORD got=0;
        if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"metadata download"); goto done; }
        if (!WinHttpReadData(h.req,buf,sizeof(buf),&got)) { e=SYS(NMC_HTTP,L"metadata read"); goto done; }
        if (!got) break;
        if (got>JSON_LIMIT-total) { e=ERR(NMC_JSON,L"metadata limit"); goto done; }
        TRY(nmc_write(f,buf,got)); total+=got;
    }
    if (!FlushFileBuffers(f)) { e=SYS(NMC_FILE,L"metadata flush"); goto done; }
    CloseHandle(f); f=INVALID_HANDLE_VALUE;
    TRY(nmc_hash_file(tmp,40,digest,t));
    if (strcmp(digest,hash)) { e=ERR(NMC_HASH,L"version JSON digest"); goto done; }
    TRY(nmc_promote(tmp,path));
    if (h.mirrored && t && t->mirror && !atomic_exchange(&t->mirror_announced,true)) t->mirror(t->user);
done:
    if (f!=INVALID_HANDLE_VALUE) CloseHandle(f);
    if (own_tmp && e.code) DeleteFileW(tmp);
    close_http(&h); return e;
}
NmcError nmc_http_meta(const char *url,const wchar_t *path,const char *hash,NmcTask *t) {
    NmcError e=http_meta(url,path,hash,t,true);
    if (e.code==NMC_HASH || e.code==NMC_HTTP) e=http_meta(url,path,hash,t,false);
    return e;
}

