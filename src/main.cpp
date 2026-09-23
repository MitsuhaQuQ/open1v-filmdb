#include "filmrecorder/application.hpp"
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

void syncRecords(Options options, open1v::CameraProtocolSession& camera,
                 bool keepSession) {
    if (options.output.empty()) {
        options.output = options.format == "sqlite"
            ? (executableDirectory() / "film-records.sqlite3").string()
            : "film-records." + options.format;
    }
    const filmrecorder::SyncRequest request{
        options.format == "sqlite" ? filmrecorder::ExportFormat::sqlite :
        options.format == "json" ? filmrecorder::ExportFormat::json :
                                   filmrecorder::ExportFormat::csv,
        options.output,
        options.winusb ? "winusb" :
            (options.port.empty() ? "serial-auto" : options.port),
        keepSession};
    const auto result = filmrecorder::syncFilmRecords(camera, request);
    if (result.cameraEmpty) {
        std::cout << "Camera film-record storage is empty; nothing to sync.\n";
        return;
    }
    if (request.format == filmrecorder::ExportFormat::sqlite)
        std::cout << "Saved import " << result.importId << ": ";
    else std::cout << "Saved ";
    std::cout << result.rolls << " roll(s), " << result.frames
              << " frame(s) to " << result.output.string() << '\n';
}

void syncRecords(Options options) {
    auto transport=makeTransport(options);
    open1v::BridgeClient bridge(*transport);
    open1v::CameraProtocolSession camera(bridge);
    syncRecords(std::move(options),camera,false);
}

void clearRecords(open1v::CameraProtocolSession& camera, bool keepSession) {
    std::cout << "Clearing all film records from EOS-1V...\n";
    filmrecorder::clearFilmRecords(camera, keepSession);
    std::cout << "Camera film-record storage is empty (verified by E1).\n";
}

void clearRecords(const Options& options) {
    auto transport=makeTransport(options);
    open1v::BridgeClient bridge(*transport);
    open1v::CameraProtocolSession camera(bridge);
    clearRecords(camera,false);
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

void setShootingData(open1v::CameraProtocolSession& camera) {
    if(!camera.sessionActive())camera.beginSession();
    auto selection=filmrecorder::ShootingDataSelection::fromPackets(
        camera.perform(open1v::CameraRead::settings));
    const auto& fields=filmrecorder::ShootingDataSelection::fields();
    for (;;) {
        const auto usedBytes=selection.usedBytes();
        std::cout << "\nFilm shooting-data fields\n";
        for (std::size_t i=0;i<fields.size();++i) {
            const bool enabled=selection.enabled(i);
            const bool changed=selection.changed(i);
            std::cout << (changed?'*':' ') << i+1 << ") [" << (enabled?"ON ":"OFF")
                      << "] " << fields[i].name << " {" << fields[i].bytes
                      << (fields[i].bytes==1?" byte}":" bytes}");
            if (!enabled && usedBytes+fields[i].bytes>28)
                std::cout << " - cannot enable: limit exceeded";
            std::cout << '\n';
        }
        std::cout << "Current usage: " << usedBytes
                  << "/28 bytes | Internal record length: "
                  << selection.recordWidth() << " bytes\n"
                  << "* marks staged changes\n"
                  << "Enter 1..18 to toggle, commit, discard, or q\nset> " << std::flush;
        std::string input;
        if (!std::getline(std::cin,input)) return;
        input=normalized(input);
        if (input=="q" || input=="discard") {
            std::cout << "Shooting-data changes discarded.\n";
            return;
        }
        if(input=="commit") {
            if(!selection.dirty()) {
                std::cout << "No shooting-data changes to commit.\n";
                return;
            }
            std::cout << "Warning: changing recorded fields can split a partially shot film "
                         "into a new logical roll segment.\n";
            camera.setShootingDataMask(selection.mask());
            std::cout << "Shooting-data changes committed and verified.\n";
            return;
        }
        const auto selected=menuIndex(input,fields.size());
        if (!selected) { std::cout << "Invalid selection.\n"; continue; }

        const auto& field=fields[*selected];
        const bool current=selection.enabled(*selected);
        const bool enabled=!current;
        const auto currentBytes=selection.usedBytes();
        if (enabled && currentBytes+field.bytes>28) {
            std::cout << "Cannot enable " << field.name << ": it needs "
                      << field.bytes << (field.bytes==1?" byte":" bytes")
                      << ", but only " << 28-currentBytes
                      << (28-currentBytes==1?" byte is":" bytes are")
                      << " available.\n";
            continue;
        }
        try {
            selection.toggle(*selected);
        } catch(const std::exception& ex) {
            std::cout << "Cannot toggle " << field.name << ": " << ex.what() << ".\n";
        }
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
    auto transport=makeTransport(options);
    open1v::BridgeClient bridge(*transport);
    open1v::CameraProtocolSession camera(bridge);
    std::cout << "EOS-1V Film Record interactive mode\n";
    for (;;) {
        std::cout << "\n"
                     "sync - Download camera film records to the local SQLite database\n"
                     "clear - Permanently delete all film records from the camera\n"
                     "set - Select which shooting-data fields the camera records\n"
                     "stop - End the active camera session\n"
                     "view - Browse imported rolls by month and date\n"
                     "help - Show this command table again\n"
                     "exit - Close the program\n\n";
        std::cout << "film-record> " << std::flush;
        std::string command;
        if (!std::getline(std::cin, command)) {
            std::cout << '\n';
            try { if(camera.sessionActive())camera.endSession(); } catch(...) {}
            return;
        }
        command = normalized(command);
        if (command.empty()) continue;
        if (command == "exit" || command == "quit") {
            if(camera.sessionActive()) {
                std::cout << "Camera session is active; enter stop before exit.\n";
                continue;
            }
            return;
        }
        if (command == "help") continue;
        try {
            if (command == "sync") syncRecords(options,camera,true);
            else if (command == "set") setShootingData(camera);
            else if(command=="stop") {
                if(camera.sessionActive()) {
                    camera.endSession();
                    std::cout << "Camera session ended.\n";
                } else std::cout << "No active camera session.\n";
            }
            else if (command == "view")
                viewDatabase(executableDirectory() / "film-records.sqlite3");
            else if (command == "clear") {
                if (confirmClear()) clearRecords(camera,true);
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
    std::vector<open1v::CameraPacket> settingPackets{{
        "E8", {0xe8,0x08,0xf6,0x09,0,0,0,0,0,0,0}}};
    auto selection=filmrecorder::ShootingDataSelection::fromPackets(settingPackets);
    const auto initialBytes=selection.usedBytes();
    selection.toggle(0);
    if(!selection.dirty() || selection.usedBytes()==initialBytes)
        throw std::runtime_error("shooting-data selection model self-test failed");
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
    frame.capturedAt = "2026-09-22T12:00:01";
    frame.cfnValues = "0,1,0,3,1,0,0,1,0,0,0,0,0,0,0,0,0,0,0";
    roll.frames.push_back(frame); auto& markedFrame = roll.frames.back();
    markedFrame.number = 2; markedFrame.aebPosition = "Underexposed";
    markedFrame.multipleExposure = true; markedFrame.capturedAt = "2026-09-22T12:00:02";
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
    auto appended=fixture;
    appended.rolls[0].recordWidth=16;
    appended.rolls[0].fieldMaskHex="FF FF 00 00 00 00 00 00";
    appended.rolls[0].frames.push_back(markedFrame);
    appended.rolls[0].frames.back().number=3;
    appended.rolls[0].frames.back().capturedAt="2026-09-22T12:00:03";
    const auto appendedResult=filmrecorder::saveToDatabase(testDb,appended,"self-test-append");
    const auto appendedReport=filmrecorder::inspectDatabase(testDb);
    if(appendedResult.rolls!=1 || appendedResult.frames!=1 ||
       appendedReport.find("Imports: 2") == std::string::npos ||
       appendedReport.find("Rolls: 1") == std::string::npos ||
       appendedReport.find("Frames: 3") == std::string::npos ||
       filmrecorder::listFramesForRoll(testDb,1).size()!=3)
        throw std::runtime_error("same-roll append self-test failed");
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
