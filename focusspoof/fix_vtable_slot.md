# 双影奇镜 (Split Fiction) 手柄模拟修复日志

**日期**: 2026-10-15
**游戏**: Split Fiction (UE5, D:\Games\Split Fictionv20250527)
**DLL**: focusspoof.dll (64-bit 249344 bytes) + focusspoof32.dll (32-bit 361472 bytes)

## 问题

双影奇镜是 UE5 游戏，通过 Windows.Gaming.Input (WGI) 读取手柄，不调用 XInput。
DLL 注入在游戏启动后执行，UE5 已在注入前获取了 WGI statics 并完成输入初始化。
因此 `RoGetActivationFactory` hook 无法拦截（日志: `roget=0 wgiact=0 wgilist=0 wgiadd=0`）。

## 解决方案：Vtable Hooking

替换游戏已缓存的真实 WGI statics 的 vtable 指针，让游戏通过已持有的 statics
也能调用到我们的假实现。

## 修复的两个关键 Bug

### Bug 1: Vtable Slot 索引全部错误（根本原因）

代码中 `WgiIGamepadStatics` 的 C++ struct 定义（继承自 IInspectable）：

```
IGamepadStatics vtable 布局:
  slot 0-2: IUnknown (QueryInterface, AddRef, Release)
  slot 3-5: IInspectable (GetIids, GetRuntimeClassName, GetTrustLevel)
  slot 6:   add_GamepadAdded
  slot 7:   remove_GamepadAdded
  slot 8:   add_GamepadRemoved
  slot 9:   remove_GamepadRemoved
  slot 10:  get_Gamepads

IGamepadStatics2 vtable 布局:
  slot 0-5: IInspectable
  slot 6:   FromGameController
```

**之前的错误代码**:
```cpp
// IGamepadStatics — 错误！slot 6 是 add_GamepadAdded，不是 get_Gamepads
copy[6] = &WgiVtableGetGamepads_Thunk;

// IGamepadStatics2 — 错误！slot 6 是 FromGameController，不是 get_Gamepads
copy[6] = &WgiVtableGetGamepads_Thunk;       // 应为 FromGameController
copy[7] = &WgiVtableAddGamepadAdded_Thunk;   // 不存在于此接口
copy[9] = &WgiVtableAddGamepadRemoved_Thunk; // 不存在于此接口
copy[11] = &WgiVtableFromGameController_Thunk; // 应为 slot 6
```

**修复后**:
```cpp
// IGamepadStatics — 11 个 slot
copy[6]  = &WgiVtableAddGamepadAdded_Thunk;    // add_GamepadAdded
copy[8]  = &WgiVtableAddGamepadRemoved_Thunk;  // add_GamepadRemoved
copy[10] = &WgiVtableGetGamepads_Thunk;         // get_Gamepads

// IGamepadStatics2 — 7 个 slot
copy[6]  = &WgiVtableFromGameController_Thunk;  // FromGameController
```

同时修复 `WgiVtableFromGameController_Thunk` 中原始 vtable 调用索引
`g_wgiOrigStatics2Vtable[11]` → `g_wgiOrigStatics2Vtable[6]`。

### Bug 2: 退出时致命错误（访问已释放的 COM 对象）

`WgiFakeRestore` 中恢复 vtable 时写入 `*reinterpret_cast<void***>(g_wgiRealStatics)`，
但游戏退出时可能已释放该 COM 对象，导致写入已释放内存崩溃。

**修复**: 跳过 vtable 恢复（进程退出不需要恢复），只释放 fake vtable 内存，
不再调用 `real->Release()` / `real2->Release()`。

## 修改的代码区域

- `focusspoof.cpp` ~line 6943: 添加 `g_wgiAddLogged` 声明
- `focusspoof.cpp` ~line 8487: 添加 `RoGetActivationFactory` 类名诊断日志
- `focusspoof.cpp` ~line 8564: `WgiVtableAddGamepadAdded_Thunk` 添加日志
- `focusspoof.cpp` ~line 8582: `WgiVtableFromGameController_Thunk` 添加日志 + 修复 slot 索引
- `focusspoof.cpp` ~line 8741: 修复 IGamepadStatics vtable hooking（slot 6/8/10, 大小 11）
- `focusspoof.cpp` ~line 8755: 修复 IGamepadStatics2 vtable hooking（slot 6, 大小 7）
- `focusspoof.cpp` ~line 8787: 简化 WgiFakeRestore，跳过 vtable 恢复和 Release

## 验证

日志中出现 `wgi: vtable add_GamepadAdded intercepted` 和 `wgiadd` 计数器非零，
游戏成功识别虚拟手柄。退出不再崩溃。