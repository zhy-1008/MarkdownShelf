#include "markdownshelf/abi_probe.h"

#include <pybind11/pybind11.h>

namespace py = pybind11;

PYBIND11_MODULE(_markdownshelf_core, module) {
    module.def(
        "probe",
        [](const py::str &text) {
            const auto result = markdownshelf::probe(text.cast<std::string>());
            py::dict value;
            value["contract_version"] = result.contract_version;
            value["text"] = py::str(result.text);
            return value;
        },
        py::arg("text").noconvert());
}
