#include "filmrecorder/film_records.hpp"
#include "filmrecorder/database.hpp"
#include "open1v/bridge_client.hpp"
#include "open1v/serial_transport.hpp"
#include "open1v/winusb_transport.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace {
std::filesystem::path executableDirectory() {
    std::wstring buffer(260, L'\0');
    while (true) {
        const auto length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
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
                 "  film-record download [--port COM3 | --winusb] [--format sqlite|json|csv] [--output FILE]\n"
                 "  film-record inspect [--database FILE]\n"
                 "  film-record self-test\n";
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "self-test") {
            std::string error;
            if (!filmrecorder::runSelfTest(error)) {
                std::cerr << "Self-test failed: " << error << '\n'; return 1;
            }
            const auto testDb = std::filesystem::temp_directory_path() / "film-record-self-test.sqlite3";
            std::error_code ignored; std::filesystem::remove(testDb, ignored);
            filmrecorder::Download fixture; fixture.reportedRolls=1; fixture.rolls.emplace_back();
            auto& roll=fixture.rolls.back(); roll.filmId="00 03 29"; roll.recordWidth=32;
            roll.fieldMaskHex="FF FF 0C 3F 00 08 7F 00"; roll.dxIsoWire=0x48; roll.dxIso=100;
            roll.rawHeaderHex="E3"; roll.frames.emplace_back(); auto& frame=roll.frames.back();
            frame.number=1; frame.focalLengthPresent=true; frame.focalLengthMm=50;
            frame.maxAperturePresent=true; frame.maxApertureWire=12; frame.maxApertureF=2.828427;
            frame.shutterPresent=true; frame.shutterWire=0x40; frame.shutterSeconds=1.0/256; frame.shutterDisplay="1/256 s";
            frame.aperturePresent=true; frame.apertureWire=0x18; frame.apertureF=8.0;
            frame.shootingMode="Aperture-priority AE"; frame.rawHex="E4";
            frame.cfnValues="0,1,0,3,1,0,0,1,0,0,0,0,0,0,0,0,0,0,0";
            filmrecorder::saveToDatabase(testDb, fixture, "self-test");
            const auto report=filmrecorder::inspectDatabase(testDb);
            if(report.find("Frames: 1") == std::string::npos || report.find("Invalid C.Fn rows: 0") == std::string::npos)
                throw std::runtime_error("database self-test verification failed");
            std::filesystem::remove(testDb, ignored);
            std::cout << "Self-test passed.\n"; return 0;
        }
        if (argc >= 2 && std::string(argv[1]) == "inspect") {
            std::filesystem::path database = executableDirectory() / "film-records.sqlite3";
            if (argc == 4 && std::string(argv[2]) == "--database") database = argv[3];
            else if (argc != 2) throw std::runtime_error("inspect accepts only --database FILE");
            std::cout << "Database: " << std::filesystem::absolute(database).string() << '\n'
                      << filmrecorder::inspectDatabase(database);
            return 0;
        }
        if (argc < 2 || std::string(argv[1]) != "download") { usage(); return argc < 2 ? 0 : 2; }

        std::string port, format = "sqlite", output;
        bool winusb = false;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--winusb") winusb = true;
            else if ((arg == "--port" || arg == "--format" || arg == "--output") && i + 1 < argc) {
                const std::string value = argv[++i];
                if (arg == "--port") port = value;
                else if (arg == "--format") format = value;
                else output = value;
            } else throw std::runtime_error("unknown or incomplete option: " + arg);
        }
        if (winusb && !port.empty()) throw std::runtime_error("choose either --port or --winusb");
        if (format != "sqlite" && format != "json" && format != "csv")
            throw std::runtime_error("format must be sqlite, json or csv");
        if (output.empty()) output = format == "sqlite" ?
            (executableDirectory() / "film-records.sqlite3").string() :
            "film-records." + format;

        std::unique_ptr<open1v::ITransport> transport;
        if (winusb) transport = std::make_unique<open1v::WinUsbTransport>();
        else transport = std::make_unique<open1v::SerialTransport>(
            std::wstring(port.begin(), port.end()));
        open1v::BridgeClient bridge(*transport);
        open1v::CameraProtocolSession camera(bridge);

        std::cout << "Downloading Film Records from EOS-1V...\n";
        const auto packets = camera.readOnce(open1v::CameraRead::filmRecords);
        const auto download = filmrecorder::parsePackets(packets);
        const std::filesystem::path path(output);
        std::size_t frames = 0;
        for (const auto& roll : download.rolls) frames += roll.frames.size();
        if (format == "sqlite") {
            const auto saved = filmrecorder::saveToDatabase(
                path, download, winusb ? "winusb" : (port.empty() ? "serial-auto" : port.c_str()));
            std::cout << "Saved import " << saved.importId << ": " << saved.rolls
                      << " roll(s), " << saved.frames << " frame(s) to ";
        } else {
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
            std::ofstream stream(path, std::ios::binary);
            if (!stream) throw std::runtime_error("cannot open output file: " + output);
            stream << (format == "json" ? filmrecorder::toJson(download) :
                                           filmrecorder::toCsv(download));
            if (!stream) throw std::runtime_error("failed while writing output file: " + output);
            std::cout << "Saved " << download.rolls.size() << " roll(s), " << frames << " frame(s) to ";
        }
        std::cout << std::filesystem::absolute(path).string() << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n'; return 1;
    }
}
