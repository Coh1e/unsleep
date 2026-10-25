# Unsleep

一个极简的 Windows 托盘防休眠工具。下载、编译或备份时，让电脑保持唤醒；需要时也可以保持屏幕常亮。没有主窗口，日常操作都在托盘菜单里完成。

使用 C++23 和原生 Win32 开发，无第三方运行库依赖，无需管理员权限。

## 使用

运行 `unsleep.exe` 即开启防休眠，默认允许屏幕熄灭。左键或右键点击托盘图标，打开同一个菜单：

| 菜单 | 作用 |
| --- | --- |
| 防休眠 | 开启或停止防休眠；停止后仍留在托盘 |
| 保持屏幕常亮 | 本次运行期间阻止自动息屏，默认关闭 |
| 持续时间 | 不限时、30 分钟、1 小时或 2 小时；到期后停止防休眠 |
| 电池保护 | 选择 10%、20% 或 30%，默认 20%；未插电且电量达到或低于阈值时停止 |
| 退出 | 退出程序并释放防休眠请求 |

每次重新开启都回到“不限时、允许息屏”。锁屏时允许息屏，解锁后按本次选择恢复；睡眠或唤醒后保持停止，需要手动开启。程序只运行一个实例。

电池阈值保存在程序同目录的 `unsleep.settings`，其他选项不保存。删除该文件即可恢复默认阈值；目录不可写时，本次选择仍有效，但无法保存。

目前尚未发布下载版本，可按下方步骤从源码构建。

## 使用限制

Unsleep 请求 Windows 阻止空闲自动休眠，不改变系统电源策略，也不能阻止手动睡眠、休眠、关机或合盖导致的睡眠。电池状态无法确认时，会停止防休眠。

当前只在 **Windows 11 x64 无电池设备**上完成验收。真实电池保护、插拔电、合盖及其他 Windows 版本和架构尚未验证；Modern Standby 下的效果取决于设备与系统策略。

## 开发环境与构建

需要 Windows、CMake ≥ 3.28、Ninja、Visual Studio 或 Build Tools 中的 MSVC C++ 工具及 Windows SDK。使用 C++23、静态 MSVC 运行时（`/MT`），当前无第三方代码依赖。

在项目根目录执行以下 PowerShell 命令。先将 `vcvars64.bat` 路径改为本机安装位置；环境设置只作用于子进程，临时文件和构建产物留在项目内。

```powershell
New-Item -ItemType Directory -Force .pi/tmp | Out-Null
# 在当前 PowerShell 的子进程中设置临时目录，不修改持久环境
powershell -NoProfile -Command {
    $env:TEMP = Join-Path $PWD '.pi/tmp'
    $env:TMP = $env:TEMP
    $vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    cmd /c "`"$vcvars`" && cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release && ctest --test-dir build-release --output-on-failure"
    exit $LASTEXITCODE
}
```
产物为 `build-release/unsleep.exe`。运行后立即开启防休眠；测试临时数据位于 `.pi/tmp/core-tests/`。

修改图标原稿或转换脚本后，使用已有 Node.js 执行 `node resources/icons/generate-icons-gpt.mjs`，再重新构建。CMake 已跟踪图标和清单依赖。

## 参与开发

产品目标见 [产品范围](docs/PRODUCT-SCOPE.md)，行为和验收规则见 [实施规格](docs/IMPLEMENTATION-SPEC.md)。问题、任务及验证记录在 [GitHub Issues](https://github.com/Coh1e/unsleep/issues)；AI 协作规则见 [AGENTS.md](AGENTS.md)。

## 许可证与资源

本项目采用 [MIT 许可证](LICENSE)。当前咖啡杯图标由 GPT 绘制、人类选定，经项目脚本转换为应用与托盘图标。原稿及转换脚本保留在仓库中，详细说明见 [资源来源](docs/RESOURCE-SOURCES.md)。
