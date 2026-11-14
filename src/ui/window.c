#include "ui.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

enum { MSG_PROGRESS=WM_APP+1, MSG_DEVICE, MSG_DONE, MSG_MIRROR };
enum { OP_CATALOG, OP_INSTALL, OP_REPAIR, OP_PLAY, OP_LOGIN };
enum {
    TAB=100, VERSION, INSTANCE, ACCOUNT, PLAY, INSTALL, REPAIR, REFRESH, CREATE, REMOVE,
    LOGIN, LOGOUT, OPEN, SAVE, CANCEL, ROOT, JAVA, XMS, XMX, JOBS, JVM, EXIT_GAME, GC,
    LABEL_VERSION, LABEL_INSTANCE, LABEL_ACCOUNT, LABEL_ROOT, LABEL_JAVA, LABEL_MEM, LABEL_JVM, LABEL_GC,
    CONTROL_END
};
typedef struct { wchar_t url[512], code[64]; } Device;
typedef struct {
    HWND hwnd, ctr[CONTROL_END-TAB]; HINSTANCE module; HFONT font; HBRUSH bg, input;
    UINT dpi; bool dark, closing, mirrored;
    NmcRenderer renderer;
    NmcConfig config, snapshot;
    NmcTask task; HANDLE thread; int operation;
    NmcError error; unsigned long pid;
    NmcVersionInfo *versions; size_t version_count;
    NmcAccountInfo signed_in, *accounts; size_t account_count;
    wchar_t status[512], sub[512], verify_url[512];
    LARGE_INTEGER start, freq;
} Window;

static HWND item(Window *s,int id) { return s->ctr[id-TAB]; }
static bool busy(Window *s) { return s->thread!=NULL; }
static void label(HWND w,const wchar_t *text) { SetWindowTextW(w,text); }
static void set_status(Window *s,const wchar_t *main,const wchar_t *detail) {
    if (main!=s->status) wcsncpy_s(s->status,NMC_ARRAY(s->status),main,_TRUNCATE);
    if (detail!=s->sub) wcsncpy_s(s->sub,NMC_ARRAY(s->sub),detail,_TRUNCATE);
    InvalidateRect(s->hwnd,NULL,FALSE);
}
static void result(Window *s,NmcError e,const wchar_t *success) {
    if (!e.code) { set_status(s,success,L""); return; }
    wchar_t info[256];
    swprintf(info,NMC_ARRAY(info),L"%ls  ·  0x%08lX",e.where?e.where:L"PureMCL",e.detail);
    set_status(s,nmc_error_text(e.code),info);
    if (e.code!=NMC_CANCELLED) nmc_diagnostic(&s->config,e);
}
static int dp(Window *s,int n) { return MulDiv(n,(int)s->dpi,96); }
static bool high_contrast(void) {
    HIGHCONTRASTW hc={.cbSize=sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0) && (hc.dwFlags&HCF_HIGHCONTRASTON);
}
static void theme(Window *s) {
    DWORD light=1,n=sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme",RRF_RT_REG_DWORD,NULL,&light,&n);
    s->dark=light==0 && !high_contrast();
    BOOL enabled=s->dark;
    DwmSetWindowAttribute(s->hwnd,DWMWA_USE_IMMERSIVE_DARK_MODE,&enabled,sizeof(enabled));
    if (s->bg) DeleteObject(s->bg);
    if (s->input) DeleteObject(s->input);
    s->bg=CreateSolidBrush(s->dark?RGB(25,30,36):RGB(242,245,243));
    s->input=CreateSolidBrush(s->dark?RGB(40,46,52):RGB(255,255,255));
    InvalidateRect(s->hwnd,NULL,TRUE);
}
static void font(Window *s) {
    HFONT prev=s->font;
    s->font=CreateFontW(-dp(s,15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    for (int i=TAB;i<CONTROL_END;++i) if (item(s,i))
        SendMessageW(item(s,i),WM_SETFONT,(WPARAM)(s->font?s->font:GetStockObject(DEFAULT_GUI_FONT)),TRUE);
    if (prev) DeleteObject(prev);
}
static void place(Window *s,int id,int x,int y,int w,int h) {
    SetWindowPos(item(s,id),NULL,dp(s,x),dp(s,y),dp(s,w),dp(s,h),SWP_NOZORDER|SWP_NOACTIVATE);
    ShowWindow(item(s,id),SW_SHOWNA);
    EnableWindow(item(s,id),!busy(s) || id==CANCEL || id==OPEN);
}
static void layout(Window *s) {
    if (!item(s,TAB)) return;
    RECT r; GetClientRect(s->hwnd,&r);
    int width=MulDiv(r.right,96,(int)s->dpi), height=MulDiv(r.bottom,96,(int)s->dpi);
    if (width<540 || height<485) return;
    int room=width-52, page=(int)SendMessageW(item(s,TAB),TCM_GETCURSEL,0,0);
    for (int id=VERSION;id<CONTROL_END;++id) ShowWindow(item(s,id),SW_HIDE);
    place(s,TAB,26,125,room,29);
    EnableWindow(item(s,TAB),TRUE);
    if (page==0 || page==1) {
        place(s,LABEL_INSTANCE,26,174,106,22); place(s,INSTANCE,142,169,room-116,260);
        place(s,LABEL_VERSION,26,215,106,22); place(s,VERSION,142,210,room-116,360);
        place(s,LABEL_ACCOUNT,26,256,room,26);
        if (page==0) {
            place(s,PLAY,26,302,155,41); place(s,INSTALL,191,302,130,41);
        } else {
            place(s,REFRESH,26,298,116,35); place(s,CREATE,150,298,116,35);
            place(s,REPAIR,274,298,116,35); place(s,REMOVE,398,298,116,35);
            place(s,INSTALL,26,342,116,35);
        }
    } else if (page==2) {
        place(s,LABEL_ACCOUNT,26,177,room,25);
        place(s,ACCOUNT,26,211,room,260);
        place(s,LOGIN,26,259,157,37); place(s,LOGOUT,193,259,134,37);
    } else {
        place(s,LABEL_ROOT,26,174,125,22); place(s,ROOT,153,171,room-127,27);
        place(s,LABEL_JAVA,26,209,125,22); place(s,JAVA,153,206,room-127,27);
        place(s,LABEL_MEM,26,244,165,24); place(s,XMS,195,240,62,26);
        place(s,XMX,264,240,72,26); place(s,JOBS,344,240,51,26);
        place(s,LABEL_GC,26,278,125,22); place(s,GC,153,275,160,120);
        place(s,EXIT_GAME,327,275,room-301,25);
        place(s,LABEL_JVM,26,316,125,24); place(s,JVM,153,310,room-127,64);
        place(s,SAVE,26,385,134,33);
    }
    if (s->verify_url[0]) place(s,OPEN,width-286,height-37,148,27);
    if (busy(s)) place(s,CANCEL,width-124,height-37,98,27);
    InvalidateRect(s->hwnd,NULL,FALSE);
}
static void control(Window *s,int id,const wchar_t *cls,const wchar_t *text,DWORD style,DWORD ex) {
    s->ctr[id-TAB]=CreateWindowExW(ex,cls,text,WS_CHILD|WS_CLIPSIBLINGS|style,0,0,0,0,s->hwnd,
                                  (HMENU)(INT_PTR)id,s->module,NULL);
}
static void controls(Window *s) {
    control(s,TAB,WC_TABCONTROLW,L"",WS_TABSTOP,0);
    static const wchar_t *const names[]={L"主页",L"版本与实例",L"账户",L"设置"};
    for (int i=0;i<(int)NMC_ARRAY(names);++i) {
        TCITEMW ti={.mask=TCIF_TEXT,.pszText=(wchar_t *)names[i]};
        SendMessageW(item(s,TAB),TCM_INSERTITEMW,(WPARAM)i,(LPARAM)&ti);
    }
    control(s,INSTANCE,L"COMBOBOX",L"",CBS_DROPDOWN|CBS_AUTOHSCROLL|WS_TABSTOP|WS_VSCROLL,0);
    control(s,VERSION,L"COMBOBOX",L"",CBS_DROPDOWN|CBS_AUTOHSCROLL|WS_TABSTOP|WS_VSCROLL,0);
    control(s,ACCOUNT,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,0);
    static const struct { int id; const wchar_t *name; } buttons[]={
        {PLAY,L"启动游戏"},{INSTALL,L"安装版本"},{REPAIR,L"校验和修复"},{REFRESH,L"刷新版本"},
        {CREATE,L"保存实例"},{REMOVE,L"移除实例"},{LOGIN,L"添加微软账号"},{LOGOUT,L"退出所选账号"},
        {OPEN,L"在系统浏览器验证"},{SAVE,L"保存设置"},{CANCEL,L"取消任务"}
    };
    for (size_t i=0;i<NMC_ARRAY(buttons);++i)
        control(s,buttons[i].id,L"BUTTON",buttons[i].name,BS_PUSHBUTTON|WS_TABSTOP,0);
    control(s,ROOT,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|ES_READONLY,WS_EX_CLIENTEDGE);
    control(s,JAVA,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,WS_EX_CLIENTEDGE);
    for (int i=XMS;i<=JOBS;++i) control(s,i,L"EDIT",L"",WS_TABSTOP|ES_NUMBER,WS_EX_CLIENTEDGE);
    control(s,JVM,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,WS_EX_CLIENTEDGE);
    control(s,EXIT_GAME,L"BUTTON",L"启动游戏后退出",BS_AUTOCHECKBOX|WS_TABSTOP,0);
    control(s,GC,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,0);
    SendMessageW(item(s,GC),CB_ADDSTRING,0,(LPARAM)L"Java 默认");
    SendMessageW(item(s,GC),CB_ADDSTRING,0,(LPARAM)L"G1");
    SendMessageW(item(s,GC),CB_ADDSTRING,0,(LPARAM)L"ZGC（Java 17+）");
    static const wchar_t *const labels[]={L"游戏版本",L"实例名",L"",L"版本目录（固定）",L"Java 路径（可空）",
        L"Xms / Xmx (MiB) / 并发",L"JVM 参数（每行一个）",L"垃圾回收"};
    for (int i=LABEL_VERSION;i<CONTROL_END;++i)
        control(s,i,L"STATIC",labels[i-LABEL_VERSION],SS_LEFT,0);
    SendMessageW(item(s,INSTANCE),CB_LIMITTEXT,NMC_ID-1,0);
    SendMessageW(item(s,VERSION),CB_LIMITTEXT,NMC_ID-1,0);
    SendMessageW(item(s,ROOT),EM_SETLIMITTEXT,NMC_PATH-1,0);
    SendMessageW(item(s,JAVA),EM_SETLIMITTEXT,NMC_PATH-1,0);
    SendMessageW(item(s,JVM),EM_SETLIMITTEXT,NMC_ARRAY(s->config.jvm)-1,0);
    font(s);
}
static void select_fields(Window *s) {
    wchar_t w[128];
    if (nmc_utf16(s->config.instance,w,NMC_ARRAY(w))) label(item(s,INSTANCE),w);
    if (nmc_utf16(s->config.version,w,NMC_ARRAY(w))) label(item(s,VERSION),w);
    label(item(s,ROOT),s->config.versions_dir); label(item(s,JAVA),s->config.java); label(item(s,JVM),s->config.jvm);
    swprintf(w,NMC_ARRAY(w),L"%u",s->config.xms); label(item(s,XMS),w);
    swprintf(w,NMC_ARRAY(w),L"%u",s->config.xmx); label(item(s,XMX),w);
    swprintf(w,NMC_ARRAY(w),L"%u",s->config.jobs); label(item(s,JOBS),w);
    SendMessageW(item(s,EXIT_GAME),BM_SETCHECK,s->config.exit_on_launch?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(item(s,GC),CB_SETCURSEL,s->config.gc,0);
}
static void versions(Window *s) {
    HWND c=item(s,VERSION); wchar_t current[NMC_ID], w[NMC_ID];
    GetWindowTextW(c,current,NMC_ID);
    SendMessageW(c,WM_SETREDRAW,FALSE,0);
    SendMessageW(c,CB_RESETCONTENT,0,0); SendMessageW(c,CB_ADDSTRING,0,(LPARAM)L"latest");
    for (size_t i=0;i<s->version_count;++i)
        if (nmc_utf16(s->versions[i].id,w,NMC_ID)) SendMessageW(c,CB_ADDSTRING,0,(LPARAM)w);
    label(c,current); SendMessageW(c,WM_SETREDRAW,TRUE,0);
}
static void accounts(Window *s) {
    free(s->accounts); s->accounts=NULL; s->account_count=0;
    NmcError e=nmc_accounts(&s->config,&s->accounts,&s->account_count);
    if (e.code) { result(s,e,L""); return; }
    HWND c=item(s,ACCOUNT);
    SendMessageW(c,CB_RESETCONTENT,0,0);
    wchar_t display[128]=L"当前账号：未登录";
    for (size_t i=0;i<s->account_count;++i) {
        wchar_t name[64];
        if (!nmc_utf16(s->accounts[i].name,name,NMC_ARRAY(name))) continue;
        LRESULT row=SendMessageW(c,CB_ADDSTRING,0,(LPARAM)name);
        if (row>=0) SendMessageW(c,CB_SETITEMDATA,(WPARAM)row,(LPARAM)i);
        if (!strcmp(s->accounts[i].id,s->config.account)) {
            if (row>=0) SendMessageW(c,CB_SETCURSEL,(WPARAM)row,0);
            swprintf(display,NMC_ARRAY(display),L"当前账号：%ls",name);
        }
    }
    label(item(s,LABEL_ACCOUNT),display);
}
static void instances(Window *s) {
    NmcVersionInfo *v=NULL; size_t n=0; wchar_t w[NMC_ID];
    HWND c=item(s,INSTANCE); SendMessageW(c,CB_RESETCONTENT,0,0);
    if (!nmc_instances(&s->config,&v,&n).code)
        for (size_t i=0;i<n;++i) if (nmc_utf16(v[i].id,w,NMC_ID)) SendMessageW(c,CB_ADDSTRING,0,(LPARAM)w);
    free(v);
    if (nmc_utf16(s->config.instance,w,NMC_ID)) label(c,w);
}
static bool number(HWND w,unsigned *out) {
    wchar_t text[16],*end=NULL; int n=GetWindowTextW(w,text,NMC_ARRAY(text));
    if (!n || n>=15) return false;
    unsigned long v=wcstoul(text,&end,10);
    if (*end || v>262144) return false;
    *out=(unsigned)v; return true;
}
static NmcError inputs(Window *s,NmcConfig *c) {
    wchar_t w[NMC_ID];
    *c=s->config;
    GetWindowTextW(item(s,INSTANCE),w,NMC_ID);
    if (!nmc_utf8(w,c->instance,sizeof(c->instance))) return ERR(NMC_PATH_ERROR,L"instance ID encoding");
    GetWindowTextW(item(s,VERSION),w,NMC_ID);
    if (!nmc_utf8(w,c->version,sizeof(c->version))) return ERR(NMC_PATH_ERROR,L"version ID encoding");
    GetWindowTextW(item(s,JAVA),c->java,NMC_PATH);
    GetWindowTextW(item(s,JVM),c->jvm,NMC_ARRAY(c->jvm));
    if (!number(item(s,XMS),&c->xms) ||
        !number(item(s,XMX),&c->xmx) || !number(item(s,JOBS),&c->jobs))
        return ERR(NMC_PATH_ERROR,L"app ID or numerical setting");
    LRESULT gc=SendMessageW(item(s,GC),CB_GETCURSEL,0,0);
    c->gc=gc<0?0:(unsigned)gc;
    c->exit_on_launch=SendMessageW(item(s,EXIT_GAME),BM_GETCHECK,0,0)==BST_CHECKED;
    return nmc_config_validate(c);
}
static void progress(void *ctx,NmcStage stage,size_t at,size_t total) {
    Window *s=ctx;
    unsigned percent=total?(unsigned)(at*100/total):101;
    PostMessageW(s->hwnd,MSG_PROGRESS,(WPARAM)stage,(LPARAM)percent);
}
static void device(void *ctx,const wchar_t *url,const wchar_t *code) {
    Window *s=ctx; Device *d=malloc(sizeof(*d));
    if (!d) return;
    wcscpy_s(d->url,NMC_ARRAY(d->url),url); wcscpy_s(d->code,NMC_ARRAY(d->code),code);
    if (!PostMessageW(s->hwnd,MSG_DEVICE,0,(LPARAM)d)) free(d);
}
static void mirror(void *ctx) {
    Window *s=ctx;
    PostMessageW(s->hwnd,MSG_MIRROR,0,0);
}
static DWORD WINAPI work(void *ctx) {
    Window *s=ctx; const NmcConfig *c=&s->snapshot;
    switch (s->operation) {
    case OP_CATALOG: s->error=nmc_versions(c,true,&s->task,&s->versions,&s->version_count); break;
    case OP_INSTALL: s->error=nmc_run(c,NMC_INSTALL,&s->task,NULL); break;
    case OP_REPAIR: s->error=nmc_run(c,NMC_REPAIR,&s->task,NULL); break;
    case OP_PLAY: s->error=nmc_run(c,NMC_PLAY,&s->task,&s->pid); break;
    case OP_LOGIN: s->error=nmc_sign_in(c,&s->task,&s->signed_in); break;
    }
    PostMessageW(s->hwnd,MSG_DONE,0,0); return 0;
}
static void start(Window *s,int op) {
    NmcError e=inputs(s,&s->snapshot);
    if (e.code) { result(s,e,L""); return; }
    if (op==OP_CATALOG) { free(s->versions); s->versions=NULL; s->version_count=0; }
    s->operation=op; s->mirrored=false; nmc_task_reset(&s->task);
    QueryPerformanceCounter(&s->start);
    s->thread=CreateThread(NULL,0,work,s,0,NULL);
    if (!s->thread) { result(s,SYS(NMC_FILE,L"create task worker"),L""); return; }
    set_status(s,L"正在准备任务…",L""); layout(s);
    SetFocus(item(s,CANCEL));
}
static void finish(Window *s) {
    WaitForSingleObject(s->thread,INFINITE); CloseHandle(s->thread); s->thread=NULL;
    s->verify_url[0]=0;
    if (s->closing) { DestroyWindow(s->hwnd); return; }
    if (s->error.code) result(s,s->error,L"");
    else {
        switch (s->operation) {
        case OP_CATALOG: versions(s); set_status(s,L"版本列表已更新",L""); break;
        case OP_INSTALL: case OP_REPAIR:
            s->config=s->snapshot;
            (void)nmc_instance_read(&s->config,s->snapshot.instance);
            const wchar_t *done=s->operation==OP_INSTALL?L"安装完成":L"修复完成";
            result(s,nmc_config_save(&s->config),done);
            if (s->mirrored && !wcscmp(s->status,done)) set_status(s,done,L"本次任务使用过 BMCLAPI 下载源。");
            select_fields(s); instances(s); accounts(s); break;
        case OP_LOGIN:
            s->config=s->snapshot; (void)nmc_copy(s->config.account,sizeof(s->config.account),s->signed_in.id);
            result(s,nmc_config_save(&s->config),L"微软账号已验证");
            accounts(s); break;
        case OP_PLAY: {
            LARGE_INTEGER end; QueryPerformanceCounter(&end);
            wchar_t info[128];
            double ms=(double)(end.QuadPart-s->start.QuadPart)*1000.0/(double)s->freq.QuadPart;
            swprintf(info,NMC_ARRAY(info),L"进程 ID %lu  ·  点击到进程交接 %.0f ms",s->pid,ms);
            set_status(s,L"已启动 Java 进程",info);
            OutputDebugStringW(info);
            if (s->snapshot.exit_on_launch) { DestroyWindow(s->hwnd); return; }
            break;
        }
        }
    }
    layout(s); SetFocus(item(s,TAB));
}
static void action(Window *s,int id,int notification) {
    if (id==CANCEL && busy(s)) {
        atomic_store(&s->task.cancel,true); set_status(s,L"正在取消任务…",L"等待当前请求结束"); return;
    }
    if (id==OPEN && s->verify_url[0]) {
        INT_PTR opened=(INT_PTR)ShellExecuteW(s->hwnd,L"open",s->verify_url,NULL,NULL,SW_SHOWNORMAL);
        if (opened<=32) set_status(s,L"无法打开默认浏览器，请手动打开验证地址。",s->sub);
        return;
    }
    if (busy(s)) return;
    if (id==ACCOUNT && notification==CBN_SELCHANGE) {
        LRESULT idx=SendMessageW(item(s,ACCOUNT),CB_GETCURSEL,0,0);
        LRESULT i=idx<0?-1:SendMessageW(item(s,ACCOUNT),CB_GETITEMDATA,(WPARAM)idx,0);
        if (i>=0 && (size_t)i<s->account_count) {
            (void)nmc_copy(s->config.account,sizeof(s->config.account),s->accounts[i].id);
            result(s,nmc_config_save(&s->config),L"已切换账户"); accounts(s);
        }
        return;
    }
    if (id==INSTANCE && notification==CBN_SELCHANGE) {
        LRESULT row=SendMessageW(item(s,INSTANCE),CB_GETCURSEL,0,0);
        wchar_t name[NMC_ID]; char utf8[NMC_ID];
        if (row>=0 && SendMessageW(item(s,INSTANCE),CB_GETLBTEXT,(WPARAM)row,(LPARAM)name)>0 &&
            nmc_utf8(name,utf8,sizeof(utf8))) {
            NmcError e=nmc_instance_read(&s->config,utf8);
            result(s,e,L"已切换实例");
            if (!e.code) select_fields(s);
        }
        return;
    }
    if (notification!=BN_CLICKED) return;
    switch (id) {
    case REFRESH: start(s,OP_CATALOG); break;
    case INSTALL: start(s,OP_INSTALL); break;
    case REPAIR: start(s,OP_REPAIR); break;
    case PLAY: start(s,OP_PLAY); break;
    case LOGIN: start(s,OP_LOGIN); break;
    case LOGOUT: {
        NmcError e=nmc_sign_out(&s->config);
        if (!e.code) { s->config.account[0]=0; e=nmc_config_save(&s->config); }
        result(s,e,L"已移除本机账号凭据"); accounts(s); break;
    }
    case SAVE: case CREATE: case REMOVE: {
        NmcConfig next; NmcError e=inputs(s,&next);
        if (!e.code && id==REMOVE) {
            e=nmc_instance_remove(&next);
            result(s,e,L"已移除实例记录；游戏文件和存档保留"); instances(s); break;
        }
        if (!e.code) {
            e=nmc_instance_create(&next);
            if (!e.code) e=nmc_config_save(&next);
        }
        if (!e.code) {
            s->config=next;
            instances(s); accounts(s);
        }
        result(s,e,id==CREATE?L"实例已保存":L"设置已保存"); break;
    }
    }
}
static LRESULT CALLBACK window_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    Window *s=(Window *)GetWindowLongPtrW(w,GWLP_USERDATA);
    if (msg==WM_NCCREATE) {
        s=((CREATESTRUCTW *)lp)->lpCreateParams; s->hwnd=w;
        SetWindowLongPtrW(w,GWLP_USERDATA,(LONG_PTR)s);
    }
    if (!s) return DefWindowProcW(w,msg,wp,lp);
    switch (msg) {
    case WM_CREATE: {
        s->dpi=GetDpiForWindow(w);
        theme(s); nmc_renderer_init(&s->renderer); controls(s);
        NmcError e=nmc_config_load(&s->config);
        if (!e.code) {
            NmcError inst=nmc_instance_read(&s->config,s->config.instance);
            if (inst.code!=NMC_MISSING) e=inst;
        }
        select_fields(s); instances(s); accounts(s);
        (void)nmc_versions(&s->config,false,NULL,&s->versions,&s->version_count);
        versions(s);
        result(s,e,L"选择版本和实例。首次使用请先添加正版账号。"); layout(s); return 0;
    }
    case WM_COMMAND: if (lp) action(s,LOWORD(wp),HIWORD(wp)); return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom==TAB && ((NMHDR *)lp)->code==TCN_SELCHANGE) layout(s);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *m=(MINMAXINFO *)lp; UINT dpi=s->dpi?s->dpi:96;
        RECT r={0,0,570,550}; r.right=MulDiv(r.right,(int)dpi,96); r.bottom=MulDiv(r.bottom,(int)dpi,96);
        AdjustWindowRectExForDpi(&r,WS_OVERLAPPEDWINDOW,FALSE,0,dpi);
        m->ptMinTrackSize.x=r.right-r.left; m->ptMinTrackSize.y=r.bottom-r.top; return 0;
    }
    case WM_SIZE: if (wp!=SIZE_MINIMIZED) layout(s); return 0;
    case WM_DPICHANGED: {
        const RECT *r=(const RECT *)lp; s->dpi=HIWORD(wp);
        nmc_renderer_reset(&s->renderer); font(s);
        SetWindowPos(w,NULL,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        layout(s); return 0;
    }
    case WM_THEMECHANGED: case WM_SETTINGCHANGE: theme(s); return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        if (high_contrast()) return DefWindowProcW(w,msg,wp,lp);
        SetTextColor((HDC)wp,s->dark?RGB(240,246,242):RGB(30,40,36));
        SetBkColor((HDC)wp,msg==WM_CTLCOLORSTATIC?
            (s->dark?RGB(25,30,36):RGB(242,245,243)):(s->dark?RGB(40,46,52):RGB(255,255,255)));
        return (LRESULT)(msg==WM_CTLCOLORSTATIC?s->bg:s->input);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: nmc_renderer_draw(&s->renderer,w,s->dpi,s->dark,s->status,s->sub); return 0;
    case MSG_PROGRESS: {
        static const wchar_t *const labels[]={L"正在读取官方版本目录",L"正在安装游戏依赖",L"正在处理游戏资源",
            L"正在检查或安装 Java",L"正在解压原生库",L"正在验证账号",L"正在创建 Java 进程"};
        if (wp<NMC_ARRAY(labels) && !nmc_cancelled(&s->task)) {
            wchar_t detail[64]=L"";
            if (lp<=100) swprintf(detail,NMC_ARRAY(detail),s->mirrored?L"已完成 %u%% · 本次任务使用过 BMCLAPI":L"已完成 %u%%（按文件数）",(unsigned)lp);
            set_status(s,labels[wp],detail);
        }
        return 0;
    }
    case MSG_MIRROR:
        s->mirrored=true;
        if (!nmc_cancelled(&s->task)) set_status(s,s->status,L"已从 BMCLAPI 下载并校验文件。");
        return 0;
    case MSG_DEVICE: {
        Device *d=(Device *)lp;
        if (!s->closing && !nmc_cancelled(&s->task)) {
            wcscpy_s(s->verify_url,NMC_ARRAY(s->verify_url),d->url);
            wchar_t text[640];
            swprintf(text,NMC_ARRAY(text),L"%ls  ·  验证代码：%ls",d->url,d->code);
            set_status(s,L"在系统浏览器完成微软验证",text); layout(s);
        }
        free(d); return 0;
    }
    case MSG_DONE: finish(s); return 0;
    case WM_CLOSE:
        if (!busy(s)) DestroyWindow(w);
        else {
            s->closing=true; atomic_store(&s->task.cancel,true);
            set_status(s,L"正在取消任务并退出…",L"");
        }
        return 0;
    case WM_DESTROY:
        nmc_renderer_free(&s->renderer); if (s->font) DeleteObject(s->font);
        if (s->bg) DeleteObject(s->bg);
        if (s->input) DeleteObject(s->input);
        free(s->versions); free(s->accounts); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}
int nmc_ui_run(HINSTANCE module,int show,LARGE_INTEGER startup) {
    Window *s=calloc(1,sizeof(*s));
    if (!s) return 1;
    s->module=module; s->task=(NmcTask){.progress=progress,.device=device,.mirror=mirror,.user=s};
    nmc_task_reset(&s->task); QueryPerformanceFrequency(&s->freq);
    WNDCLASSEXW cls={.cbSize=sizeof(cls),.lpfnWndProc=window_proc,.hInstance=module,
        .hCursor=LoadCursorW(NULL,IDC_ARROW),.lpszClassName=L"PureMCL.Window"};
    if (!RegisterClassExW(&cls)) { free(s); return 1; }
    UINT dpi=GetDpiForSystem();
    RECT size={0,0,MulDiv(700,(int)dpi,96),MulDiv(570,(int)dpi,96)};
    AdjustWindowRectExForDpi(&size,WS_OVERLAPPEDWINDOW,FALSE,0,dpi);
    HWND w=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"PureMCL",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                           CW_USEDEFAULT,CW_USEDEFAULT,size.right-size.left,size.bottom-size.top,NULL,NULL,module,s);
    if (!w) { free(s); return 1; }
    ShowWindow(w,show); UpdateWindow(w); SetFocus(item(s,TAB));
    LARGE_INTEGER painted; QueryPerformanceCounter(&painted);
    if (s->freq.QuadPart>0 && !s->sub[0]) {
        wchar_t timing[128];
        double ms=(double)(painted.QuadPart-startup.QuadPart)*1000.0/(double)s->freq.QuadPart;
        swprintf(timing,NMC_ARRAY(timing),L"窗口首次绘制 %.0f ms",ms);
        set_status(s,s->status,timing);
        OutputDebugStringW(timing);
    }
    MSG msg; BOOL status;
    while ((status=GetMessageW(&msg,NULL,0,0))>0) {
        if (!IsDialogMessageW(w,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    free(s);
    return status<0?1:(int)msg.wParam;
}

