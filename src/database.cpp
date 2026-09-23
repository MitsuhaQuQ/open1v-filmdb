#include "filmrecorder/database.hpp"

#if defined(_WIN32)
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <array>
#include <chrono>
#include <cmath>
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
 flash_mode TEXT, metering_mode TEXT, shooting_mode TEXT, aeb_position TEXT, film_advance TEXT, af_mode TEXT,
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

    if (!columnExists(db.get(), "frames", "aeb_position"))
        db.exec("ALTER TABLE frames ADD COLUMN aeb_position TEXT");

    const auto redecodeKnownValues = [&db] {
        // Re-decode values retained by earlier parser versions without needing
        // the original camera to be connected again.
        db.exec(R"SQL(
UPDATE frames SET
 aeb_position=CASE shooting_mode
   WHEN 'Unknown(0x41)' THEN 'Standard exposure'
   WHEN 'Unknown(0x42)' THEN 'Underexposed'
   WHEN 'Unknown(0x43)' THEN 'Overexposed' END,
 shooting_mode='Aperture-priority AE'
WHERE shooting_mode IN ('Unknown(0x41)','Unknown(0x42)','Unknown(0x43)');
UPDATE frames AS target SET multiple_exposure=1
WHERE EXISTS (
 SELECT 1 FROM frames AS marker
 WHERE marker.roll_id=target.roll_id
   AND marker.frame_number=target.frame_number
   AND marker.film_advance='Unknown(0x88)')
AND (SELECT count(*) FROM frames AS member
     WHERE member.roll_id=target.roll_id
       AND member.frame_number=target.frame_number) > 1;
UPDATE frames SET film_advance='Single-frame'
WHERE film_advance='Unknown(0x88)';
)SQL");
    };

    const bool legacy = columnExists(db.get(), "frames", "max_aperture_wire");
    if (!legacy) { redecodeKnownValues(); return; }
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
 flash_mode TEXT, metering_mode TEXT, shooting_mode TEXT, aeb_position TEXT, film_advance TEXT, af_mode TEXT,
 multiple_exposure INTEGER, bulb_time_units INTEGER, captured_at TEXT,
 cfn_values TEXT, battery_loaded_at TEXT, raw_e4 TEXT NOT NULL, UNIQUE(roll_id,frame_index));
INSERT INTO frames_new(id,roll_id,frame_index,frame_number,focal_length_mm,max_aperture_f,
 shutter_seconds,shutter_display,aperture_f,manual_iso,exposure_compensation_ev,
 flash_compensation_ev,flash_mode,metering_mode,shooting_mode,aeb_position,film_advance,af_mode,
 multiple_exposure,bulb_time_units,captured_at,cfn_values,battery_loaded_at,raw_e4)
 SELECT id,roll_id,frame_index,frame_number,focal_length_mm,max_aperture_f,
 shutter_seconds,shutter_display,aperture_f,manual_iso,exposure_compensation_ev,
 flash_compensation_ev,flash_mode,metering_mode,shooting_mode,aeb_position,film_advance,af_mode,
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
    redecodeKnownValues();
}

std::pair<std::string, std::string> localImportDateTime() {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &value) != 0)
#else
    if (localtime_r(&value, &local) == nullptr)
#endif
        throw std::runtime_error("cannot read local system time");
    std::ostringstream date;
    std::ostringstream time;
    date << std::put_time(&local, "%Y-%m-%d");
    time << std::put_time(&local, "%H:%M:%S");
    return {date.str(), time.str()};
}

std::string apertureDisplay(sqlite3_stmt* statement, int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) return "n/a";
    static constexpr std::array<double, 45> thirdStops{
        0.5,0.6,0.7,0.8,0.9,1.0,1.1,1.2,1.4,1.6,1.8,2.0,2.2,2.5,2.8,
        3.2,3.5,4.0,4.5,5.0,5.6,6.3,7.1,8.0,9.0,10.0,11.0,13.0,
        14.0,16.0,18.0,20.0,22.0,25.0,29.0,32.0,36.0,40.0,45.0,
        51.0,57.0,64.0,72.0,81.0,91.0};
    const auto decoded = sqlite3_column_double(statement, column);
    const auto nearest = std::min_element(thirdStops.begin(), thirdStops.end(),
        [decoded](double left, double right) {
            return std::abs(left - decoded) < std::abs(right - decoded);
        });
    std::ostringstream out;
    if (std::floor(*nearest) == *nearest) out << static_cast<int>(*nearest);
    else out << std::fixed << std::setprecision(1) << *nearest;
    return out.str();
}

std::string shutterDisplay(sqlite3_stmt* statement, int secondsColumn,
                           int fallbackColumn) {
    if (sqlite3_column_type(statement, secondsColumn) == SQLITE_NULL) {
        const auto fallback = sqlite3_column_text(statement, fallbackColumn);
        return fallback ? reinterpret_cast<const char*>(fallback) : "n/a";
    }
    const auto seconds = sqlite3_column_double(statement, secondsColumn);
    if (seconds <= 0) return "n/a";

    if (seconds >= 0.3) {
        static constexpr std::array<double, 21> longStops{
            0.3,0.4,0.5,0.6,0.8,1.0,1.3,1.6,2.0,2.5,3.2,
            4.0,5.0,6.0,8.0,10.0,13.0,15.0,20.0,25.0,30.0};
        const auto nearest = std::min_element(longStops.begin(), longStops.end(),
            [seconds](double left, double right) {
                return std::abs(left - seconds) < std::abs(right - seconds);
            });
        std::ostringstream out;
        if (std::floor(*nearest) == *nearest) out << static_cast<int>(*nearest);
        else out << std::fixed << std::setprecision(1) << *nearest;
        out << " s";
        return out.str();
    }

    static constexpr std::array<int, 34> denominators{
        4,5,6,8,10,13,15,20,25,30,40,50,60,80,100,125,160,
        200,250,320,400,500,640,800,1000,1250,1600,2000,2500,
        3200,4000,5000,6400,8000};
    const auto reciprocal = 1.0 / seconds;
    const auto nearest = std::min_element(denominators.begin(), denominators.end(),
        [reciprocal](int left, int right) {
            return std::abs(left - reciprocal) < std::abs(right - reciprocal);
        });
    return "1/" + std::to_string(*nearest) + " s";
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
        Statement addFrame(db.get(), "INSERT INTO frames(roll_id,frame_index,frame_number,focal_length_mm,max_aperture_f,shutter_seconds,shutter_display,aperture_f,manual_iso,exposure_compensation_ev,flash_compensation_ev,flash_mode,metering_mode,shooting_mode,aeb_position,film_advance,af_mode,multiple_exposure,bulb_time_units,captured_at,cfn_values,battery_loaded_at,raw_e4) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
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
                addFrame.nullableText(15,frame.aebPosition);
                if(frame.filmAdvancePresent)addFrame.text(16,frame.filmAdvance);else addFrame.null(16);
                if(frame.afModePresent)addFrame.text(17,frame.afMode);else addFrame.null(17);
                addFrame.integer(18,frame.multipleExposure); addFrame.optionalInteger(19,frame.bulbTimeWire);
                addFrame.nullableText(20,frame.capturedAt); addFrame.nullableText(21,frame.cfnValues);
                addFrame.nullableText(22,frame.batteryLoadedAt); addFrame.text(23,frame.rawHex); addFrame.done();
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

std::vector<FrameListItem> listFramesForRoll(const std::filesystem::path& path,
                                             std::int64_t rollId) {
    Database db(path); db.exec(schema); migrate(db);
    sqlite3_stmt* statement = nullptr;
    constexpr auto sql = R"SQL(
        SELECT f.id, f.frame_index, f.shutter_seconds, f.shutter_display, f.aperture_f,
               CASE WHEN r.dx_iso IS NOT NULL THEN r.dx_iso ELSE f.manual_iso END,
               f.focal_length_mm, f.captured_at
        FROM frames f JOIN rolls r ON r.id=f.roll_id
        WHERE f.roll_id=? ORDER BY f.frame_index
    )SQL";
    if (sqlite3_prepare_v2(db.get(), sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db.get()));
    sqlite3_bind_int64(statement, 1, rollId);
    std::vector<FrameListItem> frames;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        FrameListItem item;
        item.frameId = sqlite3_column_int64(statement, 0);
        item.frameIndex = sqlite3_column_int64(statement, 1);
        const auto value = [&](int column) -> std::string {
            const auto text = sqlite3_column_text(statement, column);
            return text ? reinterpret_cast<const char*>(text) : "n/a";
        };
        item.shutterSpeed = shutterDisplay(statement, 2, 3);
        item.aperture = apertureDisplay(statement, 4);
        item.iso = value(5);
        item.focalLength = value(6);
        item.capturedAt = value(7);
        frames.push_back(std::move(item));
    }
    sqlite3_finalize(statement);
    return frames;
}

std::string describeFrame(const std::filesystem::path& path,
                          std::int64_t frameId) {
    Database db(path); db.exec(schema); migrate(db);
    sqlite3_stmt* statement = nullptr;
    constexpr auto sql = R"SQL(
        SELECT i.import_date, i.import_time, i.id, r.id, r.film_id,
               r.record_width, r.dx_iso, r.loaded_at,
               f.frame_index, f.frame_number, f.focal_length_mm,
               f.max_aperture_f, f.shutter_seconds, f.shutter_display,
               f.aperture_f, f.manual_iso, f.exposure_compensation_ev,
               f.flash_compensation_ev, f.flash_mode, f.metering_mode,
               f.shooting_mode, f.aeb_position, f.film_advance, f.af_mode,
               f.multiple_exposure, f.bulb_time_units, f.captured_at,
               f.cfn_values, f.battery_loaded_at
        FROM frames f
        JOIN rolls r ON r.id=f.roll_id
        JOIN imports i ON i.id=r.import_id
        WHERE f.id=?
    )SQL";
    if (sqlite3_prepare_v2(db.get(), sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db.get()));
    sqlite3_bind_int64(statement, 1, frameId);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        throw std::runtime_error("frame ID was not found");
    }
    const auto field = [&](int column) -> std::string {
        const auto value = sqlite3_column_text(statement, column);
        return value ? reinterpret_cast<const char*>(value) : "n/a";
    };
    std::ostringstream out;
    out << "Import Date: " << field(0) << '\n'
        << "Import Time: " << field(1) << '\n'
        << "Import ID: " << field(2) << '\n'
        << "Roll ID: " << field(3) << '\n'
        << "Film ID: " << field(4) << '\n'
        << "Record Width: " << field(5) << '\n'
        << "DX ISO: " << field(6) << '\n'
        << "Loaded At: " << field(7) << '\n'
        << "Frame Index: " << field(8) << '\n'
        << "Frame Number: " << field(9) << '\n'
        << "Focal Length (mm): " << field(10) << '\n'
        << "Maximum Aperture (f): " << apertureDisplay(statement, 11) << '\n'
        << "Shutter Speed: " << shutterDisplay(statement, 12, 13) << '\n'
        << "Aperture (f): " << apertureDisplay(statement, 14) << '\n'
        << "Manual ISO: " << field(15) << '\n'
        << "Exposure Compensation (EV): " << field(16) << '\n'
        << "Flash Compensation (EV): " << field(17) << '\n'
        << "Flash Mode: " << field(18) << '\n'
        << "Metering Mode: " << field(19) << '\n'
        << "Shooting Mode: " << field(20) << '\n';
    if (sqlite3_column_type(statement, 21) != SQLITE_NULL)
        out << "AEB Position: " << field(21) << '\n';
    out << "Film Advance: " << field(22) << '\n'
        << "AF Mode: " << field(23) << '\n';
    if (sqlite3_column_int(statement, 24) != 0)
        out << "Multiple Exposure: Yes\n";
    out << "Bulb Time Units: " << field(25) << '\n'
        << "Captured At: " << field(26) << '\n'
        << "C.Fn Values: " << field(27) << '\n'
        << "Battery Loaded At: " << field(28) << "\n\n";
    sqlite3_finalize(statement);
    return out.str();
}

} // namespace filmrecorder
