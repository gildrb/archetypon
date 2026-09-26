use std::collections::BTreeMap;
use std::ffi::OsStr;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::Arc;
use std::time::Instant;
use std::{fs, io};

use anstyle::{AnsiColor, Style};
use archetypon::{Asset, Color, Format, Options, Source, fontdb};
use clap::Parser;
use rayon::prelude::*;

#[global_allocator]
static ALLOCATOR: mimalloc::MiMalloc = mimalloc::MiMalloc;

const BOLD: Style = Style::new().bold();
const DIM: Style = Style::new().dimmed();
const RED: Style = AnsiColor::Red.on_default().bold();
const GREEN: Style = AnsiColor::Green.on_default();

/// Turn SVG files into optimized SVG, PDF, PNG, WebP, JPEG, and ICO
/// assets. Each input gets a folder named after it, next to the input.
#[derive(Parser)]
#[command(version, max_term_width = 80)]
#[command(after_help = "Examples:
  archetypon logo.svg
  archetypon -f png,ico -s 64,512 logo.svg
  archetypon -o assets *.svg")]
struct Cli {
	/// SVG or SVGZ files
	#[arg(required = true, value_name = "SVG")]
	inputs: Vec<PathBuf>,

	/// Formats to create: svg, pdf, png, webp, jpeg, ico [default: all]
	#[arg(
		short,
		long,
		value_name = "LIST",
		value_delimiter = ',',
		value_parser = parse::<Format>
	)]
	formats: Vec<Format>,

	/// Raster sizes in pixels along the longest side
	#[arg(
		short,
		long,
		value_name = "LIST",
		value_delimiter = ',',
		default_value = "16,32,48,64,128,256,512,1024,2048",
		value_parser = clap::value_parser!(u32)
			.range(1..=i64::from(archetypon::MAX_EDGE))
	)]
	sizes: Vec<u32>,

	/// Put each input's folder in DIR instead of next to the input
	#[arg(short, long, value_name = "DIR")]
	out: Option<PathBuf>,

	/// Color behind raster output; JPEG uses white when unset
	#[arg(short, long, value_name = "COLOR", value_parser = parse::<Color>)]
	background: Option<Color>,

	/// JPEG quality
	#[arg(
		short,
		long,
		value_name = "1-100",
		default_value_t = 90,
		value_parser = clap::value_parser!(u8).range(1..=100)
	)]
	quality: u8,

	/// Load an extra font file for SVG text (repeatable)
	#[arg(long, value_name = "FILE")]
	font: Vec<PathBuf>,
}

fn parse<T: std::str::FromStr<Err = archetypon::Error>>(
	text: &str,
) -> Result<T, String> {
	text.parse()
		.map_err(|error: archetypon::Error| error.to_string())
}

struct Job {
	input: PathBuf,
	stem: String,
	directory: PathBuf,
}

fn main() -> ExitCode {
	let cli = Cli::parse();
	let started = Instant::now();
	let options = Options {
		formats: if cli.formats.is_empty() {
			Format::ALL.to_vec()
		} else {
			cli.formats.clone()
		},
		sizes: cli.sizes.clone(),
		background: cli.background,
		jpeg_quality: cli.quality,
	};
	let prepared = plan(&cli).and_then(|jobs| {
		load_fonts(&cli.font).map(|fontdb| (jobs, Arc::new(fontdb)))
	});
	let (jobs, fontdb) = match prepared {
		Ok(prepared) => prepared,
		Err(message) => {
			report_error(&message);
			return ExitCode::FAILURE;
		}
	};
	let results: Vec<_> = jobs
		.par_iter()
		.map(|job| create(job, &fontdb, &options))
		.collect();
	let mut files = 0;
	let mut bytes = 0;
	let mut failures = 0;

	for (job, result) in jobs.iter().zip(results) {
		let result = result.and_then(|created| {
			print(&listing(job, &created)).map(|()| created)
		});

		match result {
			Ok(created) => {
				files += created.outputs.len();
				bytes += created
					.outputs
					.iter()
					.map(|output| output.bytes)
					.sum::<usize>();
			}
			Err(message) => {
				failures += 1;
				report_error(&format!(
					"{}: {message}",
					job.input.display()
				));
			}
		}
	}
	if print(&summary(files, bytes, failures, started)).is_err()
		|| failures > 0
	{
		return ExitCode::FAILURE;
	}
	ExitCode::SUCCESS
}

fn plan(cli: &Cli) -> Result<Vec<Job>, String> {
	let mut seen = BTreeMap::new();

	cli.inputs
		.iter()
		.map(|input| {
			let stem = stem(input).ok_or_else(|| {
				format!("{}: not a file name", input.display())
			})?;
			let parent = match &cli.out {
				Some(out) => out.clone(),
				None => input
					.parent()
					.map(Path::to_path_buf)
					.unwrap_or_default(),
			};
			let directory = parent.join(&stem);

			if let Some(other) =
				seen.insert(directory.clone(), input)
			{
				return Err(format!(
					"{} and {} would both write to {}",
					other.display(),
					input.display(),
					directory.display()
				));
			}
			Ok(Job {
				input: input.clone(),
				stem,
				directory,
			})
		})
		.collect()
}

fn stem(input: &Path) -> Option<String> {
	let name = input.file_name().and_then(OsStr::to_str)?;
	let lower = name.to_ascii_lowercase();
	let stem = [".svgz", ".svg"]
		.iter()
		.find(|suffix| lower.ends_with(*suffix))
		.map_or(name, |suffix| &name[..name.len() - suffix.len()]);

	(!stem.is_empty()).then(|| stem.to_owned())
}

fn load_fonts(files: &[PathBuf]) -> Result<fontdb::Database, String> {
	let mut fontdb = fontdb::Database::new();

	fontdb.load_system_fonts();
	for file in files {
		fontdb.load_font_file(file).map_err(|error| {
			format!("{}: cannot load font: {error}", file.display())
		})?;
	}
	Ok(fontdb)
}

struct Output {
	format: Format,
	edge: Option<u32>,
	bytes: usize,
}

struct Created {
	input_bytes: usize,
	outputs: Vec<Output>,
}

fn create(
	job: &Job,
	fontdb: &Arc<fontdb::Database>,
	options: &Options,
) -> Result<Created, String> {
	let data = fs::read(&job.input).map_err(|error| error.to_string())?;
	let resources = job.input.parent().map(Path::to_path_buf);
	let source = Source::parse(&data, Arc::clone(fontdb), resources)
		.map_err(|error| error.to_string())?;
	let assets = archetypon::generate(&source, &job.stem, options)
		.map_err(|error| error.to_string())?;

	write_assets(&job.directory, &assets).map_err(|error| {
		format!("cannot write {}: {error}", job.directory.display())
	})?;
	Ok(Created {
		input_bytes: data.len(),
		outputs: assets
			.iter()
			.map(|asset| Output {
				format: asset.format,
				edge: asset.edge,
				bytes: asset.bytes.len(),
			})
			.collect(),
	})
}

fn write_assets(directory: &Path, assets: &[Asset]) -> io::Result<()> {
	for asset in assets {
		let path = directory.join(&asset.path);

		if let Some(parent) = path.parent() {
			fs::create_dir_all(parent)?;
		}
		fs::write(&path, &asset.bytes)?;
	}
	Ok(())
}

fn listing(job: &Job, created: &Created) -> String {
	let mut groups: BTreeMap<Format, Vec<&Output>> = BTreeMap::new();
	let mut out = format!(
		"{BOLD}{}{BOLD:#} {DIM}→{DIM:#} {}{}\n",
		job.input.display(),
		job.directory.display(),
		std::path::MAIN_SEPARATOR
	);

	for output in &created.outputs {
		groups.entry(output.format).or_default().push(output);
	}
	out.extend(groups.iter().map(|(format, group)| {
		let total = group.iter().map(|o| o.bytes).sum::<usize>();
		let detail = match format {
			Format::Svg => savings(created.input_bytes, total),
			Format::Pdf => String::new(),
			Format::Ico => join(&archetypon::ICO_SIZES),
			Format::Png | Format::Webp | Format::Jpeg => {
				let edges: Vec<u32> = group
					.iter()
					.filter_map(|o| o.edge)
					.collect();
				join(&edges)
			}
		};
		let detail = if detail.is_empty() {
			String::new()
		} else {
			format!("  {DIM}{detail}{DIM:#}")
		};

		format!(
			"  {GREEN}{:<5}{GREEN:#}{:>9}{detail}\n",
			format.name(),
			human(total)
		)
	}));
	out
}

fn savings(before: usize, after: usize) -> String {
	if before == 0 || after >= before {
		return String::new();
	}
	format!("−{}% of {}", (before - after) * 100 / before, human(before))
}

fn join(values: &[u32]) -> String {
	values.iter()
		.map(u32::to_string)
		.collect::<Vec<_>>()
		.join(" ")
}

fn human(bytes: usize) -> String {
	const UNITS: [&str; 3] = ["kB", "MB", "GB"];
	let mut value = bytes;
	let mut unit = 0;

	if bytes < 1000 {
		return format!("{bytes} B");
	}
	while value >= 1_000_000 && unit + 1 < UNITS.len() {
		value /= 1000;
		unit += 1;
	}
	format!("{}.{} {}", value / 1000, value % 1000 / 100, UNITS[unit])
}

fn summary(
	files: usize,
	bytes: usize,
	failures: usize,
	started: Instant,
) -> String {
	let seconds = started.elapsed().as_secs_f64();
	let failed = if failures > 0 {
		format!(", {RED}{failures} failed{RED:#}")
	} else {
		String::new()
	};

	format!(
		"{BOLD}{files}{BOLD:#} files, {} in {seconds:.2} s{failed}\n",
		human(bytes)
	)
}

fn print(text: &str) -> Result<(), String> {
	anstream::stdout()
		.write_all(text.as_bytes())
		.map_err(|error| format!("cannot write output: {error}"))
}

fn report_error(message: &str) {
	let line = format!("{RED}error:{RED:#} {message}\n");

	if anstream::stderr().write_all(line.as_bytes()).is_err() {
		std::process::exit(1);
	}
}
