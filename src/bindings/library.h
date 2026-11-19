#pragma once

#include <pybind11/pybind11.h>

// 在已持有 GIL 的扩展初始化线程上注册库服务及其结构化错误。
void bind_library(pybind11::module_ &module);
