//! Emulator save files (.sav / .srm): validation and normalisation.
//!
//! Emulators do not agree on the exact layout of a save file:
//! - mGBA and VBA-M may append a small real-time clock block after the data;
//! - some emulators always write 64 KiB or 128 KiB regardless of the chip;
//! - some write EEPROM saves at 8 KiB even for 4 Kbit chips.
//!
//! `normalize` turns any of these into the exact size of the target chip.

use crate::Error;
use crate::gba::SaveKind;

#[derive(Debug, Clone)]
pub struct NormalizedSave {
    pub data: Vec<u8>,
    pub notes: Vec<String>,
}

/// Guess the chip from a save file size alone.
pub fn guess_kind(len: usize) -> Option<SaveKind> {
    // Allow small trailers (RTC blocks are 16 or 20 bytes, some tools add up to 0x200).
    let fits = |n: usize| len >= n && len <= n + 0x200;
    if fits(128 * 1024) {
        Some(SaveKind::Flash1m)
    } else if fits(64 * 1024) {
        Some(SaveKind::Flash512k)
    } else if fits(32 * 1024) {
        Some(SaveKind::Sram)
    } else if fits(8 * 1024) {
        Some(SaveKind::Eeprom64k)
    } else if fits(512) {
        Some(SaveKind::Eeprom4k)
    } else {
        None
    }
}

pub fn normalize(sav: &[u8], kind: SaveKind) -> Result<NormalizedSave, Error> {
    let want = kind.size();
    if want == 0 {
        return Err(Error::Save(
            "this game has no save memory; set a save type before importing a save".into(),
        ));
    }
    if sav.is_empty() {
        return Err(Error::Save("the save file is empty".into()));
    }
    let mut notes = Vec::new();
    let mut data = sav.to_vec();
    if data.len() > want {
        let extra = data.len() - want;
        let tail_used = data[want..].iter().any(|&b| b != 0xFF && b != 0x00);
        if extra <= 0x200 {
            notes.push(format!(
                "Removed a {extra}-byte trailer (emulator clock data) after the save."
            ));
        } else if !tail_used {
            notes.push(format!(
                "File is {} bytes; only the first {} bytes are used by this chip and the rest was empty.",
                sav.len(),
                want
            ));
        } else {
            return Err(Error::Save(format!(
                "save file is {} bytes but the selected save type ({}) holds {} bytes and the extra data is not empty. Check the save type.",
                sav.len(),
                kind.label(),
                want
            )));
        }
        data.truncate(want);
    } else if data.len() < want {
        notes.push(format!(
            "Save is {} bytes, padded to {} bytes for {}.",
            sav.len(),
            want,
            kind.label()
        ));
        // Erased flash and EEPROM read back as 0xFF.
        data.resize(want, 0xFF);
    }
    if data.iter().all(|&b| b == 0xFF) || data.iter().all(|&b| b == 0x00) {
        notes.push("Warning: the save contains no data (all bytes identical).".into());
    }
    Ok(NormalizedSave { data, notes })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn trims_rtc_trailer() {
        let mut s = vec![0x12; 128 * 1024];
        s.extend_from_slice(&[1; 16]);
        let n = normalize(&s, SaveKind::Flash1m).unwrap();
        assert_eq!(n.data.len(), 128 * 1024);
        assert_eq!(guess_kind(s.len()), Some(SaveKind::Flash1m));
    }

    #[test]
    fn pads_short_flash() {
        let s = vec![0x12; 64 * 1024];
        let n = normalize(&s, SaveKind::Flash1m).unwrap();
        assert_eq!(n.data.len(), 128 * 1024);
        assert_eq!(n.data[64 * 1024], 0xFF);
    }

    #[test]
    fn rejects_wrong_type_with_data() {
        let s = vec![0x12; 128 * 1024];
        assert!(normalize(&s, SaveKind::Sram).is_err());
    }
}
