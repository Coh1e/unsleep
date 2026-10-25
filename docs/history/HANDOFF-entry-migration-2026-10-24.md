> 历史记录，不是当前任务入口。

# 文档入口迁移（临时）

业务开发、业务构建/运行、功能测试和逆向继续暂停；允许文档整理和工作流配置验证。

本项目取消长期交接台账。通用协作流程见 docs/AI-WORKFLOW.md，任务与验收使用 Coh1e/unsleep 的 GitHub Issues，技术约束见 docs/BUILD.md。

AGENTS.md 完整替换稿位于 docs/agents/AGENTS-proposed.md，按人类要求仍待明确确认；根目录旧 AGENTS.md 尚未覆盖。本文件仅为兼容旧 AGENTS.md 的必读入口，确认新稿后与草稿一起删除。

本轮已完成通用工作流与项目配置分离，新增 CMake + Ninja + MSVC、FetchContent 或项目内源码且不用 vcpkg 的约定。仅静态文档检查与远端任务同步；构建、程序运行、功能测试、逆向、Pi/DeepSeek 复跑及工具执行验证均未运行。未提交或推送，历史证据见 docs/history/，不作为本轮通过结果。

本轮已读取 Issues #1—#13 正文及评论，更新并回读确认正文匹配、任务状态不变；未修改原生关系。旧拆票清单已归档至 docs/history/TICKET-BREAKDOWN-2026-10-24.md，撤掉活跃副本。证据留在被忽略的 .pi/reports/docs-sync-20261024/。活跃文档本地链接检查通过，通用工作流未残留项目名称、模型或本地运行路径。

## 2026-10-24 AGENTS 更新收尾

人类明确要求开始写 AGENTS.md，并保留 Matt 内容。已在原文件基础上局部修改项目约定，Agent skills 段落原样保留；docs/agents 三份文件恢复为本机 Matt setup 技能模板原文。补齐项目技术约束与当前业务暂停边界，移除对长期 HANDOFF 的依赖。废弃整篇替换稿，不采用其内容。

本轮仅静态文档修改和核对；构建、程序运行、功能测试、逆向及 Pi/DeepSeek 复跑均未运行。未提交或推送。临时 HANDOFF 与废弃草稿在本记录归档后删除；任务状态和验收继续使用 GitHub Issues。