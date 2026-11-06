/**
 * rhi.c -- forwards rhi.h to the backend in use (rhi_backend.h).
 *
 * GPU calls are forwarding only: the one place a call from the
 * renderer meets the backend table, so a breakpoint here sees every GPU
 * call either backend makes. The small host boundary keeps OS window/events
 * outside the common Xbox device implementation.
 */

#include "rhi_backend.h"
#include "graphics/present_metrics.h"
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static RhiHost g_host;
void rhi_set_host(const RhiHost *host)
{
    if(host)g_host=*host;
    else memset(&g_host,0,sizeof g_host);
}
void rhi_pump_events(void)
{
    if(g_host.pump_events) {g_host.pump_events(g_host.context);return;}
#if defined(_WIN32)
    MSG msg;
    while(PeekMessageA(&msg,NULL,0,0,PM_REMOVE)) {
        /* Normal window close must run registered cleanup/health reports.
         * ExitProcess bypasses CRT atexit handlers on Windows. */
        if(msg.message==WM_QUIT)exit(0);
        TranslateMessage(&msg);DispatchMessageA(&msg);
    }
#endif
}
int rhi_drawable_size(void *window,uint32_t *width,uint32_t *height)
{
    uint32_t w=0,h=0;
    if(!width || !height)return 0;
    if(g_host.drawable_size) {
        if(!g_host.drawable_size(g_host.context,window,&w,&h) || !w || !h)return 0;
    } else {
#if defined(_WIN32)
        RECT rect;
        if(!window || !GetClientRect((HWND)window,&rect) ||
           rect.right<=rect.left || rect.bottom<=rect.top)return 0;
        w=(uint32_t)(rect.right-rect.left);h=(uint32_t)(rect.bottom-rect.top);
#else
        (void)window;return 0;
#endif
    }
    *width=w;*height=h;return 1;
}

int rhi_text_bitmap(const char *text,unsigned char *pixels,uint32_t width,uint32_t height,uint32_t *used_width)
{
    uint32_t used=0;
    if(!text || !pixels || !used_width || !width || !height || width>16384 || height>4096)return 0;
    if(g_host.text_bitmap) {
        if(!g_host.text_bitmap(g_host.context,text,pixels,width,height,&used) || !used || used>width)return 0;
    } else {
#if defined(_WIN32)
        HDC dc=CreateCompatibleDC(NULL);
        BITMAPINFO bi={0};void *bits=NULL;
        if(!dc)return 0;
        bi.bmiHeader.biSize=sizeof bi.bmiHeader;bi.bmiHeader.biWidth=(LONG)width;
        bi.bmiHeader.biHeight=-(LONG)height;bi.bmiHeader.biPlanes=1;
        bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
        HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,NULL,0);
        HFONT font=CreateFontW(-18,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
            FIXED_PITCH|FF_MODERN,L"Consolas");
        if(!bitmap || !font) {DeleteDC(dc);if(bitmap)DeleteObject(bitmap);if(font)DeleteObject(font);return 0;}
        SelectObject(dc,bitmap);SelectObject(dc,font);
        SetBkMode(dc,OPAQUE);SetBkColor(dc,RGB(0,0,0));SetTextColor(dc,RGB(255,255,255));
        wchar_t wide[384];size_t count=strlen(text);if(count>383)count=383;
        for(size_t i=0;i<count;i++)wide[i]=(wchar_t)(unsigned char)text[i];
        RECT rect={0,0,(LONG)width,(LONG)height};SIZE extent={0};
        memset(bits,0,(size_t)width*height*4);
        int ok=GetTextExtentPoint32W(dc,wide,(int)count,&extent) &&
            ExtTextOutW(dc,6,4,ETO_OPAQUE,&rect,wide,(UINT)count,NULL);
        GdiFlush();
        if(ok){memcpy(pixels,bits,(size_t)width*height*4);used=(uint32_t)extent.cx+12;if(used>width)used=width;}
        DeleteDC(dc);DeleteObject(bitmap);DeleteObject(font);
        if(!ok || !used)return 0;
#else
        return 0;
#endif
    }
    *used_width=used;return 1;
}

#if defined(_WIN32)
const RhiBackend *g_rhi = &rhi_d3d11_backend;
#elif defined(RHI_HAVE_VULKAN)
const RhiBackend *g_rhi = &rhi_vulkan_backend;
#else
const RhiBackend *g_rhi = NULL;
#endif

/* Every backend this build carries, first is the default. */
static const RhiBackend *const g_backends[] = {
#if defined(_WIN32)
    &rhi_d3d11_backend,
#endif
#if defined(RHI_HAVE_VULKAN)
    &rhi_vulkan_backend,
#endif
    NULL
};

int rhi_select_backend(const char *name)
{
    int i;

    if (!name || !*name)
        name = getenv("RECOMP_D3D8_BACKEND");
    if (!name || !*name)
        return 0;
    for (i = 0; g_backends[i]; i++) {
        if (strcmp(g_backends[i]->name, name) == 0) {
            g_rhi = g_backends[i];
            return 0;
        }
    }
    fprintf(stderr, "[RHI] renderer backend \"%s\" is not in this build; using %s\n",
            name, g_rhi ? g_rhi->name : "none");
    fflush(stderr);
    return -1;
}

const char *rhi_backend_name(void) { return g_rhi ? g_rhi->name : "none"; }

/* A backend that was asked for and cannot start (no loader, no Vulkan 1.3
 * device, no shader compiler) gives way to the default, said once, rather
 * than leaving the title with no picture at all. */
int rhi_device_create(const RhiDeviceDesc *d)
{
    if (!g_rhi)
        return -1;
    if (g_rhi->device_create(d) == 0)
        return 0;
    if (g_rhi != g_backends[0] && g_backends[0]) {
        fprintf(stderr, "[RHI] the %s renderer could not start; using %s\n",
                g_rhi->name, g_backends[0]->name);
        fflush(stderr);
        g_rhi->device_destroy();
        g_rhi = g_backends[0];
        return g_rhi->device_create(d);
    }
    return -1;
}
void     rhi_device_destroy(void)                      { if (g_rhi) g_rhi->device_destroy(); }
int      rhi_device_ready(void)                        { return g_rhi && g_rhi->device_ready(); }
RhiView *rhi_swapchain_view(void)                      { return g_rhi ? g_rhi->swapchain_view() : NULL; }
int      rhi_swapchain_resize(uint32_t w, uint32_t h)  { return g_rhi ? g_rhi->swapchain_resize(w, h) : -1; }
static TsfpPresentMetrics g_present_metrics;
static uint64_t present_clock_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
    return (uint64_t)((double)now.QuadPart * 1e9 / (double)frequency.QuadPart);
#else
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)!=0)return 0;
    return (uint64_t)now.tv_sec*1000000000u+(uint64_t)now.tv_nsec;
#endif
}
int32_t rhi_present_kind(uint32_t interval, uint32_t kind)
{
    uint64_t start=present_clock_ns();
    int32_t result=g_rhi ? g_rhi->present(interval) : -1;
    tsfp_present_record(&g_present_metrics,rhi_backend_name(),kind,result,start,present_clock_ns());
    return result;
}
int32_t rhi_present(uint32_t interval) {return rhi_present_kind(interval,RHI_PRESENT_OTHER);}
int      rhi_swapchain_readback(void *dst, uint32_t pitch, uint32_t *w, uint32_t *h)
{
    return g_rhi ? g_rhi->swapchain_readback(dst, pitch, w, h) : -1;
}
void     rhi_swapchain_set_vrr(int on)                 { if (g_rhi) g_rhi->swapchain_set_vrr(on); }

RhiBuffer *rhi_buffer_create(const RhiBufferDesc *d, const void *init) { return g_rhi->buffer_create(d, init); }
void  rhi_buffer_destroy(RhiBuffer *b)                  { if (b) g_rhi->buffer_destroy(b); }
void *rhi_buffer_map(RhiBuffer *b, uint32_t mode)       { return g_rhi->buffer_map(b, mode); }
void  rhi_buffer_unmap(RhiBuffer *b)                    { g_rhi->buffer_unmap(b); }
void  rhi_buffer_update(RhiBuffer *b, const void *data) { g_rhi->buffer_update(b, data); }

RhiImage *rhi_image_create(const RhiImageDesc *d, const RhiSubresourceData *init) { return g_rhi->image_create(d, init); }
RhiImage *rhi_image_retain(RhiImage *img)                        { return img ? g_rhi->image_retain(img) : NULL; }
void rhi_image_destroy(RhiImage *img)                            { if (img) g_rhi->image_destroy(img); }
void rhi_image_get_desc(const RhiImage *img, RhiImageDesc *out)  { g_rhi->image_get_desc(img, out); }
void rhi_image_update(RhiImage *img, uint32_t sub, const RhiBox *box,
                      const void *data, uint32_t row_pitch, uint32_t slice_pitch)
{
    g_rhi->image_update(img, sub, box, data, row_pitch, slice_pitch);
}
int rhi_image_readback(RhiImage *img, uint32_t sub, void *dst, uint32_t dst_pitch)
{
    return g_rhi->image_readback(img, sub, dst, dst_pitch);
}

RhiView  *rhi_view_create(RhiImage *img, uint32_t kind, const RhiViewDesc *d) { return g_rhi->view_create(img, kind, d); }
void      rhi_view_destroy(RhiView *v)       { if (v) g_rhi->view_destroy(v); }
RhiImage *rhi_view_image(const RhiView *v)   { return v ? g_rhi->view_image(v) : NULL; }

int rhi_sample_count_supported(RhiFormat f, uint32_t samples)   { return g_rhi->sample_count_supported(f, samples); }

/* Bytes per texel, or per 4x4 block for the BC formats. */
static uint32_t format_block_bytes(RhiFormat f, int *block)
{
    *block = 0;
    switch (f) {
    case RHI_FORMAT_R32G32B32A32_FLOAT: case RHI_FORMAT_R32G32B32A32_SINT:
        return 16;
    case RHI_FORMAT_R32G32B32_FLOAT:
        return 12;
    case RHI_FORMAT_R16G16B16A16_FLOAT: case RHI_FORMAT_R16G16B16A16_UNORM:
    case RHI_FORMAT_R16G16B16A16_SNORM: case RHI_FORMAT_R32G32_FLOAT:
        return 8;
    case RHI_FORMAT_R10G10B10A2_UNORM: case RHI_FORMAT_R11G11B10_FLOAT:
    case RHI_FORMAT_R8G8B8A8_UNORM: case RHI_FORMAT_R8G8B8A8_SNORM:
    case RHI_FORMAT_R16G16_FLOAT: case RHI_FORMAT_R16G16_UNORM: case RHI_FORMAT_R16G16_SNORM:
    case RHI_FORMAT_D32_FLOAT: case RHI_FORMAT_R32_FLOAT: case RHI_FORMAT_R32_UINT:
    case RHI_FORMAT_D24_UNORM_S8_UINT:
    case RHI_FORMAT_B8G8R8A8_UNORM: case RHI_FORMAT_B8G8R8X8_UNORM:
        return 4;
    case RHI_FORMAT_R8G8_UNORM: case RHI_FORMAT_R8G8_SNORM: case RHI_FORMAT_R16_FLOAT:
    case RHI_FORMAT_D16_UNORM: case RHI_FORMAT_R16_UNORM: case RHI_FORMAT_R16_UINT:
    case RHI_FORMAT_R16_SNORM: case RHI_FORMAT_B5G6R5_UNORM: case RHI_FORMAT_B5G5R5A1_UNORM:
    case RHI_FORMAT_B4G4R4A4_UNORM:
        return 2;
    case RHI_FORMAT_R8_UNORM: case RHI_FORMAT_A8_UNORM:
        return 1;
    case RHI_FORMAT_BC1_UNORM:
        *block = 1;
        return 8;
    case RHI_FORMAT_BC2_UNORM: case RHI_FORMAT_BC3_UNORM: case RHI_FORMAT_BC5_UNORM:
        *block = 1;
        return 16;
    default:
        return 0;
    }
}

uint32_t rhi_format_row_pitch(RhiFormat f, uint32_t width)
{
    int block;
    uint32_t bytes = format_block_bytes(f, &block);
    return block ? ((width + 3) / 4) * bytes : width * bytes;
}

uint32_t rhi_format_rows(RhiFormat f, uint32_t height)
{
    int block;
    (void)format_block_bytes(f, &block);
    return block ? (height + 3) / 4 : height;
}

RhiShader *rhi_shader_create(uint32_t stage, const RhiShaderSource *src, char *err, size_t err_len)
{
    return g_rhi->shader_create(stage, src, err, err_len);
}
void rhi_shader_destroy(RhiShader *s) { if (s) g_rhi->shader_destroy(s); }
RhiVertexLayout *rhi_vertex_layout_create(const RhiVertexElement *e, uint32_t n, const RhiShader *vs)
{
    return g_rhi->vertex_layout_create(e, n, vs);
}
void rhi_vertex_layout_destroy(RhiVertexLayout *l) { if (l) g_rhi->vertex_layout_destroy(l); }

RhiBlendState  *rhi_blend_state_create(const RhiBlendDesc *d)   { return g_rhi->blend_state_create(d); }
RhiDepthState  *rhi_depth_state_create(const RhiDepthDesc *d)   { return g_rhi->depth_state_create(d); }
RhiRasterState *rhi_raster_state_create(const RhiRasterDesc *d) { return g_rhi->raster_state_create(d); }
RhiSampler     *rhi_sampler_create(const RhiSamplerDesc *d)     { return g_rhi->sampler_create(d); }
void rhi_blend_state_destroy(RhiBlendState *s)   { if (s) g_rhi->blend_state_destroy(s); }
void rhi_depth_state_destroy(RhiDepthState *s)   { if (s) g_rhi->depth_state_destroy(s); }
void rhi_raster_state_destroy(RhiRasterState *s) { if (s) g_rhi->raster_state_destroy(s); }
void rhi_sampler_destroy(RhiSampler *s)          { if (s) g_rhi->sampler_destroy(s); }

void rhi_set_render_target(RhiView *c, RhiView *d)            { g_rhi->set_render_target(c, d); }
void rhi_output_save(RhiOutputState *s)                       { g_rhi->output_save(s); }
void rhi_output_restore(RhiOutputState *s)                    { g_rhi->output_restore(s); }
int  rhi_output_color_is(const RhiOutputState *s, const RhiView *v) { return g_rhi->output_color_is(s, v); }
void rhi_set_viewports(uint32_t n, const RhiViewport *vps)    { g_rhi->set_viewports(n, vps); }
void rhi_set_scissor(const RhiRect *r)                        { g_rhi->set_scissor(r); }
void rhi_set_topology(uint32_t t)                             { g_rhi->set_topology(t); }
void rhi_set_vertex_layout(RhiVertexLayout *l)                { g_rhi->set_vertex_layout(l); }
void rhi_set_vertex_buffer(uint32_t slot, RhiBuffer *b, uint32_t stride, uint32_t offset)
{
    g_rhi->set_vertex_buffer(slot, b, stride, offset);
}
void rhi_set_index_buffer(RhiBuffer *b, uint32_t bits, uint32_t offset) { g_rhi->set_index_buffer(b, bits, offset); }
void rhi_set_shader(uint32_t stage, RhiShader *s)             { g_rhi->set_shader(stage, s); }
void rhi_set_uniform_buffers(uint32_t stage, uint32_t slot, uint32_t n, RhiBuffer *const *b)
{
    g_rhi->set_uniform_buffers(stage, slot, n, b);
}
void rhi_set_textures(uint32_t slot, uint32_t n, RhiView *const *v)     { g_rhi->set_textures(slot, n, v); }
void rhi_set_samplers(uint32_t slot, uint32_t n, RhiSampler *const *s)  { g_rhi->set_samplers(slot, n, s); }
void rhi_set_blend_state(RhiBlendState *s, const float f[4], uint32_t mask) { g_rhi->set_blend_state(s, f, mask); }
void rhi_set_depth_state(RhiDepthState *s, uint32_t ref)      { g_rhi->set_depth_state(s, ref); }
void rhi_set_raster_state(RhiRasterState *s)                  { g_rhi->set_raster_state(s); }
void rhi_draw(uint32_t n, uint32_t first)                     { g_rhi->draw(n, first); }
void rhi_draw_indexed(uint32_t n, uint32_t first, int32_t base) { g_rhi->draw_indexed(n, first, base); }
void rhi_clear_color(RhiView *v, const float rgba[4])         { g_rhi->clear_color(v, rgba); }
void rhi_clear_depth(RhiView *v, uint32_t flags, float z, uint8_t s) { g_rhi->clear_depth(v, flags, z, s); }
