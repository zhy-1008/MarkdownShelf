#pragma once

#include "markdownshelf/library.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace markdownshelf::platform {

// Windows 文件句柄的关闭策略，可以在任何线程释放。
struct CloseHandle {
    // 关闭调用方独占的非空句柄，清理过程不抛异常。
    void operator()(void *handle) const noexcept;
};

using Handle = std::unique_ptr<void, CloseHandle>;

// 保存由句柄得到的物理身份，用于比较别名和被替换的目录。
struct Directory {
    // 独占目录句柄，阻止其他调用移除该对象。
    Handle handle;
    // 保存规范化的 Win32 扩展路径，供创建子项和 SQLite 打开。
    std::filesystem::path path;
    // 保存卷序列号和文件 ID。
    FILE_ID_INFO identity;
};

// 验证 Windows 单一目录名，并将合法 UTF-8 转换为 UTF-16。
std::wstring directory_name(std::string_view name);
// 验证输入为 UTF-8，拒绝内嵌 NUL 或非法字节序列。
bool valid_utf8(std::string_view value) noexcept;
// 生成小写字符串形式的 Windows GUID。
std::string new_uuid();
// 从 epoch 毫秒生成 UTC 时间文本，无效或越界时返回空字符串。
std::string utc_text(std::int64_t epoch_ms);
// 将普通长度的扩展路径转换为调用方熟悉的 DOS 或 UNC 路径。
std::filesystem::path display_path(const std::filesystem::path &path);
// 解析用户指定的库目录或已存在的父目录，返回守护句柄和物理身份。
Directory resolve_directory(const std::filesystem::path &input);
// 打开受管资产目录并拒绝重解析点，缺失或类型错误抛出 LibraryError。
Handle open_assets(const Directory &root);
// 验证并守护数据库文件，核验现有 SQLite 辅助文件的类型和归属。
Handle guard_database(const Directory &root);
// 用 Windows 访问检查确认 root 允许创建文件或子目录。
void require_directory_access(const Directory &root, DWORD access);
// 提前拒绝非空目标，发布前仍须按实际句柄重新核验。
void check_create_target(const std::filesystem::path &target);
// 在规范父目录的路径锁文件上取得不共享的句柄，失败抛出 LibraryError。
Handle lock_library_path(const Directory &root);
// 比较目录的实际卷和文件 ID。
bool same_directory(const Directory &first, const Directory &second) noexcept;
// 读取受管清单，文件大小须在上限以内。
std::string read_manifest_file(const Directory &root);

// 管理本次创建的同卷暂存目录及发布回滚，所有操作在调用线程进行。
class StagedDirectory {
  public:
    // 借用 parent 并建立唯一暂存根，parent 必须覆盖本对象生命周期。
    StagedDirectory(const Directory &parent, std::string_view uuid);
    // 清理已确认归属且尚未发布的暂存项。
    ~StagedDirectory();
    // 返回当前暂存目录，借用至本对象销毁。
    const Directory &root() const noexcept;
    // 独占创建空数据库文件并记下身份，返回阻止替换的守护句柄。
    Handle create_database();
    // 创建空资源目录并记录身份，返回阻止替换的守护句柄。
    Handle create_assets();
    // 独占创建清单并记录身份，写入且刷盘后返回阻止替换的守护句柄。
    Handle write_manifest(std::string_view contents);
    // 暂移已复核为空的目标目录，原目标不存在时直接返回。
    void prepare_target(const std::filesystem::path &target, std::string_view uuid);
    // 以禁止覆盖的重命名发布，返回无法清理的旧目录位置。
    std::optional<std::filesystem::path> publish(const std::filesystem::path &target);
    // 在抛出原始异常前尝试恢复原目录，路径冲突时报告保留位置。
    void restore_target(const std::filesystem::path &target);
    // 清理尚未发布的暂存项，返回暂存根是否可删除。
    bool discard() noexcept;

  private:
    // 借用规范化父目录，守护暂存和发布使用的路径。
    const Directory &parent_;
    // 独占暂存根及其物理身份。
    Directory root_;
    // 保存本次创建的数据库文件身份，用于失败清理。
    std::optional<FILE_ID_INFO> database_id_;
    // 保存本次创建的资源目录身份，用于失败清理。
    std::optional<FILE_ID_INFO> assets_id_;
    // 保存本次创建的清单文件身份，用于失败清理。
    std::optional<FILE_ID_INFO> manifest_id_;
    // 保存被暂移的原目标目录，空表示本次没有移动旧目录。
    Handle previous_;
    // 保存原目标暂移后的位置。
    std::filesystem::path previous_path_;
    // 标记暂存根已经成为正式库，析构时跳过清理。
    bool published_ = false;
};

} // namespace markdownshelf::platform
