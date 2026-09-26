const $ = (id) => document.getElementById(id);

const NO_FONTS = 2;
const MAX_EDGE = 8192;
const LABELS = {
	svg: "SVG",
	pdf: "PDF",
	png: "PNG",
	webp: "WebP",
	jpeg: "JPEG",
	ico: "ICO",
};

const svgs = [];
const fonts = [];
const urls = [];
let worker = null;
let dragDepth = 0;

function kind(name) {
	const lower = name.toLowerCase();
	if (/\.svgz?$/.test(lower)) {
		return "svg";
	}
	if (/\.(ttf|otf|ttc)$/.test(lower)) {
		return "font";
	}
	return null;
}

function stemOf(name) {
	return name.replace(/\.svgz?$/i, "");
}

function count(n, noun) {
	return `${n} ${noun}${n === 1 ? "" : "s"}`;
}

function formatBytes(bytes) {
	if (bytes < 1000) {
		return `${bytes} B`;
	}
	const units = ["kB", "MB", "GB"];
	let value = bytes / 1000;
	let unit = 0;
	while (value >= 1000 && unit < units.length - 1) {
		value /= 1000;
		unit += 1;
	}
	const digits = value < 10 ? 1 : 0;
	return `${value.toFixed(digits)} ${units[unit]}`;
}

function element(tag, props = {}, ...children) {
	const node = document.createElement(tag);
	Object.assign(node, props);
	node.append(...children);
	return node;
}

function setStatus(message, error = false) {
	const status = $("status");
	status.textContent = message;
	status.classList.toggle("error", error);
}

function upsert(list, file) {
	const index = list.findIndex((item) => item.name === file.name);
	if (index === -1) {
		list.push(file);
	} else {
		list[index] = file;
	}
}

function add(files) {
	const ignored = [];
	for (const file of files) {
		const type = kind(file.name);
		if (type === "svg") {
			upsert(svgs, file);
		} else if (type === "font") {
			upsert(fonts, file);
		} else {
			ignored.push(file.name);
		}
	}
	updateButtons();
	if (svgs.length > 0) {
		convert(ignored);
	} else if (ignored.length > 0) {
		setStatus(`Ignored ${ignored.join(", ")}: only SVG, SVGZ, ` +
			"TTF, OTF, and TTC files are supported.", true);
	} else if (fonts.length > 0) {
		setStatus(`${count(fonts.length, "font file")} ready. ` +
			"Add an SVG.");
	}
}

function updateButtons() {
	$("convert").disabled = svgs.length === 0;
	$("clear").disabled = svgs.length === 0 && fonts.length === 0;
}

function readOptions() {
	const form = $("options");
	const formats = [...form.querySelectorAll("[name=format]:checked")]
		.map((input) => input.value);
	const sizesInput = $("sizes");
	const qualityInput = $("quality");
	const raster = formats.some((f) => ["png", "webp", "jpeg"]
		.includes(f));
	const errors = [];
	const sizes = [];

	for (const part of sizesInput.value.split(/[\s,]+/)) {
		if (part === "") {
			continue;
		}
		const size = Number(part);
		if (!/^\d+$/.test(part) || size < 1 || size > MAX_EDGE) {
			errors.push(`“${part}” is not a size between 1 and ` +
				`${MAX_EDGE}.`);
			break;
		}
		sizes.push(size);
	}
	if (raster && sizes.length === 0 && errors.length === 0) {
		errors.push("Enter at least one size for PNG, WebP, or JPEG.");
	}
	sizesInput.setAttribute("aria-invalid", String(errors.length > 0));

	const quality = qualityInput.value.trim();
	const qualityValid = /^\d+$/.test(quality) &&
		Number(quality) >= 1 && Number(quality) <= 100;
	qualityInput.setAttribute("aria-invalid", String(!qualityValid));
	if (!qualityValid) {
		errors.push("JPEG quality must be a whole number from 1 " +
			"to 100.");
	}
	if (formats.length === 0) {
		errors.push("Select at least one format.");
	}
	const background = $("background").value.trim();
	const options = `sizes=${sizes.join(",")}\n` +
		`background=${background}\njpeg-quality=${quality}\n`;
	return { errors, formats, options };
}

async function previewUrl(file) {
	let blob = file;
	if (/\.svgz$/i.test(file.name)) {
		const stream = file.stream()
			.pipeThrough(new DecompressionStream("gzip"));
		blob = await new Response(stream).blob();
	}
	const url = URL.createObjectURL(
		new Blob([blob], { type: "image/svg+xml" }));
	urls.push(url);
	return url;
}

function releaseUrls() {
	for (const url of urls.splice(0)) {
		URL.revokeObjectURL(url);
	}
}

function resultItem(file, formats) {
	const preview = element("img", {
		className: "preview",
		alt: `Preview of ${file.name}`,
	});
	previewUrl(file).then((url) => {
		preview.src = url;
	}, () => {
		preview.alt = `No preview for ${file.name}`;
	});
	const steps = element("ol", {
		className: "steps",
		ariaLabel: "Formats",
	});
	const chips = new Map();
	for (const format of formats) {
		const chip = element("li", {}, LABELS[format]);
		chip.dataset.state = "pending";
		chips.set(format, chip);
		steps.append(chip);
	}
	const list = element("ul", { className: "files" });
	const count = element("span", {}, "Files");
	const details = element("details", { hidden: true },
		element("summary", {}, count), list);
	const meta = element("p", { className: "meta" }, "Waiting");
	const error = element("p", { className: "error", hidden: true });
	const item = element("li", { className: "result" }, preview,
		element("div", {}, element("h3", {}, file.name), meta, steps,
			error, details));
	return { item, meta, chips, list, count, details, error, files: 0,
		bytes: 0, size: "" };
}

function fileItem(file) {
	return element("li", {}, element("span", {}, file.path),
		element("span", {}, formatBytes(file.size)));
}

function failMessage(name, code, message) {
	if (code === NO_FONTS) {
		return `${name} contains text, but no fonts were provided. ` +
			"Drop the font files it uses (.ttf, .otf, or .ttc) " +
			"here, or convert its text to outlines.";
	}
	return `${name}: ${message}`;
}

async function convert(ignored = []) {
	if (svgs.length === 0) {
		return;
	}
	const { errors, formats, options } = readOptions();
	if (errors.length > 0) {
		setStatus(errors.join(" "), true);
		return;
	}
	worker?.terminate();
	releaseUrls();

	const download = $("download");
	download.hidden = true;
	download.removeAttribute("href");
	const fontList = $("fonts");
	fontList.replaceChildren();
	const results = $("results");
	const items = svgs.map((file) => resultItem(file, formats));
	results.replaceChildren(...items.map((entry) => entry.item));
	$("output").hidden = false;

	const note = ignored.length > 0
		? ` Ignored unsupported ${ignored.join(", ")}.` : "";
	setStatus(`Converting ${count(svgs.length, "SVG")}…${note}`);

	const current = new Worker("worker.js", { type: "module" });
	worker = current;
	const svgData = await Promise.all(svgs.map(async (file) => ({
		stem: stemOf(file.name),
		data: await file.arrayBuffer(),
	})));
	const fontData = await Promise.all(fonts.map(async (file) => ({
		name: file.name,
		data: await file.arrayBuffer(),
	})));
	if (worker !== current) {
		return;
	}
	const failures = [];

	current.onmessage = ({ data }) => {
		const entry = items[data.index];
		switch (data.type) {
		case "font":
			fontList.append(element("li", {},
				data.families.join(", ") || data.name));
			break;
		case "font-error":
			failures.push(`${data.name}: ${data.message}`);
			fontList.append(element("li", { className: "error" },
				`${data.name}: ${data.message}`));
			break;
		case "open":
			entry.size = `${data.width} × ${data.height}`;
			entry.meta.textContent = entry.size;
			break;
		case "step":
			entry.chips.get(data.format).dataset.state = "running";
			entry.meta.textContent = `${entry.size} · ` +
				`Generating ${LABELS[data.format]}…`;
			break;
		case "stepped":
			entry.chips.get(data.format).dataset.state = "done";
			for (const file of data.files) {
				entry.files += 1;
				entry.bytes += file.size;
				entry.list.append(fileItem(file));
			}
			entry.count.textContent = `${entry.files} files, ` +
				formatBytes(entry.bytes);
			entry.details.hidden = false;
			break;
		case "converted":
			entry.meta.textContent = entry.size;
			break;
		case "failed": {
			const { code, message: detail } = data;
			const name = svgs[data.index].name;
			const message = failMessage(name, code, detail);
			failures.push(message);
			entry.meta.textContent = entry.size || "Not converted";
			entry.error.textContent = code === NO_FONTS
				? message : detail;
			entry.error.hidden = false;
			entry.details.hidden = true;
			entry.list.replaceChildren();
			for (const chip of entry.chips.values()) {
				chip.dataset.state = "pending";
			}
			break;
		}
		case "done":
			finish(data, items, failures, note);
			current.terminate();
			break;
		case "error":
			setStatus(`Conversion failed: ${data.message}`, true);
			current.terminate();
			break;
		}
	};
	current.onerror = (event) => {
		event.preventDefault();
		const reason = event.message ||
			"check that archetypon.wasm is served.";
		setStatus(`The converter could not start: ${reason}`, true);
	};
	current.postMessage({
		svgs: svgData,
		fonts: fontData,
		formats,
		options,
	}, [...svgData, ...fontData].map((item) => item.data));
}

function finish(data, items, failures, note) {
	if (data.zip === null) {
		setStatus("Nothing was converted; see the problems below.",
			true);
		return;
	}
	const converted = items.filter((entry) => entry.error.hidden);
	const name = svgs.length === 1
		? `${stemOf(svgs[0].name)}.zip` : "archetypon.zip";
	const url = URL.createObjectURL(
		new Blob([data.zip], { type: "application/zip" }));
	urls.push(url);
	const download = $("download");
	download.href = url;
	download.download = name;
	download.textContent = `Download ${name} ` +
		`(${formatBytes(data.zip.byteLength)})`;
	download.hidden = false;
	const files = converted.reduce((sum, entry) => sum + entry.files, 0);
	const seconds = (data.ms / 1000).toFixed(1);
	const summary = `Converted ${converted.length} of ` +
		`${count(items.length, "SVG")} into ` +
		`${count(files, "file")} in ${seconds} s.`;
	const problems = failures.length === 0 ? "" :
		` ${count(failures.length, "problem")} listed below.`;
	setStatus(summary + problems + note, failures.length > 0);
}

function clear() {
	worker?.terminate();
	worker = null;
	svgs.length = 0;
	fonts.length = 0;
	releaseUrls();
	$("results").replaceChildren();
	$("fonts").replaceChildren();
	$("output").hidden = true;
	$("download").hidden = true;
	updateButtons();
	setStatus("Cleared.");
}

function hasFiles(event) {
	return event.dataTransfer?.types.includes("Files");
}

addEventListener("dragenter", (event) => {
	if (!hasFiles(event)) {
		return;
	}
	event.preventDefault();
	dragDepth += 1;
	$("overlay").hidden = false;
});

addEventListener("dragover", (event) => {
	if (hasFiles(event)) {
		event.preventDefault();
		event.dataTransfer.dropEffect = "copy";
	}
});

addEventListener("dragleave", (event) => {
	if (!hasFiles(event)) {
		return;
	}
	dragDepth = Math.max(0, dragDepth - 1);
	$("overlay").hidden = dragDepth > 0;
});

addEventListener("drop", (event) => {
	if (!hasFiles(event)) {
		return;
	}
	event.preventDefault();
	dragDepth = 0;
	$("overlay").hidden = true;
	add([...event.dataTransfer.files]);
});

$("pick").addEventListener("click", () => $("files").click());
$("files").addEventListener("change", (event) => {
	add([...event.target.files]);
	event.target.value = "";
});
$("convert").addEventListener("click", () => convert());
$("clear").addEventListener("click", clear);
$("options").addEventListener("submit", (event) => {
	event.preventDefault();
	convert();
});
