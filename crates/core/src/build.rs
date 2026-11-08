//! High-level build pipeline: ROM in, installable CIA out.

use crate::cia::{self, CiaInput, Meta};
use crate::gba::{self, RomInfo, SaveKind};
use crate::image_util::{self, Rgba};
use crate::ncch::{self, NcchInput, sha256};
use crate::{Error, art, banner, smdh, title_id, vc};

const NCCH_HEADER: &[u8] = include_bytes!("../../../assets/template/ncchheader.bin");
const EXHEADER: &[u8] = include_bytes!("../../../assets/template/exheader.bin");
const LOGO: &[u8] = include_bytes!("../../../assets/template/logo.darc.lz");
const EMPTY_ROMFS: &[u8] = include_bytes!("../../../assets/template/romfs.bin");

pub use crate::vc::Colour;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// Native GBA hardware through AGB_FIRM.
    VirtualConsole,
    /// Emulator player with a touch panel on the bottom screen.
    DualScreen,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScreenScale {
    /// 400x240, the whole top screen.
    Full,
    /// 360x240, original aspect ratio.
    Aspect,
    /// 240x160, unscaled.
    Native,
}

#[derive(Debug, Clone)]
pub struct BuildOptions {
    pub mode: Mode,
    pub title: Option<String>,
    pub publisher: Option<String>,
    pub title_id: Option<u64>,
    /// `None` = detect from the ROM.
    pub save: Option<SaveKind>,
    /// `None` = detect from the ROM.
    pub rtc: Option<bool>,
    pub icon: Option<Vec<u8>>,
    pub banner: Option<Vec<u8>>,
    pub banner_sound: Option<Vec<u8>>,
    pub scale: ScreenScale,
    /// Virtual Console colour rendering.
    pub colour: Colour,
    /// Dual screen mode: save file installed on first launch.
    pub initial_save: Option<Vec<u8>>,
}

impl Default for BuildOptions {
    fn default() -> Self {
        Self {
            mode: Mode::VirtualConsole,
            title: None,
            publisher: None,
            title_id: None,
            save: None,
            rtc: None,
            icon: None,
            banner: None,
            banner_sound: None,
            scale: ScreenScale::Full,
            colour: Colour::Vivid,
            initial_save: None,
        }
    }
}

#[derive(Debug, Clone)]
pub struct BuildOutput {
    pub cia: Vec<u8>,
    pub rom: RomInfo,
    pub title_id: u64,
    pub product_code: String,
    pub title: String,
    pub save: SaveKind,
    pub rtc: bool,
    pub mode_label: &'static str,
    pub notes: Vec<String>,
}

fn clean_title(s: &str) -> String {
    s.chars()
        .filter(|c| !c.is_control())
        .collect::<String>()
        .trim()
        .to_string()
}

fn title_from_rom(info: &RomInfo) -> String {
    if info.title.is_empty() {
        "GBA game".into()
    } else {
        info.title.clone()
    }
}

/// Exheader for a GBA VC title: the official template with this title's IDs
/// and the save container size matching the save chip.
fn vc_exheader(title_id: u64, product_code: &str, save_size: u32) -> Vec<u8> {
    let mut exh = EXHEADER.to_vec();
    // Application title: the product code suffix, never empty (an empty name
    // makes the HOME Menu fail to launch the title).
    let suffix = product_code.rsplit('-').next().unwrap_or("AGBV");
    let mut name = [0u8; 8];
    for (i, b) in suffix.bytes().take(8).enumerate() {
        name[i] = b;
    }
    exh[0x000..0x008].copy_from_slice(&name);
    exh[0x1C0..0x1C8].copy_from_slice(&(save_size as u64).to_le_bytes());
    exh[0x1C8..0x1D0].copy_from_slice(&title_id.to_le_bytes()); // jump ID
    exh[0x200..0x208].copy_from_slice(&title_id.to_le_bytes()); // ACI program ID
    // The access descriptor at 0x400 is signed and left untouched.
    exh
}

fn load_icon(opts: &BuildOptions, seed: &str) -> Result<(Rgba, Rgba), Error> {
    let src = match &opts.icon {
        Some(bytes) => image_util::decode(bytes)?,
        None => art::default_icon(seed),
    };
    Ok((
        image_util::square_icon(&src, 48),
        image_util::square_icon(&src, 24),
    ))
}

fn load_banner(opts: &BuildOptions, seed: &str) -> Result<Rgba, Error> {
    Ok(match &opts.banner {
        Some(bytes) => image_util::fit(&image_util::decode(bytes)?, banner::WIDTH, banner::HEIGHT),
        None => art::default_banner(seed),
    })
}

pub fn build(rom: &[u8], opts: &BuildOptions) -> Result<BuildOutput, Error> {
    let info = gba::inspect(rom)?;
    let mut notes = info.notes.clone();
    let save = opts.save.unwrap_or(info.save);
    let mut rtc = opts.rtc.unwrap_or(info.rtc);
    let title = clean_title(opts.title.as_deref().unwrap_or(&title_from_rom(&info)));
    if title.is_empty() {
        return Err(Error::Option("title cannot be empty".into()));
    }
    let publisher = clean_title(opts.publisher.as_deref().unwrap_or("GBA"));
    let tid = match opts.title_id {
        Some(t) => {
            title_id::validate(t)?;
            t
        }
        None => title_id::derive(&info.game_code, &title),
    };
    let product_code = title_id::product_code(&info.game_code);
    let seed = format!("{}{}", info.game_code, title);

    let mut rom = rom.to_vec();
    if !info.header_checksum_ok {
        gba::fix_header_checksum(&mut rom);
    }

    let (icon_l, icon_s) = load_icon(opts, &seed)?;
    let smdh_bytes = smdh::Smdh {
        short_title: &title,
        long_title: &title,
        publisher: &publisher,
        icon_large: &icon_l,
        icon_small: &icon_s,
        flags: smdh::flags::VISIBLE
            | smdh::flags::AUTOSAVE_ON_EXIT
            | smdh::flags::USES_SAVE_DATA
            | smdh::flags::RECORD_USAGE,
    }
    .build();
    let banner_img = load_banner(opts, &seed)?;
    let banner_bytes = banner::build(&banner_img, opts.banner_sound.as_deref())?;

    let (ncch, save_size, mode_label) = match opts.mode {
        Mode::VirtualConsole => {
            if opts.initial_save.is_some() {
                notes.push(
                    "Virtual Console titles cannot carry a save file. Import it on the console with GodMode9 (see the save converter)."
                        .into(),
                );
            }
            if rtc && !vc::rtc_supported(save) {
                notes.push(format!(
                    "The real-time clock only works with flash saves on Virtual Console; it is disabled for {}.",
                    save.label()
                ));
                rtc = false;
            }
            let padded = vc::pad_rom(&rom);
            if padded.len() != rom.len() {
                notes.push(format!(
                    "ROM padded from {} to {} bytes (next power of two), as a cartridge would read.",
                    rom.len(),
                    padded.len()
                ));
            }
            let code = vc::build_code(
                &padded,
                &vc::VcOptions {
                    save,
                    rtc,
                    colour: opts.colour,
                    sleep_combo: 0,
                },
            )?;
            let save_size = vc::save_container_size(save);
            let exefs = ncch::build_exefs(&[
                (".code", &code),
                ("banner", &banner_bytes),
                ("icon", &smdh_bytes),
                ("logo", LOGO),
            ])?;
            let exh = vc_exheader(tid, &product_code, save_size);
            let n = ncch::build_ncch(&NcchInput {
                header_base: NCCH_HEADER,
                exheader: &exh,
                exefs: &exefs,
                romfs: Some(EMPTY_ROMFS),
                title_id: tid,
                product_code: &product_code,
                maker_code: *b"01",
            })?;
            (n, save_size, "Virtual Console (native GBA hardware)")
        }
        Mode::DualScreen => {
            return Err(Error::Option(
                "dual screen mode needs the player template, which is not bundled in this build"
                    .into(),
            ));
        }
    };

    // Self-check: the NCCH must parse and every hash must verify.
    let view = ncch::parse_ncch(&ncch)?;
    let deps = view.exheader[0x40..0x1C0].to_vec();
    let core_version = u32::from_le_bytes(view.exheader[0x208..0x20C].try_into().unwrap());

    let entropy: [u8; 16] = sha256(&[&tid.to_le_bytes()[..], &sha256(&rom)[..]].concat())[..16]
        .try_into()
        .unwrap();
    let cia_bytes = cia::build(&CiaInput {
        title_id: tid,
        title_version: 0,
        content: &ncch,
        save_data_size: save_size,
        meta: Some(Meta {
            dependencies: &deps,
            core_version,
            smdh: &smdh_bytes,
        }),
        entropy,
    });
    cia::parse(&cia_bytes)?;

    if save == SaveKind::None {
        notes
            .push("No save type: in-game saving will not work. Pick one if the game saves.".into());
    }
    Ok(BuildOutput {
        cia: cia_bytes,
        rom: info,
        title_id: tid,
        product_code,
        title,
        save,
        rtc,
        mode_label,
        notes,
    })
}
