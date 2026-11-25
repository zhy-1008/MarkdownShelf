# MarkdownShelf 当前开发状态

## 正式规格

[MarkdownShelf_Project_Design_v4.0.md](../specification/MarkdownShelf_Project_Design_v4.0.md)，SHA-256 `1D9DF12402C2E9855F83C6154C2F072EEFF871ADBA3A306439BEE7A5AD4A004E`。

## 当前功能与阶段

P1.3 文件夹库初始化与检查，实施方案 v1 已实现并完成本机验证，等待用户验收。P1.2 已由用户验收。

## 需求与测试

[P1.3 需求映射](../requirements/ACCEPTANCE_CHECKLIST.md#p13-文件夹库初始化与检查)列出 REQ-P1.3-01 至 REQ-P1.3-07，关联 T01 的文件夹库部分、T11 的未来 schema 拒绝写入和 T18 的 C++/Python 契约。另见 [P1.3 学习文档](../development/P1.3_library_foundation.md)和 [P1.3 验证记录](../testing/P1.3_library_foundation.md)。

[P1.2 需求映射](../requirements/ACCEPTANCE_CHECKLIST.md#p12-c-sqlite-基础)包含 REQ-P1.2-01 至 REQ-P1.2-06 和对应测试。另见 [P1.2 学习文档](../development/P1.2_sqlite_foundation.md)、[P1.2 验证记录](../testing/P1.2_sqlite_foundation.md)和[决策记录](DECISIONS.md)。前置功能见 [P1.1 需求映射](../requirements/ACCEPTANCE_CHECKLIST.md#p11-可重复构建与最小绑定)及 [P1.1 验证记录](../testing/P1.1_build_and_abi.md)。

## P1.3 实施方案 v1

- **依据与前置。** V4.0 §2.1–2.4、§3.1–3.4、§6.2、§8.3、§9.1、§10.1 和 §10.2。沿用 P1.1 的 Windows x64、MSVC v143、C++17 和 pybind11，以及 P1.2 的 SQLite RAII、事务和迁移执行器。通过 §10.3 和 §10.5 核对与登记、独占协调及升级的交付边界。
- **创建与格式。** `Library.create(parent, name) -> LibraryInfo` 使用 C++ 初始化 `library.json`、`library.db` 和 `assets/`。清单为 UTF-8 JSON，固定魔数 `MarkdownShelfLibrary`，拟定 `format_version=1`、初始 schema `user_version=1` 与 `application_id=0x4D53484C`（MSHL）。清单与 DB 共用同一 UUID 和创建时刻，分别使用带毫秒及 `Z` 的 UTC 文本和 UTC epoch 毫秒。显示名存于 `library_meta`。`0001_initial.sql` 建立规格所列的单行元数据，`content_epoch` 和 `search_index_version` 初始为 0，活动 FTS 槽初始为空。业务表随所属功能增加迁移。清单拟用现有 SQLite 的 JSON 函数在隔离内存连接中解析和生成，校验 UTF-8、字段类型与重复必填键，复用已锁定的构建依赖。
- **路径与发布。** 在实际父目录下创建同卷唯一暂存目录，完成数据库迁移和结构复查，关闭文件和 SQLite 句柄后整体发布。校验 Windows 名称和权限，使用最终句柄路径和物理身份复核父目录、库根与受管条目，覆盖中文、空格、长路径及联接/符号链接的处理。目标非空则报错。已有目标经复核为空后按原身份暂移，再以禁止覆盖的重命名发布。遇到占用或竞争时还原原目录，若原路径已被占据则保留原目录并报告位置。仅递归清理归属可核实的本次暂存目录，原空目录成功后的回收仍使用仅空目录可删除的操作。
- **检查。** `Library.inspect(root) -> LibraryInspection` 只读核验根目录、清单、SQLite `application_id`、`user_version`、单行元数据、UUID、创建时刻和 `assets/`。数据库、清单和资源内容在检查期间保持原样。SQLite 在 WAL 模式下可能处理 `-wal`/`-shm` 辅助文件，验证记录单列其行为。将元信息检查与 `quick_check(1)` 在空库和可重建的合成库上分别计时，计划用前者做 `inspect`，用后者做 `open` 启用写入前的额外检查，并记录测量结果。较新 schema 返回 `SchemaTooNew`，格式不支持、损坏、占用和权限分别提供可定位的错误。
- **开库、锁与线程。** `Library.open(root, lock) -> LibrarySession` 接受由核心取得的真实 `LibraryLease`。拟提供 `LibraryLease.acquire(root)`，使用库父目录的稳定文件句柄取得最小同路径独占，绑定库 UUID 和物理目录身份。开库时消费租约并复核身份、格式、结构及可写性，之后启用 WAL 与 `synchronous=FULL`。会话持有连接和租约，`close()` 幂等，先释放连接再释放租约。阻塞的纯 C++ 调用释放 GIL，连接按线程归属使用且同一会话不并发访问。P2.1 负责登记和迁移路径，P2.2 负责完整的按 UUID 协调、稳定路径锁的应用层接入、IPC 与同库激活，P4.9 负责保护快照和自动升级。P1.3 的真实租约为后续复用的最小实现。
- **接口和文件。** 新增 `src/core/include/markdownshelf/library.h`、`src/core/domain/` 和 `src/core/platform/` 的库服务与 Windows 路径、发布和锁实现、`src/core/migrations/0001_initial.sql`。接入 `CMakeLists.txt` 和 `src/bindings/`，构建时将初始 SQL 嵌入核心，暂存扩展可独立运行。C++ 持有 I/O 权限，Python 路径使用 `pathlib.Path`。值类型返回规范化根路径、UUID、显示名、创建时刻及格式和 schema 版本等信息。结构化错误包含 `code`、消息键、可重试性和操作 ID。为 `tests/cpp/`、`tests/python/` 和 `tests/contracts/` 增加库集成用例及版本化样例。
- **验收与命令。** 按需求清单验证三项产物一致和重新打开、复制后身份保持、权限拒绝及格式错误、未来 schema 原样保留、非空及并发目标保护、发布失败回滚和租约释放。用 `MARKDOWNSHELF_STRICT=ON` 和现有本地依赖缓存构建 Debug、Release，分别运行 `cmake --preset win-x64-debug` 或 `win-x64-release`、相应的 `cmake --build --preset` 和 `ctest --preset`，再用 `cmake --install` 分别暂存，使用对应暂存路径导入真实 `.pyd` 并运行 `python -m pytest tests/python`。再以 `MARKDOWNSHELF_BUILD_BINDINGS=OFF` 复验独立核心并执行 `clang-format --dry-run --Werror`。记录命令、工具链、结果和需求证据到 `docs/testing/P1.3_library_foundation.md`，交付代码审查、Deep 源码讲解及 `docs/development/P1.3_library_foundation.md`。

## P1.2 已验收方案 v1

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
| P1.2 交付验收 | 已验收 | 2026-11-19，用户回复「p1.2验收完成，接下来进行p1.3」。 |
| P1.3 实施方案 v1 | 已批准 | 2026-11-20，用户回复「批准 P1.3 方案 v1」。 |
| P1.3 交付验收 | 等待用户验收 | 2026-11-20，当前会话提交本机测试、自审、源码讲解和学习文档。 |

## 已完成内容

P1.1 和 P1.2 已验收。P1.2 已实现 SQLite 3.54.1 静态编入核心、连接与语句 RAII、事务提交与回滚、逐步版本迁移及 FTS5 tokenizer 注册入口。已完成源码自审、学习文档和测试证据关联。

P1.3 已实现文件夹库创建、检查、打开与关闭。复用 SQLite 迁移和连接，增加 Windows 路径身份校验、暂存发布与回滚、真实文件租约、Python 值类型和结构化错误。已完成代码自审与需求证据关联。

## 已执行测试

MSVC v143 下，Debug 和 Release 的默认构建与本地缓存干净构建各通过 16 项 CTest、8 项 pytest 和暂存导入，关闭绑定的独立核心通过 16 项 CTest。严格编译、格式检查、错误归档拒绝、静态链接和 clangd 前台 LSP 验证均有记录。[P1.2 验证记录](../testing/P1.2_sqlite_foundation.md)

P1.3 在 Debug 和 Release 各通过 25 项 CTest、39 项 pytest 和真实暂存扩展导入。关闭绑定的独立核心通过 25 项 CTest。三组均通过严格编译，格式检查通过。实测 DACL 权限、长路径与目录联接，验证两个进程的租约竞争、WAL 辅助文件行为和发布失败回滚。[P1.3 验证记录](../testing/P1.3_library_foundation.md)

2026-11-25 修正托管工作流 #3 暴露的两个 C++ 路径断言。Debug、Release 和独立核心在正常路径与大小写别名下均各通过 25 项 CTest。Debug 和 Release 的暂存扩展导入及各 39 项 pytest 均通过。[路径断言复测](../testing/P1.3_library_foundation.md#windows-路径断言复测)

## 当前待处理事项与下一步

等待用户确认 P1.3 验收。路径断言修正后的托管 GitHub Actions 待运行，其他未运行项见 P1.3 验证记录。
