# Building on macOS and Linux

## Requirements

- A compiler with C++20 support
- CMake 3.20 or newer
- SQLite 3 headers and library
- Git, including submodule support

The application uses the POSIX serial transport on macOS and Linux. WinUSB is
not available on these platforms. macOS links IOKit and CoreFoundation for
automatic serial-device discovery; Linux uses the standard POSIX device APIs
and does not require libusb or libudev.

## Clone with the transport submodule

```sh
git clone --recurse-submodules https://github.com/MitsuhaQuQ/open1v-filmdb.git
cd open1v-filmdb
```

For an existing clone:

```sh
git submodule update --init --recursive
```

## macOS

Install Apple's command-line tools and CMake:

```sh
xcode-select --install
brew install cmake
```

Configure and build with the default Unix Makefiles generator:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Ninja may be used instead:

```sh
brew install ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The macOS SDK normally supplies SQLite. If `find_package(SQLite3)` fails, use
Homebrew SQLite explicitly:

```sh
brew install sqlite
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix sqlite)"
```

The build is native to the terminal architecture. Run the commands in a native
arm64 terminal for Apple Silicon or an x86_64 terminal for an Intel build.

Typical serial devices can be listed with:

```sh
ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null
```

Use the callout device explicitly if automatic discovery is not appropriate:

```sh
./build/film-record --port /dev/cu.usbmodem1101
```

## Linux

Install a compiler, CMake, and SQLite development files.

Debian or Ubuntu:

```sh
sudo apt update
sudo apt install build-essential cmake libsqlite3-dev
```

Fedora:

```sh
sudo dnf install gcc-c++ cmake sqlite-devel
```

Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake sqlite
```

Configure, build, and test:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For Ninja, install the distribution's `ninja-build` or `ninja` package and add
`-G Ninja` to the configure command.

Typical UNO R4 devices can be checked with:

```sh
ls -l /dev/ttyACM* 2>/dev/null
```

Use a device explicitly when needed:

```sh
./build/film-record --port /dev/ttyACM0
```

If access is denied, identify the device group from `ls -l`. Debian, Ubuntu,
and many related distributions normally use `dialout`; Arch commonly uses
`uucp`:

```sh
sudo usermod -aG dialout "$USER"
# or, when the device belongs to uucp:
sudo usermod -aG uucp "$USER"
```

Sign out and back in after changing group membership. A temporary root shell is
not recommended because it can create a root-owned database beside the binary.

## Build results and tests

The primary executable is:

```text
build/film-record
```

The test suite includes the FilmDB parser/database self-test and the transport
submodule's offline protocol and POSIX serial tests. No camera is required by
`ctest`.

Run the main self-test directly with:

```sh
./build/film-record self-test
```

The default SQLite database is created beside the executable, so a CMake build
uses `build/film-records.sqlite3`. To keep data elsewhere, use the non-interactive
`sync --output FILE` option.

## Troubleshooting

- `SQLite3 not found`: install the development package, not only the SQLite
  command-line shell. On macOS, pass the Homebrew prefix as shown above.
- `Permission denied` for `/dev/ttyACM*`: add the user to the device's group and
  start a new login session.
- No automatically detected bridge: pass `--port` with the exact `/dev` path.
- `--winusb` error: WinUSB is Windows-only; use the serial bridge on POSIX.
- Camera exchange failure after another operation: interactive camera commands
  share a session. Use `stop` to close it cleanly before reconnecting or exiting.
