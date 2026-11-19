#include "markdownshelf/fts5.h"

#include "sqlite_internal.h"

namespace markdownshelf::db {

// 获取当前连接的 FTS5 API，检查重名后注册三项回调，失败时报告参数或 SQLite 错误。
void register_fts5_tokenizer(Connection &connection, std::string_view name,
                             const fts5_tokenizer &tokenizer) {
    if (name.empty() || name.find('\0') != std::string_view::npos || !tokenizer.xCreate ||
        !tokenizer.xDelete || !tokenizer.xTokenize) {
        throw std::invalid_argument("FTS5 requires a name and three tokenizer callbacks");
    }
    const std::string registered_name(name);

    fts5_api *api = nullptr;
    {
        auto query = connection.prepare("SELECT fts5(?1)");
        check_sqlite(
            sqlite3_bind_pointer(SqliteAccess::handle(query), 1, &api, "fts5_api_ptr", nullptr),
            "Bind FTS5 API pointer");
        if (!query.step() || !api) {
            throw std::runtime_error("SQLite did not return its FTS5 API");
        }
        query.step();
    }

    void *context = nullptr;
    fts5_tokenizer existing{};
    const int lookup = api->xFindTokenizer(api, registered_name.c_str(), &context, &existing);
    if (lookup == SQLITE_OK) {
        throw std::invalid_argument("FTS5 tokenizer name is already registered");
    }
    if (lookup != SQLITE_ERROR) {
        check_sqlite(lookup, "Find FTS5 tokenizer");
    }

    auto callbacks = tokenizer;
    check_sqlite(api->xCreateTokenizer(api, registered_name.c_str(), nullptr, &callbacks, nullptr),
                 "Register FTS5 tokenizer");
}

} // namespace markdownshelf::db
