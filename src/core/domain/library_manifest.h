#pragma once

#include "markdownshelf/library.h"

namespace markdownshelf::detail {

// 标识第一版库文件夹格式，独立于 SQLite schema。
inline constexpr int library_format_version = 1;

// 保存经过解析的清单身份，自有字段可随检查结果移动。
struct LibraryManifest {
    // 保存校验并规范化的小写库 UUID。
    std::string library_uuid;
    // 保存清单使用的 UTC 毫秒文本。
    std::string created_utc;
};

// 验证 UUID 的标准文本形式并转为小写，非法或全零 UUID 返回空字符串。
std::string canonical_uuid(std::string_view value);
// 验证并解析 UTF-8 清单，格式错误抛出 UnsupportedFormat。
LibraryManifest parse_manifest(std::string_view text);
// 将已验证的身份编码为 UTF-8 JSON。
std::string encode_manifest(const LibraryManifest &manifest);

} // namespace markdownshelf::detail
