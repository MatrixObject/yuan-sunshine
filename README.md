# YuanSunshine

Sunshine 的分支。

目标是：在 Sunshine 的桌面串流之外，额外实现**进程串流**：只串流指定进程的窗口，输入也只注入该进程。

当前已实现**窗口伪聚焦**——让目标窗口保持前台状态正常渲染，可接受游戏手柄输入，同时主机焦点不受影响。

进程控制部分参考了星喵小桌面的实现方式，它有完备的进程串流功能。

相关项目：

- Sunshine：https://github.com/LizardByte/Sunshine
- 星喵小桌面：https://www.palstreaming.com