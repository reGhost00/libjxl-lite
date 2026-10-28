# jxlimg：JPG / JXL 统一编解码库（stb_image 风格）

> 目标：在不改 `lib/jxl/` 核心的前提下，新增一个顶层 `wrap/` 库，
> 对外提供"自动识别 JPEG / JPEG XL 并编解码"的统一 API。
>
> 状态：**已实现**。代码在 `wrap/`，测试在 `wrap/tests/jxlimg_test.cc`。

---

## 1. 设计前提与边界

### 1.1 要解决什么问题

应用层现在必须二选一：

```c
// 以前：两套完全不同的调用方式
if (JxlSignatureCheck(buf, size) == JXL_SIG_JXL) {
  JxlDecoder* dec = JxlDecoderCreate(NULL);
  // ... 一长串 JxlDecoderProcessInput 事件循环
} else if (buf[0] == 0xFF && buf[1] == 0xD8) {
  jpegli_decompress_struct cinfo;
  jpegli_create_decompress(&cinfo);
  // ... 一长串 libjpeg 风格 API
}
```

`jxlimg` 的目标就是让应用层只写一次：

```c
unsigned char* pixels =
    jxlimg_load_from_memory(buf, size, &w, &h, &channels, 0);
if (pixels == NULL) {
  fprintf(stderr, "decode failed: %s\n", jxlimg_failure_reason());
}
// ... 用 pixels / w / h / channels
jxlimg_image_free(pixels);
```

### 1.2 范围

| 能力 | 解码 | 编码 |
|------|------|------|
| JPEG（基线 / 渐进） | ✅ | ✅（可选 progressive、4:4:4 / 4:2:2 / 4:2:0） |
| JPEG XL（容器 / 裸码流） | ✅ | ✅（可选容器 / 裸码流、lossless） |
| 8-bit 灰度 / 灰度+Alpha | ✅ | ✅ |
| 8-bit RGB | ✅ | ✅ |
| 8-bit RGBA | ✅（JPEG 的 alpha 填 0xFF） | ✅（JPEG 丢弃 alpha） |
| 16-bit | ❌（v1.1） | ❌（v1.1） |
| 动画 | 只取第 0 帧 | — |
| ICC / EXIF | ❌（v2） | ❌（v2） |

### 1.3 与 fork 其它部分的关系

| 路径 | 状态 | 说明 |
|------|------|------|
| `lib/jxl/` | **不动** | upstream 同步区 |
| `lib/jpegli/` | **不动** | upstream 同步区，但第一次被真正编译进构建 |
| `lib/CMakeLists.txt` | 改 14 行 | 只加 `include(cmake/jpegli.cmake)` |
| `lib/jxl.cmake` 等 4 个 cmake | **不动** | fork 裁剪层 |
| `cmake/jpegli.cmake` | **新增** | fork 自有，替代 upstream 的 `lib/jpegli.cmake` |
| `third_party/libjpeg-turbo/` | **新增** | vendor 3 个头文件 |
| `wrap/` | **新增** | 顶层目录，不在 upstream 同步区 |
| `CMakeLists.txt` | 改 12 行 | 加 `add_subdirectory(wrap)` |

> `wrap/` 之所以放顶层而不是 `lib/`：`docs/upstream-sync.zh-CN.md` §3.2 的同步流程
> 会用 upstream tarball 整体覆盖 `lib/*`，放在那里会被冲掉。

---

## 2. 目录结构

```
libjxl/
├── wrap/                          <- 新增
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── jxlimg.h               <- 公开 C API（唯一对外接口）
│   │   └── jxlimg.hpp             <- 可选 C++ RAII 头（header-only）
│   ├── jxlimg_internal.h          <- 内部接口
│   ├── jxlimg.cc                  <- dispatcher / 分配器 / 格式探测 / 文件 IO
│   ├── jxlimg_jpeg.cc             <- JPEG 后端（jpegli）
│   ├── jxlimg_jxl.cc              <- JPEG XL 后端（libjxl）
│   └── tests/jxlimg_test.cc       <- 自检程序（不需要 gtest）
├── cmake/jpegli.cmake             <- 新增，构建 jpegli-static
├── third_party/libjpeg-turbo/     <- 新增，vendor 的 3 个头文件
│   ├── jpeglib.h  jmorecfg.h  jconfig.h.in  README.md
├── lib/CMakeLists.txt             <- 加 include(cmake/jpegli.cmake)
└── CMakeLists.txt                 <- 加 add_subdirectory(wrap)
```

### 2.1 为什么需要 vendor libjpeg-turbo 的头文件

`lib/jpegli/` 是 libjpeg62 的独立实现，但它的源码直接使用了 libjpeg-turbo 的
**公开头文件**：

* `lib/jpegli/common.h` → `lib/jxl/base/include_jpeglib.h` → `#include <jpeglib.h>`
* `lib/jpegli/decode_internal.h` 使用 `struct jpeg_decompress_struct`、
  `jvirt_barray_ptr`、`JBLOCKARRAY`、`NUM_HUFF_TBLS` 等 libjpeg 内部类型。

本 fork 在 `a3ad2e0b`（"移除无用组件"）中删掉了 `third_party/libjpeg-turbo`
submodule，所以 `lib/jpegli/` 目前**根本编不过**。

处理方式：把 upstream libjxl 当年使用的那个 submodule commit
（libjpeg-turbo `8ecba3647`，版本 2.1.5.1）的 **3 个头文件** vendor 回来：

| 文件 | 用途 |
|------|------|
| `jpeglib.h` | libjpeg 公开 API 与结构体定义 |
| `jmorecfg.h` | 采样类型 / 色彩空间定义 |
| `jconfig.h.in` | CMake 模板，由 `cmake/jpegli.cmake` 生成 `jconfig.h` |

只搬头文件，不搬任何 `.c`，因此不引入额外的编译负担，也不存在符号冲突。
许可：IJG + BSD-3-Clause，与本仓库兼容（见 `third_party/libjpeg-turbo/README.md`）。

> 之所以选 2.1.5.1 而不是最新的 3.x：jpegli 的源码就是针对这个版本写的，
> `lib/jpegli.cmake` 里的 `JPEG_LIB_VERSION 62` 分支也是按它校验的。

---

## 3. 公开 API

完整定义见 [`wrap/include/jxlimg.h`](../wrap/include/jxlimg.h)。

### 3.1 风格约定（对齐 stb_image / stb_image_write）

* 一次性入口，没有需要管理的 decoder / encoder 对象。
* 返回 `NULL` 表示失败，原因用 `jxlimg_failure_reason()` 取（thread-local）。
* 返回的缓冲区用 `jxlimg_image_free()` 释放。
* 全部函数线程安全（每次调用创建独立实例）。

### 3.2 解码

```c
unsigned char* jxlimg_load_from_memory(const void* data, size_t size,
                                        int* x, int* y, int* channels_in_file,
                                        int desired_channels);
unsigned char* jxlimg_load(const char* filename, int* x, int* y,
                           int* channels_in_file, int desired_channels);
int jxlimg_info_from_memory(const void* data, size_t size, int* x, int* y,
                             int* channels_in_file);
int jxlimg_info(const char* filename, int* x, int* y, int* channels_in_file);
void jxlimg_image_free(void* ptr);
```

* `desired_channels`：`0` 保持源图通道数（1/2/3/4），`1` 灰度，`3` RGB，`4` RGBA。
* `jxlimg_info*` 只读文件头，**不解码像素**（JPEG 走 `jpegli_read_header`，
  JXL 只订阅 `JXL_DEC_BASIC_INFO`）。

### 3.3 编码

```c
unsigned char* jxlimg_write_to_mem(const void* pixels, int width, int height,
                                   int channels, jxlimg_format format,
                                   int quality, size_t* out_size);
int jxlimg_write(const char* filename, const void* pixels, int width, int height,
                 int channels, jxlimg_format format, int quality);
```

* `channels` ∈ {1, 3, 4}，`pixels` 为紧凑排列的 8-bit 交错数据。
* `format` 为 `JXLLIMG_FORMAT_JPEG` 或 `JXLLIMG_FORMAT_JXL`。
* `quality` 1..100，0 表示默认 90。JPEG 与 JXL 都按"距离"模型换算，
  同一个数字在两个后端的视觉质量大致可比。

### 3.4 高级选项

```c
typedef struct {
  int desired_channels;   /* 0=保持, 1=灰度, 3=RGB, 4=RGBA */
  int quality;            /* 1..100, 0=默认 90 */
  int jpeg_progressive;   /* 0/1 */
  int jpeg_subsampling;   /* 0=自动(4:2:0) 1=4:4:4 2=4:2:2 3=4:2:0 */
  int jxl_effort;         /* 1..9, 默认 7 */
  int jxl_lossless;       /* 0/1 */
  int jxl_container;      /* 0=裸码流 1=容器, 默认 1 */
} jxlimg_options;

void jxlimg_options_init(jxlimg_options* options);
unsigned char* jxlimg_load_from_memory_ex(..., const jxlimg_options*, ...);
unsigned char* jxlimg_write_to_mem_ex(..., const jxlimg_options*, ...);
```

简单 API 就是 `_ex` + 默认 options，参数语义完全不变。

### 3.5 自定义内存分配器

```c
typedef void* (*jxlimg_malloc_func)(void* user_data, size_t size);
typedef void* (*jxlimg_realloc_func)(void* user_data, void* ptr, size_t size);
typedef void  (*jxlimg_free_func)(void* user_data, void* ptr);

typedef struct {
  jxlimg_malloc_func  malloc_func;
  jxlimg_realloc_func realloc_func;   /* 可为 NULL */
  jxlimg_free_func    free_func;
  void* user_data;
} jxlimg_allocator;

int jxlimg_set_allocator(const jxlimg_allocator* allocator);
```

* 传 `NULL` 恢复默认（`malloc`/`realloc`/`free`）。
* `malloc_func` 与 `free_func` 必须同时提供或同时为空，否则返回 `-1`
  （否则会出现"用 A 分配、用 B 释放"的错配）。
* `realloc_func` 可以为空，此时内部用 `malloc + memcpy + free` 模拟。
* 分配器管到的东西：像素缓冲区、压缩缓冲区、以及 **libjxl 解码器/编码器内部的
  全部内存**（通过 `JxlMemoryManager` 转发）。jpegli 内部仍用 libc 的
  `malloc`（它有自己的内存管理器），这是实现细节。
* 必须在任何其它调用之前安装，且不能与其它调用并发。

### 3.6 C++ 头（可选）

```cpp
#include "jxlimg.hpp"

jxlimg::Image img;
if (jxlimg::LoadFile("a.jxl", &img)) { /* img.pixels, img.width, ... */ }

std::vector<uint8_t> out;
jxlimg::Encode(img, JXLLIMG_FORMAT_JPEG, &out, 85);
```

---

## 4. 架构

```
┌────────────────────────────────────────────────────────────┐
│ jxlimg.h / jxlimg.hpp                                        │
├────────────────────────────────────────────────────────────┤
│ jxlimg.cc   格式探测 · 调度 · 分配器 · 错误缓冲 · 文件 IO      │
├──────────────────────┬─────────────────────────────────────┤
│ jxlimg_jxl.cc        │ jxlimg_jpeg.cc                      │
│ JxlDecoder/Encoder   │ jpegli_* API                        │
└──────────────────────┴─────────────────────────────────────┘
```

**为什么不把 JXL 也做成事件模型**：JXL 是事件驱动的、JPEG 不是。强行统一只会
得到"两套思维、一套外壳"。统一入口走一次性全量解码，JXL 的事件循环被封装在
后端内部，对调用者不可见。

**通道数统一**：两个后端都输出"源图的自然通道数"（JPEG：1 或 3；JXL：1/2/3/4），
再由 wrapper 统一转换到 `desired_channels`：

| 源 → 目标 | 处理 |
|-----------|------|
| 3 → 1 | BT.601 luma |
| 1 → 3 | 灰度复制到 RGB |
| n → n+1（补 alpha） | alpha 填 0xFF |
| 4 → 3 | 丢弃 alpha |

---

## 5. 实现要点（踩过的坑）

### 5.1 jpegli 出错会 `exit()` 整个进程

`lib/jpegli/error.cc:30-34` 的默认 `error_exit` 直接 `exit(EXIT_FAILURE)`。
库绝不能这么做，所以 `wrap/jxlimg_jpeg.cc` 换掉了错误处理器：

```c
void JpegErrorExit(j_common_ptr cinfo) {
  auto* error = reinterpret_cast<JpegErrorManager*>(cinfo->err);
  // lib/jpegli/ 把错误文本放在 msg_parm.s
  snprintf(error->message, sizeof(error->message), "%s", cinfo->err->msg_parm.s);
  longjmp(error->jump_buffer, 1);
}
```

### 5.2 `longjmp` 会跳过 C++ 析构、并让被修改的局部变量变成不确定值

因此：

* 所有需要在错误后仍然有效的状态（`jpeg_decompress_struct`、目标管理器、行指针
  数组）都放在**堆上**，通过指针访问；
* 输出值通过指针参数回传；
* 含 `setjmp` 的函数里不放需要析构的局部对象。

### 5.3 `jpegli_CreateDecompress()` 不做整体 memset

它只初始化了一部分字段，所以调用前必须 `memset(cinfo, 0, sizeof(*cinfo))`，
否则 `out_color_space` / `scale_num` 等是脏值。

### 5.4 `jpegli_create_compress()` 会把 `cinfo->dest` 清空

目标管理器必须在 `jpegli_create_compress()` **之后**安装。

### 5.5 量化表依赖色度采样

`jpegli::SetQuantMatrices()` 会检查 `IsYUV420()`，4:2:0 用不同的表。
所以采样因子的修改必须在 `jpegli_set_defaults()` 之后、
`jpegli_set_distance()` 之前。

### 5.6 JXL 的 `quality == 100` 会隐式切到 lossless

`JxlEncoderDistanceFromQuality(100) == 0`，而
`JxlEncoderSetFrameDistance(fs, 0)` 内部会调用 `JxlEncoderSetFrameLossless()`，
后者要求 `uses_original_profile == true`（否则与 XYB 编码冲突）。
`wrap/jxlimg_jxl.cc` 因此在 distance 为 0 时自动设置
`uses_original_profile = JXL_TRUE`。

### 5.7 JXL 的 alpha 是 extra channel，但可以和颜色通道一起取

`JxlDecoderSetImageOutBuffer()` 的 `num_channels` 取 2（灰度+alpha）或
4（RGBA）时，libjxl 会把 alpha 一起交出来，不需要单独走 extra channel API。
编码端同理：`JxlEncoderAddImageFrame()` 直接接受交错的 RGBA。

### 5.8 色彩管理语义不一致（已知限制）

* **JXL 解码**：输出的是图片**原始色彩空间**下的像素（典型是 sRGB）。
  XYB 编码的图片会被解回原始 profile。
* **JPEG 解码**：只做 YCbCr→RGB 的矩阵变换，**不处理 ICC**。

也就是说同一次 `jxlimg_load`，对 JPEG 和 JXL 返回的 RGB 数值语义并不完全
一致。v1 不做统一（要给 JPEG 接 CMS 成本较高），文档中已明确写出。

---

## 6. 构建集成

### 6.1 根 `CMakeLists.txt`

```cmake
option(JXLL_BUILD_WRAP "build the unified JXL/JPEG image wrapper (wrap/)" ON)
...
if (JXLL_BUILD_WRAP)
  set(JPEGXL_ENABLE_JPEGLI ON CACHE BOOL "Build the in-tree jpegli JPEG codec")
  enable_testing()
endif()
add_subdirectory(lib)
if (JXLL_BUILD_WRAP)
  add_subdirectory(wrap)
endif()
```

`set(... CACHE ...)` 不带 `FORCE`，用户显式关掉 jpegli 时不会被覆盖；
此时 `wrap/CMakeLists.txt` 会给出明确的 `FATAL_ERROR` 提示。

### 6.2 `lib/CMakeLists.txt`

```cmake
option(JPEGXL_ENABLE_JPEGLI "Build the in-tree jpegli JPEG codec" OFF)
if (JPEGXL_ENABLE_JPEGLI)
  include(${PROJECT_SOURCE_DIR}/cmake/jpegli.cmake)
endif()
```

### 6.3 `cmake/jpegli.cmake`（替代 upstream 的 `lib/jpegli.cmake`）

upstream 的 `lib/jpegli.cmake` 有两个问题：它会连带构建 gtest 测试（本 fork
没有 googletest），而且按 `docs/upstream-sync.zh-CN.md` 的同步流程，那个文件
可能在同步时被删掉。fork 自有的版本：

* 只产出 `jpegli-static`，不建测试、不建 `libjpeg.so` 兼容库；
* 显式设置 `JPEG_LIB_VERSION 62`（否则 `jpeglib.h` 里的
  `#if JPEG_LIB_VERSION >= 70` 会走错分支）与 `jconfig.h.in` 需要的
  `VERSION` / `LIBJPEG_TURBO_VERSION_NUMBER`；
* 把 jpegli 单独钉在 C++17（`lib/jpegli/` 是 2023 年的代码，不受全局
  C++26 影响）。

### 6.4 `wrap/CMakeLists.txt`

```cmake
add_library(jxlimg STATIC jxlimg.cc jxlimg_jpeg.cc jxlimg_jxl.cc)
target_include_directories(jxlimg PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(jxlimg PUBLIC jxl PRIVATE jpegli-static)

add_executable(jxlimg_test tests/jxlimg_test.cc)
target_link_libraries(jxlimg_test PRIVATE jxlimg)
add_test(NAME jxlimg_test COMMAND jxlimg_test)
```

---

## 7. 测试

`wrap/tests/jxlimg_test.cc` 不依赖 gtest（fork 里没有），是一个带断言的
普通程序，`ctest` 直接跑它。当前 **160 项检查**：

| 用例 | 覆盖内容 |
|------|----------|
| `format` | JPEG / JXL 容器 / JXL 裸码流 / 垃圾数据的魔数识别 |
| `roundtrip-jpeg3` `roundtrip-jxl3` | RGB 编解码往返 + 像素误差上限 |
| `roundtrip-jpeg1` `roundtrip-jxl1` | 灰度往返 |
| `lossless` | JXL lossless 必须逐字节一致 |
| `channels` | `desired_channels` = 1/2/3/4 的转换 |
| `grayscale` | 灰度 JPEG 的通道数 |
| `alpha` | JXL 的 alpha 通道精确保留 |
| `broken` | 垃圾数据 / 截断 JPEG / 篡改后的 JPEG 都不崩溃 |
| `info` | 只读头信息（两种格式），且不依赖像素 |
| `file` | 写文件 → 探测格式 → 读回来 |
| `options` | progressive / 4 种色度采样 / JXL 裸码流 |
| `allocator` | 自定义分配器确实被使用；只给一半函数会被拒绝 |
| `status` | 状态码字符串 |
| `cpp` | `jxlimg.hpp` 的 C++ 接口 |

运行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jxlimg jxlimg_test
ctest --test-dir build --output-on-failure
```

单独跑某个用例：`./build/wrap/jxlimg_test.exe broken`。

---

## 8. v1.1 / v2 路线图

1. **16-bit**：解码走 `JXL_TYPE_UINT16` / `JPEGLI_TYPE_UINT16`，
   按 stb 的习惯另开 `_16` 系列函数。
2. **调用方提供缓冲区**：`jxlimg_load_into()` / `jxlimg_write_into()`，
   避免 malloc。
3. **多线程**：接 `JxlThreadParallelRunner`（`lib/threads/`）。
   v1 是单线程的——每次调用都新建线程池对小程序得不偿失。
4. **ICC / EXIF 元数据**：`JxlImageMetadata` 侧好做，JPEG 侧需要
   `jpegli_save_markers`。
5. **JPEG 的 CMS**：给 JPEG 也接色彩管理，让两种格式的输出语义一致（§5.8）。
6. **动画**：JXL 多帧遍历。

---

## 9. 相关文档

* `docs/technical-overview.zh-CN.md` —— 仓库整体结构、`lib/jpegli/` 的定位
* `docs/upstream-sync.zh-CN.md` —— 与 upstream 的差异和同步流程
* `third_party/libjpeg-turbo/README.md` —— vendor 头文件的来源与许可
