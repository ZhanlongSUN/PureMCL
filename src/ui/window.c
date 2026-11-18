#include "ui.h"
#include "resource.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>

enum { MSG_PROGRESS=WM_APP+1, MSG_DEVICE, MSG_DONE, MSG_MIRROR, MSG_TRANSFER };
enum { OP_CATALOG, OP_INSTALL, OP_REPAIR, OP_PLAY, OP_LOGIN, OP_PACK, OP_REMOVE };
enum {
    TAB=100, VERSION, INSTANCE, INSTALLED, INSTALLED_LIST, LOADER, LOADER_VERSION, PROFILE, IMPORT, ACCOUNT, PLAY, INSTALL, REPAIR, REFRESH, REMOVE,
    LOGIN, LOGOUT, OPEN, SAVE, CANCEL, ROOT, JAVA, XMS, XMX, JOBS, JVM, EXIT_GAME, TRACK_LOGS, GC,
    LABEL_VERSION, LABEL_INSTANCE, LABEL_ACCOUNT, LABEL_ROOT, LABEL_JAVA, LABEL_MEM, LABEL_JVM, LABEL_GC,
    LABEL_LOADER, LABEL_LOADER_VERSION, LABEL_PROFILE,
    CONTROL_END
};
typedef struct { wchar_t url[512], code[64]; } Device;
typedef struct {
    HWND hwnd, ctr[CONTROL_END-TAB]; HINSTANCE module; HFONT font; HBRUSH bg, input;
    RECT bounds[CONTROL_END-TAB];
    bool visible[CONTROL_END-TAB], catalog_loaded;
    UINT dpi; bool dark, closing, mirrored;
    NmcRenderer renderer;
    NmcConfig config, snapshot;
    NmcTask task; HANDLE thread; int operation;
    NmcError error; unsigned long pid;
    NmcVersionInfo *versions; size_t version_count;
    NmcVersionInfo *installed; size_t installed_count;
    NmcAccountInfo signed_in, *accounts; size_t account_count;
    wchar_t status[512], sub[512], verify_url[512];
    wchar_t profile[NMC_PATH], archive[NMC_PATH];
    bool have_progress;
    NmcStage stage;
    uint32_t done, total;
    uint64_t bytes;
} Window;

static HWND item(Window *s,int id) { return s->ctr[id-TAB]; }
static bool busy(Window *s) { return s->thread!=NULL; }
static void label(HWND w,const wchar_t *text) { SetWindowTextW(w,text); }
static int dp(Window *s,int n) { return MulDiv(n,(int)s->dpi,96); }
static void set_status(Window *s,const wchar_t *main,const wchar_t *detail) {
    if (!wcscmp(s->status,main) && !wcscmp(s->sub,detail)) return;
    if (main!=s->status) wcsncpy_s(s->status,NMC_ARRAY(s->status),main,_TRUNCATE);
    if (detail!=s->sub) wcsncpy_s(s->sub,NMC_ARRAY(s->sub),detail,_TRUNCATE);
    RECT status;
    GetClientRect(s->hwnd,&status);
    status.top=status.bottom-dp(s,NMC_STATUS_HEIGHT);
    InvalidateRect(s->hwnd,&status,FALSE);
}
static void result(Window *s,NmcError e,const wchar_t *success) {
    if (!e.code) { set_status(s,success,L""); return; }
    wchar_t info[256];
    swprintf(info,NMC_ARRAY(info),L"%ls  ·  0x%08lX",e.where?e.where:L"PureMCL",e.detail);
    set_status(s,nmc_error_text(e.code),info);
    if (e.code!=NMC_CANCELLED) nmc_diagnostic(&s->config,e);
}
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
    HWND control=item(s,id);
    size_t index=(size_t)(id-TAB);
    RECT next={dp(s,x),dp(s,y),dp(s,x+w),dp(s,y+h)};
    s->visible[index]=true;
    if (!EqualRect(&s->bounds[index],&next) &&
        SetWindowPos(control,NULL,next.left,next.top,next.right-next.left,next.bottom-next.top,
                     SWP_NOZORDER|SWP_NOACTIVATE)) s->bounds[index]=next;
    if (!(GetWindowLongPtrW(control,GWL_STYLE)&WS_VISIBLE)) ShowWindow(control,SW_SHOWNA);
    bool enable=!busy(s) || id==TAB || id==CANCEL || id==OPEN;
    if (enable && (id==PLAY || id==REPAIR || id==REMOVE))
        enable=SendMessageW(item(s,INSTALLED),CB_GETCURSEL,0,0)>=0;
    if ((IsWindowEnabled(control)!=0)!=enable) EnableWindow(control,enable);
}
static void layout(Window *s) {
    if (!item(s,TAB)) return;
    RECT r; GetClientRect(s->hwnd,&r);
    int width=MulDiv(r.right,96,(int)s->dpi), height=MulDiv(r.bottom,96,(int)s->dpi);
    if (width<540 || height<485) return;
    int room=width-52, page=(int)SendMessageW(item(s,TAB),TCM_GETCURSEL,0,0);
    memset(s->visible,0,sizeof(s->visible));
    place(s,TAB,26,125,room,34);
    if (page==0) {
        place(s,LABEL_INSTANCE,26,174,106,22); place(s,INSTALLED,142,169,room-116,260);
        place(s,LABEL_ACCOUNT,26,215,room,26);
        place(s,PLAY,26,254,155,41);
    } else if (page==2) {
        place(s,LABEL_INSTANCE,26,170,room,22); place(s,INSTALLED_LIST,26,195,room,145);
        place(s,REPAIR,26,355,145,37); place(s,REMOVE,179,355,145,37);
    } else if (page==1) {
        place(s,LABEL_INSTANCE,26,174,106,22); place(s,INSTANCE,170,171,room-144,26);
        place(s,LABEL_VERSION,26,211,106,22); place(s,VERSION,170,206,room-144,235);
        place(s,LABEL_LOADER,26,248,106,22); place(s,LOADER,170,243,room-144,180);
        if (SendMessageW(item(s,LOADER),CB_GETCURSEL,0,0)>0) {
            place(s,LABEL_LOADER_VERSION,26,285,142,22); place(s,LOADER_VERSION,170,282,room-144,26);
            place(s,PROFILE,26,317,137,30); place(s,LABEL_PROFILE,170,317,room-144,39);
        }
        place(s,INSTALL,26,360,116,35); place(s,IMPORT,150,360,145,35); place(s,REFRESH,303,360,116,35);
    } else if (page==3) {
        place(s,LABEL_ACCOUNT,26,177,room,25);
        place(s,ACCOUNT,26,211,room,260);
        place(s,LOGIN,26,259,157,37); place(s,LOGOUT,193,259,134,37);
    } else {
        place(s,LABEL_ROOT,26,174,162,26); place(s,ROOT,194,171,room-168,29);
        place(s,LABEL_JAVA,26,209,162,26); place(s,JAVA,194,206,room-168,29);
        place(s,LABEL_MEM,26,244,165,24); place(s,XMS,195,240,62,26);
        place(s,XMX,264,240,72,26); place(s,JOBS,344,240,51,26);
        place(s,LABEL_GC,26,278,125,22); place(s,GC,153,275,160,120);
        place(s,EXIT_GAME,327,275,room-301,25);
        place(s,LABEL_JVM,26,313,162,43); place(s,JVM,194,309,room-168,55);
        place(s,TRACK_LOGS,26,367,room,28);
        place(s,SAVE,26,400,134,33);
    }
    if (s->verify_url[0]) place(s,OPEN,width-286,height-37,148,27);
    if (busy(s)) place(s,CANCEL,width-124,height-37,98,27);
    for (int id=VERSION;id<CONTROL_END;++id)
        if (!s->visible[id-TAB] && (GetWindowLongPtrW(item(s,id),GWL_STYLE)&WS_VISIBLE)) ShowWindow(item(s,id),SW_HIDE);
    InvalidateRect(s->hwnd,NULL,FALSE);
}
static void control(Window *s,int id,const wchar_t *cls,const wchar_t *text,DWORD style,DWORD ex) {
    s->ctr[id-TAB]=CreateWindowExW(ex,cls,text,WS_CHILD|WS_CLIPSIBLINGS|style,0,0,0,0,s->hwnd,
                                  (HMENU)(INT_PTR)id,s->module,NULL);
}
static void controls(Window *s) {
    control(s,TAB,WC_TABCONTROLW,L"",WS_TABSTOP,0);
    static const wchar_t *const names[]={L"主页",L"安装",L"已安装",L"账户",L"设置"};
    for (int i=0;i<(int)NMC_ARRAY(names);++i) {
        TCITEMW ti={.mask=TCIF_TEXT,.pszText=(wchar_t *)names[i]};
        SendMessageW(item(s,TAB),TCM_INSERTITEMW,(WPARAM)i,(LPARAM)&ti);
    }
    control(s,INSTANCE,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,WS_EX_CLIENTEDGE);
    control(s,INSTALLED,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,0);
    control(s,INSTALLED_LIST,L"LISTBOX",L"",LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_TABSTOP|WS_VSCROLL,WS_EX_CLIENTEDGE);
    control(s,VERSION,L"COMBOBOX",L"",CBS_DROPDOWN|CBS_AUTOHSCROLL|WS_TABSTOP|WS_VSCROLL,0);
    control(s,LOADER,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,0);
    static const wchar_t *const loaders[]={L"原版",L"Fabric",L"Forge",L"NeoForge",L"OptiFine"};
    for (size_t i=0;i<NMC_ARRAY(loaders);++i) SendMessageW(item(s,LOADER),CB_ADDSTRING,0,(LPARAM)loaders[i]);
    control(s,LOADER_VERSION,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,WS_EX_CLIENTEDGE);
    control(s,ACCOUNT,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,0);
    static const struct { int id; const wchar_t *name; } buttons[]={
        {PLAY,L"启动游戏"},{INSTALL,L"安装版本"},{REPAIR,L"校验和修复"},{REFRESH,L"刷新版本"},
        {REMOVE,L"卸载游戏"},{LOGIN,L"添加微软账号"},{LOGOUT,L"退出所选账号"},
        {OPEN,L"在系统浏览器验证"},{SAVE,L"保存设置"},{CANCEL,L"取消任务"},
        {PROFILE,L"导入加载器配置"},{IMPORT,L"导入整合包"}
    };
    for (size_t i=0;i<NMC_ARRAY(buttons);++i)
        control(s,buttons[i].id,L"BUTTON",buttons[i].name,BS_PUSHBUTTON|WS_TABSTOP,0);
    control(s,ROOT,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|ES_READONLY,WS_EX_CLIENTEDGE);
    control(s,JAVA,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,WS_EX_CLIENTEDGE);
    for (int i=XMS;i<=JOBS;++i) control(s,i,L"EDIT",L"",WS_TABSTOP|ES_NUMBER,WS_EX_CLIENTEDGE);
    control(s,JVM,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,WS_EX_CLIENTEDGE);
    control(s,EXIT_GAME,L"BUTTON",L"启动游戏后退出",BS_AUTOCHECKBOX|WS_TABSTOP,0);
    control(s,TRACK_LOGS,L"BUTTON",L"追踪游戏日志（会增加资源占用）",BS_AUTOCHECKBOX|WS_TABSTOP,0);
    control(s,GC,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,0);
    SendMessageW(item(s,GC),CB_ADDSTRING,0,(LPARAM)L"Java 默认");
    SendMessageW(item(s,GC),CB_ADDSTRING,0,(LPARAM)L"G1");
    SendMessageW(item(s,GC),CB_ADDSTRING,0,(LPARAM)L"ZGC（Java 17+）");
    static const wchar_t *const labels[]={L"游戏版本",L"实例名",L"",L"版本目录（固定）",L"Java 路径（可空）",
        L"堆内存 (MiB) / 并发",L"JVM 参数（每行一个）",L"垃圾回收",
        L"加载器",L"加载器版本（可空）",L""};
    for (int i=LABEL_VERSION;i<CONTROL_END;++i)
        control(s,i,L"STATIC",labels[i-LABEL_VERSION],SS_LEFT,0);
    SendMessageW(item(s,INSTANCE),EM_SETLIMITTEXT,NMC_ID-1,0);
    SendMessageW(item(s,LOADER_VERSION),EM_SETLIMITTEXT,NMC_ID-1,0);
    SendMessageW(item(s,VERSION),CB_LIMITTEXT,NMC_ID-1,0);
    SendMessageW(item(s,ROOT),EM_SETLIMITTEXT,NMC_PATH-1,0);
    SendMessageW(item(s,JAVA),EM_SETLIMITTEXT,NMC_PATH-1,0);
    SendMessageW(item(s,JVM),EM_SETLIMITTEXT,NMC_ARRAY(s->config.jvm)-1,0);
    font(s);
}
static void select_fields(Window *s) {
    wchar_t w[128];
    label(item(s,ROOT),s->config.versions_dir); label(item(s,JAVA),s->config.java); label(item(s,JVM),s->config.jvm);
    swprintf(w,NMC_ARRAY(w),L"%u",s->config.xms); label(item(s,XMS),w);
    swprintf(w,NMC_ARRAY(w),L"%u",s->config.xmx); label(item(s,XMX),w);
    swprintf(w,NMC_ARRAY(w),L"%u",s->config.jobs); label(item(s,JOBS),w);
    SendMessageW(item(s,EXIT_GAME),BM_SETCHECK,s->config.exit_on_launch?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(item(s,TRACK_LOGS),BM_SETCHECK,s->config.track_logs?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(item(s,GC),CB_SETCURSEL,s->config.gc,0);
}
static void versions(Window *s) {
    HWND c=item(s,VERSION); wchar_t current[NMC_ID], w[NMC_ID];
    GetWindowTextW(c,current,NMC_ID);
    SendMessageW(c,WM_SETREDRAW,FALSE,0);
    SendMessageW(c,CB_RESETCONTENT,0,0); SendMessageW(c,CB_ADDSTRING,0,(LPARAM)L"latest");
    for (size_t i=0;i<s->version_count;++i)
        if (nmc_utf16(s->versions[i].id,w,NMC_ID)) SendMessageW(c,CB_ADDSTRING,0,(LPARAM)w);
    free(s->versions); s->versions=NULL; s->version_count=0;
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
    free(s->installed); s->installed=NULL; s->installed_count=0;
    HWND c=item(s,INSTALLED), list=item(s,INSTALLED_LIST);
    SendMessageW(c,CB_RESETCONTENT,0,0); SendMessageW(list,LB_RESETCONTENT,0,0);
    if (nmc_instances(&s->config,&s->installed,&s->installed_count).code) return;
    LRESULT selected=-1;
    for (size_t i=0;i<s->installed_count;++i) {
        NmcVersionInfo *v=s->installed+i;
        wchar_t id[NMC_ID], ver[NMC_ID+32], row[2*NMC_ID+64];
        if (!nmc_utf16(v->id,id,NMC_ID) || !nmc_utf16(v->type,ver,NMC_ARRAY(ver))) continue;
        swprintf(row,NMC_ARRAY(row),L"%ls  ·  %ls",id,ver);
        LRESULT entry=SendMessageW(c,CB_ADDSTRING,0,(LPARAM)row);
        if (entry>=0) {
            if (SendMessageW(list,LB_ADDSTRING,0,(LPARAM)row)<0) {
                SendMessageW(c,CB_DELETESTRING,(WPARAM)entry,0); break;
            }
            SendMessageW(c,CB_SETITEMDATA,(WPARAM)entry,(LPARAM)i);
            if (!_stricmp(s->config.instance,v->id)) selected=entry;
        }
    }
    if (selected<0 && SendMessageW(c,CB_GETCOUNT,0,0)>0) selected=0;
    if (selected>=0) {
        LRESULT idx=SendMessageW(c,CB_GETITEMDATA,(WPARAM)selected,0);
        if (idx>=0 && (size_t)idx<s->installed_count &&
            !nmc_instance_read(&s->config,s->installed[idx].id).code) {
            SendMessageW(c,CB_SETCURSEL,(WPARAM)selected,0);
            SendMessageW(list,LB_SETCURSEL,(WPARAM)selected,0);
        }
    }
}
static bool number(HWND w,unsigned *out) {
    wchar_t text[16],*end=NULL; int n=GetWindowTextW(w,text,NMC_ARRAY(text));
    if (!n || n>=15) return false;
    unsigned long v=wcstoul(text,&end,10);
    if (*end || v>262144) return false;
    *out=(unsigned)v; return true;
}
static NmcError inputs(Window *s,NmcConfig *c,bool installation) {
    wchar_t w[NMC_ID];
    *c=s->config;
    if (installation) {
        GetWindowTextW(item(s,INSTANCE),w,NMC_ID);
        if (!nmc_utf8(w,c->instance,sizeof(c->instance))) return ERR(NMC_PATH_ERROR,L"instance ID encoding");
        GetWindowTextW(item(s,VERSION),w,NMC_ID);
        if (!nmc_utf8(w,c->version,sizeof(c->version))) return ERR(NMC_PATH_ERROR,L"version ID encoding");
        LRESULT loader=SendMessageW(item(s,LOADER),CB_GETCURSEL,0,0);
        c->loader=loader<0?NMC_VANILLA:(NmcLoader)loader;
        GetWindowTextW(item(s,LOADER_VERSION),w,NMC_ID);
        c->loader_version[0]=0; c->loader_profile[0]=0;
        if (c->loader!=NMC_VANILLA) {
            if (!nmc_utf8(w,c->loader_version,sizeof(c->loader_version))) return ERR(NMC_PATH_ERROR,L"loader version encoding");
            wcscpy_s(c->loader_profile,NMC_PATH,s->profile);
        }
    }
    GetWindowTextW(item(s,JAVA),c->java,NMC_PATH);
    GetWindowTextW(item(s,JVM),c->jvm,NMC_ARRAY(c->jvm));
    if (!number(item(s,XMS),&c->xms) ||
        !number(item(s,XMX),&c->xmx) || !number(item(s,JOBS),&c->jobs))
        return ERR(NMC_PATH_ERROR,L"JVM memory or download concurrency");
    LRESULT gc=SendMessageW(item(s,GC),CB_GETCURSEL,0,0);
    c->gc=gc<0?0:(unsigned)gc;
    c->exit_on_launch=SendMessageW(item(s,EXIT_GAME),BM_GETCHECK,0,0)==BST_CHECKED;
    c->track_logs=SendMessageW(item(s,TRACK_LOGS),BM_GETCHECK,0,0)==BST_CHECKED;
    return nmc_config_validate(c);
}
static bool choose_file(Window *s,const wchar_t *filters,wchar_t file[NMC_PATH]) {
    file[0]=0;
    OPENFILENAMEW dialog={.lStructSize=sizeof(dialog),.hwndOwner=s->hwnd,.lpstrFilter=filters,.lpstrFile=file,
        .nMaxFile=NMC_PATH,.Flags=OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT};
    return GetOpenFileNameW(&dialog)!=0;
}
static void progress(void *ctx,NmcStage stage,size_t at,size_t total) {
    Window *s=ctx;
    _Static_assert(sizeof(LPARAM)==sizeof(uint64_t),"PureMCL targets Windows x64");
    PostMessageW(s->hwnd,MSG_PROGRESS,(WPARAM)stage,(LPARAM)(((uint64_t)at<<32)|(uint32_t)total));
}
static void transfer(void *ctx,uint64_t bytes) {
    Window *s=ctx;
    PostMessageW(s->hwnd,MSG_TRANSFER,0,(LPARAM)bytes);
}
static void progress_text(Window *s) {
    static const wchar_t *const labels[]={L"正在读取官方版本目录",L"正在安装游戏依赖",L"正在处理游戏资源",
        L"正在检查或安装 Java",L"正在解压原生库",L"正在验证账号",L"正在创建 Java 进程",
        L"正在安装加载器",L"正在导入整合包"};
    wchar_t detail[128]=L"", received[64]=L"";
    const bool network=s->stage==NMC_STAGE_FILES || s->stage==NMC_STAGE_ASSETS ||
        s->stage==NMC_STAGE_JAVA || s->stage==NMC_STAGE_LOADER || s->stage==NMC_STAGE_PACK;
    if (network && s->bytes) {
        if (s->bytes<1048576) swprintf(received,NMC_ARRAY(received),L"已接收 %.1f KiB",(double)s->bytes/1024);
        else swprintf(received,NMC_ARRAY(received),L"已接收 %.1f MiB",(double)s->bytes/1048576);
    }
    if (s->total) {
        swprintf(detail,NMC_ARRAY(detail),L"%u / %u 个文件%ls%ls%ls",s->done,s->total,
                 received[0]?L"  ·  ":L"",received,s->mirrored?L"  ·  使用过 BMCLAPI":L"");
    } else if (received[0] || s->mirrored) {
        swprintf(detail,NMC_ARRAY(detail),L"%ls%ls%ls",received,
                 received[0] && s->mirrored?L"  ·  ":L"",s->mirrored?L"使用过 BMCLAPI":L"");
    }
    if ((unsigned)s->stage<NMC_ARRAY(labels)) set_status(s,labels[s->stage],detail);
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
    case OP_PACK: s->error=nmc_import_pack(c,s->archive,&s->task); break;
    case OP_REMOVE: s->error=nmc_uninstall(c,&s->task); break;
    }
    PostMessageW(s->hwnd,MSG_DONE,0,0); return 0;
}
static void start(Window *s,int op) {
    bool install=op==OP_INSTALL || op==OP_PACK;
    bool selected=SendMessageW(item(s,INSTALLED),CB_GETCURSEL,0,0)>=0;
    if (!selected && (op==OP_REPAIR || op==OP_PLAY || op==OP_REMOVE)) {
        result(s,ERR(NMC_MISSING,L"请先选择一个已安装的实例"),L""); return;
    }
    s->snapshot=s->config;
    NmcError e=(install || op==OP_PLAY || op==OP_REPAIR)?inputs(s,&s->snapshot,install):OK;
    if (e.code) { result(s,e,L""); return; }
    if (op==OP_CATALOG) { free(s->versions); s->versions=NULL; s->version_count=0; }
    s->operation=op; s->mirrored=false; s->have_progress=false; s->bytes=0; nmc_task_reset(&s->task);
    s->thread=CreateThread(NULL,0,work,s,0,NULL);
    if (!s->thread) { result(s,SYS(NMC_FILE,L"create task worker"),L""); return; }
    set_status(s,L"正在准备任务…",L""); layout(s);
    SetFocus(item(s,CANCEL));
}
static void finish(Window *s) {
    WaitForSingleObject(s->thread,INFINITE); CloseHandle(s->thread); s->thread=NULL;
    s->verify_url[0]=0;
    if (s->closing) { DestroyWindow(s->hwnd); return; }
    if (s->error.code) {
        result(s,s->error,L"");
        if (s->operation==OP_REMOVE) { instances(s); select_fields(s); }
    }
    else {
        switch (s->operation) {
        case OP_CATALOG: s->catalog_loaded=true; versions(s); set_status(s,L"版本列表已更新",L""); break;
        case OP_INSTALL: case OP_REPAIR: case OP_PACK: {
            s->config=s->snapshot;
            (void)nmc_instance_read(&s->config,s->snapshot.instance);
            const wchar_t *done=s->operation==OP_REPAIR?L"修复完成":
                                s->operation==OP_PACK?L"整合包安装完成":L"安装完成";
            result(s,nmc_config_save(&s->config),done);
            if (s->mirrored && !wcscmp(s->status,done)) set_status(s,done,L"本次任务使用过 BMCLAPI 下载源。");
            instances(s); select_fields(s); accounts(s); break;
        }
        case OP_REMOVE:
            instances(s); select_fields(s);
            result(s,nmc_config_save(&s->config),L"游戏已卸载"); break;
        case OP_LOGIN:
            s->config=s->snapshot; (void)nmc_copy(s->config.account,sizeof(s->config.account),s->signed_in.id);
            result(s,nmc_config_save(&s->config),L"微软账号已验证");
            accounts(s); break;
        case OP_PLAY: {
            wchar_t info[128];
            swprintf(info,NMC_ARRAY(info),L"进程 ID %lu",s->pid);
            if (s->task.watch_error.code) {
                set_status(s,L"游戏已启动，日志监控启动失败",s->task.watch_error.where);
                if (s->snapshot.exit_on_launch)
                    MessageBoxW(s->hwnd,L"游戏已启动，但日志监控无法启动。请检查启动器目录中的 PureMCLMonitor.exe。",
                                L"PureMCL",MB_OK|MB_ICONWARNING);
            } else set_status(s,L"已启动 Java 进程",info);
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
    if ((id==INSTALLED && notification==CBN_SELCHANGE) || (id==INSTALLED_LIST && notification==LBN_SELCHANGE)) {
        LRESULT row=SendMessageW(item(s,id),id==INSTALLED?CB_GETCURSEL:LB_GETCURSEL,0,0);
        LRESULT idx=row<0?-1:SendMessageW(item(s,INSTALLED),CB_GETITEMDATA,(WPARAM)row,0);
        if (idx>=0 && (size_t)idx<s->installed_count) {
            NmcError e=nmc_instance_read(&s->config,s->installed[idx].id);
            if (!e.code) {
                SendMessageW(item(s,INSTALLED),CB_SETCURSEL,(WPARAM)row,0);
                SendMessageW(item(s,INSTALLED_LIST),LB_SETCURSEL,(WPARAM)row,0);
                select_fields(s); e=nmc_config_save(&s->config);
            } else { instances(s); select_fields(s); layout(s); }
            result(s,e,L"已切换实例");
        }
        return;
    }
    if (id==LOADER && notification==CBN_SELCHANGE) {
        s->profile[0]=0;
        label(item(s,LOADER_VERSION),L"");
        label(item(s,LABEL_PROFILE),SendMessageW(item(s,LOADER),CB_GETCURSEL,0,0)==NMC_OPTIFINE?
              L"请选择 OptiFine 官方安装器生成的版本 JSON":L"高级：可选用官方安装器生成的版本 JSON");
        layout(s); return;
    }
    if (notification!=BN_CLICKED) return;
    switch (id) {
    case REFRESH: start(s,OP_CATALOG); break;
    case INSTALL: start(s,OP_INSTALL); break;
    case PROFILE:
        if (choose_file(s,L"加载器版本 JSON\0*.json\0",s->profile)) {
            const wchar_t *name=wcsrchr(s->profile,L'\\');
            label(item(s,LABEL_PROFILE),name?name+1:s->profile);
        } else label(item(s,LABEL_PROFILE),L"未选择版本 JSON");
        break;
    case IMPORT:
        if (choose_file(s,L"整合包\0*.mrpack;*.zip;*.rar;*.7z;*.tar;*.tar.gz;*.tar.xz;*.tgz\0所有文件\0*.*\0",s->archive)) start(s,OP_PACK);
        break;
    case REMOVE: {
        wchar_t name[NMC_ID], confirm[320];
        if (!nmc_utf16(s->config.instance,name,NMC_ID)) break;
        swprintf(confirm,NMC_ARRAY(confirm),L"永久删除「%ls」的游戏文件、存档、模组和设置？",name);
        if (MessageBoxW(s->hwnd,confirm,L"卸载游戏",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES) start(s,OP_REMOVE);
        break;
    }
    case REPAIR: start(s,OP_REPAIR); break;
    case PLAY: start(s,OP_PLAY); break;
    case LOGIN: start(s,OP_LOGIN); break;
    case LOGOUT: {
        NmcError e=nmc_sign_out(&s->config);
        if (!e.code) { s->config.account[0]=0; e=nmc_config_save(&s->config); }
        result(s,e,L"已移除本机账号凭据"); accounts(s); break;
    }
    case SAVE: {
        NmcConfig next; NmcError e=inputs(s,&next,false);
        if (!e.code) {
            if (SendMessageW(item(s,INSTALLED),CB_GETCURSEL,0,0)>=0) e=nmc_instance_create(&next);
            if (!e.code) e=nmc_config_save(&next);
        }
        if (!e.code) {
            s->config=next;
            accounts(s);
        }
        result(s,e,L"设置已保存"); break;
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
        instances(s); select_fields(s); accounts(s);
        label(item(s,INSTANCE),s->installed_count?L"":L"default");
        label(item(s,VERSION),L"latest");
        SendMessageW(item(s,LOADER),CB_SETCURSEL,NMC_VANILLA,0);
        result(s,e,s->installed_count?L"请选择已安装的实例。":L"请在「安装」页安装游戏或导入整合包。"); layout(s); return 0;
    }
    case WM_COMMAND: if (lp) action(s,LOWORD(wp),HIWORD(wp)); return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom==TAB && ((NMHDR *)lp)->code==TCN_SELCHANGE) {
            if (!busy(s)) {
                LRESULT page=SendMessageW(item(s,TAB),TCM_GETCURSEL,0,0);
                if (page==1 && !s->catalog_loaded) {
                    (void)nmc_versions(&s->config,false,NULL,&s->versions,&s->version_count);
                    versions(s);
                    s->catalog_loaded=true;
                } else if (page==2) {
                    instances(s); select_fields(s);
                }
            }
            layout(s);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *m=(MINMAXINFO *)lp; UINT dpi=s->dpi?s->dpi:96;
        RECT r={0,0,570,550}; r.right=MulDiv(r.right,(int)dpi,96); r.bottom=MulDiv(r.bottom,(int)dpi,96);
        AdjustWindowRectExForDpi(&r,WS_OVERLAPPEDWINDOW,FALSE,0,dpi);
        m->ptMinTrackSize.x=r.right-r.left; m->ptMinTrackSize.y=r.bottom-r.top; return 0;
    }
    case WM_SIZE:
        if (wp==SIZE_MINIMIZED) nmc_renderer_reset(&s->renderer);
        else layout(s);
        return 0;
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
        if (busy(s) && !atomic_load(&s->task.cancel)) {
            s->stage=(NmcStage)wp; s->have_progress=true;
            s->done=(uint32_t)((uint64_t)lp>>32); s->total=(uint32_t)lp;
            s->bytes=atomic_load(&s->task.received_bytes);
            progress_text(s);
        }
        return 0;
    }
    case MSG_TRANSFER:
        if (busy(s) && !atomic_load(&s->task.cancel) && s->have_progress && (uint64_t)lp>s->bytes) {
            s->bytes=(uint64_t)lp;
            progress_text(s);
        }
        return 0;
    case MSG_MIRROR:
        s->mirrored=true;
        if (busy(s) && !atomic_load(&s->task.cancel) && s->have_progress) progress_text(s);
        return 0;
    case MSG_DEVICE: {
        Device *d=(Device *)lp;
        if (!s->closing && !nmc_cancelled(&s->task)) {
            wcscpy_s(s->verify_url,NMC_ARRAY(s->verify_url),d->url);
            wchar_t text[640];
            swprintf(text,NMC_ARRAY(text),L"验证代码：%ls\n%ls",d->code,d->url);
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
        free(s->versions); free(s->accounts); free(s->installed); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}
int nmc_ui_run(HINSTANCE module,int show) {
    Window *s=calloc(1,sizeof(*s));
    if (!s) return 1;
    s->module=module; s->task=(NmcTask){.progress=progress,.device=device,.mirror=mirror,.transfer=transfer,.user=s};
    nmc_task_reset(&s->task);
    WNDCLASSEXW cls={.cbSize=sizeof(cls),.lpfnWndProc=window_proc,.hInstance=module,
        .hIcon=LoadIconW(module,MAKEINTRESOURCEW(IDI_PUREMCL)),.hCursor=LoadCursorW(NULL,IDC_ARROW),
        .lpszClassName=L"PureMCL.Window"};
    if (!RegisterClassExW(&cls)) { free(s); return 1; }
    UINT dpi=GetDpiForSystem();
    RECT size={0,0,MulDiv(700,(int)dpi,96),MulDiv(570,(int)dpi,96)};
    AdjustWindowRectExForDpi(&size,WS_OVERLAPPEDWINDOW,FALSE,0,dpi);
    HWND w=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"PureMCL",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                           CW_USEDEFAULT,CW_USEDEFAULT,size.right-size.left,size.bottom-size.top,NULL,NULL,module,s);
    if (!w) { free(s); return 1; }
    ShowWindow(w,show); UpdateWindow(w); SetFocus(item(s,TAB));
    MSG msg; BOOL status;
    while ((status=GetMessageW(&msg,NULL,0,0))>0) {
        if (!IsDialogMessageW(w,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    free(s);
    return status<0?1:(int)msg.wParam;
}

