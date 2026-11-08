//! Default artwork used when no icon or banner image is supplied: a plain
//! cartridge pictogram on a colour derived from the game code, so different
//! games are easy to tell apart on the HOME Menu.

use crate::image_util::Rgba;
use crate::ncch::sha256;

fn put(img: &mut Rgba, x: i32, y: i32, c: [u8; 4]) {
    if x >= 0 && y >= 0 && (x as u32) < img.width && (y as u32) < img.height {
        let i = ((y as u32 * img.width + x as u32) * 4) as usize;
        img.pixels[i..i + 4].copy_from_slice(&c);
    }
}

fn rect(img: &mut Rgba, x: i32, y: i32, w: i32, h: i32, c: [u8; 4]) {
    for yy in y..y + h {
        for xx in x..x + w {
            put(img, xx, yy, c);
        }
    }
}

/// HSL (h in degrees) to RGB.
fn hsl(h: f32, s: f32, l: f32) -> [u8; 4] {
    let c = (1.0 - (2.0 * l - 1.0).abs()) * s;
    let hp = h / 60.0;
    let x = c * (1.0 - (hp % 2.0 - 1.0).abs());
    let (r, g, b) = match hp as u32 {
        0 => (c, x, 0.0),
        1 => (x, c, 0.0),
        2 => (0.0, c, x),
        3 => (0.0, x, c),
        4 => (x, 0.0, c),
        _ => (c, 0.0, x),
    };
    let m = l - c / 2.0;
    let f = |v: f32| ((v + m) * 255.0).round().clamp(0.0, 255.0) as u8;
    [f(r), f(g), f(b), 255]
}

pub fn accent(seed: &str) -> [u8; 4] {
    let h = sha256(seed.as_bytes());
    let hue = (u16::from_le_bytes([h[0], h[1]]) % 360) as f32;
    hsl(hue, 0.45, 0.42)
}

/// Draw a GBA cartridge pictogram of `scale` units (1 unit = 1/16 of the size).
fn cartridge(img: &mut Rgba, ox: i32, oy: i32, unit: i32, body: [u8; 4], label: [u8; 4]) {
    // Body with a notched top-right corner.
    rect(img, ox + 2 * unit, oy + unit, 12 * unit, 14 * unit, body);
    rect(
        img,
        ox + 11 * unit,
        oy + unit,
        3 * unit,
        2 * unit,
        [0, 0, 0, 0],
    );
    rect(img, ox + 11 * unit, oy + 3 * unit, 3 * unit, unit / 2, body);
    // Label.
    rect(img, ox + 4 * unit, oy + 4 * unit, 8 * unit, 6 * unit, label);
    // Contacts.
    let gold = [0xC8, 0xA8, 0x4A, 0xFF];
    let mut x = ox + 4 * unit;
    while x + unit <= ox + 12 * unit {
        rect(img, x, oy + 12 * unit, unit / 2 + 1, 2 * unit, gold);
        x += unit;
    }
}

pub fn default_icon(seed: &str) -> Rgba {
    let mut img = Rgba::filled(48, 48, accent(seed));
    cartridge(
        &mut img,
        0,
        0,
        3,
        [0xEE, 0xEE, 0xEE, 0xFF],
        [0x30, 0x30, 0x30, 0xFF],
    );
    img
}

pub fn default_banner(seed: &str) -> Rgba {
    let bg = accent(seed);
    let mut img = Rgba::filled(256, 128, [0, 0, 0, 0]);
    rect(&mut img, 0, 16, 256, 96, bg);
    cartridge(
        &mut img,
        96,
        32,
        4,
        [0xEE, 0xEE, 0xEE, 0xFF],
        [0x30, 0x30, 0x30, 0xFF],
    );
    img
}
