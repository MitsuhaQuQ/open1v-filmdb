# EOS-1V Film Database

A Windows command-line application for downloading, decoding, and storing Canon EOS-1V film shooting records.

## Features

- Downloads all film-record segments through an UNO R4 or compatible bridge
- Reuses the tested serial, WinUSB, bridge, and camera-session layers from `open1V-cli`
- Stores decoded records in a local SQLite database
- Preserves one complete raw E3 packet per roll and one complete raw E4 packet per frame
- Decodes fields according to each roll's dynamic shooting-field mask
- Keeps unknown enum values visible as `Unknown(0xNN)` instead of guessing
- Supports optional JSON and CSV exports

## Clone

The connection library is included as a Git submodule:

```powershell
git clone --recurse-submodules https://github.com/MitsuhaQuQ/open1v-filmdb.git
cd open1v-filmdb
```

If the repository was cloned without submodules:

```powershell
git submodule update --init --recursive
```

## Build

Visual Studio with the Desktop development with C++ workload is required.

```powershell
.\build.cmd
```

The Release executable is written to `x64\Release\film-record.exe`.

## Usage

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

Interactive mode accepts these commands:

- `sync` first checks the camera's E1 roll count. If it is zero, the command returns without creating an empty import; otherwise it downloads and decodes all film records into the local SQLite database.
- `clear` permanently deletes all film records stored by the camera. It requires an explicit `y`; entering `n` returns to the prompt without changing the camera.
- `view` opens the database browser at the current month. Dates and rolls use numbered menus; `p` and `n` change month, and `q` returns to the preceding menu.
- `help` shows the command list.
- `exit` closes the program.

The clear operation sends the verified, parameterless E2 command exactly once, keeps the same PC session open, and polls E1 until it reports zero rolls before refreshing FC. If the acknowledgement or verification is uncertain, the application reports an error and does not automatically retry the destructive command.

The camera must be in PC mode before each new connection. A completed download exits PC mode normally.

## SQLite storage

The default database is `film-records.sqlite3` beside `film-record.exe`, independent of the process working directory. Repeated downloads create new import batches and do not overwrite earlier records. Use `--output` to select another path.

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
