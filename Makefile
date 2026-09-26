PREFIX ?= /usr/local
CARGO ?= cargo
NIX_SHELL ?= nix develop -c
BIN = target/release/archetypon
WASM = target/wasm32-wasip1/release/archetypon_web.wasm
WASM_FEATURES = --enable-bulk-memory --enable-nontrapping-float-to-int \
	--enable-sign-ext --enable-mutable-globals

.PHONY: all install test web clean

all:
	$(CARGO) build --release -p archetypon-cli

install:
	test -x $(BIN) || { echo 'run make first' >&2; exit 1; }
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/archetypon"

test: all
	./tests/test.sh

web:
	$(NIX_SHELL) $(CARGO) build --release --target wasm32-wasip1 \
		-p archetypon-web
	$(NIX_SHELL) wasm-opt -O3 $(WASM_FEATURES) $(WASM) \
		-o web/archetypon.wasm

clean:
	$(CARGO) clean
	rm -f web/archetypon.wasm
