// imgui-sw: Dear ImGui software (CPU) renderer as an independent library (experimental)
// MIT License. See Dear ImGui license as well.
//
// This library renders Dear ImGui draw data into a user-supplied framebuffer
// without requiring any GPU. Good for small embedded displays or offscreen tests.
//
// Minimal API:
//   bool  ImGuiSW_Init(int width, int height, void* pixels, int pitch_bytes, int pixel_format);
//   void  ImGuiSW_NewFrame(int width, int height, void* pixels, int pitch_bytes);
//   void  ImGuiSW_RenderDrawData(ImDrawData*);
//   void  ImGuiSW_Shutdown();
//
// Pixel formats:
//   0 = RGBA8888, 1 = BGRA8888, 2 = RGB565
//
// Optional texture mapping:
//   ImTextureID ImGuiSW_CreateTexture(const void* pixels, int w, int h, int bpp);
//   void        ImGuiSW_DestroyAllUserTextures();
//
// Integration notes:
//   - Include <imgui.h> and build Dear ImGui core .cpp files alongside this library.
//   - On embedded, point 'pixels' to your LCD framebuffer, then flush to panel after Render.

#pragma once

#include "imgui.h"
#include <stdint.h>

// Optional: pick up ESP-IDF sdkconfig symbols if available
#if defined(__has_include)
#  if __has_include("sdkconfig.h")
#    include "sdkconfig.h"
#  endif
#endif

// Compile-time feature toggles (can be overridden by -D on the compiler cmdline)
#ifndef IMGUISW_ENABLE_DIRTY_RECTS
#  ifdef CONFIG_IMGUISW_ENABLE_DIRTY_RECTS
#    define IMGUISW_ENABLE_DIRTY_RECTS 1
#  else
#    define IMGUISW_ENABLE_DIRTY_RECTS 0
#  endif
#endif

#ifndef IMGUISW_AUTO_FLUSH
#  ifdef CONFIG_IMGUISW_AUTO_FLUSH
#    define IMGUISW_AUTO_FLUSH 1
#  else
#    define IMGUISW_AUTO_FLUSH 1
#  endif
#endif

namespace ImGuiSW
{
    enum PixelFormat
    {
        RGBA32 = 0,
        BGRA32 = 1,
        RGB565 = 2
    };

    bool Init(int width, int height, void* pixels, int pitch_bytes, PixelFormat fmt);
    void Shutdown();

    void NewFrame(int width, int height, void* pixels, int pitch_bytes);
    void RenderDrawData(ImDrawData* draw_data);

    // Simple optional texture registry
    ImTextureID CreateTexture(const void* pixels, int width, int height, int bytes_per_pixel);
    void        DestroyAllUserTextures();

#if IMGUISW_ENABLE_DIRTY_RECTS
    // Dirty-rects support: rectangle type
    struct Rect { int x1, y1, x2, y2; }; // half-open [x1,x2) x [y1,y2)

    // Flush callback signature: push rects from the framebuffer to the display
    typedef void (*FlushRectCallback)(
        const uint8_t* framebuffer,
        int pitch_bytes,
        PixelFormat fmt,
        const Rect* rects,
        int rect_count,
        void* user_data);

    struct DirtyRectsConfig {
        int max_rects = 12;   // cap number of rects; fallback to full-screen if exceeded
        int inflate_px = 1;   // expand each rect to avoid AA edge leaks
        int merge_dist = 2;   // merge rects closer than this distance (in pixels)
        int align_px  = 1;    // align rect edges to N-pixel boundaries for DMA efficiency
    };

    // Install/uninstall a flush callback. If auto-flush is enabled, RenderDrawData() will
    // compute and call the callback after rendering; otherwise call Present() manually.
    void SetFlushCallback(FlushRectCallback cb, void* user_data, const DirtyRectsConfig& cfg);
    void SetAutoFlush(bool enabled);
    void Present(); // call the flush callback (if set) with latest computed rects

    // Utility: compute dirty rects from ImDrawData (can be used standalone).
    void ComputeDirtyRects(ImDrawData* dd, ImVector<Rect>& out,
                           int screen_w, int screen_h,
                           int inflate_px, int merge_dist, int max_rects, int align_px);
#endif
}
