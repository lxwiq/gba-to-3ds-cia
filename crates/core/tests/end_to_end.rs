//! Build a CIA from a synthetic ROM and check every layer of the result.

use gba2cia_core::build::{BuildOptions, build};
use gba2cia_core::gba::SaveKind;
use gba2cia_core::{cia, ncch, vc};

fn fake_rom(size: usize, save_lib: &[u8], code: &[u8; 4]) -> Vec<u8> {
    let mut rom = vec![0u8; size];
    rom[0xA0..0xAC].copy_from_slice(b"TEST GAME   ");
    rom[0xAC..0xB0].copy_from_slice(code);
    rom[0xB0..0xB2].copy_from_slice(b"01");
    rom[0xB2] = 0x96;
    rom[0x1000..0x1000 + save_lib.len()].copy_from_slice(save_lib);
    // deliberately wrong header checksum
    rom[0xBD] = 0;
    rom
}

#[test]
fn virtual_console_cia() {
    let rom = fake_rom(4 * 1024 * 1024, b"FLASH1M_V103", b"BPRE");
    let out = build(&rom, &BuildOptions::default()).unwrap();
    assert_eq!(out.save, SaveKind::Flash1m);
    assert!(out.rtc, "FireRed engine gets the clock");

    let c = cia::parse(&out.cia).unwrap();
    assert_eq!(c.title_id, out.title_id);
    assert_eq!(
        c.save_data_size, 0x40400,
        "Flash 1M needs a 0x40400-byte save container"
    );
    let n = ncch::parse_ncch(c.content).unwrap();
    assert_eq!(n.title_id, out.title_id);
    let files = ncch::parse_exefs(n.exefs).unwrap();
    let names: Vec<&str> = files.iter().map(|(n, _)| n.as_str()).collect();
    assert_eq!(names, [".code", "banner", "icon", "logo"]);

    let code = &files[0].1;
    assert_eq!(code.len(), rom.len() + vc::FOOTER_SIZE);
    // ROM copied with a fixed header checksum
    assert_eq!(code[0xBD], gba2cia_core::gba::header_checksum(&rom));
    assert_eq!(&code[0xC0..rom.len()], &rom[0xC0..]);
    let cfg = &code[rom.len()..];
    assert_eq!(
        u32::from_le_bytes(cfg[4..8].try_into().unwrap()) as usize,
        rom.len()
    );
    assert_eq!(u32::from_le_bytes(cfg[8..12].try_into().unwrap()), 0xA); // Flash 1M + RTC
    assert_eq!(&code[code.len() - 16..code.len() - 12], b".CAA");

    // exheader IDs
    let exh = n.exheader;
    assert_eq!(
        u64::from_le_bytes(exh[0x1C8..0x1D0].try_into().unwrap()),
        out.title_id
    );
    assert_eq!(
        u64::from_le_bytes(exh[0x200..0x208].try_into().unwrap()),
        out.title_id
    );
    assert_eq!(&exh[0..4], b"BPRE");
    // meta carries the SMDH
    assert_eq!(&c.meta.unwrap()[0x400..0x404], b"SMDH");
}

#[test]
fn stable_title_id() {
    let rom = fake_rom(1024 * 1024, b"SRAM_V113", b"AMTE");
    let a = build(&rom, &BuildOptions::default()).unwrap();
    let b = build(&rom, &BuildOptions::default()).unwrap();
    assert_eq!(a.title_id, b.title_id);
    assert_eq!(a.cia, b.cia, "builds are reproducible");
    assert_eq!(a.save, SaveKind::Sram);
}
