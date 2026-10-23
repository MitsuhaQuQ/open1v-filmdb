#pragma once

#include "filmrecorder/film_records.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace filmrecorder {

struct SaveResult {
    std::int64_t importId{};
    std::size_t rolls{};
    std::size_t frames{};
};

// One row of the roll browser. Values stay raw so a caller can sort or format
// them; only identifiers and dates that the database stores as text stay text.
struct RollListItem {
    std::int64_t rollId{};
    std::int64_t importId{};
    std::string importTime;
    std::string filmId;
    std::int64_t frameCount{};
};

// One row of the frame browser, with the recorded values rather than rendered
// strings. Formatting belongs to the presentation layer.
struct FrameListItem {
    std::int64_t frameId{};
    std::int64_t frameIndex{};
    std::optional<double> shutterSeconds;
    std::optional<std::string> shutterDisplay;
    std::optional<double> apertureF;
    std::optional<std::int64_t> isoValue;
    std::optional<std::int64_t> focalLengthMm;
    std::optional<std::string> capturedAt;
};

// Complete decoded record for one frame, joined with its roll and import.
struct FrameDetail {
    std::string importDate;
    std::string importTime;
    std::int64_t importId{};
    std::int64_t rollId{};
    std::optional<std::string> filmId;
    std::int64_t recordWidth{};
    std::optional<std::int64_t> dxIso;
    std::optional<std::string> loadedAt;
    std::int64_t frameIndex{};
    std::int64_t frameNumber{};
    std::optional<std::int64_t> focalLengthMm;
    std::optional<double> maxApertureF;
    std::optional<double> shutterSeconds;
    std::optional<std::string> shutterDisplay;
    std::optional<double> apertureF;
    std::optional<std::int64_t> manualIso;
    std::optional<double> exposureCompensationEv;
    std::optional<double> flashCompensationEv;
    std::optional<std::string> flashMode;
    std::optional<std::string> meteringMode;
    std::optional<std::string> shootingMode;
    std::optional<std::string> aebPosition;
    std::optional<std::string> filmAdvance;
    std::optional<std::string> afMode;
    bool multipleExposure{};
    std::optional<std::int64_t> bulbTimeUnits;
    std::optional<std::string> capturedAt;
    std::optional<std::string> cfnValues;
    std::optional<std::string> batteryLoadedAt;
};

// Database-wide counts and integrity results. Rendering the report is left to
// the caller.
struct DatabaseStats {
    std::string integrity;
    std::int64_t imports{};
    std::int64_t rolls{};
    std::int64_t frames{};
    std::int64_t invalidImportDateTime{};
    std::int64_t framesWithCfn{};
    std::int64_t invalidCfn{};
    std::int64_t splitRawColumns{};
};

SaveResult saveToDatabase(const std::filesystem::path& path,
                          const Download& download,
                          const char* sourceType);
DatabaseStats inspectDatabase(const std::filesystem::path& path);
std::vector<std::string> listImportDates(const std::filesystem::path& path,
                                         const std::string& yearMonth);
std::vector<RollListItem> listRollsForDate(const std::filesystem::path& path,
                                           const std::string& date);
std::vector<FrameListItem> listFramesForRoll(const std::filesystem::path& path,
                                             std::int64_t rollId);
FrameDetail loadFrameDetail(const std::filesystem::path& path,
                            std::int64_t frameId);

}  // namespace filmrecorder
