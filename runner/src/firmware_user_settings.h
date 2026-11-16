#pragma once

// In-memory firmware patches (beads-yjp.16).
//
// Every one of these mutates THIS PROCESS's private copy of the firmware
// image after it has been loaded (retail dump) or synthesized (generated
// firmware) and after the dump's SHA-1 has already been verified against
// the pristine on-disk bytes — never the file on disk, and never the guest's
// view of hardware (the guest still reads all of it over its own SPI path,
// exactly like a physical console). melonDS's frontend does the same thing
// at the same point for the same reasons.
//
// These lived in main.cpp's anonymous namespace until the player-name patch
// needed a testable seam; they are hoisted verbatim so a minimal-link unit
// test can pin the byte layout and the CRC reseal for all of them
// (tests/firmware_user_settings_test.cpp), matching frontend_config_test's
// minimal-link style.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "frontend.h"  // NdsStartupMode

// Firmware CRC16 (melonDS SPI.cpp CRC16 == CRC-16/MODBUS). The accumulator
// MUST be 32-bit: the table terms overflow 16 bits and their high bits shift
// back into range — a uint16_t accumulator computes a DIFFERENT (wrong)
// checksum on most inputs, verified against the CRCs a real firmware dump
// stores.
uint16_t nds_firmware_crc16(const uint8_t* data, size_t len, uint32_t start);

// Deterministic, ideal DS touch calibration in both redundant user-settings
// blocks, so a host pixel means the same thing on both sides of the oracle.
bool nds_normalize_touch_calibration(std::vector<uint8_t>& fw);

// Firmware "start automatically / manually" user setting.
bool nds_apply_startup_mode(std::vector<uint8_t>& fw, NdsStartupMode mode);

// Firmware language (user settings +0x64 bits 0-2) in both copies, resealing
// each CRC. Preserve is a byte-exact no-op. A dump carrying iQue/DSi-style
// extended settings (+0x74 == 1) also gets its extended language (+0x75) and
// supported-language mask (+0x76) updated, since the console prefers that
// field, and its own CRC (+0xFE over +0x74..+0xFD) resealed.
bool nds_apply_firmware_language(std::vector<uint8_t>& fw,
                                 NdsFirmwareLanguage language);

// Per-instance guest MAC perturbation (Wiimmfi multi-instance identity).
// Instance 0 is a deliberate total no-op: LLE-faithful, the guest reads the
// real dump's MAC over SPI unperturbed.
bool nds_apply_instance_mac(std::vector<uint8_t>& fw, uint32_t instance_index);

// ---- Player name (the firmware console nickname) -------------------------
//
// The DS stores the nickname as up to 10 UTF-16LE code units at
// user-settings +0x06 with the length (in CHARACTERS, not bytes) at +0x1A,
// in BOTH redundant copies, each covered by the copy's own CRC16 at +0x72
// (start 0xFFFF over 0x70 bytes). Games surface it as the player's default
// name; WFC/Wiimmfi surface it as the online display name.

// 1..10 characters, printable ASCII subset (letters, digits, space, and
// common punctuation). Rejects anything else with a human-readable reason in
// *error — a too-long or unrepresentable name is NEVER silently truncated or
// transliterated, because the name the player typed is the name their peers
// must see.
bool nds_validate_player_name(const std::string& name, std::string* error);

// Writes `name` into both user-settings copies and reseals both CRCs.
// An EMPTY name is a deliberate no-op returning true: unset means "leave the
// firmware's own name alone" (a retail dump keeps its console's real
// nickname; a generated image keeps its "ndsrecomp" default). Returns false
// only for a malformed user-settings layout or an invalid name.
bool nds_apply_player_name(std::vector<uint8_t>& fw, const std::string& name);

// Reads the nickname back out of user-settings copy `copy` (0 or 1) as
// ASCII. Returns false for a malformed layout or a length/character the
// writer above could not have produced. Exists for the unit test and for
// provenance reporting; nothing in the boot path depends on it.
bool nds_read_player_name(const std::vector<uint8_t>& fw, unsigned copy,
                          std::string* out);

// ---- Wi-Fi connection slots vs the runner's virtual access point ---------
//
// The guest connects only to an AP whose SSID matches a configured
// connection slot (three 0x100-byte slots at user-settings - 0x400; SSID at
// +0x40, status at +0xE7, CRC16 start 0 over 0xFE bytes at +0xFE). The
// runner's AP calls itself "ndsrecomp" (vendor/melonds/patches/
// 0001-wifi-ap-identity.patch), so a dump whose slots were set up under
// melonDS ("melonAP") or a real router never finds it: error 51099.
//
// Rewrites every configured "melonAP" slot's SSID to the runner's AP,
// keeping the rest of the slot (WFC user ID, DNS, ...). If afterwards no
// configured slot names the runner's AP, the first unconfigured slot is set
// up for it (DHCP, no encryption). Slots set up for other networks stay as
// they are. In-memory only, like every patch here. *changed (optional)
// reports whether any byte moved. Returns false only for a malformed
// layout; a dump with three slots all configured for other networks is left
// untouched and still returns true.
bool nds_apply_wifi_ap_slot(std::vector<uint8_t>& fw, bool* changed);
