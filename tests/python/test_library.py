import json
import re
import shutil
import sqlite3
import stat
import subprocess
import sys
from contextlib import closing
from datetime import datetime
from pathlib import Path
from uuid import UUID

import pytest

import _markdownshelf_core as core


CONTRACT = json.loads(
    (Path(__file__).parents[1] / "contracts" / "library.v1.json").read_text(encoding="utf-8")
)


# 校验版本化错误示例，操作 ID 由核心为每次失败生成。
def assert_contract_error(error, fixture):
    expected = CONTRACT["errors"][fixture]
    assert {key: getattr(error, key) for key in expected} == expected
    assert str(UUID(error.operation_id)) == error.operation_id
    assert error.recovery_path is None


# 真实绑定创建并打开库，从清单和独立 SQLite 连接交叉检查持久化信息。
def test_create_inspect_and_open_folder(tmp_path):
    name = CONTRACT["library"]["display_name"]
    created = core.Library.create(tmp_path, name)
    assert isinstance(created, core.LibraryInfo)
    assert isinstance(created.root, Path)
    assert created.root == tmp_path / name
    assert {item.name for item in created.root.iterdir()} == {"library.db", "library.json", "assets"}
    assert not list((created.root / "assets").iterdir())
    manifest_path = created.root / "library.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest == {
        **CONTRACT["manifest"],
        "library_uuid": created.library_uuid,
        "created_utc": created.created_utc,
    }
    assert str(UUID(created.library_uuid)) == created.library_uuid
    assert int(datetime.fromisoformat(created.created_utc).timestamp() * 1000 + 0.5) == created.created_at
    with closing(sqlite3.connect(created.root / "library.db")) as reader:
        assert reader.execute("PRAGMA application_id").fetchone() == (CONTRACT["library"]["application_id"],)
        assert reader.execute("PRAGMA user_version").fetchone() == (created.schema_version,)
        assert reader.execute("SELECT library_uuid, display_name, created_at FROM library_meta").fetchall() == [
            (created.library_uuid, name, created.created_at)
        ]
    assert created.schema_version == CONTRACT["library"]["schema_version"]
    assert created.format_version == CONTRACT["manifest"]["format_version"]
    checked = core.Library.inspect(created.root)
    assert isinstance(checked, core.LibraryInspection)
    assert checked.info.display_name == name
    manifest_bytes = manifest_path.read_bytes()
    lease = core.LibraryLease.acquire(created.root)
    assert lease.valid
    session = core.Library.open(created.root, lease)
    assert not lease.valid and session.is_open
    assert session.info.library_uuid == checked.info.library_uuid
    session.close()
    session.close()
    assert not session.is_open
    assert manifest_path.read_bytes() == manifest_bytes
    with closing(sqlite3.connect(created.root / "library.db")) as reader:
        assert reader.execute("PRAGMA journal_mode").fetchone() == ("wal",)


# 通过 Windows 实际创建接口拒绝路径穿越、设备名和不合法的目录名。
@pytest.mark.parametrize("name", ["", "../旁路", "a\\b", "CON.txt", "COM¹.log", "字. ", "a\x00b", "a:b"])
def test_invalid_names_are_rejected_without_creating_files(tmp_path, name):
    with pytest.raises(core.LibraryError) as failure:
        core.Library.create(tmp_path, name)
    assert_contract_error(failure.value, "invalid_name")
    assert list(tmp_path.iterdir()) == []


# 用独立库覆盖缺失、错误类型、应用标识和 DB 内容错误并确认原文件内容。
@pytest.mark.parametrize(
    "problem, expected",
    [
        ("missing_assets", "NotFound"),
        ("asset_file", "IntegrityFailed"),
        ("wrong_application", "IntegrityFailed"),
        ("uuid_mismatch", "IntegrityFailed"),
        ("time_mismatch", "IntegrityFailed"),
        ("bad_db", "IntegrityFailed"),
    ],
)
def test_inspection_rejects_bad_structure(tmp_path, problem, expected):
    root = core.Library.create(tmp_path, "Broken").root
    db = root / "library.db"
    manifest = root / "library.json"
    if problem.startswith(("wrong", "uuid", "time")):
        mutation = {
            "wrong_application": "PRAGMA application_id=10",
            "uuid_mismatch": "UPDATE library_meta SET library_uuid='11111111-1111-1111-1111-111111111111'",
            "time_mismatch": "UPDATE library_meta SET created_at=created_at+1",
        }[problem]
        with closing(sqlite3.connect(db)) as writer, writer:
            writer.execute(mutation)
    elif problem == "bad_db":
        db.write_bytes(b"not sqlite")
    else:
        (root / "assets").rmdir()
        if problem == "asset_file":
            (root / "assets").write_bytes(b"keep")
    before = (db.read_bytes(), manifest.read_bytes())
    with pytest.raises(core.LibraryError) as error:
        core.Library.inspect(root)
    assert error.value.code == expected
    assert (db.read_bytes(), manifest.read_bytes()) == before


# 通过真实解析器检查非法 UTF-8、缺失字段、重复字段和格式版本。
@pytest.mark.parametrize("problem", ["encoding", "missing", "duplicate", "noninteger", "future_format"])
def test_inspection_rejects_bad_manifest(tmp_path, problem):
    root = core.Library.create(tmp_path, "Bad manifest").root
    path = root / "library.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    if problem == "missing":
        del data["created_utc"]
    if problem == "noninteger":
        data["format_version"] = "1"
    if problem == "future_format":
        data["format_version"] = 2
    raw = json.dumps(data).encode("utf-8")
    if problem == "encoding":
        raw = raw[:-1] + b', "comment": "\xff"}'
    if problem == "duplicate":
        raw = raw[:-1] + b', "format_version": 1}'
    path.write_bytes(raw)
    with pytest.raises(core.LibraryError) as failure:
        core.Library.inspect(root)
    if problem == "future_format":
        assert failure.value.code == "UnsupportedFormat"
        assert failure.value.message_key == "library.unsupported_format"
    else:
        assert_contract_error(failure.value, "invalid_manifest")
    assert path.read_bytes() == raw


# 在取得租约后升级 SQLite 版本，验证 open 重检、释放和字节不变。
def test_open_refuses_future_schema_before_enable_wal(tmp_path):
    root = core.Library.create(tmp_path, "Future").root
    lease = core.LibraryLease.acquire(root)
    db = root / "library.db"
    with closing(sqlite3.connect(db)) as writer:
        writer.execute("PRAGMA user_version=2")
    before = db.read_bytes()
    for operation in (core.Library.inspect, core.LibraryLease.acquire):
        with pytest.raises(core.LibraryError) as failure:
            operation(root)
        assert_contract_error(failure.value, "future_schema")
    with pytest.raises(core.LibraryError) as failure:
        core.Library.open(root, lease)
    assert_contract_error(failure.value, "future_schema")
    assert not lease.valid
    assert db.read_bytes() == before
    assert not (root / "library.db-wal").exists()
    assert not (root / "library.db-shm").exists()
    with closing(sqlite3.connect(db)) as writer:
        writer.execute("PRAGMA user_version=1")
    assert core.LibraryLease.acquire(root).valid


# 破坏与元数据无关的表根页，验证写前额外快速检查。
def test_open_checks_sqlite_beyond_metadata_before_writing(tmp_path):
    root = core.Library.create(tmp_path, "Integrity").root
    db = root / "library.db"
    with closing(sqlite3.connect(db)) as writer, writer:
        writer.execute("CREATE TABLE sample(value INTEGER)")
        writer.execute("INSERT INTO sample VALUES(1)")
        page_size = writer.execute("PRAGMA page_size").fetchone()[0]
        root_page = writer.execute("SELECT rootpage FROM sqlite_schema WHERE name='sample'").fetchone()[0]
    with db.open("r+b") as contents:
        contents.seek((root_page - 1) * page_size)
        assert contents.read(1) == b"\x0d"
        contents.seek(-1, 1)
        contents.write(b"\0")
    before = db.read_bytes()
    assert core.Library.inspect(root).info.display_name == "Integrity"
    lease = core.LibraryLease.acquire(root)
    with pytest.raises(core.LibraryError) as failure:
        core.Library.open(root, lease)
    assert failure.value.code == "IntegrityFailed"
    assert failure.value.message_key == "library.quick_check_failed"
    assert db.read_bytes() == before
    assert not lease.valid
    assert core.LibraryLease.acquire(root).valid


# 用只读文件属性让真实 Windows SQLite 写连接失败，检查租约释放和数据不变。
def test_open_rejects_read_only_database(tmp_path):
    root = core.Library.create(tmp_path, "Read Only").root
    db = root / "library.db"
    before = db.read_bytes()
    db.chmod(stat.S_IREAD)
    try:
        lease = core.LibraryLease.acquire(root)
        with pytest.raises(core.LibraryError) as failure:
            core.Library.open(root, lease)
        assert_contract_error(failure.value, "permission_denied")
        assert not lease.valid
        assert db.read_bytes() == before
    finally:
        db.chmod(stat.S_IWRITE)
    assert core.LibraryLease.acquire(root).valid


# 检查已关闭 WAL 库时记录辅助文件存在情况，主文件及清单内容保持一致。
def test_inspection_preserves_contents_of_wal_library(tmp_path, record_property):
    root = core.Library.create(tmp_path, "Existing WAL").root
    session = core.Library.open(root, core.LibraryLease.acquire(root))
    session.close()
    managed = [root / "library.db", root / "library.json"]
    before = [path.read_bytes() for path in managed]
    names_before = sorted(path.name for path in root.iterdir())
    assert core.Library.inspect(root).info.display_name == "Existing WAL"
    assert [path.read_bytes() for path in managed] == before
    names_after = sorted(path.name for path in root.iterdir())
    record_property("wal_directory_before_inspect", ",".join(names_before))
    record_property("wal_directory_after_inspect", ",".join(names_after))
    assert set(names_after) - set(names_before) <= {"library.db-wal", "library.db-shm"}


# 对隔离临时目录设置当前用户的 Windows DACL 拒绝项，用完立即恢复。
def test_create_respects_real_windows_directory_permissions(tmp_path):
    identity = subprocess.run(["whoami", "/user"], capture_output=True, check=True).stdout
    sid = "*" + re.search(rb"S-1-\d+(?:-\d+)+", identity).group().decode("ascii")
    subprocess.run(["icacls", str(tmp_path), "/deny", sid + ":(AD)"], capture_output=True, check=True)
    try:
        with pytest.raises(core.LibraryError) as failure:
            core.Library.create(tmp_path, "Permission")
        assert_contract_error(failure.value, "permission_denied")
        assert list(tmp_path.iterdir()) == []
    finally:
        subprocess.run(["icacls", str(tmp_path), "/remove:d", sid], capture_output=True, check=True)


# 使用真实卷的扩展路径，验证主库、资源目录及 WAL 能跨越传统 260 字符边界。
def test_supports_long_native_paths(tmp_path):
    parent = Path("\\\\?\\" + str(tmp_path))
    while len(str(parent)) < 270:
        parent /= "技术资料 folder " + "x" * 18
        parent.mkdir()
    created = core.Library.create(parent, "最终 库")
    assert len(str(created.root)) > 260
    assert created.root == parent / "最终 库"
    lease = core.LibraryLease.acquire(created.root)
    session = core.Library.open(created.root, lease)
    session.close()
    assert core.Library.inspect(created.root).info.library_uuid == created.library_uuid


# 真实目录联接别名应解析至原物理库，资源目录联接则拒绝跟随。
def test_resolves_root_junction_and_rejects_assets_junction(tmp_path):
    created = core.Library.create(tmp_path, "physical")
    alias = tmp_path / "alias"
    subprocess.run(
        ["cmd.exe", "/d", "/c", "mklink", "/J", str(alias), str(created.root)],
        capture_output=True, check=True,
    )
    assert core.Library.inspect(alias).info.root == created.root
    session = core.Library.open(alias, core.LibraryLease.acquire(created.root))
    with pytest.raises(core.LibraryError) as busy:
        core.LibraryLease.acquire(alias)
    assert_contract_error(busy.value, "busy")
    session.close()
    elsewhere = tmp_path / "other assets"
    elsewhere.mkdir()
    (elsewhere / "keep").write_bytes(b"user data")
    assets = created.root / "assets"
    assets.rmdir()
    subprocess.run(
        ["cmd.exe", "/d", "/c", "mklink", "/J", str(assets), str(elsewhere)],
        capture_output=True, check=True,
    )
    with pytest.raises(core.LibraryError) as failure:
        core.Library.inspect(alias)
    assert failure.value.code == "IntegrityFailed"
    assert (elsewhere / "keep").read_bytes() == b"user data"


# 由独立解释器实测活动会话的路径锁，并验证不同库可同时打开。
def test_real_second_process_sees_busy_and_can_reopen(tmp_path):
    created = core.Library.create(tmp_path, "Busy")
    other = core.Library.create(tmp_path, "other")
    child = """
from pathlib import Path
import sys
import _markdownshelf_core as core

root, mode = Path(sys.argv[1]), sys.argv[2]
try:
    session = core.Library.open(root, core.LibraryLease.acquire(root))
except core.LibraryError as error:
    assert mode == 'busy' and error.code == 'LibraryBusy' and error.retryable
else:
    assert mode == 'open' and session.is_open
    session.close()
"""
    lease = core.LibraryLease.acquire(created.root)
    session = core.Library.open(created.root, lease)
    with pytest.raises(core.LibraryError) as failure:
        core.LibraryLease.acquire(created.root)
    assert_contract_error(failure.value, "busy")
    with pytest.raises(core.LibraryError) as failure:
        core.Library.open(created.root, lease)
    assert_contract_error(failure.value, "invalid_lease")
    subprocess.run([sys.executable, "-c", child, str(created.root), "busy"], check=True, timeout=30)
    subprocess.run([sys.executable, "-c", child, str(other.root), "open"], check=True, timeout=30)
    session.close()
    subprocess.run([sys.executable, "-c", child, str(created.root), "open"], check=True, timeout=30)


# 在没有显式 close 的子进程中终止解释器，核对系统会回收文件句柄。
def test_process_exit_releases_lease(tmp_path):
    root = core.Library.create(tmp_path, "After exit").root
    child = """
from pathlib import Path
import os
import sys
import _markdownshelf_core as core

root = Path(sys.argv[1])
session = core.Library.open(root, core.LibraryLease.acquire(root))
assert session.is_open
os._exit(0)
"""
    subprocess.run([sys.executable, "-c", child, str(root)], check=True, timeout=30)
    assert core.LibraryLease.acquire(root).valid


# 证明租约存续期间库根无法改名，释放后可移动并按新路径重新打开。
def test_lease_keeps_root_in_place_until_release(tmp_path):
    root = core.Library.create(tmp_path, "Pinned").root
    moved = tmp_path / "Moved"
    lease = core.LibraryLease.acquire(root)
    with pytest.raises(OSError) as failure:
        root.rename(moved)
    assert failure.value.winerror in (5, 32)
    session = core.Library.open(root, lease)
    session.close()
    root.rename(moved)
    session = core.Library.open(moved, core.LibraryLease.acquire(moved))
    session.close()


# 复制完整文件夹到另一个实际路径，保留 UUID、创建时间和库内资产。
def test_copy_preserves_identity(tmp_path):
    original = core.Library.create(tmp_path, "Source")
    (original.root / "assets" / "kept.txt").write_bytes(b"physical asset")
    copy = shutil.copytree(original.root, tmp_path / "Destination")
    info = core.Library.inspect(copy).info
    assert (info.library_uuid, info.created_at) == (original.library_uuid, original.created_at)
    assert (copy / "assets" / "kept.txt").read_bytes() == b"physical asset"
    session = core.Library.open(copy, core.LibraryLease.acquire(copy))
    assert session.info.root == copy
    session.close()
