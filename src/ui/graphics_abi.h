#ifndef NMC_GRAPHICS_ABI_H
#define NMC_GRAPHICS_ABI_H

/* The Microsoft SDK exposes the types and D2D factory entry point to C,
 * but only declares the graphics interface methods in its C++ headers.
 * These are the used slots of the original interfaces, in SDK order.
 * Unused slots are sized as function pointers; they are never called. */
typedef void (STDMETHODCALLTYPE *NmcUnusedSlot)(void);

typedef struct {
    NmcUnusedSlot unused_before_text_format[15];
    HRESULT (STDMETHODCALLTYPE *CreateTextFormat)(IDWriteFactory *,const WCHAR *,void *,
        int,int,int,FLOAT,const WCHAR *,IDWriteTextFormat **);
} NmcDwriteVtbl;
struct IDWriteFactory { const NmcDwriteVtbl *lpVtbl; };

typedef struct {
    NmcUnusedSlot unused_before_hwnd[14];
    HRESULT (STDMETHODCALLTYPE *CreateHwndRenderTarget)(ID2D1Factory *,const D2D1_RENDER_TARGET_PROPERTIES *,
        const D2D1_HWND_RENDER_TARGET_PROPERTIES *,ID2D1HwndRenderTarget **);
} NmcD2dFactoryVtbl;
struct ID2D1Factory { const NmcD2dFactoryVtbl *lpVtbl; };

typedef struct {
    NmcUnusedSlot unused_before_wic[5];
    HRESULT (STDMETHODCALLTYPE *CreateBitmapFromWicBitmap)(ID2D1HwndRenderTarget *,IWICBitmapSource *,
        const D2D1_BITMAP_PROPERTIES *,ID2D1Bitmap **);
    NmcUnusedSlot unused_before_brush[2];
    HRESULT (STDMETHODCALLTYPE *CreateSolidColorBrush)(ID2D1HwndRenderTarget *,const D2D1_COLOR_F *,
        const D2D1_BRUSH_PROPERTIES *,ID2D1SolidColorBrush **);
    NmcUnusedSlot unused_before_fill[8];
    void (STDMETHODCALLTYPE *FillRectangle)(ID2D1HwndRenderTarget *,const D2D1_RECT_F *,ID2D1Brush *);
    NmcUnusedSlot unused_before_bitmap[8];
    void (STDMETHODCALLTYPE *DrawBitmap)(ID2D1HwndRenderTarget *,ID2D1Bitmap *,const D2D1_RECT_F *,
        FLOAT,D2D1_BITMAP_INTERPOLATION_MODE,const D2D1_RECT_F *);
    void (STDMETHODCALLTYPE *DrawTextW)(ID2D1HwndRenderTarget *,const WCHAR *,UINT32,IDWriteTextFormat *,
        const D2D1_RECT_F *,ID2D1Brush *,D2D1_DRAW_TEXT_OPTIONS,DWRITE_MEASURING_MODE);
    NmcUnusedSlot unused_before_clear[19];
    void (STDMETHODCALLTYPE *Clear)(ID2D1HwndRenderTarget *,const D2D1_COLOR_F *);
    void (STDMETHODCALLTYPE *BeginDraw)(ID2D1HwndRenderTarget *);
    HRESULT (STDMETHODCALLTYPE *EndDraw)(ID2D1HwndRenderTarget *,D2D1_TAG *,D2D1_TAG *);
    NmcUnusedSlot unused_before_resize[8];
    HRESULT (STDMETHODCALLTYPE *Resize)(ID2D1HwndRenderTarget *,const D2D1_SIZE_U *);
} NmcD2dHwndVtbl;
struct ID2D1HwndRenderTarget { const NmcD2dHwndVtbl *lpVtbl; };

typedef struct {
    NmcUnusedSlot unused_before_color[8];
    void (STDMETHODCALLTYPE *SetColor)(ID2D1SolidColorBrush *,const D2D1_COLOR_F *);
} NmcD2dColorVtbl;
struct ID2D1SolidColorBrush { const NmcD2dColorVtbl *lpVtbl; };

__declspec(dllimport) HRESULT WINAPI DWriteCreateFactory(int,REFIID,IUnknown **);
static const IID nmc_dwrite_iid={0xb859ee5a,0xd838,0x4b5b,{0xa2,0xe8,0x1a,0xdc,0x7d,0x93,0xdb,0x48}};
static const IID nmc_d2d_iid={0x06152247,0x6f50,0x465a,{0x92,0x45,0x11,0x8b,0xfd,0x3b,0x60,0x07}};
#define IID_IDWriteFactory nmc_dwrite_iid
#define IID_ID2D1Factory nmc_d2d_iid
#define DWRITE_FACTORY_TYPE_SHARED 0
#define DWRITE_FONT_WEIGHT_SEMI_BOLD 600
#define DWRITE_FONT_WEIGHT_NORMAL 400
#define DWRITE_FONT_STYLE_NORMAL 0
#define DWRITE_FONT_STRETCH_NORMAL 5

#define nmc_com_release(p) IUnknown_Release((IUnknown *)(p))
#define ID2D1Bitmap_Release nmc_com_release
#define ID2D1Factory_Release nmc_com_release
#define ID2D1HwndRenderTarget_Release nmc_com_release
#define ID2D1SolidColorBrush_Release nmc_com_release
#define IDWriteFactory_Release nmc_com_release
#define IDWriteTextFormat_Release nmc_com_release
#define IDWriteFactory_CreateTextFormat(p,...) ((p)->lpVtbl->CreateTextFormat((p),__VA_ARGS__))
#define ID2D1Factory_CreateHwndRenderTarget(p,...) ((p)->lpVtbl->CreateHwndRenderTarget((p),__VA_ARGS__))
#define ID2D1HwndRenderTarget_Resize(p,...) ((p)->lpVtbl->Resize((p),__VA_ARGS__))
#define ID2D1HwndRenderTarget_CreateSolidColorBrush(p,...) ((p)->lpVtbl->CreateSolidColorBrush((p),__VA_ARGS__))
#define ID2D1HwndRenderTarget_CreateBitmapFromWicBitmap(p,...) ((p)->lpVtbl->CreateBitmapFromWicBitmap((p),__VA_ARGS__))
#define ID2D1SolidColorBrush_SetColor(p,...) ((p)->lpVtbl->SetColor((p),__VA_ARGS__))
#define ID2D1HwndRenderTarget_FillRectangle(p,...) ((p)->lpVtbl->FillRectangle((p),__VA_ARGS__))
#define ID2D1HwndRenderTarget_DrawText(p,...) ((p)->lpVtbl->DrawTextW((p),__VA_ARGS__))
#define ID2D1HwndRenderTarget_DrawBitmap(p,...) ((p)->lpVtbl->DrawBitmap((p),__VA_ARGS__))
#define ID2D1HwndRenderTarget_BeginDraw(p) ((p)->lpVtbl->BeginDraw((p)))
#define ID2D1HwndRenderTarget_Clear(p,...) ((p)->lpVtbl->Clear((p),__VA_ARGS__))
#define ID2D1HwndRenderTarget_EndDraw(p,...) ((p)->lpVtbl->EndDraw((p),__VA_ARGS__))
#endif

