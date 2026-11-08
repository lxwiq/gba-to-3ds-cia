//! ROM patch formats: IPS, UPS and BPS.
//!
//! ROM hacks such as Pokemon Unbound are distributed as patches that must be
//! applied to a clean dump of the original game. All three common formats are
//! supported. UPS and BPS carry CRC32 checksums of the expected source, the
//! produced target and the patch itself; they are always verified.

use crate::Error;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PatchFormat {
    Ips,
    Ups,
    Bps,
}

impl PatchFormat {
    pub fn detect(patch: &[u8]) -> Option<Self> {
        if patch.starts_with(b"PATCH") {
            Some(Self::Ips)
        } else if patch.starts_with(b"UPS1") {
            Some(Self::Ups)
        } else if patch.starts_with(b"BPS1") {
            Some(Self::Bps)
        } else {
            None
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Ips => "IPS",
            Self::Ups => "UPS",
            Self::Bps => "BPS",
        }
    }
}

/// Information a checksummed patch declares about its expected input.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PatchInfo {
    pub format: PatchFormat,
    pub source_size: Option<u64>,
    pub source_crc32: Option<u32>,
    pub target_size: Option<u64>,
    pub target_crc32: Option<u32>,
}

pub fn crc32(data: &[u8]) -> u32 {
    let mut h = crc32fast::Hasher::new();
    h.update(data);
    h.finalize()
}

/// Read the header and footer of a patch without applying it.
pub fn inspect(patch: &[u8]) -> Result<PatchInfo, Error> {
    let format = PatchFormat::detect(patch).ok_or(Error::Patch("unknown patch format".into()))?;
    match format {
        PatchFormat::Ips => Ok(PatchInfo {
            format,
            source_size: None,
            source_crc32: None,
            target_size: None,
            target_crc32: None,
        }),
        PatchFormat::Ups | PatchFormat::Bps => {
            if patch.len() < 4 + 12 {
                return Err(Error::Patch("patch is truncated".into()));
            }
            let mut r = Reader::new(&patch[..patch.len() - 12], 4);
            let source_size = r.varint()?;
            let target_size = r.varint()?;
            let foot = &patch[patch.len() - 12..];
            Ok(PatchInfo {
                format,
                source_size: Some(source_size),
                source_crc32: Some(le32(&foot[0..4])),
                target_size: Some(target_size),
                target_crc32: Some(le32(&foot[4..8])),
            })
        }
    }
}

/// Apply a patch (format detected from its magic) to `source`.
pub fn apply(source: &[u8], patch: &[u8]) -> Result<Vec<u8>, Error> {
    match PatchFormat::detect(patch) {
        Some(PatchFormat::Ips) => apply_ips(source, patch),
        Some(PatchFormat::Ups) => apply_ups(source, patch),
        Some(PatchFormat::Bps) => apply_bps(source, patch),
        None => Err(Error::Patch(
            "unknown patch format (expected an IPS, UPS or BPS file)".into(),
        )),
    }
}

fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

struct Reader<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn new(data: &'a [u8], pos: usize) -> Self {
        Self { data, pos }
    }

    fn eof(&self) -> bool {
        self.pos >= self.data.len()
    }

    fn byte(&mut self) -> Result<u8, Error> {
        let b = *self
            .data
            .get(self.pos)
            .ok_or_else(|| Error::Patch("patch is truncated".into()))?;
        self.pos += 1;
        Ok(b)
    }

    fn bytes(&mut self, n: usize) -> Result<&'a [u8], Error> {
        let end = self
            .pos
            .checked_add(n)
            .filter(|&e| e <= self.data.len())
            .ok_or_else(|| Error::Patch("patch is truncated".into()))?;
        let s = &self.data[self.pos..end];
        self.pos = end;
        Ok(s)
    }

    /// Variable-length integer shared by UPS and BPS.
    fn varint(&mut self) -> Result<u64, Error> {
        let mut data: u64 = 0;
        let mut shift: u64 = 1;
        loop {
            let x = self.byte()? as u64;
            data = data
                .checked_add((x & 0x7f).checked_mul(shift).ok_or_else(overflow)?)
                .ok_or_else(overflow)?;
            if x & 0x80 != 0 {
                return Ok(data);
            }
            shift = shift.checked_shl(7).ok_or_else(overflow)?;
            data = data.checked_add(shift).ok_or_else(overflow)?;
        }
    }
}

fn overflow() -> Error {
    Error::Patch("malformed variable-length integer".into())
}

fn to_usize(v: u64) -> Result<usize, Error> {
    usize::try_from(v).map_err(|_| Error::Patch("value too large".into()))
}

/// Upper bound for patched output, far above any GBA ROM.
const MAX_OUTPUT: u64 = 256 * 1024 * 1024;

fn apply_ips(source: &[u8], patch: &[u8]) -> Result<Vec<u8>, Error> {
    let mut out = source.to_vec();
    let mut r = Reader::new(patch, 5);
    loop {
        let off = r.bytes(3)?;
        if off == b"EOF" {
            // Optional 3-byte truncation size (Lunar IPS extension).
            if r.data.len() - r.pos >= 3 {
                let t = r.bytes(3)?;
                let size = (t[0] as usize) << 16 | (t[1] as usize) << 8 | t[2] as usize;
                out.truncate(size);
            }
            return Ok(out);
        }
        let offset = (off[0] as usize) << 16 | (off[1] as usize) << 8 | off[2] as usize;
        let s = r.bytes(2)?;
        let size = (s[0] as usize) << 8 | s[1] as usize;
        if size == 0 {
            let c = r.bytes(2)?;
            let count = (c[0] as usize) << 8 | c[1] as usize;
            let value = r.byte()?;
            if out.len() < offset + count {
                out.resize(offset + count, 0);
            }
            out[offset..offset + count].fill(value);
        } else {
            let data = r.bytes(size)?;
            if out.len() < offset + size {
                out.resize(offset + size, 0);
            }
            out[offset..offset + size].copy_from_slice(data);
        }
        if r.eof() {
            return Err(Error::Patch("IPS patch has no EOF marker".into()));
        }
    }
}

fn check_footer(source: &[u8], patch: &[u8], kind: &str) -> Result<(u32, u32), Error> {
    if patch.len() < 4 + 12 {
        return Err(Error::Patch(format!("{kind} patch is truncated")));
    }
    let foot = &patch[patch.len() - 12..];
    let src_crc = le32(&foot[0..4]);
    let dst_crc = le32(&foot[4..8]);
    let patch_crc = le32(&foot[8..12]);
    if crc32(&patch[..patch.len() - 4]) != patch_crc {
        return Err(Error::Patch(format!(
            "{kind} patch file is corrupted (bad checksum)"
        )));
    }
    let actual = crc32(source);
    if actual != src_crc {
        return Err(Error::PatchSourceMismatch {
            expected: src_crc,
            actual,
        });
    }
    Ok((src_crc, dst_crc))
}

fn apply_ups(source: &[u8], patch: &[u8]) -> Result<Vec<u8>, Error> {
    let (_, dst_crc) = check_footer(source, patch, "UPS")?;
    let body = &patch[..patch.len() - 12];
    let mut r = Reader::new(body, 4);
    let src_size = r.varint()?;
    let dst_size = r.varint()?;
    if src_size != source.len() as u64 {
        return Err(Error::Patch(format!(
            "UPS patch expects a {src_size}-byte source, got {} bytes",
            source.len()
        )));
    }
    if dst_size > MAX_OUTPUT {
        return Err(Error::Patch("UPS target size is unreasonably large".into()));
    }
    let dst_size = to_usize(dst_size)?;
    let mut out = vec![0u8; dst_size];
    let n = source.len().min(dst_size);
    out[..n].copy_from_slice(&source[..n]);
    let mut pos: usize = 0;
    while !r.eof() {
        pos = pos
            .checked_add(to_usize(r.varint()?)?)
            .ok_or_else(overflow)?;
        loop {
            let x = r.byte()?;
            if pos < dst_size {
                out[pos] ^= x;
            }
            pos += 1;
            if x == 0 {
                break;
            }
        }
    }
    if crc32(&out) != dst_crc {
        return Err(Error::Patch("UPS output checksum mismatch".into()));
    }
    Ok(out)
}

fn apply_bps(source: &[u8], patch: &[u8]) -> Result<Vec<u8>, Error> {
    let (_, dst_crc) = check_footer(source, patch, "BPS")?;
    let body = &patch[..patch.len() - 12];
    let mut r = Reader::new(body, 4);
    let src_size = r.varint()?;
    let dst_size = r.varint()?;
    let meta_size = to_usize(r.varint()?)?;
    r.bytes(meta_size)?;
    if src_size != source.len() as u64 {
        return Err(Error::Patch(format!(
            "BPS patch expects a {src_size}-byte source, got {} bytes",
            source.len()
        )));
    }
    if dst_size > MAX_OUTPUT {
        return Err(Error::Patch("BPS target size is unreasonably large".into()));
    }
    let dst_size = to_usize(dst_size)?;
    let mut out = vec![0u8; dst_size];
    let mut out_pos: usize = 0;
    let mut src_rel: i64 = 0;
    let mut dst_rel: i64 = 0;
    let bad = || Error::Patch("BPS patch references data out of range".into());

    while !r.eof() {
        let data = r.varint()?;
        let command = data & 3;
        let length = to_usize((data >> 2) + 1)?;
        if out_pos.checked_add(length).is_none_or(|e| e > dst_size) {
            return Err(bad());
        }
        match command {
            0 => {
                // SourceRead
                let end = out_pos + length;
                if end > source.len() {
                    return Err(bad());
                }
                out[out_pos..end].copy_from_slice(&source[out_pos..end]);
                out_pos = end;
            }
            1 => {
                // TargetRead
                let bytes = r.bytes(length)?;
                out[out_pos..out_pos + length].copy_from_slice(bytes);
                out_pos += length;
            }
            2 | 3 => {
                let d = r.varint()?;
                let delta = (d >> 1) as i64;
                let delta = if d & 1 != 0 { -delta } else { delta };
                if command == 2 {
                    // SourceCopy
                    src_rel += delta;
                    if src_rel < 0 || (src_rel as usize) + length > source.len() {
                        return Err(bad());
                    }
                    let s = src_rel as usize;
                    out[out_pos..out_pos + length].copy_from_slice(&source[s..s + length]);
                    src_rel += length as i64;
                    out_pos += length;
                } else {
                    // TargetCopy: byte by byte, the ranges may overlap.
                    dst_rel += delta;
                    if dst_rel < 0 || dst_rel as usize >= out_pos {
                        return Err(bad());
                    }
                    for _ in 0..length {
                        out[out_pos] = out[dst_rel as usize];
                        out_pos += 1;
                        dst_rel += 1;
                    }
                }
            }
            _ => unreachable!(),
        }
    }
    if crc32(&out) != dst_crc {
        return Err(Error::Patch("BPS output checksum mismatch".into()));
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn varint(mut v: u64, out: &mut Vec<u8>) {
        loop {
            let x = (v & 0x7f) as u8;
            v >>= 7;
            if v == 0 {
                out.push(0x80 | x);
                break;
            }
            out.push(x);
            v -= 1;
        }
    }

    fn finish(mut p: Vec<u8>, src: &[u8], dst: &[u8]) -> Vec<u8> {
        p.extend_from_slice(&crc32(src).to_le_bytes());
        p.extend_from_slice(&crc32(dst).to_le_bytes());
        let c = crc32(&p);
        p.extend_from_slice(&c.to_le_bytes());
        p
    }

    #[test]
    fn varint_roundtrip() {
        for v in [
            0u64,
            1,
            127,
            128,
            129,
            16383,
            16384,
            1 << 24,
            32 * 1024 * 1024,
        ] {
            let mut b = Vec::new();
            varint(v, &mut b);
            assert_eq!(Reader::new(&b, 0).varint().unwrap(), v);
        }
    }

    #[test]
    fn bps_all_commands() {
        let src = b"Hello, World! This is the source.".to_vec();
        let dst = b"Hello, Brave World! World! source.".to_vec();
        let mut p = b"BPS1".to_vec();
        varint(src.len() as u64, &mut p);
        varint(dst.len() as u64, &mut p);
        varint(0, &mut p);
        // SourceRead "Hello, " (7)
        varint(((7 - 1) << 2) | 0, &mut p);
        // TargetRead "Brave " (6)
        varint(((6 - 1) << 2) | 1, &mut p);
        p.extend_from_slice(b"Brave ");
        // SourceCopy "World! " from source offset 7 (7 bytes)
        varint(((7 - 1) << 2) | 2, &mut p);
        varint(7 << 1, &mut p);
        // TargetCopy "World! " from target offset 13 (7 bytes): rel 0 -> +13
        varint(((7 - 1) << 2) | 3, &mut p);
        varint(13 << 1, &mut p);
        // SourceCopy "source." from source offset 26: src_rel is 14 -> +12
        varint(((7 - 1) << 2) | 2, &mut p);
        varint(12 << 1, &mut p);
        let p = finish(p, &src, &dst);
        assert_eq!(apply(&src, &p).unwrap(), dst);
        assert!(matches!(
            apply(b"wrong source", &p),
            Err(Error::PatchSourceMismatch { .. })
        ));
    }

    #[test]
    fn ups_basic() {
        let src = b"abcdefgh".to_vec();
        let dst = b"abXdefghZZ".to_vec();
        let mut p = b"UPS1".to_vec();
        varint(src.len() as u64, &mut p);
        varint(dst.len() as u64, &mut p);
        varint(2, &mut p);
        p.push(b'c' ^ b'X');
        p.push(0);
        // after the terminator pos is 4; skip to 8
        varint(4, &mut p);
        p.push(b'Z');
        p.push(b'Z');
        p.push(0);
        let p = finish(p, &src, &dst);
        assert_eq!(apply(&src, &p).unwrap(), dst);
    }

    #[test]
    fn ips_basic_and_rle() {
        let src = vec![0u8; 16];
        let mut p = b"PATCH".to_vec();
        p.extend_from_slice(&[0, 0, 2, 0, 3, 1, 2, 3]);
        p.extend_from_slice(&[0, 0, 18, 0, 0, 0, 4, 0xAA]);
        p.extend_from_slice(b"EOF");
        let out = apply(&src, &p).unwrap();
        assert_eq!(&out[2..5], &[1, 2, 3]);
        assert_eq!(out.len(), 22);
        assert_eq!(&out[18..22], &[0xAA; 4]);
    }
}
