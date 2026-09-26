//! WebAssembly exports. Operations return 0 on success, 1 on failure,
//! and 2 when an SVG with text needs fonts; the output buffer then
//! holds the message.
#![expect(unsafe_code, reason = "exports need #[unsafe(no_mangle)]")]

use std::ptr;

/// Resizes the input buffer to `len` bytes for JS to fill; null when
/// too large or out of memory, with the message in the output buffer.
#[unsafe(no_mangle)]
pub extern "C" fn input(len: usize) -> *mut u8 {
	crate::input(len).unwrap_or(ptr::null_mut())
}

#[unsafe(no_mangle)]
pub extern "C" fn output_ptr() -> *const u8 {
	crate::output().0
}

#[unsafe(no_mangle)]
pub extern "C" fn output_len() -> usize {
	crate::output().1
}

/// Opens an empty archive stamped with an MS-DOS date and time,
/// discarding fonts, options, and any previous archive.
#[unsafe(no_mangle)]
pub extern "C" fn start(stamp: u32) -> u32 {
	crate::call(|state| crate::start(state, stamp))
}

/// Loads the input as a font file; outputs its family names.
#[unsafe(no_mangle)]
pub extern "C" fn font() -> u32 {
	crate::call(crate::font)
}

/// Sets options from `key=value` input lines: `sizes` (comma-separated
/// edges), `background` (CSS color, empty for none), `jpeg-quality`.
#[unsafe(no_mangle)]
pub extern "C" fn configure() -> u32 {
	crate::call(crate::configure)
}

/// Parses an SVG from input holding `stem_len` bytes of UTF-8 name,
/// then the SVG or SVGZ data; outputs "width height".
#[unsafe(no_mangle)]
pub extern "C" fn begin(stem_len: usize) -> u32 {
	crate::call(|state| crate::begin(state, stem_len))
}

/// Generates the format named by the input for the open SVG and adds
/// it to the archive; outputs "path\tsize" lines. A failure removes
/// the SVG from the archive.
#[unsafe(no_mangle)]
pub extern "C" fn step() -> u32 {
	crate::call(crate::step)
}

/// Closes the archive and outputs the ZIP bytes.
#[unsafe(no_mangle)]
pub extern "C" fn finish() -> u32 {
	crate::call(crate::finish)
}
