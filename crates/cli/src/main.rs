use clap::{Parser, Subcommand, ValueEnum};
use gba2cia_core::{
    build::{self, BuildOptions, Colour, Mode, ScreenScale},
    gba::{self, SaveKind},
    patch, save, title_id,
};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

#[derive(Parser)]
#[command(
    name = "gba2cia",
    version,
    about = "Turn a GBA ROM into an installable Nintendo 3DS .cia",
    long_about = "Turn a Game Boy Advance ROM into an installable Nintendo 3DS title (.cia).\n\n\
Two output modes:\n  \
vc    Virtual Console: runs on the console's built-in GBA hardware (AGB_FIRM).\n        \
Exact speed and sound, bottom screen off.\n  \
dual  Dual screen: runs in an emulator, the game fills the top screen and the\n        \
bottom screen is a touch panel.\n\n\
Install the .cia with FBI on a console running Luma3DS."
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Clone, Copy, ValueEnum)]
enum ModeArg {
    Vc,
    Dual,
}

#[derive(Clone, Copy, ValueEnum)]
enum RtcArg {
    Auto,
    On,
    Off,
}

#[derive(Clone, Copy, ValueEnum)]
enum ColourArg {
    /// No colour correction, no ghosting (like an emulator).
    Vivid,
    /// Dimmer colours and LCD ghosting, as in Nintendo's Virtual Console titles.
    Original,
}

#[derive(Clone, Copy, ValueEnum)]
enum ScaleArg {
    /// Stretch to the full 400x240 top screen.
    Full,
    /// Keep the 3:2 aspect ratio (360x240).
    Aspect,
    /// Native 240x160, no scaling.
    Native,
}

#[derive(Subcommand)]
enum Command {
    /// Build a .cia from a ROM (optionally patching it first).
    Build {
        /// GBA ROM (.gba). With --patch, the clean original game.
        rom: PathBuf,
        /// IPS, UPS or BPS patch to apply before building (ROM hacks).
        #[arg(long)]
        patch: Option<PathBuf>,
        /// Output file (default: next to the ROM, .cia extension).
        #[arg(short, long)]
        output: Option<PathBuf>,
        #[arg(long, value_enum, default_value = "vc")]
        mode: ModeArg,
        /// Name shown on the HOME Menu (default: ROM file name).
        #[arg(short, long)]
        title: Option<String>,
        #[arg(short, long)]
        publisher: Option<String>,
        /// Icon image (PNG/JPEG/GIF/BMP/WebP), cropped to a square.
        #[arg(long)]
        icon: Option<PathBuf>,
        /// Banner image shown on the top screen in the HOME Menu (256x128 area).
        #[arg(long)]
        banner: Option<PathBuf>,
        /// Banner sound (WAV, up to 3 seconds).
        #[arg(long)]
        banner_sound: Option<PathBuf>,
        /// Save type: auto, none, eeprom4k, eeprom64k, sram, flash512k, flash1m (= Flash 128 KB).
        #[arg(long, default_value = "auto")]
        save: String,
        /// Real-time clock (needed by Pokemon Ruby/Sapphire/Emerald and hacks such as Unbound).
        #[arg(long, value_enum, default_value = "auto")]
        rtc: RtcArg,
        /// Title ID (16 hex digits). Default: derived from the game, stable across rebuilds.
        #[arg(long)]
        title_id: Option<String>,
        /// Virtual Console colour rendering.
        #[arg(long, value_enum, default_value = "vivid")]
        colour: ColourArg,
        /// Dual screen mode: how the game fills the top screen.
        #[arg(long, value_enum, default_value = "full")]
        scale: ScaleArg,
        /// Save file to bundle (dual screen mode only; for VC see `gba2cia save`).
        #[arg(long)]
        sav: Option<PathBuf>,
        /// Write the bare NCCH (.cxi) instead of a CIA (boots directly in emulators).
        #[arg(long, hide = true)]
        cxi: bool,
        /// Dual screen test script: scripted input and screenshots.
        #[arg(long, hide = true)]
        test_script: Option<String>,
    },
    /// Show what the tool detects in a ROM.
    Info { rom: PathBuf },
    /// Apply an IPS/UPS/BPS patch and write the patched ROM.
    Patch {
        rom: PathBuf,
        patch: PathBuf,
        #[arg(short, long)]
        output: PathBuf,
    },
    /// Convert an emulator .sav for import on the console.
    Save {
        /// Emulator save file (.sav, .srm).
        sav: PathBuf,
        /// Save type (default: guessed from the file size, or from --rom).
        #[arg(long)]
        save: Option<String>,
        /// ROM the save belongs to, used to detect the save type.
        #[arg(long)]
        rom: Option<PathBuf>,
        #[arg(short, long)]
        output: PathBuf,
    },
}

fn read(p: &Path) -> Result<Vec<u8>, String> {
    std::fs::read(p).map_err(|e| format!("cannot read {}: {e}", p.display()))
}

fn write(p: &Path, data: &[u8]) -> Result<(), String> {
    std::fs::write(p, data).map_err(|e| format!("cannot write {}: {e}", p.display()))
}

fn parse_save(s: &str) -> Result<Option<SaveKind>, String> {
    if s.eq_ignore_ascii_case("auto") {
        return Ok(None);
    }
    SaveKind::from_id(s).map(Some).ok_or_else(|| {
        format!("unknown save type '{s}' (use auto, none, eeprom4k, eeprom64k, sram, flash512k, flash1m)")
    })
}

fn print_info(info: &gba::RomInfo) {
    println!("Title        {}", info.title);
    println!("Game code    {}", info.game_code);
    println!("Maker        {}", info.maker_code);
    println!(
        "Size         {} bytes ({:.1} MiB)",
        info.size,
        info.size as f64 / 1048576.0
    );
    println!(
        "Save         {}{}",
        info.save.label(),
        info.save_signature
            .as_ref()
            .map(|s| format!("  [{s}]"))
            .unwrap_or_default()
    );
    println!(
        "Clock (RTC)  {}",
        if info.rtc {
            format!("yes ({})", info.rtc_reason.unwrap_or(""))
        } else {
            "no".into()
        }
    );
    for n in &info.notes {
        println!("Note         {n}");
    }
}

fn run() -> Result<(), String> {
    let cli = Cli::parse();
    match cli.command {
        Command::Info { rom } => {
            let rom = read(&rom)?;
            let info = gba::inspect(&rom).map_err(|e| e.to_string())?;
            print_info(&info);
        }
        Command::Patch {
            rom,
            patch: p,
            output,
        } => {
            let rom = read(&rom)?;
            let p = read(&p)?;
            let out = patch::apply(&rom, &p).map_err(|e| e.to_string())?;
            write(&output, &out)?;
            println!("Wrote {} ({} bytes)", output.display(), out.len());
        }
        Command::Save {
            sav,
            save: kind,
            rom,
            output,
        } => {
            let data = read(&sav)?;
            let kind = match (kind.as_deref().map(parse_save).transpose()?.flatten(), rom) {
                (Some(k), _) => k,
                (None, Some(rom)) => gba::inspect(&read(&rom)?).map_err(|e| e.to_string())?.save,
                (None, None) => save::guess_kind(data.len()).ok_or_else(|| {
                    "cannot guess the save type from the file size; pass --save or --rom"
                        .to_string()
                })?,
            };
            let n = save::normalize(&data, kind).map_err(|e| e.to_string())?;
            write(&output, &n.data)?;
            for note in &n.notes {
                println!("{note}");
            }
            println!(
                "Wrote {} ({}, {} bytes)",
                output.display(),
                kind.label(),
                n.data.len()
            );
        }
        Command::Build {
            rom,
            patch: p,
            output,
            mode,
            title,
            publisher,
            icon,
            banner,
            banner_sound,
            save: save_type,
            rtc,
            title_id: tid,
            colour,
            scale,
            sav,
            cxi,
            test_script,
        } => {
            let mut data = read(&rom)?;
            if let Some(p) = &p {
                let pd = read(p)?;
                data = patch::apply(&data, &pd).map_err(|e| e.to_string())?;
                println!("Applied {} ({} bytes)", p.display(), data.len());
            }
            let default_title = p
                .as_ref()
                .unwrap_or(&rom)
                .file_stem()
                .map(|s| s.to_string_lossy().to_string())
                .unwrap_or_else(|| "GBA game".into());
            let opts = BuildOptions {
                mode: match mode {
                    ModeArg::Vc => Mode::VirtualConsole,
                    ModeArg::Dual => Mode::DualScreen,
                },
                title: Some(title.unwrap_or(default_title)),
                publisher,
                title_id: tid
                    .as_deref()
                    .map(title_id::parse)
                    .transpose()
                    .map_err(|e| e.to_string())?,
                save: parse_save(&save_type)?,
                rtc: match rtc {
                    RtcArg::Auto => None,
                    RtcArg::On => Some(true),
                    RtcArg::Off => Some(false),
                },
                icon: icon.as_deref().map(read).transpose()?,
                banner: banner.as_deref().map(read).transpose()?,
                banner_sound: banner_sound.as_deref().map(read).transpose()?,
                scale: match scale {
                    ScaleArg::Full => ScreenScale::Full,
                    ScaleArg::Aspect => ScreenScale::Aspect,
                    ScaleArg::Native => ScreenScale::Native,
                },
                colour: match colour {
                    ColourArg::Vivid => Colour::Vivid,
                    ColourArg::Original => Colour::Original,
                },
                initial_save: sav.as_deref().map(read).transpose()?,
                test_script,
                output_cxi: cxi,
            };
            let out = build::build(&data, &opts).map_err(|e| e.to_string())?;
            let output = output.unwrap_or_else(|| {
                let mut o = p.as_ref().unwrap_or(&rom).clone();
                o.set_extension(if cxi { "cxi" } else { "cia" });
                o
            });
            write(&output, &out.cia)?;
            print_info(&out.rom);
            println!("Mode         {}", out.mode_label);
            println!("Title ID     {:016X}", out.title_id);
            println!("Save used    {}", out.save.label());
            println!("Clock used   {}", if out.rtc { "on" } else { "off" });
            for n in &out.notes {
                println!("Note         {n}");
            }
            println!(
                "Wrote {} ({:.1} MiB)",
                output.display(),
                out.cia.len() as f64 / 1048576.0
            );
        }
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::FAILURE
        }
    }
}
