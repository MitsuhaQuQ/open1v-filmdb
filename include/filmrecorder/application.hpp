#pragma once

#include "filmrecorder/database.hpp"
#include "open1v/camera_protocol.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace filmrecorder {

enum class ExportFormat { sqlite, json, csv };

struct SyncRequest {
    ExportFormat format{ExportFormat::sqlite};
    std::filesystem::path output;
    std::string sourceType;
    bool keepSession{};
};

struct SyncResult {
    bool cameraEmpty{};
    unsigned reportedRolls{};
    std::size_t rolls{};
    std::size_t frames{};
    std::int64_t importId{};
    std::filesystem::path output;
};

SyncResult syncFilmRecords(open1v::CameraProtocolSession& camera,
                           const SyncRequest& request);
void clearFilmRecords(open1v::CameraProtocolSession& camera, bool keepSession);

struct ShootingDataField {
    const char* name;
    unsigned bytes;
    std::array<std::uint8_t, 8> bits;
};

class ShootingDataSelection {
public:
    static ShootingDataSelection fromPackets(
        const std::vector<open1v::CameraPacket>& packets);
    static const std::array<ShootingDataField, 18>& fields();

    [[nodiscard]] bool enabled(std::size_t index) const;
    [[nodiscard]] bool changed(std::size_t index) const;
    [[nodiscard]] unsigned usedBytes() const;
    [[nodiscard]] unsigned recordWidth() const;
    [[nodiscard]] bool dirty() const noexcept { return mask_ != original_; }
    [[nodiscard]] const std::array<std::uint8_t, 8>& mask() const noexcept {
        return mask_;
    }
    void toggle(std::size_t index);

private:
    explicit ShootingDataSelection(std::array<std::uint8_t, 8> mask)
        : original_(mask), mask_(mask) {}
    static bool enabledIn(const std::array<std::uint8_t, 8>& mask,
                          const ShootingDataField& field);
    std::array<std::uint8_t, 8> original_{};
    std::array<std::uint8_t, 8> mask_{};
};

} // namespace filmrecorder
