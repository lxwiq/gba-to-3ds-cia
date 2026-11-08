fn main() {
    let p = std::fs::read(std::env::args().nth(1).expect("patch path")).unwrap();
    let i = gba2cia_core::patch::inspect(&p).unwrap();
    println!(
        "{:?} src={:?} crc={:08X?} dst={:?} crc={:08X?}",
        i.format, i.source_size, i.source_crc32, i.target_size, i.target_crc32
    );
    let body_crc = gba2cia_core::patch::crc32(&p[..p.len() - 4]);
    let stored = u32::from_le_bytes(p[p.len() - 4..].try_into().unwrap());
    println!("patch self-check {}", body_crc == stored);
}
