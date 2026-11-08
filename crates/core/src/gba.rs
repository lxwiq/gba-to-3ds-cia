//! GBA ROM inspection: cartridge header, save hardware and real-time clock.

use crate::Error;

/// Smallest file that still contains a full cartridge header.
pub const HEADER_END: usize = 0xC0;
/// Largest GBA ROM the cartridge bus can address.
pub const MAX_ROM_SIZE: usize = 32 * 1024 * 1024;

/// Save hardware found on GBA cartridges.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SaveKind {
    None,
    /// 4 Kbit (512 byte) serial EEPROM.
    Eeprom4k,
    /// 64 Kbit (8 KiB) serial EEPROM.
    Eeprom64k,
    /// 256 Kbit (32 KiB) battery-backed SRAM or FRAM.
    Sram,
    /// 512 Kbit (64 KiB) flash.
    Flash512k,
    /// 1 Mbit (128 KiB) flash, two banks. This is the "Flash 128K" type.
    Flash1m,
}

impl SaveKind {
    /// Size of the save in bytes, as stored in an emulator `.sav` file.
    pub fn size(self) -> usize {
        match self {
            Self::None => 0,
            Self::Eeprom4k => 512,
            Self::Eeprom64k => 8 * 1024,
            Self::Sram => 32 * 1024,
            Self::Flash512k => 64 * 1024,
            Self::Flash1m => 128 * 1024,
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Self::None => "No save",
            Self::Eeprom4k => "EEPROM 4 Kbit (512 B)",
            Self::Eeprom64k => "EEPROM 64 Kbit (8 KB)",
            Self::Sram => "SRAM 256 Kbit (32 KB)",
            Self::Flash512k => "Flash 512 Kbit (64 KB)",
            Self::Flash1m => "Flash 1 Mbit (128 KB)",
        }
    }

    pub fn id(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::Eeprom4k => "eeprom4k",
            Self::Eeprom64k => "eeprom64k",
            Self::Sram => "sram",
            Self::Flash512k => "flash512k",
            Self::Flash1m => "flash1m",
        }
    }

    pub fn from_id(s: &str) -> Option<Self> {
        Some(match s.to_ascii_lowercase().as_str() {
            "none" => Self::None,
            "eeprom4k" | "eeprom512" | "eeprom-4k" => Self::Eeprom4k,
            "eeprom64k" | "eeprom8k" | "eeprom-64k" | "eeprom" => Self::Eeprom64k,
            "sram" | "fram" | "sram32k" => Self::Sram,
            "flash512k" | "flash64k" | "flash512" | "flash-64k" => Self::Flash512k,
            "flash1m" | "flash128k" | "flash128" | "flash-128k" => Self::Flash1m,
            _ => return None,
        })
    }

    pub const ALL: [SaveKind; 6] = [
        Self::None,
        Self::Eeprom4k,
        Self::Eeprom64k,
        Self::Sram,
        Self::Flash512k,
        Self::Flash1m,
    ];
}

/// How a detection result was obtained.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Confidence {
    /// Matched a known game code.
    Database,
    /// Found the save library signature in the ROM.
    Signature,
    /// Guessed (for example the EEPROM size, which the signature does not reveal).
    Guess,
    /// No signature found at all.
    Default,
}

#[derive(Debug, Clone)]
pub struct RomInfo {
    /// Internal title (12 ASCII characters at 0xA0).
    pub title: String,
    /// Four-character game code (0xAC), for example `BPRE` for FireRed (USA).
    pub game_code: String,
    /// Two-character maker code (0xB0).
    pub maker_code: String,
    pub version: u8,
    pub size: usize,
    pub header_checksum_ok: bool,
    /// Save library version string found in the ROM, such as `FLASH1M_V103`.
    pub save_signature: Option<String>,
    pub save: SaveKind,
    pub save_confidence: Confidence,
    /// The ROM contains the Seiko S-3511 RTC library (`SIIRTC_V`), or is a known
    /// game that needs a clock.
    pub rtc: bool,
    pub rtc_reason: Option<&'static str>,
    /// Human readable notes about the detection.
    pub notes: Vec<String>,
}

fn ascii(b: &[u8]) -> String {
    b.iter()
        .take_while(|&&c| c != 0)
        .map(|&c| {
            if (0x20..0x7f).contains(&c) {
                c as char
            } else {
                '?'
            }
        })
        .collect::<String>()
        .trim_end()
        .to_string()
}

/// Header complement check at 0xBD, which the BIOS verifies at boot.
pub fn header_checksum(rom: &[u8]) -> u8 {
    let mut chk: u8 = 0;
    for &b in &rom[0xA0..0xBD] {
        chk = chk.wrapping_sub(b);
    }
    chk.wrapping_sub(0x19)
}

/// Find `needle` at a 4-byte aligned offset, as the Nintendo SDK stores its
/// library identification strings word aligned.
fn find_aligned(rom: &[u8], needle: &[u8]) -> Option<usize> {
    let n = needle.len();
    let mut i = 0;
    while i + n <= rom.len() {
        if &rom[i..i + n] == needle {
            return Some(i);
        }
        i += 4;
    }
    None
}

fn signature_at(rom: &[u8], at: usize, prefix_len: usize) -> String {
    // e.g. "FLASH1M_V103": prefix followed by three version digits.
    let end = (at + prefix_len + 3).min(rom.len());
    ascii(&rom[at..end])
}

/// Games whose save hardware is known and differs from what the signature
/// scan would suggest, or which need a clock the scan cannot see.
/// Keyed by the first three characters of the game code (region independent).
struct Known {
    code3: &'static str,
    save: Option<SaveKind>,
    rtc: bool,
    note: &'static str,
}

const KNOWN: &[Known] = &[
    // Pokemon: Ruby, Sapphire and Emerald use Flash 1M plus the RTC.
    Known {
        code3: "AXV",
        save: Some(SaveKind::Flash1m),
        rtc: true,
        note: "Pokemon Ruby",
    },
    Known {
        code3: "AXP",
        save: Some(SaveKind::Flash1m),
        rtc: true,
        note: "Pokemon Sapphire",
    },
    Known {
        code3: "BPE",
        save: Some(SaveKind::Flash1m),
        rtc: true,
        note: "Pokemon Emerald",
    },
    // FireRed and LeafGreen use Flash 1M. The original games have no clock,
    // but most ROM hacks built on them keep the game code and add one
    // (Pokemon Unbound's day/night system reads the RTC). Enabling the clock
    // is harmless for a game that never talks to it, so it is on by default.
    Known {
        code3: "BPR",
        save: Some(SaveKind::Flash1m),
        rtc: true,
        note: "Pokemon FireRed engine",
    },
    Known {
        code3: "BPG",
        save: Some(SaveKind::Flash1m),
        rtc: true,
        note: "Pokemon LeafGreen engine",
    },
    // Boktai series: RTC (and a solar sensor, which cannot be emulated by AGB_FIRM).
    Known {
        code3: "U3I",
        save: None,
        rtc: true,
        note: "Boktai",
    },
    Known {
        code3: "U32",
        save: None,
        rtc: true,
        note: "Boktai 2",
    },
    // Sennen Kazoku.
    Known {
        code3: "BKA",
        save: None,
        rtc: true,
        note: "Sennen Kazoku",
    },
    // Rockman EXE 4.5.
    Known {
        code3: "BR4",
        save: None,
        rtc: true,
        note: "Rockman EXE 4.5",
    },
    // Classic NES Series / Famicom Mini use EEPROM 4 Kbit.
    Known {
        code3: "FBM",
        save: Some(SaveKind::Eeprom4k),
        rtc: false,
        note: "Classic NES Series",
    },
    Known {
        code3: "FSM",
        save: Some(SaveKind::Eeprom4k),
        rtc: false,
        note: "Classic NES Series",
    },
    Known {
        code3: "FZL",
        save: Some(SaveKind::Eeprom4k),
        rtc: false,
        note: "Classic NES Series",
    },
    // The Legend of Zelda: The Minish Cap uses EEPROM 64 Kbit.
    Known {
        code3: "BZM",
        save: Some(SaveKind::Eeprom64k),
        rtc: false,
        note: "Zelda: The Minish Cap",
    },
    // Metroid Fusion / Zero Mission: SRAM.
    Known {
        code3: "AMT",
        save: Some(SaveKind::Sram),
        rtc: false,
        note: "Metroid Fusion",
    },
    Known {
        code3: "BMX",
        save: Some(SaveKind::Sram),
        rtc: false,
        note: "Metroid Zero Mission",
    },
];

pub fn inspect(rom: &[u8]) -> Result<RomInfo, Error> {
    if rom.len() < HEADER_END {
        return Err(Error::Rom("file is too small to be a GBA ROM".into()));
    }
    if rom.len() > MAX_ROM_SIZE {
        return Err(Error::Rom(format!(
            "ROM is {} bytes; GBA ROMs cannot exceed 32 MiB",
            rom.len()
        )));
    }
    if rom[0xB2] != 0x96 {
        return Err(Error::Rom(
            "missing GBA header marker (byte 0xB2 is not 0x96); is this really a .gba file?".into(),
        ));
    }

    let title = ascii(&rom[0xA0..0xAC]);
    let game_code = ascii(&rom[0xAC..0xB0]);
    let maker_code = ascii(&rom[0xB0..0xB2]);
    let version = rom[0xBC];
    let header_checksum_ok = header_checksum(rom) == rom[0xBD];
    let mut notes = Vec::new();
    if !header_checksum_ok {
        notes.push(
            "Header checksum is wrong; it will be corrected in the built title (the GBA BIOS refuses to boot otherwise)."
                .to_string(),
        );
    }

    // Save library signatures, most specific first.
    let mut save_signature = None;
    let mut save = SaveKind::None;
    let mut confidence = Confidence::Default;
    let probes: &[(&[u8], SaveKind)] = &[
        (b"FLASH1M_V", SaveKind::Flash1m),
        (b"FLASH512_V", SaveKind::Flash512k),
        (b"FLASH_V", SaveKind::Flash512k),
        (b"SRAM_F_V", SaveKind::Sram),
        (b"SRAM_V", SaveKind::Sram),
        (b"EEPROM_V", SaveKind::Eeprom64k),
    ];
    for (needle, kind) in probes {
        if let Some(at) = find_aligned(rom, needle) {
            save_signature = Some(signature_at(rom, at, needle.len()));
            save = *kind;
            confidence = Confidence::Signature;
            break;
        }
    }
    if save == SaveKind::Eeprom64k {
        // The EEPROM library supports both sizes and the game picks one at
        // run time. The library version is a good hint (heuristic used by
        // open_agb_firm): V121 and V124 were used by 64 Kbit games, the
        // other versions mostly by 4 Kbit games.
        let sig = save_signature.as_deref().unwrap_or("");
        save = if sig.ends_with("V121") || sig.ends_with("V124") {
            SaveKind::Eeprom64k
        } else {
            SaveKind::Eeprom4k
        };
        confidence = Confidence::Guess;
        notes.push(format!(
            "The EEPROM size is guessed from the library version ({sig}): {}. If the game does not save, rebuild with the other EEPROM size.",
            save.label()
        ));
    }
    if save == SaveKind::None {
        notes.push("No save library signature found; the game probably has no save.".to_string());
    }

    let mut rtc = false;
    let mut rtc_reason = None;
    if find_aligned(rom, b"SIIRTC_V").is_some() {
        rtc = true;
        rtc_reason = Some("ROM contains the RTC library (SIIRTC_V)");
    }

    let code3: String = game_code.chars().take(3).collect();
    if let Some(k) = KNOWN.iter().find(|k| k.code3 == code3) {
        if let Some(s) = k.save {
            if s != save {
                notes.push(format!(
                    "Game code {game_code} ({}) is known to use {}.",
                    k.note,
                    s.label()
                ));
            }
            save = s;
            confidence = Confidence::Database;
        }
        if k.rtc && !rtc {
            rtc = true;
            rtc_reason = Some(if code3 == "BPR" || code3 == "BPG" {
                "FireRed/LeafGreen engine: ROM hacks such as Pokemon Unbound read the clock"
            } else {
                "known game that uses the real-time clock"
            });
        }
    }

    Ok(RomInfo {
        title,
        game_code,
        maker_code,
        version,
        size: rom.len(),
        header_checksum_ok,
        save_signature,
        save,
        save_confidence: confidence,
        rtc,
        rtc_reason,
        notes,
    })
}

/// Return a copy of `rom` with a valid header checksum.
pub fn fix_header_checksum(rom: &mut [u8]) {
    rom[0xBD] = header_checksum(rom);
}
