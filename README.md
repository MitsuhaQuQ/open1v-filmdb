# EOS-1V Film Database

A Windows, macOS, and Linux command-line application for downloading, decoding,
and storing Canon EOS-1V film shooting records.

## Features

- Downloads all film-record segments through an UNO R4 or compatible bridge
- Reuses the tested serial, bridge, and camera-session layers from `open1V-cli`
- Uses the native COM API on Windows and a shared POSIX termios/poll serial layer
  on macOS and Linux; WinUSB remains available on Windows
- Stores decoded records in a local SQLite database
- Preserves one complete raw E3 packet per roll and one complete raw E4 packet per frame
- Decodes fields according to each roll's dynamic shooting-field mask
- Keeps unknown enum values visible as `Unknown(0xNN)` instead of guessing
- Supports optional JSON and CSV exports

## Clone

The connection library is included as a Git submodule:

```sh
git clone --recurse-submodules https://github.com/MitsuhaQuQ/open1v-filmdb.git
cd open1v-filmdb
```

If the repository was cloned without submodules:

```sh
git submodule update --init --recursive
```

## Build

### Windows

Visual Studio with the Desktop development with C++ workload is required:

```powershell
.\build.cmd
```

The Release executable is written to `x64\Release\film-record.exe`.

### macOS and Linux

A C++20 compiler, CMake 3.20 or newer, and SQLite 3 development files are
required. macOS provides SQLite through the SDK. On Linux, install the SQLite
development package supplied by the distribution, then run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The executable is written to `build/film-record`.

## Usage

### Windows examples

```powershell
# Start interactive mode with automatic bridge detection
.\x64\Release\film-record.exe

# Start interactive mode on an explicit serial port
.\x64\Release\film-record.exe --port COM3

# Offline parser and database tests
.\x64\Release\film-record.exe self-test

# Non-interactive sync with automatic bridge detection
.\x64\Release\film-record.exe sync

# Non-interactive sync on an explicit serial port
.\x64\Release\film-record.exe sync --port COM3

# WinUSB
.\x64\Release\film-record.exe sync --winusb

# CSV export
.\x64\Release\film-record.exe sync --port COM3 --format csv --output .\exports\film-records.csv

# Inspect the default database
.\x64\Release\film-record.exe inspect

# Browse the default database interactively
.\x64\Release\film-record.exe view
```

### macOS and Linux examples

```sh
# Start interactive mode with automatic UNO R4 detection
./build/film-record

# Use a specific macOS serial device
./build/film-record sync --port /dev/cu.usbmodem1101

# Use a specific Linux serial device
./build/film-record sync --port /dev/ttyACM0

# Offline parser and database tests
./build/film-record self-test

# Export CSV
./build/film-record sync --format csv --output ./exports/film-records.csv

# Inspect or browse the default database
./build/film-record inspect
./build/film-record view
```

Automatic serial discovery recognizes the supported UNO R4 and experimental
Minima ES-E1-ID USB identities. The `--winusb` option is Windows-only.

Interactive mode accepts these commands:

- `sync` first checks the camera's E1 roll-count snapshot. If it is zero, the
  command returns without creating an empty import; otherwise it downloads and
  decodes records until the camera returns the explicit E3 all-end packet. E1
  is retained as reported metadata but is not treated as a hard iteration limit,
  because the available segment count can occasionally change during a sync.
- `clear` permanently deletes all film records stored by the camera. It requires an explicit `y`; entering `n` returns to the prompt without changing the camera.
- `set` reads the camera's current shooting-data field mask and lists all 18 selectable recorded fields as a numbered ON/OFF menu. The current and resulting internal record lengths are shown separately as 8, 16, or 32 bytes. Selecting a field offers numbered ON/OFF choices and requires `y` confirmation before writing. The length calculation includes four fixed bytes, while composite bits, field dependencies, and the 28-byte optional-data limit are validated before the E7/E9 write. E8 read-back verifies the result. Changing this setting while a film is partly exposed can create a new logical roll segment.
- `view` opens the database browser at the current month. Dates, rolls, and frames use numbered menus; `p` and `n` change month immediately without Enter, and `q` returns to the preceding menu. The first frame level shows Index, Shutter Speed, Aperture, ISO, Focal Length, and Shooting Date/Time. Aperture and shutter speed are normalized to familiar whole-, half-, or third-stop camera values instead of raw conversion decimals. ISO is shown as an integer, preferring the roll's DX value and falling back to the frame's manual ISO. Selecting a frame displays one decoded `Field: value` entry per line; missing values display as `n/a`. The 19 comma-separated C.Fn option numbers remain together as one `C.Fn Values` entry.
- `help` shows the command list.
- `exit` closes the program.

The full `command - description` table is printed whenever control returns to the main menu. In roll details, every frame record has its own number and a blank line after it for readability.

The clear operation sends the verified, parameterless E2 command exactly once, keeps the same PC session open, and polls E1 until it reports zero rolls before refreshing FC. If the acknowledgement or verification is uncertain, the application reports an error and does not automatically retry the destructive command.

The camera must be in PC mode before each new connection. A completed download exits PC mode normally.

## SQLite storage

The default database is `film-records.sqlite3` beside the executable, independent
of the process working directory. Repeated downloads create new import batches
and do not overwrite earlier records. Use `--output` to select another path.

The database contains three main tables:

- `imports` — one row for each completed download, including separate local-system import date and time fields
- `rolls` — decoded E3 roll metadata and the complete `raw_e3` packet
- `frames` — decoded E4 frame data and the complete `raw_e4` packet

Raw transport values are not duplicated into individual columns. Decoded columns contain values intended for browsing and queries. The 19 C.Fn option numbers are stored in `cfn_values` as one comma-separated string.

Recognized fields include DX/manual ISO, focal length, maximum and selected aperture, shutter time, exposure compensation, flash compensation, flash mode, metering mode, shooting mode, film advance, AF mode, multiple exposure, capture time, C.Fn settings, and battery-load time. Fields disabled by the roll mask are stored as `NULL`.

## Documentation

- [E3/E4/EFD film-record field map](docs/e3-e4-efd-field-map.md)
- [SQLite data model](docs/data-model.md)
- [Test fixture policy](fixtures/README.md)
