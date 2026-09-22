#include "filmrecorder/database.hpp"
#include "filmrecorder/film_records.hpp"
#include "open1v/bridge_client.hpp"
#include "open1v/serial_transport.hpp"
#include "open1v/winusb_transport.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace {
struct Options {
    std::string port;
    bool winusb{};
    std::string format{"sqlite"};
    std::string output;
};

std::filesystem::path executableDirectory() {
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
}

void usage() {
    std::cout << "EOS-1V Film Record downloader\n\n"
                 "Usage:\n"
                 "  film-record [--port COM3 | --winusb]\n"
                 "  film-record sync [--port COM3 | --winusb] [--format sqlite|json|csv] [--output FILE]\n"
                 "  film-record clear [--port COM3 | --winusb]\n"
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
    return std::make_unique<open1v::SerialTransport>(std::wstring(options.port.begin(), options.port.end()));
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
    std::cout << "Downloading film records from EOS-1V...\n";
    const auto download = filmrecorder::parsePackets(camera.readOnce(open1v::CameraRead::filmRecords));
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

void interactive(const Options& options) {
    std::cout << "EOS-1V Film Record interactive mode\nCommands: sync, clear, help, exit\n";
    for (;;) {
        std::cout << "film-record> " << std::flush;
        std::string command;
        if (!std::getline(std::cin, command)) { std::cout << '\n'; return; }
        command = normalized(command);
        if (command.empty()) continue;
        if (command == "exit" || command == "quit") return;
        if (command == "help") {
            std::cout << "  sync   Download camera film records to the local SQLite database\n"
                         "  clear  Permanently delete all film records from the camera\n"
                         "  exit   Close the program\n";
            continue;
        }
        try {
            if (command == "sync") syncRecords(options);
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
    filmrecorder::saveToDatabase(testDb, fixture, "self-test");
    const auto report = filmrecorder::inspectDatabase(testDb);
    if (report.find("Frames: 1") == std::string::npos || report.find("Invalid C.Fn rows: 0") == std::string::npos)
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
