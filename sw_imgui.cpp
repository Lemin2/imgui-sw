// imgui-sw: Dear ImGui software (CPU) renderer as an independent library
// Implementation adapted from in-repo experiment, kept self-contained.

#include "sw_imgui.h"
#include "imgui_internal.h"
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace ImGuiSW
{
struct Context
{
    uint8_t* Pixels = nullptr;
    int Width = 0, Height = 0, Pitch = 0;
    PixelFormat Format = RGBA32;

    const uint8_t* FontPixels = nullptr;
    int FontW = 0, FontH = 0, FontBpp = 1;

    struct Entry { ImTextureID id; const uint8_t* pixels; int w, h, bpp; };
    std::vector<Entry> UserTextures;

    ImTextureID FontTextureID = (ImTextureID)1; // non-zero sentinel

#if IMGUISW_ENABLE_DIRTY_RECTS
    // Dirty rects state
    ImGuiSW::FlushRectCallback FlushCb = nullptr;
    void* FlushUser = nullptr;
    ImGuiSW::DirtyRectsConfig DirtyCfg{};
    bool AutoFlush = IMGUISW_AUTO_FLUSH != 0;
    ImVector<ImGuiSW::Rect> DirtyRects; // latest frame
#endif
};
static Context* G = nullptr;

static inline void UnpackColor(ImU32 c, int& r, int& g, int& b, int& a)
{
#ifdef IMGUI_USE_BGRA_PACKED_COLOR
    a = (c >> IM_COL32_A_SHIFT) & 0xFF;
    r = (c >> IM_COL32_R_SHIFT) & 0xFF;
    g = (c >> IM_COL32_G_SHIFT) & 0xFF;
    b = (c >> IM_COL32_B_SHIFT) & 0xFF;
#else
    r = (c >> IM_COL32_R_SHIFT) & 0xFF;
    g = (c >> IM_COL32_G_SHIFT) & 0xFF;
    b = (c >> IM_COL32_B_SHIFT) & 0xFF;
    a = (c >> IM_COL32_A_SHIFT) & 0xFF;
#endif
}

static inline void WriteDestPixel(uint8_t* dst, PixelFormat fmt, int r, int g, int b, int a)
{
    if (fmt == RGBA32)
    { dst[0]= (uint8_t)r; dst[1]=(uint8_t)g; dst[2]=(uint8_t)b; dst[3]=(uint8_t)a; }
    else if (fmt == BGRA32)
    { dst[0]= (uint8_t)b; dst[1]=(uint8_t)g; dst[2]=(uint8_t)r; dst[3]=(uint8_t)a; }
    else
    {
        uint16_t R5 = (uint16_t)((r * 31 + 127) / 255);
        uint16_t G6 = (uint16_t)((g * 63 + 127) / 255);
        uint16_t B5 = (uint16_t)((b * 31 + 127) / 255);
        uint16_t v = (uint16_t)((R5 << 11) | (G6 << 5) | B5);
        dst[0] = (uint8_t)(v & 0xFF);
        dst[1] = (uint8_t)(v >> 8);
    }
}

static inline void ReadDestPixel(uint8_t* dst, PixelFormat fmt, int& r, int& g, int& b, int& a)
{
    if (fmt == RGBA32)
    { r = dst[0]; g = dst[1]; b = dst[2]; a = dst[3]; }
    else if (fmt == BGRA32)
    { b = dst[0]; g = dst[1]; r = dst[2]; a = dst[3]; }
    else
    {
        uint16_t v = (uint16_t)(dst[0] | (dst[1] << 8));
        r = ((v >> 11) & 31) * 255 / 31;
        g = ((v >> 5) & 63) * 255 / 63;
        b = (v & 31) * 255 / 31;
        a = 255;
    }
}

static inline float SampleTextureAlpha(ImTextureID tex_id, float u, float v)
{
    if (tex_id == G->FontTextureID && G->FontPixels)
    {
        int x = (int)std::floor(u * G->FontW);
        int y = (int)std::floor(v * G->FontH);
        x = std::clamp(x, 0, G->FontW - 1);
        y = std::clamp(y, 0, G->FontH - 1);
        if (G->FontBpp == 1)
            return G->FontPixels[y * G->FontW + x] / 255.0f;
        const uint8_t* p = G->FontPixels + (y * G->FontW + x) * 4;
        return p[3] / 255.0f;
    }
    for (const auto& e : G->UserTextures)
    {
        if (e.id == tex_id && e.pixels)
        {
            int x = (int)std::floor(u * e.w);
            int y = (int)std::floor(v * e.h);
            x = std::clamp(x, 0, e.w - 1);
            y = std::clamp(y, 0, e.h - 1);
            if (e.bpp == 1) return e.pixels[y * e.w + x] / 255.0f;
            const uint8_t* p = e.pixels + (y * e.w + x) * 4;
            return p[3] / 255.0f;
        }
    }
    return 1.0f;
}

static void BlendPixel(uint8_t* dst, PixelFormat fmt, int sr, int sg, int sb, int sa)
{
    int dr, dg, db, da;
    ReadDestPixel(dst, fmt, dr, dg, db, da);
    const float fa = sa / 255.0f, inv = 1.0f - fa;
    int or_ = (int)(sr * fa + dr * inv + 0.5f);
    int og_ = (int)(sg * fa + dg * inv + 0.5f);
    int ob_ = (int)(sb * fa + db * inv + 0.5f);
    int oa_ = (fmt == RGB565) ? 255 : (int)(sa + da * inv + 0.5f);
    WriteDestPixel(dst, fmt, or_, og_, ob_, oa_);
}

struct EdgeEq { float A, B, C; };
static inline EdgeEq MakeEdge(const ImVec2& a, const ImVec2& b)
{ EdgeEq e{a.y - b.y, b.x - a.x, a.x * b.y - a.y * b.x}; return e; }
static inline float EvalEdge(const EdgeEq& e, float x, float y) { return e.A * x + e.B * y + e.C; }

static void RasterizeTriangle(const ImDrawVert& v0, const ImDrawVert& v1, const ImDrawVert& v2, ImTextureID tex_id, const ImVec4& clip, float ox, float oy)
{
    ImVec2 p0(v0.pos.x - ox, v0.pos.y - oy);
    ImVec2 p1(v1.pos.x - ox, v1.pos.y - oy);
    ImVec2 p2(v2.pos.x - ox, v2.pos.y - oy);

    float minx = std::floor(std::min({p0.x, p1.x, p2.x}));
    float maxx = std::ceil (std::max({p0.x, p1.x, p2.x}));
    float miny = std::floor(std::min({p0.y, p1.y, p2.y}));
    float maxy = std::ceil (std::max({p0.y, p1.y, p2.y}));

    minx = std::max(minx, clip.x); miny = std::max(miny, clip.y);
    maxx = std::min(maxx, clip.z); maxy = std::min(maxy, clip.w);
    if (maxx <= minx || maxy <= miny) return;

    EdgeEq e0 = MakeEdge(p1, p2), e1 = MakeEdge(p2, p0), e2 = MakeEdge(p0, p1);
    const float area = e2.A * p2.x + e2.B * p2.y + e2.C; if (area == 0.0f) return; const float inv_area = 1.0f / area;

    int r0,g0,b0,a0, r1,g1,b1,a1, r2,g2,b2,a2;
    UnpackColor(v0.col, r0,g0,b0,a0);
    UnpackColor(v1.col, r1,g1,b1,a1);
    UnpackColor(v2.col, r2,g2,b2,a2);

    const int xmin = (int)minx, xmax = (int)maxx, ymin = (int)miny, ymax = (int)maxy;
    const int bpp = (G->Format == RGB565) ? 2 : 4;
    for (int y = ymin; y < ymax; ++y)
    {
        uint8_t* row = G->Pixels + y * G->Pitch;
        for (int x = xmin; x < xmax; ++x)
        {
            const float px = x + 0.5f, py = y + 0.5f;
            float w0 = EvalEdge(e0, px, py), w1 = EvalEdge(e1, px, py), w2 = EvalEdge(e2, px, py);
            if ((w0 < 0) || (w1 < 0) || (w2 < 0)) continue;
            w0 *= inv_area; w1 *= inv_area; w2 *= inv_area;

            const float u = v0.uv.x * w0 + v1.uv.x * w1 + v2.uv.x * w2;
            const float v = v0.uv.y * w0 + v1.uv.y * w1 + v2.uv.y * w2;
            const float rf = (r0*w0 + r1*w1 + r2*w2);
            const float gf = (g0*w0 + g1*w1 + g2*w2);
            const float bf = (b0*w0 + b1*w1 + b2*w2);
            const float af = (a0*w0 + a1*w1 + a2*w2);
            const float ta = SampleTextureAlpha(tex_id, u, v);
            int sr = (int)(rf + 0.5f), sg = (int)(gf + 0.5f), sb = (int)(bf + 0.5f), sa = (int)(af * ta + 0.5f);
            BlendPixel(row + x * bpp, G->Format, sr, sg, sb, sa);
        }
    }
}

bool Init(int width, int height, void* pixels, int pitch_bytes, PixelFormat fmt)
{
    IM_ASSERT(G == nullptr);
    G = new Context();
    G->Width = width; G->Height = height; G->Pixels = (uint8_t*)pixels; G->Pitch = pitch_bytes; G->Format = fmt;

    ImGuiIO& io = ImGui::GetIO();
    unsigned char* font_pixels = nullptr; int fw=0, fh=0;
    io.Fonts->GetTexDataAsAlpha8(&font_pixels, &fw, &fh);
    G->FontPixels = font_pixels; G->FontW = fw; G->FontH = fh; G->FontBpp = 1;
    io.Fonts->SetTexID(G->FontTextureID);

    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    return true;
}

void Shutdown()
{
    if (!G) return; G->UserTextures.clear(); delete G; G = nullptr;
}

void NewFrame(int width, int height, void* pixels, int pitch_bytes)
{
    if (!G) return; G->Width = width; G->Height = height; G->Pixels = (uint8_t*)pixels; G->Pitch = pitch_bytes;
}

void RenderDrawData(ImDrawData* draw_data)
{
    if (!G || !G->Pixels || !draw_data) return;

    ImVec2 clip_off = draw_data->DisplayPos;
    ImVec2 clip_scale = draw_data->FramebufferScale;
    const int fb_width = (int)(draw_data->DisplaySize.x * clip_scale.x);
    const int fb_height = (int)(draw_data->DisplaySize.y * clip_scale.y);
    if (fb_width <= 0 || fb_height <= 0) return;

    // Optional: collect dirty rects for this frame
#if IMGUISW_ENABLE_DIRTY_RECTS
    G->DirtyRects.clear();
#endif

    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        const ImDrawVert* vtx_buf = cmd_list->VtxBuffer.Data;
        const ImDrawIdx*  idx_buf = cmd_list->IdxBuffer.Data;
        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback) { pcmd->UserCallback(cmd_list, pcmd); continue; }

            ImVec4 cr;
            cr.x = (pcmd->ClipRect.x - clip_off.x) * clip_scale.x;
            cr.y = (pcmd->ClipRect.y - clip_off.y) * clip_scale.y;
            cr.z = (pcmd->ClipRect.z - clip_off.x) * clip_scale.x;
            cr.w = (pcmd->ClipRect.w - clip_off.y) * clip_scale.y;
            if (cr.x < 0) cr.x = 0; if (cr.y < 0) cr.y = 0;
            if (cr.z > fb_width)  cr.z = (float)fb_width;
            if (cr.w > fb_height) cr.w = (float)fb_height;
            if (cr.x >= cr.z || cr.y >= cr.w) continue;

#if IMGUISW_ENABLE_DIRTY_RECTS
            // Collect rect (convert to integer half-open coordinates)
            ImGuiSW::Rect r;
            r.x1 = (int)std::floor(cr.x);
            r.y1 = (int)std::floor(cr.y);
            r.x2 = (int)std::ceil (cr.z);
            r.y2 = (int)std::ceil (cr.w);
            G->DirtyRects.push_back(r);
#endif

            const ImTextureID tex_id = pcmd->GetTexID();

            const ImDrawIdx* idx_end = idx_buf + pcmd->ElemCount;
            for (; idx_buf < idx_end; idx_buf += 3)
            {
                const ImDrawVert& v0 = vtx_buf[idx_buf[0]];
                const ImDrawVert& v1 = vtx_buf[idx_buf[1]];
                const ImDrawVert& v2 = vtx_buf[idx_buf[2]];
                RasterizeTriangle(v0, v1, v2, tex_id, cr, clip_off.x, clip_off.y);
            }
        }
    }

#if IMGUISW_ENABLE_DIRTY_RECTS
    // Merge/align and optionally flush
    if (G->DirtyRects.Size > 0)
    {
        // Inflate
        const int W = fb_width, H = fb_height;
        const int inflate_px = G->DirtyCfg.inflate_px;
        for (int i = 0; i < G->DirtyRects.Size; ++i)
        {
            auto& r = G->DirtyRects[i];
            r.x1 = std::max(0, r.x1 - inflate_px);
            r.y1 = std::max(0, r.y1 - inflate_px);
            r.x2 = std::min(W, r.x2 + inflate_px);
            r.y2 = std::min(H, r.y2 + inflate_px);
        }

        // Greedy merge
        auto& v = G->DirtyRects;
        // sort by area desc
        std::sort(v.begin(), v.end(), [](const ImGuiSW::Rect& a, const ImGuiSW::Rect& b){
            const int aa = (a.x2 - a.x1) * (a.y2 - a.y1);
            const int bb = (b.x2 - b.x1) * (b.y2 - b.y1);
            return aa > bb;
        });
        ImVector<ImGuiSW::Rect> merged;
        merged.reserve(v.Size);
        auto overlap_or_close = [&](const ImGuiSW::Rect& a, const ImGuiSW::Rect& b, int dist){
            return !(a.x2 + dist <= b.x1 || b.x2 + dist <= a.x1 || a.y2 + dist <= b.y1 || b.y2 + dist <= a.y1);
        };
        for (int i = 0; i < v.Size; ++i)
        {
            const auto& r = v[i];
            bool m = false;
            for (int j = 0; j < merged.Size; ++j)
            {
                auto& mr = merged[j];
                if (overlap_or_close(mr, r, G->DirtyCfg.merge_dist))
                {
                    mr.x1 = std::min(mr.x1, r.x1);
                    mr.y1 = std::min(mr.y1, r.y1);
                    mr.x2 = std::max(mr.x2, r.x2);
                    mr.y2 = std::max(mr.y2, r.y2);
                    m = true; break;
                }
            }
            if (!m)
            {
                merged.push_back(r);
                if (merged.Size > G->DirtyCfg.max_rects)
                {
                    merged.clear();
                    merged.push_back({0,0,W,H});
                    break;
                }
            }
        }

        // Align
        const int ap = std::max(1, G->DirtyCfg.align_px);
        for (int i = 0; i < merged.Size; ++i)
        {
            auto& r = merged[i];
            r.x1 = (r.x1 / ap) * ap;
            r.y1 = (r.y1 / ap) * ap;
            r.x2 = std::min(W, ((r.x2 + ap - 1) / ap) * ap);
            r.y2 = std::min(H, ((r.y2 + ap - 1) / ap) * ap);
        }

        G->DirtyRects.swap(merged);

        if (G->FlushCb && G->AutoFlush)
            G->FlushCb(G->Pixels, G->Pitch, G->Format, G->DirtyRects.begin(), G->DirtyRects.Size, G->FlushUser);
    }
#endif
}

ImTextureID CreateTexture(const void* pixels, int w, int h, int bpp)
{
    if (!G) return ImTextureID_Invalid;
    G->UserTextures.push_back({}); auto& e = G->UserTextures.back();
    e.pixels = (const uint8_t*)pixels; e.w = w; e.h = h; e.bpp = bpp; e.id = (ImTextureID)(uintptr_t)&e;
    return e.id;
}

void DestroyAllUserTextures()
{
    if (!G) return; G->UserTextures.clear();
}

#if IMGUISW_ENABLE_DIRTY_RECTS
void SetFlushCallback(FlushRectCallback cb, void* user_data, const DirtyRectsConfig& cfg)
{
    if (!G) return;
    G->FlushCb = cb;
    G->FlushUser = user_data;
    G->DirtyCfg = cfg;
}

void SetAutoFlush(bool enabled)
{
    if (!G) return; G->AutoFlush = enabled;
}

void Present()
{
    if (!G || !G->FlushCb || G->DirtyRects.Size == 0) return;
    G->FlushCb(G->Pixels, G->Pitch, G->Format, G->DirtyRects.begin(), G->DirtyRects.Size, G->FlushUser);
}

void ComputeDirtyRects(ImDrawData* dd, ImVector<Rect>& out,
                       int screen_w, int screen_h,
                       int inflate_px, int merge_dist, int max_rects, int align_px)
{
    out.clear(); if (!dd) return;
    ImVec2 clip_off = dd->DisplayPos; ImVec2 clip_scale = dd->FramebufferScale;
    for (int n = 0; n < dd->CmdListsCount; n++)
    {
        const ImDrawList* dl = dd->CmdLists[n];
        for (int i = 0; i < dl->CmdBuffer.Size; ++i)
        {
            const ImDrawCmd& pcmd = dl->CmdBuffer[i];
            if (pcmd.ElemCount == 0) continue;
            Rect r;
            r.x1 = (int)std::floor((pcmd.ClipRect.x - clip_off.x) * clip_scale.x);
            r.y1 = (int)std::floor((pcmd.ClipRect.y - clip_off.y) * clip_scale.y);
            r.x2 = (int)std::ceil ((pcmd.ClipRect.z - clip_off.x) * clip_scale.x);
            r.y2 = (int)std::ceil ((pcmd.ClipRect.w - clip_off.y) * clip_scale.y);
            r.x1 = std::max(0, std::min(r.x1, screen_w));
            r.y1 = std::max(0, std::min(r.y1, screen_h));
            r.x2 = std::max(0, std::min(r.x2, screen_w));
            r.y2 = std::max(0, std::min(r.y2, screen_h));
            if (r.x2 <= r.x1 || r.y2 <= r.y1) continue; out.push_back(r);
        }
    }
    if (out.Size == 0) return;
    for (int i = 0; i < out.Size; ++i)
    {
        auto& r = out[i];
        r.x1 = std::max(0, r.x1 - inflate_px);
        r.y1 = std::max(0, r.y1 - inflate_px);
        r.x2 = std::min(screen_w, r.x2 + inflate_px);
        r.y2 = std::min(screen_h, r.y2 + inflate_px);
    }
    // merge
    std::sort(out.begin(), out.end(), [](const Rect& a, const Rect& b){
        const int aa = (a.x2-a.x1)*(a.y2-a.y1); const int bb = (b.x2-b.x1)*(b.y2-b.y1); return aa > bb; });
    ImVector<Rect> merged; merged.reserve(out.Size);
    auto overlap_or_close2 = [&](const Rect& a, const Rect& b){
        return !(a.x2 + merge_dist <= b.x1 || b.x2 + merge_dist <= a.x1 || a.y2 + merge_dist <= b.y1 || b.y2 + merge_dist <= a.y1);
    };
    for (int i = 0; i < out.Size; ++i)
    {
        const Rect& r = out[i]; bool m = false;
        for (int j = 0; j < merged.Size; ++j)
        {
            Rect& mr = merged[j];
            if (overlap_or_close2(mr, r)) { mr.x1 = std::min(mr.x1, r.x1); mr.y1 = std::min(mr.y1, r.y1); mr.x2 = std::max(mr.x2, r.x2); mr.y2 = std::max(mr.y2, r.y2); m = true; break; }
        }
        if (!m) { merged.push_back(r); if (merged.Size > max_rects) { merged.clear(); merged.push_back({0,0,screen_w,screen_h}); break; } }
    }
    // align
    const int ap = std::max(1, align_px);
    for (int i = 0; i < merged.Size; ++i)
    {
        Rect& r = merged[i];
        r.x1 = (r.x1 / ap) * ap; r.y1 = (r.y1 / ap) * ap;
        r.x2 = std::min(screen_w, ((r.x2 + ap - 1) / ap) * ap);
        r.y2 = std::min(screen_h, ((r.y2 + ap - 1) / ap) * ap);
    }
    out.swap(merged);
}
#endif
} // namespace ImGuiSW
