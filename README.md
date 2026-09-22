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
# Offline parser and database tests
.\x64\Release\film-record.exe self-test

# Automatically detected compatible bridge
.\x64\Release\film-record.exe download

# Explicit serial port
.\x64\Release\film-record.exe download --port COM3

# WinUSB
.\x64\Release\film-record.exe download --winusb

# CSV export
.\x64\Release\film-record.exe download --port COM3 --format csv --output .\exports\film-records.csv

# Inspect the default database
.\x64\Release\film-record.exe inspect
```

The camera must be in PC mode before each new connection. A completed download exits PC mode normally.

## SQLite storage

The default database is `film-records.sqlite3` beside `film-record.exe`, independent of the process working directory. Repeated downloads create new import batches and do not overwrite earlier records. Use `--output` to select another path.

The database contains three main tables:

- `imports` — one row for each completed download
- `rolls` — decoded E3 roll metadata and the complete `raw_e3` packet
- `frames` — decoded E4 frame data and the complete `raw_e4` packet

Raw transport values are not duplicated into individual columns. Decoded columns contain values intended for browsing and queries. The 19 C.Fn option numbers are stored in `cfn_values` as one comma-separated string.

Recognized fields include DX/manual ISO, focal length, maximum and selected aperture, shutter time, exposure compensation, flash compensation, flash mode, metering mode, shooting mode, film advance, AF mode, multiple exposure, capture time, C.Fn settings, and battery-load time. Fields disabled by the roll mask are stored as `NULL`.

## Documentation

- [E3/E4/EFD film-record field map](docs/e3-e4-efd-field-map.md)
- [SQLite data model](docs/data-model.md)
- [Test fixture policy](fixtures/README.md)
