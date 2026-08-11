#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)

fail() {
	printf 'test failed: %s\n' "$*" >&2
	exit 1
}

assert_status() (
	expected=$1
	shift
	if "$@" >/dev/null 2>&1; then
		actual=0
	else
		actual=$?
	fi
	if test "$actual" -ne "$expected"; then
		fail "expected exit status $expected, got $actual: $*"
	fi
)

assert_dimensions() (
	path=$1
	expected=$2
	if ! actual=$(identify -format '%wx%h' "$path"); then
		fail "ImageMagick could not decode $path"
	fi
	if test "$actual" != "$expected"; then
		fail "expected $path to be $expected, got $actual"
	fi
)

assert_pixel() (
	path=$1
	x=$2
	y=$3
	expected=$4
	format="%[fx:round(255*p{$x,$y}.r)],%[fx:round(255*p{$x,$y}.g)],%[fx:round(255*p{$x,$y}.b)],%[fx:round(255*p{$x,$y}.a)]"
	if ! actual=$(identify -format "$format" "$path"); then
		fail "ImageMagick could not sample $path at $x,$y"
	fi
	if test "$actual" != "$expected"; then
		fail "expected $path pixel $x,$y to be $expected, got $actual"
	fi
)

assert_same_image() (
	left=$1
	right=$2
	if ! compare -metric AE "$left" "$right" null: >/dev/null 2>&1; then
		fail "decoded images differ: $left and $right"
	fi
)

assert_create_fails() (
	case_directory=$1
	expected_error=$2
	if output=$(cd "$case_directory" && "$root/diopton" create input.svg 2>stderr); then
		fail "invalid input was accepted: $case_directory/input.svg"
	else
		status=$?
	fi
	if test "$status" -ne 1; then
		fail "expected invalid input status 1, got $status: $case_directory/input.svg"
	fi
	if test -n "$output"; then
		fail "invalid input wrote to stdout: $case_directory/input.svg"
	fi
	if ! grep -Fq "$expected_error" "$case_directory/stderr"; then
		fail "missing diagnostic '$expected_error': $case_directory/input.svg"
	fi
	for directory in svg png webp favicon; do
		if test -e "$case_directory/$directory"; then
			fail "invalid input created $case_directory/$directory"
		fi
	done
)

command -v identify >/dev/null 2>&1 ||
	fail "make test requires ImageMagick's identify command"
command -v compare >/dev/null 2>&1 ||
	fail "make test requires ImageMagick's compare command"

assert_status 0 "$root/diopton" --help
assert_status 2 "$root/diopton"
assert_status 2 "$root/diopton" create
assert_status 2 "$root/diopton" unknown file.svg

help_stderr=$(mktemp)
if ! help_output=$("$root/diopton" --help 2>"$help_stderr"); then
	rm -f "$help_stderr"
	fail "--help failed"
fi
expected_help='Usage: diopton create <file.svg>
       diopton --help'
test "$help_output" = "$expected_help" || fail "unexpected --help output"
test ! -s "$help_stderr" || fail "--help wrote to stderr"
rm -f "$help_stderr"

temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' 0 HUP INT TERM

cat >"$temporary/logo.svg" <<'SVG'
<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 320 160" color="#7c3aed">
  <!-- removed from the optimized copy -->
  <g transform="translate(4 4)" opacity="0.95">
    <rect width="112" height="112" rx="20" fill="#1957d2"/>
    <circle cx="56" cy="56" r="34" fill="white"/>
    <path d="M35 58 C45 30 70 30 79 58 S68 90 56 78 Q42 92 35 58Z" fill="#ff5a36"/>
  </g>
  <path d="M135 88L155 30L175 88M142 68H168M185 88V30A20 20 0 0 1 205 50" fill="none" stroke="#111827" stroke-width="8" stroke-linecap="round"/>
  <ellipse cx="235" cy="35" rx="20" ry="12" style="fill:#22c55e"/>
  <line x1="220" y1="70" x2="250" y2="70" stroke="#06b6d4" stroke-width="8"/>
  <polyline points="260,80 275,60 290,80" fill="none" stroke="#f59e0b" stroke-width="6" transform="rotate(0 275 70)"/>
  <polygon points="220,120 240,90 260,120" fill="currentColor"/>
</svg>
SVG

cat >"$temporary/expected-optimized.svg" <<'SVG'
<?xml version="1.0"?><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 320 160" color="#7c3aed"><g transform="translate(4 4)" opacity="0.95"><rect width="112" height="112" rx="20" fill="#1957d2"/><circle cx="56" cy="56" r="34" fill="white"/><path d="M35 58 C45 30 70 30 79 58 S68 90 56 78 Q42 92 35 58Z" fill="#ff5a36"/></g><path d="M135 88L155 30L175 88M142 68H168M185 88V30A20 20 0 0 1 205 50" fill="none" stroke="#111827" stroke-width="8" stroke-linecap="round"/><ellipse cx="235" cy="35" rx="20" ry="12" style="fill:#22c55e"/><line x1="220" y1="70" x2="250" y2="70" stroke="#06b6d4" stroke-width="8"/><polyline points="260,80 275,60 290,80" fill="none" stroke="#f59e0b" stroke-width="6" transform="rotate(0 275 70)"/><polygon points="220,120 240,90 260,120" fill="currentColor"/></svg>
SVG

mkdir -p "$temporary/svg" "$temporary/png" "$temporary/webp" "$temporary/favicon"
printf 'preserve me\n' >"$temporary/png/unrelated.txt"
printf 'replace me\n' >"$temporary/png/16.png"

if ! create_output=$(cd "$temporary" && "$root/diopton" create logo.svg 2>create.stderr); then
	fail "valid SVG generation failed: $(cat "$temporary/create.stderr")"
fi
test "$create_output" = 'Created SVG, PNG, WebP, and favicon assets in .' ||
	fail "unexpected create output: $create_output"
test ! -s "$temporary/create.stderr" || fail "valid generation wrote to stderr"
test "$(cat "$temporary/png/unrelated.txt")" = 'preserve me' ||
	fail "generation changed an unrelated file"

cat >"$temporary/expected-tree" <<'FILES'
favicon/favicon-16.png
favicon/favicon-32.png
favicon/favicon.ico
favicon/favicon.svg
png/1024.png
png/128.png
png/16.png
png/2048.png
png/256.png
png/32.png
png/48.png
png/512.png
png/64.png
png/unrelated.txt
svg/optimized.svg
svg/original.svg
webp/1024.webp
webp/256.webp
webp/512.webp
FILES
(
	cd "$temporary"
	find svg png webp favicon -type f -print | LC_ALL=C sort
) >"$temporary/actual-tree"
cmp "$temporary/expected-tree" "$temporary/actual-tree" >/dev/null ||
	fail "generated asset tree differs from the contract"
while IFS= read -r path; do
	test -s "$temporary/$path" || fail "missing or empty output: $path"
done <"$temporary/expected-tree"

cmp "$temporary/logo.svg" "$temporary/svg/original.svg" >/dev/null ||
	fail "svg/original.svg does not preserve the input"
cmp "$temporary/expected-optimized.svg" "$temporary/svg/optimized.svg" >/dev/null ||
	fail "svg/optimized.svg does not match the optimization contract"
cmp "$temporary/svg/optimized.svg" "$temporary/favicon/favicon.svg" >/dev/null ||
	fail "favicon.svg differs from svg/optimized.svg"
cmp "$temporary/png/16.png" "$temporary/favicon/favicon-16.png" >/dev/null ||
	fail "favicon-16.png differs from png/16.png"
cmp "$temporary/png/32.png" "$temporary/favicon/favicon-32.png" >/dev/null ||
	fail "favicon-32.png differs from png/32.png"

png_signature=$(od -An -tx1 -N8 "$temporary/png/2048.png" | tr -d ' \n')
ico_signature=$(od -An -tx1 -N4 "$temporary/favicon/favicon.ico" | tr -d ' \n')
webp_signature=$(od -An -tc -N4 "$temporary/webp/1024.webp" | tr -d ' \n')
test "$png_signature" = '89504e470d0a1a0a' || fail "invalid PNG signature"
test "$ico_signature" = '00000100' || fail "invalid ICO signature"
test "$webp_signature" = 'RIFF' || fail "invalid WebP signature"

for size in 16 32 48 64 128 256 512 1024 2048; do
	assert_dimensions "$temporary/png/$size.png" "${size}x$((size / 2))"
done
for size in 256 512 1024; do
	assert_dimensions "$temporary/webp/$size.webp" "${size}x$((size / 2))"
	assert_same_image "$temporary/png/$size.png" "$temporary/webp/$size.webp"
done
assert_dimensions "$temporary/favicon/favicon.ico[0]" '16x8'
assert_dimensions "$temporary/favicon/favicon.ico[1]" '32x16'
assert_dimensions "$temporary/favicon/favicon.ico[2]" '48x24'
assert_same_image "$temporary/png/16.png" "$temporary/favicon/favicon.ico[0]"
assert_same_image "$temporary/png/32.png" "$temporary/favicon/favicon.ico[1]"
assert_same_image "$temporary/png/48.png" "$temporary/favicon/favicon.ico[2]"

assert_pixel "$temporary/png/256.png" 1 1 '0,0,0,0'
assert_pixel "$temporary/png/256.png" 2 40 '0,0,0,0'
assert_pixel "$temporary/png/256.png" 16 16 '25,87,210,242'
assert_pixel "$temporary/png/256.png" 48 28 '244,247,253,254'
assert_pixel "$temporary/png/256.png" 48 48 '254,98,64,255'
assert_pixel "$temporary/png/256.png" 122 56 '17,24,39,255'
assert_pixel "$temporary/png/256.png" 188 28 '34,197,94,255'
assert_pixel "$temporary/png/256.png" 188 56 '6,182,212,255'
assert_pixel "$temporary/png/256.png" 220 48 '245,158,11,255'
assert_pixel "$temporary/png/256.png" 192 88 '124,58,237,255'

mkdir -p \
	"$temporary/rejections/text" \
	"$temporary/rejections/path" \
	"$temporary/rejections/nested" \
	"$temporary/rejections/paint" \
	"$temporary/rejections/missing"
cat >"$temporary/rejections/text/input.svg" <<'SVG'
<svg viewBox="0 0 10 10"><text x="1" y="5">no</text></svg>
SVG
cat >"$temporary/rejections/path/input.svg" <<'SVG'
<svg viewBox="0 0 10 10"><path d="M nope"/></svg>
SVG
cat >"$temporary/rejections/nested/input.svg" <<'SVG'
<svg viewBox="0 0 10 10"><svg viewBox="0 0 5 5"/></svg>
SVG
cat >"$temporary/rejections/paint/input.svg" <<'SVG'
<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="url(#gradient)"/></svg>
SVG

assert_create_fails "$temporary/rejections/text" 'SVG <text> is not supported'
assert_create_fails "$temporary/rejections/path" 'invalid SVG move command'
assert_create_fails "$temporary/rejections/nested" 'nested <svg> elements are not supported'
assert_create_fails "$temporary/rejections/paint" 'paint servers are not supported'
assert_create_fails "$temporary/rejections/missing" "cannot open 'input.svg'"

printf 'tests passed\n'
