# MarkdownShelf

面向技术笔记的 Windows 离线 Markdown 文档库。功能进度见[当前任务](docs/project/CURRENT_TASK.md)，正式需求见 [V4.0 设计规格](docs/specification/MarkdownShelf_Project_Design_v4.0.md)。

## 构建与测试

在 VS Code 的 Windows x64 Developer PowerShell 中运行。需要 MSVC v143、Windows SDK、CMake 3.25 或更高版本、Ninja，以及带开发头文件和导入库的 CPython 3.12 x64。可在 VS Code 的命令提示符终端中运行 Visual Studio 安装目录下的 `VC\Auxiliary\Build\vcvarsall.bat x64 -vcvars_ver=14.44`，再启动 PowerShell 继承该环境。将下列版本核对和创建虚拟环境所用的 `python` 指向已核对的 CPython 3.12 x64。逐条执行，遇到失败先检查错误输出。

```powershell
where.exe cl
$env:VCToolsVersion
$env:WindowsSDKVersion
cmake --version
ninja --version
python -c "import struct, sys; assert sys.implementation.name == 'cpython' and sys.version_info[:2] == (3, 12) and struct.calcsize('P') == 8; print(sys.version, sys.executable)"
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --require-hashes --only-binary=:all: -r requirements-dev.lock

cmake --preset win-x64-debug "-DPython_EXECUTABLE=$((Get-Command python).Source)"
cmake --build --preset win-x64-debug
ctest --preset win-x64-debug
cmake --install build/win-x64-debug --config Debug --prefix .stage/debug
$env:PYTHONPATH = "$PWD\.stage\debug;$PWD\src\python"
python -c "from pathlib import Path; import _markdownshelf_core as m; p = Path(m.__file__).resolve(); assert p.suffix == '.pyd' and p.parent == Path('.stage/debug').resolve(); print(p)"
python -m pytest tests/python
```

Release 使用 `win-x64-release`、`--config Release` 和 `.stage/release`，并同步更新 `PYTHONPATH` 和导入断言中的目录。Debug 与 Release 各自从对应的暂存根加载。

仅验证 C++ 核心时，执行以下命令。

```powershell
cmake --preset win-x64-debug -B build/win-x64-core-only -DMARKDOWNSHELF_BUILD_BINDINGS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Python=ON
cmake --build build/win-x64-core-only
ctest --test-dir build/win-x64-core-only --no-tests=error --output-on-failure
```

CI 配置时加 `-DMARKDOWNSHELF_STRICT=ON` 对一方目标启用 `/WX /analyze`。格式检查使用锁文件中的 clang-format 20.1.8。

```powershell
$files = (Get-ChildItem src/core, src/bindings, tests/cpp -Recurse -File -Include *.h, *.cpp).FullName
clang-format --dry-run --Werror @files
```

## VS Code 与 clangd

用 VS Code 打开仓库根目录，启用 clangd 扩展。上面的 `cmake --preset win-x64-debug` 在配置时生成 `build/win-x64-debug/compile_commands.json`，clangd 随后即可使用真实编译参数。Release 预设也生成自己的数据库，工作区默认使用 Debug。

首次配置或更换 MSVC、Windows SDK、Python 路径后，在工具链环境已初始化的 Developer PowerShell 中重新运行 Debug 配置。如果 clangd 输出仍显示 `--compile-commands-dir=…/build` 或 `clangd fallback`，在 VS Code 命令面板执行 `clangd: Restart language server`，然后核对输出中的 `Loaded compilation database` 是否指向 `build/win-x64-debug/compile_commands.json`。

## 使用依赖缓存

联网时，使用 CPython 3.12 x64 执行 `python -m pip download --require-hashes --only-binary=:all: -r requirements-dev.lock --dest build/deps/wheels` 准备 wheel。按构建配置中的 URL 和 SHA-256 准备 `sqlite-amalgamation-3540100.zip`、`pybind11-3.0.1.tar.gz` 和 `googletest-1.16.0.tar.gz`，存入 `build/deps/archives`。SQLite 以 C 源码编入 C++ 静态核心，启用 FTS5。[CMakeLists.txt](CMakeLists.txt)

无网时，用 `python -m pip install --no-index --find-links build/deps/wheels --require-hashes --only-binary=:all: -r requirements-dev.lock` 安装 Python 依赖。首次配置时加 `"-DMARKDOWNSHELF_DEPS_DIR=$((Resolve-Path build/deps/archives).Path)"`，CMake 会从本地归档提取并核对哈希。复验时可用 `-B` 选择新的构建目录，再用 `cmake --build <目录>` 和 `ctest --test-dir <目录>` 指向该目录。使用新的暂存前缀，并同步更新 `PYTHONPATH` 和导入断言。

参见 [P1.1 构建与 ABI 学习文档](docs/development/P1.1_build_and_abi.md)、[P1.1 验证记录](docs/testing/P1.1_build_and_abi.md)、[P1.2 SQLite 学习文档](docs/development/P1.2_sqlite_foundation.md)和 [P1.2 验证记录](docs/testing/P1.2_sqlite_foundation.md)。
