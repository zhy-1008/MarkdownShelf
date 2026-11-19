#pragma once

#include <stdexcept>
#include <string_view>
#include <vector>

namespace markdownshelf::db {

// 迁移接口借用的数据库连接类型。
class Connection;

// 描述一次版本升级。SQL 和数据转换都在调用方提供的暂存库中执行。
struct Migration {
    // 此步骤完成后写入 user_version 的版本号，完整迁移序列从 1 开始。
    int version;
    // 借用的 SQL 脚本文本，调用期间须一直有效。仅有数据转换时可为空。
    std::string_view sql;
    // 可选的 C++ 数据转换函数，在当前连接和同一个事务内同步执行。
    void (*transform)(Connection &) = nullptr;
};

// 表示数据库 schema 高于当前程序支持的版本，供调用方拒绝升级。
class SchemaTooNew : public std::runtime_error {
  public:
    // 使用实际库版本与程序支持的最高版本生成错误描述。
    SchemaTooNew(int actual_version, int supported_version);
};

// 读取主数据库的 user_version。无结果或读取到无效版本时抛出异常。
int schema_version(Connection &connection);

// 调用方持有隔离的数据库，并提供从版本 1 开始的全部迁移步骤。
// SQL 和数据转换在当前连接上同步执行。每一步均为原子操作。
// 失败时抛出异常，暂存库保留上一次提交的版本，可从该版本继续迁移。
void migrate_schema(Connection &connection, const std::vector<Migration> &migrations);

} // namespace markdownshelf::db
