//! Links the WASI reactor startup object so the module exports
//! `_initialize` instead of wrapping every export as a command.

use std::env;
use std::error::Error;
use std::path::PathBuf;
use std::process::Command;

fn main() -> Result<(), Box<dyn Error>> {
	println!("cargo::rerun-if-changed=build.rs");
	let target = env::var("TARGET")?;
	if target != "wasm32-wasip1" {
		return Ok(());
	}
	let output = Command::new(env::var("RUSTC")?)
		.args(["--print", "sysroot"])
		.output()?;
	if !output.status.success() {
		return Err("rustc --print sysroot failed".into());
	}
	let sysroot = String::from_utf8(output.stdout)?;
	let crt: PathBuf = [
		sysroot.trim(),
		"lib",
		"rustlib",
		&target,
		"lib",
		"self-contained",
		"crt1-reactor.o",
	]
	.iter()
	.collect();
	if !crt.is_file() {
		return Err(format!("missing {}", crt.display()).into());
	}
	println!("cargo::rustc-cdylib-link-arg={}", crt.display());
	Ok(())
}
