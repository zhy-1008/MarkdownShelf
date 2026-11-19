#include "markdownshelf/library.h"

#include "../db/sqlite_internal.h"
#include "../platform/library_files_win.h"
#include "initial_sql.h"
#include "library_internal.h"
#include "library_manifest.h"
#include "markdownshelf/migrations.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <utility>

namespace markdownshelf {
namespace {

using Code = LibraryErrorCode;
namespace fs = std::filesystem;

// 保存第一版 schema 的版本号。
constexpr int current_schema = 1;
// 保存写入 SQLite 文件头的应用标识 MSHL。
constexpr std::int64_t application_id = 0x4D53484C;

// 生成错误关联 ID，系统无法分配 GUID 时使用进程内的递增序号。
std::string error_operation_id() {
    auto uuid = platform::new_uuid();
    if (!uuid.empty()) {
        return uuid;
    }
    static std::atomic<std::uint64_t> fallback{0};
    return std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" +
           std::to_string(fallback.fetch_add(1, std::memory_order_relaxed));
}

// 翻译 SQLite 主结果码，保留占用、磁盘失败和数据损坏的语义。
[[noreturn]] void report_sqlite_error(const db::SqliteError &error) {
    switch (error.primary_code()) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        throw LibraryError(Code::library_busy, "library.database_busy");
    case SQLITE_READONLY:
    case SQLITE_PERM:
    case SQLITE_AUTH:
        throw LibraryError(Code::io_error, "library.permission_denied");
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_CONSTRAINT:
    case SQLITE_ERROR:
        throw LibraryError(Code::integrity_failed, "library.invalid_database");
    default:
        throw LibraryError(Code::io_error, "library.database_io");
    }
}

// 验证数据库身份字段失败时使用统一的稳定消息键。
[[noreturn]] void invalid_metadata() {
    throw LibraryError(Code::integrity_failed, "library.invalid_metadata");
}

// 读取应用标识和 schema 版本，未来 schema 在读取业务表前被拒绝。
int check_header(db::Connection &connection) {
    auto marker = connection.prepare("PRAGMA main.application_id");
    if (!marker.step() || marker.column_int64(0) != application_id) {
        throw LibraryError(Code::integrity_failed, "library.application_id_mismatch");
    }
    int version;
    try {
        version = db::schema_version(connection);
    } catch (const db::SqliteError &) {
        throw;
    } catch (const std::runtime_error &) {
        invalid_metadata();
    }
    if (version > current_schema) {
        throw LibraryError(Code::schema_too_new, "library.schema_too_new");
    }
    if (version < current_schema) {
        throw LibraryError(Code::integrity_failed, "library.schema_not_initialized");
    }
    return version;
}

// 确认 library_meta 是普通表，并读取恰好一行符合当前契约的元数据。
LibraryInfo read_metadata(db::Connection &connection, const platform::Directory &root,
                          const detail::LibraryManifest &manifest, int version) {
    auto table = connection.prepare("PRAGMA main.table_list('library_meta')");
    if (!table.step() || table.column_text(2) != "table") {
        invalid_metadata();
    }
    auto row = connection.prepare(
        "SELECT singleton, library_uuid, display_name, created_at, search_index_version, "
        "active_fts_slot, content_epoch FROM main.library_meta");
    if (!row.step()) {
        invalid_metadata();
    }
    std::string uuid;
    std::string name;
    std::int64_t created;
    try {
        uuid = detail::canonical_uuid(row.column_text(1).value_or(""));
        name = row.column_text(2).value_or("");
        created = row.column_int64(3).value_or(-1);
        const auto active_slot = row.column_text(5);
        if (row.column_int64(0) != 1 || row.column_int64(4).value_or(-1) < 0 ||
            row.column_int64(6).value_or(-1) < 0 ||
            (active_slot && *active_slot != "a" && *active_slot != "b")) {
            invalid_metadata();
        }
    } catch (const std::logic_error &) {
        invalid_metadata();
    }
    if (row.step() || !platform::valid_utf8(name)) {
        invalid_metadata();
    }
    if (uuid != manifest.library_uuid || platform::utc_text(created) != manifest.created_utc) {
        throw LibraryError(Code::integrity_failed, "library.identity_mismatch");
    }
    return {platform::display_path(root.path),
            std::move(uuid),
            std::move(name),
            manifest.created_utc,
            created,
            detail::library_format_version,
            version,
            std::nullopt};
}

// 在同一只读快照检查身份和版本，可在开库前额外走 SQLite 快速检查。
LibraryInfo check_library(const platform::Directory &root, bool quick_check) {
    const auto manifest = detail::parse_manifest(platform::read_manifest_file(root));
    const auto asset_guard = platform::open_assets(root);
    const auto database_guard = platform::guard_database(root);
    db::Connection connection(root.path / L"library.db", db::OpenMode::read_only);
    connection.execute("PRAGMA trusted_schema=OFF");
    db::Transaction snapshot(connection, db::TransactionMode::deferred);
    auto info = read_metadata(connection, root, manifest, check_header(connection));
    if (quick_check) {
        auto check = connection.prepare("PRAGMA main.quick_check(1)");
        if (!check.step() || check.column_text(0) != "ok") {
            throw LibraryError(Code::integrity_failed, "library.quick_check_failed");
        }
    }
    snapshot.commit();
    return info;
}

// 使用现有迁移执行器建立 schema，再以绑定参数保存库身份和名称。
void initialize_database(const platform::Directory &stage, std::string_view name,
                         const detail::LibraryManifest &manifest, std::int64_t created) {
    db::Connection connection(stage.path / L"library.db", db::OpenMode::read_write);
    db::migrate_schema(connection, {{current_schema, db::initial_sql}});
    db::Transaction transaction(connection);
    // 应用标识和身份行一并提交，SQL 只拼接编译期固定的整数。
    connection.execute("PRAGMA main.application_id = " + std::to_string(application_id));
    {
        auto insert = connection.prepare(
            "INSERT INTO library_meta(singleton, library_uuid, display_name, created_at) "
            "VALUES(1, ?, ?, ?)");
        insert.bind_text(1, manifest.library_uuid);
        insert.bind_text(2, name);
        insert.bind_int64(3, created);
        insert.step();
    }
    transaction.commit();
}

} // namespace

// 将枚举映射为跨语言协议约定的字符串。
std::string_view library_error_code_name(LibraryErrorCode code) noexcept {
    switch (code) {
    case Code::invalid_input:
        return "InvalidInput";
    case Code::not_found:
        return "NotFound";
    case Code::library_busy:
        return "LibraryBusy";
    case Code::unsupported_format:
        return "UnsupportedFormat";
    case Code::schema_too_new:
        return "SchemaTooNew";
    case Code::integrity_failed:
        return "IntegrityFailed";
    case Code::io_error:
        return "IoError";
    }
    return "IoError";
}

// 复制稳定诊断字段，并为本次负责报告的错误生成操作 ID。
LibraryError::LibraryError(Code code, std::string_view message_key,
                           std::optional<fs::path> recovery_path)
    : std::runtime_error(std::string(message_key)), code_(code), message_key_(message_key),
      operation_id_(error_operation_id()), recovery_path_(std::move(recovery_path)) {}

// 返回不依赖翻译的错误分类。
LibraryErrorCode LibraryError::code() const noexcept {
    return code_;
}

// 返回界面可用于查表的本地化键。
const std::string &LibraryError::message_key() const noexcept {
    return message_key_;
}

// 共享冲突解除后，调用方可以重新发起操作。
bool LibraryError::retryable() const noexcept {
    return code_ == Code::library_busy;
}

// 返回发生失败的操作的关联标识。
const std::string &LibraryError::operation_id() const noexcept {
    return operation_id_;
}

// 返回需要在失败提示中展示的保留目录。
const std::optional<fs::path> &LibraryError::recovery_path() const noexcept {
    return recovery_path_;
}

// 持有真实路径锁、已核验的目录及其身份，移动所有权时句柄保持有效。
struct LibraryLease::State {
    // 守护稳定锁文件所在的父目录。
    platform::Directory parent;
    // 守护库根并提供物理文件 ID。
    platform::Directory root;
    // 保存创建租约时在清单和 DB 中核验的库 UUID。
    std::string uuid;
    // 独占稳定文件，状态销毁时先释放锁再释放目录守护。
    platform::Handle lock;
};

// 保存开库后的资源，按反向声明顺序关闭 DB、释放条目守护，最后释放租约。
struct LibrarySession::State {
    // 保持库独占直到所有会话资源释放。
    std::unique_ptr<LibraryLease::State> lease;
    // 阻止会话活动期间移除资源目录。
    platform::Handle assets;
    // 阻止会话活动期间替换数据库主文件。
    platform::Handle database;
    // 独占可写 SQLite 连接，其生命周期短于全部守护对象。
    std::unique_ptr<db::Connection> connection;
};

// 将已取得的租约状态移入不可复制的公开对象。
LibraryLease::LibraryLease(std::unique_ptr<State> state) : state_(std::move(state)) {}

// 使用内部状态的 RAII 清理尚未交接的租约。
LibraryLease::~LibraryLease() = default;

// 在互斥保护下查询租约能否被 open 接管。
bool LibraryLease::valid() const {
    const std::lock_guard guard(mutex_);
    return state_ != nullptr;
}

// 先检查库格式，再通过 Windows 共享模式取得稳定路径锁。
std::unique_ptr<LibraryLease> LibraryLease::acquire(const fs::path &root) try {
    auto directory = platform::resolve_directory(root);
    auto parent = platform::resolve_directory(directory.path.parent_path());
    auto info = check_library(directory, false);
    auto lock = platform::lock_library_path(directory);
    auto state = std::make_unique<State>(State{std::move(parent), std::move(directory),
                                               std::move(info.library_uuid), std::move(lock)});
    return std::unique_ptr<LibraryLease>(new LibraryLease(std::move(state)));
} catch (const db::SqliteError &error) {
    report_sqlite_error(error);
}

// 构造会话并保存一份可在 close 后继续读取的元数据。
LibrarySession::LibrarySession(LibraryInfo info, std::unique_ptr<State> state)
    : info_(std::move(info)), state_(std::move(state)) {}

// 析构按 State 的成员顺序释放资源，语句和事务已在打开调用中结束。
LibrarySession::~LibrarySession() = default;

// 返回会话持有的元数据，内容在并发关闭时也保持不变。
const LibraryInfo &LibrarySession::info() const noexcept {
    return info_;
}

// 查询连接和库租约是否仍处于会话所有。
bool LibrarySession::is_open() const {
    const std::lock_guard guard(mutex_);
    return state_ != nullptr;
}

// 串行销毁状态，其他线程观察到关闭时租约已经释放。
void LibrarySession::close() {
    const std::lock_guard guard(mutex_);
    state_.reset();
}

// 正常入口与集成测试共用同一套创建和回滚逻辑。
LibraryInfo Library::create(const fs::path &parent, std::string_view name) {
    return detail::create_library(parent, name);
}

// 守护解析后的库根并执行只读元信息检查。
LibraryInspection Library::inspect(const fs::path &root) try {
    return {check_library(platform::resolve_directory(root), false)};
} catch (const db::SqliteError &error) {
    report_sqlite_error(error);
}

// 消费租约后重新核对目录和 UUID，完成只读快速检查后才启用写连接。
std::unique_ptr<LibrarySession> Library::open(const fs::path &root, LibraryLease &lock) try {
    std::unique_ptr<LibraryLease::State> lease;
    {
        const std::lock_guard guard(lock.mutex_);
        lease = std::move(lock.state_);
    }
    if (!lease) {
        throw LibraryError(Code::invalid_input, "library.invalid_lease");
    }
    const auto requested = platform::resolve_directory(root);
    if (!platform::same_directory(requested, lease->root)) {
        throw LibraryError(Code::invalid_input, "library.lease_path_mismatch");
    }
    auto assets = platform::open_assets(lease->root);
    auto database = platform::guard_database(lease->root);
    auto info = check_library(lease->root, true);
    if (info.library_uuid != lease->uuid) {
        throw LibraryError(Code::integrity_failed, "library.lease_uuid_mismatch");
    }
    platform::require_directory_access(lease->root, FILE_ADD_FILE);
    auto connection = std::make_unique<db::Connection>(lease->root.path / L"library.db",
                                                       db::OpenMode::read_write);
    connection->execute("PRAGMA trusted_schema=OFF");
    try {
        connection->configure_wal();
    } catch (const db::SqliteError &) {
        throw;
    } catch (const std::runtime_error &) {
        throw LibraryError(Code::io_error, "library.wal_unavailable");
    }
    auto state = std::make_unique<LibrarySession::State>(LibrarySession::State{
        std::move(lease), std::move(assets), std::move(database), std::move(connection)});
    return std::unique_ptr<LibrarySession>(new LibrarySession(std::move(info), std::move(state)));
} catch (const db::SqliteError &error) {
    report_sqlite_error(error);
}

namespace detail {

// 在父目录同卷建立完整库，处理旧空目录的暂移、原子发布和异常回滚。
LibraryInfo create_library(const fs::path &parent, std::string_view name,
                           const std::function<void(CreateStep, const fs::path &)> &hook) try {
    const auto basename = platform::directory_name(name);
    const auto directory = platform::resolve_directory(parent);
    const auto target = directory.path / basename;
    platform::check_create_target(target);
    platform::require_directory_access(directory, FILE_ADD_SUBDIRECTORY);
    const auto created = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    const LibraryManifest manifest{platform::new_uuid(), platform::utc_text(created)};
    if (manifest.library_uuid.empty() || manifest.created_utc.empty()) {
        throw LibraryError(Code::io_error, "library.create_identity");
    }
    platform::StagedDirectory stage(directory, manifest.library_uuid);
    try {
        if (hook) {
            hook(CreateStep::before_initialize, stage.root().path);
        }
        // 同时守护三个子项，在目录发布前释放供复查的句柄。
        auto info = [&] {
            const auto assets = stage.create_assets();
            const auto database = stage.create_database();
            initialize_database(stage.root(), name, manifest, created);
            const auto manifest_file = stage.write_manifest(encode_manifest(manifest));
            return check_library(stage.root(), true);
        }();
        info.root = platform::display_path(target);
        stage.prepare_target(target, manifest.library_uuid);
        if (hook) {
            hook(CreateStep::before_publish, stage.root().path);
        }
        info.retained_directory = stage.publish(target);
        return info;
    } catch (...) {
        stage.restore_target(target);
        if (!stage.discard()) {
            throw LibraryError(Code::io_error, "library.cleanup_stage_failed",
                               platform::display_path(stage.root().path));
        }
        throw;
    }
} catch (const db::SqliteError &error) {
    report_sqlite_error(error);
}

} // namespace detail
} // namespace markdownshelf
