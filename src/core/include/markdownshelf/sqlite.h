#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// SQLite 的不透明连接句柄类型。
struct sqlite3;
// SQLite 的不透明预编译语句句柄类型。
struct sqlite3_stmt;

namespace markdownshelf::db {

// 封装数据库操作失败的诊断信息和 SQLite 结果码，供调用方判断失败原因。
class SqliteError : public std::runtime_error {
  public:
    // operation 描述失败的操作，extended_code 保留 SQLite 返回的完整结果码。
    SqliteError(std::string_view operation, int extended_code);
    // 返回结果码的低 8 位，用于判断 SQLITE_BUSY 等错误类别。
    int primary_code() const noexcept;
    // 返回完整结果码，用于区分唯一约束和外键约束等具体原因。
    int extended_code() const noexcept;

  private:
    // 保存原始扩展结果码，供两种查询接口读取。
    int extended_code_;
};

// 控制文件数据库的访问权限和是否允许创建文件。
enum class OpenMode {
    // 只读打开已有数据库。
    read_only,
    // 读写打开已有数据库，文件必须存在。
    read_write,
    // 读写打开数据库，允许在文件缺失时创建。
    read_write_create
};
// 决定开始事务时是否立即取得写锁。
enum class TransactionMode {
    // 首次访问时开始读或写事务，可用于持有只读快照。
    deferred,
    // 开始事务时取得写锁，若已有写入者则按连接的等待设置处理。
    immediate
};

// Connection::prepare() 返回的预编译语句类型。
class Statement;

// 独占一个 SQLite 文件数据库连接，并提供 SQL 执行入口和连接设置。
// 连接及其借出的语句和事务均由同一线程使用，连接最后销毁。
class Connection {
  public:
    // 按 path 和 mode 打开文件并启用外键。锁等待默认 1000 毫秒，传 0 表示立即返回。
    // 参数无效或打开失败时抛出异常。
    Connection(const std::filesystem::path &path, OpenMode mode, int busy_timeout_ms = 1000);
    // 禁止复制构造，保证连接句柄只有一个所有者。
    Connection(const Connection &) = delete;
    // 禁止复制赋值，避免覆盖已被语句或事务借用的连接。
    Connection &operator=(const Connection &) = delete;

    // 执行服务提供的一条或多条固定 SQL 并丢弃结果行，SQLite 失败时抛出 SqliteError。
    // 由 prepare() 绑定运行时参数，由 Transaction 控制事务。
    void execute(std::string_view sql);
    // 编译单条 SQL 并返回拥有语句句柄的对象。末尾允许空白，返回对象借用当前连接。
    Statement prepare(std::string_view sql);

    // 在事务之外启用 WAL 和 synchronous=FULL。调用方须先核验库身份、版本和可写性。
    void configure_wal();

  private:
    // 允许语句检查连接状态并使用原生句柄。
    friend class Statement;
    // 允许事务管理此连接的事务归属和回滚后的可用状态。
    friend class Transaction;
    // 允许核心内部的迁移和 FTS5 适配代码访问原生句柄。
    friend struct SqliteAccess;

    // 按已校验的 UTF-8 文件名初始化连接。内部调用方可指定 VFS 或使用内存库。
    Connection(const std::string &filename, OpenMode mode, int busy_timeout_ms, const char *vfs);

    // 连接句柄的 RAII 删除器，负责在独占指针销毁时调用 SQLite 关闭接口。
    struct Close {
        // 关闭 handle 指向的连接，不从清理路径抛出异常。
        void operator()(sqlite3 *handle) const noexcept;
    };

    // 返回可用连接的借用句柄，回滚异常使连接失效时抛出 std::logic_error。
    sqlite3 *handle() const;

    // 独占原生连接句柄，包括打开失败时 SQLite 可能返回的待关闭句柄。
    std::unique_ptr<sqlite3, Close> handle_;
    // 标记连接是否由 Transaction 对象持有，防止嵌套事务。
    bool transaction_active_ = false;
    // 回滚后仍有原生事务时置为 false，阻止后续操作复用该连接。
    bool usable_ = true;
};

// 管理一条预编译 SQL 的参数绑定、逐行读取和重置，销毁时释放语句句柄。
// 借用的连接须在语句销毁后再关闭。
class Statement {
  public:
    // 禁止复制构造，确保预编译语句句柄只由一个对象释放。
    Statement(const Statement &) = delete;
    // 禁止复制赋值，保持原有语句与其连接的关联。
    Statement &operator=(const Statement &) = delete;

    // 将有符号 64 位整数绑定到从 1 开始的参数位置，索引无效时抛出 SqliteError。
    void bind_int64(int parameter, std::int64_t value);
    // 按字节长度绑定并复制 UTF-8 文本。parameter 从 1 开始，支持空文本及内嵌 NUL。
    void bind_text(int parameter, std::string_view utf8);
    // 将指定参数绑定为 SQL NULL。parameter 从 1 开始。
    void bind_null(int parameter);

    // 读到一行时返回 true，执行结束时返回 false。带返回行的写语句须执行至结束。
    // 执行失败抛出 SqliteError。结束或失败后需先 reset() 才能再次执行。
    bool step();
    // 释放当前结果并清除所有绑定，使语句可以复用。此操作可能报告 SQLite 执行错误。
    void reset();

    // 读取当前行的整数列，列索引从 0 开始。NULL 返回 std::nullopt，类型不符时抛出异常。
    std::optional<std::int64_t> column_int64(int column) const;
    // 复制当前行的 UTF-8 文本列，列索引从 0 开始。NULL 返回 std::nullopt，空文本返回空字符串。
    std::optional<std::string> column_text(int column) const;

  private:
    // 允许 Connection::prepare() 使用私有构造函数创建语句。
    friend class Connection;
    // 允许内部 FTS5 适配代码把原生指针绑定到预编译语句。
    friend struct SqliteAccess;

    // 约束绑定、执行和读取结果的顺序。
    enum class State {
        // 尚未执行或已经重置，可以绑定参数并开始执行。
        ready,
        // 当前有一行结果，可以读取列或继续执行。
        row,
        // 执行已完成或失败，复用前必须重置。
        finished
    };

    // 语句句柄的 RAII 删除器，负责释放 SQLite 预编译语句。
    struct Finalize {
        // 释放 handle 指向的语句及其占用的资源，清理时不抛出异常。
        void operator()(sqlite3_stmt *handle) const noexcept;
    };

    // 借用 connection 并编译单条 SQL，输入或 SQLite 编译错误通过异常报告。
    Statement(Connection &connection, std::string_view sql);
    // 检查连接可用且语句处于 ready 状态，避免在执行后直接改绑参数。
    void check_bind() const;
    // 校验当前行和列索引，再返回该列的 SQLite 存储类型。
    int column_type(int column) const;

    // 借用创建语句的连接，用于检查连接可用性。连接必须比语句存活更久。
    Connection &connection_;
    // 独占 SQLite 预编译语句句柄，并负责最终释放。
    std::unique_ptr<sqlite3_stmt, Finalize> handle_;
    // 记录语句执行进度，控制绑定、读取列和重置的调用顺序。
    State state_ = State::ready;
};

// 通过 RAII 管理连接上的单个事务。显式提交，提前退出时自动清理。
// 连接的生命周期必须覆盖事务，提交前销毁仍在使用的语句。
class Transaction {
  public:
    // 在 connection 上按 mode 开启事务，默认立即取得写锁。已有事务或开启失败时抛出异常。
    explicit Transaction(Connection &connection, TransactionMode mode = TransactionMode::immediate);
    // 退出作用域时回滚尚未结束的事务。清理过程不抛出异常。
    ~Transaction() noexcept;
    // 禁止复制构造，避免两个对象同时管理同一个事务。
    Transaction(const Transaction &) = delete;
    // 禁止复制赋值，确保事务始终关联其打开时借用的连接。
    Transaction &operator=(const Transaction &) = delete;

    // 提交事务并解除对连接的占用。SQLite 拒绝提交时先尝试回滚，再用原始结果码抛出异常。
    void commit();

  private:
    // 清理仍活跃的事务，必要时执行回滚并在无法恢复时将连接标记为不可用。
    void rollback() noexcept;

    // 借用执行 BEGIN、COMMIT 和 ROLLBACK 的连接。
    Connection &connection_;
    // 标记当前对象是否仍负责收尾，提交或回滚完成后置为 false。
    bool active_ = false;
};

} // namespace markdownshelf::db
