// MCP サーバの疎通テスト。RyuE を起動して ROM を読み込んだ状態で:
//   node test.mjs
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [path.join(HERE, "index.ts")],
});
const client = new Client({ name: "ryue-test", version: "1.0.0" });
await client.connect(transport);

const tools = await client.listTools();
console.log(`ツール ${tools.tools.length} 個: ${tools.tools.map((t) => t.name).join(", ")}\n`);

async function call(name, args = {}) {
  const r = await client.callTool({ name, arguments: args });
  const text = r.content.filter((c) => c.type === "text").map((c) => c.text).join("\n");
  const images = r.content.filter((c) => c.type === "image").length;
  const head = text.length > 700 ? text.slice(0, 700) + "\n…" : text;
  console.log(`--- ${name} ${JSON.stringify(args)}${r.isError ? "  [ERROR]" : ""}`);
  console.log(head + (images ? `\n(画像 ${images} 枚)` : "") + "\n");
  return r;
}

const only = process.argv[2];
const steps = [
  ["ryue_instances", {}],
  ["ryue_status", {}],
  ["ryue_read", { addr: "0x02000000", len: 32, format: "hex" }],
  ["ryue_regs", {}],
  ["ryue_rom_info", {}],
  ["ryue_rom_files", { filter: "\\.sdat$", limit: 5 }],
  ["ryue_rom_overlays", {}],
  ["ryue_strings", { source: "rom", min: 6, filter: "mario", limit: 5 }],
  ["ryue_disasm", { source: "ram", addr: "0x02000800", count: 6, mode: "arm" }],
  ["ryue_search", { mode: "new", type: "u32", op: "eq", value: 999 }],
  ["ryue_search", { mode: "list", max: 3 }],
  ["ryue_watch", { addrs: ["0x02000000"], samples: 3, intervalMs: 50 }],
  ["ryue_freeze", { action: "list" }],
  ["ryue_input", { keys: ["a"], frames: 5 }],
  ["ryue_screenshot", {}],
];

for (const [name, args] of steps) {
  if (only && !name.includes(only)) continue;
  try {
    await call(name, args);
  } catch (e) {
    console.log(`--- ${name} 例外: ${e.message}\n`);
  }
}

await client.close();
process.exit(0);
