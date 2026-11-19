#include "markdownshelf/sqlite.h"

#include "sqlite_internal.h"

#include <limits>

namespace markdownshelf::db {
namespace {

// 检查传给 SQLite 的 SQL 文本。空输入和内嵌 NUL 会抛出 std::invalid_argument。
void validate_sql(std::string_view sql) {
    if (sql.empty() || sql.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SQL must be nonempty and contain no NUL bytes");
    }
}

// 将项目的文件打开模式映射为 sqlite3_open_v2 所需的标志位。
int open_flags(OpenMode mode) {
    switch (mode) {
    case OpenMode::read_only:
        return SQLITE_OPEN_READONLY;
    case OpenMode::read_write:
        return SQLITE_OPEN_READWRITE;
    case OpenMode::read_write_create:
        return SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    }
    throw std::invalid_argument("Invalid database open mode");
}

// 为延迟事务或立即事务选择固定的 BEGIN 语句，拒绝未知模式。
const char *begin_sql(TransactionMode mode) {
    switch (mode) {
    case TransactionMode::deferred:
        return "BEGIN DEFERRED";
    case TransactionMode::immediate:
        return "BEGIN IMMEDIATE";
    }
    throw std::invalid_argument("Invalid transaction mode");
}

} // namespace

// 将操作名和 SQLite 对结果码的描述组合成异常消息，同时保存数值结果码。
SqliteError::SqliteError(std::string_view operation, int extended_code)
    : std::runtime_error(std::string(operation) + ": " + sqlite3_errstr(extended_code)),
      extended_code_(extended_code) {}

// 屏蔽扩展信息，令同一类约束错误具有相同的主结果码。
int SqliteError::primary_code() const noexcept {
    return extended_code_ & 0xff;
}

// 直接返回异常构造时保存的结果码，保留具体失败原因。
int SqliteError::extended_code() const noexcept {
    return extended_code_;
}

// 通过原生关闭接口释放独占指针接管的连接。
void Connection::Close::operator()(sqlite3 *handle) const noexcept {
    sqlite3_close_v2(handle);
}

// 以 UTF-8 路径打开文件。先接管 SQLite 返回的句柄，再检查结果并配置连接。
Connection::Connection(const std::filesystem::path &path, OpenMode mode, int busy_timeout_ms) {
    const auto filename = path.u8string();
    if (filename.empty() || filename.find('\0') != std::string::npos || filename == ":memory:") {
        throw std::invalid_argument("A valid database file path is required");
    }
    if (busy_timeout_ms < 0) {
        throw std::invalid_argument("SQLite busy timeout must be nonnegative");
    }

    sqlite3 *opened = nullptr;
    const int result = sqlite3_open_v2(filename.c_str(), &opened,
                                       open_flags(mode) | SQLITE_OPEN_EXRESCODE, nullptr);
    handle_.reset(opened);
    check_sqlite(result, "Open database");
    if (mode != OpenMode::read_only && sqlite3_db_readonly(handle(), "main") != 0) {
        throw SqliteError("Open writable database", SQLITE_READONLY);
    }
    check_sqlite(sqlite3_busy_timeout(handle(), busy_timeout_ms), "Set busy timeout");
    execute("PRAGMA foreign_keys = ON");
}

// 集中拦截上次回滚后仍不可用的连接，并向内部调用方借出原生句柄。
sqlite3 *Connection::handle() const {
    if (!usable_) {
        throw std::logic_error("Database connection could not recover from a rollback failure");
    }
    return handle_.get();
}

// 用以 NUL 结尾的 SQL 文本调用 sqlite3_exec，由统一错误入口处理执行失败。
void Connection::execute(std::string_view sql) {
    validate_sql(sql);
    check_sqlite(sqlite3_exec(handle(), std::string(sql).c_str(), nullptr, nullptr, nullptr),
                 "Execute SQL");
}

// 在当前连接上调用私有构造函数，直接构造返回的语句对象。
Statement Connection::prepare(std::string_view sql) {
    return Statement(*this, sql);
}

// 先设置同步级别，再启用并核对 SQLite 实际返回的 WAL 模式。
void Connection::configure_wal() {
    auto *db = handle();
    if (transaction_active_ || !sqlite3_get_autocommit(db)) {
        throw std::logic_error("Configure WAL before starting a transaction");
    }
    if (sqlite3_db_readonly(db, "main") != 0) {
        throw SqliteError("Configure WAL", SQLITE_READONLY);
    }

    execute("PRAGMA main.synchronous = FULL");
    auto mode = prepare("PRAGMA main.journal_mode = WAL");
    if (!mode.step() || mode.column_text(0) != "wal") {
        throw std::runtime_error("SQLite did not enable WAL");
    }
    mode.step();
}

// 在独占指针清理时终结语句并释放 SQLite 分配的资源。
void Statement::Finalize::operator()(sqlite3_stmt *handle) const noexcept {
    sqlite3_finalize(handle);
}

// 按明确长度编译 SQL。接管句柄后检查末尾，确保调用只传入了一条语句。
Statement::Statement(Connection &connection, std::string_view sql) : connection_(connection) {
    validate_sql(sql);
    if (sql.size() > (std::numeric_limits<int>::max)()) {
        throw std::length_error("SQL statement exceeds SQLite's input size");
    }

    sqlite3_stmt *prepared = nullptr;
    const char *tail = nullptr;
    const int result = sqlite3_prepare_v2(connection_.handle(), sql.data(),
                                          static_cast<int>(sql.size()), &prepared, &tail);
    handle_.reset(prepared);
    check_sqlite(result, "Prepare SQL");
    if (!handle_ ||
        std::string_view(tail, sql.data() + sql.size() - tail).find_first_not_of(" \t\r\n\f") !=
            std::string_view::npos) {
        throw std::invalid_argument("Prepare expects one SQL statement");
    }
}

// 绑定前检查连接和执行状态，已开始执行的语句必须先重置。
void Statement::check_bind() const {
    connection_.handle();
    if (state_ != State::ready) {
        throw std::logic_error("Reset the statement before binding parameters");
    }
}

// 检查绑定时机，再用 SQLite 的 64 位接口绑定整数，避免缩窄数值。
void Statement::bind_int64(int parameter, std::int64_t value) {
    check_bind();
    check_sqlite(sqlite3_bind_int64(handle_.get(), parameter, value), "Bind integer");
}

// 传入文本的实际字节长度并让 SQLite 复制缓冲区，空文本使用有效指针。
void Statement::bind_text(int parameter, std::string_view utf8) {
    check_bind();
    const char *text = utf8.empty() ? "" : utf8.data();
    check_sqlite(sqlite3_bind_text64(handle_.get(), parameter, text, utf8.size(), SQLITE_TRANSIENT,
                                     SQLITE_UTF8),
                 "Bind text");
}

// 使用 SQLite 专用接口绑定 SQL NULL，使其与空文本明确区分。
void Statement::bind_null(int parameter) {
    check_bind();
    check_sqlite(sqlite3_bind_null(handle_.get(), parameter), "Bind null");
}

// 前进一步并记录行或结束状态，防止 SQLite 在执行结束后隐式重跑语句。
bool Statement::step() {
    connection_.handle();
    if (state_ == State::finished) {
        throw std::logic_error("Reset the statement before executing it again");
    }
    const int result = sqlite3_step(handle_.get());
    state_ = result == SQLITE_ROW ? State::row : State::finished;
    if (result == SQLITE_ROW) {
        return true;
    }
    if (result == SQLITE_DONE) {
        return false;
    }
    throw SqliteError("Step SQL", result);
}

// 复位语句并清除参数，状态恢复后再报告 SQLite 返回的重置或清理错误。
void Statement::reset() {
    connection_.handle();
    const int result = sqlite3_reset(handle_.get());
    const int clear_result = sqlite3_clear_bindings(handle_.get());
    state_ = State::ready;
    check_sqlite(result, "Reset SQL");
    check_sqlite(clear_result, "Clear SQL parameters");
}

// 先验证结果行存在及列索引有效，再查询 SQLite 返回值的存储类型。
int Statement::column_type(int column) const {
    connection_.handle();
    if (state_ != State::row) {
        throw std::logic_error("Step to a result row before reading columns");
    }
    if (column < 0 || column >= sqlite3_column_count(handle_.get())) {
        throw std::out_of_range("Invalid result column");
    }
    return sqlite3_column_type(handle_.get(), column);
}

// 逐列核对整数或 NULL 类型，再按原始 64 位宽度读取。
std::optional<std::int64_t> Statement::column_int64(int column) const {
    const int type = column_type(column);
    if (type == SQLITE_NULL) {
        return std::nullopt;
    }
    if (type != SQLITE_INTEGER) {
        throw std::logic_error("Result column is not an integer");
    }
    return sqlite3_column_int64(handle_.get(), column);
}

// 逐列核对文本或 NULL 类型，再按字节长度复制文本，使返回值独立于当前行。
std::optional<std::string> Statement::column_text(int column) const {
    const int type = column_type(column);
    if (type == SQLITE_NULL) {
        return std::nullopt;
    }
    if (type != SQLITE_TEXT) {
        throw std::logic_error("Result column is not text");
    }
    const auto *text = sqlite3_column_text(handle_.get(), column);
    if (!text) {
        throw SqliteError("Read text", SQLITE_NOMEM);
    }
    return std::string(reinterpret_cast<const char *>(text),
                       sqlite3_column_bytes(handle_.get(), column));
}

// 检查当前连接是否已有事务对象，在 BEGIN 成功后登记事务归属。
Transaction::Transaction(Connection &connection, TransactionMode mode) : connection_(connection) {
    if (connection_.transaction_active_) {
        throw std::logic_error("A transaction already owns this connection");
    }
    connection_.execute(begin_sql(mode));
    connection_.transaction_active_ = true;
    active_ = true;
}

// 统一由回滚路径处理作用域退出，已提交的事务会直接返回。
Transaction::~Transaction() noexcept {
    rollback();
}

// 直接执行 COMMIT。失败时尝试回滚，并用原始提交结果码报告失败。
void Transaction::commit() {
    if (!active_) {
        throw std::logic_error("Transaction is no longer active");
    }
    const int result = sqlite3_exec(connection_.handle(), "COMMIT", nullptr, nullptr, nullptr);
    if (result != SQLITE_OK) {
        rollback();
        throw SqliteError("Commit transaction", result);
    }
    connection_.transaction_active_ = false;
    active_ = false;
}

// 用 autocommit 区分 SQLite 已自动回滚与仍需回滚两种路径，再释放事务归属。
void Transaction::rollback() noexcept {
    if (!active_) {
        return;
    }
    auto *db = connection_.handle_.get();
    if (!sqlite3_get_autocommit(db)) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        connection_.usable_ = sqlite3_get_autocommit(db) != 0;
    }
    connection_.transaction_active_ = false;
    active_ = false;
}

} // namespace markdownshelf::db
