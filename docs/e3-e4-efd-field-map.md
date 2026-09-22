# EOS-1V film-record field map

This document consolidates results confirmed through EOS-1V communication captures, Canon Memory EFD files, and controlled camera tests. It describes the download and decoding layer without changing the camera session state machine.

## Confidence labels

| Label | Meaning |
|---|---|
| **Confirmed** | Repeatedly verified against camera packets and/or EFD output |
| **Offline confirmed** | Stable in existing EFD files but not independently reproduced in a new E4 sample |
| **Partially confirmed** | Primary meaning is known; some flags or boundary values remain unexplained |
| **Preserve** | Must remain in the raw packet and must not be interpreted speculatively |

## Download state machine

| Order | Packet | Purpose |
|---:|---|---|
| 1 | `E1 02 00 nn nn` | Number of roll segments; do not request E3/E4 when zero |
| 2 | `E3 21 ...` | 36-byte roll header |
| 3 | `E4 xx ...` | Variable-length frame record |
| 4 | `E4 01 00 00` | End of the current roll |
| 5 | `E3 01 00 00` | End of all rolls |

E3 and E4 use command, length, data, and additive checksum framing. Packet boundaries must be derived from the length byte. A trailing asynchronous F4 must not be consumed as part of E3/E4.

## E3 header

Offsets are relative to E3 `data[0]`.

| Data offset | Size | Meaning | Status |
|---:|---:|---|---|
| 0–2 | 3 | Internal roll sequence/status | Preserve |
| 4–6 | 3 | BCD Film ID; `00 03 29` → `00-329` | **Confirmed** |
| 8 | 1 | Record width: `20`=32, `10`=16, `08`=8 bytes | **Confirmed** |
| 9–16 | 8 | Shooting-field mask controlling E4 layout | **Confirmed** |
| 18 | 1 | DX ISO wire value; `48`=ISO 100, `F0`=no valid DX value | **Confirmed** |
| 19–24 | 6 | Film-load time as BCD `YY MM DD hh mm ss` | **Confirmed** |
| 3, 7, 17, 25–32 | — | Internal/reserved/extended state | Preserve |

Invalid BCD must not be silently converted into a valid date.

## E4 lengths

| Length byte | Total packet | Internal width |
|---:|---:|---:|
| `21` | 36 | 32 bytes |
| `11` | 20 | 16 bytes |
| `09` | 12 | 8 bytes |

E4 fields do not have universal absolute offsets. The parser must consume selected fields in protocol order according to the E3 mask.

## Selectable fields

| Field | Size | Mask bits |
|---|---:|---|
| Focal length | 2 | byte 0, `30` |
| Maximum aperture | 1 | byte 0, `08` |
| Shutter speed | 1 | byte 0, `04` |
| Selected aperture | 1 | byte 0, `02` |
| Manual ISO | 1 | byte 0, `01` |
| Exposure compensation | 1 | byte 1, `80` |
| Flash exposure compensation | 1 | byte 1, `40` |
| Flash mode | 1 | byte 1, `20` |
| Metering mode | 1 | byte 1, `10` |
| Film advance | 1 | byte 1, `04` |
| AF mode | 1 | byte 1, `02` |
| Bulb time | 2 | byte 2, `0C` |
| Capture date | 3 | byte 3, `38` |
| Capture time | 3 | byte 3, `07` |
| C.Fn snapshot | 11 | byte 4 `7F` + byte 5 `F0` |
| Focus-point selection | 1 | byte 5, `08` |
| In-focus point data | 7 | byte 6, `7F` |
| Battery-load date/time | 6 | byte 7, `3F` |

Four internal bytes are reserved for fields recorded independently of the selectable-field total. The parser also preserves the complete E4 packet.

## Confirmed default-layout fields

These offsets apply only to mask `FF FF 0C 3F 00 08 7F 00`.

| Data offset | Field | Confirmed conversion |
|---:|---|---|
| 0–1 | Record/status flags | Partially confirmed; preserve raw |
| 2 | Frame number | Unsigned integer |
| 3–4 | Focal length | Big-endian millimetres |
| 5 | Maximum aperture | EOS aperture code |
| 6 | Shutter | EOS shutter code; `F0` is Bulb |
| 7 | Selected aperture | EOS aperture code |
| 8 | Manual/DX ISO context | Interpret with E3 DX value |
| 9 | Exposure compensation | Signed eighth-EV code |
| 10 | Flash compensation | Signed eighth-EV code |
| 11 | Flash mode | See confirmed enums below |
| 12 | Metering mode plus flags | Decode using the confirmed mask |
| 13 | Shooting mode | See confirmed enums below |
| 14 | Film advance | See confirmed enums below |
| 15 | AF mode plus flags | Decode using the confirmed mask |
| 16 | Multiple-exposure state | `80` flag observed in repeated-exposure groups |
| 19–24 | Capture time | BCD `YY MM DD hh mm ss` |
| 25–32 | Optional extensions | Preserve unless selected fields identify them |

## Confirmed enum values

### Flash

- `02` → OFF
- `48` → Manual, including Multi
- `49` → E-TTL

Other values remain `Unknown(0xNN)`.

### Metering

- `20` → Evaluative
- `10` → Center-weighted average
- `80` → Spot
- `40` → Partial

Additional low-bit flags are preserved.

### Shooting mode

- `80` → Manual
- `10` → Program AE
- `20` → Shutter-priority AE
- `40` → Aperture-priority AE
- `04` → Bulb

The E4 wire value for depth-of-field AE is not yet confirmed.

### Film advance

- `08` → Single-frame
- `10` → 2-second self-timer
- `20` → 10-second self-timer
- `40` → Body-only continuous
- `01` → Low-speed continuous
- `02` → High-speed continuous

The E4 wire value for ultra-high-speed continuous is not yet confirmed.

### AF

- `02` or `42` → One-Shot AF
- `04` → AI Servo AF
- `14` → Manual focus

## Numeric conversions

- E4 aperture code uses eighth-stop APEX values: (f = 2^{wire/8}).
- Shutter code is stored as an eighth-stop exposure value; `F0` is treated separately as Bulb.
- ISO examples confirm `48`=100 and `50`=200.
- Exposure and flash compensation use signed eighth-EV codes. Examples include `08`=+1 EV, `03`≈+0.3 EV, `FD`≈−0.3 EV, and `FB`≈−0.7 EV.

## C.Fn snapshot

The 11-byte snapshot uses the same layout as the D1 data area:

- bytes 0–8 contain two one-hot nibbles each;
- the low nibble represents odd-numbered C.Fn items;
- the high nibble represents even-numbered C.Fn items;
- byte 9 contains the full one-hot value for C.Fn-19;
- byte 10 is auxiliary data and is not one of the 19 option values.

The database stores the decoded option indices as exactly 19 comma-separated numbers. The complete source bytes remain recoverable from `raw_e4`.

## EFD reference mapping

EFD is used only as validation evidence; FilmDB does not store records in EFD format.

| EFRM offset | Meaning |
|---:|---|
| `18` | Frame number |
| `1C` | Focal length in millimetres |
| `20` | Maximum aperture ×100 |
| `24` | Shutter denominator ×−100; `-1` can represent Bulb |
| `28` | Aperture ×100 |
| `2C` | Manual ISO |
| `30`, `34` | Exposure and flash compensation ×100 |
| `38–3E` | Capture date/time |
| `40` | Flash enum |
| `44` | Film-advance enum |
| `48` | Multiple exposure |
| `54` | Metering enum |
| `58` | Shooting-mode enum |
| `5C` | AF enum |
| `64+` | Expanded C.Fn data |
| `8B+` | Battery-load date/time |

## Implementation constraints

- Never assume a fixed total download size.
- Never assume 36 frames per roll.
- Validate packet lengths, checksums, and BCD values.
- Preserve complete E3/E4 packets.
- Do not decode a 16- or 8-byte record with the default 32-byte layout.
- Store fields disabled by the mask as `NULL`.
- Never map an unknown enum to a nearby known value.
- Image path and remarks are desktop-side metadata and must not be invented from E4.

## Current coverage

| Layer | Status |
|---|---|
| Session plus E1/E3/E4/end markers | Complete and camera-tested |
| Default 32-byte visible fields | Substantially complete |
| Variable 16/8-byte framing | Confirmed |
| Dynamic-mask decoding | Implemented for recognized fields |
| C.Fn 1–19 decoding | Confirmed |
| Rare legacy enum wire values | Awaiting controlled samples |
