# 资源来源与许可证

日期：2026-10-24（GPT 咖啡杯原画接入）。本文件记录随仓库维护的非代码资源及其可核查来源。

## 结论摘要

- **项目许可证为 MIT**，根 `LICENSE` 为 MIT 全文（`Copyright (c) 2026 Unsleep contributors`，
  由 ChatGPT 加入）。新的图标原画与转换产物按该 MIT 条款授权。
- **造型由 GPT 绘制**，人类选定咖啡杯方案后，本项目只做确定性格式转换
  （裁切 / 去白底 / 重采样 / 调色 / 角标合成 / ICO 封装）。本项目**不重新设计造型**，
  不从图标库、字体或网络取素材，也不描边重建杯子。
- **当前随产品使用的 15 个图标**全部由 `resources/icons/generate-icons-gpt.mjs`
  从同一张原画生成，可按 `DESIGN.md` 的参数**逐字节重新生成**。
- **历史图标**（5 个旧图标 + `color-*.ico`）来源仍**未知（unverified）**，已不随当前产品使用。
  其哈希仅作历史核查记录保留在本节；**不为未知来源的历史素材补加授权声明**，也不改写 Git 历史。

## GPT 原画（MIT）

| 项 | 值 |
| --- | --- |
| 设计稿路径 | `.pi/design/unsleep-gpt-cup-refinement-002.png` |
| 仓库内副本 | `resources/icons/source/gpt-cup-refinement-002.png` |
| 尺寸 / 格式 | 1536×1024，24 位 RGB PNG |
| 字节 | 1225510 |
| SHA-256 | `8A421526679EDA08DE44FBD12676ABB3DE2034958DB7B75B04E03C9F021E4F58` |
| 作者 | GPT（本项目委托，人类选定 C 稿） |
| 许可 | 仓库根 `LICENSE`（MIT） |

转换脚本在读取时强制校验该 SHA-256，不匹配即退出，因此无法在不知情的情况下换成别的图。
`.pi/design/` 是任务输入目录；`resources/icons/source/` 的副本是转换的**权威输入**，
`--source` 子命令负责安装并校验。

## 当前图标（本项目生成，MIT）

生成方式：`node resources/icons/generate-icons-gpt.mjs`
（校验 `--check`，对比度与合成记录 `--report`，预览 `--preview <dir>`，接触图 `--sheet <dir>`，
文字视图 `--dump <file>`，安装源图 `--source`）。
设计参数、单元格坐标、裁切与调色规则见 [`resources/icons/DESIGN.md`](../resources/icons/DESIGN.md)。

下表为 t11-cup-final-011 实际写盘的 SHA-256（连续多次独立生成核对一致）：

| 文件 | 字节 | SHA-256 | 用途 |
| --- | --- | --- | --- |
| `resources/app.ico` | 16972 | `49F5E352CFA423C94664C4DF563DFC6B03977A746FE4C74FF4DB37256DFC9079` | 应用图标：基础 ON 咖啡杯，无角标（ID 128） |
| `resources/light-active.ico` | 16972 | `49F5E352CFA423C94664C4DF563DFC6B03977A746FE4C74FF4DB37256DFC9079` | 浅背景 + 运行（ID 140） |
| `resources/light-inactive.ico` | 12053 | `F39F4FE1773962B267C002E3C4EEAB0BD31F652F356B3C2146213705134A0E2D` | 浅背景 + 停止（ID 130） |
| `resources/light-screen.ico` | 18837 | `B1122682F9AB206AE821CB7C71244D53D30ECACF0E7EBA7C8AF9B41816CAC33A` | 浅背景 + 屏幕生效，实线显示器（ID 150） |
| `resources/light-screen-paused.ico` | 19199 | `0FF81915EEDF756510228BDD385CD50778ED1E17D802A67BBF04658B23BD9E99` | 浅背景 + 屏幕暂缓，虚线显示器（ID 151） |
| `resources/light-duration.ico` | 19569 | `3F4F543D109BE8C679B6A21C1DAA3E7F8636A09267A1C50A697EC7E182C2306F` | 浅背景 + 有限时长（ID 152） |
| `resources/light-screen-duration.ico` | 21199 | `5D80E22138184DD22FA8FBBFF5F0742C6F176421CE5F2F6FC9B6A5F0ECCC9E3F` | 浅背景 + 屏幕生效 + 时长（ID 153） |
| `resources/light-screen-paused-duration.ico` | 21566 | `93A5C03EDE679E66E259A01D2F915732CD85F01C66C010EFDB46FA1CB5491E36` | 浅背景 + 屏幕暂缓 + 时长（ID 154） |
| `resources/dark-active.ico` | 16995 | `CD187E98B7A60285DAC8DE499B108C48A560669A69BA0B091D14FFB08E77D2B7` | 深背景 + 运行（ID 145） |
| `resources/dark-inactive.ico` | 12067 | `B8DD30E60AAEA3E899181345BDDBCF0FEFC2E3139A57DA60FF3D02B985A2C916` | 深背景 + 停止（ID 144） |
| `resources/dark-screen.ico` | 18844 | `A9D7626D2F969F0464F436663AEEB43D961187B70DDBF3FE47274195CB765B6A` | 深背景 + 屏幕生效（ID 155） |
| `resources/dark-screen-paused.ico` | 19210 | `EB6B0D57C0495CC30733C412EA5E9C89656D195CBDCC7786D930847043948849` | 深背景 + 屏幕暂缓（ID 156） |
| `resources/dark-duration.ico` | 19592 | `34DCD48522DB4402AAF013DBCEFC057A02CDB203C1E737DCADB50B88902D82A2` | 深背景 + 有限时长（ID 157） |
| `resources/dark-screen-duration.ico` | 21221 | `9F58FD751BA2C859836164F59B53907F927DBC00046C0E7FBE5C9D5BA392D500` | 深背景 + 屏幕生效 + 时长（ID 158） |
| `resources/dark-screen-paused-duration.ico` | 21588 | `6BD128BE86A9B0F63A39E870BEB5E96DE00CCE20E87539F440FD81EDD26434CC` | 深背景 + 屏幕暂缓 + 时长（ID 159） |
| `resources/unsleep.rc` | 1479 | `E954D3D24D3CE213F41DEB083ED05FF202DA8D97349325674FF9FBBD9831B54E` | 菜单 / 图标 / 清单定义 |
| `resources/app.manifest` | 884 | `99CDFF4FA0C2731E296FA8349277C8F942CEAF1BDC0B639D866E8EA6A4F2F193` | 通用 Win32 清单文本 |

每个 ICO 内含 16/20/24/32/48/64/256 七种尺寸的 PNG 载荷，均由同一份 226×226 画布独立做
精确面积平均重采样。

**七个运行时状态里六个就是原稿整格**（OFF / ON / SCREEN / TIMER / BOTH / PAUSED）。
`PAUSED` 格只有一个**左侧虚线显示器**——没有时钟、没有 pause 扇形、内部没有斜线填充。
因此 `light-screen-paused.ico` 与 `light-screen-paused-duration.ico` 现在**不再相同**：
后者是唯一的合成态（`BOTH` 整格 + 把左侧实线显示器换成 `PAUSED` 的虚线显示器，右侧时钟
保持 `BOTH` 原样）。早期"PAUSED 自带时钟、要搬到左边"的说法是误读，已删除。

`app.ico` 与 `light-active.ico` 字节相同，这是"应用图标就是基础 ON 咖啡杯"的必然结果，
不是缺陷。**不要求 15 个文件字节两两不同**。

### 命名与语义

文件名中的 `light-*` / `dark-*` 指**目标任务栏背景**，不指笔画明暗：

- `light-*.ico` = 给**浅色**背景用，笔画是**深色**（`#1C1C1C`）；
- `dark-*.ico` = 给**深色**背景用，笔画是**浅色**（`#E8E8E8`）。

生成器表内 `background` 与 `palette` 是两个独立字段，`--report` 输出各自实测对比度，
不依赖文件名推断。运行时代码按 `light_taskbar()` 测出的**背景**选择对应资源。

### 运行时代码引用关系

`resources/unsleep.rc` 声明了 `src/win32.cpp` 使用的资源：

- 图标 ID：`128`（应用图标）；浅背景 `130`/`140`/`150`~`154`；深背景 `144`/`145`/`155`~`159`。
- 菜单 ID：`129`，包含五项产品功能与一个分隔符。
- 清单 ID：`1`，指向 `app.manifest`。

`CMakeLists.txt` 把 15 个 `.ico` 与 `app.manifest` 声明为 `unsleep.rc` 的 `OBJECT_DEPENDS`
依赖，并用 `CMAKE_CONFIGURE_DEPENDS` 跟踪原画与转换脚本，因此替换图标会真正触发资源重编译。
构建步骤见 [README](../README.md#开发环境与构建)；历史资源依赖验证见项目内 `.pi/reports/t11-cup-review-009/` 与 `.pi/reports/t11-cup-final-011/`（改内容会重编，构建失败不会留下陈旧 `.rc.res`）。

## 历史图标（已不随当前产品使用）

以下文件来自本项目初始提交 `d1b53f1`。它们是标准 4,286 字节 ICO 容器，**原创性与许可证无法从
历史证明**，因此记为未知。它们已被 GPT 原画生成的图标替换；记录在此仅为历史核查，
不进入当前 exe 或本地交付包。**这些历史素材不因本项目采用 MIT 而被主张为 MIT**——
MIT 只覆盖本项目自行生成或委托生成的内容。

| 文件 | 字节 | SHA-256 | 当前状态 |
| --- | --- | --- | --- |
| `resources/app.ico`（旧） | 4286 | `51BF8307C3D0791EAD82865DDCF7848DE6732FAA7381647BF715F58DC40C25DE` | 已替换 |
| `resources/color-active.ico` | 4286 | `51BF8307C3D0791EAD82865DDCF7848DE6732FAA7381647BF715F58DC40C25DE` | 未引用，已移出 |
| `resources/color-inactive.ico` | 4286 | `6CA8C61F6EC56C544C121DE6AAFEE42C8F8C18F549F325A7AC4CA31F6C6B5334` | 未引用，已移出 |
| `resources/light-active.ico`（旧） | 4286 | `A9178BEA5F08F465E034BEA8324766812613FBA8AEFE55503869529012E7823A` | 已替换 |
| `resources/light-inactive.ico`（旧） | 4286 | `7A5BD2CFB6705A9957C554F996D6F6AA01FCFF0C89755B0DCC2595B3576C1C73` | 已替换 |
| `resources/dark-active.ico`（旧） | 4286 | `235053D493E320B3BA4AE95D677F47DDA6A2F6878FFB88FA61A49F7CFF1F6601` | 已替换 |
| `resources/dark-inactive.ico`（旧） | 4286 | `2B0E16AB5CF62CB97C7F62A597EFBC8EDF4F8CC8BC848B2F4DA8EDDE99BA62ED` | 已替换 |

旧文件的原始字节备份在任务报告目录（项目内、未被 Git 跟踪），不进入交付包。
Git 历史未改写，初始提交仍可追溯这些哈希。

### 已被取代的中间版本

`t11-config-icons-006` 使用的眼睛/U 形母题生成器 `resources/icons/generate-icons.mjs`
已被移除（人类在看过 GPT 视觉稿后改为采用咖啡杯方案）。其源码与当时的图标备份在
`.pi/recovery/before-gpt-cup-007/`。该造型**不得再用于任何资源**。

## 第三方代码依赖

- 无。构建只使用 Windows SDK / MSVC 标准库；`CMakeLists.txt` 未使用 FetchContent、vcpkg
  或任何下载步骤。
- 图标转换器为开发期工具，使用 Node.js 标准库，不参与产品构建，也不是运行时依赖。

## 核查方法与可复核性

```powershell
# 安装/校验原画副本，然后确认可重复（三次运行哈希应完全一致）
node resources/icons/generate-icons-gpt.mjs --source
node resources/icons/generate-icons-gpt.mjs --check

# 来源 / 单元格 / 背景 / 调色 / 对比度
node resources/icons/generate-icons-gpt.mjs --report

# 当前图标哈希
Get-ChildItem resources\*.ico | ForEach-Object { "$($_.Name)|$($_.Length)|$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)" }

# 明暗背景接触图（实际 16/20/24/32 各一列 + 16 px 的 8 倍放大；每状态独占一行）
node resources/icons/generate-icons-gpt.mjs --sheet .pi/reports/t11-cup-final-011/evidence/sheets

# 结构校验（读已写入的 .ico 载荷，证明 PAUSED 没有隐藏时钟、合成态保留 BOTH 的时钟）
node .pi/tasks/t11-cup-final-011/verify-structure.mjs

# 接触图排版审计（宽 ≤ 1400、行高 160、不跨行、放大 128 px）
node .pi/tasks/t11-cup-final-011/audit-sheets.mjs
```

## 未完成事项

- 历史旧图标的原创性仍未确认，但已不随当前产品使用，也不被主张为 MIT。
- 真实任务栏下的目视验收（16 px 可读性、明暗对比观感）仍需人工看图；
  本轮的 16 px 判断基于逐像素 alpha 度量与尺寸检查，不是人眼观感结论。
