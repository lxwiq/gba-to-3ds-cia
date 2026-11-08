//! Title ID and product code selection.
//!
//! Title IDs have the form `00040000 00UUUU00`:
//! - `00040000` is the application category;
//! - the unique ID is kept in `0x00F000..=0x00FFFF`, above every retail title,
//!   with a zero top byte;
//! - the variation byte is `00`.
//!
//! The unique ID is derived from the game code and title so that rebuilding
//! the same game produces the same title ID: reinstalling then updates the
//! title in place and keeps its save data.

use crate::Error;
use crate::ncch::sha256;

pub const MIN_UNIQUE: u32 = 0xF000;
pub const MAX_UNIQUE: u32 = 0xFFFF;

pub fn from_unique(unique: u32) -> u64 {
    0x0004_0000_0000_0000 | ((unique as u64 & 0xFF_FFFF) << 8)
}

pub fn unique_of(title_id: u64) -> u32 {
    ((title_id >> 8) & 0xFF_FFFF) as u32
}

/// Deterministic title ID for a game.
pub fn derive(game_code: &str, title: &str) -> u64 {
    let h = sha256(format!("gba-to-3ds-cia/{game_code}/{title}").as_bytes());
    let v = u16::from_le_bytes([h[0], h[1]]) as u32;
    from_unique(MIN_UNIQUE + v % (MAX_UNIQUE - MIN_UNIQUE + 1))
}

pub fn parse(s: &str) -> Result<u64, Error> {
    let t = s.trim().trim_start_matches("0x").trim_start_matches("0X");
    if t.len() != 16 || !t.chars().all(|c| c.is_ascii_hexdigit()) {
        return Err(Error::Option(format!(
            "title ID must be 16 hexadecimal digits, got '{s}'"
        )));
    }
    let id = u64::from_str_radix(t, 16).unwrap();
    validate(id)?;
    Ok(id)
}

pub fn validate(id: u64) -> Result<(), Error> {
    if id >> 32 != 0x0004_0000 {
        return Err(Error::Option(format!(
            "title ID {id:016X} must start with 00040000 (application)"
        )));
    }
    if id & 0xFF != 0 {
        return Err(Error::Option(format!(
            "title ID {id:016X} must end with 00 (variation byte); other values make the title fail to launch"
        )));
    }
    if (id >> 24) & 0xFF != 0 {
        return Err(Error::Option(format!(
            "title ID {id:016X}: the 5th byte must be 00 (top byte of the unique ID)"
        )));
    }
    Ok(())
}

/// Product code `CTR-N-XXXX`, XXXX from the GBA game code when it is clean.
pub fn product_code(game_code: &str) -> String {
    let clean: String = game_code
        .chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .map(|c| c.to_ascii_uppercase())
        .collect();
    if clean.len() == 4 {
        format!("CTR-N-{clean}")
    } else {
        "CTR-N-AGBV".to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn derived_ids_are_valid_and_stable() {
        let a = derive("BPRE", "POKEMON FIRE");
        assert_eq!(a, derive("BPRE", "POKEMON FIRE"));
        validate(a).unwrap();
        let u = unique_of(a);
        assert!((MIN_UNIQUE..=MAX_UNIQUE).contains(&u));
        assert_eq!(parse(&format!("{a:016X}")).unwrap(), a);
    }

    #[test]
    fn rejects_bad_ids() {
        assert!(parse("0004000000F00101").is_err());
        assert!(parse("0004000001F00100").is_err());
        assert!(parse("1234").is_err());
    }
}
