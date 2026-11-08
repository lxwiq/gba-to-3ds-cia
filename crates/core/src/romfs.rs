//! RomFS builder (IVFC hash tree over a level-3 file system image).
//!
//! Only a flat or nested set of files is needed here. The layout follows the
//! format the 3DS file system module expects:
//!
//! ```text
//! IVFC header (0x5C) + master hash      (padded to one block)
//! Level 3: RomFS file system            (padded to one block)
//! Level 1: hashes of level 2 blocks     (padded to one block)
//! Level 2: hashes of level 3 blocks     (padded to one block)
//! ```

use crate::ncch::{align, sha256};

const BLOCK_LOG2: u32 = 12;
const BLOCK: usize = 1 << BLOCK_LOG2;
const EMPTY: u32 = 0xFFFF_FFFF;

/// A file to place in the RomFS, by `/`-separated path (no leading slash).
pub struct RomfsFile<'a> {
    pub path: &'a str,
    pub data: &'a [u8],
}

#[derive(Default)]
struct Dir {
    name: Vec<u16>,
    parent: usize,
    dirs: Vec<usize>,
    files: Vec<usize>,
    offset: u32,
}

struct File<'a> {
    name: Vec<u16>,
    parent: usize,
    data: &'a [u8],
    offset: u32,
    data_offset: u64,
}

fn hash_name(parent: u32, name: &[u16]) -> u32 {
    let mut h = parent ^ 123_456_789;
    for &c in name {
        h = h.rotate_right(5);
        h ^= c as u32;
    }
    h
}

fn bucket_count(entries: usize) -> usize {
    if entries < 3 {
        3
    } else if entries < 19 {
        entries | 1
    } else {
        let mut n = entries;
        while [2, 3, 5, 7, 11, 13, 17].iter().any(|&p| n % p == 0) {
            n += 1;
        }
        n
    }
}

fn dir_entry_size(d: &Dir) -> usize {
    0x18 + align(d.name.len() * 2, 4)
}

fn file_entry_size(f: &File) -> usize {
    0x20 + align(f.name.len() * 2, 4)
}

/// Build the level-3 image (the file system itself).
fn build_level3(files: &[RomfsFile]) -> Vec<u8> {
    let mut dirs = vec![Dir::default()];
    let mut fs: Vec<File> = Vec::new();

    let mut sorted: Vec<&RomfsFile> = files.iter().collect();
    sorted.sort_by(|a, b| a.path.cmp(b.path));
    for f in sorted {
        let parts: Vec<&str> = f.path.split('/').filter(|p| !p.is_empty()).collect();
        let mut cur = 0;
        for part in &parts[..parts.len() - 1] {
            let name: Vec<u16> = part.encode_utf16().collect();
            cur = match dirs[cur]
                .dirs
                .iter()
                .copied()
                .find(|&d| dirs[d].name == name)
            {
                Some(d) => d,
                None => {
                    dirs.push(Dir {
                        name,
                        parent: cur,
                        ..Default::default()
                    });
                    let id = dirs.len() - 1;
                    dirs[cur].dirs.push(id);
                    id
                }
            };
        }
        let name: Vec<u16> = parts[parts.len() - 1].encode_utf16().collect();
        fs.push(File {
            name,
            parent: cur,
            data: f.data,
            offset: 0,
            data_offset: 0,
        });
        let id = fs.len() - 1;
        dirs[cur].files.push(id);
    }

    // Assign metadata offsets in breadth-first order (root first).
    let mut order = vec![0usize];
    let mut i = 0;
    while i < order.len() {
        let d = order[i];
        order.extend(dirs[d].dirs.iter().copied());
        i += 1;
    }
    let mut off = 0u32;
    for &d in &order {
        dirs[d].offset = off;
        off += dir_entry_size(&dirs[d]) as u32;
    }
    let dir_meta_len = off as usize;
    let mut off = 0u32;
    let mut data_off = 0u64;
    for &d in &order {
        for &f in &dirs[d].files.clone() {
            fs[f].offset = off;
            off += file_entry_size(&fs[f]) as u32;
            fs[f].data_offset = data_off;
            data_off = align((data_off as usize) + fs[f].data.len(), 0x10) as u64;
        }
    }
    let file_meta_len = off as usize;

    let dir_buckets = bucket_count(dirs.len());
    let file_buckets = bucket_count(fs.len());
    let mut dir_hash = vec![EMPTY; dir_buckets];
    let mut file_hash = vec![EMPTY; file_buckets];
    let mut dir_next = vec![EMPTY; dirs.len()];
    let mut file_next = vec![EMPTY; fs.len()];
    for &d in &order {
        let parent_off = dirs[dirs[d].parent].offset;
        let b = hash_name(parent_off, &dirs[d].name) as usize % dir_buckets;
        dir_next[d] = dir_hash[b];
        dir_hash[b] = dirs[d].offset;
    }
    for f in 0..fs.len() {
        let parent_off = dirs[fs[f].parent].offset;
        let b = hash_name(parent_off, &fs[f].name) as usize % file_buckets;
        file_next[f] = file_hash[b];
        file_hash[b] = fs[f].offset;
    }

    let header_len = 0x28usize;
    let dir_hash_off = header_len;
    let dir_meta_off = dir_hash_off + dir_buckets * 4;
    let file_hash_off = dir_meta_off + dir_meta_len;
    let file_meta_off = file_hash_off + file_buckets * 4;
    let data_start = align(file_meta_off + file_meta_len, 0x10);

    let mut out = Vec::with_capacity(data_start + data_off as usize);
    let p32 = |o: &mut Vec<u8>, v: u32| o.extend_from_slice(&v.to_le_bytes());
    p32(&mut out, header_len as u32);
    p32(&mut out, dir_hash_off as u32);
    p32(&mut out, (dir_buckets * 4) as u32);
    p32(&mut out, dir_meta_off as u32);
    p32(&mut out, dir_meta_len as u32);
    p32(&mut out, file_hash_off as u32);
    p32(&mut out, (file_buckets * 4) as u32);
    p32(&mut out, file_meta_off as u32);
    p32(&mut out, file_meta_len as u32);
    p32(&mut out, data_start as u32);
    for v in &dir_hash {
        p32(&mut out, *v);
    }
    let name_bytes = |o: &mut Vec<u8>, n: &[u16]| {
        for c in n {
            o.extend_from_slice(&c.to_le_bytes());
        }
        o.resize(align(o.len(), 4), 0);
    };
    for &d in &order {
        let dir = &dirs[d];
        let parent = &dirs[dir.parent];
        let sibling = parent
            .dirs
            .iter()
            .position(|&x| x == d)
            .and_then(|i| parent.dirs.get(i + 1))
            .map(|&s| dirs[s].offset)
            .unwrap_or(EMPTY);
        let sibling = if d == 0 { EMPTY } else { sibling };
        p32(&mut out, parent.offset);
        p32(&mut out, sibling);
        p32(
            &mut out,
            dir.dirs.first().map(|&c| dirs[c].offset).unwrap_or(EMPTY),
        );
        p32(
            &mut out,
            dir.files.first().map(|&c| fs[c].offset).unwrap_or(EMPTY),
        );
        p32(&mut out, dir_next[d]);
        p32(&mut out, (dir.name.len() * 2) as u32);
        name_bytes(&mut out, &dir.name);
    }
    for v in &file_hash {
        p32(&mut out, *v);
    }
    for &d in &order {
        let files = &dirs[d].files;
        for (i, &f) in files.iter().enumerate() {
            let file = &fs[f];
            p32(&mut out, dirs[file.parent].offset);
            p32(
                &mut out,
                files.get(i + 1).map(|&s| fs[s].offset).unwrap_or(EMPTY),
            );
            out.extend_from_slice(&file.data_offset.to_le_bytes());
            out.extend_from_slice(&(file.data.len() as u64).to_le_bytes());
            p32(&mut out, file_next[f]);
            p32(&mut out, (file.name.len() * 2) as u32);
            name_bytes(&mut out, &file.name);
        }
    }
    out.resize(data_start, 0);
    for &d in &order {
        for &f in &dirs[d].files {
            debug_assert_eq!(out.len(), data_start + fs[f].data_offset as usize);
            out.extend_from_slice(fs[f].data);
            out.resize(align(out.len(), 0x10), 0);
        }
    }
    out
}

fn hash_level(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len().div_ceil(BLOCK) * 32);
    for chunk in data.chunks(BLOCK) {
        if chunk.len() == BLOCK {
            out.extend_from_slice(&sha256(chunk));
        } else {
            let mut b = chunk.to_vec();
            b.resize(BLOCK, 0);
            out.extend_from_slice(&sha256(&b));
        }
    }
    out
}

/// Build a complete RomFS image containing `files`.
pub fn build(files: &[RomfsFile]) -> Vec<u8> {
    let l3 = build_level3(files);
    let l2 = hash_level(&l3);
    let l1 = hash_level(&l2);
    let master = hash_level(&l1);

    let l1_off = 0u64;
    let l2_off = align(l1.len(), BLOCK) as u64;
    let l3_off = align(l2_off as usize + l2.len(), BLOCK) as u64;

    let mut out = Vec::new();
    out.extend_from_slice(b"IVFC");
    out.extend_from_slice(&0x10000u32.to_le_bytes());
    out.extend_from_slice(&(master.len() as u32).to_le_bytes());
    for (off, len) in [(l1_off, l1.len()), (l2_off, l2.len()), (l3_off, l3.len())] {
        out.extend_from_slice(&off.to_le_bytes());
        out.extend_from_slice(&(len as u64).to_le_bytes());
        out.extend_from_slice(&BLOCK_LOG2.to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes());
    }
    out.extend_from_slice(&0x5Cu32.to_le_bytes());
    out.resize(0x60, 0);
    out.extend_from_slice(&master);
    out.resize(align(out.len(), BLOCK), 0);
    out.extend_from_slice(&l3);
    out.resize(align(out.len(), BLOCK), 0);
    out.extend_from_slice(&l1);
    out.resize(align(out.len(), BLOCK), 0);
    out.extend_from_slice(&l2);
    out.resize(align(out.len(), BLOCK), 0);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_romfs_matches_reference() {
        // The empty RomFS found in GBA Virtual Console titles.
        let reference = include_bytes!("../../../assets/template/romfs.bin");
        assert_eq!(build(&[]), reference.to_vec());
    }

    #[test]
    fn nested_files_layout() {
        let a = vec![1u8; 5000];
        let b = b"hello".to_vec();
        let img = build(&[
            RomfsFile {
                path: "game.gba",
                data: &a,
            },
            RomfsFile {
                path: "cfg/settings.ini",
                data: &b,
            },
        ]);
        assert_eq!(&img[0..4], b"IVFC");
        assert_eq!(img.len() % BLOCK, 0);
    }
}
