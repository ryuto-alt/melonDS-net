import fs from "node:fs";
import path from "node:path";

// NDS の ROM を静的に読む。ヘッダ / バナー / ファイルシステム(FNT+FAT) /
// オーバーレイテーブル / ARM9・ARM7 バイナリ(BLZ 解凍つき)。

export type NdsHeader = {
  title: string;
  gameCode: string;
  makerCode: string;
  unitCode: number;
  deviceCapacity: number;      // 実バイト数
  romVersion: number;
  arm9: { romOffset: number; entry: number; ramAddress: number; size: number };
  arm7: { romOffset: number; entry: number; ramAddress: number; size: number };
  fnt: { offset: number; size: number };
  fat: { offset: number; size: number };
  arm9Overlay: { offset: number; size: number };
  arm7Overlay: { offset: number; size: number };
  bannerOffset: number;
  usedRomSize: number;
  headerSize: number;
  fileSize: number;
};

export type FsEntry = {
  id: number;
  path: string;
  offset: number;
  size: number;
};

export type Overlay = {
  id: number;
  fileId: number;
  ramAddress: number;
  ramSize: number;
  bssSize: number;
  staticInitStart: number;
  staticInitEnd: number;
  compressedSize: number;
  compressed: boolean;
  romOffset: number;
  romSize: number;
};

export function readHeader(buf: Buffer, fileSize: number): NdsHeader {
  const str = (off: number, len: number) =>
    buf.subarray(off, off + len).toString("latin1").replace(/\0+$/, "");

  return {
    title: str(0x00, 12),
    gameCode: str(0x0c, 4),
    makerCode: str(0x10, 2),
    unitCode: buf[0x12],
    deviceCapacity: 128 * 1024 * (1 << buf[0x14]),
    romVersion: buf[0x1e],
    arm9: {
      romOffset: buf.readUInt32LE(0x20),
      entry: buf.readUInt32LE(0x24),
      ramAddress: buf.readUInt32LE(0x28),
      size: buf.readUInt32LE(0x2c),
    },
    arm7: {
      romOffset: buf.readUInt32LE(0x30),
      entry: buf.readUInt32LE(0x34),
      ramAddress: buf.readUInt32LE(0x38),
      size: buf.readUInt32LE(0x3c),
    },
    fnt: { offset: buf.readUInt32LE(0x40), size: buf.readUInt32LE(0x44) },
    fat: { offset: buf.readUInt32LE(0x48), size: buf.readUInt32LE(0x4c) },
    arm9Overlay: { offset: buf.readUInt32LE(0x50), size: buf.readUInt32LE(0x54) },
    arm7Overlay: { offset: buf.readUInt32LE(0x58), size: buf.readUInt32LE(0x5c) },
    bannerOffset: buf.readUInt32LE(0x68),
    usedRomSize: buf.readUInt32LE(0x80),
    headerSize: buf.readUInt32LE(0x84),
    fileSize,
  };
}

const BANNER_LANGS = ["ja", "en", "fr", "de", "it", "es", "zh", "ko"];

export function readBanner(buf: Buffer, offset: number): Record<string, string> | null {
  if (!offset || offset + 0x240 > buf.length) return null;
  const version = buf.readUInt16LE(offset);
  const count = version >= 3 ? 8 : version === 2 ? 7 : 6;
  const out: Record<string, string> = {};
  for (let i = 0; i < count; i++) {
    const base = offset + 0x240 + i * 0x100;
    if (base + 0x100 > buf.length) break;
    const raw = buf.subarray(base, base + 0x100);
    const text = raw.toString("utf16le").replace(/\0.*$/s, "").trim();
    if (text) out[BANNER_LANGS[i]] = text;
  }
  return out;
}

// ---- ファイルシステム ----------------------------------------------------

export function readFileSystem(buf: Buffer, h: NdsHeader): FsEntry[] {
  const out: FsEntry[] = [];
  if (!h.fnt.size || !h.fat.size) return out;

  const fntBase = h.fnt.offset;
  const fatBase = h.fat.offset;

  const fileInfo = (id: number) => {
    const p = fatBase + id * 8;
    if (p + 8 > buf.length) return null;
    const start = buf.readUInt32LE(p);
    const end = buf.readUInt32LE(p + 4);
    return { offset: start, size: end - start };
  };

  const walk = (dirId: number, prefix: string, depth: number) => {
    if (depth > 32) return;
    const entry = fntBase + (dirId & 0x0fff) * 8;
    if (entry + 8 > buf.length) return;

    let p = fntBase + buf.readUInt32LE(entry);
    let fileId = buf.readUInt16LE(entry + 4);

    for (;;) {
      if (p >= buf.length) return;
      const type = buf[p++];
      if (type === 0) return;

      const isDir = (type & 0x80) !== 0;
      const nameLen = type & 0x7f;
      if (p + nameLen > buf.length) return;
      const name = buf.subarray(p, p + nameLen).toString("latin1");
      p += nameLen;

      if (isDir) {
        if (p + 2 > buf.length) return;
        const subDir = buf.readUInt16LE(p);
        p += 2;
        walk(subDir, `${prefix}${name}/`, depth + 1);
      } else {
        const info = fileInfo(fileId);
        if (info) out.push({ id: fileId, path: `${prefix}${name}`, ...info });
        fileId++;
      }
    }
  };

  walk(0xf000, "/", 0);
  out.sort((a, b) => a.id - b.id);
  return out;
}

// ---- オーバーレイ --------------------------------------------------------

export function readOverlays(buf: Buffer, h: NdsHeader, which: "arm9" | "arm7" = "arm9"): Overlay[] {
  const tbl = which === "arm9" ? h.arm9Overlay : h.arm7Overlay;
  const out: Overlay[] = [];
  if (!tbl.size) return out;

  const count = Math.floor(tbl.size / 32);
  for (let i = 0; i < count; i++) {
    const p = tbl.offset + i * 32;
    if (p + 32 > buf.length) break;
    const fileId = buf.readUInt32LE(p + 24);
    const flags = buf.readUInt32LE(p + 28);
    const fatp = h.fat.offset + fileId * 8;
    const start = fatp + 8 <= buf.length ? buf.readUInt32LE(fatp) : 0;
    const end = fatp + 8 <= buf.length ? buf.readUInt32LE(fatp + 4) : 0;

    out.push({
      id: buf.readUInt32LE(p),
      fileId,
      ramAddress: buf.readUInt32LE(p + 4),
      ramSize: buf.readUInt32LE(p + 8),
      bssSize: buf.readUInt32LE(p + 12),
      staticInitStart: buf.readUInt32LE(p + 16),
      staticInitEnd: buf.readUInt32LE(p + 20),
      compressedSize: flags & 0x00ffffff,
      compressed: ((flags >> 24) & 0x01) !== 0,
      romOffset: start,
      romSize: end - start,
    });
  }
  return out;
}

// ---- BLZ (ARM9 / オーバーレイの後方 LZ 圧縮) -----------------------------

export function blzDecode(input: Buffer): { data: Buffer; wasCompressed: boolean } {
  if (input.length < 8) return { data: input, wasCompressed: false };

  const incLen = input.readUInt32LE(input.length - 4);
  if (incLen === 0) return { data: input, wasCompressed: false };

  const hdrLen = input[input.length - 5];
  if (hdrLen < 8 || hdrLen > 0x0b) return { data: input, wasCompressed: false };

  const encLen = input.readUInt32LE(input.length - 8) & 0x00ffffff;
  if (encLen > input.length) return { data: input, wasCompressed: false };

  const decLen = input.length - encLen;        // 頭の非圧縮部分
  const pakLen = encLen - hdrLen;              // 圧縮された本体
  const rawLen = input.length + incLen;

  const out = Buffer.alloc(rawLen);
  input.copy(out, 0, 0, decLen);

  // 後方 LZ なので、圧縮部と展開先の両方を反転して普通の LZ として処理する
  const pak = Buffer.from(input.subarray(decLen, decLen + pakLen)).reverse();
  const raw = Buffer.alloc(rawLen - decLen);

  let pakPos = 0;
  let rawPos = 0;
  let mask = 0;
  let flags = 0;

  while (rawPos < raw.length && pakPos < pak.length) {
    mask >>= 1;
    if (mask === 0) {
      flags = pak[pakPos++];
      mask = 0x80;
      if (pakPos > pak.length) break;
    }

    if ((flags & mask) === 0) {
      raw[rawPos++] = pak[pakPos++];
    } else {
      if (pakPos + 1 >= pak.length) break;
      const b1 = pak[pakPos++];
      const b2 = pak[pakPos++];
      const dist = (((b1 & 0x0f) << 8) | b2) + 3;
      let len = (b1 >> 4) + 3;
      while (len-- > 0 && rawPos < raw.length) {
        if (rawPos - dist < 0) break;
        raw[rawPos] = raw[rawPos - dist];
        rawPos++;
      }
    }
  }

  raw.reverse();
  raw.copy(out, decLen);
  return { data: out, wasCompressed: true };
}

// ---- まとめ --------------------------------------------------------------

export class NdsRom {
  readonly buf: Buffer;
  readonly header: NdsHeader;
  readonly filePath: string;

  constructor(filePath: string) {
    this.filePath = filePath;
    this.buf = fs.readFileSync(filePath);
    if (this.buf.length < 0x200) throw new Error(`ROM が小さすぎる: ${filePath}`);
    this.header = readHeader(this.buf, this.buf.length);
  }

  banner() {
    return readBanner(this.buf, this.header.bannerOffset);
  }

  files(): FsEntry[] {
    return readFileSystem(this.buf, this.header);
  }

  overlays(which: "arm9" | "arm7" = "arm9"): Overlay[] {
    return readOverlays(this.buf, this.header, which);
  }

  slice(offset: number, size: number): Buffer {
    return this.buf.subarray(offset, Math.min(offset + size, this.buf.length));
  }

  // ARM9 本体。BLZ 圧縮されていれば解凍して返す。
  arm9(): { data: Buffer; loadAddress: number; compressed: boolean } {
    const raw = this.slice(this.header.arm9.romOffset, this.header.arm9.size);
    const { data, wasCompressed } = blzDecode(Buffer.from(raw));
    return { data, loadAddress: this.header.arm9.ramAddress, compressed: wasCompressed };
  }

  arm7(): { data: Buffer; loadAddress: number; compressed: boolean } {
    const raw = this.slice(this.header.arm7.romOffset, this.header.arm7.size);
    const { data, wasCompressed } = blzDecode(Buffer.from(raw));
    return { data, loadAddress: this.header.arm7.ramAddress, compressed: wasCompressed };
  }

  overlayData(ov: Overlay): Buffer {
    const raw = Buffer.from(this.slice(ov.romOffset, ov.romSize));
    if (!ov.compressed) return raw;
    return blzDecode(raw).data;
  }
}

// ---- 文字列抽出 ----------------------------------------------------------

export type FoundString = { offset: number; encoding: string; text: string };

export function extractStrings(
  buf: Buffer,
  opts: { min?: number; encodings?: string[]; filter?: string; limit?: number; baseOffset?: number } = {}
): FoundString[] {
  const min = opts.min ?? 4;
  const limit = opts.limit ?? 500;
  const base = opts.baseOffset ?? 0;
  const encodings = opts.encodings ?? ["ascii", "shift_jis", "utf16le"];
  const re = opts.filter ? new RegExp(opts.filter, "i") : null;
  const out: FoundString[] = [];

  const push = (offset: number, encoding: string, text: string) => {
    if (text.length < min) return;
    if (re && !re.test(text)) return;
    out.push({ offset: base + offset, encoding, text });
  };

  if (encodings.includes("ascii")) {
    let start = -1;
    for (let i = 0; i <= buf.length; i++) {
      const c = i < buf.length ? buf[i] : 0;
      const printable = c >= 0x20 && c < 0x7f;
      if (printable) {
        if (start < 0) start = i;
      } else {
        if (start >= 0 && i - start >= min)
          push(start, "ascii", buf.subarray(start, i).toString("latin1"));
        start = -1;
      }
      if (out.length >= limit) return out;
    }
  }

  if (encodings.includes("utf16le")) {
    let start = -1;
    for (let i = 0; i + 1 < buf.length; i += 2) {
      const c = buf.readUInt16LE(i);
      const printable = (c >= 0x20 && c < 0x7f) || (c >= 0x3000 && c <= 0x9fff) || (c >= 0xff01 && c <= 0xff9f);
      if (printable) {
        if (start < 0) start = i;
      } else {
        if (start >= 0 && (i - start) / 2 >= min)
          push(start, "utf16le", buf.subarray(start, i).toString("utf16le"));
        start = -1;
      }
      if (out.length >= limit) return out;
    }
  }

  if (encodings.includes("shift_jis")) {
    const dec = new TextDecoder("shift_jis", { fatal: false });
    let start = -1;
    for (let i = 0; i <= buf.length; ) {
      const c = i < buf.length ? buf[i] : 0;
      const lead = (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xef);
      const single = (c >= 0x20 && c < 0x7f) || (c >= 0xa1 && c <= 0xdf);
      if (lead && i + 1 < buf.length) {
        const t = buf[i + 1];
        if ((t >= 0x40 && t <= 0x7e) || (t >= 0x80 && t <= 0xfc)) {
          if (start < 0) start = i;
          i += 2;
          continue;
        }
      }
      if (single) {
        if (start < 0) start = i;
        i++;
        continue;
      }
      if (start >= 0 && i - start >= min) {
        const text = dec.decode(buf.subarray(start, i)).replace(/�/g, "");
        // ASCII だけのものは ascii 側で拾えているので、日本語が混じるものだけ足す
        if (/[^\x00-\x7f]/.test(text)) push(start, "shift_jis", text);
      }
      start = -1;
      i++;
      if (out.length >= limit) return out;
    }
  }

  out.sort((a, b) => a.offset - b.offset);
  return out.slice(0, limit);
}

export function resolveRomPath(p: string): string {
  const abs = path.resolve(p);
  if (!fs.existsSync(abs)) throw new Error(`ROM が見つからない: ${abs}`);
  return abs;
}
