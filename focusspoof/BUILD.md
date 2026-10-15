# focusspoof 编译说明

`focusspoof.dll` / `focusspoof32.dll` 通过 MinGW-w64 编译器直接从 `focusspoof.cpp` 编译，单文件生成，不依赖 CMake。

## 1. 工具链

| 目标 | 工具链路径 | 输出 |
|------|-----------|------|
| 64-bit (x86_64-w64) | `C:\ProgramData\mingw64\mingw64\bin\g++.exe` | `focusspoof.dll` |
| 32-bit (i686-w64)   | `D:\msys64\mingw32\bin\g++.exe`   | `focusspoof32.dll` |

- 编译器版本：GCC 13（mingw-w64 UCRT64 工具链）
- 两个 DLL 都链接 `-static`，**不会** 依赖系统上的 `libwinpthread-1.dll` / `libstdc++-6.dll` 等 MinGW runtime，可直接投放到任意 Windows 进程里
- 额外链接的 Win32 库：`ws2_32`（UDP 假手柄桥）、`ole32` / `oleaut32`（Windows.Gaming.Input COM 假手柄 / WMI / HID 层）

## 2. 编译命令

PowerShell 下执行：

```powershell
# 64-bit
& "C:\ProgramData\mingw64\mingw64\bin\g++.exe" -shared -O2 -s focusspoof.cpp `
  -o focusspoof.dll -static -lws2_32 -lole32 -loleaut32

# 32-bit（先把 mingw32\bin 加入 PATH，g++ 是 i686 版）
$env:PATH = "D:\msys64\mingw32\bin;$env:PATH"
g++ -shared -O2 -s focusspoof.cpp -o focusspoof32.dll -static -lws2_32 -lole32 -loleaut32
```

关键参数：

- `-shared`：生成 DLL
- `-O2 -s`：体积优化 + strip 符号
- `-static`：静态链接 MinGW C/C++ runtime，不产生额外的 dll 依赖
- `-lws2_32 -lole32 -loleaut32`：显式链接 Win32 / WinRT / WMI 用到的库

期望退出码 `0`。如果出现 `wingspoof.cpp` 相关的编译报错（例如新加的 `WgiRealStatics2Acquire` 未定义），先用 PowerShell 读 `err64.txt` 看具体行号，回到 `focusspoof.cpp` 修复。

## 3. 产物部署

| 位置 | 说明 |
|------|------|
| `focusspoof\` 目录 | 原始编译产物（`focusspoof.dll` / `focusspoof32.dll`） |
| `wpf-window-controller\bin\Release\net472\` | WPF 控制端的输出目录，把两份 DLL **复制**过去，控制端按目标游戏位数自动选择 |

每次改完 `focusspoof.cpp` 后，重新编译并把两个 DLL **复制**到 `wpf-window-controller\bin\Release\net472\`：

```powershell
$rel = "I:\NBOX2024-9-6\YuanSunshine\Sunshine-2026.906.222525\wpf-window-controller\bin\Release\net472"
Copy-Item "I:\NBOX2024-9-6\YuanSunshine\Sunshine-2026.906.222525\focusspoof\focusspoof.dll"   $rel -Force
Copy-Item "I:\NBOX2024-9-6\YuanSunshine\Sunshine-2026.906.222525\focusspoof\focusspoof32.dll" $rel -Force
```

注入器（`injectedll.exe` / `injectedll32.exe`）由 `Services\FocusSpoof.cs` 启动；它把目标 PID 对应的 DLL 通过 `CreateRemoteThread` + `LoadLibraryW` 注入，再让被注入端读取同目录下的 `focus_options.txt`（开关）并安装对应 hook。

## 4. 注入器 / 测试宿主

同目录下的 C 源码（用同一套 MinGW 工具链）：

```powershell
# 注入器（64-bit，注入 64-bit DLL 到 64-bit 进程）
& "C:\ProgramData\mingw64\mingw64\bin\gcc.exe" -O2 -o injectedll.exe    injector.c -static -lshell32 -static

# 注入器（32-bit，注入 32-bit DLL 到 32-bit 进程）
$env:PATH = "D:\msys64\mingw32\bin;$env:PATH"
gcc -O2 -o injectedll32.exe injector.c -static -lshell32 -static

# Win32 测试宿主（验证 GetForegroundWindow 等 API 在注入后是否仍指向原窗口）
& "C:\ProgramData\mingw64\mingw64\bin\gcc.exe" -O2 -o testtarget.exe    testtarget.c -static
$env:PATH = "D:\msys64\mingw32\bin;$env:PATH"
gcc -O2 -o testtarget32.exe testtarget.c -static
```

## 5. 烟雾测试

| 脚本 | 用途 |
|------|------|
| `smoke_test.ps1` / `smoke_test32.ps1` | 注入器 + testtarget 基础链路验证 |
| `smoke_fakepad.ps1` | 启动 64-bit 宿主并注入 64-bit DLL，校验 XInput UDP 假手柄是否上报 |
| `smoke_udp_rewrite.ps1` | 校验 IAT 重写模式下 UDP 桥是否工作 |
| `smoke_wgi.ps1` | 校验 Windows.Gaming.Input 假手柄 COM 路径 |
| `smoke_di.ps1` | 校验 DirectInput8 / HID / WMI 假手柄路径 |
| `smoke_options_override.ps1` | 校验 `focus_options.txt` 各开关生效 |

所有脚本输出到 `logs\`，按 PID 命名。

## 6. 排错清单

- **编译报错 `was not declared in this scope`**：多半是其它 AI 新加了调用但没加对应 helper；参考已有 `WgiRawRealStaticsAcquire`（约 7789 行）写法补一个同型函数
- **`-lws2_32` / `-lole32` 链接失败**：确认用的是 mingw 版 g++，而不是 MSVC 的 `cl`
- **运行时找不到 `libwinpthread-1.dll` / `libstdc++-6.dll`**：少加了 `-static`
- **注入后游戏闪退 / 弹"heap allocation failed"**：多半是 RawInput / IAT 重写相关，先关闭"屏蔽键鼠"开关，再用 `smoke_*.ps1` 逐个路径排查
- **真手柄透传异常**：确认 `WgiRealStatics2Acquire` 存在（7790 行附近），`g_wgiRealStatics2` 在 `WgiFakeInstall` 里被捕获到

## 7. 版本记录

| 时间 | 变更 |
|------|------|
| 2026-10-15 | 清理冗余日志：UDP 每按键 Dlog / WGI RoGet QI 热路径 Dlog / 重复 Trace+Dlog 双写 / worker 安装进度噪音 |
| 2026-10-16 | 进一步减少日志：DiagEvent 默认彻底关、心跳仅出错时打、XInput 安装日志压缩为汇总行 |
| 2026-10-16 | 补上 `WgiRealStatics2Acquire` helper，支持真手柄时 `FromGameController` 透传 |
