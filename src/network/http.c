#include "internal.h"
#include <winhttp.h>

typedef struct { HINTERNET session, conn, req; unsigned status; bool own_session; } Http;
void *nmc_http_pool_open(void) {
    return WinHttpOpen(L"PureMCL/0.1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
}
void nmc_http_pool_close(void *session) { if (session) WinHttpCloseHandle(session); }
static void close_http(Http *h) {
    if (h->req) WinHttpCloseHandle(h->req);
    if (h->conn) WinHttpCloseHandle(h->conn);
    if (h->own_session) nmc_http_pool_close(h->session);
    *h=(Http){0};
}
#define BMCL "https://bmclapi2.bangbang93.com"
#define GEOIP_URL "https://api.ip.sb/geoip"
#define GEOIP_TIMEOUT 2200
#define SLOW_WINDOW 15000ULL
#define SLOW_BYTES 15360ULL
static const char *const content_hosts[]={
    "piston-meta.mojang.com","launchermeta.mojang.com","piston-data.mojang.com",
    "launcher.mojang.com","libraries.minecraft.net","resources.download.minecraft.net",
    "launchercontent.mojang.com","meta.fabricmc.net","maven.fabricmc.net","maven.minecraftforge.net",
    "files.minecraftforge.net","maven.neoforged.net","repo.maven.apache.org","repo1.maven.org","cdn.modrinth.com","cdn-raw.modrinth.com",
    "edge.forgecdn.net","mediafilez.forgecdn.net"
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
bool nmc_pack_url(const char *url) {
    if (!url || strlen(url)>=2048) return false;
    static const char *const sources[]={"cdn.modrinth.com","cdn-raw.modrinth.com","github.com",
        "raw.githubusercontent.com","gitlab.com","release-assets.githubusercontent.com","objects.githubusercontent.com",
        "edge.forgecdn.net","mediafilez.forgecdn.net"};
    for (size_t i=0;i<NMC_ARRAY(sources);++i) if (host_is(url,sources[i])) return true;
    return false;
}
unsigned nmc_mirror_url(const char *url,char out[2048]) {
    if (!nmc_content_url(url)) return 0;
    const char *path=strchr(url+8,'/'), *prefix="";
    unsigned group=0;
    if (host_is(url,"resources.download.minecraft.net")) { group=1; prefix="/assets"; }
    else if (host_is(url,"libraries.minecraft.net")) { group=2; prefix="/maven"; }
    else {
        if (strncmp(path,"/v1/objects/",12) && strncmp(path,"/v1/packages/",13)) return 0;
        if (host_is(url,"launcher.mojang.com")) group=3;
        else if (host_is(url,"piston-data.mojang.com")) group=4;
        else if (host_is(url,"piston-meta.mojang.com")) group=5;
        else if (host_is(url,"launchermeta.mojang.com")) group=6;
        else return 0;
    }
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
bool nmc_bmcl_storage_url(const char *url,const char *digest) {
    static const char prefix[]="/bmclapi/download/";
    if (!nmc_hex(digest,40) || !host_is(url,"download.mocdn.net")) return false;
    const char *path=strchr(url+8,'/');
    if (strncmp(path,prefix,sizeof(prefix)-1)) return false;
    path+=sizeof(prefix)-1;
    return !strncmp(path,digest,2) && path[2]=='/' && !strncmp(path+3,digest,40) &&
        path[43]=='?' && strstr(path+44,"X-Amz-Signature=")!=NULL;
}
static bool auth_url(const char *s) {
    return host_is(s,"login.microsoftonline.com") || host_is(s,"user.auth.xboxlive.com") ||
        host_is(s,"xsts.auth.xboxlive.com") || host_is(s,"api.minecraftservices.com");
}
static NmcError request(Http *h,const char *url,const wchar_t *headers,const char *body,NmcTask *t,bool mirror,
                        const char *cluster_digest,bool pack,int lookup_timeout) {
    wchar_t wurl[2048], server[256], target[2048];
    URL_COMPONENTS parts={.dwStructSize=sizeof(parts),.lpszHostName=server,.dwHostNameLength=NMC_ARRAY(server),
        .dwUrlPathLength=(DWORD)-1,.dwExtraInfoLength=(DWORD)-1,.dwUserNameLength=(DWORD)-1,.dwPasswordLength=(DWORD)-1};
    if (nmc_cancelled(t)) return ERR(NMC_CANCELLED,L"HTTPS request");
    bool geo_lookup=!headers && !body && !mirror && !pack && !strcmp(url,GEOIP_URL);
    bool bmcl_node=mirror && (mirror_node(url) || (cluster_digest &&
        (cluster_node(url,cluster_digest) || nmc_bmcl_storage_url(url,cluster_digest))));
    if ((pack && (body || !nmc_pack_url(url))) ||
        (!pack && !geo_lookup && !nmc_content_url(url) && !auth_url(url) && !bmcl_node) ||
        (mirror && (body || auth_url(url))) || !nmc_utf16(url,wurl,NMC_ARRAY(wurl)) ||
        !WinHttpCrackUrl(wurl,0,0,&parts) || parts.nScheme!=INTERNET_SCHEME_HTTPS ||
        parts.nPort!=443 || parts.dwUserNameLength || parts.dwPasswordLength ||
        parts.dwUrlPathLength+parts.dwExtraInfoLength>=NMC_ARRAY(target) || wcschr(wurl,L'#') || wcschr(wurl,L'\\'))
        return ERR(NMC_HTTP,L"untrusted HTTPS URL");
    wmemcpy(target,parts.lpszUrlPath,parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) wmemcpy(target+parts.dwUrlPathLength,parts.lpszExtraInfo,parts.dwExtraInfoLength);
    target[parts.dwUrlPathLength+parts.dwExtraInfoLength]=0;
    h->session=t?t->http_session:NULL;
    if (!h->session) { h->session=nmc_http_pool_open(); h->own_session=true; }
    if (!h->session) return SYS(NMC_HTTP,L"WinHttpOpen");
    h->conn=WinHttpConnect(h->session,server,443,0);
    if (!h->conn) return SYS(NMC_HTTP,L"WinHttpConnect");
    h->req=WinHttpOpenRequest(h->conn,body?L"POST":L"GET",target,NULL,WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    if (!h->req) return SYS(NMC_HTTP,L"WinHttpOpenRequest");
    int connect=lookup_timeout?lookup_timeout:8000;
    if (!WinHttpSetTimeouts(h->req,connect,connect,lookup_timeout?lookup_timeout:10000,connect))
        return SYS(NMC_HTTP,L"HTTPS timeouts");
    if (!auth_url(url)) {
        DWORD retries=lookup_timeout?1:2, response=lookup_timeout?(DWORD)lookup_timeout:10000;
        WinHttpSetOption(h->req,WINHTTP_OPTION_CONNECT_RETRIES,&retries,sizeof(retries));
        WinHttpSetOption(h->req,WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,&response,sizeof(response));
    }
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
static NmcError request_redirected(Http *h,const char *first,const wchar_t *headers,NmcTask *t,
                                   const char *digest,bool pack,bool bmcl) {
    char url[2048];
    strcpy(url,first);
    bool cluster=false, storage=false;
    for (unsigned i=0;i<5;++i) {
        NmcError e=request(h,url,headers,NULL,t,bmcl,(cluster || storage)?digest:NULL,pack,0);
        if (e.code || !redirect(h->status)) return e;
        bool from_bmcl=host_is(url,"bmclapi2.bangbang93.com");
        wchar_t value[2048]; DWORD bytes=sizeof(value);
        char location[2048];
        if (!WinHttpQueryHeaders(h->req,WINHTTP_QUERY_LOCATION,NULL,value,&bytes,NULL) ||
            !nmc_utf8(value,location,sizeof(location))) return ERR(NMC_HTTP,L"download redirect");
        if (!strncmp(location,"https://",8)) strcpy(url,location);
        else if (location[0]=='/' && location[1]!='/' && location[1]!='\\') {
            char *path=strchr(url+8,'/');
            if (!path || (size_t)(path-url)+strlen(location)>=sizeof(url))
                return ERR(NMC_HTTP,L"download redirect path");
            strcpy(path,location);
        } else return ERR(NMC_HTTP,L"download redirect protocol");
        /* Some cluster nodes delegate the same signed object to storage. */
        storage=cluster && nmc_bmcl_storage_url(url,digest);
        cluster=bmcl && from_bmcl && cluster_node(url,digest);
        if (pack ? !nmc_pack_url(url) : (!nmc_content_url(url) && !(bmcl && (mirror_node(url) || cluster || storage))))
            return ERR(NMC_HTTP,L"untrusted download redirect");
        close_http(h);
    }
    return ERR(NMC_HTTP,L"download redirect limit");
}
typedef struct { const char *url,*hash; NmcTask *task; char mirror[2048]; } SourcePair;
bool nmc_geo_is_mainland(const char *json,size_t size) {
    yyjson_doc *doc=yyjson_read(json,size,0);
    if (!doc) return false;
    const char *country=nmc_str(yyjson_doc_get_root(doc),"country_code");
    bool mainland=country && !strcmp(country,"CN");
    yyjson_doc_free(doc);
    return mainland;
}
static NmcDownloadRegion lookup_region(NmcTask *t) {
    Http h={0};
    NmcDownloadRegion region=NMC_REGION_OFFICIAL;
    NmcError e=request(&h,GEOIP_URL,NULL,NULL,t,false,NULL,false,GEOIP_TIMEOUT);
    if (!e.code && h.status==200) {
        char json[4096];
        DWORD used=0;
        ULONGLONG deadline=GetTickCount64()+GEOIP_TIMEOUT;
        while (used<sizeof(json) && !nmc_cancelled(t) && GetTickCount64()<deadline) {
            DWORD got=0;
            if (WinHttpReadDataEx(h.req,json+used,(DWORD)sizeof(json)-used,&got,0,0,NULL)) break;
            if (!got) {
                if (nmc_geo_is_mainland(json,used)) region=NMC_REGION_MAINLAND;
                break;
            }
            used+=got;
        }
    }
    close_http(&h);
    return region;
}
static NmcDownloadRegion download_region(NmcTask *t,unsigned group) {
    if (!group) return NMC_REGION_OFFICIAL;
    while (!nmc_cancelled(t)) {
        unsigned region=atomic_load(&t->download_region);
        if (region==NMC_REGION_MAINLAND || region==NMC_REGION_OFFICIAL) return (NmcDownloadRegion)region;
        if (region==NMC_REGION_CHECKING) { Sleep(25); continue; }
        if (atomic_compare_exchange_strong(&t->download_region,&region,NMC_REGION_CHECKING)) {
            NmcDownloadRegion found=lookup_region(t);
            atomic_store(&t->download_region,found);
            return found;
        }
    }
    return NMC_REGION_OFFICIAL;
}
static void report_bytes(NmcTask *t,DWORD n) {
    if (!t->transfer || !n) return;
    uint64_t bytes=atomic_fetch_add(&t->received_bytes,n)+n;
    unsigned long long now=GetTickCount64(), next=atomic_load(&t->next_transfer_notice);
    if (now>=next && atomic_compare_exchange_strong(&t->next_transfer_notice,&next,now+750))
        t->transfer(t->user,bytes);
}
typedef struct { uint64_t since,offset; } ReadWindow;
static NmcError read_content(Http *h,void *buf,DWORD cap,DWORD *got,ReadWindow *w,uint64_t total,uint64_t size,NmcTask *t) {
    if (nmc_cancelled(t)) return ERR(NMC_CANCELLED,L"download");
    /* Avoid waiting to fill an entire buffer when the peer is barely sending. */
    DWORD code=WinHttpReadDataEx(h->req,buf,cap,got,0,0,NULL);
    if (code) return nmc_winerr(NMC_HTTP,code,L"download read");
    report_bytes(t,*got);
    uint64_t now=GetTickCount64();
    if (*got && total+*got!=size && now-w->since>=SLOW_WINDOW) {
        if (total+*got-w->offset<SLOW_BYTES)
            return nmc_winerr(NMC_HTTP,ERROR_WINHTTP_TIMEOUT,L"download stalled");
        *w=(ReadWindow){now,total+*got};
    }
    return OK;
}
NmcError nmc_http_json(const char *url,const wchar_t *headers,const char *body,NmcTask *t,
                       unsigned *status,yyjson_doc **doc) {
    Http h={0}; NmcBytes b={0}; char buf[16384];
    *status=0; *doc=NULL;
    NmcError e=request(&h,url,headers,body,t,false,NULL,false,0);
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
NmcError nmc_http_digest(const char *url,char out[129],NmcTask *t) {
    Http h={0}; char body[257]={0}; size_t n=0;
    if (!nmc_content_url(url)) return ERR(NMC_HTTP,L"checksum source");
    NmcError e=request(&h,url,NULL,NULL,t,false,NULL,false,0);
    if (e.code) goto done;
    if (h.status!=200) { e=(NmcError){NMC_HTTP,h.status,L"checksum HTTP status"}; goto done; }
    while (n<sizeof(body)-1) {
        DWORD got=0;
        if (!WinHttpReadData(h.req,body+n,(DWORD)(sizeof(body)-1-n),&got)) { e=SYS(NMC_HTTP,L"checksum read"); goto done; }
        if (!got) break;
        n+=got;
    }
    body[strcspn(body," \t\r\n")]=0;
    if (!nmc_hex(body,40) && !nmc_hex(body,64) && !nmc_hex(body,128)) { e=ERR(NMC_HASH,L"Maven checksum"); goto done; }
    strcpy(out,body);
done:
    close_http(&h);
    return e;
}
static void announce_mirror(NmcTask *t) {
    if (t->mirror && !atomic_exchange(&t->mirror_announced,true)) t->mirror(t->user);
}
typedef struct { SourcePair pair; const NmcFile *file; } FileTransfer;
static NmcError http_file(void *ctx,bool mirror) {
    const FileTransfer *download=ctx;
    const NmcFile *file=download->file;
    NmcTask *t=download->pair.task;
    Http h={0}; wchar_t tmp[NMC_PATH]={0}, headers[128]; HANDLE f=INVALID_HANDLE_VALUE;
    uint64_t total=0; char *buf=NULL; bool own_tmp=false;
    bool known_size=file->size!=NMC_SIZE_UNKNOWN, resumable=known_size && file->size>=1048576;
    uint64_t limit=known_size?file->size:UINT64_C(536870912);
    NmcError e=OK;
    if (resumable) {
        TRY(nmc_parent(file->path));
        wchar_t digest[129]; const wchar_t *slash=wcsrchr(file->path,L'\\');
        if (!slash || !nmc_utf16(file->hash,digest,NMC_ARRAY(digest)) ||
            swprintf(tmp,NMC_PATH,L"%ls.pure-%ls.part",file->path,digest)<0) {
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
    TRY(request_redirected(&h,mirror?download->pair.mirror:file->url,total?headers:NULL,t,
                           file->hash,file->pack,mirror));
    if (h.status==206 && total) {
        wchar_t range[128], expected[128]; DWORD length=sizeof(range);
        swprintf(expected,NMC_ARRAY(expected),L"bytes %llu-%llu/%llu",(unsigned long long)total,
            (unsigned long long)(file->size-1),(unsigned long long)file->size);
        if (!WinHttpQueryHeaders(h.req,WINHTTP_QUERY_CONTENT_RANGE,NULL,range,&length,NULL) ||
            wcscmp(range,expected)) { e=ERR(NMC_HASH,L"partial response range"); goto done; }
    } else if (h.status==200) total=0;
    else if (h.status==416 && total) { e=ERR(NMC_HASH,L"server refused partial range"); goto done; }
    else { e=(NmcError){NMC_HTTP,h.status,L"download HTTP status"}; goto done; }
    if (known_size) {
        wchar_t length[32], *end=NULL; DWORD bytes=sizeof(length);
        if (WinHttpQueryHeaders(h.req,WINHTTP_QUERY_CONTENT_LENGTH,NULL,length,&bytes,NULL)) {
            unsigned long long declared=wcstoull(length,&end,10);
            if (!length[0] || *end || declared!=file->size-total) {
                e=ERR(NMC_HASH,L"download content length"); goto done;
            }
        }
    }
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
    ReadWindow window={GetTickCount64(),total};
    /* The official length and digest allow verification without waiting for a
       server to close an otherwise complete response. */
    while (!known_size || total<file->size) {
        DWORD n=0;
        TRY(read_content(&h,buf,65536,&n,&window,total,file->size,t));
        if (!n) break;
        if (n>limit-total) { e=ERR(NMC_HASH,L"download size exceeds metadata"); goto done; }
        TRY(nmc_write(f,buf,n));
        total+=n;
    }
    if (known_size && total!=file->size) { e=ERR(NMC_HTTP,L"short download"); goto done; }
validate:
    if (total>=1048576 && !FlushFileBuffers(f)) { e=SYS(NMC_FILE,L"download flush"); goto done; }
    CloseHandle(f); f=INVALID_HANDLE_VALUE;
    char hash[129];
    TRY(nmc_hash_file(tmp,strlen(file->hash),hash,t));
    if (strcmp(hash,file->hash)) { e=ERR(NMC_HASH,L"download digest"); goto done; }
    if (file->hash512[0]) {
        TRY(nmc_hash_file(tmp,128,hash,t));
        if (strcmp(hash,file->hash512)) { e=ERR(NMC_HASH,L"download SHA-512"); goto done; }
    }
    TRY(nmc_promote(tmp,file->path));
    if (mirror && h.req) announce_mirror(t);
done:
    if (f!=INVALID_HANDLE_VALUE) CloseHandle(f);
    if (own_tmp && e.code && (!resumable || (e.code!=NMC_HTTP && e.code!=NMC_CANCELLED))) DeleteFileW(tmp);
    free(buf); close_http(&h); return e;
}
NmcError nmc_http_file(const NmcFile *file,NmcTask *t) {
    if (file->pack ? !nmc_pack_url(file->url) : !nmc_content_url(file->url))
        return ERR(NMC_HTTP,L"file source");
    if (!nmc_hex(file->hash,40) && !nmc_hex(file->hash,64) && !nmc_hex(file->hash,128))
        return ERR(NMC_JSON,L"file digest");
    NmcTask local={0};
    if (!t) { nmc_task_reset(&local); t=&local; }
    FileTransfer download={.pair={.url=file->url,.hash=file->hash,.task=t},.file=file};
    unsigned group=file->pack?0:nmc_mirror_url(file->url,download.pair.mirror);
    return nmc_transfer_route(t,group,download_region(t,group),http_file,&download);
}
typedef struct { SourcePair pair; const wchar_t *path; } MetaTransfer;
static NmcError http_meta(void *ctx,bool mirror) {
    const MetaTransfer *download=ctx;
    const char *url=download->pair.url, *hash=download->pair.hash;
    const wchar_t *path=download->path;
    NmcTask *t=download->pair.task;
    Http h={0}; wchar_t tmp[NMC_PATH]={0}; HANDLE f=INVALID_HANDLE_VALUE;
    char buf[16384], digest[65]; size_t total=0; bool own_tmp=false;
    NmcError e=request_redirected(&h,mirror?download->pair.mirror:url,NULL,t,hash,false,mirror);
    if (e.code) goto done;
    if (h.status!=200) { e=(NmcError){NMC_HTTP,h.status,L"metadata HTTP status"}; goto done; }
    TRY(nmc_temp(path,tmp,&f));
    own_tmp=true;
    ReadWindow window={GetTickCount64(),0};
    for (;;) {
        DWORD got=0;
        TRY(read_content(&h,buf,sizeof(buf),&got,&window,total,NMC_SIZE_UNKNOWN,t));
        if (!got) break;
        if (got>JSON_LIMIT-total) { e=ERR(NMC_JSON,L"metadata limit"); goto done; }
        TRY(nmc_write(f,buf,got)); total+=got;
    }
    if (!FlushFileBuffers(f)) { e=SYS(NMC_FILE,L"metadata flush"); goto done; }
    CloseHandle(f); f=INVALID_HANDLE_VALUE;
    TRY(nmc_hash_file(tmp,40,digest,t));
    if (strcmp(digest,hash)) { e=ERR(NMC_HASH,L"version JSON digest"); goto done; }
    TRY(nmc_promote(tmp,path));
    if (mirror) announce_mirror(t);
done:
    if (f!=INVALID_HANDLE_VALUE) CloseHandle(f);
    if (own_tmp && e.code) DeleteFileW(tmp);
    close_http(&h); return e;
}
NmcError nmc_http_meta(const char *url,const wchar_t *path,const char *hash,NmcTask *t) {
    if (!nmc_content_url(url) || !nmc_hex(hash,40)) return ERR(NMC_JSON,L"metadata source");
    NmcTask local={0};
    if (!t) { nmc_task_reset(&local); t=&local; }
    MetaTransfer download={.pair={.url=url,.hash=hash,.task=t},.path=path};
    unsigned group=nmc_mirror_url(url,download.pair.mirror);
    return nmc_transfer_route(t,group,download_region(t,group),http_meta,&download);
}

