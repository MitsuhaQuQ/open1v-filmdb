#pragma once

#include "open1v/camera_protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace filmrecorder {

struct FilmFrame {
    std::uint8_t number{};
    std::uint16_t focalLengthMm{};
    std::uint8_t maxApertureWire{};
    std::uint8_t shutterWire{};
    std::uint8_t apertureWire{};
    std::uint8_t isoSourceWire{};
    std::int8_t exposureCompensationWire{};
    std::int8_t flashCompensationWire{};
    std::uint8_t flashModeWire{};
    std::uint8_t meteringWire{};
    std::uint8_t shootingModeWire{};
    std::uint8_t filmAdvanceWire{};
    std::uint8_t afModeWire{};
    std::uint8_t multipleExposureWire{};
    std::optional<std::string> capturedAt;
    std::optional<std::string> cfnValues;
    std::optional<std::uint8_t> cfnAuxWire;
    std::string cfnRawHex;
    bool focalLengthPresent{};
    bool maxAperturePresent{};
    bool shutterPresent{};
    bool aperturePresent{};
    bool manualIsoPresent{};
    bool exposureCompensationPresent{};
    bool flashCompensationPresent{};
    bool flashModePresent{};
    bool meteringPresent{};
    bool filmAdvancePresent{};
    bool afModePresent{};
    std::optional<double> maxApertureF;
    std::optional<double> shutterSeconds;
    std::optional<std::string> shutterDisplay;
    std::optional<double> apertureF;
    std::optional<unsigned> manualIso;
    std::optional<double> exposureCompensationEv;
    std::optional<double> flashCompensationEv;
    std::string flashMode;
    std::string meteringMode;
    std::string shootingMode;
    std::optional<std::string> aebPosition;
    std::string filmAdvance;
    std::string afMode;
    bool multipleExposure{};
    std::optional<std::uint16_t> bulbTimeWire;
    std::optional<std::uint8_t> focusSelectionWire;
    std::string focusPointsRawHex;
    std::optional<std::string> batteryLoadedAt;
    std::string rawHex;
};

struct FilmRoll {
    std::string filmId;
    std::uint8_t recordWidth{};
    std::string fieldMaskHex;
    std::uint8_t dxIsoWire{};
    std::optional<unsigned> dxIso;
    std::optional<std::string> loadedAt;
    bool knownFullLayout{};
    std::string rawHeaderHex;
    std::vector<FilmFrame> frames;
};

struct Download {
    std::uint16_t reportedRolls{};
    std::vector<FilmRoll> rolls;
};

Download parsePackets(const std::vector<open1v::CameraPacket>& packets);
std::string toJson(const Download& download);
std::string toCsv(const Download& download);
bool runSelfTest(std::string& error);

} // namespace filmrecorder
