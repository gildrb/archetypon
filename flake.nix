{
	inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

	outputs = { nixpkgs, ... }:
	let
		systems = [
			"x86_64-linux"
			"aarch64-linux"
			"x86_64-darwin"
			"aarch64-darwin"
		];
		shell = system:
		let
			pkgs = nixpkgs.legacyPackages.${system};
			llvm = pkgs.llvmPackages;
			bin = llvm.bintools-unwrapped;
			wasi = pkgs.pkgsCross.wasi32.wasilibc.dev;
		in pkgs.mkShell {
			packages = [ pkgs.binaryen pkgs.imagemagick ];
			CC_wasm32_wasip1 = "${llvm.clang-unwrapped}/bin/clang";
			AR_wasm32_wasip1 = "${bin}/bin/llvm-ar";
			CFLAGS_wasm32_wasip1 = "--target=wasm32-wasip1 "
				+ "-isystem ${wasi}/include/wasm32-wasip1";
		};
	in {
		devShells = nixpkgs.lib.genAttrs systems
			(system: { default = shell system; });
	};
}
