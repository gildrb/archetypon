use krilla::Document;
use krilla::geom::Size;
use krilla::page::PageSettings;
use krilla_svg::{SurfaceExt, SvgSettings};
use resvg::usvg::Tree;

use crate::Error;

pub(crate) fn encode(tree: &Tree) -> Result<Vec<u8>, Error> {
	let size = Size::from_wh(tree.size().width(), tree.size().height())
		.ok_or_else(|| Error::Pdf("invalid page size".into()))?;
	let mut document = Document::new();
	let mut page = document.start_page_with(PageSettings::new(size));
	let mut surface = page.surface();
	let drawn = surface.draw_svg(tree, size, SvgSettings::default());

	surface.finish();
	page.finish();
	drawn.ok_or_else(|| Error::Pdf("cannot draw SVG".into()))?;
	document.finish()
		.map_err(|error| Error::Pdf(format!("{error:?}")))
}
