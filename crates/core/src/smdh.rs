//! SMDH: the icon and title metadata the HOME Menu reads (ExeFS file `icon`).

use crate::image_util::{Rgba, swizzle_rgb565};

pub const SMDH_SIZE: usize = 0x36C0;

pub mod flags {
    pub const VISIBLE: u32 = 0x0001;
    pub const AUTO_BOOT: u32 = 0x0002;
    pub const ALLOW_3D: u32 = 0x0004;
    pub const REQUIRE_EULA: u32 = 0x0008;
    pub const AUTOSAVE_ON_EXIT: u32 = 0x0010;
    pub const EXTENDED_BANNER: u32 = 0x0020;
    pub const RATING_REQUIRED: u32 = 0x0040;
    pub const USES_SAVE_DATA: u32 = 0x0080;
    pub const RECORD_USAGE: u32 = 0x0100;
    pub const DISABLE_SAVE_BACKUPS: u32 = 0x0400;
    pub const NEW_3DS_ONLY: u32 = 0x1000;
}

pub const REGION_FREE: u32 = 0x7FFF_FFFF;

pub struct Smdh<'a> {
    pub short_title: &'a str,
    pub long_title: &'a str,
    pub publisher: &'a str,
    /// 48x48 icon.
    pub icon_large: &'a Rgba,
    /// 24x24 icon.
    pub icon_small: &'a Rgba,
    pub flags: u32,
}

fn put_utf16(out: &mut [u8], text: &str) {
    // Leave room for a terminating NUL code unit.
    let max_units = out.len() / 2 - 1;
    for (i, unit) in text.encode_utf16().take(max_units).enumerate() {
        out[i * 2..i * 2 + 2].copy_from_slice(&unit.to_le_bytes());
    }
}

impl Smdh<'_> {
    pub fn build(&self) -> Vec<u8> {
        let mut out = vec![0u8; SMDH_SIZE];
        out[0..4].copy_from_slice(b"SMDH");
        // version 0, reserved
        for lang in 0..16 {
            let base = 0x8 + lang * 0x200;
            put_utf16(&mut out[base..base + 0x80], self.short_title);
            put_utf16(&mut out[base + 0x80..base + 0x180], self.long_title);
            put_utf16(&mut out[base + 0x180..base + 0x200], self.publisher);
        }
        // Application settings at 0x2008.
        // Age ratings (0x2008..0x2018) left inactive.
        out[0x2018..0x201C].copy_from_slice(&REGION_FREE.to_le_bytes());
        // Match maker IDs (0x201C, 0x2020) unused.
        out[0x2028..0x202C].copy_from_slice(&self.flags.to_le_bytes());
        // EULA version 0.0, reserved, optimal animation frame 0.0, CEC ID 0.
        out[0x2040..0x2040 + 0x480].copy_from_slice(&swizzle_rgb565(self.icon_small));
        out[0x24C0..0x24C0 + 0x1200].copy_from_slice(&swizzle_rgb565(self.icon_large));
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layout() {
        let large = Rgba::filled(48, 48, [255, 0, 0, 255]);
        let small = Rgba::filled(24, 24, [0, 0, 255, 255]);
        let s = Smdh {
            short_title: "Pokemon Unbound",
            long_title: "Pokemon Unbound",
            publisher: "Skeli",
            icon_large: &large,
            icon_small: &small,
            flags: flags::VISIBLE,
        }
        .build();
        assert_eq!(s.len(), 14016);
        assert_eq!(&s[0..4], b"SMDH");
        assert_eq!(&s[8..10], &[b'P', 0]);
        // Pure red in RGB565 is 0xF800.
        assert_eq!(&s[0x24C0..0x24C2], &[0x00, 0xF8]);
        // Pure blue is 0x001F.
        assert_eq!(&s[0x2040..0x2042], &[0x1F, 0x00]);
    }
}
