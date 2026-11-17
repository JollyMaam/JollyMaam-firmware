// A firmware package (jollymaam package, make firmware): manifest.txt, its
// first line "jollymaam-firmware 2", then a line a file:
//   NAME ROLE PATH SIZE SHA256
// NAME a product's PID (ROLE install or update) or a Jieli chip (ROLE
// loader); src/lib/firmware.c reads the same.  In a zip as jollymaam writes
// it (stored), or as a zip program makes one (deflated) of its folder.

const MAGIC = "jollymaam-firmware 2";
const MAIN_HEAD_SIZE = 0x120;

const u16 = (d, o) => d[o] | (d[o + 1] << 8);
const u32 = (d, o) => (d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)) >>> 0;

// The zip's entries from its central directory: {name: read() -> bytes}.
function unzip(z) {
  let end = z.length - 22;
  while (end >= 0 && u32(z, end) !== 0x06054b50) end--;
  if (end < 0) throw new Error("not a zip file");
  const count = u16(z, end + 10);
  let at = u32(z, end + 16);
  const entries = new Map();
  for (let i = 0; i < count; i++) {
    if (u32(z, at) !== 0x02014b50) throw new Error("a damaged zip file");
    const method = u16(z, at + 10), packed = u32(z, at + 20), size = u32(z, at + 24);
    const nameLen = u16(z, at + 28), extra = u16(z, at + 30), comment = u16(z, at + 32);
    const local = u32(z, at + 42);
    const name = new TextDecoder().decode(z.subarray(at + 46, at + 46 + nameLen));
    const from = local + 30 + u16(z, local + 26) + u16(z, local + 28);
    const data = z.subarray(from, from + packed);
    entries.set(name, async () => {
      if (method === 0) return data;
      if (method !== 8) throw new Error(`${name}: compressed in a way this page cannot read`);
      const out = new Uint8Array(await new Response(
        new Blob([data]).stream().pipeThrough(new DecompressionStream("deflate-raw"))).arrayBuffer());
      if (out.length !== size) throw new Error(`${name}: damaged`);
      return out;
    });
    at += 46 + nameLen + extra + comment;
  }
  return entries;
}

const hex = (d) => Array.from(d, (b) => b.toString(16).padStart(2, "0")).join("");

export async function sha256(data) {
  return hex(new Uint8Array(await crypto.subtle.digest("SHA-256", data)));
}

// {files: [{name, role, path, size, sha256}], read(file) -> checked bytes}
export async function openPackage(zipBytes) {
  const entries = unzip(zipBytes);
  // manifest.txt at the top, or in the one folder a zipped folder has.
  const manifestName = [...entries.keys()].filter((n) => n === "manifest.txt" || n.endsWith("/manifest.txt"))
    .sort((a, b) => a.length - b.length)[0];
  if (!manifestName) throw new Error("no manifest.txt: not a firmware package");
  const prefix = manifestName.slice(0, -"manifest.txt".length);
  const lines = new TextDecoder().decode(await entries.get(manifestName)()).split("\n").filter((l) => l.trim());
  if (lines[0]?.trim() !== MAGIC) throw new Error(`manifest.txt is not "${MAGIC}"`);
  const files = lines.slice(1).map((line, i) => {
    const f = line.trim().split(/\s+/);
    if (f.length !== 5 || !/^\d+$/.test(f[3]) || !/^[0-9a-f]{64}$/.test(f[4])) {
      throw new Error(`manifest.txt line ${i + 2}: not NAME ROLE PATH SIZE SHA256`);
    }
    return { name: f[0], role: f[1], path: f[2], size: Number(f[3]), sha256: f[4] };
  });
  if (!files.length) throw new Error("manifest.txt lists no files");
  return {
    files,
    async read(file) {
      const entry = entries.get(prefix + file.path);
      if (!entry) throw new Error(`${file.path}: missing`);
      const data = await entry();
      if (data.length !== file.size) throw new Error(`${file.path}: not the size stated`);
      if ((await sha256(data)) !== file.sha256) throw new Error(`${file.path}: not the SHA-256 stated`);
      return data;
    },
  };
}

export function findFile(pkg, name, role) {
  return pkg.files.find((f) => f.name === name && f.role === role) || null;
}

// update.bin: the MAIN header (src/lib/proto_jollymaam_wire.h) for this
// product.  The updater checks it again, and the CRCs, before it erases.
export function checkUpdateImage(d, product) {
  if (d.length < MAIN_HEAD_SIZE || new TextDecoder().decode(d.subarray(0, 4)) !== "MAIN") {
    return "not an update image (update.bin)";
  }
  if (u32(d, 16) !== 1) return "an update image of another format";
  if (u32(d, 4) !== d.length - MAIN_HEAD_SIZE) return "the update image's size is wrong";
  const pid = new TextDecoder().decode(d.subarray(24, 40)).replace(/\0.*$/s, "");
  if (pid !== product) return `the update image is for ${pid}, the pad is ${product}`;
  return null;
}
