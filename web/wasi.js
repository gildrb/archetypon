// The WASI preview 1 imports archetypon.wasm needs, for a module that
// only computes: no files, no environment, output to the console.

const SUCCESS = 0;
const EBADF = 8;
const EINVAL = 28;
const ENOSYS = 52;

const REALTIME = 0;
const MONOTONIC = 1;
const STDOUT = 1;
const STDERR = 2;

const UNSUPPORTED = [
	"fd_close",
	"fd_fdstat_get",
	"fd_filestat_get",
	"fd_prestat_dir_name",
	"fd_read",
	"path_filestat_get",
	"path_open",
];

export class WasiExit extends Error {
	constructor(code) {
		super(`exited with code ${code}`);
		this.code = code;
	}
}

export class Wasi {
	#memory = null;
	#decoders = [new TextDecoder(), new TextDecoder()];
	#pending = ["", ""];

	bind(memory) {
		this.#memory = memory;
	}

	imports() {
		const calls = {
			clock_time_get: this.#clockTimeGet.bind(this),
			environ_get: () => SUCCESS,
			environ_sizes_get: this.#environSizesGet.bind(this),
			// EBADF ends wasi-libc's scan for preopens.
			fd_prestat_get: () => EBADF,
			fd_write: this.#fdWrite.bind(this),
			proc_exit: (code) => {
				throw new WasiExit(code);
			},
			random_get: this.#randomGet.bind(this),
			sched_yield: () => SUCCESS,
		};
		for (const name of UNSUPPORTED) {
			calls[name] = () => ENOSYS;
		}
		return { wasi_snapshot_preview1: calls };
	}

	#view() {
		return new DataView(this.#memory.buffer);
	}

	#bytes(ptr, len) {
		const buffer = this.#memory.buffer;
		return new Uint8Array(buffer, ptr >>> 0, len >>> 0);
	}

	#clockTimeGet(id, _precision, time) {
		let nanos;
		if (id === REALTIME) {
			nanos = BigInt(Date.now()) * 1_000_000n;
		} else if (id === MONOTONIC) {
			nanos = BigInt(Math.round(performance.now() * 1e6));
		} else {
			return EINVAL;
		}
		this.#view().setBigUint64(time, nanos, true);
		return SUCCESS;
	}

	#environSizesGet(count, size) {
		const view = this.#view();
		view.setUint32(count, 0, true);
		view.setUint32(size, 0, true);
		return SUCCESS;
	}

	#fdWrite(fd, iovs, count, written) {
		if (fd !== STDOUT && fd !== STDERR) {
			return EBADF;
		}
		const view = this.#view();
		let total = 0;
		for (let i = 0; i < count; i += 1) {
			const ptr = view.getUint32(iovs + i * 8, true);
			const len = view.getUint32(iovs + i * 8 + 4, true);
			this.#print(fd, this.#bytes(ptr, len));
			total += len;
		}
		view.setUint32(written, total, true);
		return SUCCESS;
	}

	#print(fd, bytes) {
		const index = fd - STDOUT;
		const decoder = this.#decoders[index];
		const text = decoder.decode(bytes, { stream: true });
		const lines = (this.#pending[index] + text).split("\n");
		this.#pending[index] = lines.pop();
		const log = fd === STDOUT ? console.log : console.error;
		for (const line of lines) {
			log(line);
		}
	}

	#randomGet(ptr, len) {
		const chunk = 65536;
		for (let done = 0; done < len; done += chunk) {
			const size = Math.min(chunk, len - done);
			crypto.getRandomValues(this.#bytes(ptr + done, size));
		}
		return SUCCESS;
	}
}
