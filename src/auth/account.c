#include "internal.h"
#include "client_id.h"
#include <wincrypt.h>

#define OAUTH "https://login.microsoftonline.com/consumers/oauth2/v2.0/"
#define SCOPE "XboxLive.signin%20offline_access"
#define XBL "https://user.auth.xboxlive.com/user/authenticate"
#define XSTS "https://xsts.auth.xboxlive.com/xsts/authorize"
#define MC "https://api.minecraftservices.com/"
static const wchar_t FORM[]=L"Content-Type: application/x-www-form-urlencoded";
static const wchar_t JSON[]=L"Content-Type: application/json\r\nAccept: application/json";
static const char ENTROPY[]="PureMCL/Account/v1";
static const char CLIENT_ID[]=NMC_MICROSOFT_CLIENT_ID;
_Static_assert(sizeof(CLIENT_ID)==1 || sizeof(CLIENT_ID)==37,
               "NMC_MICROSOFT_CLIENT_ID in src/auth/client_id.h must be empty or a 36-character GUID");

NmcError nmc_auth_configured(void) {
    if (!CLIENT_ID[0])
        return ERR(NMC_AUTH,L"请在 src/auth/client_id.h 填写微软 Client ID，再重新编译。");
    for (size_t i=0;i<36;++i) {
        char ch=CLIENT_ID[i];
        bool dash=i==8 || i==13 || i==18 || i==23;
        if (!ch || (dash ? ch!='-' : !strchr("0123456789abcdefABCDEF",ch)))
            return ERR(NMC_AUTH,L"src/auth/client_id.h 中的微软 Client ID 格式无效。");
    }
    return OK;
}
static uint64_t seconds(void) {
    FILETIME now; GetSystemTimeAsFileTime(&now);
    return (((uint64_t)now.dwHighDateTime<<32)|now.dwLowDateTime)/10000000-UINT64_C(11644473600);
}
static bool account_path(const NmcConfig *c,const char *id,wchar_t p[NMC_PATH]) {
    char rel[64];
    if (!nmc_hex(id,32)) return false;
    snprintf(rel,sizeof(rel),"accounts/%s.bin",id);
    return nmc_path(p,c->root,rel);
}
static NmcError save(const NmcConfig *c,NmcSession *s) {
    wchar_t path[NMC_PATH];
    if (!account_path(c,s->player.id,path)) return ERR(NMC_AUTH,L"account identity");
    DATA_BLOB plain={sizeof(*s),(BYTE *)s}, entropy={sizeof(ENTROPY),(BYTE *)ENTROPY}, sealed={0};
    if (!CryptProtectData(&plain,L"PureMCL", &entropy,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&sealed))
        return SYS(NMC_AUTH,L"protect credentials with DPAPI");
    NmcError e=nmc_atomic(path,sealed.pbData,sealed.cbData);
    LocalFree(sealed.pbData); return e;
}
static NmcError load(const NmcConfig *c,const char *id,NmcSession *s) {
    wchar_t path[NMC_PATH]; NmcBytes bytes={0};
    if (!account_path(c,id,path)) return ERR(NMC_AUTH,L"select a Microsoft account");
    CHECK(nmc_read(path,65536,&bytes));
    DATA_BLOB sealed={(DWORD)bytes.len,(BYTE *)bytes.data}, entropy={sizeof(ENTROPY),(BYTE *)ENTROPY}, plain={0};
    NmcError e=OK;
    if (!CryptUnprotectData(&sealed,NULL,&entropy,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&plain))
        e=SYS(NMC_AUTH,L"unprotect credentials with DPAPI");
    else if (plain.cbData!=sizeof(*s)) e=ERR(NMC_AUTH,L"credential schema");
    else {
        memcpy(s,plain.pbData,sizeof(*s));
        if (!memchr(s->player.id,0,sizeof(s->player.id)) || strcmp(s->player.id,id) ||
            !memchr(s->player.name,0,sizeof(s->player.name)) || !s->player.name[0] ||
            !memchr(s->token,0,sizeof(s->token)) || !memchr(s->refresh,0,sizeof(s->refresh)) ||
            !memchr(s->xuid,0,sizeof(s->xuid)) || !memchr(s->client,0,sizeof(s->client))) {
            SecureZeroMemory(s,sizeof(*s)); e=ERR(NMC_AUTH,L"credential schema");
        }
    }
    if (plain.pbData) { SecureZeroMemory(plain.pbData,plain.cbData); LocalFree(plain.pbData); }
    free(bytes.data); return e;
}
NmcError nmc_sign_out(const NmcConfig *c) {
    wchar_t path[NMC_PATH]; uint64_t size;
    if (!account_path(c,c->account,path)) return ERR(NMC_AUTH,L"select an account");
    if (!nmc_file_size(path,&size)) return ERR(NMC_MISSING,L"stored credentials");
    return DeleteFileW(path)?OK:SYS(NMC_AUTH,L"delete stored credentials");
}
NmcError nmc_accounts(const NmcConfig *c,NmcAccountInfo **out,size_t *count) {
    wchar_t path[NMC_PATH]; *out=NULL; *count=0;
    if (nmc_auth_configured().code) return OK;
    if (!nmc_join(path,c->root,L"accounts\\*.bin")) return ERR(NMC_PATH_ERROR,L"accounts directory");
    WIN32_FIND_DATAW data; HANDLE f=FindFirstFileW(path,&data);
    if (f==INVALID_HANDLE_VALUE) {
        DWORD error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND ? OK : nmc_winerr(NMC_FILE,error,L"account list");
    }
    size_t cap=0; NmcError e=OK;
    do {
        char id[37]; NmcSession s={0};
        if (data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT) ||
            !nmc_utf8(data.cFileName,id,sizeof(id)) || strlen(id)!=36 || _stricmp(id+32,".bin")) continue;
        id[32]=0;
        NmcError read=load(c,id,&s);
        if (!read.code && !_stricmp(s.client,CLIENT_ID)) {
            if (!nmc_grow((void **)out,&cap,*count+1,sizeof(**out))) e=ERR(NMC_MEMORY,L"account list");
            else (*out)[(*count)++]=s.player;
        }
        SecureZeroMemory(&s,sizeof(s));
    } while (!e.code && FindNextFileW(f,&data));
    FindClose(f);
    if (e.code) { free(*out); *out=NULL; *count=0; }
    return e;
}
static bool form_value(NmcBytes *b,const char *s) {
    static const char hex[]="0123456789ABCDEF";
    for (const unsigned char *p=(const unsigned char *)s;*p;++p) {
        unsigned char ch=*p;
        if (strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~",ch)) {
            if (!nmc_bytes_add(b,&ch,1)) return false;
        } else {
            char escaped[]={'%',hex[ch>>4],hex[ch&15]};
            if (!nmc_bytes_add(b,escaped,3)) return false;
        }
    }
    return true;
}
static bool visible_ascii(const char *s) {
    if (!s || !*s) return false;
    for (;*s;++s) if ((unsigned char)*s<=32 || (unsigned char)*s>=127) return false;
    return true;
}
static void forget_doc(yyjson_doc **doc) {
    if (*doc) {
        yyjson_val *v=yyjson_doc_get_root(*doc);
        /* Only wipe live string values, never keys while navigating the tree. */
        static const char *keys[]={"access_token","refresh_token","id_token","device_code","Token"};
        for (size_t i=0;i<NMC_ARRAY(keys);++i) {
            yyjson_val *value=yyjson_obj_get(v,keys[i]);
            const char *secret=yyjson_get_str(value);
            if (secret) SecureZeroMemory((void *)secret,yyjson_get_len(value));
        }
        yyjson_doc_free(*doc); *doc=NULL;
    }
}
static NmcError auth_request(const char *url,const wchar_t *headers,const char *body,NmcTask *t,
                             const wchar_t *label,yyjson_doc **doc) {
    unsigned status=0; NmcError e=nmc_http_json(url,headers,body,t,&status,doc);
    if (status && status!=200) return (NmcError){NMC_AUTH,status,label};
    return e;
}
static NmcError token_request(const char *client,const char *grant,const char *name,const char *value,
                              NmcTask *t,unsigned *status,yyjson_doc **d) {
    char prefix[256]; NmcBytes b={0};
    int n=snprintf(prefix,sizeof(prefix),"client_id=%s&scope=%s&grant_type=%s&%s=",client,SCOPE,grant,name);
    NmcError e=ERR(NMC_MEMORY,L"OAuth token form");
    if (n>0 && n<(int)sizeof(prefix) && nmc_bytes_add(&b,prefix,(size_t)n) && form_value(&b,value))
        e=nmc_http_json(OAUTH "token",FORM,b.data,t,status,d);
    if (b.data) SecureZeroMemory(b.data,b.len);
    free(b.data); return e;
}
static NmcError oauth_device(const char *client,NmcTask *t,yyjson_doc **token) {
    char body[128], code[8192]={0}; wchar_t user[64], url[512];
    yyjson_doc *device=NULL; NmcError e=OK;
    if (!t || !t->device) return ERR(NMC_AUTH,L"device code callback");
    snprintf(body,sizeof(body),"client_id=%s&scope=%s",client,SCOPE);
    TRY(auth_request(OAUTH "devicecode",FORM,body,t,L"Microsoft device code",&device));
    yyjson_val *r=yyjson_doc_get_root(device);
    const char *uri=nmc_str(r,"verification_uri");
    if (!uri || (strcmp(uri,"https://www.microsoft.com/link") &&
        strcmp(uri,"https://microsoft.com/devicelogin") && strcmp(uri,"https://www.microsoft.com/devicelogin")) ||
        !nmc_utf16(uri,url,NMC_ARRAY(url)) ||
        !nmc_utf16(nmc_str(r,"user_code"),user,NMC_ARRAY(user)) ||
        !nmc_copy(code,sizeof(code),nmc_str(r,"device_code"))) {
        e=ERR(NMC_AUTH,L"Microsoft device response"); goto done;
    }
    uint64_t expires=yyjson_get_uint(yyjson_obj_get(r,"expires_in")), delay=yyjson_get_uint(yyjson_obj_get(r,"interval"));
    if (!expires || expires>3600) { e=ERR(NMC_AUTH,L"Microsoft device code expiration"); goto done; }
    if (!delay || delay>60) delay=5;
    t->device(t->user,url,user);
    ULONGLONG end=GetTickCount64()+expires*1000;
    while (GetTickCount64()<end) {
        ULONGLONG next=GetTickCount64()+delay*1000;
        while (GetTickCount64()<next && GetTickCount64()<end) {
            if (nmc_cancelled(t)) { e=ERR(NMC_CANCELLED,L"Microsoft login"); goto done; }
            Sleep(125);
        }
        if (GetTickCount64()>=end) break;
        unsigned status=0;
        e=token_request(client,"urn:ietf:params:oauth:grant-type:device_code","device_code",code,t,&status,token);
        if (!e.code && status==200) goto done;
        const char *message=nmc_str(yyjson_doc_get_root(*token),"error");
        bool pending=message && !strcmp(message,"authorization_pending");
        bool slow=message && !strcmp(message,"slow_down");
        forget_doc(token);
        if (!pending && !slow) {
            if (status) e=(NmcError){NMC_AUTH,status,L"Microsoft device token"};
            goto done;
        }
        if (slow) delay+=5;
    }
    e=ERR(NMC_AUTH,L"Microsoft device code expired");
done:
    SecureZeroMemory(code,sizeof(code)); forget_doc(&device); return e;
}
static char *body_json(unsigned stage,const char *token,const char *user) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL); char *body=NULL;
    yyjson_mut_val *root=d?yyjson_mut_obj(d):NULL, *props=d?yyjson_mut_obj(d):NULL;
    if (!root || !props) goto done;
    yyjson_mut_doc_set_root(d,root);
    if (stage==2) {
        if (!yyjson_mut_obj_add_str(d,root,"identityToken",token)) goto done;
    } else {
        if (!yyjson_mut_obj_add_val(d,root,"Properties",props) ||
            !yyjson_mut_obj_add_str(d,root,"TokenType","JWT") ||
            !yyjson_mut_obj_add_str(d,root,"RelyingParty",stage?"rp://api.minecraftservices.com/":"http://auth.xboxlive.com")) goto done;
        if (stage==0) {
            if (!yyjson_mut_obj_add_str(d,props,"AuthMethod","RPS") ||
                !yyjson_mut_obj_add_str(d,props,"SiteName","user.auth.xboxlive.com") ||
                !yyjson_mut_obj_add_str(d,props,"RpsTicket",token)) goto done;
        } else {
            yyjson_mut_val *a=yyjson_mut_arr(d);
            if (!a || !yyjson_mut_obj_add_str(d,props,"SandboxId","RETAIL") ||
                !yyjson_mut_obj_add_val(d,props,"UserTokens",a) || !yyjson_mut_arr_add_str(d,a,token)) goto done;
        }
    }
    body=yyjson_mut_write(d,0,NULL);
done:
    (void)user; yyjson_mut_doc_free(d); return body;
}
static NmcError exchange(unsigned stage,const char *value,NmcTask *t,yyjson_doc **out) {
    static const char *const urls[]={XBL,XSTS,MC "authentication/login_with_xbox"};
    static const wchar_t *const labels[]={L"Xbox Live sign-in",L"Xbox XSTS authorization",L"Minecraft app authorization"};
    char *body=body_json(stage,value,NULL);
    if (!body) return ERR(NMC_MEMORY,L"account request JSON");
    NmcError e=auth_request(urls[stage],JSON,body,t,labels[stage],out);
    SecureZeroMemory(body,strlen(body)); free(body); return e;
}
static NmcError verify_player(NmcSession *s,NmcTask *t,const char *expected_id) {
    yyjson_doc *ent=NULL,*profile=NULL;
    wchar_t bearer[8256]=L"Authorization: Bearer ";
    NmcError e=OK;
    size_t prefix=wcslen(bearer);
    if (!visible_ascii(s->token) || !nmc_utf16(s->token,bearer+prefix,NMC_ARRAY(bearer)-prefix)) {
        e=ERR(NMC_AUTH,L"Minecraft authorization header"); goto done;
    }
    TRY(auth_request(MC "entitlements/mcstore",bearer,NULL,t,L"Minecraft entitlement",&ent));
    yyjson_val *items=yyjson_obj_get(yyjson_doc_get_root(ent),"items"),*v;
    size_t i,n; bool entitled=false;
    yyjson_arr_foreach(items,i,n,v) {
        const char *name=nmc_str(v,"name");
        if (name && (!strcmp(name,"game_minecraft") || !strcmp(name,"product_minecraft") ||
                     !strcmp(name,"game_minecraft_java") || !strcmp(name,"product_minecraft_java"))) entitled=true;
    }
    if (!entitled) { e=ERR(NMC_AUTH,L"no Minecraft Java entitlement"); goto done; }
    TRY(auth_request(MC "minecraft/profile",bearer,NULL,t,L"Minecraft Java profile",&profile));
    yyjson_val *r=yyjson_doc_get_root(profile); const char *id=nmc_str(r,"id"), *name=nmc_str(r,"name");
    if (!nmc_hex(id,32) || (expected_id && strcmp(id,expected_id)) ||
        !name || !*name || !nmc_copy(s->player.name,sizeof(s->player.name),name)) {
        e=ERR(NMC_AUTH,L"Minecraft Java profile or account mismatch"); goto done;
    }
    strcpy(s->player.id,id);
done:
    SecureZeroMemory(bearer,sizeof(bearer));
    forget_doc(&ent); forget_doc(&profile); return e;
}
static NmcError minecraft(yyjson_doc *oauth,NmcTask *t,NmcSession *s,const char *expected_id) {
    yyjson_doc *xbox=NULL,*xsts=NULL,*mc=NULL;
    char ticket[8192]={0}, identity[16384]={0};
    yyjson_val *r=yyjson_doc_get_root(oauth);
    const char *token=nmc_str(r,"access_token"), *refresh=nmc_str(r,"refresh_token");
    NmcError e=OK;
    if (!visible_ascii(token) || strlen(token)>sizeof(ticket)-3 ||
        (refresh && !nmc_copy(s->refresh,sizeof(s->refresh),refresh)) || !s->refresh[0]) {
        e=ERR(NMC_AUTH,L"Microsoft token response"); goto done;
    }
    snprintf(ticket,sizeof(ticket),"d=%s",token);
    TRY(exchange(0,ticket,t,&xbox));
    token=nmc_str(yyjson_doc_get_root(xbox),"Token");
    if (!visible_ascii(token)) { e=ERR(NMC_AUTH,L"Xbox token response"); goto done; }
    TRY(exchange(1,token,t,&xsts));
    r=yyjson_doc_get_root(xsts);
    yyjson_val *claims=yyjson_arr_get_first(yyjson_obj_get(yyjson_obj_get(r,"DisplayClaims"),"xui"));
    token=nmc_str(r,"Token"); const char *uhs=nmc_str(claims,"uhs"), *xid=nmc_str(claims,"xid");
    if (!visible_ascii(token) || !visible_ascii(uhs) || strlen(uhs)+strlen(token)+8>=sizeof(identity)) {
        e=ERR(NMC_AUTH,L"XSTS token response"); goto done;
    }
    if (xid && strspn(xid,"0123456789")==strlen(xid)) (void)nmc_copy(s->xuid,sizeof(s->xuid),xid);
    snprintf(identity,sizeof(identity),"XBL3.0 x=%s;%s",uhs,token);
    TRY(exchange(2,identity,t,&mc));
    r=yyjson_doc_get_root(mc); token=nmc_str(r,"access_token");
    uint64_t expiry=yyjson_get_uint(yyjson_obj_get(r,"expires_in"));
    if (!visible_ascii(token) || !expiry || expiry>604800 || !nmc_copy(s->token,sizeof(s->token),token)) {
        e=ERR(NMC_AUTH,L"Minecraft access token"); goto done;
    }
    TRY(verify_player(s,t,expected_id));
    s->expires=seconds()+expiry;
done:
    SecureZeroMemory(ticket,sizeof(ticket)); SecureZeroMemory(identity,sizeof(identity));
    forget_doc(&xbox); forget_doc(&xsts); forget_doc(&mc); return e;
}
NmcError nmc_sign_in(const NmcConfig *c,NmcTask *t,NmcAccountInfo *info) {
    CHECK(nmc_auth_configured());
    CHECK(nmc_config_validate(c));
    NmcSession s={0}; yyjson_doc *doc=NULL;
    nmc_progress(t,NMC_STAGE_AUTH,0,0);
    NmcError e=oauth_device(CLIENT_ID,t,&doc);
    if (!e.code) e=minecraft(doc,t,&s,NULL);
    if (!e.code) { strcpy(s.client,CLIENT_ID); e=save(c,&s); }
    if (!e.code) *info=s.player;
    forget_doc(&doc); SecureZeroMemory(&s,sizeof(s)); return e;
}
NmcError nmc_session(const NmcConfig *c,NmcTask *t,NmcSession *s) {
    CHECK(nmc_auth_configured());
    NmcError loaded=load(c,c->account,s);
    if (loaded.code==NMC_MISSING) return ERR(NMC_AUTH,L"Microsoft account credentials are missing; sign in");
    if (loaded.code) return loaded;
    if (_stricmp(CLIENT_ID,s->client)) return ERR(NMC_AUTH,L"应用 Client ID 已更改，请重新登录。");
    nmc_progress(t,NMC_STAGE_AUTH,0,0);
    if (s->expires>seconds()+120 && visible_ascii(s->token)) {
        NmcError check=verify_player(s,t,c->account);
        if (check.code!=NMC_AUTH || check.detail!=401) return check;
    }
    yyjson_doc *doc=NULL; unsigned status=0;
    NmcError e=token_request(CLIENT_ID,"refresh_token","refresh_token",s->refresh,t,&status,&doc);
    if (status && status!=200) e=(NmcError){NMC_AUTH,status,L"Microsoft refresh; sign in again if expired"};
    if (!e.code) e=minecraft(doc,t,s,c->account);
    if (!e.code) e=save(c,s);
    forget_doc(&doc); return e;
}

