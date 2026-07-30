# 上游同步指南（与 upstream libjxl 的差异 + 同步流程）

> 面向本仓库 `libjxl-lite`（libjxl 的精简版 fork）。
> 配套阅读：`README.md`、`docs/technical-overview.zh-CN.md`。

---

## 1. 为什么不能直接 `git merge`

本 fork 在文件路径层面就与 upstream **不一致**：

- 删掉了 `lib/extras/` 整目录
- 删掉了 `lib/jpegli.cmake`、`lib/BUILD`、`lib/lib.gni`、`lib/gbench_main.cc`、`lib/jxl_benchmark.cmake`、`lib/jxl_extras.cmake`、`lib/jxl_lists.bzl`、`lib/jxl_tests.cmake`、`lib/jxl_vars.bzl`
- 5 个顶层 cmake 文件被 fork 改写（移除 install、TCMalloc、MSVC 特殊路径等）

单纯 `git merge upstream/main` 会立刻产生大量冲突（因为上游的 `lib/CMakeLists.txt` 是新文件，会冲突；`lib/jxl_lists.cmake` 上游删除了 jpegli 引用，fork 版保留），无法靠 3-way merge 解决。

正确策略是：**保留 fork 的裁剪层 + 应用 upstream 对核心 codec 的更新**。具体来说：

- 5 个顶部 cmake 文件 → 永远从 fork 自己的 HEAD 取
- 核心 codec 文件（`lib/jxl/`、`lib/include/jxl/`、`lib/threads/`、`lib/jpegli/`） → 接受 upstream 内容
- 裁剪掉的路径 → 直接删除

---

## 2. Fork 与 upstream 的差异清单

### 2.1 保留并与 upstream 保持一致（核心 codec）

| 路径 | 说明 |
|------|------|
| `lib/jxl/` | 核心 JXL 编解码器 |
| `lib/include/jxl/` | 公开 C/C++ 头 |
| `lib/threads/` | 线程运行器 |
| `lib/jpegli/` | 源码保留，**不参与构建** |
| `lib/jxl/jxl.syms` `lib/jxl/jxl.version` `lib/jxl/jxl_osx.syms` | 链接脚本 |
| `lib/jxl/libjxl.pc.in` `lib/jxl/libjxl_cms.pc.in` `lib/threads/libjxl_threads.pc.in` | pkg-config 模板 |
| `lib/nothing.cc` | OBJECT 编译占位 |

**这些文件应当接受 upstream 的更新。**

### 2.2 Fork 自身维护的 5 个 cmake 文件（**不要**被 upstream 覆盖）

| 文件 | 关键改动 |
|------|----------|
| `lib/CMakeLists.txt` | 不 include `jpegli.cmake`；CMS 强制 `JPEGXL_ENABLE_SKCMS=1` |
| `lib/jxl.cmake` | 无 install 规则、无 TCMalloc、无 MSVC 下的 `LINKER_EXCLUDE_LIBS` 特判 |
| `lib/jxl_cms.cmake` | 固定走 skcms（无 lcms2 分支） |
| `lib/jxl_threads.cmake` | 无 install 规则、pkg-config 注释、`WIN32` 下 link 标志为空 |
| `lib/jxl_lists.cmake` | **保留** `JPEGXL_INTERNAL_JPEGLI_*` / `JPEGXL_INTERNAL_EXTRAS_*` 等“死引用” |

### 2.3 Fork 移除的（upstream 存在）

- `lib/extras/`（整目录，约 76 个文件）
- `lib/jpegli.cmake`
- `lib/BUILD`
- `lib/lib.gni`
- `lib/gbench_main.cc`
- `lib/jxl_benchmark.cmake`
- `lib/jxl_extras.cmake`
- `lib/jxl_lists.bzl`
- `lib/jxl_tests.cmake`
- `lib/jxl_vars.bzl`
- `lib/libjxl libjxl main lib.zip`（upstream 自带的 zip 产物，非源码）

### 2.4 `lib/jxl_lists.cmake` 中为什么保留"死引用"

upstream 在最近版本里清理掉了所有 `JPEGXL_INTERNAL_JPEGLI_*` / `JPEGXL_INTERNAL_EXTRAS_*` source list，因为它们对应的源文件不存在了。

**fork 版有意保留这些 list**，原因：

1. `lib/jpegli/` 的源码还在仓库里（虽然不构建）
2. 万一未来想接回 jpegli，list 已经有了
3. 这是设计意图，不是 bug —— 见 `docs/technical-overview.zh-CN.md` 第 11 节

**同步时不要"清理"这些 list**。永远是 fork 自己的版本。

### 2.5 仓库其他目录的差异

- `CMakeLists.txt`、`tools/`、`cmake/`、`third_party/` —— fork 自行改写过，与 upstream 对应目录大幅不同
- `.github/`、CI 配置 —— fork 独有

这些不在 `lib/` 范围，同步 upstream 时**不要碰**。

---

## 3. 同步流程

### 3.1 准备工作

```bash
# 1) 确认工作区干净
git status

# 2) 备份当前的 lib/（强烈建议）
cp -r lib/ /tmp/lib_backup_$(date +%Y%m%d)
# 或者如果空间紧张：
# git stash -u
```

### 3.2 下载 upstream 并覆盖 `lib/`

到 https://github.com/libjxl/libjxl/releases 下载对应版本源码（注意要选 **Source code (tar.gz)**，不要 git clone）：

```bash
cd /tmp
tar -xzf libjxl-0.12.0.tar.gz
cp -r libjxl-0.12.0/lib/* <path-to-this-repo>/lib/
```

### 3.3 还原 fork 的裁剪层

```bash
cd <path-to-this-repo>
git checkout HEAD -- \
  lib/CMakeLists.txt \
  lib/jxl.cmake \
  lib/jxl_cms.cmake \
  lib/jxl_threads.cmake \
  lib/jxl_lists.cmake
```

### 3.4 删除 fork 不要的 upstream 文件

```bash
# 整目录
rm -rf lib/extras

# 单文件
rm -f lib/BUILD \
      lib/gbench_main.cc \
      lib/jpegli.cmake \
      lib/jxl_benchmark.cmake \
      lib/jxl_extras.cmake \
      lib/jxl_lists.bzl \
      lib/jxl_tests.cmake \
      lib/jxl_vars.bzl \
      lib/lib.gni \
      "lib/libjxl libjxl main lib.zip"
```

### 3.5 验证

```bash
# 1) 文件总数应与 fork 的 HEAD 一致（513）
git ls-files lib/ | wc -l

# 2) 5 个 cmake 顶层文件不应出现在 modified 列表
git status --short -- lib/ | grep -E '(jxl\.cmake|jxl_cms|jxl_threads|jxl_lists|lib/CMakeLists\.txt)$' || echo "cmake OK"

# 3) 核心 codec 应有上游改进的 diff
git diff --stat HEAD -- lib/

# 4) 不应有任何 untracked
git ls-files --others --exclude-standard lib/
```

### 3.6 提交

```bash
git add lib/
git commit -m "bump upstream jxl to <version>"
```

---

## 4. 常见坑

### 4.1 不要 `git merge`

```bash
# 错误 ❌
git merge upstream/v0.13.0
```

这条命令会让冲突淹没你。**永远不要**对 `lib/` 走 merge 路径。

### 4.2 不要用 upstream 的 `lib/jxl_lists.cmake`

upstream 清理 jpegli 引用 = 好事，但对 fork 来说意味着"丢失了未来想接回 jpegli 的选项"。**永远用 fork 自己的 `jxl_lists.cmake`**。

### 4.3 行尾符号带来的噪音

在 Windows + `core.autocrlf=true` 下，`git status` 会显示几百个文件"modified"，但其中只有大约 100 个有真正的内容差异（其余只是 `LF` vs `CRLF`）。判断实际改动要看：

```bash
git diff --stat HEAD -- lib/   # 列内容 diff
git diff -w HEAD -- lib/       # 忽略空白
```

### 4.4 上游新增的 core 文件

如果 upstream 新增了 `lib/jxl/foo.cc` 或 `lib/jpegli/bar.cc` 这种"路径上属于 core 范围"的文件，它会作为 untracked 出现在 `git status`。

**应该 add 它**。这是 fork 想要的核心改进。

### 4.5 上游移动/重命名 core 文件

如果 upstream 把 `lib/jxl/old.cc` 改成 `lib/jxl/new.cc`：

```bash
git ls-files --deleted lib/jxl/old.cc   # 列出删除
git status --short -- lib/              # 看到 deleted + untracked
git add -A lib/                         # 一次性 stage 删除 + 添加
```

### 4.6 不要去碰 `tools/`、`cmake/`、`third_party/`

这些 fork 改写过多，与 upstream 完全脱钩。同步时**只动 `lib/`**。

### 4.7 `lib/jpegli.cmake` 删不删都行

fork 的 HEAD 里有 `lib/jpegli.cmake`（虽然是"未被引用的"文件）。两种选择都合理：

- **保留**：与 fork 历史一致
- **删除**：更干净

建议保留以避免 git 出现"delete"记录。

---

## 5. 版本对应表

记录每次同步的大版本，便于回溯：

| Upstream 版本 | 同步日期       | 同步 commit | 备注 |
|---------------|----------------|-------------|------|
| 0.12.0        | 2026-07-30     | 见 git log  | 首次按本流程同步 |

---

## 6. 快速脚本

把整个流程包成一个脚本（保存为 `scripts/sync_upstream.sh`）：

```bash
#!/bin/bash
set -euo pipefail

UPSTREAM_TARBALL="$1"   # e.g. /tmp/libjxl-0.13.0.tar.gz
NEW_VERSION="$2"        # e.g. 0.13.0

REPO_ROOT="$(git rev-parse --show-toplevel)"
WORK=$(mktemp -d)

echo "==> Extracting upstream"
tar -xzf "$UPSTREAM_TARBALL" -C "$WORK"

echo "==> Backing up current lib/"
cp -r "$REPO_ROOT/lib" "$WORK/lib.backup"

echo "==> Overwriting lib/"
cp -r "$WORK"/libjxl-*/lib/* "$REPO_ROOT/lib/"

echo "==> Restoring fork cmake files"
cd "$REPO_ROOT"
git checkout HEAD -- \
  lib/CMakeLists.txt \
  lib/jxl.cmake \
  lib/jxl_cms.cmake \
  lib/jxl_threads.cmake \
  lib/jxl_lists.cmake

echo "==> Removing upstream-only files"
rm -rf lib/extras
rm -f lib/BUILD lib/gbench_main.cc \
  lib/jpegli.cmake \
  lib/jxl_benchmark.cmake lib/jxl_extras.cmake \
  lib/jxl_lists.bzl lib/jxl_tests.cmake lib/jxl_vars.bzl \
  lib/lib.gni \
  "lib/libjxl libjxl main lib.zip"

echo "==> Verifying"
EXPECTED=513
ACTUAL=$(git ls-files lib/ | wc -l)
if [ "$ACTUAL" -ne "$EXPECTED" ]; then
  echo "WARNING: file count $ACTUAL != $EXPECTED, please check"
fi

echo "==> Done. Review with: git diff --stat HEAD -- lib/"
```

用法：

```bash
./scripts/sync_upstream.sh /tmp/libjxl-0.13.0.tar.gz 0.13.0
```

> 注意：期望的文件数 513 是基于 0.12.0 同步后的结果。新版本如果 upstream 加了新核心文件，记得同步更新这个数字。
