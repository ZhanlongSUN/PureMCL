#include "ui.h"
#include "graphics_abi.h"

#define RELEASE(i,method) do { if (i) { method(i); (i)=NULL; } } while (0)
static D2D1_COLOR_F color(unsigned rgb) {
    return (D2D1_COLOR_F){(float)((rgb>>16)&255)/255.0f,(float)((rgb>>8)&255)/255.0f,(float)(rgb&255)/255.0f,1.0f};
}
void nmc_renderer_init(NmcRenderer *r) {
    D2D1_FACTORY_OPTIONS options={0};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&IID_ID2D1Factory,&options,(void **)&r->d2d);
    if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,&IID_IDWriteFactory,(IUnknown **)&r->write))) {
        IDWriteFactory_CreateTextFormat(r->write,L"Segoe UI Variable Display",NULL,DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,29.0f,L"zh-CN",&r->title);
        IDWriteFactory_CreateTextFormat(r->write,L"Segoe UI",NULL,DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,13.0f,L"zh-CN",&r->caption);
    }
    CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void **)&r->wic);
}
void nmc_renderer_reset(NmcRenderer *r) {
    RELEASE(r->mark,ID2D1Bitmap_Release); RELEASE(r->brush,ID2D1SolidColorBrush_Release);
    RELEASE(r->target,ID2D1HwndRenderTarget_Release);
}
void nmc_renderer_free(NmcRenderer *r) {
    nmc_renderer_reset(r);
    RELEASE(r->title,IDWriteTextFormat_Release); RELEASE(r->caption,IDWriteTextFormat_Release);
    RELEASE(r->write,IDWriteFactory_Release); RELEASE(r->wic,IWICImagingFactory_Release); RELEASE(r->d2d,ID2D1Factory_Release);
}
static void bitmap(NmcRenderer *r) {
    if (!r->wic) return;
    HMODULE mod=GetModuleHandleW(NULL);
    HRSRC res=FindResourceW(mod,MAKEINTRESOURCEW(101),RT_RCDATA);
    if (!res) return;
    HGLOBAL bytes=LoadResource(mod,res);
    BYTE *ptr=bytes?LockResource(bytes):NULL;
    DWORD len=SizeofResource(mod,res);
    IWICStream *stream=NULL; IWICBitmapDecoder *decoder=NULL; IWICBitmapFrameDecode *frame=NULL;
    IWICFormatConverter *conv=NULL;
    if (ptr && len && SUCCEEDED(IWICImagingFactory_CreateStream(r->wic,&stream)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(stream,ptr,len)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(r->wic,(IStream *)stream,NULL,WICDecodeMetadataCacheOnLoad,&decoder)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(decoder,0,&frame)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(r->wic,&conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv,(IWICBitmapSource *)frame,&GUID_WICPixelFormat32bppPBGRA,
                                                 WICBitmapDitherTypeNone,NULL,0.0,WICBitmapPaletteTypeCustom)))
        ID2D1HwndRenderTarget_CreateBitmapFromWicBitmap(r->target,(IWICBitmapSource *)conv,NULL,&r->mark);
    RELEASE(conv,IWICFormatConverter_Release); RELEASE(frame,IWICBitmapFrameDecode_Release);
    RELEASE(decoder,IWICBitmapDecoder_Release); RELEASE(stream,IWICStream_Release);
}
static bool target(NmcRenderer *r,HWND w,UINT dpi,D2D1_SIZE_U size) {
    if (!r->d2d || !r->title || !r->caption) return false;
    if (r->target) {
        if (size.width==r->pixels.width && size.height==r->pixels.height) return true;
        if (SUCCEEDED(ID2D1HwndRenderTarget_Resize(r->target,&size))) { r->pixels=size; return true; }
        nmc_renderer_reset(r);
    }
    D2D1_RENDER_TARGET_PROPERTIES prop={.dpiX=(float)dpi,.dpiY=(float)dpi};
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd={.hwnd=w,.pixelSize=size};
    D2D1_COLOR_F c=color(0xffffff);
    if (FAILED(ID2D1Factory_CreateHwndRenderTarget(r->d2d,&prop,&hwnd,&r->target)) ||
        FAILED(ID2D1HwndRenderTarget_CreateSolidColorBrush(r->target,&c,NULL,&r->brush))) {
        nmc_renderer_reset(r); return false;
    }
    r->pixels=size; bitmap(r); return true;
}
static void fill(NmcRenderer *r,D2D1_RECT_F box,unsigned rgb) {
    D2D1_COLOR_F c=color(rgb);
    ID2D1SolidColorBrush_SetColor(r->brush,&c);
    ID2D1HwndRenderTarget_FillRectangle(r->target,&box,(ID2D1Brush *)r->brush);
}
static void draw(NmcRenderer *r,const wchar_t *s,IDWriteTextFormat *fmt,D2D1_RECT_F box,unsigned rgb) {
    D2D1_COLOR_F c=color(rgb);
    ID2D1SolidColorBrush_SetColor(r->brush,&c);
    ID2D1HwndRenderTarget_DrawText(r->target,s,(UINT32)wcslen(s),fmt,&box,(ID2D1Brush *)r->brush,
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP,DWRITE_MEASURING_MODE_NATURAL);
}
void nmc_renderer_draw(NmcRenderer *r,HWND w,UINT dpi,bool dark,const wchar_t *status,const wchar_t *sub) {
    PAINTSTRUCT ps; HDC dc=BeginPaint(w,&ps); RECT px;
    GetClientRect(w,&px);
    if (!IsIconic(w) && px.right>0 && px.bottom>0) {
        if (target(r,w,dpi,(D2D1_SIZE_U){(UINT32)px.right,(UINT32)px.bottom})) {
            float x=(float)px.right*96.0f/(float)dpi, y=(float)px.bottom*96.0f/(float)dpi;
            D2D1_COLOR_F base=color(dark?0x191e24:0xf2f5f3);
            ID2D1HwndRenderTarget_BeginDraw(r->target);
            ID2D1HwndRenderTarget_Clear(r->target,&base);
            fill(r,(D2D1_RECT_F){0,0,x,110},dark?0x142820:0xe5efe8);
            fill(r,(D2D1_RECT_F){0,108,x,110},dark?0x4fbb86:0x25835c);
            if (r->mark) {
                D2D1_RECT_F rect={24,24,76,76};
                ID2D1HwndRenderTarget_DrawBitmap(r->target,r->mark,&rect,1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,NULL);
            }
            draw(r,L"PureMCL",r->title,(D2D1_RECT_F){90,17,x-20,59},dark?0xf3fbf7:0x15372a);
            draw(r,L"MINECRAFT  /  JAVA EDITION",r->caption,(D2D1_RECT_F){92,62,x-20,86},dark?0xafcbbd:0x426355);
            draw(r,status,r->caption,(D2D1_RECT_F){26,y-94,x-22,y-70},dark?0xf1f5f3:0x193429);
            draw(r,sub,r->caption,(D2D1_RECT_F){26,y-67,x-22,y-44},dark?0xb1c5bc:0x426355);
            if (ID2D1HwndRenderTarget_EndDraw(r->target,NULL,NULL)==D2DERR_RECREATE_TARGET) {
                nmc_renderer_reset(r); InvalidateRect(w,NULL,FALSE);
            }
        } else {
            FillRect(dc,&px,GetSysColorBrush(COLOR_WINDOW));
            SetBkMode(dc,TRANSPARENT);
            TextOutW(dc,24,26,L"PureMCL",7);
            TextOutW(dc,24,px.bottom-MulDiv(94,(int)dpi,96),status,(int)wcslen(status));
        }
    }
    EndPaint(w,&ps);
}

