//! NCCH (CXI) assembly: header, extended header, ExeFS and RomFS.
//!
//! The content is stored unencrypted with the NoCrypto flag set, which is what
//! every homebrew injector produces and what custom firmware accepts.

use crate::Error;
use sha2::{Digest, Sha256};

pub const MEDIA_UNIT: usize = 0x200;

pub fn sha256(data: &[u8]) -> [u8; 32] {
    Sha256::digest(data).into()
}

pub fn align(v: usize, a: usize) -> usize {
    v.div_ceil(a) * a
}

/// Build an ExeFS image from up to ten named files, in the given order.
pub fn build_exefs(files: &[(&str, &[u8])]) -> Result<Vec<u8>, Error> {
    if files.len() > 10 {
        return Err(Error::Build("ExeFS holds at most 10 files".into()));
    }
    let mut header = vec![0u8; 0x200];
    let mut data = Vec::new();
    for (i, (name, content)) in files.iter().enumerate() {
        let name = name.as_bytes();
        if name.len() > 8 {
            return Err(Error::Build(format!(
                "ExeFS file name too long: {}",
                String::from_utf8_lossy(name)
            )));
        }
        let entry = &mut header[i * 16..i * 16 + 16];
        entry[..name.len()].copy_from_slice(name);
        entry[8..12].copy_from_slice(&(data.len() as u32).to_le_bytes());
        entry[12..16].copy_from_slice(&(content.len() as u32).to_le_bytes());
        // File hashes are stored in reverse order, slot 9 for the first file.
        let h = 0xC0 + (9 - i) * 0x20;
        header[h..h + 32].copy_from_slice(&sha256(content));
        data.extend_from_slice(content);
        data.resize(align(data.len(), MEDIA_UNIT), 0);
    }
    header.extend_from_slice(&data);
    Ok(header)
}

/// List the files of an ExeFS image.
pub fn parse_exefs(exefs: &[u8]) -> Result<Vec<(String, Vec<u8>)>, Error> {
    if exefs.len() < 0x200 {
        return Err(Error::Build("ExeFS is truncated".into()));
    }
    let mut out = Vec::new();
    for i in 0..10 {
        let e = &exefs[i * 16..i * 16 + 16];
        if e[0] == 0 {
            continue;
        }
        let name = String::from_utf8_lossy(&e[..8])
            .trim_end_matches('\0')
            .to_string();
        let off = u32::from_le_bytes(e[8..12].try_into().unwrap()) as usize + 0x200;
        let size = u32::from_le_bytes(e[12..16].try_into().unwrap()) as usize;
        let data = exefs
            .get(off..off + size)
            .ok_or_else(|| Error::Build("ExeFS entry out of range".into()))?;
        out.push((name, data.to_vec()));
    }
    Ok(out)
}

pub struct NcchInput<'a> {
    /// 0x200-byte header used as a base: signature and fixed fields are kept,
    /// everything describing the layout is recomputed.
    pub header_base: &'a [u8],
    /// 0x800-byte extended header (SCI + ACI + signed access descriptor).
    pub exheader: &'a [u8],
    pub exefs: &'a [u8],
    pub romfs: Option<&'a [u8]>,
    pub title_id: u64,
    pub product_code: &'a str,
    pub maker_code: [u8; 2],
}

pub fn build_ncch(inp: &NcchInput) -> Result<Vec<u8>, Error> {
    if inp.header_base.len() != 0x200 {
        return Err(Error::Build("NCCH header base must be 0x200 bytes".into()));
    }
    if inp.exheader.len() != 0x800 {
        return Err(Error::Build("extended header must be 0x800 bytes".into()));
    }
    let mut hdr = inp.header_base.to_vec();
    hdr[0x100..0x104].copy_from_slice(b"NCCH");

    let exefs_off = 0x200 + 0x800;
    let exefs_len = align(inp.exefs.len(), MEDIA_UNIT);
    let mut total = exefs_off + exefs_len;

    let mut romfs_off = 0;
    let mut romfs_len = 0;
    let mut romfs_hashed = 0;
    if let Some(romfs) = inp.romfs {
        romfs_off = align(total, 0x1000);
        romfs_len = align(romfs.len(), MEDIA_UNIT);
        romfs_hashed = romfs_superblock_size(romfs)?;
        total = romfs_off + romfs_len;
    }

    let mut out = vec![0u8; total];
    out[0x200..0xA00].copy_from_slice(inp.exheader);
    out[exefs_off..exefs_off + inp.exefs.len()].copy_from_slice(inp.exefs);
    if let Some(romfs) = inp.romfs {
        out[romfs_off..romfs_off + romfs.len()].copy_from_slice(romfs);
    }

    let w32 = |h: &mut Vec<u8>, at: usize, v: usize| {
        h[at..at + 4].copy_from_slice(&((v / MEDIA_UNIT) as u32).to_le_bytes());
    };
    w32(&mut hdr, 0x104, total);
    hdr[0x108..0x110].copy_from_slice(&inp.title_id.to_le_bytes());
    hdr[0x110..0x112].copy_from_slice(&inp.maker_code);
    hdr[0x112..0x114].copy_from_slice(&2u16.to_le_bytes());
    hdr[0x114..0x118].fill(0);
    hdr[0x118..0x120].copy_from_slice(&inp.title_id.to_le_bytes());
    hdr[0x130..0x150].fill(0); // no logo region
    let mut pc = [0u8; 16];
    let code = inp.product_code.as_bytes();
    pc[..code.len().min(16)].copy_from_slice(&code[..code.len().min(16)]);
    hdr[0x150..0x160].copy_from_slice(&pc);
    hdr[0x160..0x180].copy_from_slice(&sha256(&inp.exheader[..0x400]));
    hdr[0x180..0x184].copy_from_slice(&0x400u32.to_le_bytes());
    hdr[0x184..0x188].fill(0);
    // Flags: crypto method 0, platform CTR, content type Data|Executable,
    // NoCrypto.
    hdr[0x18B] = 0;
    hdr[0x18C] = 1;
    hdr[0x18D] = 0x03;
    hdr[0x18E] = 0;
    hdr[0x18F] = 0x04;
    hdr[0x190..0x1A0].fill(0); // no plain region, no logo region
    w32(&mut hdr, 0x1A0, exefs_off);
    w32(&mut hdr, 0x1A4, exefs_len);
    w32(&mut hdr, 0x1A8, MEDIA_UNIT);
    hdr[0x1AC..0x1B0].fill(0);
    w32(&mut hdr, 0x1B0, romfs_off);
    w32(&mut hdr, 0x1B4, romfs_len);
    w32(&mut hdr, 0x1B8, romfs_hashed);
    hdr[0x1BC..0x1C0].fill(0);
    hdr[0x1C0..0x1E0].copy_from_slice(&sha256(&out[exefs_off..exefs_off + MEDIA_UNIT]));
    if romfs_len > 0 {
        hdr[0x1E0..0x200].copy_from_slice(&sha256(&out[romfs_off..romfs_off + romfs_hashed]));
    } else {
        hdr[0x1E0..0x200].fill(0);
    }
    out[..0x200].copy_from_slice(&hdr);
    Ok(out)
}

/// Size of the RomFS region covered by the NCCH superblock hash: the IVFC
/// header plus the master hash, rounded up to a media unit.
fn romfs_superblock_size(romfs: &[u8]) -> Result<usize, Error> {
    if romfs.len() < 0x60 || &romfs[0..4] != b"IVFC" {
        return Err(Error::Build(
            "RomFS does not start with an IVFC header".into(),
        ));
    }
    let master_hash_size = u32::from_le_bytes(romfs[8..12].try_into().unwrap()) as usize;
    Ok(align(0x60 + master_hash_size, MEDIA_UNIT))
}

/// Read back the main fields of an NCCH, used by tests and by `inspect`.
#[derive(Debug)]
pub struct NcchView<'a> {
    pub title_id: u64,
    pub exheader: &'a [u8],
    pub exefs: &'a [u8],
    pub romfs: Option<&'a [u8]>,
}

pub fn parse_ncch(ncch: &[u8]) -> Result<NcchView<'_>, Error> {
    if ncch.len() < 0x200 || &ncch[0x100..0x104] != b"NCCH" {
        return Err(Error::Build("not an NCCH".into()));
    }
    let r32 =
        |at: usize| u32::from_le_bytes(ncch[at..at + 4].try_into().unwrap()) as usize * MEDIA_UNIT;
    let exh_size = u32::from_le_bytes(ncch[0x180..0x184].try_into().unwrap()) as usize;
    let exefs_off = r32(0x1A0);
    let exefs_len = r32(0x1A4);
    let romfs_off = r32(0x1B0);
    let romfs_len = r32(0x1B4);
    let get = |o: usize, l: usize| {
        ncch.get(o..o + l)
            .ok_or_else(|| Error::Build("NCCH region out of range".into()))
    };
    // Verify hashes, as the 3DS does at install and launch.
    if sha256(get(0x200, exh_size)?) != ncch[0x160..0x180] {
        return Err(Error::Build("exheader hash mismatch".into()));
    }
    if sha256(get(exefs_off, r32(0x1A8))?) != ncch[0x1C0..0x1E0] {
        return Err(Error::Build("ExeFS hash mismatch".into()));
    }
    if romfs_len > 0 && sha256(get(romfs_off, r32(0x1B8))?) != ncch[0x1E0..0x200] {
        return Err(Error::Build("RomFS hash mismatch".into()));
    }
    Ok(NcchView {
        title_id: u64::from_le_bytes(ncch[0x118..0x120].try_into().unwrap()),
        exheader: get(0x200, 0x800)?,
        exefs: get(exefs_off, exefs_len)?,
        romfs: if romfs_len > 0 {
            Some(get(romfs_off, romfs_len)?)
        } else {
            None
        },
    })
}
