#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
bin=$root/target/release/archetypon

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
	test "$actual" -eq "$expected" ||
		fail "expected exit status $expected, got $actual: $*"
)

assert_dimensions() (
	actual=$(identify -format '%wx%h' "$1") ||
		fail "ImageMagick could not decode $1"
	test "$actual" = "$2" || fail "expected $1 to be $2, got $actual"
)

assert_pixel() (
	p="p{$2,$3}"
	format="%[fx:round(255*$p.r)] %[fx:round(255*$p.g)]"
	format="$format %[fx:round(255*$p.b)] %[fx:round(255*$p.a)]"
	actual=$(identify -format "$format" "$1") ||
		fail "ImageMagick could not sample $1 at $2,$3"
	# 8-bit premultiplied rendering may round a channel by one.
	echo "$actual $4" | tr ',' ' ' | awk '{
		for (i = 1; i <= 4; i++) {
			d = $i - $(i + 4)
			if (d > 1 || d < -1)
				exit 1
		}
	}' || fail "expected $1 pixel $2,$3 to be $4, got $actual"
)

assert_same_image() (
	compare -metric AE "$1" "$2" null: >/dev/null 2>&1 ||
		fail "decoded images differ: $1 and $2"
)

assert_rejected() (
	directory=$1
	expected=$2
	shift 2
	before=$(find "$directory" -type d | wc -l)
	if (cd "$directory" && "$bin" "$@" >stdout 2>stderr); then
		fail "rejected input was accepted: $*"
	fi
	grep -Fq "$expected" "$directory/stderr" ||
		fail "missing diagnostic '$expected' for: $*"
	test "$(find "$directory" -type d | wc -l)" -eq "$before" ||
		fail "rejected input created a directory: $*"
)

command -v identify >/dev/null 2>&1 || fail "tests require ImageMagick"
test -x "$bin" || fail "build first: make"

assert_status 0 "$bin" --help
assert_status 0 "$bin" --version
assert_status 2 "$bin"
assert_status 2 "$bin" -f gif logo.svg
assert_status 2 "$bin" -s 0 logo.svg
assert_status 2 "$bin" -s 8193 logo.svg
assert_status 2 "$bin" -q 101 logo.svg
assert_status 2 "$bin" -b notacolor logo.svg

temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' 0 HUP INT TERM

cat >"$temporary/logo.svg" <<SVG
<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 320 160" \
color="#7c3aed">
  <!-- removed from the optimized copy -->
  <defs>
    <linearGradient id="g" x2="1">
      <stop offset="0" stop-color="#0ea5e9"/>
      <stop offset="1" stop-color="#6366f1"/>
    </linearGradient>
    <clipPath id="c"><rect x="280" width="40" height="40"/></clipPath>
  </defs>
  <g transform="translate(4 4)">
    <rect width="112" height="112" rx="20" fill="#1957d2" \
opacity="0.95"/>
    <circle cx="56" cy="56" r="34" fill="white" opacity="0.95"/>
  </g>
  <path d="M135 88L155 30L175 88M142 68H168" fill="none" \
stroke="#111827" stroke-width="8" stroke-linecap="round"/>
  <ellipse cx="235" cy="35" rx="20" ry="12" style="fill:#22c55e"/>
  <polygon points="220,120 240,90 260,120" fill="currentColor"/>
  <rect x="10" y="130" width="200" height="30" fill="url(#g)"/>
  <circle cx="300" cy="40" r="40" fill="#f59e0b" clip-path="url(#c)"/>
</svg>
SVG

mkdir -p "$temporary/logo/png"
printf 'preserve me\n' >"$temporary/logo/png/unrelated.txt"
(cd "$temporary" && "$bin" logo.svg >stdout 2>stderr) ||
	fail "generation failed: $(cat "$temporary/stderr")"
test ! -s "$temporary/stderr" || fail "generation wrote to stderr"
test "$(cat "$temporary/logo/png/unrelated.txt")" = 'preserve me' ||
	fail "generation changed an unrelated file"

sizes='16 32 48 64 128 256 512 1024 2048'
{
	printf '%s\n' favicon.ico logo.pdf logo.svg png/unrelated.txt
	for size in $sizes; do
		printf 'jpeg/logo-%s.jpg\npng/logo-%s.png\n' "$size" "$size"
		printf 'webp/logo-%s.webp\n' "$size"
	done
} | LC_ALL=C sort >"$temporary/expected-tree"
(cd "$temporary/logo" && find . -type f | sed 's|^\./||' |
	LC_ALL=C sort) >"$temporary/actual-tree"
cmp -s "$temporary/expected-tree" "$temporary/actual-tree" ||
	fail "asset tree differs: $(diff "$temporary/expected-tree" \
		"$temporary/actual-tree")"

out=$temporary/logo
for size in $sizes; do
	assert_dimensions "$out/png/logo-$size.png" "${size}x$((size / 2))"
	assert_dimensions "$out/webp/logo-$size.webp" "${size}x$((size / 2))"
	assert_dimensions "$out/jpeg/logo-$size.jpg" "${size}x$((size / 2))"
	assert_same_image "$out/png/logo-$size.png" "$out/webp/logo-$size.webp"
done
assert_dimensions "$out/favicon.ico[0]" 16x16
assert_dimensions "$out/favicon.ico[1]" 32x32
assert_dimensions "$out/favicon.ico[2]" 48x48

assert_pixel "$out/png/logo-256.png" 1 1 '0,0,0,0'
assert_pixel "$out/png/logo-256.png" 16 16 '25,87,210,242'
assert_pixel "$out/png/logo-256.png" 188 28 '34,197,94,255'
assert_pixel "$out/png/logo-256.png" 192 88 '124,58,237,255'
assert_pixel "$out/png/logo-256.png" 250 20 '245,158,11,255'
assert_pixel "$out/png/logo-256.png" 250 60 '0,0,0,0'
assert_pixel "$out/favicon.ico[2]" 0 0 '0,0,0,0'
white=$(identify -format '%[fx:p{1,1}.r>0.97&&p{1,1}.b>0.97]' \
	"$out/jpeg/logo-256.jpg")
test "$white" = 1 || fail "JPEG transparency is not flattened on white"

test "$(head -c 5 "$out/logo.pdf")" = '%PDF-' || fail "invalid PDF"
test "$(wc -c <"$out/logo.svg")" -lt "$(wc -c <"$temporary/logo.svg")" ||
	fail "optimized SVG is not smaller"
! grep -q 'removed from' "$out/logo.svg" ||
	fail "optimized SVG keeps comments"
(cd "$temporary" && "$bin" -f png -s 64 -o re logo/logo.svg \
	>/dev/null) || fail "optimized SVG does not render"
assert_same_image "$out/png/logo-64.png" "$temporary/re/logo/png/logo-64.png"

mkdir "$temporary/selected"
cp "$temporary/logo.svg" "$temporary/selected/mark.svg"
(cd "$temporary/selected" &&
	"$bin" -f png,ico -s 64 -b '#ff0000' -o dist mark.svg \
		>/dev/null) || fail "selected generation failed"
(cd "$temporary/selected/dist/mark" && find . -type f | sort |
	tr '\n' ' ') >"$temporary/selected-tree"
test "$(cat "$temporary/selected-tree")" = \
	'./favicon.ico ./png/mark-64.png ' ||
	fail "unexpected selection: $(cat "$temporary/selected-tree")"
assert_pixel "$temporary/selected/dist/mark/png/mark-64.png" \
	0 0 '255,0,0,255'

mkdir "$temporary/bad" "$temporary/twice" "$temporary/twice/a" \
	"$temporary/twice/b" "$temporary/missing"
printf '<svg viewBox="0 0 10 10"><path d="M nope"/>' \
	>"$temporary/bad/broken.svg"
printf 'not svg' >"$temporary/bad/text.svg"
cp "$temporary/logo.svg" "$temporary/twice/a/x.svg"
cp "$temporary/logo.svg" "$temporary/twice/b/x.svg"
assert_rejected "$temporary/bad" 'invalid SVG' broken.svg
assert_rejected "$temporary/bad" 'invalid SVG' text.svg
assert_rejected "$temporary/missing" 'No such file' absent.svg
assert_rejected "$temporary/twice" 'would both write to' \
	-o out a/x.svg b/x.svg

printf 'tests passed\n'
