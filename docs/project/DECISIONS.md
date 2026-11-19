# MarkdownShelf 决策记录

正式依据为 [V4.0 设计规格](../specification/MarkdownShelf_Project_Design_v4.0.md)。产品决策及原始确认的出处见 §1.1 和 §13.1。当前批准记录见[任务状态](CURRENT_TASK.md)，逐项验收见[需求清单](../requirements/ACCEPTANCE_CHECKLIST.md)。

| 日期 | 来源 | 决策与适用范围 | 状态 |
| --- | --- | --- | --- |
| 2026-11-19 | V4.0 §1.1、§10.1 | 开发模式 B。每个 P 先批准方案再编码，完成实现、测试、自审、Deep 讲解和学习文档后再次等待该 P 验收。 | 已确认规则 |
| 2026-11-19 | V4.0 §1.1、§1.2、§2、§2.4、§11 | Windows V1.0 的技术和产品边界包括 C++17 核心、pybind11、PySide6、简体中文、离线使用，以及标准安装版和绿色便携版。 | V4.0 基线 |
| 2026-11-19 | P1.1 实施方案 v1，V4.0 §2.1–2.3、§10.2 | 采用 Ninja 生成器、MSVC v143 x64、CPython 3.12 x64。分开构建和暂存 Debug 与 Release。扩展通过同步探针验证 UTF-8 和版本化结构化结果。若发现调试版 Python 依赖，按 §2.2 实测并记录回退。 | 方案已批准 |
| 2026-11-19 | P1.1 首轮编译和 ABI 实测 | CMake 最低 3.25，Debug 用 `/Z7` 处理本机 `/Zi` 的 C1902，仍产出链接 PDB。Debug 和 Release 均在 CPython 3.12.10 x64 导入，使用原有预设。 | 工程选择，已复验 |
| 2026-11-19 | 用户补充 P1.1 的 clangd 配置 | 两种 Ninja 预设导出编译数据库，clangd 工作区默认用 Debug。配置时从 Developer PowerShell 取得 MSVC 和 Windows SDK 头文件路径，工作区使用 clangd 处理 C++ 语言功能。 | 工程选择，已复验 |
