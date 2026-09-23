#include "filmrecorder/database.hpp"

#include <winsqlite/winsqlite3.h>

#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <stdexcept>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

namespace filmrecorder {
namespace {

class Database {
public:
    explicit Database(const std::filesystem::path& path) {
        const auto text = path.u8string();
        if (sqlite3_open(reinterpret_cast<const char*>(text.c_str()), &db_) != SQLITE_OK) {
            const std::string message = db_ ? sqlite3_errmsg(db_) : "cannot allocate database";
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("cannot open SQLite database: " + message);
        }
        exec("PRAGMA foreign_keys=ON");
    }
    ~Database() { if (db_) sqlite3_close(db_); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    sqlite3* get() const { return db_; }
    void exec(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : "unknown SQLite error";
            sqlite3_free(error); throw std::runtime_error(message);
        }
    }
private:
    sqlite3* db_{};
};

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    void integer(int index, std::int64_t value) { check(sqlite3_bind_int64(statement_, index, value)); }
    void real(int index, double value) { check(sqlite3_bind_double(statement_, index, value)); }
    void text(int index, const std::string& value) { check(sqlite3_bind_text(statement_, index, value.c_str(), -1, SQLITE_TRANSIENT)); }
    void nullableText(int index, const std::optional<std::string>& value) {
        if (value) text(index, *value); else check(sqlite3_bind_null(statement_, index));
    }
    void nullableInteger(int index, const std::optional<std::uint8_t>& value) {
        if (value) integer(index, *value); else check(sqlite3_bind_null(statement_, index));
    }
    template<class T> void optionalInteger(int index, const std::optional<T>& value) {
        if (value) integer(index, static_cast<std::int64_t>(*value)); else null(index);
    }
    void optionalReal(int index, const std::optional<double>& value) {
        if (value) real(index, *value); else null(index);
    }
    void null(int index) { check(sqlite3_bind_null(statement_, index)); }
    void done() {
        if (sqlite3_step(statement_) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
        sqlite3_reset(statement_); sqlite3_clear_bindings(statement_);
    }
private:
    void check(int result) { if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_)); }
    sqlite3* db_{}; sqlite3_stmt* statement_{};
};

constexpr const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS imports (
 id INTEGER PRIMARY KEY, imported_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
 import_date TEXT NOT NULL, import_time TEXT NOT NULL,
 source_type TEXT NOT NULL, parser_version INTEGER NOT NULL, reported_rolls INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS rolls (
 id INTEGER PRIMARY KEY, import_id INTEGER NOT NULL REFERENCES imports(id) ON DELETE CASCADE,
 roll_index INTEGER NOT NULL, film_id TEXT NOT NULL, record_width INTEGER NOT NULL,
 dx_iso INTEGER, loaded_at TEXT, raw_e3 TEXT NOT NULL,
 UNIQUE(import_id, roll_index)
);
CREATE TABLE IF NOT EXISTS frames (
 id INTEGER PRIMARY KEY, roll_id INTEGER NOT NULL REFERENCES rolls(id) ON DELETE CASCADE,
 frame_index INTEGER NOT NULL, frame_number INTEGER, focal_length_mm INTEGER,
 max_aperture_f REAL, shutter_seconds REAL, shutter_display TEXT, aperture_f REAL,
 manual_iso INTEGER, exposure_compensation_ev REAL, flash_compensation_ev REAL,
 flash_mode TEXT, metering_mode TEXT, shooting_mode TEXT, film_advance TEXT, af_mode TEXT,
 multiple_exposure INTEGER, bulb_time_units INTEGER, captured_at TEXT,
 cfn_values TEXT, battery_loaded_at TEXT, raw_e4 TEXT NOT NULL,
 UNIQUE(roll_id, frame_index)
);
CREATE INDEX IF NOT EXISTS frames_captured_at_idx ON frames(captured_at);
CREATE INDEX IF NOT EXISTS frames_focal_length_idx ON frames(focal_length_mm);
)SQL";

bool columnExists(sqlite3* db, const char* table, const char* column) {
    const std::string sql = "PRAGMA table_info(" + std::string(table) + ")";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
    bool found = false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        if (name && column == std::string(name)) { found = true; break; }
    }
    sqlite3_finalize(statement); return found;
}

void migrate(Database& db) {
    if (!columnExists(db.get(), "imports", "import_date"))
        db.exec("ALTER TABLE imports ADD COLUMN import_date TEXT");
    if (!columnExists(db.get(), "imports", "import_time"))
        db.exec("ALTER TABLE imports ADD COLUMN import_time TEXT");
    // Preserve older imports by deriving their split fields from imported_at.
    // New imports always receive local system date/time from the application.
    db.exec("UPDATE imports SET import_date=substr(imported_at,1,10) WHERE import_date IS NULL");
    db.exec("UPDATE imports SET import_time=substr(imported_at,12,8) WHERE import_time IS NULL");

    const bool legacy = columnExists(db.get(), "frames", "max_aperture_wire");
    if (!legacy) return;
    const std::array<std::tuple<const char*,const char*,const char*>,18> columns{{
        {"rolls","dx_iso","INTEGER"},
        {"frames","max_aperture_f","REAL"},{"frames","shutter_seconds","REAL"},
        {"frames","shutter_display","TEXT"},{"frames","aperture_f","REAL"},
        {"frames","manual_iso","INTEGER"},{"frames","exposure_compensation_ev","REAL"},
        {"frames","flash_compensation_ev","REAL"},{"frames","flash_mode","TEXT"},
        {"frames","metering_mode","TEXT"},{"frames","shooting_mode","TEXT"},
        {"frames","film_advance","TEXT"},{"frames","af_mode","TEXT"},
        {"frames","multiple_exposure","INTEGER"},{"frames","bulb_time_wire","INTEGER"},
        {"frames","focus_selection_wire","INTEGER"},{"frames","focus_points_raw","TEXT"},
        {"frames","battery_loaded_at","TEXT"}
    }};
    for (const auto& [table,column,type] : columns) if (!columnExists(db.get(),table,column))
        db.exec(("ALTER TABLE " + std::string(table) + " ADD COLUMN " + column + " " + type).c_str());

    if (legacy) {
        db.exec("PRAGMA foreign_keys=OFF");
        db.exec(R"SQL(
BEGIN IMMEDIATE;
CREATE TABLE rolls_new (
 id INTEGER PRIMARY KEY, import_id INTEGER NOT NULL REFERENCES imports(id) ON DELETE CASCADE,
 roll_index INTEGER NOT NULL, film_id TEXT NOT NULL, record_width INTEGER NOT NULL,
 dx_iso INTEGER, loaded_at TEXT, raw_e3 TEXT NOT NULL, UNIQUE(import_id,roll_index));
INSERT INTO rolls_new(id,import_id,roll_index,film_id,record_width,dx_iso,loaded_at,raw_e3)
 SELECT id,import_id,roll_index,film_id_raw,record_width,dx_iso,loaded_at,raw_e3 FROM rolls;
CREATE TABLE frames_new (
 id INTEGER PRIMARY KEY, roll_id INTEGER NOT NULL REFERENCES rolls(id) ON DELETE CASCADE,
 frame_index INTEGER NOT NULL, frame_number INTEGER, focal_length_mm INTEGER,
 max_aperture_f REAL, shutter_seconds REAL, shutter_display TEXT, aperture_f REAL,
 manual_iso INTEGER, exposure_compensation_ev REAL, flash_compensation_ev REAL,
 flash_mode TEXT, metering_mode TEXT, shooting_mode TEXT, film_advance TEXT, af_mode TEXT,
 multiple_exposure INTEGER, bulb_time_units INTEGER, captured_at TEXT,
 cfn_values TEXT, battery_loaded_at TEXT, raw_e4 TEXT NOT NULL, UNIQUE(roll_id,frame_index));
INSERT INTO frames_new(id,roll_id,frame_index,frame_number,focal_length_mm,max_aperture_f,
 shutter_seconds,shutter_display,aperture_f,manual_iso,exposure_compensation_ev,
 flash_compensation_ev,flash_mode,metering_mode,shooting_mode,film_advance,af_mode,
 multiple_exposure,bulb_time_units,captured_at,cfn_values,battery_loaded_at,raw_e4)
 SELECT id,roll_id,frame_index,frame_number,focal_length_mm,max_aperture_f,
 shutter_seconds,shutter_display,aperture_f,manual_iso,exposure_compensation_ev,
 flash_compensation_ev,flash_mode,metering_mode,shooting_mode,film_advance,af_mode,
 multiple_exposure,bulb_time_wire,captured_at,cfn_values,battery_loaded_at,raw_e4 FROM frames;
DROP TABLE frames;
DROP TABLE rolls;
ALTER TABLE rolls_new RENAME TO rolls;
ALTER TABLE frames_new RENAME TO frames;
CREATE INDEX frames_captured_at_idx ON frames(captured_at);
CREATE INDEX frames_focal_length_idx ON frames(focal_length_mm);
COMMIT;
)SQL");
        db.exec("PRAGMA foreign_keys=ON");
    }
}

std::pair<std::string, std::string> localImportDateTime() {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    if (localtime_s(&local, &value) != 0)
        throw std::runtime_error("cannot read local system time");
    std::ostringstream date;
    std::ostringstream time;
    date << std::put_time(&local, "%Y-%m-%d");
    time << std::put_time(&local, "%H:%M:%S");
    return {date.str(), time.str()};
}

} // namespace

SaveResult saveToDatabase(const std::filesystem::path& path,
                          const Download& download, const char* sourceType) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    Database db(path); db.exec(schema); migrate(db); db.exec("BEGIN IMMEDIATE");
    try {
        const auto [importDate, importTime] = localImportDateTime();
        Statement addImport(db.get(), "INSERT INTO imports(import_date,import_time,source_type,parser_version,reported_rolls) VALUES(?,?,?,1,?)");
        addImport.text(1, importDate); addImport.text(2, importTime);
        addImport.text(3, sourceType); addImport.integer(4, download.reportedRolls); addImport.done();
        const auto importId = sqlite3_last_insert_rowid(db.get());
        Statement addRoll(db.get(), "INSERT INTO rolls(import_id,roll_index,film_id,record_width,dx_iso,loaded_at,raw_e3) VALUES(?,?,?,?,?,?,?)");
        Statement addFrame(db.get(), "INSERT INTO frames(roll_id,frame_index,frame_number,focal_length_mm,max_aperture_f,shutter_seconds,shutter_display,aperture_f,manual_iso,exposure_compensation_ev,flash_compensation_ev,flash_mode,metering_mode,shooting_mode,film_advance,af_mode,multiple_exposure,bulb_time_units,captured_at,cfn_values,battery_loaded_at,raw_e4) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        std::size_t frameTotal = 0;
        for (std::size_t r = 0; r < download.rolls.size(); ++r) {
            const auto& roll = download.rolls[r];
            addRoll.integer(1, importId); addRoll.integer(2, r + 1); addRoll.text(3, roll.filmId);
            addRoll.integer(4, roll.recordWidth); addRoll.optionalInteger(5, roll.dxIso);
            addRoll.nullableText(6, roll.loadedAt); addRoll.text(7, roll.rawHeaderHex); addRoll.done();
            const auto rollId = sqlite3_last_insert_rowid(db.get());
            for (std::size_t f = 0; f < roll.frames.size(); ++f) {
                const auto& frame = roll.frames[f];
                addFrame.integer(1, rollId); addFrame.integer(2, f + 1);
                addFrame.integer(3,frame.number);
                if(frame.focalLengthPresent)addFrame.integer(4,frame.focalLengthMm);else addFrame.null(4);
                addFrame.optionalReal(5,frame.maxApertureF); addFrame.optionalReal(6,frame.shutterSeconds);
                addFrame.nullableText(7,frame.shutterDisplay); addFrame.optionalReal(8,frame.apertureF);
                addFrame.optionalInteger(9,frame.manualIso); addFrame.optionalReal(10,frame.exposureCompensationEv);
                addFrame.optionalReal(11,frame.flashCompensationEv);
                if(frame.flashModePresent)addFrame.text(12,frame.flashMode);else addFrame.null(12);
                if(frame.meteringPresent)addFrame.text(13,frame.meteringMode);else addFrame.null(13);
                addFrame.text(14,frame.shootingMode);
                if(frame.filmAdvancePresent)addFrame.text(15,frame.filmAdvance);else addFrame.null(15);
                if(frame.afModePresent)addFrame.text(16,frame.afMode);else addFrame.null(16);
                addFrame.integer(17,frame.multipleExposure); addFrame.optionalInteger(18,frame.bulbTimeWire);
                addFrame.nullableText(19,frame.capturedAt); addFrame.nullableText(20,frame.cfnValues);
                addFrame.nullableText(21,frame.batteryLoadedAt); addFrame.text(22,frame.rawHex); addFrame.done();
                ++frameTotal;
            }
        }
        db.exec("COMMIT");
        return {importId, download.rolls.size(), frameTotal};
    } catch (...) { try { db.exec("ROLLBACK"); } catch (...) {} throw; }
}

std::string inspectDatabase(const std::filesystem::path& path) {
    Database db(path);
    db.exec(schema);
    migrate(db);
    auto scalarText = [&](const char* sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db.get(), sql, -1, &statement, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db.get()));
        const auto result = sqlite3_step(statement);
        if (result != SQLITE_ROW) {
            sqlite3_finalize(statement); throw std::runtime_error(sqlite3_errmsg(db.get()));
        }
        const auto value = sqlite3_column_text(statement, 0);
        const std::string text = value ? reinterpret_cast<const char*>(value) : "";
        sqlite3_finalize(statement); return text;
    };
    auto scalar = [&](const char* sql) { return std::stoll(scalarText(sql)); };
    const auto integrity = scalarText("PRAGMA integrity_check");
    const auto imports = scalar("SELECT count(*) FROM imports");
    const auto rolls = scalar("SELECT count(*) FROM rolls");
    const auto frames = scalar("SELECT count(*) FROM frames");
    const auto invalidImportDateTime = scalar(R"SQL(
        SELECT count(*) FROM imports
        WHERE import_date IS NULL OR length(import_date) <> 10
           OR import_date NOT GLOB '????-??-??'
           OR import_time IS NULL OR length(import_time) <> 8
           OR import_time NOT GLOB '??:??:??'
    )SQL");
    const auto cfnRows = scalar("SELECT count(*) FROM frames WHERE cfn_values IS NOT NULL");
    const auto invalidCfn = scalar(R"SQL(
        SELECT count(*) FROM frames
        WHERE cfn_values IS NOT NULL AND (
            (length(cfn_values) - length(replace(cfn_values, ',', ''))) <> 18
            OR cfn_values GLOB '*[^0-9,]*'
        )
    )SQL");
    const auto splitRawColumns = scalar(R"SQL(
        SELECT
          (SELECT count(*) FROM pragma_table_info('frames')
           WHERE name LIKE '%_wire' OR name IN ('cfn_raw','focus_points_raw'))
          +
          (SELECT count(*) FROM pragma_table_info('rolls')
           WHERE name IN ('film_id_raw','field_mask_raw','dx_iso_wire'))
    )SQL");
    std::ostringstream out;
    out << "Integrity: " << integrity << '\n'
        << "Imports: " << imports << '\n'
        << "Rolls: " << rolls << '\n'
        << "Frames: " << frames << '\n'
        << "Invalid import date/time rows: " << invalidImportDateTime << '\n'
        << "Frames with C.Fn: " << cfnRows << '\n'
        << "Invalid C.Fn rows: " << invalidCfn << '\n'
        << "Split raw columns: " << splitRawColumns << '\n';
    return out.str();
}

std::vector<std::string> listImportDates(const std::filesystem::path& path,
                                         const std::string& yearMonth) {
    Database db(path); db.exec(schema); migrate(db);
    sqlite3_stmt* statement = nullptr;
    constexpr auto sql = R"SQL(
        SELECT DISTINCT import_date FROM imports
        WHERE substr(import_date,1,7)=?
        ORDER BY import_date
    )SQL";
    if (sqlite3_prepare_v2(db.get(), sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db.get()));
    sqlite3_bind_text(statement, 1, yearMonth.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<std::string> dates;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto value = sqlite3_column_text(statement, 0);
        if (value) dates.emplace_back(reinterpret_cast<const char*>(value));
    }
    sqlite3_finalize(statement);
    return dates;
}

std::vector<RollListItem> listRollsForDate(const std::filesystem::path& path,
                                           const std::string& date) {
    Database db(path); db.exec(schema); migrate(db);
    sqlite3_stmt* statement = nullptr;
    constexpr auto sql = R"SQL(
        SELECT r.id, i.id, i.import_time, r.film_id, count(f.id)
        FROM imports i
        JOIN rolls r ON r.import_id=i.id
        LEFT JOIN frames f ON f.roll_id=r.id
        WHERE i.import_date=?
        GROUP BY r.id, i.id, i.import_time, r.film_id
        ORDER BY i.import_time, r.id
    )SQL";
    if (sqlite3_prepare_v2(db.get(), sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db.get()));
    sqlite3_bind_text(statement, 1, date.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<RollListItem> rolls;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        RollListItem item;
        item.rollId = sqlite3_column_int64(statement, 0);
        item.importId = sqlite3_column_int64(statement, 1);
        if (const auto value = sqlite3_column_text(statement, 2))
            item.importTime = reinterpret_cast<const char*>(value);
        if (const auto value = sqlite3_column_text(statement, 3))
            item.filmId = reinterpret_cast<const char*>(value);
        item.frameCount = sqlite3_column_int64(statement, 4);
        rolls.push_back(std::move(item));
    }
    sqlite3_finalize(statement);
    return rolls;
}

std::string describeRoll(const std::filesystem::path& path,
                         std::int64_t rollId) {
    Database db(path); db.exec(schema); migrate(db);
    sqlite3_stmt* statement = nullptr;
    constexpr auto rollSql = R"SQL(
        SELECT i.import_date, i.import_time, i.id, r.id, r.film_id,
               r.record_width, r.dx_iso, r.loaded_at
        FROM rolls r JOIN imports i ON i.id=r.import_id WHERE r.id=?
    )SQL";
    if (sqlite3_prepare_v2(db.get(), rollSql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db.get()));
    sqlite3_bind_int64(statement, 1, rollId);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        throw std::runtime_error("roll ID was not found");
    }
    const auto text = [&](int column) -> std::string {
        const auto value = sqlite3_column_text(statement, column);
        return value ? reinterpret_cast<const char*>(value) : "n/a";
    };
    std::ostringstream out;
    out << "Import Date: " << text(0)
        << " | Import Time: " << text(1)
        << " | Import ID: " << sqlite3_column_int64(statement, 2)
        << " | Roll ID: " << sqlite3_column_int64(statement, 3)
        << " | Film ID: " << text(4)
        << " | Record Width: " << text(5)
        << " | DX ISO: " << text(6)
        << " | Loaded At: " << text(7) << "\n\nFrames:\n";
    sqlite3_finalize(statement);

    constexpr auto frameSql = R"SQL(
        SELECT frame_index, frame_number, focal_length_mm, max_aperture_f,
               shutter_seconds, shutter_display, aperture_f, manual_iso,
               exposure_compensation_ev, flash_compensation_ev, flash_mode,
               metering_mode, shooting_mode, film_advance, af_mode,
               multiple_exposure, bulb_time_units, captured_at, cfn_values,
               battery_loaded_at
        FROM frames WHERE roll_id=? ORDER BY frame_index
    )SQL";
    if (sqlite3_prepare_v2(db.get(), frameSql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db.get()));
    sqlite3_bind_int64(statement, 1, rollId);
    bool found = false;
    std::size_t recordNumber = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        found = true;
        ++recordNumber;
        const auto field = [&](int column) -> std::string {
            const auto value = sqlite3_column_text(statement, column);
            return value ? reinterpret_cast<const char*>(value) : "n/a";
        };
        out << recordNumber << ") Frame Index: " << field(0)
            << " | Frame Number: " << field(1)
            << " | Focal Length (mm): " << field(2)
            << " | Maximum Aperture (f): " << field(3)
            << " | Shutter Time (seconds): " << field(4)
            << " | Shutter Display: " << field(5)
            << " | Aperture (f): " << field(6)
            << " | Manual ISO: " << field(7)
            << " | Exposure Compensation (EV): " << field(8)
            << " | Flash Compensation (EV): " << field(9)
            << " | Flash Mode: " << field(10)
            << " | Metering Mode: " << field(11)
            << " | Shooting Mode: " << field(12)
            << " | Film Advance: " << field(13)
            << " | AF Mode: " << field(14)
            << " | Multiple Exposure: " << field(15)
            << " | Bulb Time Units: " << field(16)
            << " | Captured At: " << field(17)
            << " | C.Fn Values: " << field(18)
            << " | Battery Loaded At: " << field(19) << "\n\n";
    }
    if (!found) out << "  n/a\n";
    sqlite3_finalize(statement);
    return out.str();
}

} // namespace filmrecorder
