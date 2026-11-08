//! Browser bindings. Everything runs locally: files never leave the page.

use gba2cia_core::{
    build::{self, BuildOptions, Colour, Mode, ScreenScale},
    gba::{self, Confidence, RomInfo, SaveKind},
    patch, save, title_id,
};
use serde::Deserialize;
use serde_json::{Value, json};
use wasm_bindgen::prelude::*;

fn err(e: impl std::fmt::Display) -> JsError {
    JsError::new(&e.to_string())
}

fn rom_json(i: &RomInfo) -> Value {
    json!({
        "title": i.title,
        "gameCode": i.game_code,
        "makerCode": i.maker_code,
        "version": i.version,
        "size": i.size,
        "headerChecksumOk": i.header_checksum_ok,
        "saveSignature": i.save_signature,
        "save": i.save.id(),
        "saveLabel": i.save.label(),
        "saveConfidence": match i.save_confidence {
            Confidence::Database => "database",
            Confidence::Signature => "signature",
            Confidence::Guess => "guess",
            Confidence::Default => "default",
        },
        "rtc": i.rtc,
        "rtcReason": i.rtc_reason,
        "notes": i.notes,
    })
}

/// Detected information about a ROM, as JSON.
#[wasm_bindgen(js_name = inspectRom)]
pub fn inspect_rom(rom: &[u8]) -> Result<String, JsError> {
    let info = gba::inspect(rom).map_err(err)?;
    Ok(rom_json(&info).to_string())
}

/// Patch header information, as JSON.
#[wasm_bindgen(js_name = inspectPatch)]
pub fn inspect_patch(data: &[u8]) -> Result<String, JsError> {
    let i = patch::inspect(data).map_err(err)?;
    Ok(json!({
        "format": i.format.name(),
        "sourceSize": i.source_size,
        "sourceCrc32": i.source_crc32.map(|c| format!("{c:08X}")),
        "targetSize": i.target_size,
        "targetCrc32": i.target_crc32.map(|c| format!("{c:08X}")),
    })
    .to_string())
}

#[wasm_bindgen(js_name = applyPatch)]
pub fn apply_patch(rom: &[u8], data: &[u8]) -> Result<Vec<u8>, JsError> {
    patch::apply(rom, data).map_err(err)
}

#[wasm_bindgen]
pub fn crc32(data: &[u8]) -> String {
    format!("{:08X}", patch::crc32(data))
}

#[wasm_bindgen(js_name = saveTypes)]
pub fn save_types() -> String {
    Value::Array(
        SaveKind::ALL
            .iter()
            .map(|k| json!({"id": k.id(), "label": k.label(), "size": k.size()}))
            .collect(),
    )
    .to_string()
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct JsOptions {
    mode: Option<String>,
    title: Option<String>,
    publisher: Option<String>,
    title_id: Option<String>,
    save: Option<String>,
    rtc: Option<String>,
    scale: Option<String>,
    colour: Option<String>,
}

#[wasm_bindgen]
pub struct BuildResult {
    cia: Vec<u8>,
    report: String,
}

#[wasm_bindgen]
impl BuildResult {
    /// The .cia file.
    pub fn cia(&self) -> Vec<u8> {
        self.cia.clone()
    }
    /// JSON build report.
    pub fn report(&self) -> String {
        self.report.clone()
    }
}

fn non_empty(s: Option<String>) -> Option<String> {
    s.map(|v| v.trim().to_string()).filter(|v| !v.is_empty())
}

#[wasm_bindgen(js_name = buildCia)]
pub fn build_cia(
    rom: &[u8],
    options: &str,
    icon: Option<Vec<u8>>,
    banner: Option<Vec<u8>>,
    banner_sound: Option<Vec<u8>>,
    initial_save: Option<Vec<u8>>,
) -> Result<BuildResult, JsError> {
    let o: JsOptions = serde_json::from_str(options).map_err(err)?;
    let save = match non_empty(o.save).as_deref() {
        None | Some("auto") => None,
        Some(s) => Some(SaveKind::from_id(s).ok_or_else(|| err(format!("unknown save type {s}")))?),
    };
    let opts = BuildOptions {
        mode: match o.mode.as_deref() {
            Some("dual") => Mode::DualScreen,
            _ => Mode::VirtualConsole,
        },
        title: non_empty(o.title),
        publisher: non_empty(o.publisher),
        title_id: non_empty(o.title_id)
            .map(|t| title_id::parse(&t))
            .transpose()
            .map_err(err)?,
        save,
        rtc: match o.rtc.as_deref() {
            Some("on") => Some(true),
            Some("off") => Some(false),
            _ => None,
        },
        icon,
        banner,
        banner_sound,
        scale: match o.scale.as_deref() {
            Some("aspect") => ScreenScale::Aspect,
            Some("native") => ScreenScale::Native,
            _ => ScreenScale::Full,
        },
        colour: match o.colour.as_deref() {
            Some("original") => Colour::Original,
            _ => Colour::Vivid,
        },
        initial_save,
        test_script: None,
        output_cxi: false,
    };
    let out = build::build(rom, &opts).map_err(err)?;
    let report = json!({
        "rom": rom_json(&out.rom),
        "titleId": format!("{:016X}", out.title_id),
        "productCode": out.product_code,
        "title": out.title,
        "save": out.save.id(),
        "saveLabel": out.save.label(),
        "rtc": out.rtc,
        "mode": out.mode_label,
        "notes": out.notes,
        "size": out.cia.len(),
    })
    .to_string();
    Ok(BuildResult {
        cia: out.cia,
        report,
    })
}

#[wasm_bindgen]
pub struct SaveResult {
    data: Vec<u8>,
    report: String,
}

#[wasm_bindgen]
impl SaveResult {
    pub fn data(&self) -> Vec<u8> {
        self.data.clone()
    }
    pub fn report(&self) -> String {
        self.report.clone()
    }
}

/// Normalise an emulator save. `kind` is a save type id, or "auto" to guess
/// from the file size.
#[wasm_bindgen(js_name = convertSave)]
pub fn convert_save(sav: &[u8], kind: &str) -> Result<SaveResult, JsError> {
    let k = if kind == "auto" || kind.is_empty() {
        save::guess_kind(sav.len())
            .ok_or_else(|| err("cannot guess the save type from the file size; choose one"))?
    } else {
        SaveKind::from_id(kind).ok_or_else(|| err(format!("unknown save type {kind}")))?
    };
    let n = save::normalize(sav, k).map_err(err)?;
    Ok(SaveResult {
        report:
            json!({"save": k.id(), "saveLabel": k.label(), "size": n.data.len(), "notes": n.notes})
                .to_string(),
        data: n.data,
    })
}
