//! Session state behind the WebAssembly exports in `exports`.
//!
//! JS fills the input buffer, calls an operation, and reads the output
//! buffer: the result on success, a UTF-8 message on failure.

mod exports;
mod zip;

use std::cell::RefCell;
use std::fmt::Write as _;
use std::mem;
use std::sync::Arc;

use archetypon::{Color, Error, Format, Options, Source, fontdb};

use crate::zip::Zip;

const INPUT_LIMIT: usize = 256 << 20;

const OK: u32 = 0;
const FAILED: u32 = 1;
const NO_FONTS: u32 = 2;

struct Failure {
	code: u32,
	message: String,
}

impl From<Error> for Failure {
	fn from(error: Error) -> Self {
		let code = if matches!(error, Error::NoFonts) {
			NO_FONTS
		} else {
			FAILED
		};
		Self {
			code,
			message: error.to_string(),
		}
	}
}

impl From<String> for Failure {
	fn from(message: String) -> Self {
		Self {
			code: FAILED,
			message,
		}
	}
}

fn fail(message: &str) -> Failure {
	Failure::from(message.to_owned())
}

struct Session {
	stem: String,
	source: Source,
	done: Vec<Format>,
	mark: usize,
}

#[derive(Default)]
struct State {
	input: Vec<u8>,
	output: Vec<u8>,
	fonts: Arc<fontdb::Database>,
	options: Options,
	stems: Vec<String>,
	session: Option<Session>,
	zip: Option<Zip>,
}

thread_local! {
	static STATE: RefCell<State> = RefCell::default();
}

fn call(operation: impl FnOnce(&mut State) -> Result<(), Failure>) -> u32 {
	STATE.with_borrow_mut(|state| {
		state.output.clear();
		match operation(state) {
			Ok(()) => OK,
			Err(failure) => {
				state.output = failure.message.into_bytes();
				failure.code
			}
		}
	})
}

fn input(len: usize) -> Option<*mut u8> {
	STATE.with_borrow_mut(|state| {
		state.input.clear();
		let error = if len > INPUT_LIMIT {
			format!(
				"input of {len} bytes exceeds the {} MiB limit",
				INPUT_LIMIT >> 20
			)
		} else if state.input.try_reserve_exact(len).is_err() {
			format!("out of memory for {len} bytes of input")
		} else {
			state.input.resize(len, 0);
			return Some(state.input.as_mut_ptr());
		};
		state.output = error.into_bytes();
		None
	})
}

fn output() -> (*const u8, usize) {
	STATE.with_borrow(|state| (state.output.as_ptr(), state.output.len()))
}

fn start(state: &mut State, stamp: u32) -> Result<(), Failure> {
	let zip = Zip::new(stamp)?;
	*state = State {
		zip: Some(zip),
		..State::default()
	};
	Ok(())
}

fn text<'a>(input: &'a [u8], what: &str) -> Result<&'a str, Failure> {
	str::from_utf8(input)
		.map_err(|_| Failure::from(format!("{what} is not UTF-8")))
}

fn font(state: &mut State) -> Result<(), Failure> {
	if state.session.is_some() || !state.stems.is_empty() {
		return Err(fail("fonts must be added before the first SVG"));
	}
	let data = mem::take(&mut state.input);
	let fonts = Arc::make_mut(&mut state.fonts);
	let before = fonts.len();
	fonts.load_font_data(data);
	if fonts.len() == before {
		return Err(fail("not a TrueType or OpenType font"));
	}
	let mut families = String::new();
	for face in fonts.faces().skip(before) {
		if let Some((family, _)) = face.families.first() {
			families.push_str(family);
			families.push('\n');
		}
	}
	state.output = families.into_bytes();
	Ok(())
}

fn configure(state: &mut State) -> Result<(), Failure> {
	if state.session.is_some() || !state.stems.is_empty() {
		return Err(fail("options must be set before the first SVG"));
	}
	let lines = text(&state.input, "options")?;
	let mut options = Options::default();
	let mut seen = Vec::new();
	for line in lines.lines().filter(|line| !line.trim().is_empty()) {
		let (key, value) = line.split_once('=').ok_or_else(|| {
			Failure::from(format!("option '{line}' lacks '='"))
		})?;
		let key = key.trim();
		let value = value.trim();
		if seen.contains(&key) {
			return Err(
				format!("option '{key}' is repeated").into()
			);
		}
		seen.push(key);
		match key {
			"sizes" => options.sizes = sizes(value)?,
			"background" if value.is_empty() => {
				options.background = None;
			}
			"background" => {
				options.background =
					Some(value.parse::<Color>()?);
			}
			"jpeg-quality" => {
				options.jpeg_quality = quality(value)?;
			}
			_ => {
				return Err(format!("unknown option '{key}'")
					.into());
			}
		}
	}
	state.options = options;
	Ok(())
}

fn sizes(value: &str) -> Result<Vec<u32>, Failure> {
	value.split(',')
		.map(str::trim)
		.filter(|size| !size.is_empty())
		.map(|size| {
			size.parse().map_err(|_| {
				format!("invalid size '{size}'").into()
			})
		})
		.collect()
}

fn quality(value: &str) -> Result<u8, Failure> {
	value.parse()
		.map_err(|_| format!("invalid JPEG quality '{value}'").into())
}

fn begin(state: &mut State, stem_len: usize) -> Result<(), Failure> {
	let Some(mark) = state.zip.as_ref().map(Zip::len) else {
		return Err(fail("no archive is open"));
	};
	state.session = None;
	let input = mem::take(&mut state.input);
	let (stem, svg) = input
		.split_at_checked(stem_len)
		.ok_or_else(|| fail("name length exceeds the input"))?;
	let stem = text(stem, "file name")?;
	if stem.is_empty()
		|| stem == "." || stem == ".."
		|| stem.contains(['/', '\\'])
		|| stem.contains(char::is_control)
	{
		return Err(format!("invalid file name '{stem}'").into());
	}
	let folded = stem.to_lowercase();
	if state.stems
		.iter()
		.any(|other| other.to_lowercase() == folded)
	{
		return Err(format!(
			"two SVGs are named '{stem}'; rename one of them"
		)
		.into());
	}
	let source = Source::parse(svg, Arc::clone(&state.fonts), None)?;
	state.output =
		format!("{} {}", source.width(), source.height()).into_bytes();
	state.stems.push(stem.to_owned());
	state.session = Some(Session {
		stem: stem.to_owned(),
		source,
		done: Vec::new(),
		mark,
	});
	Ok(())
}

fn step(state: &mut State) -> Result<(), Failure> {
	let format = text(&state.input, "format")?.parse::<Format>()?;
	let Some(session) = state.session.as_mut() else {
		return Err(fail("no SVG is open"));
	};
	let Some(zip) = state.zip.as_mut() else {
		return Err(fail("no archive is open"));
	};
	if session.done.contains(&format) {
		return Err(format!(
			"{} was already generated for '{}'",
			format.name(),
			session.stem
		)
		.into());
	}
	state.options.formats.clear();
	state.options.formats.push(format);
	let result = archetypon::generate(
		&session.source,
		&session.stem,
		&state.options,
	)
	.map_err(Failure::from)
	.and_then(|assets| {
		let mut manifest = String::new();
		for asset in assets {
			let path = format!("{}/{}", session.stem, asset.path);
			let _ = writeln!(
				manifest,
				"{path}\t{}",
				asset.bytes.len()
			);
			zip.add(path, asset.bytes)?;
		}
		Ok(manifest)
	});
	match result {
		Ok(manifest) => {
			session.done.push(format);
			state.output = manifest.into_bytes();
			Ok(())
		}
		Err(failure) => {
			zip.truncate(session.mark);
			state.stems.pop();
			state.session = None;
			Err(failure)
		}
	}
}

fn finish(state: &mut State) -> Result<(), Failure> {
	state.session = None;
	let zip = state.zip.take().ok_or_else(|| fail("no archive is open"))?;
	state.stems.clear();
	if zip.is_empty() {
		return Err(fail("no files were converted"));
	}
	state.output = zip.finish()?;
	Ok(())
}
