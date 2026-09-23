#include "filmrecorder/database.hpp"
#include "filmrecorder/film_records.hpp"
#include "open1v/bridge_client.hpp"
#include "open1v/serial_transport.hpp"
#include "open1v/winusb_transport.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#include <conio.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <termios.h>
#include <unistd.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace {
struct Options {
    std::string port;
    bool winusb{};
    std::string format{"sqlite"};
    std::string output;
};

std::filesystem::path executableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(260, L'\0');
    while (true) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) throw std::runtime_error("cannot determine executable path");
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        if (buffer.size() >= 32768) throw std::runtime_error("executable path is too long");
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        throw std::runtime_error("cannot determine executable path");
    return std::filesystem::weakly_canonical(buffer).parent_path();
#elif defined(__linux__)
    std::string buffer(256, '\0');
    for (;;) {
        const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) throw std::runtime_error("cannot determine executable path");
        if (static_cast<std::size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length));
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    return std::filesystem::current_path();
#endif
}

void usage() {
    std::cout << "EOS-1V Film Record downloader\n\n"
                 "Usage:\n"
                 "  film-record [--port PATH | --winusb]\n"
                 "  film-record sync [--port PATH | --winusb] [--format sqlite|json|csv] [--output FILE]\n"
                 "  film-record clear [--port PATH | --winusb]\n"
                 "  film-record view [--database FILE]\n"
                 "  film-record inspect [--database FILE]\n"
                 "  film-record self-test\n\n"
                 "Running without a command starts interactive mode.\n";
}

std::string normalized(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

Options parseOptions(int argc, char** argv, int first, bool allowOutput) {
    Options options;
    for (int i = first; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--winusb") options.winusb = true;
        else if ((arg == "--port" || (allowOutput && (arg == "--format" || arg == "--output"))) && i + 1 < argc) {
            const std::string value = argv[++i];
            if (arg == "--port") options.port = value;
            else if (arg == "--format") options.format = value;
            else options.output = value;
        } else throw std::runtime_error("unknown or incomplete option: " + arg);
    }
    if (options.winusb && !options.port.empty()) throw std::runtime_error("choose either --port or --winusb");
    if (options.format != "sqlite" && options.format != "json" && options.format != "csv")
        throw std::runtime_error("format must be sqlite, json or csv");
    return options;
}

std::unique_ptr<open1v::ITransport> makeTransport(const Options& options) {
    if (options.winusb) return std::make_unique<open1v::WinUsbTransport>();
    return std::make_unique<open1v::SerialTransport>(options.port);
}

void syncRecords(Options options) {
    if (options.output.empty()) {
        options.output = options.format == "sqlite"
            ? (executableDirectory() / "film-records.sqlite3").string()
            : "film-records." + options.format;
    }
    auto transport = makeTransport(options);
    open1v::BridgeClient bridge(*transport);
    open1v::CameraProtocolSession camera(bridge);
    std::vector<open1v::CameraPacket> packets;
    try {
        camera.beginSession();
        const auto status = camera.perform(open1v::CameraRead::filmStatus);
        const auto e1 = std::find_if(status.begin(), status.end(),
            [](const open1v::CameraPacket& packet) {
                return packet.label == "FILM STATUS E1";
            });
        if (e1 == status.end() || e1->bytes.size() != 5)
            throw std::runtime_error("camera did not return a valid film-record status");
        const auto rollCount = static_cast<unsigned>(e1->bytes[2]) << 8 |
                               static_cast<unsigned>(e1->bytes[3]);
        if (rollCount == 0) {
            camera.endSession();
            std::cout << "Camera film-record storage is empty; nothing to sync.\n";
            return;
        }

        std::cout << "Camera reports " << rollCount
                  << " roll(s). Downloading film records...\n";
        packets = camera.perform(open1v::CameraRead::filmRecords);
        camera.endSession();
    } catch (...) {
        try { if (camera.sessionActive()) camera.endSession(); } catch (...) {}
        throw;
    }
    const auto download = filmrecorder::parsePackets(packets);
    const std::filesystem::path path(options.output);
    std::size_t frames = 0;
    for (const auto& roll : download.rolls) frames += roll.frames.size();
    if (options.format == "sqlite") {
        const auto saved = filmrecorder::saveToDatabase(path, download,
            options.winusb ? "winusb" : (options.port.empty() ? "serial-auto" : options.port.c_str()));
        std::cout << "Saved import " << saved.importId << ": " << saved.rolls
                  << " roll(s), " << saved.frames << " frame(s) to ";
    } else {
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("cannot open output file: " + options.output);
        stream << (options.format == "json" ? filmrecorder::toJson(download) : filmrecorder::toCsv(download));
        if (!stream) throw std::runtime_error("failed while writing output file: " + options.output);
        std::cout << "Saved " << download.rolls.size() << " roll(s), " << frames << " frame(s) to ";
    }
    std::cout << std::filesystem::absolute(path).string() << '\n';
}

void clearRecords(const Options& options) {
    auto transport = makeTransport(options);
    open1v::BridgeClient bridge(*transport);
    open1v::CameraProtocolSession camera(bridge);
    std::cout << "Clearing all film records from EOS-1V...\n";
    camera.clearFilmRecords();
    std::cout << "Camera film-record storage is empty (verified by E1).\n";
}

bool confirmClear() {
    while (true) {
        std::cout << "Clear ALL film records from the camera? This cannot be undone. [y/n]: " << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer)) return false;
        answer = normalized(answer);
        if (answer == "y") return true;
        if (answer == "n") return false;
        std::cout << "Please enter y or n.\n";
    }
}

std::optional<std::size_t> menuIndex(const std::string& input,
                                     std::size_t count) {
    if (input.empty()) return std::nullopt;
    std::size_t value = 0;
    const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
    if (result.ec != std::errc{} || result.ptr != input.data() + input.size() ||
        value == 0 || value > count) return std::nullopt;
    return value - 1;
}

struct ShootingDataField {
    const char* name;
    unsigned bytes;
    std::array<std::uint8_t,8> bits;
};

const std::array<ShootingDataField,18>& shootingDataFields() {
    static const std::array<ShootingDataField,18> fields{{
        {"Focal length",                 2,{0x30,0,0,0,0,0,0,0}},
        {"Maximum aperture",             1,{0x08,0,0,0,0,0,0,0}},
        {"Shutter speed",                1,{0x04,0,0,0,0,0,0,0}},
        {"Selected aperture",            1,{0x02,0,0,0,0,0,0,0}},
        {"Manual ISO",                   1,{0x01,0,0,0,0,0,0,0}},
        {"Exposure compensation",        1,{0,0x80,0,0,0,0,0,0}},
        {"Flash exposure compensation",  1,{0,0x40,0,0,0,0,0,0}},
        {"Flash mode",                   1,{0,0x20,0,0,0,0,0,0}},
        {"Metering mode",                1,{0,0x10,0,0,0,0,0,0}},
        {"Film advance",                 1,{0,0x04,0,0,0,0,0,0}},
        {"AF mode",                      1,{0,0x02,0,0,0,0,0,0}},
        {"Bulb exposure time",           2,{0,0,0x0c,0,0,0,0,0}},
        {"Shooting date",                3,{0,0,0,0x38,0,0,0,0}},
        {"Shooting time",                3,{0,0,0,0x07,0,0,0,0}},
        {"C.Fn settings",               11,{0,0,0,0,0x7f,0xf0,0,0}},
        {"Focus-point selection",        1,{0,0,0,0,0,0x08,0,0}},
        {"In-focus point data",          7,{0,0,0,0,0,0,0x7f,0}},
        {"Battery-load date/time",       6,{0,0,0,0,0,0,0,0x3f}}
    }};
    return fields;
}

std::array<std::uint8_t,8> shootingMask(
    const std::vector<open1v::CameraPacket>& packets) {
    for (const auto& packet : packets) {
        if (packet.bytes.size() == 11 && packet.bytes[0] == 0xe8) {
            std::array<std::uint8_t,8> mask{};
            std::copy_n(packet.bytes.begin()+2,mask.size(),mask.begin());
            return mask;
        }
    }
    throw std::runtime_error("camera did not return the shooting-data field mask");
}

bool fieldEnabled(const std::array<std::uint8_t,8>& mask,
                  const ShootingDataField& field) {
    for (std::size_t i=0;i<mask.size();++i)
        if ((mask[i]&field.bits[i])!=field.bits[i]) return false;
    return true;
}

unsigned selectedShootingDataBytes(const std::array<std::uint8_t,8>& mask) {
    unsigned total=0;
    for (const auto& field:shootingDataFields())
        if (fieldEnabled(mask,field)) total+=field.bytes;
    return total;
}

void setShootingData(const Options& options) {
    for (;;) {
        auto transport=makeTransport(options);
        open1v::BridgeClient bridge(*transport);
        open1v::CameraProtocolSession camera(bridge);
        auto mask=shootingMask(camera.readOnce(open1v::CameraRead::settings));
        const auto& fields=shootingDataFields();
        const auto usedBytes=selectedShootingDataBytes(mask);
        std::cout << "\nFilm shooting-data fields\n";
        for (std::size_t i=0;i<fields.size();++i) {
            const bool enabled=fieldEnabled(mask,fields[i]);
            std::cout << i+1 << ") [" << (enabled?"ON ":"OFF")
                      << "] " << fields[i].name << " {" << fields[i].bytes
                      << (fields[i].bytes==1?" byte}":" bytes}");
            if (!enabled && usedBytes+fields[i].bytes>28)
                std::cout << " - cannot enable: limit exceeded";
            std::cout << '\n';
        }
        std::cout << "Current usage: " << usedBytes
                  << "/28 bytes | Internal record length: "
                  << unsigned(open1v::shootingDataRecordWidth(mask)) << " bytes\n"
                  << "q) Back\nset/field> " << std::flush;
        std::string input;
        if (!std::getline(std::cin,input)) return;
        input=normalized(input);
        if (input=="q") return;
        const auto selected=menuIndex(input,fields.size());
        if (!selected) { std::cout << "Invalid selection.\n"; continue; }

        const auto& field=fields[*selected];
        const bool current=fieldEnabled(mask,field);
        std::cout << "\n" << field.name << " is currently "
                  << (current?"ON":"OFF") << ".\n"
                  << "1) ON\n2) OFF\nq) Cancel\nset/value> " << std::flush;
        if (!std::getline(std::cin,input)) return;
        input=normalized(input);
        if (input=="q") continue;
        const auto state=menuIndex(input,2);
        if (!state) { std::cout << "Invalid selection.\n"; continue; }
        const bool enabled=*state==0;
        if (enabled==current) { std::cout << "Setting is already unchanged.\n"; continue; }
        const auto currentBytes=selectedShootingDataBytes(mask);
        if (enabled && currentBytes+field.bytes>28) {
            std::cout << "Cannot enable " << field.name << ": it needs "
                      << field.bytes << (field.bytes==1?" byte":" bytes")
                      << ", but only " << 28-currentBytes
                      << (28-currentBytes==1?" byte is":" bytes are")
                      << " available.\n";
            continue;
        }
        for (std::size_t i=0;i<mask.size();++i) {
            if(enabled)mask[i]|=field.bits[i];
            else mask[i]&=static_cast<std::uint8_t>(~field.bits[i]);
        }
        std::cout << "Resulting internal record length: "
                  << unsigned(open1v::shootingDataRecordWidth(mask))
                  << " bytes\n";
        std::cout << "Warning: changing recorded fields can split a partially shot film "
                     "into a new logical roll segment.\nApply and verify this change? [y/n]: "
                  << std::flush;
        if (!std::getline(std::cin,input)) return;
        input=normalized(input);
        if (input!="y") { std::cout << "Setting change cancelled.\n"; continue; }
        camera.setShootingDataMask(mask);
        std::cout << "Verified: " << field.name << " is now "
                  << (enabled?"ON":"OFF") << ".\n";
    }
}

std::string readMenuInput(bool immediateMonthKeys = false) {
#if defined(_WIN32)
    if (immediateMonthKeys && _isatty(_fileno(stdin))) {
        const int key = _getch();
        if (key == 'p' || key == 'P' || key == 'n' || key == 'N' ||
            key == 'q' || key == 'Q') {
            std::cout << static_cast<char>(key) << '\n';
            return normalized(std::string(1, static_cast<char>(key)));
        }
        if (key >= '0' && key <= '9') {
            std::cout << static_cast<char>(key) << std::flush;
            std::string remainder;
            if (!std::getline(std::cin, remainder)) return {};
            return normalized(std::string(1, static_cast<char>(key)) + remainder);
        }
        std::cout << '\n';
        return {};
    }
#else
    if (immediateMonthKeys && isatty(fileno(stdin))) {
        termios saved{};
        if (tcgetattr(fileno(stdin), &saved) == 0) {
            termios immediate = saved;
            immediate.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
            if (tcsetattr(fileno(stdin), TCSANOW, &immediate) == 0) {
                const int key = std::getchar();
                tcsetattr(fileno(stdin), TCSANOW, &saved);
                if (key == EOF) return {};
                if (key == 'p' || key == 'P' || key == 'n' || key == 'N' ||
                    key == 'q' || key == 'Q') {
                    std::cout << static_cast<char>(key) << '\n';
                    return normalized(std::string(1, static_cast<char>(key)));
                }
                if (key >= '0' && key <= '9') {
                    std::cout << static_cast<char>(key) << std::flush;
                    std::string remainder;
                    if (!std::getline(std::cin, remainder)) return {};
                    return normalized(std::string(1, static_cast<char>(key)) + remainder);
                }
                std::cout << '\n';
                return {};
            }
        }
    }
#endif
    std::string input;
    if (!std::getline(std::cin, input)) return {};
    return normalized(input);
}

std::string yearMonth(int year, int month) {
    std::ostringstream out;
    out << year << '-' << (month < 10 ? "0" : "") << month;
    return out.str();
}

void moveMonth(int& year, int& month, int offset) {
    month += offset;
    if (month < 1) { month = 12; --year; }
    if (month > 12) { month = 1; ++year; }
}

void viewDatabase(const std::filesystem::path& database) {
#if defined(_WIN32)
    SYSTEMTIME local{};
    GetLocalTime(&local);
    int year = local.wYear;
    int month = local.wMonth;
#else
    const auto now = std::time(nullptr);
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr)
        throw std::runtime_error("cannot read local system time");
    int year = local.tm_year + 1900;
    int month = local.tm_mon + 1;
#endif

    for (;;) {
        const auto monthText = yearMonth(year, month);
        const auto dates = filmrecorder::listImportDates(database, monthText);
        std::cout << "\nImport dates for " << monthText << "\n";
        if (dates.empty()) std::cout << "  (no imports)\n";
        for (std::size_t i = 0; i < dates.size(); ++i)
            std::cout << i + 1 << ") " << dates[i] << '\n';
        std::cout << "p) Previous month  n) Next month  q) Back\nview/date> " << std::flush;

        std::string input = readMenuInput(true);
        if (!std::cin) return;
        if (input == "q") return;
        if (input == "p") { moveMonth(year, month, -1); continue; }
        if (input == "n") { moveMonth(year, month, 1); continue; }
        const auto selectedDate = menuIndex(input, dates.size());
        if (!selectedDate) { std::cout << "Invalid selection.\n"; continue; }

        for (;;) {
            const auto rolls = filmrecorder::listRollsForDate(database, dates[*selectedDate]);
            std::cout << "\nRolls imported on " << dates[*selectedDate] << "\n";
            if (rolls.empty()) std::cout << "  (no rolls)\n";
            for (std::size_t i = 0; i < rolls.size(); ++i) {
                const auto& roll = rolls[i];
                std::cout << i + 1 << ") " << roll.importTime
                          << " | Roll ID " << roll.rollId
                          << " | Film " << roll.filmId
                          << " | " << roll.frameCount << " frame(s)\n";
            }
            std::cout << "q) Back\nview/roll> " << std::flush;
            if (!std::getline(std::cin, input)) return;
            input = normalized(input);
            if (input == "q") break;
            const auto selectedRoll = menuIndex(input, rolls.size());
            if (!selectedRoll) { std::cout << "Invalid selection.\n"; continue; }

            for (;;) {
                const auto frames = filmrecorder::listFramesForRoll(
                    database, rolls[*selectedRoll].rollId);
                std::cout << "\nFrames for Roll ID "
                          << rolls[*selectedRoll].rollId << "\n";
                if (frames.empty()) std::cout << "  (no frames)\n";
                for (std::size_t i = 0; i < frames.size(); ++i) {
                    const auto& frame = frames[i];
                    std::cout << i + 1 << ") Index: " << frame.frameIndex
                              << " | Shutter Speed: " << frame.shutterSpeed
                              << " | Aperture: " << frame.aperture
                              << " | ISO: " << frame.iso
                              << " | Focal Length (mm): " << frame.focalLength
                              << " | Shooting Date/Time: " << frame.capturedAt
                              << '\n';
                }
                std::cout << "q) Back\nview/frame> " << std::flush;
                if (!std::getline(std::cin, input)) return;
                input = normalized(input);
                if (input == "q") break;
                const auto selectedFrame = menuIndex(input, frames.size());
                if (!selectedFrame) { std::cout << "Invalid selection.\n"; continue; }

                std::cout << '\n' << filmrecorder::describeFrame(
                    database, frames[*selectedFrame].frameId);
                do {
                    std::cout << "q) Back\nview/detail> " << std::flush;
                    if (!std::getline(std::cin, input)) return;
                    input = normalized(input);
                } while (input != "q");
            }
        }
    }
}

void interactive(const Options& options) {
    std::cout << "EOS-1V Film Record interactive mode\n";
    for (;;) {
        std::cout << "\n"
                     "sync - Download camera film records to the local SQLite database\n"
                     "clear - Permanently delete all film records from the camera\n"
                     "set - Select which shooting-data fields the camera records\n"
                     "view - Browse imported rolls by month and date\n"
                     "help - Show this command table again\n"
                     "exit - Close the program\n\n";
        std::cout << "film-record> " << std::flush;
        std::string command;
        if (!std::getline(std::cin, command)) { std::cout << '\n'; return; }
        command = normalized(command);
        if (command.empty()) continue;
        if (command == "exit" || command == "quit") return;
        if (command == "help") continue;
        try {
            if (command == "sync") syncRecords(options);
            else if (command == "set") setShootingData(options);
            else if (command == "view")
                viewDatabase(executableDirectory() / "film-records.sqlite3");
            else if (command == "clear") {
                if (confirmClear()) clearRecords(options);
                else std::cout << "Clear cancelled.\n";
            } else std::cout << "Unknown command. Enter help to list commands.\n";
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << '\n';
        }
    }
}

void selfTest() {
    std::string error;
    if (!filmrecorder::runSelfTest(error)) throw std::runtime_error("Self-test failed: " + error);
    if (open1v::shootingDataRecordWidth(
            {0xf6,0x09,0,0,0,0,0,0}) != 0x08 ||
        open1v::shootingDataRecordWidth(
            {0xff,0xff,0,0,0,0,0,0}) != 0x10 ||
        open1v::shootingDataRecordWidth(
            {0xff,0xff,0x0c,0x3f,0,0x08,0x7f,0}) != 0x20)
        throw std::runtime_error("shooting-data record-width self-test failed");
    const auto testDb = std::filesystem::temp_directory_path() / "film-record-self-test.sqlite3";
    std::error_code ignored; std::filesystem::remove(testDb, ignored);
    filmrecorder::Download fixture; fixture.reportedRolls = 1; fixture.rolls.emplace_back();
    auto& roll = fixture.rolls.back(); roll.filmId = "00 03 29"; roll.recordWidth = 32;
    roll.fieldMaskHex = "FF FF 0C 3F 00 08 7F 00"; roll.dxIsoWire = 0x48; roll.dxIso = 100;
    roll.rawHeaderHex = "E3"; roll.frames.emplace_back(); auto& frame = roll.frames.back();
    frame.number = 1; frame.focalLengthPresent = true; frame.focalLengthMm = 50;
    frame.maxAperturePresent = true; frame.maxApertureWire = 12; frame.maxApertureF = 2.828427;
    frame.shutterPresent = true; frame.shutterWire = 0x40; frame.shutterSeconds = 1.0 / 256;
    frame.shutterDisplay = "1/256 s"; frame.aperturePresent = true; frame.apertureWire = 0x18;
    frame.apertureF = 8.0; frame.shootingMode = "Aperture-priority AE"; frame.rawHex = "E4";
    frame.cfnValues = "0,1,0,3,1,0,0,1,0,0,0,0,0,0,0,0,0,0,0";
    roll.frames.push_back(frame); auto& markedFrame = roll.frames.back();
    markedFrame.number = 2; markedFrame.aebPosition = "Underexposed";
    markedFrame.multipleExposure = true;
    filmrecorder::saveToDatabase(testDb, fixture, "self-test");
    const auto report = filmrecorder::inspectDatabase(testDb);
    const auto plainDetail = filmrecorder::describeFrame(testDb, 1);
    const auto markedDetail = filmrecorder::describeFrame(testDb, 2);
    if (report.find("Frames: 2") == std::string::npos ||
        report.find("Invalid import date/time rows: 0") == std::string::npos ||
        report.find("Invalid C.Fn rows: 0") == std::string::npos ||
        plainDetail.find("AEB Position:") != std::string::npos ||
        plainDetail.find("Multiple Exposure:") != std::string::npos ||
        markedDetail.find("AEB Position: Underexposed") == std::string::npos ||
        markedDetail.find("Multiple Exposure: Yes") == std::string::npos)
        throw std::runtime_error("database self-test verification failed");
    std::filesystem::remove(testDb, ignored);
    std::cout << "Self-test passed.\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "self-test") {
            if (argc != 2) throw std::runtime_error("self-test accepts no options");
            selfTest(); return 0;
        }
        if (argc >= 2 && std::string(argv[1]) == "inspect") {
            std::filesystem::path database = executableDirectory() / "film-records.sqlite3";
            if (argc == 4 && std::string(argv[2]) == "--database") database = argv[3];
            else if (argc != 2) throw std::runtime_error("inspect accepts only --database FILE");
            std::cout << "Database: " << std::filesystem::absolute(database).string() << '\n'
                      << filmrecorder::inspectDatabase(database);
            return 0;
        }
        if (argc >= 2 && std::string(argv[1]) == "view") {
            std::filesystem::path database = executableDirectory() / "film-records.sqlite3";
            if (argc == 4 && std::string(argv[2]) == "--database") database = argv[3];
            else if (argc != 2) throw std::runtime_error("view accepts only --database FILE");
            viewDatabase(database);
            return 0;
        }
        if (argc >= 2 && (std::string(argv[1]) == "sync" || std::string(argv[1]) == "download")) {
            syncRecords(parseOptions(argc, argv, 2, true)); return 0;
        }
        if (argc >= 2 && std::string(argv[1]) == "clear") {
            const auto options = parseOptions(argc, argv, 2, false);
            if (confirmClear()) clearRecords(options); else std::cout << "Clear cancelled.\n";
            return 0;
        }
        if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
            usage(); return 0;
        }
        interactive(parseOptions(argc, argv, 1, false));
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n'; return 1;
    }
}
