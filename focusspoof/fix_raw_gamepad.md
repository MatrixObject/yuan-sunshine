# focusspoof RawGameController 修复记录

## 日期
2026-10-15

## 问题
focusspoof 在双影奇镜 (Split Fiction, UE5) 中，只有插物理手柄才能控制，不插手柄不行。

## 根因
UE5 通过 `IRawGameController` 读取实际输入，`IGamepad` 只用于枚举手柄。
focusspoof 的假 `IGamepad` 工作正常 (`wgiread=2270`)，但假 `IRawGameController` 未被游戏使用。
日志显示 `wgi: RawGameController activation intercepted` 但无后续方法调用。

## 修复
在 `MyRoGetActivationFactory` 的 RawGameController 分支添加 IID 诊断日志：
- 打印游戏请求的 IID
- 打印 IID 是否匹配 `kWgiIidIRawGameControllerStatics` (match=0/1/2/3/4)
- 打印 `QueryInterface` 结果

## 结果
修复后不插物理手柄也能正常控制。

## 关键文件
- `focusspoof.cpp` — `MyRoGetActivationFactory` (line ~8504)
- 64 位 DLL: `focusspoof.dll`
- 32 位 DLL: `focusspoof32.dll`

## 编译方法
```bash
# 64 位
export PATH=/c/ProgramData/mingw64/mingw64/bin:$PATH
g++ -shared -O2 -s focusspoof.cpp -o focusspoof.dll -static -lws2_32 -lole32 -loleaut32

# 32 位
export PATH=/d/msys64/mingw32/bin:$PATH
g++ -shared -O2 -s focusspoof.cpp -o focusspoof32.dll -static -lws2_32 -lole32 -loleaut32
```