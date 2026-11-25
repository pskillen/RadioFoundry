<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# M17 in the codeplug

M17 conventional channels use the generic layer in [channel-modes.md](channel-modes.md). M17 addresses are stored
big endian, as on air. NXDN uses little-endian fields. The firmware's codec is `core/m17/m17_codeplug.c`
(`CONFIG_M17`). The radio runs kind 6 when the build has `CONFIG_M17` and the board's air interface can receive or
transmit with the M17 profile (`radio_m17_fits`, [m17.md](m17.md) 10). Elsewhere the channel is parked
([channel-modes.md](channel-modes.md) 6).

| Id | Kind | System type |
|---|---|---|
| 6 | M17 conventional | none |

## 1. Radio-wide record (kind 6)

Key 0x0A00 + 6 (0x0A06), grown by appending fields ([channel-modes.md](channel-modes.md) 5.5).

| Offset | Size | Field |
|---|---|---|
| 0 | 10 | Own callsign, text (1.1) |

An absent, empty or invalid record uses the fallback in 1.2. Readers ignore bytes beyond the first 10.

### 1.1 The callsign's characters

| Rule | Behavior |
|---|---|
| End of text | The first 0x00 or 0xFF ends the text. Trailing spaces (0x20) are discarded. |
| Length | 1 to 9 characters after trimming. Ten characters with no end are invalid. |
| Alphabet | `A`–`Z`, `0`–`9`, `-`, `/` and `.`. Readers fold `a`–`z` to upper case. A leading or internal space or any other character makes the record invalid. |
| Write | Upper case, padded with 0x00 to 10 bytes. |

### 1.2 Without a valid callsign

Use the radio name from **Settings > My radio**, or the codeplug's radio name if that setting is absent. Skip
leading spaces, take the text up to the first space or `-`, and convert it to upper case. This is the same split
that APRS uses. Validate the result by 1.1 without applying APRS's callsign-shape check.

If the result is invalid, the radio has no callsign and refuses M17 voice and packet transmissions. BERT can still
transmit ([m17.md](m17.md) 8.1). A callsign set through debug link operation 44 takes precedence over the record
and the fallback until restart. That override is stored in RAM ([debuglink.md](debuglink.md#38-radio)).

## 2. Conventional channel (kind 6)

The tag has no system (0xFF). Flag bit 0 allows transmit. The kind has no flags in bits 4–7, which are set to 0.
See [channel-modes.md](channel-modes.md) 2 for the tag layout.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Bits 0–3 hold the CAN sent (0–15, [m17.md](m17.md) 8.3). Bit 4 set disables CAN squelch so every CAN is played. With bit 4 clear, only the channel's CAN is played. Bits 5–7 are 0 in a new block. Readers ignore them and editors preserve them. |
| 1 | 6 | Destination address, big endian (2.1). A value of 0 means all (broadcast). |

Readers use the destination only when all six bytes are present. They read 0xFFFFFFFFFFFF and the reserved range
0xEE6B28000000–0xFFFFFFFFFFFE as all. Writers store all as 0. A zeroed or absent block gives CAN 0, squelch on and
a broadcast destination. The channel is receive only when flag bit 0 is clear. The full block is 7 bytes. Editors
preserve extra bytes.

### 2.1 The destination in base 40

The address is the sum of `v(cᵢ) × 40ⁱ`, with `i = 0` for the first character. A destination has at most 9
characters, so the largest base-40 address is 0xEE6B27FFFFFF. See [m17.md](m17.md) 7 for the on-air format.

| Character | Value |
|---|---|
| Space | 0 |
| `A`–`Z` | 1–26 |
| `0`–`9` | 27–36 |
| `-`, `/`, `.` | 37, 38, 39 |

For `AB1CD`, the sum is 10,476,881 (0x9FDD51).

| Character | Contribution |
|---|---|
| `A` | 1 × 1 = 1 |
| `B` | 2 × 40 = 80 |
| `1` | 28 × 1,600 = 44,800 |
| `C` | 3 × 64,000 = 192,000 |
| `D` | 4 × 2,560,000 = 10,240,000 |

| Destination | Stored bytes |
|---|---|
| `AB1CD` | `00 00 00 9F DD 51` |
| `ECHO` | `00 00 00 0E D8 7D` |
| `INFO` | `00 00 00 0E CD B9` |
| `UNLINK` | `00 00 45 4F 77 45` |
| `M17-M17 A` | `06 16 FA CE CA ED` |

Typed destinations accept the callsign characters in 1.1 and internal spaces, as in the reflector module
`M17-M17 A`. The editor trims spaces at both ends, as it does for every text field, and reads lower case as upper
case.
Blank text, `ALL` and `@ALL` in any case all store 0. Gateways may send the base-40 text `ALL` (0x4CE1) on air.

### 2.2 The base record

| Record field | Use |
|---|---|
| Name, RX and TX frequency, power, TX time-out, scan skip | As any channel |
| Contact, RX group list, per-channel DMR ID | Preserved and ignored |
| Mode, receive only | DMR, receive only (the safe stand-in) |

The radio runs the channel at 12.5 kHz regardless of the base record's bandwidth.

### 2.3 Writing from the radio

The Codeplug Editor writes kind 6 channels. A writer for kind 6 on the radio is still pending.

## 3. On the radio

| Channel | CAN, CAN squelch and destination | Own callsign |
|---|---|---|
| Stored kind 6 | The channel's parameter block (2) | The radio-wide callsign (1) |
| VFO in M17 | The M17 settings ([m17.md](m17.md) 10) | The radio-wide callsign (1) |

The radio gives the CAN and squelch to the engine. Sending the CAN in the link setup frame and applying CAN
squelch on receive are pending. [m17.md](m17.md) 3.5 lists the TYPE fields without their bit positions.

## 4. Sharing

Zone sharing carries the tag with its 7-byte parameter block intact, within the 64-byte block limit
([zone-share.md](zone-share.md) 4.2). The radio-wide record stays local.

## 5. Tests

The firmware test `tests/host/m17CodeplugTest.c` and the Codeplug Editor test `site/tests/modes.test.mjs` use
the shared vectors in [tests/vectors/channel-modes.txt](../../tests/vectors/channel-modes.txt).

| Vector | Covers |
|---|---|
| `m17conv` | Decode the channel's parameter block |
| `m17enc` | Encode the block from the CAN, squelch and destination text |
| `m17dstbad` | Reject invalid destination text |
| `m17rec` | Decode the radio-wide callsign record |
| `m17call` | Resolve the callsign from the record or radio name |
