#pragma once

#include "filmrecorder/film_records.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace filmrecorder {

struct SaveResult {
    std::int64_t importId{};
    std::size_t rolls{};
    std::size_t frames{};
};

SaveResult saveToDatabase(const std::filesystem::path& path,
                          const Download& download,
                          const char* sourceType);
std::string inspectDatabase(const std::filesystem::path& path);

} // namespace filmrecorder
