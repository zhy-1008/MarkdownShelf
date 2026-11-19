#include "library_manifest.h"

#include "../db/sqlite_internal.h"
#include "../platform/library_files_win.h"

namespace markdownshelf::detail {
namespace {

// 清单使用的固定文件格式魔数。
constexpr std::string_view format_magic = "MarkdownShelfLibrary";

// 通过稳定消息键报告清单格式错误。
[[noreturn]] void invalid_manifest() {
    throw LibraryError(LibraryErrorCode::unsupported_format, "library.invalid_manifest");
}

// 检查固定结构、毫秒精度和 UTC 后缀，实际日历值在和数据库核对时确认。
bool timestamp_shape(std::string_view text) {
    constexpr std::string_view pattern = "0000-00-00T00:00:00.000Z";
    if (text.size() != pattern.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (pattern[i] == '0' ? text[i] < '0' || text[i] > '9' : text[i] != pattern[i]) {
            return false;
        }
    }
    return true;
}

// 从 json_each 的当前行提取必需文本，拒绝重复字段和非文本类型。
void read_text_field(db::Statement &row, std::optional<std::string> &output) {
    if (output || row.column_text(1) != "text") {
        invalid_manifest();
    }
    output = row.column_text(2);
}

} // namespace

// 核对 UUID 分隔位置和十六进制字符，并将大写字符转换为小写。
std::string canonical_uuid(std::string_view value) {
    if (value.size() != 36 || value == "00000000-0000-0000-0000-000000000000") {
        return {};
    }
    std::string result(value);
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto &ch = result[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != '-') {
                return {};
            }
            continue;
        }
        if (ch >= 'A' && ch <= 'F') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
        if ((ch < '0' || ch > '9') && (ch < 'a' || ch > 'f')) {
            return {};
        }
    }
    return result;
}

// 先验证标准 JSON 对象，再枚举字段以区分缺项、类型错误与重复键。
LibraryManifest parse_manifest(std::string_view text) {
    if (!platform::valid_utf8(text)) {
        invalid_manifest();
    }
    auto connection = db::SqliteAccess::memory_connection();
    {
        auto valid =
            connection.prepare("SELECT CASE WHEN json_valid(?1, 1) THEN json_type(?1) END");
        valid.bind_text(1, text);
        if (!valid.step() || valid.column_text(0) != "object") {
            invalid_manifest();
        }
    }
    auto fields = connection.prepare("SELECT key, type, atom FROM json_each(?)");
    fields.bind_text(1, text);
    std::optional<std::string> format;
    std::optional<std::string> uuid;
    std::optional<std::string> created;
    std::optional<std::int64_t> version;
    while (fields.step()) {
        const auto key = fields.column_text(0);
        if (key == "format") {
            read_text_field(fields, format);
        } else if (key == "library_uuid") {
            read_text_field(fields, uuid);
        } else if (key == "created_utc") {
            read_text_field(fields, created);
        } else if (key == "format_version") {
            if (version || fields.column_text(1) != "integer" ||
                sqlite3_column_type(db::SqliteAccess::handle(fields), 2) != SQLITE_INTEGER) {
                invalid_manifest();
            }
            version = fields.column_int64(2);
        }
    }
    if (!format || !version || !uuid || !created || !timestamp_shape(*created)) {
        invalid_manifest();
    }
    if (*format != format_magic || *version != library_format_version) {
        throw LibraryError(LibraryErrorCode::unsupported_format, "library.unsupported_format");
    }
    auto normalized = canonical_uuid(*uuid);
    if (normalized.empty()) {
        invalid_manifest();
    }
    return {std::move(normalized), std::move(*created)};
}

// 通过绑定参数构造对象，避免为 JSON 字符串实现独立转义规则。
std::string encode_manifest(const LibraryManifest &manifest) {
    auto connection = db::SqliteAccess::memory_connection();
    auto statement = connection.prepare("SELECT json_object('format', ?, 'format_version', ?, "
                                        "'library_uuid', ?, 'created_utc', ?)");
    statement.bind_text(1, format_magic);
    statement.bind_int64(2, library_format_version);
    statement.bind_text(3, manifest.library_uuid);
    statement.bind_text(4, manifest.created_utc);
    if (!statement.step()) {
        invalid_manifest();
    }
    return statement.column_text(0).value() + "\n";
}

} // namespace markdownshelf::detail
