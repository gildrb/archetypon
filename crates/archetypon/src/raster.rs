use resvg::tiny_skia::{self, Pixmap, Transform};
use resvg::usvg::Tree;

use crate::{Color, Error};

pub(crate) struct Raster {
	pub width: u32,
	pub height: u32,
	pub rgba: Vec<u8>,
}

/// Renders `tree` with its longest side at `edge` pixels. `square`
/// centers the drawing on an `edge` by `edge` canvas.
pub(crate) fn render(
	tree: &Tree,
	edge: u32,
	background: Option<Color>,
	square: bool,
) -> Result<Raster, Error> {
	let edge16 = u16::try_from(edge).map_err(|_| Error::Render(edge))?;
	let size = tree.size();
	let longest = size.width().max(size.height());
	let width = fit(size.width(), longest, edge16);
	let height = fit(size.height(), longest, edge16);
	let (canvas_width, canvas_height) = if square {
		(edge16, edge16)
	} else {
		(width, height)
	};
	let mut pixmap =
		Pixmap::new(u32::from(canvas_width), u32::from(canvas_height))
			.ok_or(Error::Render(edge))?;
	let transform = Transform::from_row(
		f32::from(width) / size.width(),
		0.0,
		0.0,
		f32::from(height) / size.height(),
		f32::from((canvas_width - width) / 2),
		f32::from((canvas_height - height) / 2),
	);

	if let Some(color) = background {
		pixmap.fill(tiny_skia::Color::from_rgba8(
			color.red,
			color.green,
			color.blue,
			color.alpha,
		));
	}
	resvg::render(tree, transform, &mut pixmap.as_mut());
	let mut rgba = pixmap.take();
	for pixel in rgba.as_chunks_mut::<4>().0 {
		let alpha = u32::from(pixel[3]);

		if alpha == 0 || alpha == 255 {
			continue;
		}
		for channel in &mut pixel[..3] {
			let value =
				(u32::from(*channel) * 255 + alpha / 2) / alpha;
			*channel =
				u8::try_from(value.min(255)).unwrap_or(u8::MAX);
		}
	}
	Ok(Raster {
		width: u32::from(canvas_width),
		height: u32::from(canvas_height),
		rgba,
	})
}

#[expect(
	clippy::cast_possible_truncation,
	clippy::cast_sign_loss,
	reason = "the value is clamped to 1..=edge before the cast"
)]
fn fit(side: f32, longest: f32, edge: u16) -> u16 {
	let scaled = (side * f32::from(edge) / longest).round();

	scaled.clamp(1.0, f32::from(edge)) as u16
}
