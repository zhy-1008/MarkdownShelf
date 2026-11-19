#include "markdownshelf/fts5.h"
#include "markdownshelf/migrations.h"
#include "markdownshelf/sqlite.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <limits>
#include <new>
#include <random>
#include <utility>

#if defined(_MSC_VER) && !defined(__clang__)
// MSVC 会对 GoogleTest 断言宏中的常量 switch 报告 C6326。
#pragma warning(push)
#pragma warning(disable : 6326)
#endif

namespace {

using markdownshelf::db::Connection;
using markdownshelf::db::Migration;
using markdownshelf::db::OpenMode;
using markdownshelf::db::SqliteError;
using markdownshelf::db::Transaction;
using markdownshelf::db::TransactionMode;

// 为每个 SQLite 用例管理独立的临时库目录，使测试的文件和事务彼此隔离。
class SqliteTest : public testing::Test {
  protected:
    // 创建唯一临时目录并生成含中文和空格的库路径，重复冲突时报告测试失败。
    void SetUp() override {
        const auto parent = std::filesystem::temp_directory_path();
        std::random_device random;
        for (int attempt = 0; attempt < 16; ++attempt) {
            auto candidate = parent / ("markdownshelf-sqlite-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                directory_ = std::move(candidate);
                path_ = directory_ / std::filesystem::u8path(u8"技术 笔记.db");
                return;
            }
        }
        FAIL() << "Could not create a unique database test directory";
    }

    // 递归清理本用例创建的目录，句柄未释放等清理错误会使测试失败。
    void TearDown() override {
        if (!directory_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(directory_, error);
            EXPECT_FALSE(error) << error.message();
        }
    }

    // 各用例用于打开、重开和检查数据库文件的路径。
    std::filesystem::path path_;

  private:
    // 记录本用例实际创建的目录，作为清理范围。
    std::filesystem::path directory_;
};

// 执行标量查询并读取首行首列的整数。缺行、NULL 或类型不符均报告异常。
std::int64_t read_int(Connection &connection, std::string_view sql) {
    auto query = connection.prepare(sql);
    if (!query.step()) {
        throw std::runtime_error("Expected an integer result row");
    }
    return query.column_int64(0).value();
}

// 执行标量查询并复制首行首列文本，使结果在局部语句销毁后仍可使用。
std::string read_text(Connection &connection, std::string_view sql) {
    auto query = connection.prepare(sql);
    if (!query.step()) {
        throw std::runtime_error("Expected a text result row");
    }
    return query.column_text(0).value();
}

// 检查 SQLite 锁定版本和 FTS5 编译选项，并用绑定的词项查询对应行。
TEST_F(SqliteTest, BuildsLockedSqliteWithFts5AndMatchesBoundTerm) {
    Connection connection(path_, OpenMode::read_write_create);
    EXPECT_STREQ(sqlite3_libversion(), "3.54.1");
    EXPECT_EQ(read_int(connection, "SELECT sqlite_compileoption_used('ENABLE_FTS5')"), 1);
    connection.execute(
        "CREATE VIRTUAL TABLE notes USING fts5(body);"
        "INSERT INTO notes(rowid, body) VALUES (1, 'memory allocator'), (2, 'stack')");
    auto query = connection.prepare("SELECT rowid FROM notes WHERE notes MATCH ?1");
    query.bind_text(1, "allocator");
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.column_int64(0), 1);
    EXPECT_FALSE(query.step());
}

// 验证中文路径可重开，并检查 64 位整数、内嵌 NUL、空文本和 NULL 的持久化。
TEST_F(SqliteTest, OpensUnicodePathAndPreservesBoundValuesAcrossReopen) {
    const auto max_id = (std::numeric_limits<std::int64_t>::max)();
    const std::string text = std::string(u8"书架\U0001F680") + '\0' + "tail";
    {
        Connection connection(path_, OpenMode::read_write_create);
        connection.execute("CREATE TABLE notes(id INTEGER PRIMARY KEY, text TEXT, extra TEXT)");
        auto insert = connection.prepare("INSERT INTO notes VALUES (?1, ?2, ?3)");
        insert.bind_int64(1, max_id);
        insert.bind_text(2, std::string(text));
        insert.bind_null(3);
        EXPECT_FALSE(insert.step());
        EXPECT_THROW(insert.step(), std::logic_error);
        insert.reset();
        insert.bind_int64(1, 1);
        insert.bind_text(2, std::string_view{});
        EXPECT_FALSE(insert.step());
    }
    Connection connection(path_, OpenMode::read_only);
    auto query = connection.prepare("SELECT id, text, extra FROM notes ORDER BY id DESC");
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.column_int64(0), max_id);
    EXPECT_EQ(query.column_text(1), text);
    EXPECT_EQ(query.column_text(2), std::nullopt);
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.column_text(1), "");
    EXPECT_EQ(query.column_text(2), std::nullopt);
    EXPECT_FALSE(query.step());
}

// 检查打开模式的创建权限、只读连接的写入限制和无效 SQL 的错误路径。
TEST_F(SqliteTest, EnforcesOpenModesAndRejectsInvalidStatements) {
    EXPECT_THROW(Connection(path_, OpenMode::read_only), SqliteError);
    EXPECT_THROW(Connection(path_, OpenMode::read_write), SqliteError);
    EXPECT_THROW(Connection(path_, OpenMode::read_write_create, -1), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path_));
    {
        Connection connection(path_, OpenMode::read_write_create);
        EXPECT_THROW(connection.prepare("SELECT 1; SELECT 2"), std::invalid_argument);
        EXPECT_THROW(connection.prepare("SELECT missing"), SqliteError);
        EXPECT_THROW(connection.execute(std::string_view("SELECT 1\0", 9)), std::invalid_argument);
        EXPECT_EQ(read_int(connection, "SELECT 1;  "), 1);
    }
    Connection reader(path_, OpenMode::read_only);
    EXPECT_THROW(reader.execute("CREATE TABLE forbidden(id)"), SqliteError);
    EXPECT_THROW(reader.configure_wal(), SqliteError);
}

// 核对外键、锁等待和同步级别，并验证 WAL 在重开后仍然生效。
TEST_F(SqliteTest, AppliesConnectionSettingsAndPersistsWal) {
    {
        Connection connection(path_, OpenMode::read_write_create);
        EXPECT_EQ(read_int(connection, "PRAGMA foreign_keys"), 1);
        EXPECT_EQ(read_int(connection, "PRAGMA busy_timeout"), 1000);
        EXPECT_EQ(read_text(connection, "PRAGMA journal_mode"), "delete");
        connection.configure_wal();
        EXPECT_EQ(read_text(connection, "PRAGMA journal_mode"), "wal");
        EXPECT_EQ(read_int(connection, "PRAGMA synchronous"), 2);
        const Transaction transaction(connection);
        EXPECT_THROW(connection.configure_wal(), std::logic_error);
    }
    Connection reader(path_, OpenMode::read_only, 12);
    EXPECT_EQ(read_text(reader, "PRAGMA journal_mode"), "wal");
    EXPECT_EQ(read_int(reader, "PRAGMA foreign_keys"), 1);
    EXPECT_EQ(read_int(reader, "PRAGMA busy_timeout"), 12);
}

// 在持有语句和未提交事务时抛出异常，再通过改名、重开及删除验证资源释放。
TEST_F(SqliteTest, ReleasesStatementsTransactionsAndFileHandlesWhenUnwinding) {
    try {
        Connection connection(path_, OpenMode::read_write_create);
        connection.configure_wal();
        connection.execute("CREATE TABLE notes(id)");
        const Transaction transaction(connection);
        connection.execute("INSERT INTO notes VALUES (1)");
        auto query = connection.prepare("SELECT id FROM notes");
        ASSERT_TRUE(query.step());
        throw std::runtime_error("test unwind");
    } catch (const std::runtime_error &error) {
        ASSERT_STREQ(error.what(), "test unwind");
    }

    auto moved = path_;
    moved.replace_filename("reopened.db");
    std::filesystem::rename(path_, moved);
    {
        Connection connection(moved, OpenMode::read_write);
        EXPECT_EQ(read_int(connection, "SELECT count(*) FROM notes"), 0);
        connection.execute("INSERT INTO notes VALUES (2)");
    }
    EXPECT_TRUE(std::filesystem::remove(moved));
}

// 验证读事务在写入提交后仍看见旧快照，退出后能读取持久化的新数据。
TEST_F(SqliteTest, PersistsCommitAndKeepsReaderSnapshotUntilTransactionEnds) {
    Connection writer(path_, OpenMode::read_write_create);
    writer.configure_wal();
    writer.execute("CREATE TABLE notes(id)");
    Connection reader(path_, OpenMode::read_only);
    {
        Transaction snapshot(reader, TransactionMode::deferred);
        EXPECT_EQ(read_int(reader, "SELECT count(*) FROM notes"), 0);
        Transaction write(writer);
        writer.execute("INSERT INTO notes VALUES (1)");
        write.commit();
        EXPECT_EQ(read_int(reader, "SELECT count(*) FROM notes"), 0);
        snapshot.commit();
    }
    EXPECT_EQ(read_int(reader, "SELECT count(*) FROM notes"), 1);
    Connection reopened(path_, OpenMode::read_only);
    EXPECT_EQ(read_int(reopened, "SELECT id FROM notes"), 1);
}

// 覆盖未提交退出、约束失败和 SQLite 自动回滚，验证状态清理后可以重试。
TEST_F(SqliteTest, RollsBackStatementFailureAndUncommittedScope) {
    Connection connection(path_, OpenMode::read_write_create);
    connection.execute("CREATE TABLE notes(id UNIQUE)");
    {
        const Transaction write(connection);
        connection.execute("INSERT INTO notes VALUES (1)");
    }
    EXPECT_EQ(read_int(connection, "SELECT count(*) FROM notes"), 0);

    try {
        const Transaction write(connection);
        connection.execute("INSERT INTO notes VALUES (1)");
        connection.execute("INSERT INTO notes VALUES (1)");
        FAIL() << "Duplicate row should fail";
    } catch (const SqliteError &error) {
        EXPECT_EQ(error.extended_code(), SQLITE_CONSTRAINT_UNIQUE);
    }
    EXPECT_EQ(read_int(connection, "SELECT count(*) FROM notes"), 0);

    {
        const Transaction write(connection);
        connection.execute("INSERT INTO notes VALUES (1)");
        EXPECT_THROW(connection.execute("INSERT OR ROLLBACK INTO notes VALUES (1)"), SqliteError);
        EXPECT_THROW(Transaction nested(connection), std::logic_error);
    }
    Transaction retry(connection);
    connection.execute("INSERT INTO notes VALUES (2)");
    retry.commit();
    EXPECT_EQ(read_int(connection, "SELECT id FROM notes"), 2);
}

// 制造仅在提交时报告的外键错误，验证失败后数据回滚且连接能够继续使用。
TEST_F(SqliteTest, FailedCommitRollsBackDeferredForeignKeyViolation) {
    Connection connection(path_, OpenMode::read_write_create);
    connection.execute("CREATE TABLE parent(id INTEGER PRIMARY KEY);"
                       "CREATE TABLE child(parent_id REFERENCES parent(id) "
                       "DEFERRABLE INITIALLY DEFERRED)");
    Transaction write(connection);
    connection.execute("INSERT INTO child VALUES (5)");
    try {
        write.commit();
        FAIL() << "Deferred foreign key should reject COMMIT";
    } catch (const SqliteError &error) {
        EXPECT_EQ(error.extended_code(), SQLITE_CONSTRAINT_FOREIGNKEY);
    }
    EXPECT_EQ(read_int(connection, "SELECT count(*) FROM child"), 0);
    EXPECT_THROW(write.commit(), std::logic_error);
    Transaction retry(connection);
    connection.execute("INSERT INTO parent VALUES (5); INSERT INTO child VALUES (5)");
    retry.commit();
    EXPECT_EQ(read_int(connection, "SELECT parent_id FROM child"), 5);
}

// 用两条连接竞争写锁，验证第二条收到 SQLITE_BUSY 后可在锁释放时成功重试。
TEST_F(SqliteTest, CompetingWriterReportsBusyThenCanStartAfterRelease) {
    Connection first(path_, OpenMode::read_write_create);
    first.configure_wal();
    first.execute("CREATE TABLE notes(id)");
    Connection second(path_, OpenMode::read_write, 20);
    {
        const Transaction write(first);
        first.execute("INSERT INTO notes VALUES (1)");
        try {
            const Transaction blocked(second);
            FAIL() << "Second writer must wait for the first";
        } catch (const SqliteError &error) {
            EXPECT_EQ(error.primary_code(), SQLITE_BUSY);
        }
    }
    Transaction retry(second);
    second.execute("INSERT INTO notes VALUES (2)");
    retry.commit();
    EXPECT_EQ(read_int(first, "SELECT id FROM notes"), 2);
}

// 检查 SQL 和 C++ 数据转换按顺序提交，重开后按版本跳过已完成的迁移。
TEST_F(SqliteTest, MigratesInOrderAndSkipsAppliedStepsAfterReopen) {
    const std::vector<Migration> migrations{
        {1, "CREATE TABLE notes(id INTEGER PRIMARY KEY); INSERT INTO notes VALUES (1)"},
        {2, "ALTER TABLE notes ADD COLUMN title TEXT",
         // 在第二步的事务中为已有记录填充 UTF-8 标题。
         [](Connection &connection) {
             auto update = connection.prepare("UPDATE notes SET title = ?1 WHERE id = 1");
             update.bind_text(1, u8"书架");
             update.step();
         }},
    };
    {
        Connection connection(path_, OpenMode::read_write_create);
        EXPECT_EQ(schema_version(connection), 0);
        migrate_schema(connection, migrations);
        EXPECT_EQ(schema_version(connection), 2);
    }
    Connection connection(path_, OpenMode::read_only);
    migrate_schema(connection, migrations);
    EXPECT_EQ(schema_version(connection), 2);
    EXPECT_EQ(read_text(connection, "SELECT title FROM notes"), u8"书架");
}

// 第二步建表并插入数据后触发唯一约束错误，核对回滚与从上一个版本重试。
TEST_F(SqliteTest, FailedMigrationRollsBackDdlAndDataAndCanRetryFromLastVersion) {
    const Migration initial{1, "CREATE TABLE notes(id UNIQUE); INSERT INTO notes VALUES (1)"};
    {
        Connection connection(path_, OpenMode::read_write_create);
        EXPECT_THROW(migrate_schema(connection, {initial,
                                                 {2, "CREATE TABLE derived(value);"
                                                     "INSERT INTO notes VALUES (2);"
                                                     "INSERT INTO notes VALUES (2)"}}),
                     SqliteError);
    }
    Connection connection(path_, OpenMode::read_write);
    EXPECT_EQ(schema_version(connection), 1);
    EXPECT_EQ(read_int(connection, "SELECT count(*) FROM notes"), 1);
    EXPECT_EQ(read_int(connection, "SELECT count(*) FROM sqlite_schema WHERE name = 'derived'"), 0);
    migrate_schema(connection, {initial, {2, "CREATE TABLE derived(value)"}});
    EXPECT_EQ(schema_version(connection), 2);
    EXPECT_EQ(read_int(connection, "SELECT count(*) FROM derived"), 0);
}

// 验证迁移拒绝跳号、脚本接管事务或版本号，以及高于应用支持范围的 schema。
TEST_F(SqliteTest, RejectsIncompletePlanFutureSchemaAndSqlTransactionControl) {
    Connection connection(path_, OpenMode::read_write_create);
    const Migration initial{1, "CREATE TABLE notes(id)"};
    EXPECT_THROW(migrate_schema(connection, {initial, {3, "CREATE TABLE next(id)"}}),
                 std::invalid_argument);
    EXPECT_THROW(migrate_schema(connection, {{1, "CREATE TABLE notes(id); COMMIT"}}), SqliteError);
    EXPECT_THROW(migrate_schema(connection, {{1, "PRAGMA user_version = 8"}}), SqliteError);
    EXPECT_EQ(schema_version(connection), 0);
    EXPECT_EQ(read_int(connection, "SELECT count(*) FROM sqlite_schema"), 0);

    connection.execute("PRAGMA user_version = 9");
    EXPECT_THROW(migrate_schema(connection, {initial}), markdownshelf::db::SchemaTooNew);
    EXPECT_EQ(schema_version(connection), 9);
    EXPECT_EQ(read_int(connection, "SELECT count(*) FROM sqlite_schema"), 0);
    EXPECT_EQ(read_text(connection, "PRAGMA journal_mode"), "delete");
}

// 预先写入违反外键的旧数据，验证迁移报告问题并回滚本步的表和版本变更。
TEST_F(SqliteTest, MigrationRejectsPreexistingForeignKeyViolations) {
    Connection connection(path_, OpenMode::read_write_create);
    connection.execute("PRAGMA foreign_keys = OFF;"
                       "CREATE TABLE parent(id INTEGER PRIMARY KEY);"
                       "CREATE TABLE child(parent_id REFERENCES parent(id));"
                       "INSERT INTO child VALUES (5);"
                       "PRAGMA foreign_keys = ON");
    try {
        migrate_schema(connection, {{1, "CREATE TABLE derived(id)"}});
        FAIL() << "Migration must reject the inconsistent database";
    } catch (const SqliteError &error) {
        EXPECT_EQ(error.extended_code(), SQLITE_CONSTRAINT_FOREIGNKEY);
    }
    EXPECT_EQ(schema_version(connection), 0);
    EXPECT_EQ(read_int(connection, "SELECT count(*) FROM sqlite_schema WHERE name = 'derived'"), 0);
    EXPECT_EQ(read_int(connection, "SELECT parent_id FROM child"), 5);
}

// FTS5 冒烟测试使用的无状态分词器实例，整段输入作为一个词项。
struct WholeTextTokenizer {};

// 为 SQLite 创建分词器实例，经 out 返回句柄，分配失败时返回 SQLITE_NOMEM。
int create_tokenizer(void *, const char **, int, Fts5Tokenizer **out) noexcept {
    auto *tokenizer = new (std::nothrow) WholeTextTokenizer;
    *out = reinterpret_cast<Fts5Tokenizer *>(tokenizer);
    return tokenizer ? SQLITE_OK : SQLITE_NOMEM;
}

// 接收 SQLite 交回的实例句柄，恢复具体类型并释放工厂分配的实例。
void delete_tokenizer(Fts5Tokenizer *tokenizer) noexcept {
    delete reinterpret_cast<WholeTextTokenizer *>(tokenizer);
}

// 通过 emit 发送完整文本及 UTF-8 字节区间，空输入直接返回成功。
int tokenize(Fts5Tokenizer *, void *context, int, const char *text, int length,
             int (*emit)(void *, int, const char *, int, int, int)) noexcept {
    return length > 0 ? emit(context, 0, text, length, 0, length) : SQLITE_OK;
}

// 提供给 FTS5 注册入口的回调表，分别负责创建、释放和分词。
const fts5_tokenizer whole_text_tokenizer{create_tokenizer, delete_tokenizer, tokenize};

// 验证索引和 MATCH 均使用自定义分词器，并检查重名保护和重开后逐连接注册的要求。
TEST_F(SqliteTest, CustomTokenizerIsUsedForIndexAndQueryAndRegisteredPerConnection) {
    const std::string_view name = "markdownshelf_smoke_v1";
    {
        Connection connection(path_, OpenMode::read_write_create);
        register_fts5_tokenizer(connection, name, whole_text_tokenizer);
        EXPECT_THROW(register_fts5_tokenizer(connection, name, whole_text_tokenizer),
                     std::invalid_argument);
        migrate_schema(connection,
                       {{1, "CREATE VIRTUAL TABLE notes USING "
                            "fts5(body, tokenize = 'markdownshelf_smoke_v1');"
                            "INSERT INTO notes(rowid, body) VALUES (3, 'std::vector')"}});
    }

    Connection reader(path_, OpenMode::read_only);
    {
        auto unregistered = reader.prepare("SELECT rowid FROM notes WHERE notes MATCH ?1");
        unregistered.bind_text(1, "\"std::vector\"");
        EXPECT_THROW(unregistered.step(), SqliteError);
    }
    register_fts5_tokenizer(reader, name, whole_text_tokenizer);
    auto query = reader.prepare("SELECT rowid FROM notes WHERE notes MATCH ?1");
    query.bind_text(1, "\"std::vector\"");
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.column_int64(0), 3);
    EXPECT_FALSE(query.step());
    query.reset();
    query.bind_text(1, "vector");
    EXPECT_FALSE(query.step());
}

} // namespace

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
