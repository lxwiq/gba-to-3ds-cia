//! Image helpers: decoding user artwork and converting to 3DS texture layouts.

use crate::Error;
use image::imageops::FilterType;

/// Simple owned RGBA8 image.
#[derive(Clone, Debug)]
pub struct Rgba {
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u8>,
}

impl Rgba {
    pub fn filled(width: u32, height: u32, color: [u8; 4]) -> Self {
        let mut pixels = Vec::with_capacity((width * height * 4) as usize);
        for _ in 0..width * height {
            pixels.extend_from_slice(&color);
        }
        Self {
            width,
            height,
            pixels,
        }
    }

    pub fn get(&self, x: u32, y: u32) -> [u8; 4] {
        let i = ((y * self.width + x) * 4) as usize;
        [
            self.pixels[i],
            self.pixels[i + 1],
            self.pixels[i + 2],
            self.pixels[i + 3],
        ]
    }

    fn from_image(img: image::RgbaImage) -> Self {
        Self {
            width: img.width(),
            height: img.height(),
            pixels: img.into_raw(),
        }
    }

    fn to_image(&self) -> image::RgbaImage {
        image::RgbaImage::from_raw(self.width, self.height, self.pixels.clone())
            .expect("pixel buffer matches dimensions")
    }

    /// Composite onto an opaque background colour.
    pub fn flatten(&self, bg: [u8; 3]) -> Self {
        let mut out = self.clone();
        for px in out.pixels.chunks_exact_mut(4) {
            let a = px[3] as u32;
            for c in 0..3 {
                px[c] = ((px[c] as u32 * a + bg[c] as u32 * (255 - a) + 127) / 255) as u8;
            }
            px[3] = 255;
        }
        out
    }
}

pub fn decode(bytes: &[u8]) -> Result<Rgba, Error> {
    let img = image::load_from_memory(bytes)
        .map_err(|e| Error::Image(format!("cannot decode image: {e}")))?;
    Ok(Rgba::from_image(img.to_rgba8()))
}

/// Centre-crop to a square and resize. Nearest-neighbour is used when the
/// source is pixel art that scales by an integer factor, which keeps sprites
/// crisp; otherwise Lanczos.
pub fn square_icon(src: &Rgba, side: u32) -> Rgba {
    let img = src.to_image();
    let s = img.width().min(img.height());
    let x = (img.width() - s) / 2;
    let y = (img.height() - s) / 2;
    let cropped = image::imageops::crop_imm(&img, x, y, s, s).to_image();
    let filter = if s % side == 0 || side % s == 0 {
        FilterType::Nearest
    } else {
        FilterType::Lanczos3
    };
    Rgba::from_image(image::imageops::resize(&cropped, side, side, filter))
}

/// Fit inside `w`x`h` keeping the aspect ratio, centred on a transparent canvas.
pub fn fit(src: &Rgba, w: u32, h: u32) -> Rgba {
    let img = src.to_image();
    let scale = (w as f64 / img.width() as f64).min(h as f64 / img.height() as f64);
    let nw = ((img.width() as f64 * scale).round() as u32).clamp(1, w);
    let nh = ((img.height() as f64 * scale).round() as u32).clamp(1, h);
    let integer = (scale.fract() == 0.0) || ((1.0 / scale).fract() == 0.0);
    let filter = if integer {
        FilterType::Nearest
    } else {
        FilterType::Lanczos3
    };
    let resized = image::imageops::resize(&img, nw, nh, filter);
    let mut canvas = image::RgbaImage::new(w, h);
    image::imageops::overlay(
        &mut canvas,
        &resized,
        ((w - nw) / 2) as i64,
        ((h - nh) / 2) as i64,
    );
    Rgba::from_image(canvas)
}

/// Resize to exactly `w`x`h` (cover: crop the overflow).
pub fn cover(src: &Rgba, w: u32, h: u32) -> Rgba {
    let img = src.to_image();
    let scale = (w as f64 / img.width() as f64).max(h as f64 / img.height() as f64);
    let nw = ((img.width() as f64 * scale).ceil() as u32).max(w);
    let nh = ((img.height() as f64 * scale).ceil() as u32).max(h);
    let resized = image::imageops::resize(&img, nw, nh, FilterType::Lanczos3);
    let x = (nw - w) / 2;
    let y = (nh - h) / 2;
    Rgba::from_image(image::imageops::crop_imm(&resized, x, y, w, h).to_image())
}

pub fn encode_png(img: &Rgba) -> Vec<u8> {
    let mut out = std::io::Cursor::new(Vec::new());
    img.to_image()
        .write_to(&mut out, image::ImageFormat::Png)
        .expect("PNG encoding to memory cannot fail");
    out.into_inner()
}

/// Morton (Z-order) index of a pixel inside an 8x8 tile.
#[inline]
pub fn morton8(x: u32, y: u32) -> u32 {
    let mut m = 0;
    for bit in 0..3 {
        m |= ((x >> bit) & 1) << (2 * bit);
        m |= ((y >> bit) & 1) << (2 * bit + 1);
    }
    m
}

/// Iterate the pixels of an image in the 3DS GPU tiled order: 8x8 tiles in
/// raster order, Morton order inside each tile. Calls `f(x, y)` in output order.
pub fn for_each_tiled(width: u32, height: u32, mut f: impl FnMut(u32, u32)) {
    let mut order = [(0u32, 0u32); 64];
    for y in 0..8 {
        for x in 0..8 {
            order[morton8(x, y) as usize] = (x, y);
        }
    }
    for ty in (0..height).step_by(8) {
        for tx in (0..width).step_by(8) {
            for &(x, y) in &order {
                f(tx + x, ty + y);
            }
        }
    }
}

/// Tiled RGB565 little endian, as used by SMDH icons. Alpha is composited on
/// black first, since the HOME Menu icon format has no alpha channel.
pub fn swizzle_rgb565(img: &Rgba) -> Vec<u8> {
    let flat = img.flatten([0, 0, 0]);
    let mut out = Vec::with_capacity((img.width * img.height * 2) as usize);
    for_each_tiled(img.width, img.height, |x, y| {
        let [r, g, b, _] = flat.get(x, y);
        let v: u16 = ((r as u16 >> 3) << 11) | ((g as u16 >> 2) << 5) | (b as u16 >> 3);
        out.extend_from_slice(&v.to_le_bytes());
    });
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn morton_matches_reference_table() {
        const LUT: [u32; 64] = [
            0, 1, 4, 5, 16, 17, 20, 21, 2, 3, 6, 7, 18, 19, 22, 23, 8, 9, 12, 13, 24, 25, 28, 29,
            10, 11, 14, 15, 26, 27, 30, 31, 32, 33, 36, 37, 48, 49, 52, 53, 34, 35, 38, 39, 50, 51,
            54, 55, 40, 41, 44, 45, 56, 57, 60, 61, 42, 43, 46, 47, 58, 59, 62, 63,
        ];
        for y in 0..8 {
            for x in 0..8 {
                assert_eq!(morton8(x, y), LUT[(y * 8 + x) as usize]);
            }
        }
    }
}
