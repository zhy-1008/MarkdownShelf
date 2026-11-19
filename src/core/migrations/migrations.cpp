#include "markdownshelf/migrations.h"

#include "../db/sqlite_internal.h"

#include <limits>

namespace markdownshelf::db {
namespace {

// 按 SQLite 的 action 审核迁移 SQL，阻止脚本控制事务、附加库或自行写入 PRAGMA。
int authorize_migration(void *, int action, const char *name, const char *argument, const char *,
                        const char *) noexcept {
    switch (action) {
    case SQLITE_TRANSACTION:
    case SQLITE_SAVEPOINT:
    case SQLITE_ATTACH:
    case SQLITE_DETACH:
        return SQLITE_DENY;
    case SQLITE_PRAGMA:
        // FTS5 写入自身的表时会读取 data_version。
        return !argument && name && sqlite3_stricmp(name, "data_version") == 0 ? SQLITE_OK
                                                                               : SQLITE_DENY;
    default:
        return SQLITE_OK;
    }
}

// 将 SQLite 授权回调限制在执行迁移脚本和数据转换的作用域内。
class MigrationAuthorizer {
  public:
    // 借用连接并安装授权回调。SQLite 拒绝安装时抛出异常。
    explicit MigrationAuthorizer(Connection &connection)
        : handle_(SqliteAccess::handle(connection)) {
        check_sqlite(sqlite3_set_authorizer(handle_, authorize_migration, nullptr),
                     "Install migration authorizer");
    }

    // 清除临时授权回调，让迁移执行入口继续检查版本并提交或回滚。
    ~MigrationAuthorizer() noexcept {
        sqlite3_set_authorizer(handle_, nullptr, nullptr);
    }

    // 禁止复制构造，使授权回调由单个作用域负责清除。
    MigrationAuthorizer(const MigrationAuthorizer &) = delete;
    // 禁止复制赋值，保持授权回调与原连接的关联。
    MigrationAuthorizer &operator=(const MigrationAuthorizer &) = delete;

  private:
    // 借用已注册回调的原生连接，用于析构时移除回调。
    sqlite3 *handle_;
};

// 校验步骤从 1 起连续且每步有迁移内容，返回目标版本。空序列的目标为 0。
int target_version(const std::vector<Migration> &migrations) {
    for (std::size_t index = 0; index < migrations.size(); ++index) {
        const auto &migration = migrations[index];
        if (migration.version <= 0 || static_cast<std::size_t>(migration.version) != index + 1) {
            throw std::invalid_argument("Migration versions must be consecutive starting at 1");
        }
        if (migration.sql.empty() && !migration.transform) {
            throw std::invalid_argument("A migration must provide SQL or a data transform");
        }
    }
    return migrations.empty() ? 0 : migrations.back().version;
}

// 在调用方事务内执行 SQL 和数据转换，通过外键检查后更新 user_version。
void apply_step(Connection &connection, const Migration &migration) {
    {
        MigrationAuthorizer authorizer(connection);
        if (!migration.sql.empty()) {
            connection.execute(migration.sql);
        }
        if (migration.transform) {
            migration.transform(connection);
        }
    }
    {
        auto violations = connection.prepare("PRAGMA main.foreign_key_check");
        if (violations.step()) {
            throw SqliteError("Validate migration foreign keys", SQLITE_CONSTRAINT_FOREIGNKEY);
        }
    }
    connection.execute("PRAGMA main.user_version = " + std::to_string(migration.version));
}

// 读取当前版本，超过 target 时抛出 SchemaTooNew 以阻止写入。
int supported_schema_version(Connection &connection, int target) {
    const int current = schema_version(connection);
    if (current > target) {
        throw SchemaTooNew(current, target);
    }
    return current;
}

} // namespace

// 把实际版本和支持上限写入异常消息，便于定位版本不兼容。
SchemaTooNew::SchemaTooNew(int actual_version, int supported_version)
    : std::runtime_error("Database schema " + std::to_string(actual_version) +
                         " exceeds supported " + std::to_string(supported_version)) {}

// 从主库 PRAGMA 读取整数结果，并检查其在非负 int 范围内。
int schema_version(Connection &connection) {
    auto version = connection.prepare("PRAGMA main.user_version");
    if (!version.step()) {
        throw std::runtime_error("SQLite did not return a schema version");
    }
    const auto value = version.column_int64(0);
    if (!value || *value < 0 || *value > (std::numeric_limits<int>::max)()) {
        throw std::runtime_error("Invalid SQLite schema version");
    }
    return static_cast<int>(*value);
}

// 逐步加锁、重读版本并提交。已应用的步骤被跳过，失败由当前事务回滚。
void migrate_schema(Connection &connection, const std::vector<Migration> &migrations) {
    const int target = target_version(migrations);
    int current = supported_schema_version(connection, target);
    while (current < target) {
        Transaction transaction(connection);
        // 取得写锁后重读版本，再确定要执行的迁移步骤。
        current = supported_schema_version(connection, target);
        if (current == target) {
            return;
        }
        const auto &migration = migrations.at(static_cast<std::size_t>(current));
        apply_step(connection, migration);
        transaction.commit();
        current = migration.version;
    }
}

} // namespace markdownshelf::db
