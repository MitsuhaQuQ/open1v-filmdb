# SQLite data model

Every decoded value remains traceable to the complete raw camera packet from which it was derived.

## `imports`

One row represents one completed camera download.

- `id` — primary key
- `imported_at` — SQLite UTC timestamp
- `import_date` — local system date at import time, formatted as `YYYY-MM-DD`
- `import_time` — local system time at import time, formatted as `HH:MM:SS`
- `source_type` — automatic serial detection, an explicit COM port, or WinUSB
- `parser_version` — schema/parser version used for the import
- `reported_rolls` — roll-segment count returned by E1

## `rolls`

One row represents one E3 roll segment.

- `id` — primary key
- `import_id` — owning import batch
- `roll_index` — order returned by the camera
- `film_id` — decoded BCD Film ID
- `record_width` — E4 internal record-width class
- `dx_iso` — decoded DX ISO, or `NULL` when unavailable
- `loaded_at` — decoded BCD film-load time
- `raw_e3` — complete E3 packet in hexadecimal form

## `frames`

One row represents one E4 shooting record.

- `id`, `roll_id`, `frame_index`, `frame_number`
- `focal_length_mm`
- `max_aperture_f`, `aperture_f`
- `shutter_seconds`, `shutter_display`
- `manual_iso`
- `exposure_compensation_ev`, `flash_compensation_ev`
- `flash_mode`, `metering_mode`, `shooting_mode`
- `film_advance`, `af_mode`, `multiple_exposure`
- `bulb_time_units`
- `captured_at`
- `cfn_values` — exactly 19 comma-separated option numbers when recorded
- `battery_loaded_at`
- `raw_e4` — complete E4 packet in hexadecimal form

Fields not selected by the roll's mask are stored as `NULL`. Unsupported enum values remain visible as `Unknown(0xNN)`.

## Raw-data policy

Raw transport values are stored only as `raw_e3` and `raw_e4`. The schema deliberately avoids separate per-field wire columns because every original byte can be recovered from these complete packets.

## Import behavior

Each successful download is inserted in one transaction. A failure rolls back the entire new import. Existing imports are never overwritten.

The `view` command groups imports by `import_date`. Within a selected date it orders rolls by `import_time` and database Roll ID, so multiple imports made on the same day remain distinguishable.
