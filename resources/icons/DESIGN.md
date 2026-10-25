# Unsleep 图标：GPT 原画与其确定性转换

本文件记录图标资源的**来源**与**转换参数**。造型由 GPT 绘制，本项目不再自行设计造型；
`resources/icons/generate-icons-gpt.mjs` 只做确定性技术处理，不做任何几何重画。

## 来源

| 项 | 值 |
| --- | --- |
| 设计稿 | `.pi/design/unsleep-gpt-cup-refinement-002.png`（1536×1024，24 位 RGB） |
| 仓库内副本 | `resources/icons/source/gpt-cup-refinement-002.png` |
| SHA-256 | `8A421526679EDA08DE44FBD12676ABB3DE2034958DB7B75B04E03C9F021E4F58` |
| 作者 | GPT（人类选定 C 稿：咖啡杯） |
| 许可 | 仓库根 `LICENSE`（MIT，Copyright 2026 Unsleep contributors） |
| 安装命令 | `node resources/icons/generate-icons-gpt.mjs --source` |

生成器在读取源图时**强制校验**上述 SHA-256，不匹配即报错退出，因此无法在不知情的情况下
换成别的图。转换脚本本身只用 Node.js 标准库（`node:fs`/`node:path`/`node:crypto`/`node:zlib`），
不联网、不读时钟、不使用随机数，也不读取任何"文件来历"信息：输出只是源图像素与下列常量的函数。

## 母题

一杯冒热气（绿色蒸汽）的咖啡，杯身右侧带把手，杯身内有表情。原画是六个单元格，每格是同一套
画法的**一种状态**，本项目不重新设计：

| 单元格 | 原画内容 | 用途 |
| --- | --- | --- |
| `ON` | 杯 + 蒸汽 + 表情，无角标 | 运行、无配置角标；**应用图标** |
| `OFF` | 只有杯身（无蒸汽、无表情、无角标） | 停止 |
| `SCREEN` | 杯 + 蒸汽 + 表情 + 右侧**实线**显示器 | 屏幕常亮已生效 |
| `TIMER` | 杯 + 蒸汽 + 表情 + 时钟 | 有限时长 |
| `BOTH` | 杯 + 蒸汽 + 表情 + **左侧实线显示器 + 右侧时钟** | 屏幕生效 + 有限时长 |
| `PAUSED` | 杯 + 蒸汽 + 表情 + **左侧虚线显示器**（仅此一个标记） | 屏幕暂缓 |

`PAUSED` 格**只有一个虚线显示器**：没有时钟、没有 pause 扇形、内部没有斜线填充。早期文档和
转换脚本曾据此误推出"PAUSED 自带时钟、需要搬到左边"，那是错的：显示器本来就在左边，
原稿里也没有多余的时钟可以搬。`PAUSED_CLOCK` / `moveClock` 已从脚本删除，不得再加回。

## 裁切与统一画布

裁切框由连通域扫描测得，不含标签（标签行在 y≥440）、不含分隔线和背景。所有单元格共用同一
纵向范围 `y = 233..436`（233 是蒸汽顶端，436 是最低角标底端），横向各取本格自绘画范围：

| 单元格 | x 范围 | 宽度 |
| --- | --- | --- |
| `ON` | 64–242 | 179 |
| `OFF` | 313–490 | 178 |
| `SCREEN` | 555–733 | 179 |
| `TIMER` | 799–978 | 180 |
| `BOTH` | 1044–1229 | 186 |
| `PAUSED` | 1300–1478 | 179 |

各格宽度 178–186 px 是原画自身的手绘差异，不做归一化。横向**不居中**：所有格子的裁切左边
都对齐到画布同一列 `x=8`，这样六格彼此套准，杯身不会因为格宽不同而在状态之间跳动。纵向
共用 `y = 233..436`，占画布 `y = 21..224`。

## 去底

原画背景是白色，像素值在 249–255 之间轻微抖动。转换以"最小通道距 255 的距离"作为覆盖率：

- 距离 ≤ 14 视为纯背景 → 完全透明；
- 距离 > 14 按覆盖率求 alpha，并把白色**反解**出去，使半覆盖的边缘像素保留原作者的颜色，
  而不是变成发白的一圈。

**白色是原稿的构图元素**：原稿在杯口内壁与角标之间留了一条白缝来切断杯沿。它是"没有墨"，
所以去底后该处为透明，与原有的杯内白区、轮廓外白区一样。**不额外发明轮廓**：转换只在原稿
已有镂空处保持透明，不描边、不补线、不加外框。

## 重采样与尺寸

每个尺寸都从同一份 226×226 画布**独立**做精确面积平均，不是缩放位图。尺寸固定为
16 / 20 / 24 / 32 / 48 / 64 / 256。

## 配色

`background` 指目标任务栏，`palette` 指笔画实际颜色，二者相反：

| 任务栏 | 笔画 | 蒸汽 | 对 `#F3F3F3` / `#202020` 的对比度 |
| --- | --- | --- | --- |
| 浅色（light） | `#1C1C1C` | `#1B6B2F` | 笔画 15.36 / 蒸汽 5.93 |
| 深色（dark） | `#E8E8E8` | `#60C972` | 笔画 13.30 / 蒸汽 7.83 |

明暗映射**只改颜色**：保留原画的明暗层次（最深的像素落到调色板笔画色，仍带墨的浅边稍亮），
绿色蒸汽换到对应调色板的绿。**不改任何形状、不改任何覆盖度**——同状态的明暗两个文件因此
alpha 完全一致（自检第 1 项逐像素比对，见下）。

## 资源表

同一状态的明暗两个文件使用**同一个单元格、同一套构成**，除调色外完全一致：

| 资源 ID | 文件 | 单元格 | 构成 | 说明 |
| --- | --- | --- | --- | --- |
| 128 | `app.ico` | `ON` | 原样 | 应用图标：基础咖啡杯，**无配置角标** |
| 130 | `light-inactive.ico` | `OFF` | 原样 | 浅任务栏 + 停止 |
| 140 | `light-active.ico` | `ON` | 原样 | 浅任务栏 + 运行 |
| 150 | `light-screen.ico` | `SCREEN` | 原样 | 屏幕常亮已生效（实线显示器） |
| 151 | `light-screen-paused.ico` | `PAUSED` | 原样 | 屏幕暂缓（虚线显示器） |
| 152 | `light-duration.ico` | `TIMER` | 原样 | 有限时长 |
| 153 | `light-screen-duration.ico` | `BOTH` | 原样 | 屏幕生效 + 时长（实线显示器 + 时钟） |
| 154 | `light-screen-paused-duration.ico` | `BOTH` | 左侧显示器换成 `PAUSED` 的虚线显示器 | 屏幕暂缓 + 时长 |
| 144 | `dark-inactive.ico` | `OFF` | 原样 | 深任务栏 + 停止 |
| 145 | `dark-active.ico` | `ON` | 原样 | 深任务栏 + 运行 |
| 155 | `dark-screen.ico` | `SCREEN` | 原样 | 屏幕常亮已生效 |
| 156 | `dark-screen-paused.ico` | `PAUSED` | 原样 | 屏幕暂缓 |
| 157 | `dark-duration.ico` | `TIMER` | 原样 | 有限时长 |
| 158 | `dark-screen-duration.ico` | `BOTH` | 原样 | 屏幕生效 + 时长 |
| 159 | `dark-screen-paused-duration.ico` | `BOTH` | 左侧显示器换成 `PAUSED` 的虚线显示器 | 屏幕暂缓 + 时长 |

两个方向与运行时 `src/win32.cpp` 的 `badge_slot()` / `light_running_icons` / `dark_running_icons`
一致：`badge_paused` 与 `badge_screen` 同时出现时归到暂停格（暂缓不得看起来像已生效）。

### 唯一的合成：PAUSED + TIMER

七个运行时状态里六个就是原稿整格，一个字都不用改。**只有一个组合原稿没画**："屏幕暂缓、
但有时长"。它由两格的已有像素拼成，**不新画任何线条**：

1. 起点是 `BOTH` 整格：杯 + 蒸汽 + 表情 + 左侧实线显示器 + 右侧时钟；
2. 清掉 `BOTH` 左侧显示器所在的矩形；
3. 把 `PAUSED` 的虚线显示器整块贴到同一位置。

源图绝对坐标（实测，非推断）：

| 块 | 源矩形 | 尺寸 |
| --- | --- | --- |
| `BOTH` 左侧实线显示器 | `[1043,376]..[1115,437]` | 73×62 |
| `PAUSED` 虚线显示器 | `[1407,377]..[1479,437]` | 73×61 |

两块等宽（73 px），高度差 1 px 是手绘误差，因此**不缩放**：按 `BOTH` 矩形原点落位即可。
画布坐标 `x=7..79, y=154..215`。杯子与右侧时钟完全不碰——自检第 3 项逐像素证明
`x > 80` 的区域与 `BOTH` 完全相同（0 像素差异）。

**明确禁止**：把 `PAUSED` 当"缺时钟"来搬时钟、使用 `BOTH_CLOCK_OFFSET`、把显示器误当作
clock、往 `PAUSED` 里叠第二个显示器、为"独特性"要求 15 个文件字节互不相同。

## 自检（写盘前必须全部通过）

`generate-icons-gpt.mjs` 在主流程里先跑六项检查，任一项失败就不写盘并退出码 1：

1. **同状态的明暗两文件在 7 个尺寸上 alpha 逐像素相同**——明暗只允许颜色不同。
2. 屏幕生效（实线）与屏幕暂缓（虚线）不得使用同一单元格；且 `paused+duration` 在 16/32/256
   三个尺寸上必须真的不同于 `BOTH`，否则"换虚线"没有生效。
3. **合成后右侧时钟必须仍是 `BOTH` 的**：`x > 显示器右缘` 的区域与 `BOTH` 逐像素相同。
4. 带角标的 16 px 必须与同调色的无角标版有像素差异（角标在托盘里看得见）。
5. `resources/unsleep.rc` 必须逐条声明这些文件名与 ID，且不多不少。
6. 256 px 下左右边缘不得有墨（图形不被裁切）。

另有独立的结构校验脚本（读已写入的 `.ico` 载荷，不读脚本内部状态）：
`.pi/tasks/t11-cup-final-011/verify-structure.mjs`，证明 `PAUSED` 的显示器右侧**什么都没有**
（没有隐藏的时钟）、合成态右侧的时钟与 `BOTH` 字节相同、杯子未被改动。

**不要求 15 个文件字节互不相同**：`app.ico` 与 `light-active.ico` 除 ICO 头外本就完全相同，
这是"应用图标用基础 ON 咖啡杯"的必然结果，不是缺陷。同理 `light`/`dark-screen-paused.ico`
与各自 `-duration` 版不再相同（它们现在构成不同），但**同状态的明暗两文件 alpha 必须一致**。

## 可复现

```powershell
node resources/icons/generate-icons-gpt.mjs --source        # 安装/校验源图副本
node resources/icons/generate-icons-gpt.mjs                 # 生成 15 个 .ico
node resources/icons/generate-icons-gpt.mjs --check         # 只比对，不写盘
node resources/icons/generate-icons-gpt.mjs --report        # 来源/单元格/合成/对比度表
node resources/icons/generate-icons-gpt.mjs --preview <dir> # 105 张实际尺寸 + 每状态 128/256 预览
node resources/icons/generate-icons-gpt.mjs --sheet <dir>   # 明暗 + app 三张审查用接触图
node resources/icons/generate-icons-gpt.mjs --dump <txt>    # 组合结果的文字视图
```

`--report` 会打出每个变体的 `cell`、`moves`（合成块的源矩形、尺寸、缩放比）以及
`screen_geometry`（两块显示器的画布坐标），`--dump` 逐 ASCII 打出组合后的 226×226 画布。

连续多次运行的 SHA-256 完全一致（输出只是源图字节与上述常量的函数）。

## 供人眼审查的接触图

由生成器自身的 `--sheet <dir>` 产出（只用 `node:zlib` 写 PNG，无外部依赖，不设计图标）。
排版规则是硬性的，`audit-sheets.mjs` 逐条测量生成结果：

- 每个状态**独占一行**，行高 160 px，任何内容不得跨行（009 那版行高 96、放大 256，整列互相
  覆盖，已废弃）；
- 实际 16/20/24/32 px **各占一列**，各自独立单元格，不跨行；
- 放大列是 **16 px × 8 = 128 px**，不是 32 px × 8；
- 总宽 ≤ 1400 px；
- **明暗分成两张图**，各用自己主题的任务栏底色，浅色笔画绝不画在浅底上；
- app 图标单独一小节，浅底（它的笔画本来就是深色）。

输出：

- `.pi/reports/t11-cup-final-011/evidence/sheets/contact-sheet-light.png`
- `.pi/reports/t11-cup-final-011/evidence/sheets/contact-sheet-dark.png`
- `.pi/reports/t11-cup-final-011/evidence/sheets/contact-sheet-app.png`

**排版只是测试报告，不是产物**：接触图里的每个图标像素都来自已写入的 `.ico`，排版函数不
重绘、不缩放、不改色。

## 与旧图标的关系

上一版（`generate-icons.mjs`，眼睛/U 形母题）**不再使用**，文件已从 `resources/` 移除，
备份留在 `.pi/recovery/before-gpt-cup-007/`。旧图标的造型不得恢复，也不得再用于任何资源。
