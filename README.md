# Unsleep

Unsleep 是一个使用 C++23 和 Win32 实现的 Windows 托盘工具，通过定期按键或系统执行状态请求保持电脑唤醒。支持活动时段、定时启停、CPU 阈值、电源及窗口条件。

## 构建

需要已有的 Windows SDK、支持 C++23 的 MSVC，以及 CMake 3.28 或更新版本。以下示例适用于 Visual Studio 2026；使用其他版本时替换生成器名称。

```powershell
cmake -S . -B build/x64 -G "Visual Studio 18 2026" -A x64
cmake --build build/x64 --config Release
ctest --test-dir build/x64 -C Release --output-on-failure
```

32 位构建将生成目录改为 `build/x86`，架构改为 `-A Win32`。程序位于相应目录的 `Release/unsleep.exe`。工程直接编译源码和本仓库资源，不需要运行时第三方依赖。

## 使用

```powershell
.\unsleep.exe -showdlg -ontaskbar -startoff
.\unsleep.exe 60 -stes
.\unsleep.exe -activeperiods:0900-1200,1300-1800
.\unsleep.exe -appexit
```

默认每 59 秒触发一次 F15 键事件。托盘菜单可以切换活动状态、设置活动或非活动时长、显示状态和退出；About 窗口列出命令行选项。

| 参数 | 作用 |
| --- | --- |
| 开头的正整数 | 按键或执行状态请求的间隔秒数 |
| `-startoff` | 以非活动状态启动 |
| `-stes` | 请求保持显示器唤醒 |
| `-allowss` | 请求保持系统唤醒，允许屏幕保护程序 |
| `-useshift`、`-key:X`、`-keypress` | 选择 Shift、指定虚拟键或完整按下/抬起 |
| `-activefor:X`、`-inactivefor:X`、`-exitafter:X` | 设置分钟计时 |
| `-activeperiods:HHMM-HHMM,...` | 设置每日活动时段 |
| `-cpu:X`、`-onac`、`-watchwindow:X` | CPU、电源与窗口条件 |
| `-showdlg`、`-ontaskbar`、`-noicon`、`-notify` | 界面及状态通知选项 |
| `-appon`、`-appoff`、`-apptoggle`、`-appexit` | 控制当前桌面上的已有实例 |

`-watchwindow:` 使用该位置之后的全部文本作为窗口匹配内容，应放在命令行末尾。远程控制参数针对已有实例；使用时应避免在同一桌面运行多个实例。

实现逻辑和状态规则见 [软件设计](DESIGN.md)。
