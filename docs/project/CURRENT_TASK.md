# MarkdownShelf 当前开发状态

## 正式规格

[MarkdownShelf_Project_Design_v4.0.md](../specification/MarkdownShelf_Project_Design_v4.0.md)，SHA-256 `1D9DF12402C2E9855F83C6154C2F072EEFF871ADBA3A306439BEE7A5AD4A004E`。

## 当前功能与阶段

P1.2 C++ SQLite 基础，方案 v1，实现和本机验证已完成，等待用户验收。方案依据为 V4.0 §2.1–2.4、§3.1–3.4、§6.1–6.2、§8.3、§9.1、§10.1 和 §10.2。

## 需求与测试

[P1.2 需求映射](../requirements/ACCEPTANCE_CHECKLIST.md#p12-c-sqlite-基础)包含 REQ-P1.2-01 至 REQ-P1.2-06 和对应测试。另见 [P1.2 学习文档](../development/P1.2_sqlite_foundation.md)、[P1.2 验证记录](../testing/P1.2_sqlite_foundation.md)和[决策记录](DECISIONS.md)。前置功能见 [P1.1 需求映射](../requirements/ACCEPTANCE_CHECKLIST.md#p11-可重复构建与最小绑定)及 [P1.1 验证记录](../testing/P1.1_build_and_abi.md)。

## P1.2 方案 v1

- 依赖与构建。拟采用 [SQLite 3.54.1 官方 amalgamation](https://www.sqlite.org/download.html)，核对上游 SHA3-256，并在 `CMakeLists.txt` 锁定归档 SHA-256。使用现有 C++ 依赖缓存路径，按 C 编译 `sqlite3.c`，显式启用 FTS5 并静态编入核心。
- 连接与事务。在 `src/core/db/` 和 `src/core/include/markdownshelf/` 提供 C++ 连接、预编译语句和事务。连接独占且由所属线程使用，语句和事务的寿命受连接约束，析构路径不抛异常。区分创建、读写和只读打开方式。启用外键与可配置的有界等待，超时默认拟定为 1,000 ms。受信任的可写文件库由调用入口在身份及版本检查后启用 WAL 和 `synchronous=FULL`。事务显式提交，未提交退出和提交失败时按 SQLite 事务状态回滚。
- 迁移。在 `src/core/migrations/` 提供版本化迁移执行入口，读取 `PRAGMA user_version` 并检查迁移序列。每一步的 SQL、数据变更和版本更新同事务提交。用测试用 schema 验证迁移，正式库初始化归 P1.3，保护快照和库目录发布归 P4.9。
- FTS5。提供逐连接注册入口，用具备版本名的测试 tokenizer 验证 CREATE、INSERT、绑定 MATCH 及重开后注册。正式中文和技术词规则以及业务索引归后续搜索功能。
- 验证与交付。在 `tests/cpp/` 添加 GoogleTest 用例，复用现有 CTest、严格编译及 Python ABI 测试。运行 Debug、Release、独立核心及本地缓存复验。更新 README，提交 `docs/development/P1.2_sqlite_foundation.md`、`docs/testing/P1.2_sqlite_foundation.md` 及需求证据，并在对话中交付自审和 Deep 源码讲解。

## 用户确认记录

| 确认事项 | 状态 | 用户回复定位 |
| --- | --- | --- |
| 管理文件初始化与 P1.1 方案 v1 | 已批准 | 2026-11-19，MarkdownShelf V1.0 启动会话，用户回复「管理文件初始化 和P1.1 实施」。 |
| P1.1 补充 clangd 工作区配置 | 用户已提出 | 2026-11-19，用户补充「同时也把clangd配置完成」。 |
| P1.1 交付验收 | 已验收 | 2026-11-19，用户回复「p1.1验收完成，接下来进行p1.2」。 |
| P1.2 方案 v1 | 已批准 | 2026-11-19，用户回复「批准 P1.2 方案 v1」。 |
| P1.2 交付验收 | 等待用户验收 | 2026-11-19，当前会话提交实现、测试证据、自审及 Deep 源码讲解。 |

## 已完成内容

P1.1 已验收。P1.2 已实现 SQLite 3.54.1 静态编入核心、连接与语句 RAII、事务提交与回滚、逐步版本迁移及 FTS5 tokenizer 注册入口。已完成源码自审、学习文档和测试证据关联。

## 已执行测试

MSVC v143 下，Debug 和 Release 的默认构建与本地缓存干净构建各通过 16 项 CTest、8 项 pytest 和暂存导入，关闭绑定的独立核心通过 16 项 CTest。严格编译、格式检查、错误归档拒绝、静态链接和 clangd 前台 LSP 验证均有记录。[P1.2 验证记录](../testing/P1.2_sqlite_foundation.md)

## 当前待处理事项与下一步

托管 GitHub Actions 本轮未执行。等待用户确认 P1.2 验收结果。验收后进入 P1.3 方案阶段。
