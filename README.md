# imgui-sw (software renderer for Dear ImGui)

Independent CPU renderer for Dear ImGui. No GPU required.

- Targets: embedded devices without GPU, offscreen testing, CI snapshots.
- Dest formats: RGBA8888, BGRA8888, RGB565.
- Textures: font atlas supported by default. Simple user texture API provided.

Build options:
- CMake static library: `sw_imgui/CMakeLists.txt` (link with Dear ImGui core sources in your app)
- Or compile `sw_imgui.cpp` directly with your project.

Minimal API (C++):
- `ImGuiSW::Init(w, h, pixels, pitch, fmt)`
- `ImGuiSW::NewFrame(w, h, pixels, pitch)`
- `ImGuiSW::RenderDrawData(ImGui::GetDrawData())`
- `ImGuiSW::Shutdown()`

Pixel format enum:
- `ImGuiSW::RGBA32`, `BGRA32`, `RGB565`

Embedding steps:
1. Initialize Dear ImGui (create context), set `io.DisplaySize` and `io.DeltaTime`.
2. Provide a framebuffer pointer and call `ImGuiSW::Init()`.
3. Each frame: update inputs -> `ImGui::NewFrame()` -> build UI -> `ImGui::Render()` -> `ImGuiSW::NewFrame()` -> `ImGuiSW::RenderDrawData()` -> flush to LCD.
4. On shutdown: `ImGuiSW::Shutdown()`.

See `examples/example_null_software` for a minimal run that writes a PPM image.

## Dirty-rects and multi-backend flush

This library can compute per-frame dirty rectangles (areas touched by this frame's draw commands) and invoke a user-provided flush callback to update the display partially. This dramatically reduces SPI/parallel bandwidth on small LCDs.

Enable/disable via:
- Compile-time macro: `-DIMGUISW_ENABLE_DIRTY_RECTS=1` (default 0)
- ESP-IDF sdkconfig: `CONFIG_IMGUISW_ENABLE_DIRTY_RECTS=y` (auto-detected)
- Auto flush after RenderDrawData: `-DIMGUISW_AUTO_FLUSH=1` (default 1) or `SetAutoFlush(false)` to use manual `Present()`

API:
```
// Install a flush callback and config
ImGuiSW::DirtyRectsConfig cfg; // tune max_rects, inflate_px, merge_dist, align_px

## 在 ESP-IDF 中使用

本库已支持作为 ESP-IDF 组件使用（v5.x 及以上建议）。有两种方式：

1) 本地组件（推荐用于私有项目）
- 将整个 `sw_imgui` 文件夹放到你的 IDF 工程的 `components/` 目录下（例如 `your_project/components/sw_imgui`）。
- 在你的应用组件中正常 `#include "sw_imgui.h"` 并链接即可；Dear ImGui 的核心源码需要由你的工程以另一个组件的形式提供（或直接加入到应用组件）。
- 无需额外改动 CMake，本组件的 `CMakeLists.txt` 会被 IDF 自动识别。

2) 托管组件（Managed Component）依赖
- 本仓库包含 `idf_component.yml`，可发布到 Git 仓库后直接在你的工程里添加依赖：

```powershell
ImGuiSW::SetFlushCallback(my_flush, my_user, cfg);

// Optional control

- 或手动在工程的 `idf_component.yml` 中声明依赖：

```yaml
ImGuiSW::SetAutoFlush(true); // if true, RenderDrawData() calls flush automatically
// Or manual:
ImGuiSW::Present();
```


> 提示：本组件不内置 Dear ImGui，请把 Dear ImGui 作为另一个组件提供（例如把官方 `imgui/*.cpp` 加入 `components/imgui/` 并在其 CMake 里导出头文件路径）。

### 典型接入步骤（esp_lcd + RGB565）

1. 初始化面板并准备一块帧缓冲（与面板分辨率匹配），例如 `uint16_t* fb = ...;`，`pitch = width * 2`。
2. Dear ImGui 初始化（创建上下文，设置 `io.DisplaySize` 等）。
3. 调用 `ImGuiSW::Init(width, height, fb, pitch, ImGuiSW::RGB565);`。
4. 可选：启用“脏矩形”并设置 flush 回调（也可保持默认自动 flush）：

```cpp
Callback signature:
```
using ImGuiSW::Rect;

5. 每帧：
	- 输入更新 -> `ImGui::NewFrame()` -> 构建 UI -> `ImGui::Render()`。
	- `ImGuiSW::NewFrame(width, height, fb, pitch);`
	- `ImGuiSW::RenderDrawData(ImGui::GetDrawData());`
	- 若未启用自动 flush，可在此调用 `ImGuiSW::Present()`。

6. 退出时：`ImGuiSW::Shutdown();`

### Kconfig 可选项

当以 ESP-IDF 组件方式使用时，可在 `menuconfig` 中找到以下选项：
- `IMGUISW_ENABLE_DIRTY_RECTS`：启用脏矩形计算（默认开启）。
- `IMGUISW_AUTO_FLUSH`：在 `RenderDrawData()` 后自动调用 flush 回调（默认开启）。

它们会生成 `sdkconfig.h` 中的 `CONFIG_...` 宏，并在头文件中自动识别。
void my_flush(const uint8_t* framebuffer, int pitch, ImGuiSW::PixelFormat fmt,
			  const Rect* rects, int count, void* user);
```

Examples:
- ESP-IDF + esp_lcd (RGB565):
```
void my_flush(const uint8_t* fb, int pitch, ImGuiSW::PixelFormat fmt,
			  const ImGuiSW::Rect* rects, int count, void* user)
{
	auto panel = static_cast<esp_lcd_panel_handle_t>(user);
	const int bpp = (fmt == ImGuiSW::RGB565) ? 2 : 4;
	for (int i = 0; i < count; ++i) {
		const auto& r = rects[i];
		for (int y = r.y1; y < r.y2; ++y) {
			const uint8_t* line = fb + y * pitch + r.x1 * bpp;
			// If the driver requires contiguous block per rect, replace this per-line loop
			// by a temporary line buffer or a panel-specific partial-draw API.
			ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, r.x1, y, r.x2, y+1, line));
		}
	}
}
```

- Arduino + LovyanGFX:
```
void my_flush(const uint8_t* fb, int pitch, ImGuiSW::PixelFormat fmt,
			  const ImGuiSW::Rect* rects, int count, void* user)
{
	auto* lcd = static_cast<LGFX*>(user);
	const int bpp = (fmt == ImGuiSW::RGB565) ? 2 : 4;
	for (int i = 0; i < count; ++i) {
		const auto& r = rects[i];
		for (int y = r.y1; y < r.y2; ++y) {
			const void* line = fb + y * pitch + r.x1 * bpp;
			lcd->pushImage(r.x1, y, r.x2 - r.x1, 1, (const uint16_t*)line);
		}
	}
}
```

Notes:
- For maximum throughput, prefer panel APIs that can accept a contiguous sub-rectangle; otherwise push per-line.
- `align_px` in `DirtyRectsConfig` helps align to DMA-friendly boundaries (e.g., 8/16 px).
- When too many rectangles are produced, the algorithm falls back to full-screen update.
