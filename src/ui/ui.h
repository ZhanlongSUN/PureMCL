#ifndef NMC_UI_H
#define NMC_UI_H

#define COBJMACROS
#include "internal.h"
#include <d2d1.h>
#include <wincodec.h>
typedef struct IDWriteFactory IDWriteFactory;

typedef struct {
    ID2D1Factory *d2d;
    IDWriteFactory *write;
    IWICImagingFactory *wic;
    ID2D1HwndRenderTarget *target;
    ID2D1SolidColorBrush *brush;
    IDWriteTextFormat *title, *caption;
    ID2D1Bitmap *mark;
    D2D1_SIZE_U pixels;
} NmcRenderer;

void nmc_renderer_init(NmcRenderer *);
void nmc_renderer_reset(NmcRenderer *);
void nmc_renderer_free(NmcRenderer *);
void nmc_renderer_draw(NmcRenderer *,HWND,UINT,bool,const wchar_t *,const wchar_t *);
int nmc_ui_run(HINSTANCE,int,LARGE_INTEGER);

#endif

