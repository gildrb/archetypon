mod encode;
mod pdf;
mod raster;
mod svg;

use std::fmt;
use std::path::PathBuf;
use std::str::FromStr;
use std::sync::Arc;

use resvg::usvg;
pub use resvg::usvg::fontdb;

use crate::raster::Raster;

pub const MAX_EDGE: u32 = 8192;
pub const DEFAULT_SIZES: [u32; 9] = [16, 32, 48, 64, 128, 256, 512, 1024, 2048];
pub const ICO_SIZES: [u32; 3] = [16, 32, 48];
pub const INPUT_LIMIT: usize = 32 * 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Format {
	Svg,
	Pdf,
	Png,
	Webp,
	Jpeg,
	Ico,
}

impl Format {
	pub const ALL: [Self; 6] = [
		Self::Svg,
		Self::Pdf,
		Self::Png,
		Self::Webp,
		Self::Jpeg,
		Self::Ico,
	];

	#[must_use]
	pub fn name(self) -> &'static str {
		match self {
			Self::Svg => "svg",
			Self::Pdf => "pdf",
			Self::Png => "png",
			Self::Webp => "webp",
			Self::Jpeg => "jpeg",
			Self::Ico => "ico",
		}
	}

	#[must_use]
	pub fn is_raster(self) -> bool {
		matches!(self, Self::Png | Self::Webp | Self::Jpeg)
	}
}

impl FromStr for Format {
	type Err = Error;

	fn from_str(name: &str) -> Result<Self, Error> {
		match name.trim().to_ascii_lowercase().as_str() {
			"svg" => Ok(Self::Svg),
			"pdf" => Ok(Self::Pdf),
			"png" => Ok(Self::Png),
			"webp" => Ok(Self::Webp),
			"jpeg" | "jpg" => Ok(Self::Jpeg),
			"ico" => Ok(Self::Ico),
			_ => Err(Error::InvalidOption(format!(
				"unknown format '{name}'"
			))),
		}
	}
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Color {
	pub red: u8,
	pub green: u8,
	pub blue: u8,
	pub alpha: u8,
}

impl FromStr for Color {
	type Err = Error;

	fn from_str(text: &str) -> Result<Self, Error> {
		let color = svgtypes::Color::from_str(text.trim()).map_err(
			|error| {
				Error::InvalidOption(format!(
					"invalid color '{text}': {error}"
				))
			},
		)?;
		Ok(Self {
			red: color.red,
			green: color.green,
			blue: color.blue,
			alpha: color.alpha,
		})
	}
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Options {
	pub formats: Vec<Format>,
	pub sizes: Vec<u32>,
	pub background: Option<Color>,
	pub jpeg_quality: u8,
}

impl Default for Options {
	fn default() -> Self {
		Self {
			formats: Format::ALL.to_vec(),
			sizes: DEFAULT_SIZES.to_vec(),
			background: None,
			jpeg_quality: 90,
		}
	}
}

impl Options {
	fn normalized(&self) -> Result<Self, Error> {
		let mut formats = self.formats.clone();
		let mut sizes = self.sizes.clone();

		formats.sort_unstable();
		formats.dedup();
		sizes.sort_unstable();
		sizes.dedup();
		if formats.is_empty() {
			return Err(Error::InvalidOption(
				"no output formats selected".into(),
			));
		}
		if formats.iter().any(|format| format.is_raster())
			&& sizes.is_empty()
		{
			return Err(Error::InvalidOption(
				"no raster sizes selected".into(),
			));
		}
		if let Some(size) = sizes
			.iter()
			.find(|size| !(1..=MAX_EDGE).contains(*size))
		{
			return Err(Error::InvalidOption(format!(
				"size {size} is outside 1..={MAX_EDGE}"
			)));
		}
		if !(1..=100).contains(&self.jpeg_quality) {
			return Err(Error::InvalidOption(format!(
				"JPEG quality {} is outside 1..=100",
				self.jpeg_quality
			)));
		}
		Ok(Self {
			formats,
			sizes,
			background: self.background,
			jpeg_quality: self.jpeg_quality,
		})
	}
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Asset {
	pub format: Format,
	pub path: String,
	pub edge: Option<u32>,
	pub bytes: Vec<u8>,
}

pub struct Source {
	text: String,
	tree: usvg::Tree,
	fontdb: Arc<fontdb::Database>,
}

impl Source {
	/// Parses SVG or SVGZ data. `resources` resolves relative image
	/// references; `None` leaves them unresolved.
	///
	/// # Errors
	/// Oversized, non-UTF-8, or invalid SVG input, or text without fonts.
	pub fn parse(
		data: &[u8],
		fontdb: Arc<fontdb::Database>,
		resources: Option<PathBuf>,
	) -> Result<Self, Error> {
		if data.len() > INPUT_LIMIT {
			return Err(Error::TooLarge);
		}
		let bytes = if data.starts_with(&[0x1f, 0x8b]) {
			usvg::decompress_svgz(data).map_err(Error::Parse)?
		} else {
			data.to_vec()
		};
		if bytes.len() > INPUT_LIMIT {
			return Err(Error::TooLarge);
		}
		let text = String::from_utf8(bytes)
			.map_err(|_| Error::Encoding)?;
		if fontdb.is_empty() && svg::has_text(&text)? {
			return Err(Error::NoFonts);
		}
		let options = usvg::Options {
			resources_dir: resources,
			fontdb: Arc::clone(&fontdb),
			..usvg::Options::default()
		};
		let tree = usvg::Tree::from_str(&text, &options)
			.map_err(Error::Parse)?;
		Ok(Self { text, tree, fontdb })
	}

	#[must_use]
	pub fn width(&self) -> f32 {
		self.tree.size().width()
	}

	#[must_use]
	pub fn height(&self) -> f32 {
		self.tree.size().height()
	}
}

#[derive(Clone, Copy)]
enum Job<'a> {
	Svg,
	Pdf,
	Ico,
	Raster(Format, &'a Raster),
}

/// Generates every selected asset in memory. Paths are relative and
/// use `/`; `stem` names the SVG and PDF outputs.
///
/// # Errors
/// Invalid stem or options, or a failed render or encode.
pub fn generate(
	source: &Source,
	stem: &str,
	options: &Options,
) -> Result<Vec<Asset>, Error> {
	validate_stem(stem)?;
	let options = options.normalized()?;
	let background = options.background;
	let raster_sizes = if options.formats.iter().any(|f| f.is_raster()) {
		options.sizes.clone()
	} else {
		Vec::new()
	};
	let rasters = map_all(raster_sizes, |edge| {
		raster::render(&source.tree, edge, background, false)
	})
	.into_iter()
	.collect::<Result<Vec<_>, _>>()?;
	let mut jobs = Vec::new();

	for &format in &options.formats {
		match format {
			Format::Svg => jobs.push(Job::Svg),
			Format::Pdf => jobs.push(Job::Pdf),
			Format::Ico => jobs.push(Job::Ico),
			Format::Png | Format::Webp | Format::Jpeg => jobs
				.extend(rasters
					.iter()
					.map(|r| Job::Raster(format, r))),
		}
	}
	map_all(jobs, |job| run(source, stem, &options, job))
		.into_iter()
		.collect()
}

fn run(
	source: &Source,
	stem: &str,
	options: &Options,
	job: Job<'_>,
) -> Result<Asset, Error> {
	let (format, path, edge, bytes) = match job {
		Job::Svg => (
			Format::Svg,
			format!("{stem}.svg"),
			None,
			svg::optimize(source)?,
		),
		Job::Pdf => (
			Format::Pdf,
			format!("{stem}.pdf"),
			None,
			pdf::encode(&source.tree)?,
		),
		Job::Ico => (
			Format::Ico,
			"favicon.ico".into(),
			None,
			ico(source, options.background)?,
		),
		Job::Raster(format, raster) => {
			let edge = raster.width.max(raster.height);
			let bytes = match format {
				Format::Png => encode::png(raster)?,
				Format::Webp => encode::webp(raster)?,
				_ => encode::jpeg(
					raster,
					options.jpeg_quality,
				)?,
			};
			let extension = match format {
				Format::Jpeg => "jpg",
				_ => format.name(),
			};
			(
				format,
				format!(
					"{}/{stem}-{edge}.{extension}",
					format.name()
				),
				Some(edge),
				bytes,
			)
		}
	};
	Ok(Asset {
		format,
		path,
		edge,
		bytes,
	})
}

fn ico(source: &Source, background: Option<Color>) -> Result<Vec<u8>, Error> {
	let pngs = ICO_SIZES
		.iter()
		.map(|&edge| {
			raster::render(&source.tree, edge, background, true)
				.and_then(|raster| encode::png(&raster))
		})
		.collect::<Result<Vec<_>, _>>()?;
	encode::ico(&ICO_SIZES, &pngs)
}

fn validate_stem(stem: &str) -> Result<(), Error> {
	if stem.is_empty()
		|| stem == "." || stem == ".."
		|| stem.contains(['/', '\\', '\0'])
	{
		return Err(Error::InvalidOption(format!(
			"invalid output name '{stem}'"
		)));
	}
	Ok(())
}

fn map_all<T, R, F>(items: Vec<T>, f: F) -> Vec<R>
where
	T: Send,
	R: Send,
	F: Fn(T) -> R + Sync + Send,
{
	#[cfg(feature = "parallel")]
	{
		use rayon::prelude::*;
		items.into_par_iter().map(f).collect()
	}
	#[cfg(not(feature = "parallel"))]
	{
		items.into_iter().map(f).collect()
	}
}

#[derive(Debug)]
#[non_exhaustive]
pub enum Error {
	TooLarge,
	Encoding,
	NoFonts,
	Parse(usvg::Error),
	Optimize(String),
	Render(u32),
	Png(oxipng::PngError),
	Webp(String),
	Jpeg(jpeg_encoder::EncodingError),
	Pdf(String),
	InvalidOption(String),
}

impl fmt::Display for Error {
	fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
		match self {
			Self::TooLarge => write!(
				f,
				"SVG exceeds the {} MiB limit",
				INPUT_LIMIT >> 20
			),
			Self::Encoding => f.write_str("SVG is not valid UTF-8"),
			Self::NoFonts => f.write_str(
				"SVG contains text but no fonts are loaded",
			),
			Self::Parse(error) => write!(f, "invalid SVG: {error}"),
			Self::Optimize(message) => {
				write!(f, "SVG optimization failed: {message}")
			}
			Self::Render(edge) => {
				write!(f, "cannot render at {edge} px")
			}
			Self::Png(error) => {
				write!(f, "PNG encoding failed: {error}")
			}
			Self::Webp(message) => {
				write!(f, "WebP encoding failed: {message}")
			}
			Self::Jpeg(error) => {
				write!(f, "JPEG encoding failed: {error}")
			}
			Self::Pdf(message) => {
				write!(f, "PDF encoding failed: {message}")
			}
			Self::InvalidOption(message) => f.write_str(message),
		}
	}
}

impl std::error::Error for Error {
	fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
		match self {
			Self::Parse(error) => Some(error),
			Self::Png(error) => Some(error),
			Self::Jpeg(error) => Some(error),
			_ => None,
		}
	}
}
