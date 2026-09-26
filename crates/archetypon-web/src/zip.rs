const LOCAL: u32 = 0x0403_4b50;
const CENTRAL: u32 = 0x0201_4b50;
const END: u32 = 0x0605_4b50;
const LOCAL_LEN: u32 = 30;
const CENTRAL_LEN: u32 = 46;
const END_LEN: u32 = 22;
const VERSION: u16 = 20;
const UTF8: u16 = 1 << 11;

struct Entry {
	name: String,
	name_len: u16,
	crc: u32,
	size: u32,
	data: Vec<u8>,
}

/// An archive of stored (uncompressed) entries, written by `finish`.
pub struct Zip {
	time: u16,
	date: u16,
	entries: Vec<Entry>,
}

impl Zip {
	/// `stamp` is an MS-DOS date (high half) and time (low half).
	pub fn new(stamp: u32) -> Result<Self, String> {
		let [time_low, time_high, date_low, date_high] =
			stamp.to_le_bytes();
		let time = u16::from_le_bytes([time_low, time_high]);
		let date = u16::from_le_bytes([date_low, date_high]);
		let valid = time & 0x1f < 30
			&& (time >> 5) & 0x3f < 60
			&& time >> 11 < 24 && (1..=31)
			.contains(&(date & 0x1f))
			&& (1..=12).contains(&((date >> 5) & 0xf));
		if !valid {
			return Err(format!(
				"invalid MS-DOS timestamp {stamp:#x}"
			));
		}
		Ok(Self {
			time,
			date,
			entries: Vec::new(),
		})
	}

	pub fn is_empty(&self) -> bool {
		self.entries.is_empty()
	}

	pub fn len(&self) -> usize {
		self.entries.len()
	}

	pub fn truncate(&mut self, len: usize) {
		self.entries.truncate(len);
	}

	pub fn add(
		&mut self,
		name: String,
		data: Vec<u8>,
	) -> Result<(), String> {
		if self.entries.len() >= usize::from(u16::MAX) {
			return Err("archive has too many files".into());
		}
		let name_len = u16::try_from(name.len())
			.map_err(|_| format!("file name too long: {name}"))?;
		let size = u32::try_from(data.len())
			.map_err(|_| format!("{name} exceeds 4 GiB"))?;
		self.entries.push(Entry {
			crc: crc32fast::hash(&data),
			name,
			name_len,
			size,
			data,
		});
		Ok(())
	}

	pub fn finish(self) -> Result<Vec<u8>, String> {
		let too_large = || String::from("archive exceeds 4 GiB");
		let count =
			u16::try_from(self.entries.len()).map_err(|_| {
				String::from("archive has too many files")
			})?;
		let mut offsets = Vec::with_capacity(self.entries.len());
		let mut offset = 0_u32;
		let mut directory = 0_u32;
		for entry in &self.entries {
			offsets.push(offset);
			offset = u32::from(entry.name_len)
				.checked_add(LOCAL_LEN)
				.and_then(|len| len.checked_add(entry.size))
				.and_then(|len| len.checked_add(offset))
				.ok_or_else(too_large)?;
			directory = directory
				.checked_add(
					CENTRAL_LEN + u32::from(entry.name_len),
				)
				.ok_or_else(too_large)?;
		}
		let total = offset
			.checked_add(directory)
			.and_then(|len| len.checked_add(END_LEN))
			.ok_or_else(too_large)?;
		let capacity =
			usize::try_from(total).map_err(|_| too_large())?;
		let mut out = Vec::new();
		out.try_reserve_exact(capacity).map_err(|_| too_large())?;
		for entry in &self.entries {
			put32(&mut out, LOCAL);
			put16(&mut out, VERSION);
			self.common(&mut out, entry);
			put16(&mut out, 0);
			out.extend_from_slice(entry.name.as_bytes());
			out.extend_from_slice(&entry.data);
		}
		for (entry, &start) in self.entries.iter().zip(&offsets) {
			put32(&mut out, CENTRAL);
			put16(&mut out, VERSION);
			put16(&mut out, VERSION);
			self.common(&mut out, entry);
			for _ in 0..4 {
				put16(&mut out, 0);
			}
			put32(&mut out, 0);
			put32(&mut out, start);
			out.extend_from_slice(entry.name.as_bytes());
		}
		put32(&mut out, END);
		put32(&mut out, 0);
		put16(&mut out, count);
		put16(&mut out, count);
		put32(&mut out, directory);
		put32(&mut out, offset);
		put16(&mut out, 0);
		Ok(out)
	}

	fn common(&self, out: &mut Vec<u8>, entry: &Entry) {
		put16(out, UTF8);
		put16(out, 0);
		put16(out, self.time);
		put16(out, self.date);
		put32(out, entry.crc);
		put32(out, entry.size);
		put32(out, entry.size);
		put16(out, entry.name_len);
	}
}

fn put16(out: &mut Vec<u8>, value: u16) {
	out.extend_from_slice(&value.to_le_bytes());
}

fn put32(out: &mut Vec<u8>, value: u32) {
	out.extend_from_slice(&value.to_le_bytes());
}
