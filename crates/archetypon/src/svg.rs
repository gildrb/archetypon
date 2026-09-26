use std::sync::Arc;

use oxvg_ast::parse::roxmltree::{ParsingOptions, parse_with_options};
use oxvg_ast::serialize::Node as _;
use oxvg_ast::visitor::Info;
use oxvg_optimiser::Jobs;
use resvg::usvg::{self, roxmltree};

use crate::{Error, Source};

const SVG_NAMESPACE: &str = "http://www.w3.org/2000/svg";
const SIZE_TOLERANCE: f32 = 1e-3;

pub(crate) fn has_text(text: &str) -> Result<bool, Error> {
	let options = roxmltree::ParsingOptions {
		allow_dtd: true,
		..roxmltree::ParsingOptions::default()
	};
	let document = roxmltree::Document::parse_with_options(text, options)
		.map_err(|error| {
		Error::Parse(usvg::Error::ParsingFailed(error))
	})?;

	Ok(document
		.descendants()
		.any(|node| node.has_tag_name((SVG_NAMESPACE, "text"))))
}

/// Minifies the source and proves the result still parses to the same
/// canvas size.
pub(crate) fn optimize(source: &Source) -> Result<Vec<u8>, Error> {
	let options = ParsingOptions {
		allow_dtd: true,
		..ParsingOptions::default()
	};
	let optimized =
		parse_with_options(&source.text, options, |dom, arena| {
			let info = Info::new(arena);

			Jobs::default().run(dom, &info).map_err(|error| {
				Error::Optimize(error.to_string())
			})?;
			dom.serialize().map_err(|error| {
				Error::Optimize(error.to_string())
			})
		})
		.map_err(|error| Error::Optimize(error.to_string()))??;
	let options = usvg::Options {
		fontdb: Arc::clone(&source.fontdb),
		..usvg::Options::default()
	};
	let tree = usvg::Tree::from_str(&optimized, &options)
		.map_err(|error| Error::Optimize(error.to_string()))?;
	let (before, after) = (source.tree.size(), tree.size());

	if (before.width() - after.width()).abs()
		> SIZE_TOLERANCE * before.width()
		|| (before.height() - after.height()).abs()
			> SIZE_TOLERANCE * before.height()
	{
		return Err(Error::Optimize("canvas size changed".into()));
	}
	Ok(optimized.into_bytes())
}
