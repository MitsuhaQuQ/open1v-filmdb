#pragma once

#include "filmrecorder/film_records.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace filmrecorder {

struct SaveResult {
    std::int64_t importId{};
    std::size_t rolls{};
    std::size_t frames{};
};

struct RollListItem {
    std::int64_t rollId{};
    std::int64_t importId{};
    std::string importTime;
    std::string filmId;
    std::int64_t frameCount{};
};

SaveResult saveToDatabase(const std::filesystem::path& path,
                          const Download& download,
                          const char* sourceType);
std::string inspectDatabase(const std::filesystem::path& path);
std::vector<std::string> listImportDates(const std::filesystem::path& path,
                                         const std::string& yearMonth);
std::vector<RollListItem> listRollsForDate(const std::filesystem::path& path,
                                           const std::string& date);
std::string describeRoll(const std::filesystem::path& path,
                         std::int64_t rollId);

} // namespace filmrecorder
