import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { discover, send, BridgeError } from "./bridge.ts";
import { NdsRom, extractStrings, resolveRomPath } from "./rom.ts";
import * as ghidra from "./ghidra.ts";

// RyuE(自作の DS エミュレータ)を AI から解析するための MCP サーバ。
//
// ・実行時解析: 起動中の RyuE の DebugBridge(TCP 127.0.0.1:8099〜)を叩く。
//   メモリ R/W、RAM サーチ、レジスタ、フリーズ、入力注入、スクショ、セーブステート。
// ・静的解析: ROM ファイルを直接読む。ヘッダ / ファイルシステム / オーバーレイ /
//   ARM9・ARM7 バイナリ(BLZ 解凍) / 文字列。
// ・逆アセ: capstone (disasm.py 経由)。
// ・逆コンパイル: Ghidra headless。
//
// アドレスは 10進でも "0x02000000" でも受ける。返り値のアドレスは 16進文字列で返す。

const HERE = path.dirname(fileURLToPath(import.meta.url));
const server = new McpServer({ name: "ryue", version: "0.1.0" });

const TMP = path.join(os.tmpdir(), "ryue-mcp");
fs.mkdirSync(TMP, { recursive: true });

type ToolResult = {
  content: ({ type: "text"; text: string } | { type: "image"; data: string; mimeType: string })[];
  isError?: boolean;
};

const OUT = { result: z.any() };

function ok(data: unknown): ToolResult {
  return { content: [{ type: "text", text: JSON.stringify(data, null, 2) }] };
}

function fail(e: unknown): ToolResult {
  const msg = e instanceof Error ? e.message : String(e);
  return { content: [{ type: "text", text: JSON.stringify({ error: msg }, null, 2) }], isError: true };
}

function tool(
  name: string,
  description: string,
  schema: Record<string, z.ZodTypeAny>,
  handler: (args: any) => Promise<ToolResult>
) {
  server.registerTool(
    name,
    { description, inputSchema: schema },
    async (args: any) => {
      try {
        return await handler(args ?? {});
      } catch (e) {
        return fail(e);
      }
    }
  );
}

// ---- 共通ヘルパ ----------------------------------------------------------

const hex = (n: number) => "0x" + (n >>> 0).toString(16).padStart(8, "0");

function parseAddr(v: string | number): number {
  if (typeof v === "number") return v >>> 0;
  const s = v.trim();
  const n = s.toLowerCase().startsWith("0x") ? parseInt(s.slice(2), 16) : Number(s);
  if (!Number.isFinite(n)) throw new Error(`アドレスが読めない: ${v}`);
  return n >>> 0;
}

const AddrArg = z.union([z.string(), z.number()]);
const PortArg = z
  .number()
  .int()
  .optional()
  .describe("複数の RyuE を開いているときの接続先ポート。省略時は 8099 から自動検出");

// DS のキー配置(KEYINPUT のビット順)
const KEY_BITS: Record<string, number> = {
  a: 0, b: 1, select: 2, start: 3, right: 4, left: 5, up: 6, down: 7, r: 8, l: 9, x: 10, y: 11,
};

function keysToMask(keys: string[]): number {
  let mask = 0;
  for (const k of keys) {
    const bit = KEY_BITS[k.toLowerCase()];
    if (bit === undefined)
      throw new Error(`知らないキー: ${k} (使えるのは ${Object.keys(KEY_BITS).join(", ")})`);
    mask |= 1 << bit;
  }
  return mask;
}

// ROM は mtime つきでキャッシュする
const romCache = new Map<string, { mtime: number; rom: NdsRom }>();

async function openRom(romPath?: string, port?: number): Promise<NdsRom> {
  let p = romPath;
  if (!p) {
    const st = await send("status", port);
    if (!st.romPath) throw new Error("RyuE に ROM が入っていない。rom 引数でファイルを指定してください");
    p = st.romPath;
  }
  const abs = resolveRomPath(p!);
  const mtime = fs.statSync(abs).mtimeMs;
  const hit = romCache.get(abs);
  if (hit && hit.mtime === mtime) return hit.rom;
  const rom = new NdsRom(abs);
  romCache.set(abs, { mtime, rom });
  return rom;
}

function runDisasm(data: Buffer, addr: number, mode: string, count: number): Promise<any> {
  return new Promise((resolve, reject) => {
    const py = spawn(process.env.RYUE_PYTHON || "python", [path.join(HERE, "disasm.py")], {
      windowsHide: true,
    });
    let out = "";
    let err = "";
    py.stdout.on("data", (d) => (out += d.toString()));
    py.stderr.on("data", (d) => (err += d.toString()));
    py.on("error", reject);
    py.on("close", () => {
      if (!out.trim()) return reject(new Error(`disasm.py が何も返さない: ${err.trim()}`));
      try {
        resolve(JSON.parse(out));
      } catch (e: any) {
        reject(new Error(`disasm.py の出力が壊れている: ${e.message}\n${out.slice(0, 200)}`));
      }
    });
    py.stdin.end(
      JSON.stringify({ data: data.toString("base64"), addr, mode, count })
    );
  });
}

async function readBytes(bus: string, addr: number, len: number, port?: number): Promise<Buffer> {
  const chunks: Buffer[] = [];
  const MAX = 0x40000;
  for (let off = 0; off < len; off += MAX) {
    const n = Math.min(MAX, len - off);
    const r = await send(`read ${bus} ${hex(addr + off)} ${n}`, port);
    chunks.push(Buffer.from(r.data, "base64"));
  }
  return Buffer.concat(chunks);
}

// ============================================================
//  実行時: 接続と状態
// ============================================================

tool(
  "ryue_instances",
  "動いている RyuE を探して一覧する。ポート 8099〜8106 を ping し、応答したインスタンスを返す。" +
    "2窓(ネットプレイ検証など)のときにどのポートがどれかを確かめる用。",
  {},
  async () => {
    const found = await discover();
    if (!found.length)
      return ok({
        instances: [],
        hint: "RyuE が起動していないか、まだ ROM を読み込む前(利用規約ダイアログ中はブリッジが立たない)",
      });
    return ok({ instances: found });
  }
);

tool(
  "ryue_status",
  "起動中の RyuE の状態を返す。実行中か / フレーム数 / ROM のタイトル・ゲームコード・パス / " +
    "メインRAM サイズ / DS か DSi か。他のツールを使う前の確認に。",
  { port: PortArg },
  async ({ port }) => ok(await send("status", port))
);

// ============================================================
//  実行時: メモリ
// ============================================================

tool(
  "ryue_read",
  "動作中のゲームのメモリを読む。bus=main はメインRAM(0x02000000〜)を直接読むので速く副作用もない。" +
    "bus=arm9/arm7 は CPU バスごしなので I/O レジスタや VRAM も読めるが、読むと状態が変わる領域もある。" +
    "format で見せ方を変えられる(hex ダンプ / u8,u16,u32 の配列 / 文字列)。",
  {
    addr: AddrArg.describe("読み出し開始アドレス。例 0x02000000"),
    len: z.number().int().min(1).max(0x40000).describe("バイト数(最大 262144)"),
    bus: z.enum(["main", "arm9", "arm7"]).default("main"),
    format: z.enum(["hex", "u8", "u16", "u32", "s32", "ascii", "base64"]).default("hex"),
    port: PortArg,
  },
  async ({ addr, len, bus, format, port }) => {
    const a = parseAddr(addr);
    const buf = await readBytes(bus, a, len, port);

    if (format === "base64") return ok({ addr: hex(a), len, data: buf.toString("base64") });
    if (format === "ascii")
      return ok({ addr: hex(a), len, text: buf.toString("latin1").replace(/[^\x20-\x7e]/g, ".") });

    if (format === "hex") {
      const lines: string[] = [];
      for (let i = 0; i < buf.length; i += 16) {
        const row = buf.subarray(i, i + 16);
        const h = [...row].map((b) => b.toString(16).padStart(2, "0")).join(" ");
        const asc = row.toString("latin1").replace(/[^\x20-\x7e]/g, ".");
        lines.push(`${hex(a + i)}  ${h.padEnd(47)}  ${asc}`);
      }
      return ok({ addr: hex(a), len, hexdump: lines.join("\n") });
    }

    const values: number[] = [];
    const step = format === "u8" ? 1 : format === "u16" ? 2 : 4;
    for (let i = 0; i + step <= buf.length; i += step) {
      if (format === "u8") values.push(buf[i]);
      else if (format === "u16") values.push(buf.readUInt16LE(i));
      else if (format === "u32") values.push(buf.readUInt32LE(i));
      else values.push(buf.readInt32LE(i));
    }
    return ok({ addr: hex(a), len, step, values });
  }
);

tool(
  "ryue_write",
  "動作中のゲームのメモリに書き込む。value+size で 1/2/4 バイトの数値を書くか、hex でバイト列を書く。" +
    "書いた値をゲームのロジックに上書きされ続けたくないなら ryue_freeze を使う。",
  {
    addr: AddrArg,
    value: z.number().int().optional().describe("書き込む数値(size と併用)"),
    size: z.union([z.literal(1), z.literal(2), z.literal(4)]).default(4),
    hex: z.string().optional().describe("バイト列を 16進文字列で。例 \"01ff02\"。value より優先"),
    bus: z.enum(["main", "arm9", "arm7"]).default("main"),
    port: PortArg,
  },
  async ({ addr, value, size, hex: hexStr, bus, port }) => {
    const a = parseAddr(addr);
    if (hexStr) {
      const clean = hexStr.replace(/[^0-9a-fA-F]/g, "");
      if (clean.length % 2) throw new Error("hex の桁数が奇数");
      const b64 = Buffer.from(clean, "hex").toString("base64");
      return ok(await send(`write ${bus} ${hex(a)} ${b64}`, port));
    }
    if (value === undefined) throw new Error("value か hex のどちらかが要る");
    return ok(await send(`poke ${bus} ${hex(a)} ${size} ${value}`, port));
  }
);

tool(
  "ryue_search",
  "メインRAM の値サーチ(いわゆるチート検索)。HP や所持金の番地を突き止めるのに使う。" +
    "手順: mode=new で最初の絞り込み → ゲームを動かす → mode=next で changed/dec/eq などで追い込む → " +
    "mode=list で残った候補を見る。値が分からないときは mode=new, op=unknown で全走査してから next で追う。",
  {
    mode: z.enum(["new", "next", "list", "reset"]),
    type: z
      .enum(["u8", "s8", "u16", "s16", "u32", "s32", "f32"])
      .default("u32")
      .describe("mode=new のときの値の型"),
    op: z
      .enum(["eq", "ne", "gt", "lt", "ge", "le", "unknown", "changed", "unchanged", "inc", "dec", "delta"])
      .optional()
      .describe("new: eq/ne/gt/lt/ge/le/unknown、next: それに加えて changed/unchanged/inc/dec/delta"),
    value: z.number().optional().describe("比較する値(op が値を要るとき)"),
    max: z.number().int().min(1).max(2000).default(50).describe("mode=list で返す件数"),
    port: PortArg,
  },
  async ({ mode, type, op, value, max, port }) => {
    if (mode === "reset") return ok(await send("search_reset", port));
    if (mode === "list") {
      const r = await send(`search_list ${max}`, port);
      return ok({
        ...r,
        results: (r.results ?? []).map((x: any) => ({ ...x, addr: hex(x.addr) })),
      });
    }

    if (!op) throw new Error("op が要る");
    const needValue = ["eq", "ne", "gt", "lt", "ge", "le", "delta"].includes(op);
    if (needValue && value === undefined) throw new Error(`op=${op} には value が要る`);

    const cmd =
      mode === "new"
        ? `search_new ${type} ${op}${needValue ? " " + value : ""}`
        : `search_next ${op}${needValue ? " " + value : ""}`;
    const r = await send(cmd, port, 60000);
    return ok(r);
  }
);

tool(
  "ryue_watch",
  "指定アドレスの値を一定間隔で複数回サンプリングして、時系列で返す。" +
    "「この番地はダメージを受けたときに減るか」みたいな仮説の検証に使う。",
  {
    addrs: z.array(AddrArg).min(1).max(16),
    size: z.union([z.literal(1), z.literal(2), z.literal(4)]).default(4),
    samples: z.number().int().min(1).max(120).default(10),
    intervalMs: z.number().int().min(16).max(2000).default(100),
    signed: z.boolean().default(false),
    bus: z.enum(["main", "arm9", "arm7"]).default("main"),
    port: PortArg,
  },
  async ({ addrs, size, samples, intervalMs, signed, bus, port }) => {
    const list = addrs.map(parseAddr);
    const series: { t: number; values: number[] }[] = [];
    const t0 = Date.now();

    for (let i = 0; i < samples; i++) {
      const values: number[] = [];
      for (const a of list) {
        const buf = await readBytes(bus, a, size, port);
        const v =
          size === 1
            ? signed ? buf.readInt8(0) : buf.readUInt8(0)
            : size === 2
            ? signed ? buf.readInt16LE(0) : buf.readUInt16LE(0)
            : signed ? buf.readInt32LE(0) : buf.readUInt32LE(0);
        values.push(v);
      }
      series.push({ t: Date.now() - t0, values });
      if (i < samples - 1) await new Promise((r) => setTimeout(r, intervalMs));
    }

    const summary = list.map((a, idx) => {
      const vals = series.map((s) => s.values[idx]);
      return {
        addr: hex(a),
        min: Math.min(...vals),
        max: Math.max(...vals),
        first: vals[0],
        last: vals[vals.length - 1],
        changed: new Set(vals).size > 1,
      };
    });

    return ok({ addrs: list.map(hex), summary, series });
  }
);

tool(
  "ryue_freeze",
  "アドレスの値を毎フレーム書き戻して固定する(チートの「値を固定」)。" +
    "add で登録、list で一覧、del で 1 件解除、clear で全解除。",
  {
    action: z.enum(["add", "del", "list", "clear"]),
    addr: AddrArg.optional(),
    value: z.number().int().optional(),
    size: z.union([z.literal(1), z.literal(2), z.literal(4)]).default(4),
    id: z.number().int().optional().describe("action=del で解除する登録 ID"),
    bus: z.enum(["main", "arm9", "arm7"]).default("main"),
    port: PortArg,
  },
  async ({ action, addr, value, size, id, bus, port }) => {
    if (action === "list") {
      const r = await send("freeze_list", port);
      return ok({ freezes: (r.freezes ?? []).map((f: any) => ({ ...f, addr: hex(f.addr) })) });
    }
    if (action === "clear") return ok(await send("freeze_clear", port));
    if (action === "del") {
      if (id === undefined) throw new Error("action=del には id が要る");
      return ok(await send(`freeze_del ${id}`, port));
    }
    if (addr === undefined || value === undefined)
      throw new Error("action=add には addr と value が要る");
    return ok(await send(`freeze_add ${bus} ${hex(parseAddr(addr))} ${size} ${value}`, port));
  }
);

tool(
  "ryue_regs",
  "ARM9 / ARM7 の汎用レジスタと CPSR を読む。PC(r15) を見れば今どのコードを走っているか分かる。",
  { cpu: z.enum(["arm9", "arm7"]).default("arm9"), port: PortArg },
  async ({ cpu, port }) => {
    const r = await send(`regs ${cpu}`, port);
    return ok({
      cpu: r.cpu,
      pc: hex(r.pc),
      thumb: r.thumb,
      cpsr: hex(r.cpsr),
      r: (r.r as number[]).map(hex),
      rDecimal: r.r,
    });
  }
);

tool(
  "ryue_dump",
  "メモリ領域をファイルに書き出す。ryue_read の上限(256KB)を超える大きな範囲(メインRAM 全部など)向け。" +
    "書き出したファイルは ryue_ghidra_analyze や ryue_strings の入力に使える。",
  {
    addr: AddrArg.default("0x02000000"),
    len: z.number().int().min(1).default(4 * 1024 * 1024),
    path: z.string().optional().describe("出力先。省略すると一時ディレクトリに置く"),
    bus: z.enum(["main", "arm9", "arm7"]).default("main"),
    port: PortArg,
  },
  async ({ addr, len, path: outPath, bus, port }) => {
    const a = parseAddr(addr);
    const out = outPath ?? path.join(TMP, `dump_${bus}_${hex(a)}_${len}.bin`);
    const r = await send(`dump ${bus} ${hex(a)} ${len} ${out}`, port, 60000);
    return ok({ ...r, addr: hex(a) });
  }
);

// ============================================================
//  実行時: 操作
// ============================================================

tool(
  "ryue_input",
  "ボタンとタッチを注入する。keys は押しっぱなしにするフレーム数を frames で指定(60 = 約1秒)。" +
    "keys を空配列にすると解除。touchX/touchY を出すとタッチペンを当てられる。",
  {
    keys: z.array(z.enum(["a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "x", "y"]))
      .default([]),
    frames: z.number().int().min(0).max(3600).default(10),
    touchX: z.number().int().min(0).max(255).optional(),
    touchY: z.number().int().min(0).max(191).optional(),
    touchFrames: z.number().int().min(0).max(3600).optional(),
    touchUp: z.boolean().default(false).describe("true にするとタッチを離す"),
    port: PortArg,
  },
  async ({ keys, frames, touchX, touchY, touchFrames, touchUp, port }) => {
    const out: any = {};
    out.keys = await send(`keys ${keysToMask(keys)} ${frames}`, port);
    if (touchUp) out.touch = await send("touch up", port);
    else if (touchX !== undefined && touchY !== undefined)
      out.touch = await send(`touch ${touchX} ${touchY} ${touchFrames ?? frames}`, port);
    return ok(out);
  }
);

tool(
  "ryue_control",
  "エミュレータの実行制御。pause / resume / reset / step(指定フレームだけ進めて止まる)。" +
    "step は 1 フレーム単位の観察に便利。",
  {
    action: z.enum(["pause", "resume", "reset", "step"]),
    frames: z.number().int().min(1).max(600).default(1).describe("action=step のフレーム数"),
    port: PortArg,
  },
  async ({ action, frames, port }) => {
    const cmd = action === "step" ? `step ${frames}` : action;
    return ok(await send(cmd, port));
  }
);

tool(
  "ryue_screenshot",
  "今の画面を PNG で取得して画像として返す。上下画面を縦につなげたものが既定。" +
    "ゲームの今の状況を目で確かめたいときに使う。",
  {
    which: z.enum(["both", "top", "bottom"]).default("both"),
    save: z.string().optional().describe("保存先パス。省略すると一時ファイル"),
    port: PortArg,
  },
  async ({ which, save, port }) => {
    const out = save ?? path.join(TMP, `shot_${Date.now()}.png`);
    const r = await send(`screenshot ${out} ${which}`, port);
    const data = fs.readFileSync(out).toString("base64");
    return {
      content: [
        { type: "text", text: JSON.stringify({ path: r.path, width: r.width, height: r.height }) },
        { type: "image", data, mimeType: "image/png" },
      ],
    };
  }
);

tool(
  "ryue_state",
  "セーブステートの保存 / 読み込み。解析中に同じ場面を何度も再現したいときに使う。",
  {
    action: z.enum(["save", "load"]),
    path: z.string().describe("ステートファイルのパス"),
    port: PortArg,
  },
  async ({ action, path: p, port }) =>
    ok(await send(`${action === "save" ? "savestate" : "loadstate"} ${p}`, port, 30000))
);

// ============================================================
//  静的: ROM 解析
// ============================================================

tool(
  "ryue_rom_info",
  "ROM のヘッダとバナーを読む。タイトル / ゲームコード / ARM9・ARM7 の配置(ロードアドレス、サイズ) / " +
    "ファイルシステムとオーバーレイテーブルの位置。rom を省略すると RyuE が今読み込んでいる ROM を見る。",
  { rom: z.string().optional(), port: PortArg },
  async ({ rom, port }) => {
    const r = await openRom(rom, port);
    const h = r.header;
    return ok({
      path: r.filePath,
      header: {
        ...h,
        arm9: { ...h.arm9, romOffset: hex(h.arm9.romOffset), entry: hex(h.arm9.entry), ramAddress: hex(h.arm9.ramAddress) },
        arm7: { ...h.arm7, romOffset: hex(h.arm7.romOffset), entry: hex(h.arm7.entry), ramAddress: hex(h.arm7.ramAddress) },
      },
      banner: r.banner(),
      fileCount: r.files().length,
      arm9Overlays: r.overlays("arm9").length,
      arm7Overlays: r.overlays("arm7").length,
    });
  }
);

tool(
  "ryue_rom_files",
  "ROM の中のファイル一覧(NitroFS)。filter に正規表現を渡すと絞れる。" +
    "サウンド、モデル、テキストなど、ゲームがどう素材を持っているかを掴む用。",
  {
    rom: z.string().optional(),
    filter: z.string().optional().describe("パスに対する正規表現。例 \"\\\\.nsbmd$\""),
    limit: z.number().int().min(1).max(2000).default(200),
    port: PortArg,
  },
  async ({ rom, filter, limit, port }) => {
    const r = await openRom(rom, port);
    let files = r.files();
    const total = files.length;
    if (filter) {
      const re = new RegExp(filter, "i");
      files = files.filter((f) => re.test(f.path));
    }
    return ok({
      total,
      matched: files.length,
      files: files.slice(0, limit).map((f) => ({ ...f, offset: hex(f.offset) })),
    });
  }
);

tool(
  "ryue_rom_extract",
  "ROM の中のファイルを 1 つ取り出して保存する。file にパス、または id にファイル ID を渡す。" +
    "arm9 / arm7 を指定すると、その CPU の本体バイナリを(BLZ 圧縮されていれば解凍して)書き出す。",
  {
    rom: z.string().optional(),
    file: z.string().optional().describe("NitroFS のパス、または \"arm9\" / \"arm7\""),
    id: z.number().int().optional(),
    out: z.string().optional().describe("出力先。省略すると一時ディレクトリ"),
    port: PortArg,
  },
  async ({ rom, file, id, out, port }) => {
    const r = await openRom(rom, port);

    if (file === "arm9" || file === "arm7") {
      const bin = file === "arm9" ? r.arm9() : r.arm7();
      const dest = out ?? path.join(TMP, `${path.basename(r.filePath, ".nds")}_${file}.bin`);
      fs.writeFileSync(dest, bin.data);
      return ok({
        path: dest,
        size: bin.data.length,
        loadAddress: hex(bin.loadAddress),
        wasCompressed: bin.compressed,
      });
    }

    const files = r.files();
    const hit =
      id !== undefined ? files.find((f) => f.id === id) : files.find((f) => f.path === file);
    if (!hit) throw new Error(`ファイルが見つからない: ${file ?? id}`);

    const dest = out ?? path.join(TMP, path.basename(hit.path));
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.writeFileSync(dest, r.slice(hit.offset, hit.size));
    return ok({ path: dest, size: hit.size, romOffset: hex(hit.offset), id: hit.id });
  }
);

tool(
  "ryue_rom_overlays",
  "オーバーレイテーブルを読む。DS のゲームはコードをオーバーレイに分割して必要なときだけ RAM に載せるので、" +
    "「今動いているコードがどのオーバーレイか」を ryue_regs の PC と ramAddress の範囲で突き合わせられる。",
  { rom: z.string().optional(), which: z.enum(["arm9", "arm7"]).default("arm9"), port: PortArg },
  async ({ rom, which, port }) => {
    const r = await openRom(rom, port);
    const ovs = r.overlays(which);
    return ok({
      count: ovs.length,
      overlays: ovs.map((o) => ({
        ...o,
        ramAddress: hex(o.ramAddress),
        ramEnd: hex(o.ramAddress + o.ramSize),
        romOffset: hex(o.romOffset),
      })),
    });
  }
);

tool(
  "ryue_strings",
  "文字列を抽出する。source=rom で ROM 全体、source=ram で動作中のメインRAM、source=file で任意のファイル。" +
    "ASCII だけでなく Shift-JIS と UTF-16LE も拾うので、日本語のセリフやメニュー文言も出る。",
  {
    source: z.enum(["rom", "ram", "file"]).default("rom"),
    rom: z.string().optional(),
    file: z.string().optional().describe("source=file のときの入力ファイル"),
    addr: AddrArg.optional().describe("source=ram のときの開始アドレス。既定 0x02000000"),
    len: z.number().int().optional().describe("source=ram のときの長さ。既定 4MB"),
    min: z.number().int().min(2).max(64).default(4),
    filter: z.string().optional().describe("結果に対する正規表現フィルタ"),
    encodings: z.array(z.enum(["ascii", "shift_jis", "utf16le"])).default(["ascii", "shift_jis", "utf16le"]),
    limit: z.number().int().min(1).max(2000).default(200),
    port: PortArg,
  },
  async ({ source, rom, file, addr, len, min, filter, encodings, limit, port }) => {
    let buf: Buffer;
    let base = 0;

    if (source === "rom") {
      const r = await openRom(rom, port);
      buf = r.buf;
    } else if (source === "file") {
      if (!file) throw new Error("source=file には file が要る");
      buf = fs.readFileSync(file);
    } else {
      const a = addr !== undefined ? parseAddr(addr) : 0x02000000;
      const n = len ?? 4 * 1024 * 1024;
      const tmp = path.join(TMP, `strings_${Date.now()}.bin`);
      await send(`dump main ${hex(a)} ${n} ${tmp}`, port, 60000);
      buf = fs.readFileSync(tmp);
      base = a;
      fs.rmSync(tmp, { force: true });
    }

    const found = extractStrings(buf, { min, filter, limit, encodings, baseOffset: base });
    return ok({
      source,
      count: found.length,
      strings: found.map((s) => ({ ...s, offset: hex(s.offset) })),
    });
  }
);

// ============================================================
//  逆アセンブル
// ============================================================

tool(
  "ryue_disasm",
  "ARM / Thumb の逆アセンブル(capstone)。source=ram なら動作中のメモリを、source=rom なら ROM の " +
    "ARM9 / ARM7 バイナリを、指定アドレスから読んで逆アセンブルする。" +
    "mode=auto は ryue_regs の CPSR の T ビットから ARM/Thumb を推測する(source=ram のときのみ)。",
  {
    source: z.enum(["ram", "rom"]).default("ram"),
    addr: AddrArg.describe("RAM アドレス、または ROM の場合はロード先アドレス"),
    count: z.number().int().min(1).max(400).default(40).describe("命令数"),
    mode: z.enum(["arm", "thumb", "auto"]).default("auto"),
    which: z.enum(["arm9", "arm7"]).default("arm9").describe("source=rom のときにどちらのバイナリを見るか"),
    rom: z.string().optional(),
    port: PortArg,
  },
  async ({ source, addr, count, mode, which, rom, port }) => {
    const a = parseAddr(addr);
    let realMode = mode;
    let data: Buffer;
    const byteLen = Math.min(count * 4 + 8, 0x40000);

    if (source === "ram") {
      if (mode === "auto") {
        const r = await send(`regs ${which}`, port);
        realMode = r.thumb ? "thumb" : "arm";
      }
      data = await readBytes("main", a, byteLen, port);
      // メインRAM 外(ITCM や BIOS など)は CPU バスから読む
      if (a < 0x02000000 || a >= 0x03000000) data = await readBytes(which, a, byteLen, port);
    } else {
      const r = await openRom(rom, port);
      const bin = which === "arm9" ? r.arm9() : r.arm7();
      const off = a - bin.loadAddress;
      if (off < 0 || off >= bin.data.length) {
        // オーバーレイの中かもしれない
        const ov = r.overlays(which).find((o) => a >= o.ramAddress && a < o.ramAddress + o.ramSize);
        if (!ov)
          throw new Error(
            `${hex(a)} は ${which} 本体(${hex(bin.loadAddress)}〜${hex(bin.loadAddress + bin.data.length)})にも` +
              `オーバーレイにも入っていない`
          );
        const od = r.overlayData(ov);
        data = od.subarray(a - ov.ramAddress, a - ov.ramAddress + byteLen);
      } else {
        data = bin.data.subarray(off, off + byteLen);
      }
      if (realMode === "auto") realMode = "arm";
    }

    const res = await runDisasm(data, a, realMode, count);
    if (res.error) throw new Error(res.error);
    return ok({
      source,
      mode: realMode,
      addr: hex(a),
      listing: res.insns
        .map((i: any) => `${hex(i.addr)}  ${i.bytes.padEnd(8)}  ${i.mnemonic} ${i.op}`)
        .join("\n"),
      insns: res.insns,
    });
  }
);

// ============================================================
//  逆コンパイル (Ghidra headless)
// ============================================================

tool(
  "ryue_ghidra_analyze",
  "Ghidra headless でバイナリを取り込んで自動解析する。逆コンパイルの前に 1 回やる必要がある(数分〜十数分)。\n" +
    "source=rom: ROM の ARM9 本体(BLZ 解凍済み)をロードアドレスに配置して解析。静的な全体像向け。\n" +
    "source=ram: 動作中のメインRAM 4MB をそのまま解析。オーバーレイが載った実際の状態が見えるので、" +
    "「今動いているコード」を読むならこちら。",
  {
    source: z.enum(["rom", "ram"]).default("ram"),
    name: z.string().optional().describe("プロジェクト名。省略するとゲームコードから作る"),
    rom: z.string().optional(),
    addr: AddrArg.optional().describe("source=ram の開始アドレス。既定 0x02000000"),
    len: z.number().int().optional().describe("source=ram の長さ。既定 4MB"),
    port: PortArg,
  },
  async ({ source, name, rom, addr, len, port }) => {
    let data: Buffer;
    let base: number;
    let projectName = name;

    if (source === "rom") {
      const r = await openRom(rom, port);
      const bin = r.arm9();
      data = bin.data;
      base = bin.loadAddress;
      projectName ??= `${r.header.gameCode}_arm9`;
    } else {
      const a = addr !== undefined ? parseAddr(addr) : 0x02000000;
      const n = len ?? 4 * 1024 * 1024;
      const tmp = path.join(TMP, `ghidra_ram_${Date.now()}.bin`);
      await send(`dump main ${hex(a)} ${n} ${tmp}`, port, 120000);
      data = fs.readFileSync(tmp);
      fs.rmSync(tmp, { force: true });
      base = a;
      if (!projectName) {
        const st = await send("status", port);
        projectName = `${st.gameCode || "unknown"}_ram`;
      }
    }

    const res = await ghidra.analyze({ name: projectName!, data, baseAddress: base });
    const funcLine = /Analysis succeeded|ERROR|Exception/gi;
    return ok({
      project: projectName,
      source,
      baseAddress: hex(base),
      size: data.length,
      exitCode: res.code,
      workDir: ghidra.WORK_DIR,
      log: res.stdout.split("\n").filter((l) => funcLine.test(l)).slice(-20).join("\n") ||
        res.stdout.split("\n").slice(-15).join("\n"),
      stderr: res.stderr.slice(-1500) || undefined,
    });
  }
);

tool(
  "ryue_decompile",
  "Ghidra で、指定アドレスを含む関数を C 疑似コードに逆コンパイルする。" +
    "先に ryue_ghidra_analyze を済ませておくこと。ryue_regs で得た PC をそのまま渡せば、" +
    "「今走っているコード」の中身が読める。",
  {
    addr: AddrArg,
    project: z.string().optional().describe("プロジェクト名。省略すると ROM/RAM から推測"),
    source: z.enum(["ram", "rom"]).default("ram").describe("project 省略時にどちらの命名で探すか"),
    port: PortArg,
  },
  async ({ addr, project, source, port }) => {
    const a = parseAddr(addr);
    let name = project;
    if (!name) {
      const st = await send("status", port);
      name = `${st.gameCode || "unknown"}_${source === "ram" ? "ram" : "arm9"}`;
    }
    const p = ghidra.projectPaths(name);
    const outFile = path.join(p.dir, `decomp_${hex(a)}.c`);

    const res = await ghidra.runScript(name, "RyueDecompile.java", [hex(a), outFile], );
    if (!fs.existsSync(outFile))
      throw new Error(
        `逆コンパイル結果が出てこなかった (exit ${res.code})。` +
          `プロジェクト ${name} が解析済みか確認: ${res.stdout.split("\n").slice(-8).join(" / ")}`
      );

    return ok({ project: name, addr: hex(a), code: fs.readFileSync(outFile, "utf8") });
  }
);

tool(
  "ryue_ghidra_functions",
  "Ghidra が見つけた関数の一覧(アドレス / サイズ / 名前)。filter で名前の部分一致検索ができる。" +
    "逆コンパイルしたい関数を探すのに使う。",
  {
    project: z.string().optional(),
    source: z.enum(["ram", "rom"]).default("ram"),
    filter: z.string().optional(),
    limit: z.number().int().min(1).max(2000).default(200),
    port: PortArg,
  },
  async ({ project, source, filter, limit, port }) => {
    let name = project;
    if (!name) {
      const st = await send("status", port);
      name = `${st.gameCode || "unknown"}_${source === "ram" ? "ram" : "arm9"}`;
    }
    const p = ghidra.projectPaths(name);
    const outFile = path.join(p.dir, "functions.tsv");
    const res = await ghidra.runScript(name, "RyueFunctions.java", filter ? [outFile, filter] : [outFile]);
    if (!fs.existsSync(outFile))
      throw new Error(`関数一覧が出てこなかった (exit ${res.code})。先に ryue_ghidra_analyze を`);

    const rows = fs
      .readFileSync(outFile, "utf8")
      .split("\n")
      .map((l) => l.trimEnd())
      .filter(Boolean)
      .map((l) => {
        const [address, size, fname] = l.split("\t");
        return { address, size: Number(size), name: fname };
      });

    return ok({ project: name, count: rows.length, functions: rows.slice(0, limit) });
  }
);

// ---- 起動 ---------------------------------------------------------------

const transport = new StdioServerTransport();
await server.connect(transport);
