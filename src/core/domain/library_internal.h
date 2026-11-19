#pragma once

#include "markdownshelf/library.h"

#include <functional>

namespace markdownshelf::detail {

// 定位确定性的创建故障注入点。
enum class CreateStep {
    // 暂存目录已创建，初始化即将开始。
    before_initialize,
    // 暂存库已验证，原目标已按需暂移，即将发布。
    before_publish
};

// 创建实现由入口和集成测试共用。hook 由调用线程同步执行，可以抛出故障。
LibraryInfo
create_library(const std::filesystem::path &parent, std::string_view name,
               const std::function<void(CreateStep, const std::filesystem::path &)> &hook = {});

} // namespace markdownshelf::detail
