#include "markdownshelf/abi_probe.h"

#include "library.h"

#include <pybind11/pybind11.h>

namespace py = pybind11;

// Python 导入扩展时注册 ABI 探针与文件夹库服务。
PYBIND11_MODULE(_markdownshelf_core, module) {
    bind_library(module);
    module.def(
        "probe",
        // 将 Python 字符串转换为核心的 UTF-8 输入，并把探针结果映射为 Python 字典。
        [](const py::str &text) {
            const auto result = markdownshelf::probe(text.cast<std::string>());
            py::dict value;
            value["contract_version"] = result.contract_version;
            value["text"] = py::str(result.text);
            return value;
        },
        py::arg("text").noconvert());
}
