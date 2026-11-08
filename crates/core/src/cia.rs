//! CIA container: header, certificate chain, ticket, TMD, content, meta.
//!
//! The layout matches what makerom produces with `-f cia -ignoresign`:
//! content is stored unencrypted and the ticket/TMD signatures are filled
//! with 0xFF. Custom firmware (Luma3DS) accepts such titles; the stock
//! firmware does not, which is expected for any homebrew title.

use crate::ncch::{align, sha256};

/// Public certificates CA00000003, XS0000000c and CP0000000b (retail).
const CERT_CHAIN: &[u8] = include_bytes!("../../../assets/cert_chain_retail.bin");

const SIG_RSA2048_SHA256: u32 = 0x0001_0004;
const TICKET_ISSUER: &str = "Root-CA00000003-XS0000000c";
const TMD_ISSUER: &str = "Root-CA00000003-CP0000000b";

pub struct CiaInput<'a> {
    pub title_id: u64,
    /// Title version (major << 10 | minor << 4 | micro).
    pub title_version: u16,
    /// Content 0, a complete NCCH (CXI).
    pub content: &'a [u8],
    /// Exheader save data size, copied into the TMD.
    pub save_data_size: u32,
    /// Optional meta region: exheader dependency list, core version and SMDH.
    pub meta: Option<Meta<'a>>,
    /// Random-looking 16 bytes for the (unused) title key and ticket ID.
    pub entropy: [u8; 16],
}

pub struct Meta<'a> {
    pub dependencies: &'a [u8], // 0x180 bytes from the exheader
    pub core_version: u32,
    pub smdh: &'a [u8], // 0x36C0 bytes
}

fn signature_block(buf: &mut Vec<u8>) {
    buf.extend_from_slice(&SIG_RSA2048_SHA256.to_be_bytes());
    buf.extend(std::iter::repeat_n(0xFF, 0x100));
    buf.extend(std::iter::repeat_n(0x00, 0x3C));
}

fn issuer(buf: &mut Vec<u8>, s: &str) {
    let mut b = [0u8; 0x40];
    b[..s.len()].copy_from_slice(s.as_bytes());
    buf.extend_from_slice(&b);
}

fn build_ticket(inp: &CiaInput) -> Vec<u8> {
    let mut t = Vec::with_capacity(0x350);
    signature_block(&mut t);
    issuer(&mut t, TICKET_ISSUER);
    t.extend_from_slice(&[0u8; 0x3C]); // ECC public key
    t.push(1); // format version
    t.push(0); // CA CRL version
    t.push(0); // signer CRL version
    t.extend_from_slice(&inp.entropy); // "encrypted" title key, unused (content is plaintext)
    t.push(0);
    let ticket_id = 0x0004_0000_0000_0000u64
        | (u64::from_le_bytes(inp.entropy[..8].try_into().unwrap()) & 0x0000_FFFF_FFFF_FFFF);
    t.extend_from_slice(&ticket_id.to_be_bytes());
    t.extend_from_slice(&0u32.to_be_bytes()); // console ID
    t.extend_from_slice(&inp.title_id.to_be_bytes());
    t.extend_from_slice(&[0u8; 2]);
    t.extend_from_slice(&inp.title_version.to_be_bytes());
    t.extend_from_slice(&[0u8; 8]);
    t.push(0); // license type: permanent
    t.push(0); // common key index
    t.extend_from_slice(&[0u8; 2]); // property mask
    t.extend_from_slice(&[0u8; 0x14]); // custom data
    t.extend_from_slice(&[0u8; 0x14]);
    t.extend_from_slice(&0u32.to_be_bytes()); // eShop account ID
    t.push(0);
    t.push(0); // audit
    t.extend_from_slice(&[0u8; 0x42]);
    t.extend_from_slice(&[0u8; 0x40]); // limits
    // Content index: one segment covering content index 0.
    let seg_size = 0x84u32;
    let hdr_size = 0x28u32;
    for v in [
        0x0001_0014u32,
        hdr_size + seg_size,
        0x0000_0014,
        0x0001_0014,
        0,
        hdr_size,
        1,
        seg_size,
        seg_size,
        0x0003_0000,
    ] {
        t.extend_from_slice(&v.to_be_bytes());
    }
    t.extend_from_slice(&0u32.to_be_bytes()); // level 0
    let mut bits = [0u8; 0x80];
    bits[0] = 0x01; // index 0
    t.extend_from_slice(&bits);
    debug_assert_eq!(t.len(), 0x350);
    t
}

fn build_tmd(inp: &CiaInput, content_size: u64, content_hash: [u8; 32]) -> Vec<u8> {
    // Content chunk record.
    let mut chunk = Vec::with_capacity(0x30);
    chunk.extend_from_slice(&0u32.to_be_bytes()); // content ID
    chunk.extend_from_slice(&0u16.to_be_bytes()); // index
    chunk.extend_from_slice(&0u16.to_be_bytes()); // type: not encrypted
    chunk.extend_from_slice(&content_size.to_be_bytes());
    chunk.extend_from_slice(&content_hash);

    let mut info = vec![0u8; 0x24 * 64];
    info[0..2].copy_from_slice(&0u16.to_be_bytes());
    info[2..4].copy_from_slice(&1u16.to_be_bytes());
    info[4..0x24].copy_from_slice(&sha256(&chunk));

    let mut t = Vec::with_capacity(0xB34);
    signature_block(&mut t);
    issuer(&mut t, TMD_ISSUER);
    t.push(1); // format version
    t.push(0);
    t.push(0);
    t.push(0);
    t.extend_from_slice(&0u64.to_be_bytes()); // system version
    t.extend_from_slice(&inp.title_id.to_be_bytes());
    t.extend_from_slice(&0x40u32.to_be_bytes()); // title type: CTR
    t.extend_from_slice(&0u16.to_be_bytes()); // group ID
    t.extend_from_slice(&inp.save_data_size.to_le_bytes());
    t.extend_from_slice(&0u32.to_le_bytes()); // private save data size
    t.extend_from_slice(&[0u8; 4]);
    t.push(0); // TWL flag
    t.extend_from_slice(&[0u8; 0x31]);
    t.extend_from_slice(&0u32.to_be_bytes()); // access rights
    t.extend_from_slice(&inp.title_version.to_be_bytes());
    t.extend_from_slice(&1u16.to_be_bytes()); // content count
    t.extend_from_slice(&0u16.to_be_bytes()); // boot content
    t.extend_from_slice(&[0u8; 2]);
    t.extend_from_slice(&sha256(&info));
    t.extend_from_slice(&info);
    t.extend_from_slice(&chunk);
    debug_assert_eq!(t.len(), 0xB34);
    t
}

fn build_meta(m: &Meta) -> Vec<u8> {
    let mut out = vec![0u8; 0x3AC0];
    let n = m.dependencies.len().min(0x180);
    out[..n].copy_from_slice(&m.dependencies[..n]);
    out[0x300..0x304].copy_from_slice(&m.core_version.to_le_bytes());
    out[0x400..0x400 + m.smdh.len().min(0x36C0)]
        .copy_from_slice(&m.smdh[..m.smdh.len().min(0x36C0)]);
    out
}

pub fn build(inp: &CiaInput) -> Vec<u8> {
    let content_size = align(inp.content.len(), 0x10);
    let mut content = inp.content.to_vec();
    content.resize(content_size, 0);
    let content_hash = sha256(&content);

    let ticket = build_ticket(inp);
    let tmd = build_tmd(inp, content_size as u64, content_hash);
    let meta = inp.meta.as_ref().map(build_meta);

    let mut hdr = vec![0u8; 0x2020];
    hdr[0..4].copy_from_slice(&0x2020u32.to_le_bytes());
    hdr[8..12].copy_from_slice(&(CERT_CHAIN.len() as u32).to_le_bytes());
    hdr[12..16].copy_from_slice(&(ticket.len() as u32).to_le_bytes());
    hdr[16..20].copy_from_slice(&(tmd.len() as u32).to_le_bytes());
    hdr[20..24].copy_from_slice(&(meta.as_ref().map_or(0, |m| m.len()) as u32).to_le_bytes());
    hdr[24..32].copy_from_slice(&(content_size as u64).to_le_bytes());
    hdr[0x20] = 0x80; // content index 0 present

    let mut out = hdr;
    let push = |out: &mut Vec<u8>, data: &[u8]| {
        out.resize(align(out.len(), 0x40), 0);
        out.extend_from_slice(data);
    };
    push(&mut out, CERT_CHAIN);
    push(&mut out, &ticket);
    push(&mut out, &tmd);
    push(&mut out, &content);
    if let Some(m) = &meta {
        push(&mut out, m);
    }
    out.resize(align(out.len(), 0x40), 0);
    out
}

/// Parsed view of a CIA, used to verify our own output.
#[derive(Debug)]
pub struct CiaView<'a> {
    pub title_id: u64,
    pub save_data_size: u32,
    pub content: &'a [u8],
    pub meta: Option<&'a [u8]>,
}

pub fn parse(cia: &[u8]) -> Result<CiaView<'_>, crate::Error> {
    let bad = |m: &str| crate::Error::Build(format!("invalid CIA: {m}"));
    if cia.len() < 0x2020 || u32::from_le_bytes(cia[0..4].try_into().unwrap()) != 0x2020 {
        return Err(bad("header"));
    }
    let r32 = |at: usize| u32::from_le_bytes(cia[at..at + 4].try_into().unwrap()) as usize;
    let cert = r32(8);
    let tik = r32(12);
    let tmd = r32(16);
    let meta = r32(20);
    let csize = u64::from_le_bytes(cia[24..32].try_into().unwrap()) as usize;
    let cert_off = align(0x2020, 0x40);
    let tik_off = align(cert_off + cert, 0x40);
    let tmd_off = align(tik_off + tik, 0x40);
    let content_off = align(tmd_off + tmd, 0x40);
    let meta_off = align(content_off + csize, 0x40);
    let tmd_b = cia
        .get(tmd_off..tmd_off + tmd)
        .ok_or_else(|| bad("TMD out of range"))?;
    let content = cia
        .get(content_off..content_off + csize)
        .ok_or_else(|| bad("content out of range"))?;
    let chunk = &tmd_b[0xB04..0xB34];
    if sha256(content) != chunk[0x10..0x30] {
        return Err(bad("content hash does not match the TMD"));
    }
    if sha256(&tmd_b[0xC4 + 0x140..0xC4 + 0x140 + 0x900]) != tmd_b[0x140 + 0xA4..0x140 + 0xC4] {
        return Err(bad("TMD info record hash"));
    }
    Ok(CiaView {
        title_id: u64::from_be_bytes(tmd_b[0x18C..0x194].try_into().unwrap()),
        save_data_size: u32::from_le_bytes(tmd_b[0x19A..0x19E].try_into().unwrap()),
        content,
        meta: if meta > 0 {
            cia.get(meta_off..meta_off + meta)
        } else {
            None
        },
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let content = vec![0xABu8; 0x1234];
        let smdh = vec![1u8; 0x36C0];
        let deps = vec![0u8; 0x180];
        let cia = build(&CiaInput {
            title_id: 0x0004_0000_00F1_2300,
            title_version: 0,
            content: &content,
            save_data_size: 0x30000,
            meta: Some(Meta {
                dependencies: &deps,
                core_version: 2,
                smdh: &smdh,
            }),
            entropy: [7; 16],
        });
        let v = parse(&cia).unwrap();
        assert_eq!(v.title_id, 0x0004_0000_00F1_2300);
        assert_eq!(v.save_data_size, 0x30000);
        assert_eq!(&v.content[..content.len()], &content[..]);
        assert_eq!(v.meta.unwrap().len(), 0x3AC0);
        assert_eq!(&cia[0x2040 + 0x240..0x2040 + 0x244], b"Root");
    }
}
