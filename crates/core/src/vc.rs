//! GBA Virtual Console: the `.code` payload AGB_FIRM boots, and the size of
//! the save container the system keeps on the SD card.
//!
//! `.code` is the GBA ROM followed by a 0x360-byte footer:
//!
//! ```text
//! ROM (rom_size bytes, padded to a power of two)
//! config block (0x324)       read by AGB_FIRM
//! padding (0xC)
//! section table (2 x 0x10)   [0] ROM:    type 0, offset 0,        size rom_size
//!                            [1] config: type 1, offset rom_size, size 0x324
//! ".CAA" header (0x10)       magic, version 1, section table offset, count << 4
//! ```
//!
//! Config block (sources: 3dbrew "3DS Virtual Console" and "ARM7 Registers",
//! libn3ds `lgy9.c`, GodMode9 `gba.h`, comparison with NSUI-built titles):
//!
//! | offset | size  | field |
//! |--------|-------|-------|
//! | 0x000  | 4     | 0 |
//! | 0x004  | 4     | ROM size |
//! | 0x008  | 4     | save type (see [`agb_save_type`]) |
//! | 0x00C  | 2     | 0xFFFF |
//! | 0x00E  | 2     | button combination pressed when the lid closes (0 = none) |
//! | 0x010  | 16    | save chip timings: flash chip erase, sector erase, program, EEPROM write |
//! | 0x020  | 4     | LCD ghosting (0xFF = none, lower = stronger) |
//! | 0x024  | 0x300 | colour lookup table, 256 x RGB |

use crate::Error;
use crate::gba::SaveKind;

pub const CONFIG_SIZE: usize = 0x324;
pub const FOOTER_SIZE: usize = 0x360;

/// Size of the AGBSAVE header that precedes the save data.
pub const AGBSAVE_HEADER: u32 = 0x200;
/// Size of the NAND AGBSAVE partition, also the minimum SD container size.
pub const AGBSAVE_PARTITION: u32 = 0x30000;

/// Chip timings of the 512 Kbit flash / 4 Kbit EEPROM chips.
const TIMINGS_SMALL: [u32; 4] = [0x0027_C886, 0x0008_CE35, 0x0000_0184, 0x0003_1170];
/// Chip timings of the 1 Mbit flash / 64 Kbit EEPROM chips.
const TIMINGS_LARGE: [u32; 4] = [0x0017_D43E, 0x0002_6206, 0x0000_0086, 0x0002_DD13];

/// AGB_FIRM save type values.
///
/// | value     | chip |
/// |-----------|------|
/// | 0x0 / 0x1 | EEPROM 4 Kbit (0x1 for ROMs over 16 MiB) |
/// | 0x2 / 0x3 | EEPROM 64 Kbit (0x3 for ROMs over 16 MiB) |
/// | 0x8 / 0x9 | Flash 512 Kbit (Panasonic), with / without clock |
/// | 0xA / 0xB | Flash 1 Mbit (Macronix), with / without clock |
/// | 0xE       | SRAM / FRAM 256 Kbit |
/// | 0xF       | none |
///
/// The clock is part of the save type: SRAM and EEPROM games cannot have one.
pub fn agb_save_type(kind: SaveKind, rtc: bool, rom_size: usize) -> u32 {
    let big = rom_size > 16 * 1024 * 1024;
    match kind {
        SaveKind::Eeprom4k => 0x0 | big as u32,
        SaveKind::Eeprom64k => 0x2 | big as u32,
        SaveKind::Flash512k => {
            if rtc {
                0x8
            } else {
                0x9
            }
        }
        SaveKind::Flash1m => {
            if rtc {
                0xA
            } else {
                0xB
            }
        }
        SaveKind::Sram => 0xE,
        SaveKind::None => 0xF,
    }
}

/// Whether the clock can be emulated for this save chip.
pub fn rtc_supported(kind: SaveKind) -> bool {
    matches!(kind, SaveKind::Flash512k | SaveKind::Flash1m)
}

fn timings(save_type: u32) -> [u32; 4] {
    match save_type {
        0x2 | 0x3 | 0xA..=0xD => TIMINGS_LARGE,
        _ => TIMINGS_SMALL,
    }
}

/// Size of the save container on the SD card: two slots of header + save,
/// and never less than the AGBSAVE partition. The system silently drops the
/// save when the file has any other size, so this must match the save type.
pub fn save_container_size(kind: SaveKind) -> u32 {
    AGBSAVE_PARTITION.max(2 * (AGBSAVE_HEADER + kind.size() as u32))
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Colour {
    /// No colour correction and no ghosting: the picture of an emulator.
    Vivid,
    /// The dimmer picture and LCD ghosting of Nintendo's Virtual Console titles.
    Original,
}

pub struct VcOptions {
    pub save: SaveKind,
    pub rtc: bool,
    pub colour: Colour,
    /// Button combination AGB_FIRM presses when the lid closes, in GBA key
    /// bits (L+R+SELECT = 0x0304). Only useful with a game or patch that
    /// sleeps on that combination.
    pub sleep_combo: u16,
}

/// Pad a ROM with 0xFF to the next power of two, as a real cartridge bus
/// would read past the end of a trimmed dump.
pub fn pad_rom(rom: &[u8]) -> Vec<u8> {
    let size = rom
        .len()
        .next_power_of_two()
        .max(0x10_0000)
        .min(crate::gba::MAX_ROM_SIZE);
    let mut out = rom.to_vec();
    out.resize(size.max(rom.len()), 0xFF);
    out
}

fn config_block(rom_size: usize, opt: &VcOptions) -> Vec<u8> {
    let save_type = agb_save_type(opt.save, opt.rtc, rom_size);
    let mut cfg = vec![0u8; CONFIG_SIZE];
    cfg[0x4..0x8].copy_from_slice(&(rom_size as u32).to_le_bytes());
    cfg[0x8..0xC].copy_from_slice(&save_type.to_le_bytes());
    cfg[0xC..0xE].copy_from_slice(&0xFFFFu16.to_le_bytes());
    cfg[0xE..0x10].copy_from_slice(&opt.sleep_combo.to_le_bytes());
    for (i, t) in timings(save_type).iter().enumerate() {
        cfg[0x10 + i * 4..0x14 + i * 4].copy_from_slice(&t.to_le_bytes());
    }
    let (ghosting, scale) = match opt.colour {
        Colour::Vivid => (0xFFu32, 1.0f32),
        Colour::Original => (0x80u32, 0.7f32),
    };
    cfg[0x20..0x24].copy_from_slice(&ghosting.to_le_bytes());
    for i in 0..256usize {
        let v = ((i as f32) * scale).floor() as u8;
        cfg[0x24 + i * 3..0x27 + i * 3].fill(v);
    }
    cfg
}

/// Build `.code` from an already padded ROM.
pub fn build_code(rom: &[u8], opt: &VcOptions) -> Result<Vec<u8>, Error> {
    if rom.len() > crate::gba::MAX_ROM_SIZE {
        return Err(Error::Rom("ROM is larger than 32 MiB".into()));
    }
    if rom.len() % 4 != 0 {
        return Err(Error::Rom("ROM size must be a multiple of 4".into()));
    }
    let rom_size = rom.len() as u32;
    let mut code = Vec::with_capacity(rom.len() + FOOTER_SIZE);
    code.extend_from_slice(rom);
    code.extend_from_slice(&config_block(rom.len(), opt));
    code.extend_from_slice(&[0u8; 0xC]);
    let table_off = rom_size + CONFIG_SIZE as u32 + 0xC;
    for (kind, off, size) in [(0u32, 0u32, rom_size), (1, rom_size, CONFIG_SIZE as u32)] {
        code.extend_from_slice(&kind.to_le_bytes());
        code.extend_from_slice(&off.to_le_bytes());
        code.extend_from_slice(&size.to_le_bytes());
        code.extend_from_slice(&0u32.to_le_bytes());
    }
    code.extend_from_slice(b".CAA");
    code.extend_from_slice(&1u32.to_le_bytes());
    code.extend_from_slice(&table_off.to_le_bytes());
    code.extend_from_slice(&(2u32 << 4).to_le_bytes());
    debug_assert_eq!(code.len(), rom.len() + FOOTER_SIZE);
    Ok(code)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn container_sizes() {
        assert_eq!(save_container_size(SaveKind::Flash1m), 0x40400);
        for k in [
            SaveKind::None,
            SaveKind::Eeprom4k,
            SaveKind::Eeprom64k,
            SaveKind::Sram,
            SaveKind::Flash512k,
        ] {
            assert_eq!(save_container_size(k), 0x30000);
        }
    }

    #[test]
    fn save_types() {
        assert_eq!(agb_save_type(SaveKind::Flash1m, true, 32 << 20), 0xA);
        assert_eq!(agb_save_type(SaveKind::Flash1m, false, 16 << 20), 0xB);
        assert_eq!(agb_save_type(SaveKind::Eeprom4k, false, 32 << 20), 0x1);
        assert_eq!(agb_save_type(SaveKind::Eeprom64k, false, 8 << 20), 0x2);
        assert_eq!(agb_save_type(SaveKind::None, false, 8 << 20), 0xF);
    }

    #[test]
    fn config_matches_official_layout() {
        // The template vcoven extracted from an official EEPROM 64K title.
        let official = include_bytes!("../../../assets/template/config_block.bin");
        let cfg = config_block(
            0x800000,
            &VcOptions {
                save: SaveKind::Eeprom64k,
                rtc: false,
                colour: Colour::Original,
                sleep_combo: 0,
            },
        );
        // Same timings, ghosting and marker word as the official title.
        assert_eq!(&cfg[0xC..0x24], &official[0xC..0x24]);
    }

    #[test]
    fn padding() {
        assert_eq!(pad_rom(&vec![0u8; 3 * 1024 * 1024]).len(), 4 * 1024 * 1024);
        assert_eq!(
            pad_rom(&vec![0u8; 32 * 1024 * 1024]).len(),
            32 * 1024 * 1024
        );
        let p = pad_rom(&[1, 2, 3]);
        assert_eq!(p.len(), 0x10_0000);
        assert_eq!(p[3], 0xFF);
    }
}
