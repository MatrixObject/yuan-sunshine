# YuanSunshine Windows 编译指南

> 适用于 `YuanSunshine-2026.906.222525`，在 Windows 10/11 上用 MSYS2 UCRT64 从源码编译。

---

## 目录

- [1. 环境准备](#1-环境准备)
- [2. 获取源码](#2-获取源码)
- [3. 获取子模块](#3-获取子模块)
- [4. 已知问题与补丁](#4-已知问题与补丁)
- [5. 配置 CMake](#5-配置-cmake)
- [6. 构建](#6-构建)
- [7. 运行测试](#7-运行测试)
- [8. 构建产物](#8-构建产物)
- [附录 A：完整命令速查](#附录-a完整命令速查)
- [附录 B：故障排查](#附录-b故障排查)

---

## 1. 环境准备

### 1.1 MSYS2

安装 MSYS2，默认路径 `D:\msys64`（如果不是默认路径，下文替换为实际路径）。

更新并安装 UCRT64 工具链：

```bash
D:\msys64\usr\bin\pacman.exe -Syu
D:\msys64\usr\bin\pacman.exe -S --needed base-devel mingw-w64-ucrt-x86_64-Toolchain
```

> 本项目 **仅** 使用 UCRT64 环境，不要使用 MINGW64/CLANG64 等。

### 1.2 必需工具包

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-boost \
  mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-opus \
  mingw-w64-ucrt-x86_64-curl \
  mingw-w64-ucrt-x86_64-miniupnpc \
  mingw-w64-ucrt-x86_64-nlohmann-json \
  mingw-w64-ucrt-x86_64-llvm \
  mingw-w64-ucrt-x86_64-doxygen \
  mingw-w64-ucrt-x86_64-graphviz \
  mingw-w64-ucrt-x86_64-python \
  mingw-w64-ucrt-x86_64-nodejs \
  git \
  dos2unix
```

> `mingw-w64-ucrt-x86_64-graphviz` 包含 doxygen 所需的 `dot` 工具（约 536 MiB，首次安装约 47 个包）。若构建时报 `dot: not found`，说明未安装。

### 1.3 Node.js（原生 Windows 版）

**不要** 使用 MSYS2 内的 Node.js（CMake 的 `find_program(NPM)` 在 MSYS2 bash 中找不到）。

从 https://nodejs.org 下载并安装 Windows 原生版（默认路径 `C:\Program Files\nodejs`），确保目录下有 `npm.cmd` 和 `node.exe`。

> 原生 npm 通过 CMake 参数 `-DNPM` 显式传入，不需要加入 PATH。

### 1.4 Git（Windows 版）

安装 Git for Windows（路径默认 `C:\Program Files\Git\bin\git.exe`）。

---

## 2. 获取源码

将源码放到一个**不含中文和空格**的路径（例如 `I:\NBOX2024-9-6\YuanSunshine\Sunshine-2026.906.222525`）。

> 路径深度不宜过深，Windows 有 260 字符 MAX_PATH 限制，CMake 构建路径（`cmake-build-release/`）会在源码路径下，两者相加不要超过约 180 字符。

---

## 3. 获取子模块

本项目的源码目录**不是 git 仓库**，无法使用 `git submodule update`。所有第三方依赖通过 codeload tarball 手动拉取。

### 3.1 已知需要的子模块清单

根据 CMake 构建分析，以下子模块必须在构建前就绪：

| 子模块路径 | 来源仓库 | 说明 |
|---|---|---|
| `third-party/moonlight-common-c/enet` | `cgutman/enet` `v6_multihome` 分支 | **必须使用 v6_multihome**，含 `ENetPeer.localAddress` |
| `third-party/moonlight-common-c/nanors` | `sleepybishop/nanors` `master` | Reed-Solomon FEC |
| `third-party/lizardbyte-common` | `LizardByte/lizardbyte-common` `master` | 公共工具库（含 env.cpp） |
| `third-party/libvirtualhid/third-party/lizardbyte-common` | 同上 | libvirtualhid 依赖 |
| `third-party/doxyconfig/doxygen-awesome-css` | `jothepro/doxygen-awesome-css` `main` | doxygen 文档样式 |
| `third-party/tray/third-party/lizardbyte-common` | 同上 | tray 依赖 |
| `third-party/tray/third-party/doxyconfig` | 同上 | tray 依赖 |

> `third-party/build-deps`、`third-party/wayland-protocols`、`third-party/wlr-protocols`、`third-party/plasma-wayland-protocols`、`third-party/TPCircularBuffer` 均为 Linux/macOS 专用，Windows 构建**不需要**。

### 3.2 拉取方法

GitHub 不稳定时使用镜像前缀：
```
https://gh-proxy.com/https://codeload.github.com/<owner>/<repo>/tar.gz/refs/heads/<branch>
```

#### 示例：拉取 lizardbyte-common

```bash
# 在 MSYS2 UCRT64 shell 中
SRCROOT="I:/NBOX2024-9-6/YuanSunshine/Sunshine-2026.906.222525"
MIRROR="https://gh-proxy.com"

# 拉取 lizardbyte-common
curl -L -o /tmp/lb-common.tar.gz \
  "$MIRROR/https://codeload.github.com/LizardByte/lizardbyte-common/tar.gz/refs/heads/master"
tar -xzf /tmp/lb-common.tar.gz -C "$SRCROOT/third-party/"
mv "$SRCROOT/third-party/lizardbyte-common-master" "$SRCROOT/third-party/lizardbyte-common"
```

#### 示例：拉取 doxygen-awesome-css

```bash
curl -L -o /tmp/da-css.tar.gz \
  "$MIRROR/https://codeload.github.com/jothepro/doxygen-awesome-css/tar.gz/refs/heads/main"
tar -xzf /tmp/da-css.tar.gz -C "$SRCROOT/third-party/doxyconfig/"
mv "$SRCROOT/third-party/doxyconfig/doxygen-awesome-css-main" \
   "$SRCROOT/third-party/doxyconfig/doxygen-awesome-css"
```

#### 示例：拉取 enet（关键步骤）

```bash
# 必须使用 v6_multihome 分支！
curl -L -o /tmp/enet.tar.gz \
  "$MIRROR/https://codeload.github.com/cgutman/enet/tar.gz/refs/heads/v6_multihome"
# 如果 moonlight-common-c/enet 目录已存在旧版，先备份
mv "$SRCROOT/third-party/moonlight-common-c/enet" "$SRCROOT/third-party/moonlight-common-c/enet.old"
tar -xzf /tmp/enet.tar.gz -C "$SRCROOT/third-party/moonlight-common-c/"
mv "$SRCROOT/third-party/moonlight-common-c/enet-6_multihome" \
   "$SRCROOT/third-party/moonlight-common-c/enet"
rm -rf "$SRCROOT/third-party/moonlight-common-c/enet.old"
```

> **为什么必须 v6_multihome？** cgutman/enet 的 master 分支和 v1.3.x 系列都没有 `ENetPeer.localAddress` 字段。Sunshine 的 RTSP 控制协议（ENet-based）需要从 `peer->localAddress` 获取本机路由地址。`v6_multihome` 分支是上游 moonlight-common-c 实际使用的 enet 版本。

### 3.3 辅助脚本

可在 PowerShell 中编写循环脚本批量拉取，核心模式：

```powershell
function Fetch-Submodule {
    param([string]$Repo, [string]$Branch, [string]$DestDir)
    $url = "https://gh-proxy.com/https://codeload.github.com/$Repo/tar.gz/refs/heads/$Branch"
    $tmp = "$env:TEMP\$($Repo -replace '[/]','_').tar.gz"
    Invoke-WebRequest -Uri $url -OutFile $tmp -TimeoutSec 300
    $extractDir = "$env:TEMP\$($Repo.Split('/')[-1])-$Branch"
    if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force }
    tar -xzf $tmp -C (Split-Path $DestDir)
    if (Test-Path $DestDir) { Remove-Item $DestDir -Recurse -Force }
    Rename-Item "$extractDir" $DestDir
}
```

---

## 4. 已知问题与补丁

本 fork 在编译时存在多处子模块版本不同步的问题，以下为必须修补的项。

### 4.1 Boost 合并 include 树

**问题**：CMake 从 boost-1.89.0-cmake.tar.xz 构建，Boost 使用模块化结构。每个目标的编译器只得到单一 include 路径 `libs/headers/include`（合并后的 boost 根）。源码 tarball 中，`libs/headers/include/boost/` 下的符号链接被 Windows tar 解压为**空目录**。此外 `libs/numeric/` 是元目录（真实库在 `libs/numeric/conversion/` 等深度2子目录），简单的深度1遍历无法发现它们。

**后果**：`#include <boost/log/common.hpp>` 等找不到头文件。

**修复方法**：编写脚本，遍历所有 `libs/*/include/boost`（深度2），robocopy 到 `libs/headers/include/boost` 做全量 union 合并：

```powershell
$BoostSrc = "cmake-build-release/_deps/boost-src"
$libs = Get-ChildItem (Join-Path $BoostSrc "libs") -Directory -Recurse -Depth 1 |
    Where-Object { Test-Path (Join-Path $_.FullName "include\boost") }
# 156 个库

$workRoot = Join-Path $BoostSrc "libs\headers\include\.__boost_merge"
# 清理后创建空目录
foreach ($lib in $libs) {
    robocopy (Join-Path $lib.FullName "include\boost") $workRoot /E /IS /NFL /NDL /NJH /NJS /NP /MT:8 | Out-Null
}
# 用 .__boost_merge 替换 libs/headers/include/boost
```

> 该脚本约需 15-30 分钟。合并后有 307 个顶层条目。

### 4.2 enet v6_multihome QOS 冲突

**问题**：enet `win32.c` 自定义 `QOS_FLOWID`/`PQOS_FLOWID` 的 typedef 与 MSYS2 UCRT64 的 `qos2.h` 冲突（后者已通过 `ULONG` 定义了这些类型）。

**修复**：删除 `win32.c` 中的 typedef 块，改为直接包含 `qos2.h`：

```c
// 修改前
#include "enet/enet.h"
#include <windows.h>
#include <Mswsock.h>
#ifndef HAS_QOS_FLOWID
typedef UINT32 QOS_FLOWID;
#endif
#ifndef HAS_PQOS_FLOWID
typedef UINT32 *PQOS_FLOWID;
#endif
#include <mmsystem.h>
#include <qos2.h>

// 修改后
#include "enet/enet.h"
#include <windows.h>
#include <Mswsock.h>
#include <mmsystem.h>
#include <qos2.h>
```

> 这与上游 cgutman/enet master 的做法一致。

### 4.3 `DATA_SHARDS_MAX` 未定义

**问题**：`src/stream.cpp` 使用 `DATA_SHARDS_MAX`（255），但当前 nanors/rs.h 已重命名为 `RS8_DATA_SHARDS_MAX`。

**修复**：在 `stream.cpp` 的 `#include <rs.h>` 后添加兼容宏：

```cpp
#include <rs.h>

#ifndef DATA_SHARDS_MAX
  /**
   * @brief Maximum number of data shards per FEC block (RS8).
   */
  #define DATA_SHARDS_MAX RS8_DATA_SHARDS_MAX
#endif
```

> 注意：LizardByte 项目要求所有代码必须有 doxygen 文档，宏也不例外。

### 4.4 `sunshinesvc` 目标缺少 Boost 链接

**问题**：`tools/sunshinesvc.cpp` 包含 `src/logging.h`（依赖 Boost.Log），但 `tools/CMakeLists.txt` 中 `sunshinesvc` 目标未链接 `${Boost_LIBRARIES}`（fork 新增目标遗漏）。

**修复**：在 `tools/CMakeLists.txt` 中给 `sunshinesvc` 添加 Boost 链接：

```cmake
add_executable(sunshinesvc sunshinesvc.cpp)
target_link_libraries(sunshinesvc
        ${Boost_LIBRARIES}       # 添加这一行
        ${CMAKE_THREAD_LIBS_INIT}
        wtsapi32
        ${PLATFORM_LIBRARIES})
```

---

## 5. 配置 CMake

在 MSYS2 UCRT64 shell 中执行。

### 5.1 设置环境变量

```bash
# Git sslbackend 兼容性（msys git 只支持 openssl，用户全局 git 可能使用 schannel）
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0="http.sslbackend"
export GIT_CONFIG_VALUE_0="openssl"
```

> 每次打开新的 MSYS2 shell 都需要重新设置。

### 5.2 执行 CMake 配置

```bash
cd /path/to/Sunshine-2026.906.222525

cmake -B cmake-build-release \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=install \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DNPM='C:/Program Files/nodejs/npm.cmd' \
  -DNPM_NODE_EXECUTABLE='C:/Program Files/nodejs/node.exe' \
  -DDOTNET_EXECUTABLE=''
```

**参数说明**：

| 参数 | 说明 |
|---|---|
| `-G Ninja` | 使用 Ninja 生成器（比 MSVC Makefiles 快） |
| `-DCMAKE_BUILD_TYPE=Release` | Release 优化构建 |
| `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` | 降低策略版本避免 deprecation 告警 |
| `-DNPM=...` | 显式指定 Windows 原生 npm 路径（**必须用单引号**，含空格） |
| `-DNPM_NODE_EXECUTABLE=...` | 显式指定 node.exe 路径 |
| `-DDOTNET_EXECUTABLE=''` | 跳过 WiX 打包（dotnet tool install wix 4.0.4 在受限网络下失败） |

> **参数空格处理**：`-DNPM` 和 `-DNPM_NODE_EXECUTABLE` 的值必须用**单引号**包裹（在 MSYS2 shell 内），否则空格会拆分参数导致 CMake 解析错误。

### 5.3 配置输出关键信息

正常配置应看到：
- WiX 打包 warning（`Dotnet executable not found, skipping WiX packaging.`）—— 可忽略
- mklink symlink warning（`当文件已存在时，无法创建该文件`）—— 可忽略
- `-- Configuring done`
- `-- Generating done`
- `Build files have been written to: ...`

---

## 6. 构建

```bash
# 确保 GIT_CONFIG 环境变量已设置
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0="http.sslbackend"
export GIT_CONFIG_VALUE_0="openssl"

# 并行构建（8 线程，一般 10-20 分钟）
cmake --build cmake-build-release --parallel 8
```

构建过程中可能看到：
- Boost 库首次编译较慢（log、serialization 等）
- doxygen 文档生成（`Patching output file 1100/1100` 为正常输出）
- Web UI 构建（`vite build`，每次构建都会重新运行）

**预期结果**：
- `sunshine.exe`（约 120 MB）
- `tests/test_sunshine.exe`（约 450 MB，含 GTest）
- `docs/build/html/index.html`（doxygen 文档）

---

## 7. 运行测试

```bash
./cmake-build-release/tests/test_sunshine
```

**预期结果**：
- 604 个测试从 85 个测试套件中运行
- 600 通过，4 跳过（环境相关：系统托盘、AMD/Intel 编码器、外部命令）
- 0 失败

---

## 8. 构建产物

| 文件 | 大小 | 说明 |
|---|---|---|
| `cmake-build-release/sunshine.exe` | ~120 MB | 主程序 |
| `cmake-build-release/tests/test_sunshine.exe` | ~450 MB | 测试套件（含 GTest） |
| `cmake-build-release/assets/web/` | — | Web UI 静态资源 |
| `cmake-build-release/docs/build/html/` | — | doxygen API 文档 |

---

## 附录 A：完整命令速查

以下为从零开始的完整命令序列（MSYS2 UCRT64 shell）：

```bash
# ====== 0. 环境 ======
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0="http.sslbackend"
export GIT_CONFIG_VALUE_0="openssl"

SRCROOT="I:/NBOX2024-9-6/YuanSunshine/Sunshine-2026.906.222525"
cd "$SRCROOT"

# ====== 1. 配置 ======
cmake -B cmake-build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=install \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DNPM='C:/Program Files/nodejs/npm.cmd' \
  -DNPM_NODE_EXECUTABLE='C:/Program Files/nodejs/node.exe' \
  -DDOTNET_EXECUTABLE=''

# ====== 2. 构建 ======
cmake --build cmake-build-release --parallel 8

# ====== 3. 测试 ======
./cmake-build-release/tests/test_sunshine
```

---

## 附录 B：故障排查

### B.1 `find_program(NPM)` 失败

**现象**：CMake 报 `Could not find NPM using the following names: npm`

**原因**：MSYS2 bash 中 `npm` 不在 PATH，或未传 `-DNPM` 参数。

**解决**：使用 Windows 原生 Node.js 路径，单引号包裹：
```
-DNPM='C:/Program Files/nodejs/npm.cmd'
```

### B.2 Boost `boost/log/common.hpp: No such file or directory`

**现象**：`nvenc_sdk_1100` 等目标编译报找不到 boost 头文件。

**原因**：Boost 合并 include 树未完成（符号链接变空目录 / numeric 深度2 库未收录）。

**解决**：执行 4.1 节的全量 union 合并脚本。

### B.3 `ENetPeer has no member named 'localAddress'`

**现象**：`stream.cpp:704: error: 'ENetPeer' has no member named 'localAddress'`

**原因**：vendored enet 版本过旧（master 或 v1.3.x），不含 `localAddress` 字段。

**解决**：替换为 `cgutman/enet` 的 `v6_multihome` 分支（3.2 节）。

### B.4 `QOS_FLOWID` 类型冲突

**现象**：`win32.c: conflicting types for 'QOS_FLOWID'`

**原因**：UCRT64 的 `qos2.h` 已定义 `QOS_FLOWID`。

**解决**：删除 enet `win32.c` 中的 typedef 块（4.2 节）。

### B.5 `DATA_SHARDS_MAX was not declared`

**现象**：`stream.cpp:1617: error: 'DATA_SHARDS_MAX' was not declared`

**原因**：nanors 已将常量重命名为 `RS8_DATA_SHARDS_MAX`。

**解决**：添加兼容宏（4.3 节）。

### B.6 doxygen 文档构建失败

**现象**：`Style sheet doxygen-awesome.css specified by HTML_EXTRA_STYLESHEET does not exist!`

**原因**：`third-party/doxyconfig/doxygen-awesome-css` 子模块为空。

**解决**：拉取 `jothepro/doxygen-awesome-css` 的 `main` 分支填充该目录（3.2 节）。

### B.7 GitHub 下载失败

**现象**：`curl: (28) Operation timed out`

**解决**：使用 gh-proxy 镜像：
```
https://gh-proxy.com/https://codeload.github.com/<owner>/<repo>/tar.gz/refs/heads/<branch>
```

或使用 gh.ddlc.top：
```
https://gh.ddlc.top/https://codeload.github.com/<owner>/<repo>/tar.gz/refs/heads/<branch>
```

### B.8 mklink symlink warning

**现象**：`当文件已存在时，无法创建该文件`

**原因**：CMake configure 阶段尝试创建 junction 失败（通常为 shader 目录已存在）。不影响构建，可忽略。
