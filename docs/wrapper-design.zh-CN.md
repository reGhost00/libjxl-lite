# Wrap 库设计建议（JXL + JPEG 统一解码）

> 目标：在不改 `lib/jxl/` 核心的前提下，在仓库外层新建一个 wrap 库，
> 默认构建这个 wrap，对外提供"自动识别 JXL / JPEG 并解码"的统一 API。

---

## 1. 设计前提与边界

### 1.1 要解决什么问题

应用层现在面临尴尬的二选一：

```c
// 现在 libjxl 用户必须这样：
if (JxlSignatureCheck(buf, size) == JXL_SIG_JXL) {
  JxlDecoder* dec = JxlDecoderCreate(...);
  // ... 一长串 JXL 事件循环
} else if (buf[0] == 0xFF && buf[1] == 0xD8) {
  jpegli_decompress_struct cinfo;
  jpegli_create_decompress(&cinfo);
  // ... 一长串 libjpeg 风格 API
}
```

wrap 的目标就是让应用层只写一次：

```c
WrapImage img;
if (WrapDecode(buf, size, NULL, &img) == 0) {
  // 拿到 img.pixels + img.width / img.height / img.channels
  // ...
  WrapFreeImage(&img);
}
```

### 1.2 范围

- **包含**：JPEG / JXL 解码（基础 8-bit RGB / RGBA 输出）
- **可选**：灰度输出、16-bit 输出、缩略图请求
- **不包含**：
  - 编码（v1 暂时只做解码）
  - 动画帧（JPEG 没有，JXL 也不是 wrap 的首要目标）
  - 渐进 / progressive（JPEG 端的 progressive jpegli 支持，但 wrap v1 不暴露）
  - 元数据（EXIF / ICC 由调用者按需从 `JxlDecoder` 事件里取）

### 1.3 与现有 fork 设计的关系

- `lib/jxl/`：**不动**
- `lib/jpegli/`：**不动源码**，但会被 wrap 第一次正式编译
- `lib/CMakeLists.txt` 等 5 个 cmake：**不动**
- `wrap/`：**新增**，默认开启

---

## 2. 推荐架构

### 2.1 三层结构

```
┌─────────────────────────────────────────┐
│ Public API: wrap_decode.h               │  <- 应用层唯一接触面
├─────────────────────────────────────────┤
│ Dispatcher: wrap_decode.cc              │  <- 格式探测 + 路由
├────────────────────┬────────────────────┤
│ JXL backend:       │ JPEG backend:      │
│ jxl::Decode...     │ jpegli_* API      │
└────────────────────┴────────────────────┘
```

- **Public API**：最小、稳定、C 风格 ABI
- **Dispatcher**：纯函数，根据 `WrapDetectFormat` 选 backend
- **Backends**：内部实现细节，wrap 类不暴露

### 2.2 目录布局

```
<repo-root>/
├── wrap/                         <- 新增
│   ├── CMakeLists.txt
│   ├── wrap_decode.h             <- 公开 API
│   ├── wrap_decode.cc            <- dispatcher + JXL backend
│   ├── wrap_jpeg.cc              <- jpegli backend
│   ├── wrap_image.cc             <- WrapImage 生命周期管理
│   └── wrap_test.cc              <- 基本测试
├── lib/                          <- 不动
│   ├── jxl/
│   ├── jpegli/                   <- 不动源码，但被 wrap 编译
│   └── ...
├── CMakeLists.txt                <- 增加 add_subdirectory(wrap)
└── ...
```

---

## 3. 推荐 API

### 3.1 头文件 `wrap_decode.h`

```c
#ifndef WRAP_DECODE_H_
#define WRAP_DECODE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  WRAP_FORMAT_UNKNOWN = 0,
  WRAP_FORMAT_JXL,           // 裸 codestream (FF 0A) 或 container (JXL )
  WRAP_FORMAT_JPEG,          // FF D8 FF
} WrapFormat;

typedef enum {
  WRAP_PIXEL_RGB888  = 0,    // 3 channels, 8-bit
  WRAP_PIXEL_RGBA8888 = 1,    // 4 channels, 8-bit
  WRAP_PIXEL_GRAY8   = 2,    // 1 channel,  8-bit
  // 未来: WRAP_PIXEL_RGB16, WRAP_PIXEL_RGBA16, WRAP_PIXEL_GRAY16
} WrapPixelFormat;

typedef enum {
  WRAP_OK = 0,
  WRAP_ERR_UNSUPPORTED,       // 不是 JXL 也不是 JPEG
  WRAP_ERR_INVALID_ARG,
  WRAP_ERR_DECODE_FAILED,     // 底层解码失败
  WRAP_ERR_OUT_OF_MEMORY,
  WRAP_ERR_INTERNAL,
} WrapStatus;

typedef struct {
  WrapPixelFormat format;     // 默认 RGB888
  // 未来字段: scale_num / scale_den, crop, icc_policy...
} WrapDecodeOptions;

typedef struct {
  uint8_t* pixels;            // 由 WrapDecode 分配，WrapFreeImage 释放
  size_t   width;
  size_t   height;
  uint8_t  num_channels;      // 1 / 3 / 4
  uint8_t  bits_per_sample;   // v1 固定 8
  size_t   row_stride;        // >= width * num_channels
  WrapPixelFormat pixel_format;
} WrapImage;

const char* WrapStatusString(WrapStatus s);

// 格式探测（不分配内存）
WrapFormat WrapDetectFormat(const uint8_t* data, size_t size);

// 主入口：自动识别 + 解码
WrapStatus WrapDecode(const uint8_t* data, size_t size,
                      const WrapDecodeOptions* opts,  // 可为 NULL
                      WrapImage* out);

// 释放 WrapDecode 分配的内存
void WrapFreeImage(WrapImage* img);

#ifdef __cplusplus
}
#endif

#endif  // WRAP_DECODE_H_
```

### 3.2 典型调用

```c
#include "wrap_decode.h"

uint8_t* buf;
size_t buf_size;
// ... 读取到 buf ...

WrapImage img = {0};
WrapStatus s = WrapDecode(buf, buf_size, NULL, &img);
if (s != WRAP_OK) {
  fprintf(stderr, "decode failed: %s\n", WrapStatusString(s));
  return 1;
}

// 使用 img.pixels, img.width, img.height, img.num_channels
process_image(img.pixels, img.row_stride, img.width, img.height, img.num_channels);

WrapFreeImage(&img);
```

### 3.3 为什么不沿用 JXL 的事件模型

JXL 是事件驱动的（`JxlDecoderProcessInput` → 状态机），适合复杂格式（box、progressive、preview、animation）。

JPEG 没有这些概念。强行套用事件模型会出现：

- 大量"假装是 JXL 状态机"的状态，但 JPEG 不会触发
- API 表面上对调用者更"统一"，但实际上调用者还是要写两套思维

`WrapDecode` 一次性把图解出来，更简单，性能也更好（避免多次状态切换）。

---

## 4. JPEG 解码器选择

### 4.1 推荐：fork 自带的 `lib/jpegli/`

**理由：**

1. **零依赖**：fork 已经有源码，不需要引入第三方 libjpeg
2. **质量更好**：jpegli 是 libjpeg 的现代化重写，浮点 IDCT、低位深量化噪点抑制
3. **fork 的设计意图一致**：详见 `docs/technical-overview.zh-CN.md` 第 12 节，已经把 jpegli 当成"未来保留的备选"
4. **API 兼容**：jpegli 提供 `jpegli_*` API 和 `jpeg_*` 兼容 API 两种入口。wrap 走 `jpegli_*`（更干净）

**权衡：**

- jpegli 的 in-tree 版本是 0.12.0 同期的快照，比 google/jpegli 主线旧
- 如果未来需要 jpegli 新功能，需要先把 fork 里的 jpegli 子树升级（这点和 `lib/` 同步逻辑一致，分开做）

### 4.2 备选：系统 libjpeg-turbo

**何时选**：如果不想构建 jpegli（节省 5-10s 编译时间）。

**做法**：在 `wrap/CMakeLists.txt` 里 `find_package(JPEG REQUIRED)`，引入 `<jpeglib.h>`，用 `jpeg_*` API。

```cmake
find_package(JPEG REQUIRED)
target_link_libraries(wrap_jpeg PRIVATE JPEG::JPEG)
```

**权衡：**

- 失去 jpegli 的解码质量优势
- 多一个系统依赖（影响 "lite" 定位）
- 行为差异（libjpeg-turbo 整数 IDCT vs jpegli 浮点 IDCT）

**建议**：默认走 jpegli，把系统 libjpeg 当成 fallback（找不到 jpegli 编译进 wrap 时启用）。

### 4.3 决策

> **默认走 jpegli。需要 `-DWRAP_USE_SYSTEM_JPEG=ON` 才走系统 libjpeg。**

---

## 5. 构建集成

### 5.1 顶层 CMake 改动

```cmake
# 在 root CMakeLists.txt 中：
option(JPEGXL_LITE_BUILD_WRAP "Build the JXL+JPEG unified wrap library" ON)

if(JPEGXL_LITE_BUILD_WRAP)
  add_subdirectory(wrap)
endif()
```

**默认 ON**，满足"默认构建变成 wrap"的需求。

### 5.2 `wrap/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)
project(jxl_wrap C CXX)

# ---- 1. 引入 jpegli 编译 ----
# jpegli.cmake 在 fork 的 lib/CMakeLists.txt 没被 include，
# 但是 wrap 需要它。我们的方式是直接 include 这个文件。
include(${CMAKE_CURRENT_SOURCE_DIR}/../lib/jpegli.cmake)

# ---- 2. 定义 wrap 库 ----
add_library(jxl_wrap STATIC
  wrap_decode.cc
  wrap_jpeg.cc
  wrap_image.cc
)
target_include_directories(jxl_wrap PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(jxl_wrap PUBLIC jxl jpegli-static Threads::Threads)

# OPTIONS
option(WRAP_USE_SYSTEM_JPEG "Use system libjpeg instead of in-tree jpegli" OFF)
if(WRAP_USE_SYSTEM_JPEG)
  find_package(JPEG REQUIRED)
  target_compile_definitions(jxl_wrap PRIVATE WRAP_USE_SYSTEM_JPEG=1)
  target_link_libraries(jxl_wrap PRIVATE JPEG::JPEG)
  # 不再链接 jpegli-static
endif()

# ---- 3. 测试 ----
if(JPEGXL_LITE_BUILD_TESTS)
  add_executable(wrap_test wrap_test.cc)
  target_link_libraries(wrap_test PRIVATE jxl_wrap)
  add_test(NAME wrap_test COMMAND wrap_test)
endif()
```

### 5.3 构建 jpegli 需要解决的依赖

`lib/jpegli.cmake` 默认会找 `third_party/libjpeg-turbo` 里的 `jpeglib.h` 头。fork 没有这个第三方，需要绕过：

**两种方案：**

**A. 直接用 jpegli 自己的 `common.h`**（修一行 cmake）：
```cmake
# 在 wrap/CMakeLists.txt 中先定义掩码
set(JPEGXL_ENABLE_BROTLI OFF CACHE BOOL "" FORCE)
# 然后 include jpegli.cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/../lib/jpegli.cmake)
```
多数情况下 `lib/jpegli/common.h` 已经把必要的类型定义好了，不依赖 libjpeg-turbo 的 `jconfig.h.in`。

**B. 提供 stub 头文件**：
```cmake
# wrap/third_party/jpeglib.h
#ifndef WRAP_JPEGLIB_H_
#define WRAP_JPEGLIB_H_
#include "lib/jpegli/common.h"
#endif
```
然后在 `wrap/CMakeLists.txt` 里 `-I${CMAKE_CURRENT_SOURCE_DIR}/third_party`。

**建议先 A，A 不行再 B。**

### 5.4 把 wrap 集成进 `tools/cli.cc`

```cmake
# tools/CMakeLists.txt 中
target_link_libraries(cli PRIVATE jxl_wrap)
```

CLI 调用：

```cpp
#include "wrap_decode.h"

if (WrapDetectFormat(buf, size) != WRAP_FORMAT_UNKNOWN) {
  WrapImage img;
  if (WrapDecode(buf, size, nullptr, &img) == WRAP_OK) {
    // 直接拿到 RGB 像素，省掉对 jpegli / JXL 各自的事件循环
    save_ppm(out, img);
    WrapFreeImage(&img);
    return 0;
  }
}
```

---

## 6. 各 backend 实现要点

### 6.1 JXL backend（`wrap_decode.cc`）

```cpp
WrapStatus WrapDecodeJxl(const uint8_t* data, size_t size,
                          const WrapDecodeOptions* opts,
                          WrapImage* out) {
  auto* dec = JxlDecoderCreate(nullptr);
  if (!dec) return WRAP_ERR_OUT_OF_MEMORY;

  JxlDecoderSetInput(dec, data, size);
  JxlDecoderCloseInput(dec);
  JxlDecoderSubscribeEvents(dec,
      JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);

  // 内部循环直到 FULL_IMAGE
  // 拿到 JxlBasicInfo，得到 width / height / num_channels
  // 分配 WrapImage::pixels
  // JxlDecoderSetImageOutBuffer
  // 继续 ProcessInput 直到 SUCCESS
  // 填 WrapImage 元数据

  JxlDecoderDestroy(dec);
  return WRAP_OK;
}
```

**注意**：

- JXL 8-bit 输出 → 直接 `JXL_PIXEL_FORMAT_RGBxxx`
- 灰度 / 16-bit → 后续版本支持
- 错误路径要记得 `JxlDecoderDestroy` 和释放 `out->pixels`

### 6.2 JPEG backend（`wrap_jpeg.cc`）

```cpp
#include "lib/jpegli/decode.h"

WrapStatus WrapDecodeJpeg(const uint8_t* data, size_t size,
                          const WrapDecodeOptions* opts,
                          WrapImage* out) {
  jpegli_decompress_struct cinfo;
  jpegli_error_mgr jerr;
  cinfo.err = jpegli_std_error(&jerr);

  jpegli_create_decompress(&cinfo);
  jpegli_mem_src(&cinfo, data, size);

  if (jpegli_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpegli_destroy_decompress(&cinfo);
    return WRAP_ERR_DECODE_FAILED;
  }

  cinfo.out_color_space = (opts && opts->format == WRAP_PIXEL_GRAY8)
                          ? JCS_GRAYSCALE : JCS_RGB;
  // 不支持 RGBA（JPEG 没有 alpha），RGBA8888 选项下报错：
  if (opts && opts->format == WRAP_PIXEL_RGBA8888) {
    jpegli_destroy_decompress(&cinfo);
    return WRAP_ERR_INVALID_ARG;
  }

  jpegli_start_decompress(&cinfo);

  out->width = cinfo.output_width;
  out->height = cinfo.output_height;
  out->num_channels = (cinfo.out_color_space == JCS_GRAYSCALE) ? 1 : 3;
  out->bits_per_sample = 8;
  out->row_stride = out->width * out->num_channels;
  out->pixel_format = (out->num_channels == 1) ? WRAP_PIXEL_GRAY8
                                                 : WRAP_PIXEL_RGB888;
  out->pixels = (uint8_t*)malloc(out->row_stride * out->height);
  if (!out->pixels) {
    jpegli_destroy_decompress(&cinfo);
    return WRAP_ERR_OUT_OF_MEMORY;
  }

  while (cinfo.output_scanline < cinfo.output_height) {
    uint8_t* row = out->pixels + cinfo.output_scanline * out->row_stride;
    jpegli_read_scanlines(&cinfo, &row, 1);
  }

  jpegli_finish_decompress(&cinfo);
  jpegli_destroy_decompress(&cinfo);
  return WRAP_OK;
}
```

### 6.3 Dispatcher（`wrap_decode.cc`）

```cpp
WrapFormat WrapDetectFormat(const uint8_t* data, size_t size) {
  if (!data || size < 4) return WRAP_FORMAT_UNKNOWN;
  // JXL container: 00 00 00 0C 4A 58 4C 20 0D 0A 87 0A
  if (size >= 12 && memcmp(data, "\x00\x00\x00\x0CJXL \r\n\x87\n", 12) == 0) {
    return WRAP_FORMAT_JXL;
  }
  // JXL 裸 codestream: FF 0A
  if (data[0] == 0xFF && data[1] == 0x0A) {
    return WRAP_FORMAT_JXL;
  }
  // JPEG: FF D8 FF
  if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
    return WRAP_FORMAT_JPEG;
  }
  return WRAP_FORMAT_UNKNOWN;
}

WrapStatus WrapDecode(const uint8_t* data, size_t size,
                      const WrapDecodeOptions* opts, WrapImage* out) {
  if (!data || !out) return WRAP_ERR_INVALID_ARG;
  *out = {0};

  switch (WrapDetectFormat(data, size)) {
    case WRAP_FORMAT_JXL:  return WrapDecodeJxl(data, size, opts, out);
    case WRAP_FORMAT_JPEG: return WrapDecodeJpeg(data, size, opts, out);
    default:               return WRAP_ERR_UNSUPPORTED;
  }
}

void WrapFreeImage(WrapImage* img) {
  if (!img) return;
  free(img->pixels);
  img->pixels = nullptr;
  img->width = img->height = 0;
  img->num_channels = 0;
  img->row_stride = 0;
}
```

---

## 7. 实施步骤

按这个顺序，每一步都能独立验证：

### Step 1 — 骨架与 API 头（半天）

- 创建 `wrap/` 目录
- 创建 `wrap/wrap_decode.h`（§3.1）
- 创建 `wrap/CMakeLists.txt` 占位（先不链接 jpegli，只编 wrap_decode.cc）
- 创建 `wrap/wrap_test.cc` 编译通过 + 跑通空 API

### Step 2 — JXL backend（半天）

- 实现 `WrapDecodeJxl`
- 加几张 .jxl 测试图
- 测试 RGB 输出

### Step 3 — 接入 jpegli（半天）

- 实现 `wrap_jpeg.cc`
- 解决 `third_party/libjpeg-turbo` 缺失问题（§5.3）
- 编译并链接 jpegli-static
- 验证 JPEG 测试图

### Step 4 — Dispatcher + 整合（半天）

- 完成 `WrapDetectFormat` 和 `WrapDecode`
- 跑混合测试（JXL + JPEG 混合输入）
- 默认开启（`JPEGXL_LITE_BUILD_WRAP=ON`）

### Step 5 — 集成进 `tools/cli.cc`（半天）

- 改 `tools/cli.cc` 优先走 wrap
- 验证 GUI 工程不受影响

### Step 6 — 文档（半天）

- 更新 `README.md`：增加 wrap 的使用说明
- 在 `docs/technical-overview.zh-CN.md` 加链接

---

## 8. 风险与权衡

| 风险 | 严重度 | 应对 |
|------|-------|------|
| jpegli 编译失败 / 缺 libjpeg-turbo | 中 | §5.3 提供替代方案；提供 `WRAP_USE_SYSTEM_JPEG` 兜底 |
| Wrap 默认 ON 但用户的 game engine 不想用 | 低 | 提供 `=OFF` 关闭 |
| Wrap 隐藏了 JXL 的 ICC / EXIF 能力 | 中 | 文档说明：需要这些的话直接用 `JxlDecoder` |
| 升级 libjxl 后 wrap 需要同步更新 | 中 | 用 `docs/upstream-sync.zh-CN.md` 的流程同步；wrap 部分正常 follow |
| JPEG 端不支持 RGBA（透明 PNG 输入 JPEG 不可能） | 低 | v1 文档明确：WRAP_PIXEL_RGBA8888 配 JPEG 输入返回 `WRAP_ERR_INVALID_ARG` |
| 内存模型（wrap 总是 malloc） | 低 | v2 可以加 `WrapDecodeInto` 让 caller 提供 buffer |

---

## 9. API v2 路线图（参考）

短期不要做，但记一下，未来要扩展时按这个顺序加：

1. **16-bit 输出**（`WRAP_PIXEL_RGB16`）
2. **缩略图 / 缩放**（`WrapDecodeOptions::scale_num / scale_den`）
3. **caller-provided buffer**（`WrapDecodeInto`）
4. **元数据 access**（`WrapImageExtra` 暴露 ICC / EXIF）
5. **编码**（`WrapEncode` → 内部 dispatch 到 JxlEncoder / jpegli encoder）
6. **动画**（v1 只能输出 frame 0）

---

## 10. 总结

| 维度 | 决策 |
|------|------|
| 入口位置 | `wrap_decode.h`（C ABI） |
| 入口函数 | `WrapDecode(buf, size, opts, out)` |
| JPEG decoder | jpegli（in-tree），可切 system libjpeg |
| 输出内存 | wrap 分配，调用者用 `WrapFreeImage` 释放 |
| 默认构建 | 开启（`JPEGXL_LITE_BUILD_WRAP=ON`） |
| 编码 | v1 不做 |
| 16-bit / 灰度 / 透明 | 后续版本 |

整体上，这是一个**结构清晰、依赖最小、风险可控**的方案，新增约 500-800 行代码（含测试），不影响 fork 的核心 codec。
