import { Wasi } from "./wasi.js";

const compiled = WebAssembly.compileStreaming(fetch("archetypon.wasm"));
const encoder = new TextEncoder();
const decoder = new TextDecoder();

const OK = 0;

class Failure extends Error {
	constructor(code, message) {
		super(message);
		this.code = code;
	}
}

class Engine {
	static async create() {
		const wasi = new Wasi();
		const instance = await WebAssembly.instantiate(await compiled,
			wasi.imports());
		wasi.bind(instance.exports.memory);
		instance.exports._initialize();
		return new Engine(instance.exports);
	}

	constructor(exports) {
		this.exports = exports;
	}

	output() {
		const ptr = this.exports.output_ptr() >>> 0;
		const len = this.exports.output_len() >>> 0;
		return new Uint8Array(this.exports.memory.buffer, ptr, len);
	}

	text() {
		return decoder.decode(this.output());
	}

	lines() {
		return this.text().split("\n").filter(Boolean);
	}

	input(bytes) {
		const ptr = this.exports.input(bytes.length) >>> 0;
		if (ptr === 0) {
			throw new Failure(1, this.text());
		}
		new Uint8Array(this.exports.memory.buffer, ptr, bytes.length)
			.set(bytes);
	}

	call(name, ...args) {
		const code = this.exports[name](...args);
		if (code !== OK) {
			throw new Failure(code, this.text());
		}
	}
}

function dosTime(date) {
	const year = Math.min(Math.max(date.getFullYear(), 1980), 2107);
	return ((year - 1980) << 25 | (date.getMonth() + 1) << 21 |
		date.getDate() << 16 | date.getHours() << 11 |
		date.getMinutes() << 5 | date.getSeconds() >> 1) >>> 0;
}

function entry(line) {
	const [path, size] = line.split("\t");
	return { path, size: Number(size) };
}

function concat(first, second) {
	const bytes = new Uint8Array(first.length + second.length);
	bytes.set(first);
	bytes.set(second, first.length);
	return bytes;
}

async function run({ svgs, fonts, formats, options }) {
	const started = performance.now();
	const engine = await Engine.create();
	engine.call("start", dosTime(new Date()));
	for (const font of fonts) {
		try {
			engine.input(new Uint8Array(font.data));
			engine.call("font");
			const families = [...new Set(engine.lines())];
			const name = font.name;
			postMessage({ type: "font", name, families });
		} catch (error) {
			if (!(error instanceof Failure)) {
				throw error;
			}
			postMessage({
				type: "font-error",
				name: font.name,
				message: error.message,
			});
		}
	}
	engine.input(encoder.encode(options));
	engine.call("configure");
	let converted = 0;
	for (const [index, svg] of svgs.entries()) {
		try {
			const stem = encoder.encode(svg.stem);
			engine.input(concat(stem, new Uint8Array(svg.data)));
			engine.call("begin", stem.length);
			const [width, height] = engine.text().split(" ")
				.map(Number);
			postMessage({ type: "open", index, width, height });
			for (const format of formats) {
				postMessage({ type: "step", index, format });
				engine.input(encoder.encode(format));
				engine.call("step");
				const files = engine.lines().map(entry);
				postMessage({ type: "stepped", index, format,
					files });
			}
			converted += 1;
			postMessage({ type: "converted", index });
		} catch (error) {
			if (!(error instanceof Failure)) {
				throw error;
			}
			postMessage({
				type: "failed",
				index,
				code: error.code,
				message: error.message,
			});
		}
	}
	if (converted === 0) {
		postMessage({ type: "done", zip: null, ms: 0 });
		return;
	}
	engine.call("finish");
	const zip = engine.output().slice().buffer;
	const ms = performance.now() - started;
	postMessage({ type: "done", zip, ms }, [zip]);
}

onmessage = async ({ data }) => {
	try {
		await run(data);
	} catch (error) {
		const message = error instanceof Failure ? error.message :
			`the converter stopped unexpectedly (${error.message})`;
		postMessage({ type: "error", message });
	}
};
