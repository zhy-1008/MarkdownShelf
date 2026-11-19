#pragma once

#include <string_view>

// SQLite 定义的分词器回调表类型。
struct fts5_tokenizer;

namespace markdownshelf::db {

// 用于指定注册目标的数据库连接类型。
class Connection;

// 为 connection 注册名为 name 的无状态分词器工厂。每条连接单独注册，名称须唯一。
// SQLite 会复制 tokenizer 的回调表，工厂管理实例。
// 回调用 SQLite 结果码报告失败，异常须在回调内部处理。
void register_fts5_tokenizer(Connection &connection, std::string_view name,
                             const fts5_tokenizer &tokenizer);

} // namespace markdownshelf::db
