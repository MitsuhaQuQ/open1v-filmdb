#include "filmrecorder/application.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace filmrecorder {

SyncResult syncFilmRecords(open1v::CameraProtocolSession& camera,
                           const SyncRequest& request) {
    SyncResult result;
    result.output = std::filesystem::absolute(request.output);
    try {
        if (!camera.sessionActive()) camera.beginSession();
        const auto status = camera.perform(open1v::CameraRead::filmStatus);
        const auto e1 = std::find_if(status.begin(), status.end(),
            [](const open1v::CameraPacket& packet) {
                return packet.label == "FILM STATUS E1";
            });
        if (e1 == status.end() || e1->bytes.size() != 5)
            throw std::runtime_error("camera did not return a valid film-record status");
        result.reportedRolls = static_cast<unsigned>(e1->bytes[2]) << 8 |
                               static_cast<unsigned>(e1->bytes[3]);
        if (result.reportedRolls == 0) {
            result.cameraEmpty = true;
            if (!request.keepSession) camera.endSession();
            return result;
        }

        const auto download = parsePackets(
            camera.perform(open1v::CameraRead::filmRecords));
        if (!request.keepSession) camera.endSession();
        result.rolls = download.rolls.size();
        for (const auto& roll : download.rolls) result.frames += roll.frames.size();

        if (request.format == ExportFormat::sqlite) {
            const auto saved = saveToDatabase(request.output, download,
                                               request.sourceType.c_str());
            result.importId = saved.importId;
            result.rolls = saved.rolls;
            result.frames = saved.frames;
        } else {
            if (request.output.has_parent_path())
                std::filesystem::create_directories(request.output.parent_path());
            std::ofstream stream(request.output, std::ios::binary);
            if (!stream) throw std::runtime_error("cannot open output file");
            stream << (request.format == ExportFormat::json
                ? toJson(download) : toCsv(download));
            if (!stream) throw std::runtime_error("failed while writing output file");
        }
        return result;
    } catch (...) {
        if (!request.keepSession) {
            try { if (camera.sessionActive()) camera.endSession(); } catch (...) {}
        }
        throw;
    }
}

void clearFilmRecords(open1v::CameraProtocolSession& camera, bool keepSession) {
    try {
        camera.clearFilmRecords();
        if (!keepSession && camera.sessionActive()) camera.endSession();
    } catch (...) {
        if (!keepSession) {
            try { if (camera.sessionActive()) camera.endSession(); } catch (...) {}
        }
        throw;
    }
}

const std::array<ShootingDataField, 18>& ShootingDataSelection::fields() {
    static const std::array<ShootingDataField, 18> value{{
        {"Focal length",2,{0x30,0,0,0,0,0,0,0}},
        {"Maximum aperture",1,{0x08,0,0,0,0,0,0,0}},
        {"Shutter speed",1,{0x04,0,0,0,0,0,0,0}},
        {"Selected aperture",1,{0x02,0,0,0,0,0,0,0}},
        {"Manual ISO",1,{0x01,0,0,0,0,0,0,0}},
        {"Exposure compensation",1,{0,0x80,0,0,0,0,0,0}},
        {"Flash exposure compensation",1,{0,0x40,0,0,0,0,0,0}},
        {"Flash mode",1,{0,0x20,0,0,0,0,0,0}},
        {"Metering mode",1,{0,0x10,0,0,0,0,0,0}},
        {"Film advance",1,{0,0x04,0,0,0,0,0,0}},
        {"AF mode",1,{0,0x02,0,0,0,0,0,0}},
        {"Bulb exposure time",2,{0,0,0x0c,0,0,0,0,0}},
        {"Shooting date",3,{0,0,0,0x38,0,0,0,0}},
        {"Shooting time",3,{0,0,0,0x07,0,0,0,0}},
        {"C.Fn settings",11,{0,0,0,0,0x7f,0xf0,0,0}},
        {"Focus-point selection",1,{0,0,0,0,0,0x08,0,0}},
        {"In-focus point data",7,{0,0,0,0,0,0,0x7f,0}},
        {"Battery-load date/time",6,{0,0,0,0,0,0,0,0x3f}}
    }};
    return value;
}

ShootingDataSelection ShootingDataSelection::fromPackets(
    const std::vector<open1v::CameraPacket>& packets) {
    for (const auto& packet : packets) {
        if (packet.bytes.size() == 11 && packet.bytes[0] == 0xe8) {
            std::array<std::uint8_t, 8> mask{};
            std::copy_n(packet.bytes.begin() + 2, mask.size(), mask.begin());
            return ShootingDataSelection(mask);
        }
    }
    throw std::runtime_error("camera did not return the shooting-data field mask");
}

bool ShootingDataSelection::enabledIn(
    const std::array<std::uint8_t, 8>& mask, const ShootingDataField& field) {
    for (std::size_t i = 0; i < mask.size(); ++i)
        if ((mask[i] & field.bits[i]) != field.bits[i]) return false;
    return true;
}

bool ShootingDataSelection::enabled(std::size_t index) const {
    return enabledIn(mask_, fields().at(index));
}

bool ShootingDataSelection::changed(std::size_t index) const {
    return enabled(index) != enabledIn(original_, fields().at(index));
}

unsigned ShootingDataSelection::usedBytes() const {
    unsigned total = 0;
    for (std::size_t i = 0; i < fields().size(); ++i)
        if (enabled(i)) total += fields()[i].bytes;
    return total;
}

unsigned ShootingDataSelection::recordWidth() const {
    return open1v::shootingDataRecordWidth(mask_);
}

void ShootingDataSelection::toggle(std::size_t index) {
    const auto& field = fields().at(index);
    const bool turnOn = !enabled(index);
    if (turnOn && usedBytes() + field.bytes > 28)
        throw std::runtime_error("the 28-byte shooting-data limit would be exceeded");
    auto candidate = mask_;
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        if (turnOn) candidate[i] |= field.bits[i];
        else candidate[i] &= static_cast<std::uint8_t>(~field.bits[i]);
    }
    (void)open1v::shootingDataRecordWidth(candidate);
    mask_ = candidate;
}

} // namespace filmrecorder
