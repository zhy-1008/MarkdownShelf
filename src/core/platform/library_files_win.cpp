#include "library_files_win.h"

#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <new>

namespace markdownshelf::platform {
namespace {

using Code = LibraryErrorCode;
namespace fs = std::filesystem;

// 约束清单读取量，避免把任意大文件装入内存。
constexpr DWORD manifest_limit = 64 * 1024;
// 允许读取与写入，阻止其他调用移除受管文件。
constexpr DWORD share_read_write = FILE_SHARE_READ | FILE_SHARE_WRITE;
// 直接打开目录或文件本身，使调用方能检查最终条目的重解析属性。
constexpr DWORD exact_entry = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
// 保存需要按普通文件校验的 SQLite 辅助文件名。
constexpr std::array sidecars = {L"library.db-wal", L"library.db-shm", L"library.db-journal"};

// 判断 Windows 返回的是对象或其父路径不存在。
bool missing(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// 将 Windows 文件系统失败映射为有区分的库错误。
[[noreturn]] void file_error(DWORD error, std::string_view key) {
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
        throw LibraryError(Code::library_busy, "library.path_busy");
    }
    if (error == ERROR_ACCESS_DENIED || error == ERROR_WRITE_PROTECT ||
        error == ERROR_PRIVILEGE_NOT_HELD) {
        throw LibraryError(Code::io_error, "library.permission_denied");
    }
    if (missing(error)) {
        throw LibraryError(Code::not_found, key);
    }
    throw LibraryError(Code::io_error, key);
}

// 打开路径指定的最终条目。直接打开重解析点用于随后拒绝它，可选路径不存在时返回空句柄。
Handle open_handle(const fs::path &path, DWORD access, DWORD share, DWORD disposition, DWORD flags,
                   std::string_view key, bool optional = false) {
    HANDLE raw = CreateFileW(path.c_str(), access, share, nullptr, disposition, flags, nullptr);
    if (raw != INVALID_HANDLE_VALUE) {
        return Handle(raw);
    }
    const DWORD error = GetLastError();
    if (optional && missing(error)) {
        return {};
    }
    file_error(error, key);
}

// 读取对象本身的类型和硬链接计数，拒绝错类型、重解析点和有别名的受管文件。
void check_entry(HANDLE handle, bool directory, std::string_view key) {
    BY_HANDLE_FILE_INFORMATION data{};
    if (!GetFileInformationByHandle(handle, &data)) {
        file_error(GetLastError(), key);
    }
    if (((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (!directory && data.nNumberOfLinks != 1)) {
        throw LibraryError(Code::integrity_failed, key);
    }
}

// 将路径规范化为绝对扩展路径，Win32 特殊设备命名空间直接拒绝。
fs::path extended_path(const fs::path &input) {
    auto path = input.native();
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.empty() || path.find(L'\0') != std::wstring::npos ||
        path.compare(0, 4, L"\\\\.\\") == 0) {
        throw LibraryError(Code::invalid_input, "library.invalid_path");
    }
    if (path.compare(0, 4, L"\\\\?\\") == 0) {
        if (path.compare(4, 4, L"UNC\\") != 0 &&
            (path.size() < 7 || path[5] != L':' || path[6] != L'\\')) {
            throw LibraryError(Code::invalid_input, "library.invalid_path");
        }
        return path;
    }
    const DWORD length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!length) {
        file_error(GetLastError(), "library.invalid_path");
    }
    std::wstring full(length, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), length, full.data(), nullptr);
    if (!written || written >= length) {
        throw LibraryError(Code::invalid_input, "library.invalid_path");
    }
    full.resize(written);
    if (full.compare(0, 2, L"\\\\") == 0) {
        return L"\\\\?\\UNC\\" + full.substr(2);
    }
    return L"\\\\?\\" + full;
}

// 从已打开的句柄取得卷和文件 ID。
FILE_ID_INFO identity_of(HANDLE handle) {
    FILE_ID_INFO id{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))) {
        file_error(GetLastError(), "library.file_identity");
    }
    return id;
}

// 比较底层卷和文件 ID。
bool same_identity(const FILE_ID_INFO &first, const FILE_ID_INFO &second) noexcept {
    return first.VolumeSerialNumber == second.VolumeSerialNumber &&
           std::memcmp(first.FileId.Identifier, second.FileId.Identifier,
                       sizeof(first.FileId.Identifier)) == 0;
}

// 从已经打开的目录句柄取得实际路径和物理身份，仅在成功后接管句柄。
Directory identify_directory(Handle &&handle) {
    check_entry(handle.get(), true, "library.invalid_directory");
    const DWORD capacity = GetFinalPathNameByHandleW(handle.get(), nullptr, 0, 0);
    if (!capacity) {
        file_error(GetLastError(), "library.resolve_directory");
    }
    std::wstring path(capacity, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(handle.get(), path.data(), capacity, 0);
    if (!length || length >= capacity) {
        throw LibraryError(Code::io_error, "library.resolve_directory");
    }
    path.resize(length);
    const auto id = identity_of(handle.get());
    return {std::move(handle), std::move(path), id};
}

// 返回检查目录是否为空的结果。调用方应持有阻止该目录被改名的句柄。
bool directory_is_empty(const fs::path &path) {
    std::error_code error;
    const bool empty = fs::is_empty(path, error);
    if (error) {
        file_error(static_cast<DWORD>(error.value()), "library.enumerate_directory");
    }
    return empty;
}

// 通过已核验对象的句柄重命名到同卷路径，系统将在目标存在时拒绝覆盖。
DWORD rename_directory(HANDLE source, const fs::path &target) {
    const auto &path = target.native();
    const auto size = path.size() * sizeof(wchar_t);
    const auto bytes = sizeof(FILE_RENAME_INFO) + size;
    // new[] 的缓冲区对 FILE_RENAME_INFO 所需的对齐有效。
    auto buffer = std::make_unique<unsigned char[]>(bytes);
    auto *rename = ::new (buffer.get()) FILE_RENAME_INFO{};
    rename->FileNameLength = static_cast<DWORD>(size);
    // Win32 结构的末字段是长度可变的文件名。
    std::memcpy(buffer.get() + offsetof(FILE_RENAME_INFO, FileName), path.data(), size);
    return SetFileInformationByHandle(source, FileRenameInfo, rename, static_cast<DWORD>(bytes))
               ? ERROR_SUCCESS
               : GetLastError();
}

// 请求系统在句柄关闭后删除文件或空目录，非空目录将由系统拒绝。
bool delete_on_close(HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                      sizeof(disposition)) != 0;
}

// 直接打开条目，仅当物理身份符合本次创建记录时才删除。
void delete_created_entry(const fs::path &path, const std::optional<FILE_ID_INFO> &expected) {
    if (expected) {
        const auto entry = open_handle(path, DELETE | FILE_READ_ATTRIBUTES, share_read_write,
                                       OPEN_EXISTING, exact_entry, "library.cleanup_stage", true);
        if (entry && same_identity(identity_of(entry.get()), *expected)) {
            delete_on_close(entry.get());
        }
    }
}

// 对最终目录的名称计算确定性标识，Windows 同名别名使用同一个锁文件。
std::wstring lock_filename(const fs::path &basename) {
    const auto &name = basename.native();
    const auto length = static_cast<int>(name.size());
    const int capacity = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, name.data(), length,
                                       nullptr, 0, nullptr, nullptr, 0);
    if (!capacity) {
        throw LibraryError(Code::invalid_input, "library.invalid_path");
    }
    std::wstring upper(static_cast<std::size_t>(capacity), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, name.data(), length, upper.data(),
                       capacity, nullptr, nullptr, 0)) {
        throw LibraryError(Code::invalid_input, "library.invalid_path");
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (const wchar_t unit : upper) {
        hash = (hash ^ static_cast<std::uint16_t>(unit)) * 1099511628211ULL;
    }
    std::array<wchar_t, 17> digits{};
    swprintf_s(digits.data(), digits.size(), L"%016llx", hash);
    return std::wstring(L".markdownshelf-lock-") + digits.data();
}

// 判断名称是否为 Windows DOS 设备，包括带扩展名及带上标数字的别名。
bool reserved_name(std::wstring_view name) {
    auto stem = name.substr(0, name.find(L'.'));
    const auto last = stem.find_last_not_of(L' ');
    stem = stem.substr(0, last == std::wstring_view::npos ? 0 : last + 1);
    for (const std::wstring_view device : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"}) {
        if (CompareStringOrdinal(stem.data(), static_cast<int>(stem.size()), device.data(),
                                 static_cast<int>(device.size()), TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return stem.size() == 4 && std::wstring_view(L"123456789¹²³").find(stem.back()) != stem.npos &&
           (CompareStringOrdinal(stem.data(), 3, L"COM", 3, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(stem.data(), 3, L"LPT", 3, TRUE) == CSTR_EQUAL);
}

// 创建随机命名的同卷暂存根，返回排他守护并可改名的目录句柄。
Directory make_stage(const Directory &parent, std::string_view uuid) {
    const auto path = parent.path / (".markdownshelf-stage-" + std::string(uuid));
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
        file_error(GetLastError(), "library.create_stage");
    }
    try {
        auto handle = open_handle(path, DELETE | FILE_READ_ATTRIBUTES, share_read_write,
                                  OPEN_EXISTING, exact_entry, "library.open_stage");
        return identify_directory(std::move(handle));
    } catch (const LibraryError &error) {
        // 无法确认归属时保留现场，并把候选位置交给调用方。
        throw LibraryError(error.code(), error.message_key(), display_path(path));
    }
}

} // namespace

// 关闭已接管的 Windows 文件或目录对象。
void CloseHandle::operator()(void *handle) const noexcept {
    ::CloseHandle(handle);
}

// 转换单个 UTF-8 目录名，拒绝控制字符和具有特殊路径语义的名称。
std::wstring directory_name(std::string_view name) {
    if (!valid_utf8(name) || name.size() > 4 * 255) {
        throw LibraryError(Code::invalid_input, "library.invalid_name");
    }
    const auto bytes = static_cast<int>(name.size());
    const auto units =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), bytes, nullptr, 0);
    if (units <= 0 || units > 255) {
        throw LibraryError(Code::invalid_input, "library.invalid_name");
    }
    std::wstring value(static_cast<std::size_t>(units), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), bytes, value.data(),
                             units) ||
        value.back() == L' ' || value.back() == L'.' ||
        value.find_first_of(L"<>:\"/\\|?*") != value.npos ||
        std::any_of(value.begin(), value.end(),
                    // Windows 文件名排除 U+0001 至 U+001F 的控制字符。
                    [](wchar_t ch) { return ch < 32; }) ||
        reserved_name(value)) {
        throw LibraryError(Code::invalid_input, "library.invalid_name");
    }
    return value;
}

// 用严格的 Win32 UTF-8 转换验证字节序列，保留现有文本的原始字节。
bool valid_utf8(std::string_view value) noexcept {
    return !value.empty() && value.size() <= static_cast<std::size_t>(INT_MAX) &&
           value.find('\0') == value.npos &&
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

// 直接格式化 Windows GUID 的固定字段，返回独立于系统区域的 ASCII UUID。
std::string new_uuid() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        return {};
    }
    std::array<char, 37> uuid{};
    snprintf(uuid.data(), uuid.size(), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             guid.Data1, static_cast<unsigned>(guid.Data2), static_cast<unsigned>(guid.Data3),
             static_cast<unsigned>(guid.Data4[0]), static_cast<unsigned>(guid.Data4[1]),
             static_cast<unsigned>(guid.Data4[2]), static_cast<unsigned>(guid.Data4[3]),
             static_cast<unsigned>(guid.Data4[4]), static_cast<unsigned>(guid.Data4[5]),
             static_cast<unsigned>(guid.Data4[6]), static_cast<unsigned>(guid.Data4[7]));
    return uuid.data();
}

// 转换为 Windows FILETIME，再按公历 UTC 输出固定精度的 ISO 8601 日期。
std::string utc_text(std::int64_t epoch_ms) {
    if (epoch_ms < 0 || epoch_ms > 253402300799999LL) {
        return {};
    }
    const auto ticks = (static_cast<std::uint64_t>(epoch_ms) + 11644473600000ULL) * 10000;
    const FILETIME time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&time, &utc)) {
        return {};
    }
    std::array<char, 25> text{};
    const int length =
        snprintf(text.data(), text.size(), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                 static_cast<unsigned>(utc.wYear), static_cast<unsigned>(utc.wMonth),
                 static_cast<unsigned>(utc.wDay), static_cast<unsigned>(utc.wHour),
                 static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond),
                 static_cast<unsigned>(utc.wMilliseconds));
    return length == 24 ? std::string(text.data(), 24) : std::string{};
}

// 为普通路径去掉 Win32 扩展前缀，超长路径保留它以便调用方再次传入。
fs::path display_path(const fs::path &path) {
    const auto &text = path.native();
    if (text.size() < MAX_PATH) {
        if (text.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
            return L"\\\\" + text.substr(8);
        }
        if (text.compare(0, 4, L"\\\\?\\") == 0) {
            return text.substr(4);
        }
    }
    return path;
}

// 解析调用者传入的别名，之后使用句柄确定的路径访问受管条目。
Directory resolve_directory(const fs::path &input) {
    // 目录读取访问与不共享删除一起守护目录，单纯打开属性无法提供同样的共享约束。
    return identify_directory(open_handle(extended_path(input), FILE_LIST_DIRECTORY,
                                          share_read_write, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS, "library.directory_missing"));
}

// 资产目录须为库根内的直接子目录。
Handle open_assets(const Directory &root) {
    auto assets = open_handle(
        root.path / L"assets", FILE_LIST_DIRECTORY, share_read_write, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, "library.assets_missing");
    check_entry(assets.get(), true, "library.invalid_assets");
    return assets;
}

// 检查 SQLite 主文件及已存在的辅助条目，防止跟随受管路径外的链接。
Handle guard_database(const Directory &root) {
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    auto database = open_handle(root.path / L"library.db", FILE_READ_DATA, share_read_write,
                                OPEN_EXISTING, flags, "library.database_missing");
    check_entry(database.get(), false, "library.invalid_database");
    for (const auto name : sidecars) {
        auto entry = open_handle(root.path / name, FILE_READ_ATTRIBUTES,
                                 share_read_write | FILE_SHARE_DELETE, OPEN_EXISTING, flags,
                                 "library.invalid_database", true);
        if (entry) {
            check_entry(entry.get(), false, "library.invalid_database");
        }
    }
    return database;
}

// 用目录的实际 ACL 检查写权限，不生成探测文件。
void require_directory_access(const Directory &root, DWORD access) {
    auto check = open_handle(root.path, access, share_read_write | FILE_SHARE_DELETE, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                             "library.directory_not_writable");
    check_entry(check.get(), true, "library.invalid_directory");
}

// 在初始化之前检查目标，暂移时随后复核类型和内容。
void check_create_target(const fs::path &target) {
    const auto entry = open_handle(target, FILE_LIST_DIRECTORY, share_read_write, OPEN_EXISTING,
                                   exact_entry, "library.open_target", true);
    if (entry) {
        check_entry(entry.get(), true, "library.invalid_target");
        if (!directory_is_empty(target)) {
            throw LibraryError(Code::invalid_input, "library.target_not_empty");
        }
    }
}

// 以无共享模式打开稳定锁文件；关闭句柄释放独占，保留文件名供下次使用。
Handle lock_library_path(const Directory &root) {
    const auto parent = root.path.parent_path();
    if (parent.empty() || root.path.filename().empty()) {
        throw LibraryError(Code::invalid_input, "library.invalid_path");
    }
    auto lock =
        open_handle(parent / lock_filename(root.path.filename()), GENERIC_READ | GENERIC_WRITE, 0,
                    OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, "library.lock_file");
    check_entry(lock.get(), false, "library.invalid_lock_file");
    return lock;
}

// 比较卷和固定长度的文件标识，避免依赖输入路径或大小写。
bool same_directory(const Directory &first, const Directory &second) noexcept {
    return same_identity(first.identity, second.identity);
}

// 在固定上限内读取清单原始字节，清单句柄打开期间阻止并行修改。
std::string read_manifest_file(const Directory &root) {
    auto file = open_handle(
        root.path / L"library.json", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, "library.manifest_missing");
    check_entry(file.get(), false, "library.invalid_manifest_file");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        file_error(GetLastError(), "library.read_manifest");
    }
    if (size.QuadPart <= 0 || size.QuadPart > manifest_limit) {
        throw LibraryError(Code::unsupported_format, "library.invalid_manifest");
    }
    std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD count = 0;
    if (!ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &count, nullptr)) {
        file_error(GetLastError(), "library.read_manifest");
    }
    if (count != data.size()) {
        throw LibraryError(Code::io_error, "library.read_manifest");
    }
    return data;
}

// 先登记清单身份供写入失败时清理，返回可以阻止条目删除的只读句柄。
Handle StagedDirectory::write_manifest(std::string_view contents) {
    const auto path = root_.path / L"library.json";
    auto file = open_handle(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_NEW,
                            FILE_FLAG_OPEN_REPARSE_POINT, "library.create_manifest");
    manifest_id_ = identity_of(file.get());
    DWORD count = 0;
    if (!WriteFile(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &count,
                   nullptr)) {
        file_error(GetLastError(), "library.write_manifest");
    }
    if (count != contents.size()) {
        throw LibraryError(Code::io_error, "library.write_manifest");
    }
    if (!FlushFileBuffers(file.get())) {
        file_error(GetLastError(), "library.flush_manifest");
    }
    // 在原写句柄释放前接管守护，随后只读检查才能按正常共享模式打开文件。
    return open_handle(path, FILE_READ_DATA, share_read_write, OPEN_EXISTING, exact_entry,
                       "library.read_manifest");
}

// 在已归属本次操作的暂存目录中创建资源子目录。
Handle StagedDirectory::create_assets() {
    if (!CreateDirectoryW((root_.path / L"assets").c_str(), nullptr)) {
        file_error(GetLastError(), "library.create_assets");
    }
    auto assets = open_assets(root_);
    assets_id_ = identity_of(assets.get());
    return assets;
}

// 以 CREATE_NEW 取得新的数据库子项，阻止打开其他调用预先放入的文件。
Handle StagedDirectory::create_database() {
    auto database =
        open_handle(root_.path / L"library.db", FILE_READ_DATA, share_read_write, CREATE_NEW,
                    FILE_FLAG_OPEN_REPARSE_POINT, "library.create_database");
    database_id_ = identity_of(database.get());
    return database;
}

// 持有父目录和新建暂存根，所有后续子路径使用最终句柄路径。
StagedDirectory::StagedDirectory(const Directory &parent, std::string_view uuid)
    : parent_(parent), root_(make_stage(parent, uuid)) {}

// 生命周期结束时清理尚未发布的目录。
StagedDirectory::~StagedDirectory() {
    discard();
}

// 按已记录的物理身份清理生成项，不能安全删除时保留目录并返回失败。
bool StagedDirectory::discard() noexcept {
    if (published_ || !root_.handle) {
        return true;
    }
    try {
        delete_created_entry(root_.path / L"library.json", manifest_id_);
        delete_created_entry(root_.path / L"library.db", database_id_);
        delete_created_entry(root_.path / L"assets", assets_id_);
        if (delete_on_close(root_.handle.get())) {
            root_.handle.reset();
            return true;
        }
    } catch (...) {
        // 清理失败时保留现场，避免从析构函数抛出。
    }
    return false;
}

// 返回正在构建的库目录，调用方的临时连接须在发布前销毁。
const Directory &StagedDirectory::root() const noexcept {
    return root_;
}

// 打开现有目标自身并核对为空，按该句柄暂移后再次核对目录内容。
void StagedDirectory::prepare_target(const fs::path &target, std::string_view uuid) {
    auto original = open_handle(
        target, DELETE | FILE_READ_ATTRIBUTES, share_read_write, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, "library.open_target", true);
    if (!original) {
        return;
    }
    check_entry(original.get(), true, "library.invalid_target");
    if (!directory_is_empty(target)) {
        throw LibraryError(Code::invalid_input, "library.target_not_empty");
    }
    previous_path_ = parent_.path / (".markdownshelf-original-" + std::string(uuid));
    if (const auto error = rename_directory(original.get(), previous_path_)) {
        file_error(error, "library.park_target");
    }
    previous_ = std::move(original);
    if (!directory_is_empty(previous_path_)) {
        throw LibraryError(Code::invalid_input, "library.target_not_empty");
    }
}

// 完成本次发布后仅按句柄删除确认为空的原目录，无法删除时提供保留路径。
std::optional<fs::path> StagedDirectory::publish(const fs::path &target) {
    auto retained = previous_ ? std::optional(display_path(previous_path_)) : std::nullopt;
    if (const auto error = rename_directory(root_.handle.get(), target)) {
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
            throw LibraryError(Code::invalid_input, "library.target_exists");
        }
        file_error(error, "library.publish_failed");
    }
    published_ = true;
    if (previous_ && !delete_on_close(previous_.get())) {
        return retained;
    }
    return std::nullopt;
}

// 回滚只移动暂移的原对象，原路径已有外部条目时保留两者。
void StagedDirectory::restore_target(const fs::path &target) {
    if (!published_ && previous_ && rename_directory(previous_.get(), target) != ERROR_SUCCESS) {
        throw LibraryError(Code::io_error, "library.restore_target_failed",
                           display_path(previous_path_));
    }
}

} // namespace markdownshelf::platform
