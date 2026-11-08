//! HOME Menu banner (ExeFS `banner`): CBMD container holding an LZ11
//! compressed CGFX model with a 256x128 texture, followed by a BCWAV jingle.
//!
//! The CGFX header (`assets/banner_cgfx_header.bin`, 0x1580 bytes) is the
//! template from bannertool (MIT, Steveice10 and contributors): a textured
//! quad whose material already blends with the texture alpha. The texture is
//! appended in RGBA4444, 8x8 tiled, exactly as bannertool does.

use crate::Error;
use crate::image_util::{Rgba, for_each_tiled};

pub const WIDTH: u32 = 256;
pub const HEIGHT: u32 = 128;

const CGFX_HEADER: &[u8] = include_bytes!("../../../assets/banner_cgfx_header.bin");
/// Longest jingle the HOME Menu plays correctly.
pub const MAX_SOUND_SECONDS: u32 = 3;

pub fn build(img: &Rgba, sound: Option<&[u8]>) -> Result<Vec<u8>, Error> {
    if img.width != WIDTH || img.height != HEIGHT {
        return Err(Error::Image(format!(
            "banner must be {WIDTH}x{HEIGHT}, got {}x{}",
            img.width, img.height
        )));
    }
    let cgfx = build_cgfx(img);
    let compressed = lz11_compress(&cgfx)?;
    let cwav = match sound {
        Some(wav) => wav_to_cwav(wav)?,
        None => silent_cwav(),
    };

    let mut out = vec![0u8; 0x88];
    out[0..4].copy_from_slice(b"CBMD");
    out[8..12].copy_from_slice(&0x88u32.to_le_bytes()); // common CGFX
    out.extend_from_slice(&compressed);
    out.resize(out.len().div_ceil(0x10) * 0x10, 0);
    let cwav_off = out.len() as u32;
    out[0x84..0x88].copy_from_slice(&cwav_off.to_le_bytes());
    out.extend_from_slice(&cwav);
    Ok(out)
}

fn build_cgfx(img: &Rgba) -> Vec<u8> {
    let mut out = CGFX_HEADER.to_vec();
    out.reserve((WIDTH * HEIGHT * 2) as usize);
    for_each_tiled(WIDTH, HEIGHT, |x, y| {
        let [r, g, b, a] = img.get(x, y);
        let v: u16 = ((r as u16 & 0xF0) << 8)
            | ((g as u16 & 0xF0) << 4)
            | (b as u16 & 0xF0)
            | (a as u16 >> 4);
        out.extend_from_slice(&v.to_le_bytes());
    });
    out
}

// ---------------------------------------------------------------- LZ11 ---

const LZ_WINDOW: usize = 0x1000;
const LZ_MAX: usize = 0x10110;
const LZ_MIN: usize = 3;

/// LZ11 compression (greedy, hash chains). Never emits a displacement of 1,
/// which some decoders mishandle.
pub fn lz11_compress(src: &[u8]) -> Result<Vec<u8>, Error> {
    if src.len() > 0xFF_FFFF {
        return Err(Error::Build("LZ11 input too large".into()));
    }
    let mut out = Vec::with_capacity(src.len() / 2 + 16);
    out.push(0x11);
    out.extend_from_slice(&(src.len() as u32).to_le_bytes()[..3]);

    const HASH_BITS: u32 = 14;
    let mut head = vec![usize::MAX; 1 << HASH_BITS];
    let mut prev = vec![usize::MAX; src.len()];
    let hash = |i: usize| -> usize {
        let v = (src[i] as u32) << 16 | (src[i + 1] as u32) << 8 | src[i + 2] as u32;
        (v.wrapping_mul(2_654_435_761) >> (32 - HASH_BITS)) as usize
    };
    let insert = |i: usize, head: &mut Vec<usize>, prev: &mut Vec<usize>| {
        if i + 2 < src.len() {
            let h = hash(i);
            prev[i] = head[h];
            head[h] = i;
        }
    };

    let mut pos = 0;
    while pos < src.len() {
        let flag_at = out.len();
        out.push(0);
        let mut flags = 0u8;
        for bit in 0..8 {
            if pos >= src.len() {
                break;
            }
            let mut best_len = 0;
            let mut best_disp = 0;
            if pos + LZ_MIN <= src.len() {
                let max_len = (src.len() - pos).min(LZ_MAX);
                let mut cand = head[hash(pos)];
                let mut tries = 0;
                while cand != usize::MAX && tries < 256 {
                    let disp = pos - cand;
                    if disp > LZ_WINDOW {
                        break;
                    }
                    if disp >= 2 {
                        let mut l = 0;
                        while l < max_len && src[cand + l] == src[pos + l] {
                            l += 1;
                        }
                        if l > best_len {
                            best_len = l;
                            best_disp = disp;
                            if l == max_len {
                                break;
                            }
                        }
                    }
                    cand = prev[cand];
                    tries += 1;
                }
            }
            if best_len >= LZ_MIN {
                flags |= 0x80 >> bit;
                let d = best_disp - 1;
                let l = best_len;
                if l <= 0x10 {
                    out.push((((l - 1) << 4) | (d >> 8)) as u8);
                    out.push(d as u8);
                } else if l <= 0x110 {
                    let m = l - 0x11;
                    out.push((m >> 4) as u8);
                    out.push((((m & 0xF) << 4) | (d >> 8)) as u8);
                    out.push(d as u8);
                } else {
                    let m = l - 0x111;
                    out.push((0x10 | (m >> 12)) as u8);
                    out.push((m >> 4) as u8);
                    out.push((((m & 0xF) << 4) | (d >> 8)) as u8);
                    out.push(d as u8);
                }
                for i in pos..pos + l {
                    insert(i, &mut head, &mut prev);
                }
                pos += l;
            } else {
                out.push(src[pos]);
                insert(pos, &mut head, &mut prev);
                pos += 1;
            }
        }
        out[flag_at] = flags;
    }
    out.resize(out.len().div_ceil(4) * 4, 0);
    Ok(out)
}

/// LZ11 decompression, used to verify the compressor.
pub fn lz11_decompress(src: &[u8]) -> Result<Vec<u8>, Error> {
    let bad = || Error::Build("corrupt LZ11 stream".into());
    if src.len() < 4 || src[0] != 0x11 {
        return Err(bad());
    }
    let size = u32::from_le_bytes([src[1], src[2], src[3], 0]) as usize;
    let mut out = Vec::with_capacity(size);
    let mut p = 4;
    while out.len() < size {
        let flags = *src.get(p).ok_or_else(bad)?;
        p += 1;
        for bit in 0..8 {
            if out.len() >= size {
                break;
            }
            if flags & (0x80 >> bit) == 0 {
                out.push(*src.get(p).ok_or_else(bad)?);
                p += 1;
                continue;
            }
            let b0 = *src.get(p).ok_or_else(bad)? as usize;
            let (len, d) = match b0 >> 4 {
                0 => {
                    let b1 = *src.get(p + 1).ok_or_else(bad)? as usize;
                    let b2 = *src.get(p + 2).ok_or_else(bad)? as usize;
                    p += 3;
                    (
                        (((b0 & 0xF) << 4) | (b1 >> 4)) + 0x11,
                        ((b1 & 0xF) << 8) | b2,
                    )
                }
                1 => {
                    let b1 = *src.get(p + 1).ok_or_else(bad)? as usize;
                    let b2 = *src.get(p + 2).ok_or_else(bad)? as usize;
                    let b3 = *src.get(p + 3).ok_or_else(bad)? as usize;
                    p += 4;
                    (
                        (((b0 & 0xF) << 12) | (b1 << 4) | (b2 >> 4)) + 0x111,
                        ((b2 & 0xF) << 8) | b3,
                    )
                }
                n => {
                    let b1 = *src.get(p + 1).ok_or_else(bad)? as usize;
                    p += 2;
                    (n + 1, ((b0 & 0xF) << 8) | b1)
                }
            };
            let start = out.len().checked_sub(d + 1).ok_or_else(bad)?;
            for i in 0..len {
                let b = out[start + i];
                out.push(b);
            }
        }
    }
    out.truncate(size);
    Ok(out)
}

// --------------------------------------------------------------- BCWAV ---

/// Decoded PCM16 stereo audio.
struct Pcm {
    rate: u32,
    left: Vec<i16>,
    right: Vec<i16>,
}

fn parse_wav(wav: &[u8]) -> Result<Pcm, Error> {
    let bad = |m: &str| Error::Option(format!("banner sound: {m}"));
    if wav.len() < 12 || &wav[0..4] != b"RIFF" || &wav[8..12] != b"WAVE" {
        return Err(bad("not a WAV file"));
    }
    let mut p = 12;
    let mut fmt: Option<(u16, u16, u32, u16)> = None;
    let mut data: Option<&[u8]> = None;
    while p + 8 <= wav.len() {
        let id = &wav[p..p + 4];
        let len = u32::from_le_bytes(wav[p + 4..p + 8].try_into().unwrap()) as usize;
        let body = &wav[p + 8..(p + 8 + len).min(wav.len())];
        match id {
            b"fmt " if body.len() >= 16 => {
                let format = u16::from_le_bytes([body[0], body[1]]);
                let channels = u16::from_le_bytes([body[2], body[3]]);
                let rate = u32::from_le_bytes(body[4..8].try_into().unwrap());
                let bits = u16::from_le_bytes([body[14], body[15]]);
                // WAVE_FORMAT_EXTENSIBLE carries the real format in its sub-format GUID.
                let format = if format == 0xFFFE && body.len() >= 26 {
                    u16::from_le_bytes([body[24], body[25]])
                } else {
                    format
                };
                fmt = Some((format, channels, rate, bits));
            }
            b"data" => data = Some(body),
            _ => {}
        }
        p += 8 + len + (len & 1);
    }
    let (format, channels, rate, bits) = fmt.ok_or_else(|| bad("missing fmt chunk"))?;
    let data = data.ok_or_else(|| bad("missing data chunk"))?;
    if format != 1 {
        return Err(bad("only uncompressed PCM WAV files are supported"));
    }
    if !(1..=2).contains(&channels) {
        return Err(bad("only mono or stereo WAV files are supported"));
    }
    if !(8000..=48000).contains(&rate) {
        return Err(bad("sample rate must be between 8000 and 48000 Hz"));
    }
    let bytes = (bits / 8) as usize;
    if bytes != 1 && bytes != 2 {
        return Err(bad("only 8-bit or 16-bit WAV files are supported"));
    }
    let frame = bytes * channels as usize;
    let max_frames = (rate * MAX_SOUND_SECONDS) as usize;
    let frames = (data.len() / frame).min(max_frames);
    let sample = |i: usize| -> i16 {
        if bytes == 2 {
            i16::from_le_bytes([data[i], data[i + 1]])
        } else {
            ((data[i] as i16) - 128) << 8 // 8-bit WAV is unsigned
        }
    };
    let mut left = Vec::with_capacity(frames);
    let mut right = Vec::with_capacity(frames);
    for f in 0..frames {
        let base = f * frame;
        let l = sample(base);
        let r = if channels == 2 {
            sample(base + bytes)
        } else {
            l
        };
        left.push(l);
        right.push(r);
    }
    Ok(Pcm { rate, left, right })
}

fn encode_cwav(pcm: &Pcm) -> Vec<u8> {
    // Pad to a multiple of 16 frames so each channel is 0x20 aligned.
    let frames = pcm.left.len().max(16).div_ceil(16) * 16;
    let ch_bytes = frames * 2;
    let pcm_bytes = ch_bytes * 2;
    let info_size = 0x60u32;
    let data_size = 0x20 + pcm_bytes as u32;
    let file_size = 0x40 + info_size + data_size;

    let mut o = Vec::with_capacity(file_size as usize);
    let p16 = |o: &mut Vec<u8>, v: u16| o.extend_from_slice(&v.to_le_bytes());
    let p32 = |o: &mut Vec<u8>, v: u32| o.extend_from_slice(&v.to_le_bytes());
    o.extend_from_slice(b"CWAV");
    p16(&mut o, 0xFEFF);
    p16(&mut o, 0x40);
    p32(&mut o, 0x0201_0000);
    p32(&mut o, file_size);
    p16(&mut o, 2);
    p16(&mut o, 0);
    for (kind, off, size) in [
        (0x7000u16, 0x40u32, info_size),
        (0x7001, 0x40 + info_size, data_size),
    ] {
        p16(&mut o, kind);
        p16(&mut o, 0);
        p32(&mut o, off);
        p32(&mut o, size);
    }
    o.resize(0x40, 0);

    // INFO
    o.extend_from_slice(b"INFO");
    p32(&mut o, info_size);
    o.push(1); // PCM16
    o.push(0); // no loop
    p16(&mut o, 0);
    p32(&mut o, pcm.rate);
    p32(&mut o, 0); // loop start
    p32(&mut o, frames as u32); // loop end
    p32(&mut o, 0);
    p32(&mut o, 2); // channel count
    for c in 0..2u32 {
        p16(&mut o, 0x7100);
        p16(&mut o, 0);
        p32(&mut o, 4 + 2 * 8 + c * 0x14);
    }
    for c in 0..2u32 {
        p16(&mut o, 0x1F00);
        p16(&mut o, 0);
        p32(&mut o, 0x18 + c * ch_bytes as u32);
        p16(&mut o, 0);
        p16(&mut o, 0);
        p32(&mut o, 0xFFFF_FFFF);
        p32(&mut o, 0);
    }
    o.resize(0x40 + info_size as usize, 0);

    // DATA
    o.extend_from_slice(b"DATA");
    p32(&mut o, data_size);
    o.resize(0x40 + info_size as usize + 0x20, 0);
    for ch in [&pcm.left, &pcm.right] {
        for i in 0..frames {
            p16(&mut o, *ch.get(i).unwrap_or(&0) as u16);
        }
    }
    debug_assert_eq!(o.len(), file_size as usize);
    o
}

pub fn wav_to_cwav(wav: &[u8]) -> Result<Vec<u8>, Error> {
    Ok(encode_cwav(&parse_wav(wav)?))
}

/// A quarter second of silence: the HOME Menu expects a jingle to be present.
pub fn silent_cwav() -> Vec<u8> {
    let frames = 8192;
    encode_cwav(&Pcm {
        rate: 32728,
        left: vec![0; frames],
        right: vec![0; frames],
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lz11_roundtrip() {
        let mut data = Vec::new();
        for i in 0..70000u32 {
            data.push(((i / 7) % 13) as u8);
            if i % 1000 == 0 {
                data.extend_from_slice(&[0xAA; 300]);
            }
        }
        data.extend(std::iter::repeat_n(0, 0x12000));
        let c = lz11_compress(&data).unwrap();
        assert!(c.len() < data.len() / 4);
        assert_eq!(lz11_decompress(&c).unwrap(), data);
    }

    #[test]
    fn banner_layout() {
        let img = Rgba::filled(WIDTH, HEIGHT, [10, 20, 30, 255]);
        let b = build(&img, None).unwrap();
        assert_eq!(&b[0..4], b"CBMD");
        let cwav_off = u32::from_le_bytes(b[0x84..0x88].try_into().unwrap()) as usize;
        assert_eq!(cwav_off % 0x10, 0);
        assert_eq!(&b[cwav_off..cwav_off + 4], b"CWAV");
        let cgfx = lz11_decompress(&b[0x88..cwav_off]).unwrap();
        assert_eq!(cgfx.len(), 0x11580);
        assert_eq!(&cgfx[0..4], b"CGFX");
        assert_eq!(
            u32::from_le_bytes(cgfx[0xC..0x10].try_into().unwrap()),
            0x11580
        );
    }

    #[test]
    fn wav_conversion() {
        let mut wav = Vec::new();
        let samples: Vec<i16> = (0..1000).map(|i| (i * 30) as i16).collect();
        let data_len = samples.len() as u32 * 2;
        wav.extend_from_slice(b"RIFF");
        wav.extend_from_slice(&(36 + data_len).to_le_bytes());
        wav.extend_from_slice(b"WAVEfmt ");
        wav.extend_from_slice(&16u32.to_le_bytes());
        wav.extend_from_slice(&1u16.to_le_bytes());
        wav.extend_from_slice(&1u16.to_le_bytes());
        wav.extend_from_slice(&22050u32.to_le_bytes());
        wav.extend_from_slice(&44100u32.to_le_bytes());
        wav.extend_from_slice(&2u16.to_le_bytes());
        wav.extend_from_slice(&16u16.to_le_bytes());
        wav.extend_from_slice(b"data");
        wav.extend_from_slice(&data_len.to_le_bytes());
        for s in &samples {
            wav.extend_from_slice(&s.to_le_bytes());
        }
        let c = wav_to_cwav(&wav).unwrap();
        assert_eq!(&c[0..4], b"CWAV");
        assert_eq!(
            u32::from_le_bytes(c[0xC..0x10].try_into().unwrap()) as usize,
            c.len()
        );
        assert_eq!(u32::from_le_bytes(c[0x4C..0x50].try_into().unwrap()), 22050);
    }
}
