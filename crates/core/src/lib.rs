//! Build Nintendo 3DS GBA Virtual Console titles (.cia) from GBA ROMs.
//!
//! Everything is implemented natively: no makerom, 3dstool or bannertool.
//! The same code runs as a command-line tool and, compiled to WebAssembly,
//! entirely inside a web browser.

pub mod art;
pub mod banner;
pub mod build;
pub mod cia;
pub mod dual;
pub mod gba;
pub mod image_util;
pub mod ncch;
pub mod patch;
pub mod romfs;
pub mod save;
pub mod smdh;
pub mod title_id;
pub mod vc;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("ROM: {0}")]
    Rom(String),
    #[error("patch: {0}")]
    Patch(String),
    #[error(
        "this patch was made for a different ROM (expected CRC32 {expected:08X}, got {actual:08X}). Use a clean, unmodified dump of the original game."
    )]
    PatchSourceMismatch { expected: u32, actual: u32 },
    #[error("image: {0}")]
    Image(String),
    #[error("save: {0}")]
    Save(String),
    #[error("build: {0}")]
    Build(String),
    #[error("invalid option: {0}")]
    Option(String),
}
