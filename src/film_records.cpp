#include "filmrecorder/film_records.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace filmrecorder {
namespace {

constexpr std::array<std::uint8_t, 8> fullMask{
    0xff, 0xff, 0x0c, 0x3f, 0x00, 0x08, 0x7f, 0x00};

std::string hex(const std::vector<std::uint8_t>& bytes,
                std::size_t begin = 0, std::size_t count = std::string::npos) {
    std::ostringstream out;
    const auto end = count == std::string::npos ? bytes.size() :
        std::min(bytes.size(), begin + count);
    for (std::size_t i = begin; i < end; ++i) {
        if (i != begin) out << ' ';
        out << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

int bcd(std::uint8_t value) {
    if ((value >> 4) > 9 || (value & 0x0f) > 9) return -1;
    return (value >> 4) * 10 + (value & 0x0f);
}

std::optional<std::string> dateTime(const std::vector<std::uint8_t>& packet,
                                    std::size_t dataOffset) {
    const auto at = 2 + dataOffset;
    if (at + 6 > packet.size() - 1) return std::nullopt;
    std::array<int, 6> value{};
    for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = bcd(packet[at + i]);
        if (value[i] < 0) return std::nullopt;
    }
    if (value[1] < 1 || value[1] > 12 || value[2] < 1 || value[2] > 31 ||
        value[3] > 23 || value[4] > 59 || value[5] > 59) return std::nullopt;
    std::ostringstream out;
    out << (value[0] >= 80 ? 1900 : 2000) + value[0] << '-'
        << std::setw(2) << std::setfill('0') << value[1] << '-'
        << std::setw(2) << value[2] << 'T' << std::setw(2) << value[3] << ':'
        << std::setw(2) << value[4] << ':' << std::setw(2) << value[5];
    return out.str();
}

std::string filmId(const std::vector<std::uint8_t>& packet) {
    const auto a=bcd(packet[6]), b=bcd(packet[7]), c=bcd(packet[8]);
    if(a<0||b<0||c<0)return hex(packet,6,3);
    std::ostringstream out; out<<std::setw(2)<<std::setfill('0')<<a<<'-'
        <<b<<std::setw(2)<<c;
    return out.str();
}

std::string jsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) out << "\\u" << std::hex << std::setw(4)
                              << std::setfill('0') << static_cast<unsigned>(c);
            else out << c;
        }
    }
    out << '"';
    return out.str();
}

std::string csvCell(const std::string& value) {
    std::string escaped;
    for (const char c : value) escaped += c == '"' ? "\"\"" : std::string(1, c);
    return '"' + escaped + '"';
}

bool isEnd(const std::vector<std::uint8_t>& packet, std::uint8_t command) {
    return packet == std::vector<std::uint8_t>{command, 1, 0, 0};
}

bool enabled(const std::array<std::uint8_t, 8>& mask,
             std::initializer_list<std::pair<std::size_t, unsigned>> parts) {
    for (const auto [at, bits] : parts)
        if ((static_cast<unsigned>(mask[at]) & bits) != bits) return false;
    return true;
}

int oneHot(std::uint8_t value) {
    if (!value || (value & (value - 1))) return -1;
    int option = 0;
    while ((value >>= 1) != 0) ++option;
    return option;
}

std::string decodeCfn(const std::uint8_t* bytes) {
    std::ostringstream out;
    for (int fn = 1; fn <= 19; ++fn) {
        const auto byte = bytes[(fn - 1) / 2];
        const auto wire = static_cast<std::uint8_t>(fn == 19 ? byte :
            ((fn & 1) ? byte & 0x0f : byte >> 4));
        const auto option = oneHot(wire);
        if (option < 0) throw std::runtime_error("invalid one-hot C.Fn snapshot value");
        if (fn != 1) out << ',';
        out << option;
    }
    return out.str();
}

std::string unknown(std::uint8_t wire) {
    std::ostringstream out; out << "Unknown(0x" << std::hex << std::uppercase
        << std::setw(2) << std::setfill('0') << static_cast<unsigned>(wire) << ')';
    return out.str();
}

std::string flashMode(std::uint8_t wire) {
    if (wire == 0x02) return "OFF";
    if (wire == 0x48) return "Manual";
    if (wire == 0x49) return "E-TTL";
    return unknown(wire);
}
std::string metering(std::uint8_t wire) {
    switch (wire & 0xf0) {
    case 0x20:return "Evaluative"; case 0x10:return "Center-weighted average";
    case 0x80:return "Spot"; case 0x40:return "Partial"; default:return unknown(wire);
    }
}
std::string shooting(std::uint8_t wire) {
    switch (wire) {
    case 0x80:return "Manual"; case 0x10:return "Program AE"; case 0x20:return "Shutter-priority AE";
    case 0x40:return "Aperture-priority AE"; case 0x08:return "Depth-of-field AE";
    case 0x04:return "Bulb"; default:return unknown(wire);
    }
}
std::string advance(std::uint8_t wire) {
    switch (wire) {
    case 0x08:return "Single-frame"; case 0x10:return "2-sec self-timer";
    case 0x20:return "10-sec self-timer"; case 0x40:return "Continuous (body)";
    case 0x01:return "Low-speed continuous"; case 0x02:return "High-speed continuous";
    default:return unknown(wire);
    }
}
std::string af(std::uint8_t wire) {
    switch (wire & 0x1f) {
    case 0x02:return "One-Shot AF"; case 0x04:return "AI Servo AF";
    case 0x14:return "Manual focus"; default:return unknown(wire);
    }
}

std::optional<std::string> dateTimeParts(const std::uint8_t* date,
                                         const std::uint8_t* time) {
    std::array<int,6> v{};
    for (int i=0;i<3;++i) { v[i]=bcd(date[i]); v[i+3]=bcd(time[i]); }
    if (v[0]<0||v[1]<1||v[1]>12||v[2]<1||v[2]>31||v[3]<0||v[3]>23||v[4]<0||v[4]>59||v[5]<0||v[5]>59)
        return std::nullopt;
    std::ostringstream out; out << (v[0]>=80?1900:2000)+v[0] << '-'
        << std::setw(2)<<std::setfill('0')<<v[1]<<'-'<<std::setw(2)<<v[2]<<'T'
        << std::setw(2)<<v[3]<<':'<<std::setw(2)<<v[4]<<':'<<std::setw(2)<<v[5];
    return out.str();
}

} // namespace

Download parsePackets(const std::vector<open1v::CameraPacket>& packets) {
    Download result;
    FilmRoll* current = nullptr;
    for (const auto& item : packets) {
        const auto& packet = item.bytes;
        if (packet.empty()) continue;
        if (packet[0] == 0xe1 && packet.size() == 5) {
            result.reportedRolls = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(packet[2]) << 8 | packet[3]);
        } else if (packet[0] == 0xe3) {
            if (isEnd(packet, 0xe3)) { current = nullptr; continue; }
            if (packet.size() != 36) throw std::runtime_error("invalid E3 packet length");
            FilmRoll roll;
            roll.filmId = filmId(packet);
            roll.recordWidth = packet[2 + 8];
            roll.fieldMaskHex = hex(packet, 2 + 9, 8);
            roll.dxIsoWire = packet[2 + 18];
            if (roll.dxIsoWire != 0xf0 && roll.dxIsoWire != 0xff)
                roll.dxIso = static_cast<unsigned>(std::lround(
                    100.0 * std::pow(2.0, (static_cast<int>(roll.dxIsoWire) - 0x48) / 8.0)));
            roll.loadedAt = dateTime(packet, 19);
            roll.knownFullLayout = roll.recordWidth == 0x20 &&
                std::equal(fullMask.begin(), fullMask.end(), packet.begin() + 2 + 9);
            roll.rawHeaderHex = hex(packet);
            result.rolls.push_back(std::move(roll));
            current = &result.rolls.back();
        } else if (packet[0] == 0xe4) {
            if (isEnd(packet, 0xe4)) continue;
            if (!current) throw std::runtime_error("E4 frame arrived without an E3 header");
            FilmFrame frame;
            frame.rawHex = hex(packet);
            std::array<std::uint8_t, 8> mask{};
            std::istringstream maskText(current->fieldMaskHex);
            for (auto& value : mask) {
                unsigned parsed{};
                if (!(maskText >> std::hex >> parsed))
                    throw std::runtime_error("invalid stored field mask");
                value = static_cast<std::uint8_t>(parsed);
            }
            const auto dataSize = packet.size() - 3;
            const auto* d = packet.data() + 2;
            if (dataSize < 5) throw std::runtime_error("short E4 data record");
            frame.number = d[2];
            std::size_t cursor = 3;
            auto take = [&](std::size_t size) { if (cursor + size > dataSize) throw std::runtime_error("E4 field exceeds record width"); const auto* value=d+cursor; cursor+=size; return value; };
            if (enabled(mask,{{0,0x30}})) { const auto* v=take(2); frame.focalLengthPresent=true; frame.focalLengthMm=static_cast<std::uint16_t>(v[0]<<8|v[1]); }
            if (enabled(mask,{{0,0x08}})) { frame.maxAperturePresent=true; frame.maxApertureWire=*take(1); if(frame.maxApertureWire!=0xff)frame.maxApertureF=std::pow(2.0,frame.maxApertureWire/8.0); }
            if (enabled(mask,{{0,0x04}})) { frame.shutterPresent=true; frame.shutterWire=*take(1); if(frame.shutterWire==0xf0)frame.shutterDisplay="Bulb"; else { frame.shutterSeconds=std::pow(2.0,-static_cast<std::int8_t>(frame.shutterWire)/8.0); std::ostringstream s; if(*frame.shutterSeconds>=1)s<<std::fixed<<std::setprecision(1)<<*frame.shutterSeconds<<" s";else s<<"1/"<<std::llround(1.0 / *frame.shutterSeconds)<<" s"; frame.shutterDisplay=s.str(); } }
            if (enabled(mask,{{0,0x02}})) { frame.aperturePresent=true; frame.apertureWire=*take(1); if(frame.apertureWire!=0xff)frame.apertureF=std::pow(2.0,frame.apertureWire/8.0); }
            if (enabled(mask,{{0,0x01}})) { frame.manualIsoPresent=true; frame.isoSourceWire=*take(1); if(frame.isoSourceWire!=0xff&&frame.isoSourceWire!=0xf0)frame.manualIso=static_cast<unsigned>(std::lround(100.0*std::pow(2.0,(static_cast<int>(frame.isoSourceWire)-0x48)/8.0))); }
            if (enabled(mask,{{1,0x80}})) { frame.exposureCompensationPresent=true; frame.exposureCompensationWire=static_cast<std::int8_t>(*take(1)); frame.exposureCompensationEv=frame.exposureCompensationWire/8.0; }
            if (enabled(mask,{{1,0x40}})) { frame.flashCompensationPresent=true; frame.flashCompensationWire=static_cast<std::int8_t>(*take(1)); frame.flashCompensationEv=frame.flashCompensationWire/8.0; }
            if (enabled(mask,{{1,0x20}})) { frame.flashModePresent=true; frame.flashModeWire=*take(1); frame.flashMode=flashMode(frame.flashModeWire); }
            if (enabled(mask,{{1,0x10}})) { frame.meteringPresent=true; frame.meteringWire=*take(1); frame.meteringMode=metering(frame.meteringWire); }
            frame.shootingModeWire=*take(1); frame.shootingMode=shooting(frame.shootingModeWire);
            if (enabled(mask,{{1,0x04}})) { frame.filmAdvancePresent=true; frame.filmAdvanceWire=*take(1); frame.filmAdvance=advance(frame.filmAdvanceWire); }
            if (enabled(mask,{{1,0x02}})) { frame.afModePresent=true; frame.afModeWire=*take(1); frame.afMode=af(frame.afModeWire); }
            frame.multipleExposureWire=*take(1); frame.multipleExposure=(frame.multipleExposureWire&0x80)!=0;
            if (enabled(mask,{{2,0x0c}})) { const auto* v=take(2); frame.bulbTimeWire=static_cast<std::uint16_t>(v[0]<<8|v[1]); }
            const std::uint8_t* capturedDate=nullptr; const std::uint8_t* capturedTime=nullptr;
            if (enabled(mask,{{3,0x38}})) capturedDate=take(3);
            if (enabled(mask,{{3,0x07}})) capturedTime=take(3);
            if(capturedDate&&capturedTime)frame.capturedAt=dateTimeParts(capturedDate,capturedTime);
            if (enabled(mask,{{4,0x7f},{5,0xf0}})) { const auto* v=take(11); frame.cfnValues=decodeCfn(v); frame.cfnRawHex=hex(packet,static_cast<std::size_t>(v-packet.data()),11); frame.cfnAuxWire=v[10]; }
            if (enabled(mask,{{5,0x08}})) frame.focusSelectionWire=*take(1);
            if (enabled(mask,{{6,0x7f}})) { const auto* v=take(7); frame.focusPointsRawHex=hex(packet,static_cast<std::size_t>(v-packet.data()),7); }
            if (enabled(mask,{{7,0x3f}})) { const auto* v=take(6); frame.batteryLoadedAt=dateTimeParts(v,v+3); }
            current->frames.push_back(std::move(frame));
        }
    }
    if (result.rolls.size() != result.reportedRolls)
        throw std::runtime_error("parsed roll count does not match E1");
    return result;
}

std::string toJson(const Download& download) {
    std::ostringstream out;
    out << "{\n  \"schema_version\": 1,\n  \"reported_rolls\": "
        << download.reportedRolls << ",\n  \"rolls\": [";
    for (std::size_t r = 0; r < download.rolls.size(); ++r) {
        const auto& roll = download.rolls[r];
        out << (r ? "," : "") << "\n    {\n      \"film_id_raw\": "
            << jsonString(roll.filmId) << ",\n      \"record_width\": "
            << static_cast<unsigned>(roll.recordWidth)
            << ",\n      \"field_mask_raw\": " << jsonString(roll.fieldMaskHex)
            << ",\n      \"dx_iso_wire\": " << static_cast<unsigned>(roll.dxIsoWire)
            << ",\n      \"loaded_at\": "
            << (roll.loadedAt ? jsonString(*roll.loadedAt) : "null")
            << ",\n      \"known_full_layout\": "
            << (roll.knownFullLayout ? "true" : "false")
            << ",\n      \"raw_e3\": " << jsonString(roll.rawHeaderHex)
            << ",\n      \"frames\": [";
        for (std::size_t f = 0; f < roll.frames.size(); ++f) {
            const auto& frame = roll.frames[f];
            out << (f ? "," : "") << "\n        {\"frame_number\": "
                << static_cast<unsigned>(frame.number)
                << ", \"focal_length_mm\": " << frame.focalLengthMm
                << ", \"max_aperture_wire\": " << static_cast<unsigned>(frame.maxApertureWire)
                << ", \"shutter_wire\": " << static_cast<unsigned>(frame.shutterWire)
                << ", \"aperture_wire\": " << static_cast<unsigned>(frame.apertureWire)
                << ", \"iso_source_wire\": " << static_cast<unsigned>(frame.isoSourceWire)
                << ", \"exposure_compensation_eighths\": " << static_cast<int>(frame.exposureCompensationWire)
                << ", \"flash_compensation_eighths\": " << static_cast<int>(frame.flashCompensationWire)
                << ", \"flash_mode_wire\": " << static_cast<unsigned>(frame.flashModeWire)
                << ", \"metering_wire\": " << static_cast<unsigned>(frame.meteringWire)
                << ", \"shooting_mode_wire\": " << static_cast<unsigned>(frame.shootingModeWire)
                << ", \"film_advance_wire\": " << static_cast<unsigned>(frame.filmAdvanceWire)
                << ", \"af_mode_wire\": " << static_cast<unsigned>(frame.afModeWire)
                << ", \"multiple_exposure_wire\": " << static_cast<unsigned>(frame.multipleExposureWire)
                << ", \"captured_at\": " << (frame.capturedAt ? jsonString(*frame.capturedAt) : "null")
                << ", \"cfn_values\": " << (frame.cfnValues ? jsonString(*frame.cfnValues) : "null")
                << ", \"cfn_raw\": " << (frame.cfnRawHex.empty() ? "null" : jsonString(frame.cfnRawHex))
                << ", \"raw_e4\": " << jsonString(frame.rawHex) << "}";
        }
        out << "\n      ]\n    }";
    }
    out << "\n  ]\n}\n";
    return out.str();
}

std::string toCsv(const Download& download) {
    std::ostringstream out;
    out << "roll_index,film_id_raw,record_width,field_mask_raw,frame_number,"
           "focal_length_mm,captured_at,raw_e3,raw_e4\n";
    for (std::size_t r = 0; r < download.rolls.size(); ++r) {
        const auto& roll = download.rolls[r];
        if (roll.frames.empty())
            out << r + 1 << ',' << csvCell(roll.filmId) << ','
                << static_cast<unsigned>(roll.recordWidth) << ','
                << csvCell(roll.fieldMaskHex) << ",,,,," << csvCell(roll.rawHeaderHex) << ",\n";
        for (const auto& frame : roll.frames)
            out << r + 1 << ',' << csvCell(roll.filmId) << ','
                << static_cast<unsigned>(roll.recordWidth) << ','
                << csvCell(roll.fieldMaskHex) << ',' << static_cast<unsigned>(frame.number)
                << ',' << frame.focalLengthMm << ','
                << csvCell(frame.capturedAt.value_or("")) << ','
                << csvCell(roll.rawHeaderHex) << ',' << csvCell(frame.rawHex) << '\n';
    }
    return out.str();
}

bool runSelfTest(std::string& error) {
    try {
        std::vector<std::uint8_t> e1{0xe1, 2, 0, 1, 1};
        std::vector<std::uint8_t> e3(36); e3[0] = 0xe3; e3[1] = 33;
        e3[6] = 0x00; e3[7] = 0x03; e3[8] = 0x29; e3[10] = 0x20;
        std::copy(fullMask.begin(), fullMask.end(), e3.begin() + 11);
        e3[20] = 0x48; const std::array<std::uint8_t, 6> loaded{0x26,9,0x22,0x12,0,0};
        std::copy(loaded.begin(), loaded.end(), e3.begin() + 21);
        std::vector<std::uint8_t> e4(36); e4[0] = 0xe4; e4[1] = 33;
        e4[4] = 7; e4[5] = 0; e4[6] = 105;
        const std::array<std::uint8_t, 6> captured{0x26,9,0x22,0x12,0x34,0x56};
        std::copy(captured.begin(), captured.end(), e4.begin() + 21);
        const std::vector<open1v::CameraPacket> packets{
            {"FILM E1", e1}, {"FILM E3", e3}, {"FILM E4", e4},
            {"FILM E4", {0xe4,1,0,0}}, {"FILM E3", {0xe3,1,0,0}}};
        const auto parsed = parsePackets(packets);
        if (parsed.rolls.size() != 1 || parsed.rolls[0].frames.size() != 1 ||
            parsed.rolls[0].frames[0].number != 7 ||
            parsed.rolls[0].frames[0].focalLengthMm != 105 ||
            parsed.rolls[0].frames[0].capturedAt != "2026-09-22T12:34:56")
            throw std::runtime_error("parsed values did not match fixture");

        std::vector<std::uint8_t> dynamicE3(36); dynamicE3[0] = 0xe3; dynamicE3[1] = 33;
        dynamicE3[10] = 0x20;
        const std::array<std::uint8_t,8> dynamicMask{0xc2,0x9d,0,0x3f,0x7f,0xf8,0,0x3f};
        std::copy(dynamicMask.begin(), dynamicMask.end(), dynamicE3.begin() + 11);
        std::vector<std::uint8_t> dynamicE4{0xe4,33,
            0x01,0x81,0x01, 0xff,0xfb,0x20,0x80, 0x40,0x00,
            0x26,0x09,0x21,0x18,0x16,0x27,
            0x21,0x81,0x12,0x21,0x11,0x11,0x11,0x11,0x11,0x01,0x02,
            0x4a, 0x26,0x09,0x21,0x18,0x13,0x44, 0x00};
        const std::vector<open1v::CameraPacket> dynamicPackets{
            {"FILM E1", e1}, {"FILM E3", dynamicE3}, {"FILM E4", dynamicE4},
            {"FILM E4", {0xe4,1,0,0}}, {"FILM E3", {0xe3,1,0,0}}};
        const auto dynamic = parsePackets(dynamicPackets);
        if (dynamic.rolls[0].frames[0].cfnValues !=
            "0,1,0,3,1,0,0,1,0,0,0,0,0,0,0,0,0,0,0" ||
            dynamic.rolls[0].frames[0].cfnAuxWire != 2)
            throw std::runtime_error("C.Fn snapshot did not match fixture");

        auto depthE4 = dynamicE4;
        depthE4[8] = 0x08;
        const std::vector<open1v::CameraPacket> depthPackets{
            {"FILM E1", e1}, {"FILM E3", dynamicE3}, {"FILM E4", depthE4},
            {"FILM E4", {0xe4,1,0,0}}, {"FILM E3", {0xe3,1,0,0}}};
        const auto depth = parsePackets(depthPackets);
        if (depth.rolls[0].frames[0].shootingMode != "Depth-of-field AE")
            throw std::runtime_error("depth-of-field AE shooting mode was not decoded");
        return true;
    } catch (const std::exception& ex) { error = ex.what(); return false; }
}

} // namespace filmrecorder
