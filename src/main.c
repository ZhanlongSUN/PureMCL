#include "ui/ui.h"
#include <commctrl.h>

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE prev,PWSTR cmd,int show) {
    (void)prev; (void)cmd;
    HRESULT hr=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_TAB_CLASSES|ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES};
    if (!InitCommonControlsEx(&controls)) { CoUninitialize(); return 1; }
    int result=nmc_ui_run(instance,show);
    CoUninitialize();
    return result;
}

