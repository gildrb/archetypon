#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

cat >"$temporary/logo.svg" <<'SVG'
<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 240 120">
  <!-- removed from the optimized copy -->
  <g transform="translate(4 4)" opacity="0.95">
    <rect width="112" height="112" rx="20" fill="#1957d2"/>
    <circle cx="56" cy="56" r="34" fill="white"/>
    <path d="M35 58 C45 30 70 30 79 58 S68 90 56 78 Q42 92 35 58Z" fill="#ff5a36"/>
  </g>
  <path d="M135 88L155 30L175 88M142 68H168M185 88V30A20 20 0 0 1 205 50" fill="none" stroke="#111827" stroke-width="8" stroke-linecap="round"/>
</svg>
SVG

(
	cd "$temporary"
	"$root/diopton" create logo.svg >/dev/null
)

for path in \
	svg/original.svg svg/optimized.svg \
	png/16.png png/32.png png/48.png png/64.png png/128.png \
	png/256.png png/512.png png/1024.png png/2048.png \
	webp/256.webp webp/512.webp webp/1024.webp \
	favicon/favicon.svg favicon/favicon.ico \
	favicon/favicon-16.png favicon/favicon-32.png; do
	test -s "$temporary/$path"
done

cmp "$temporary/logo.svg" "$temporary/svg/original.svg"
if grep -q 'removed from the optimized copy' "$temporary/svg/optimized.svg"; then
	echo "optimized SVG retained a comment" >&2
	exit 1
fi

png_signature=$(od -An -tx1 -N8 "$temporary/png/2048.png" | tr -d ' \n')
ico_signature=$(od -An -tx1 -N4 "$temporary/favicon/favicon.ico" | tr -d ' \n')
webp_signature=$(od -An -tc -N4 "$temporary/webp/1024.webp" | tr -d ' \n')
test "$png_signature" = "89504e470d0a1a0a"
test "$ico_signature" = "00000100"
test "$webp_signature" = "RIFF"

if command -v identify >/dev/null 2>&1; then
	dimensions=$(identify -format '%wx%h' "$temporary/png/2048.png")
	test "$dimensions" = "2048x1024"
	identify "$temporary/webp/1024.webp" "$temporary/favicon/favicon.ico" >/dev/null
fi

cat >"$temporary/unsupported.svg" <<'SVG'
<svg viewBox="0 0 10 10"><text x="1" y="5">no</text></svg>
SVG
if (cd "$temporary" && "$root/diopton" create unsupported.svg >/dev/null 2>&1); then
	echo "unsupported SVG text was accepted" >&2
	exit 1
fi

printf 'tests passed\n'
