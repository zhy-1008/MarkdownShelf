-- 单例键保证库身份只有一行，空活动槽表示尚未建立全文索引。
CREATE TABLE library_meta (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    library_uuid TEXT NOT NULL,
    display_name TEXT NOT NULL CHECK (length(display_name) > 0),
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    search_index_version INTEGER NOT NULL DEFAULT 0 CHECK (search_index_version >= 0),
    active_fts_slot TEXT CHECK (active_fts_slot IN ('a', 'b')),
    content_epoch INTEGER NOT NULL DEFAULT 0 CHECK (content_epoch >= 0)
) STRICT;
