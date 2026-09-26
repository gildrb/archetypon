use jpeg_encoder::{ColorType, SamplingFactor};
use oxipng::{BitDepth, RawImage};

use crate::Error;
use crate::raster::Raster;

pub(crate) const PNG_PRESET: u8 = 2;
pub(crate) const WEBP_METHOD: i32 = 4;

pub(crate) fn png(raster: &Raster) -> Result<Vec<u8>, Error> {
	let image = RawImage::new(
		raster.width,
		raster.height,
		oxipng::ColorType::RGBA,
		BitDepth::Eight,
		raster.rgba.clone(),
	)
	.map_err(Error::Png)?;
	image.create_optimized_png(&oxipng::Options::from_preset(PNG_PRESET))
		.map_err(Error::Png)
}

pub(crate) fn webp(raster: &Raster) -> Result<Vec<u8>, Error> {
	let mut config = webp::WebPConfig::new().map_err(|()| {
		Error::Webp("cannot initialize encoder config".into())
	})?;

	config.lossless = 1;
	config.quality = 100.0;
	config.method = WEBP_METHOD;
	webp::Encoder::from_rgba(&raster.rgba, raster.width, raster.height)
		.encode_advanced(&config)
		.map(|memory| memory.to_vec())
		.map_err(|error| Error::Webp(format!("{error:?}")))
}

pub(crate) fn jpeg(raster: &Raster, quality: u8) -> Result<Vec<u8>, Error> {
	let too_large = || Error::Render(raster.width.max(raster.height));
	let width = u16::try_from(raster.width).map_err(|_| too_large())?;
	let height = u16::try_from(raster.height).map_err(|_| too_large())?;
	let mut rgb = Vec::with_capacity(raster.rgba.len() / 4 * 3);
	let mut output = Vec::new();

	for pixel in raster.rgba.as_chunks::<4>().0 {
		let alpha = u32::from(pixel[3]);

		for &channel in &pixel[..3] {
			let value = (u32::from(channel) * alpha
				+ 255 * (255 - alpha) + 127) / 255;
			rgb.push(u8::try_from(value).unwrap_or(u8::MAX));
		}
	}
	let mut encoder = jpeg_encoder::Encoder::new(&mut output, quality);
	encoder.set_sampling_factor(SamplingFactor::R_4_4_4);
	encoder.encode(&rgb, width, height, ColorType::Rgb)
		.map_err(Error::Jpeg)?;
	Ok(output)
}

/// Packs square PNG images into an ICO container.
pub(crate) fn ico(edges: &[u32], pngs: &[Vec<u8>]) -> Result<Vec<u8>, Error> {
	let invalid = || Error::InvalidOption("invalid ICO entry".into());
	let count = u16::try_from(pngs.len()).map_err(|_| invalid())?;
	let header = 6 + 16 * pngs.len();
	let total = header + pngs.iter().map(Vec::len).sum::<usize>();
	let mut ico = Vec::with_capacity(total);
	let mut offset = header;

	if edges.len() != pngs.len() {
		return Err(invalid());
	}
	ico.extend_from_slice(&[0, 0, 1, 0]);
	ico.extend_from_slice(&count.to_le_bytes());
	for (&edge, png) in edges.iter().zip(pngs) {
		let side = match edge {
			1..=255 => u8::try_from(edge).map_err(|_| invalid())?,
			256 => 0,
			_ => return Err(invalid()),
		};
		let length = u32::try_from(png.len()).map_err(|_| invalid())?;
		let start = u32::try_from(offset).map_err(|_| invalid())?;

		ico.extend_from_slice(&[side, side, 0, 0, 1, 0, 32, 0]);
		ico.extend_from_slice(&length.to_le_bytes());
		ico.extend_from_slice(&start.to_le_bytes());
		offset += png.len();
	}
	for png in pngs {
		ico.extend_from_slice(png);
	}
	Ok(ico)
}
