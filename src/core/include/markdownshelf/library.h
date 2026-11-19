#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace markdownshelf {

// 库服务向 C++ 和 Python 调用方提供的稳定错误类别。
enum class LibraryErrorCode {
    // 路径、名称或租约参数不能用于当前操作。
    invalid_input,
    // 必需的库路径不存在。
    not_found,
    // 路径或数据库处于共享冲突，释放占用后可重试。
    library_busy,
    // 清单格式或版本无法识别。
    unsupported_format,
    // SQLite schema 高于当前程序支持的版本。
    schema_too_new,
    // 库的文件结构、内容或身份验证失败。
    integrity_failed,
    // 文件系统、权限或数据库 I/O 失败。
    io_error
};

// 返回用于跨语言传输的英文错误码名称。
std::string_view library_error_code_name(LibraryErrorCode code) noexcept;

// 保存一次库操作的错误契约，自有字符串和恢复路径可跨线程读取。
class LibraryError : public std::runtime_error {
  public:
    // 保存错误类别、消息键及可选恢复路径，并生成用于关联诊断的操作 ID。
    LibraryError(LibraryErrorCode code, std::string_view message_key,
                 std::optional<std::filesystem::path> recovery_path = std::nullopt);
    // 返回稳定错误类别。
    LibraryErrorCode code() const noexcept;
    // 返回本地化消息键，借用至异常对象销毁前。
    const std::string &message_key() const noexcept;
    // 返回占用解除后能否重试。
    bool retryable() const noexcept;
    // 返回本次失败操作的 ID，借用至异常对象销毁前。
    const std::string &operation_id() const noexcept;
    // 返回回滚或清理时保留的目录位置，无保留目录时为空。
    const std::optional<std::filesystem::path> &recovery_path() const noexcept;

  private:
    // 保存错误的稳定类别。
    LibraryErrorCode code_;
    // 保存应用翻译表使用的消息键。
    std::string message_key_;
    // 保存本次操作的诊断关联 ID。
    std::string operation_id_;
    // 保存需由调用方提示用户的保留目录。
    std::optional<std::filesystem::path> recovery_path_;
};

// 库的可复制元数据，字段为自有值，不延长文件或连接的生命周期。
struct LibraryInfo {
    // 指向物理库根的规范化路径。
    std::filesystem::path root;
    // 保存小写标准形式的库 UUID。
    std::string library_uuid;
    // 保存数据库中的 UTF-8 显示名。
    std::string display_name;
    // 保存格式为 YYYY-MM-DDTHH:MM:SS.sssZ 的创建时刻。
    std::string created_utc;
    // 保存数据库中的 UTC epoch 毫秒。
    std::int64_t created_at;
    // 保存清单格式版本。
    int format_version;
    // 保存 SQLite user_version。
    int schema_version;
    // 发布成功后如原空目录无法安全删除，保存它的保留位置。
    std::optional<std::filesystem::path> retained_directory;
};

// 保存只读结构检查结果，成功时包含已核对的库元数据。
struct LibraryInspection {
    // 保存 DB、清单与目录相互匹配后的信息。
    LibraryInfo info;
};

// 库服务使用的独占租约。同步保护一次性移交，析构释放未移交的句柄。
class LibraryLease {
  public:
    // 只读确认 root 的身份，再独占父目录下的稳定文件。失败抛出 LibraryError。
    static std::unique_ptr<LibraryLease> acquire(const std::filesystem::path &root);
    // 释放尚未移交给会话的租约。
    ~LibraryLease();
    // 返回租约是否仍可移交给 open。
    bool valid() const;

  private:
    // 允许库服务消费租约并移交给会话。
    friend class Library;
    // 允许会话持有移交后的实现。
    friend class LibrarySession;
    // 占有 Windows 句柄和库身份的内部类型。
    struct State;
    // 接管已取得的租约状态。
    explicit LibraryLease(std::unique_ptr<State> state);
    // 串行访问租约状态。
    mutable std::mutex mutex_;
    // 保存待移交状态，移交后为空。
    std::unique_ptr<State> state_;
};

// 独占打开的连接和租约。关闭可同步发起，库内连接供打开它的线程使用。
class LibrarySession {
  public:
    // 销毁连接并释放租约。
    ~LibrarySession();
    // 返回此次开库核验的元数据，借用至会话对象销毁前。
    const LibraryInfo &info() const noexcept;
    // 返回会话是否仍持有连接。
    bool is_open() const;
    // 先关闭连接再释放租约，可以重复调用。
    void close();

  private:
    // 允许库服务构造持有有效租约和连接的会话。
    friend class Library;
    // 连接与租约的内部所有权类型。
    struct State;
    // 保存开库信息并接管内部状态。
    LibrarySession(LibraryInfo info, std::unique_ptr<State> state);
    // 保存会话元数据，关闭后仍可读取。
    const LibraryInfo info_;
    // 串行访问连接和租约生命周期。
    mutable std::mutex mutex_;
    // 保存存活的连接和租约，关闭后为空。
    std::unique_ptr<State> state_;
};

// 提供同步库服务。每次调用独立管理文件句柄和临时连接。
class Library {
  public:
    // 在 parent 下新建名为 name 的文件夹库，成功返回元数据，失败抛出 LibraryError。
    static LibraryInfo create(const std::filesystem::path &parent, std::string_view name);
    // 只读检查 root 的目录、清单和 DB 元数据，失败抛出 LibraryError。
    static LibraryInspection inspect(const std::filesystem::path &root);
    // 消费 lock 并检查 root，验证通过后返回可写会话，失败释放已消费的租约。
    static std::unique_ptr<LibrarySession> open(const std::filesystem::path &root,
                                                LibraryLease &lock);
};

} // namespace markdownshelf
