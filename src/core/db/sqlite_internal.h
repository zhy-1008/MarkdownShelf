#pragma once

#include "markdownshelf/sqlite.h"

#include <sqlite3.h>

namespace markdownshelf::db {

// 核心内部的句柄访问入口。迁移和 FTS5 借用句柄调用原生 API。
struct SqliteAccess {
    // 返回仍可用的连接句柄，句柄生命周期由传入的 connection 管理。
    static sqlite3 *handle(Connection &connection) {
        return connection.handle();
    }

    // 返回用于绑定原生指针的语句句柄，所有权仍属于 statement。
    static sqlite3_stmt *handle(Statement &statement) {
        return statement.handle_.get();
    }
};

// 检查期望返回 SQLITE_OK 的调用，其他结果以操作名和结果码构造 SqliteError。
inline void check_sqlite(int code, std::string_view operation) {
    if (code != SQLITE_OK) {
        throw SqliteError(operation, code);
    }
}

} // namespace markdownshelf::db
