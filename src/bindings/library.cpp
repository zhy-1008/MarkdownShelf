#include "library.h"

#include "markdownshelf/library.h"

#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;

namespace {

// 将库异常转换为 Python 实例，保留稳定错误字段与可选恢复路径。
void translate_library_error(std::exception_ptr failure) {
    if (!failure) {
        return;
    }
    try {
        std::rethrow_exception(failure);
    } catch (const markdownshelf::LibraryError &error) {
        const auto type = py::module_::import("_markdownshelf_core").attr("LibraryError");
        const auto value = type(error.message_key());
        value.attr("code") = py::str(markdownshelf::library_error_code_name(error.code()));
        value.attr("message_key") = py::str(error.message_key());
        value.attr("retryable") = py::bool_(error.retryable());
        value.attr("operation_id") = py::str(error.operation_id());
        value.attr("recovery_path") = py::cast(error.recovery_path());
        PyErr_SetObject(type.ptr(), value.ptr());
    }
}

} // namespace

// 暴露值类型及所有权对象，文件系统和 SQLite 操作在释放 GIL 后运行。
void bind_library(py::module_ &module) {
    using namespace markdownshelf;

    py::exception<LibraryError>(module, "LibraryError", PyExc_RuntimeError);
    py::register_local_exception_translator(translate_library_error);

    py::class_<LibraryInfo>(module, "LibraryInfo")
        .def_readonly("root", &LibraryInfo::root)
        .def_readonly("library_uuid", &LibraryInfo::library_uuid)
        .def_readonly("display_name", &LibraryInfo::display_name)
        .def_readonly("created_utc", &LibraryInfo::created_utc)
        .def_readonly("created_at", &LibraryInfo::created_at)
        .def_readonly("format_version", &LibraryInfo::format_version)
        .def_readonly("schema_version", &LibraryInfo::schema_version)
        .def_readonly("retained_directory", &LibraryInfo::retained_directory);

    py::class_<LibraryInspection>(module, "LibraryInspection")
        .def_readonly("info", &LibraryInspection::info);

    py::class_<LibraryLease>(module, "LibraryLease")
        .def_static("acquire", &LibraryLease::acquire, py::arg("root"),
                    py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("valid", &LibraryLease::valid);

    py::class_<LibrarySession>(module, "LibrarySession")
        .def_property_readonly("info", &LibrarySession::info, py::return_value_policy::copy)
        .def_property_readonly("is_open", &LibrarySession::is_open)
        .def("close", &LibrarySession::close, py::call_guard<py::gil_scoped_release>());

    py::class_<Library>(module, "Library")
        .def_static(
            "create",
            // 在持有 GIL 时取出 Python 字符串，再释放 GIL 并把 UTF-8 传给核心。
            [](const std::filesystem::path &parent, const py::str &name) {
                const auto utf8 = name.cast<std::string>();
                const py::gil_scoped_release release;
                return Library::create(parent, utf8);
            },
            py::arg("parent"), py::arg("name").noconvert())
        .def_static("inspect", &Library::inspect, py::arg("root"),
                    py::call_guard<py::gil_scoped_release>())
        .def_static("open", &Library::open, py::arg("root"), py::arg("lock").none(false),
                    py::call_guard<py::gil_scoped_release>());
}
