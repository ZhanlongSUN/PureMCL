#include "ui/ui.h"

int main(void) {
    if (FAILED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED))) return 1;
    HWND w=CreateWindowExW(0,L"STATIC",L"PureMCL graphics smoke test",WS_POPUP,0,0,620,520,NULL,NULL,
                           GetModuleHandleW(NULL),NULL);
    NmcRenderer r={0};
    int result=1;
    if (w) {
        nmc_renderer_init(&r);
        nmc_renderer_draw(&r,w,96,false,L"窗口测试",L"Direct2D / DirectWrite / WIC");
        bool created=r.target && r.mark && r.title && r.caption;
        SetWindowPos(w,NULL,0,0,700,570,SWP_NOZORDER|SWP_NOACTIVATE);
        nmc_renderer_draw(&r,w,96,true,L"Dark",L"Resize");
        created=created && r.target && r.mark && r.pixels.width==700;
        const UINT scales[]={144,192};
        for (size_t i=0;i<NMC_ARRAY(scales) && created;++i) {
            int width=MulDiv(570,(int)scales[i],96), height=MulDiv(550,(int)scales[i],96);
            nmc_renderer_reset(&r);
            SetWindowPos(w,NULL,0,0,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
            nmc_renderer_draw(&r,w,scales[i],true,L"在系统浏览器完成微软验证",
                              L"验证代码：TEST-000\nhttps://example.invalid/device");
            created=r.target && r.mark && r.pixels.width==(UINT32)width && r.pixels.height==(UINT32)height;
        }
        if (created) result=0;
        nmc_renderer_free(&r); DestroyWindow(w);
    }
    CoUninitialize();
    if (result) fprintf(stderr,"Graphics resources or render target resize failed\n");
    return result;
}

