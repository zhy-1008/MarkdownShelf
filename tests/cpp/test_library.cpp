#include "markdownshelf/library.h"

#include "../../src/core/domain/library_internal.h"
#include "../../src/core/platform/library_files_win.h"
#include "markdownshelf/sqlite.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
// MSVC 会对 GoogleTest 断言宏中的常量 switch 报告 C6326。
#pragma warning(push)
#pragma warning(disable : 6326)
#endif

namespace markdownshelf {
namespace {

namespace fs = std::filesystem;
using Code = LibraryErrorCode;
using Step = detail::CreateStep;

// 给每个测试分配独立文件系统根，测试局部句柄先于清理释放。
class LibraryTest : public testing::Test {
  protected:
    // 建立为当前测试生成的隔离目录。
    void SetUp() override {
        const auto candidate = fs::temp_directory_path() / ("ms-p13-" + platform::new_uuid());
        ASSERT_TRUE(fs::create_directory(candidate));
        root_ = candidate;
    }

    // 移除测试确认创建且仍位于临时根下的目录，检查所有文件句柄已经释放。
    void TearDown() override {
        if (root_.empty()) {
            return;
        }
        std::error_code error;
        fs::remove_all(root_, error);
        EXPECT_FALSE(error) << error.message();
    }

    // 保存该用例的临时目录。
    fs::path root_;
};

// 断言调用抛出指定错误并保留稳定消息键。
template <typename Action>
void expect_library_error(Action action, Code code, std::string_view key) {
    try {
        action();
        ADD_FAILURE() << "预期库操作失败";
    } catch (const LibraryError &error) {
        EXPECT_EQ(code, error.code());
        EXPECT_EQ(key, error.message_key());
        EXPECT_FALSE(error.operation_id().empty());
    }
}

// 将固定查询的第一列读取为整数，供独立读取 SQLite 持久状态的断言使用。
std::int64_t scalar_integer(db::Connection &connection, std::string_view sql) {
    auto statement = connection.prepare(sql);
    EXPECT_TRUE(statement.step());
    return statement.column_int64(0).value();
}

// 验证 Unicode 名称、成套产物、数据库初始元信息和重新打开。
TEST_F(LibraryTest, CreatesAndReopensConsistentFolder) {
    const auto created = Library::create(root_, u8"工程 资料📚");
    EXPECT_EQ(root_ / fs::u8path(u8"工程 资料📚"), created.root);
    EXPECT_EQ(u8"工程 资料📚", created.display_name);
    EXPECT_EQ(1, created.format_version);
    EXPECT_EQ(1, created.schema_version);
    EXPECT_TRUE(fs::is_empty(created.root / "assets"));
    EXPECT_TRUE(fs::is_regular_file(created.root / "library.json"));
    EXPECT_EQ(created.created_utc, Library::inspect(created.root).info.created_utc);
    {
        db::Connection reader(created.root / "library.db", db::OpenMode::read_only);
        EXPECT_EQ(0x4D53484C, scalar_integer(reader, "PRAGMA application_id"));
        EXPECT_EQ(1, scalar_integer(reader, "PRAGMA user_version"));
        EXPECT_EQ(created.created_at,
                  scalar_integer(reader, "SELECT created_at FROM library_meta"));
        EXPECT_EQ(1, scalar_integer(reader, "SELECT count(*) FROM library_meta"));
        EXPECT_EQ(0, scalar_integer(reader, "SELECT search_index_version FROM library_meta"));
        EXPECT_EQ(0, scalar_integer(reader, "SELECT content_epoch FROM library_meta"));
        EXPECT_EQ(1, scalar_integer(reader, "SELECT active_fts_slot IS NULL FROM library_meta"));
    }
    auto lease = LibraryLease::acquire(created.root);
    auto session = Library::open(created.root, *lease);
    EXPECT_FALSE(lease->valid());
    EXPECT_TRUE(session->is_open());
    EXPECT_EQ(created.library_uuid, session->info().library_uuid);
    session->close();
    session->close();
    EXPECT_FALSE(session->is_open());
    db::Connection reader(created.root / "library.db", db::OpenMode::read_only);
    auto mode = reader.prepare("PRAGMA journal_mode");
    ASSERT_TRUE(mode.step());
    EXPECT_EQ("wal", mode.column_text(0));
}

// 验证允许替换原空目录，同时非空目标的既有文件保持原样。
TEST_F(LibraryTest, PublishesIntoEmptyTargetAndPreservesNonemptyTarget) {
    const auto target = root_ / "existing";
    ASSERT_TRUE(fs::create_directory(target));
    const auto info = Library::create(root_, "existing");
    EXPECT_EQ(target, info.root);
    EXPECT_FALSE(info.retained_directory);
    const auto user_file = target / "user.txt";
    std::ofstream(user_file) << "original";
    expect_library_error(
        // 尝试在已经包含库和用户文件的同一目录创建。
        [&] { Library::create(root_, "existing"); }, Code::invalid_input,
        "library.target_not_empty");
    EXPECT_EQ(8, fs::file_size(user_file));
    EXPECT_EQ(info.library_uuid, Library::inspect(target).info.library_uuid);
    EXPECT_EQ(1, std::distance(fs::directory_iterator(root_), fs::directory_iterator{}));
}

// 模拟初始化失败，确保暂存目录被清理且原空目录身份保持一致。
TEST_F(LibraryTest, CleansOwnedStageOnInitializationFailure) {
    const auto target = root_ / "existing";
    ASSERT_TRUE(fs::create_directory(target));
    const auto before = platform::resolve_directory(target).identity;
    EXPECT_THROW(detail::create_library(root_, "existing",
                                        // 在初始化开始时注入 I/O 失败。
                                        [](Step step, const fs::path &) {
                                            if (step == Step::before_initialize) {
                                                throw LibraryError(Code::io_error,
                                                                   "test.initialization");
                                            }
                                        }),
                 LibraryError);
    const auto after = platform::resolve_directory(target);
    EXPECT_EQ(before.VolumeSerialNumber, after.identity.VolumeSerialNumber);
    EXPECT_EQ(std::memcmp(before.FileId.Identifier, after.identity.FileId.Identifier,
                          sizeof(before.FileId.Identifier)),
              0);
    EXPECT_EQ(1, std::distance(fs::directory_iterator(root_), fs::directory_iterator{}));
}

// 在暂存路径预放独立文件，初始化失败时只清理本次生成的资产目录。
TEST_F(LibraryTest, PreservesAnUnownedFileInStage) {
    try {
        detail::create_library(root_, "unpublished",
                               // 模拟不属于本次创建的文件提前占据主库文件名。
                               [](Step step, const fs::path &stage) {
                                   if (step == Step::before_initialize) {
                                       std::ofstream(stage / "library.db") << "user content";
                                   }
                               });
        FAIL() << "预期原有文件阻止创建";
    } catch (const LibraryError &error) {
        EXPECT_EQ("library.cleanup_stage_failed", error.message_key());
        ASSERT_TRUE(error.recovery_path());
        std::ifstream existing(*error.recovery_path() / "library.db");
        std::string contents;
        std::getline(existing, contents);
        EXPECT_EQ("user content", contents);
        EXPECT_FALSE(fs::exists(*error.recovery_path() / "assets"));
    }
    EXPECT_FALSE(fs::exists(root_ / "unpublished"));
}

// 在原目录已暂移的边界失败，验证按原文件 ID 恢复并清理初始化产物。
TEST_F(LibraryTest, RestoresOriginalDirectoryIdentityOnPublishFailure) {
    const auto target = root_ / "existing";
    ASSERT_TRUE(fs::create_directory(target));
    const auto before = platform::resolve_directory(target).identity;
    EXPECT_THROW(detail::create_library(root_, "existing",
                                        // 在暂移完成和重命名发布之间模拟故障。
                                        [&](Step step, const fs::path &stage) {
                                            if (step == Step::before_publish) {
                                                EXPECT_FALSE(fs::exists(target));
                                                EXPECT_TRUE(
                                                    fs::is_regular_file(stage / "library.db"));
                                                throw LibraryError(Code::io_error, "test.publish");
                                            }
                                        }),
                 LibraryError);
    const auto after = platform::resolve_directory(target);
    EXPECT_EQ(std::memcmp(before.FileId.Identifier, after.identity.FileId.Identifier,
                          sizeof(before.FileId.Identifier)),
              0);
    EXPECT_TRUE(fs::is_empty(target));
    EXPECT_EQ(1, std::distance(fs::directory_iterator(root_), fs::directory_iterator{}));
}

// 模拟第三方在目标暂移后占据原路径，两份目录都应保留并可定位。
TEST_F(LibraryTest, PreservesBothDirectoriesWhenTargetIsOccupiedDuringPublish) {
    const auto target = root_ / "existing";
    ASSERT_TRUE(fs::create_directory(target));
    try {
        detail::create_library(root_, "existing",
                               // 模拟另一进程取得目标路径并写入文件。
                               [&](Step step, const fs::path &) {
                                   if (step == Step::before_publish) {
                                       fs::create_directory(target);
                                       std::ofstream(target / "other.txt") << "occupied";
                                   }
                               });
        FAIL() << "预期发布和还原都因路径被占据而失败";
    } catch (const LibraryError &error) {
        EXPECT_EQ("library.restore_target_failed", error.message_key());
        ASSERT_TRUE(error.recovery_path());
        EXPECT_TRUE(fs::is_empty(*error.recovery_path()));
        EXPECT_EQ(8, fs::file_size(target / "other.txt"));
    }
    EXPECT_EQ(2, std::distance(fs::directory_iterator(root_), fs::directory_iterator{}));
}

// 让两个独立服务调用同时发布到同一路径，仅应有一个成功。
TEST_F(LibraryTest, ConcurrentCreatesPublishExactlyOneLibrary) {
    std::promise<void> start;
    const auto ready = start.get_future().share();
    // 等待相同开始信号，返回是否实际获得正式库路径。
    auto create = [&] {
        ready.wait();
        try {
            Library::create(root_, "concurrent");
            return 1;
        } catch (const LibraryError &) {
            return 0;
        }
    };
    auto first = std::async(std::launch::async, create);
    auto second = std::async(std::launch::async, create);
    start.set_value();
    EXPECT_EQ(1, first.get() + second.get());
    EXPECT_EQ(1, std::distance(fs::directory_iterator(root_), fs::directory_iterator{}));
    EXPECT_EQ("concurrent", Library::inspect(root_ / "concurrent").info.display_name);
}

// 验证租约消费绑定物理根，同 UUID 的独立副本也不能借用另一目录的租约。
TEST_F(LibraryTest, RejectsLeaseForAnotherPhysicalRootAndReleasesIt) {
    const auto original = Library::create(root_, "original");
    const auto copied = root_ / "copy";
    fs::copy(original.root, copied, fs::copy_options::recursive);
    EXPECT_EQ(original.library_uuid, Library::inspect(copied).info.library_uuid);
    auto lease = LibraryLease::acquire(original.root);
    expect_library_error(
        // 在复制件上使用原库的路径租约。
        [&] { Library::open(copied, *lease); }, Code::invalid_input, "library.lease_path_mismatch");
    EXPECT_FALSE(lease->valid());
    EXPECT_TRUE(LibraryLease::acquire(original.root)->valid());
    auto copy_lease = LibraryLease::acquire(copied);
    auto session = Library::open(copied, *copy_lease);
    EXPECT_EQ(original.library_uuid, session->info().library_uuid);
}

// 在小库和固定 10,000 行的合成库上测量只读检查开销，不把本机耗时作为通过阈值。
TEST_F(LibraryTest, MeasuresMetadataAndQuickCheckOnRepeatableLibraries) {
    const auto empty = Library::create(root_, "empty");
    const auto sample = Library::create(root_, "sample");
    {
        db::Connection writer(sample.root / "library.db", db::OpenMode::read_write);
        writer.execute("CREATE TABLE samples(id INTEGER PRIMARY KEY, payload BLOB NOT NULL)");
        writer.execute("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE "
                       "i<10000) INSERT INTO samples SELECT i, zeroblob(4096) FROM n");
    }
    using Clock = std::chrono::steady_clock;
    using Micros = std::chrono::microseconds;
    for (const auto &library : {empty, sample}) {
        std::vector<Micros::rep> metadata_times;
        std::vector<Micros::rep> quick_times;
        for (int run = 0; run < 21; ++run) {
            auto start = Clock::now();
            ASSERT_EQ(library.library_uuid, Library::inspect(library.root).info.library_uuid);
            auto metadata = std::chrono::duration_cast<Micros>(Clock::now() - start).count();
            db::Connection reader(library.root / "library.db", db::OpenMode::read_only);
            start = Clock::now();
            auto quick = reader.prepare("PRAGMA quick_check(1)");
            ASSERT_TRUE(quick.step());
            ASSERT_EQ("ok", quick.column_text(0));
            auto checked = std::chrono::duration_cast<Micros>(Clock::now() - start).count();
            if (run != 0) {
                metadata_times.push_back(metadata);
                quick_times.push_back(checked);
            }
        }
        std::sort(metadata_times.begin(), metadata_times.end());
        std::sort(quick_times.begin(), quick_times.end());
        std::cout << "Library " << library.display_name
                  << " bytes=" << fs::file_size(library.root / "library.db")
                  << " inspect_us p50=" << metadata_times[9] << " p95=" << metadata_times[18]
                  << " quick_check_us p50=" << quick_times[9] << " p95=" << quick_times[18] << '\n';
    }
}

} // namespace
} // namespace markdownshelf

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
