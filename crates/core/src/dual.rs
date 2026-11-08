//! Dual screen mode: the gba2cia player (mGBA core, see `player/`) with the
//! ROM bundled in its RomFS.
//!
//! The template `assets/player/player.cxi` is the player built by makerom,
//! without icon, banner or RomFS. For each game the title IDs are rewritten,
//! the icon and banner are replaced, and a RomFS is added:
//!
//! ```text
//! romfs:/game.gba     the ROM
//! romfs:/player.cfg   settings read by the player
//! romfs:/initial.sav  optional starting save
//! ```

use crate::Error;
use crate::build::ScreenScale;
use crate::gba::SaveKind;
use crate::ncch::{self, NcchInput};
use crate::romfs::{self, RomfsFile};

const TEMPLATE: &[u8] = include_bytes!("../../../assets/player/player.cxi");

pub struct DualOptions<'a> {
    pub title: &'a str,
    pub title_id: u64,
    pub product_code: &'a str,
    pub game_code: &'a str,
    pub save: SaveKind,
    pub rtc: bool,
    pub scale: ScreenScale,
    pub initial_save: Option<&'a [u8]>,
    /// Scripted input and screenshots, used to test the player in an emulator.
    pub test_script: Option<&'a str>,
}

fn config_text(o: &DualOptions) -> String {
    let scale = match o.scale {
        ScreenScale::Full => "full",
        ScreenScale::Aspect => "aspect",
        ScreenScale::Native => "native",
    };
    let title: String = o.title.chars().filter(|c| !c.is_control()).collect();
    let mut s = format!(
        "title={title}\ntitle_id={:016X}\ngame_code={}\nsave={}\nrtc={}\nscale={scale}\ninitial_save={}\n",
        o.title_id,
        o.game_code,
        o.save.id(),
        if o.rtc { 1 } else { 0 },
        if o.initial_save.is_some() { 1 } else { 0 },
    );
    if let Some(script) = o.test_script {
        s.push_str("test=1\n");
        s.push_str(&format!("test_script={script}\n"));
    }
    s
}

/// Build the NCCH (CXI) of a dual screen title.
pub fn build_ncch(rom: &[u8], icon: &[u8], banner: &[u8], o: &DualOptions) -> Result<Vec<u8>, Error> {
    let tpl = ncch::parse_ncch(TEMPLATE)?;
    let template_files = ncch::parse_exefs(tpl.exefs)?;
    let code = template_files
        .iter()
        .find(|(n, _)| n == ".code")
        .map(|(_, d)| d.as_slice())
        .ok_or_else(|| Error::Build("player template has no code".into()))?;
    let logo = template_files.iter().find(|(n, _)| n == "logo").map(|(_, d)| d.as_slice());

    let mut exh = tpl.exheader.to_vec();
    exh[0x1C8..0x1D0].copy_from_slice(&o.title_id.to_le_bytes()); // jump ID
    exh[0x200..0x208].copy_from_slice(&o.title_id.to_le_bytes()); // ACI program ID
    exh[0x600..0x608].copy_from_slice(&o.title_id.to_le_bytes()); // access descriptor copy

    let mut files: Vec<(&str, &[u8])> = vec![(".code", code), ("banner", banner), ("icon", icon)];
    if let Some(l) = logo {
        files.push(("logo", l));
    }
    let exefs = ncch::build_exefs(&files)?;

    let cfg = config_text(o);
    let mut entries = vec![
        RomfsFile { path: "game.gba", data: rom },
        RomfsFile { path: "player.cfg", data: cfg.as_bytes() },
    ];
    if let Some(sav) = o.initial_save {
        entries.push(RomfsFile { path: "initial.sav", data: sav });
    }
    let romfs = romfs::build(&entries);

    ncch::build_ncch(&NcchInput {
        header_base: &TEMPLATE[..0x200],
        exheader: &exh,
        exefs: &exefs,
        romfs: Some(&romfs),
        title_id: o.title_id,
        product_code: o.product_code,
        maker_code: *b"00",
    })
}
