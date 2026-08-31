import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

// Ghidra headless (analyzeHeadless) 連携。
// ROM の ARM9 バイナリ、または動作中のメインRAM ダンプを取り込んで解析し、
// 指定アドレスの関数を逆コンパイルして C 疑似コードを返す。

const HERE = path.dirname(fileURLToPath(import.meta.url));

export const WORK_DIR =
  process.env.RYUE_GHIDRA_WORK || path.join(os.homedir(), "ryue-mcp", "ghidra");
const SCRIPT_DIR = path.join(HERE, "ghidra_scripts");

export function findGhidra(): string | null {
  const explicit = process.env.GHIDRA_HOME;
  const candidates = [
    ...(explicit ? [explicit] : []),
    path.join(os.homedir(), "tools"),
    "C:/tools",
    "C:/ghidra",
  ];

  for (const base of candidates) {
    const direct = path.join(base, "support", "analyzeHeadless.bat");
    if (fs.existsSync(direct)) return base;
    if (!fs.existsSync(base)) continue;
    for (const name of fs.readdirSync(base)) {
      if (!name.toLowerCase().startsWith("ghidra")) continue;
      const hit = path.join(base, name, "support", "analyzeHeadless.bat");
      if (fs.existsSync(hit)) return path.join(base, name);
    }
  }
  return null;
}

export function headlessPath(): string {
  const home = findGhidra();
  if (!home)
    throw new Error(
      "Ghidra が見つからない。GHIDRA_HOME を設定するか、~/tools/ghidra_* に展開してください"
    );
  return process.platform === "win32"
    ? path.join(home, "support", "analyzeHeadless.bat")
    : path.join(home, "support", "analyzeHeadless");
}

export type HeadlessResult = { code: number; stdout: string; stderr: string };

export function runHeadless(args: string[], timeoutMs = 30 * 60 * 1000): Promise<HeadlessResult> {
  const exe = headlessPath();
  // Windows では .bat を直接 spawn できない(Node のセキュリティ制限)ので cmd 経由で叩く
  const [cmd, cmdArgs] =
    process.platform === "win32"
      ? [process.env.ComSpec || "cmd.exe", ["/c", exe, ...args]]
      : [exe, args];

  return new Promise((resolve, reject) => {
    // stdin は閉じておく。エラー時に .bat が pause で止まると永遠に返ってこない
    const proc = spawn(cmd, cmdArgs, { windowsHide: true, stdio: ["ignore", "pipe", "pipe"] });
    let stdout = "";
    let stderr = "";
    const timer = setTimeout(() => {
      proc.kill();
      reject(new Error(`Ghidra がタイムアウトした (${Math.round(timeoutMs / 1000)}秒)`));
    }, timeoutMs);

    proc.stdout.on("data", (d) => (stdout += d.toString()));
    proc.stderr.on("data", (d) => (stderr += d.toString()));
    proc.on("error", (e) => {
      clearTimeout(timer);
      reject(e);
    });
    proc.on("close", (code) => {
      clearTimeout(timer);
      resolve({ code: code ?? -1, stdout, stderr });
    });
  });
}

export function projectPaths(name: string) {
  const dir = path.join(WORK_DIR, name);
  fs.mkdirSync(dir, { recursive: true });
  return {
    dir,
    projectName: name,
    binary: path.join(dir, `${name}.bin`),
    meta: path.join(dir, "meta.json"),
    out: path.join(dir, "out.txt"),
  };
}

export function readMeta(name: string): any | null {
  const p = projectPaths(name);
  if (!fs.existsSync(p.meta)) return null;
  return JSON.parse(fs.readFileSync(p.meta, "utf8"));
}

export function writeMeta(name: string, meta: any) {
  const p = projectPaths(name);
  fs.writeFileSync(p.meta, JSON.stringify(meta, null, 2));
}

// バイナリを取り込んで自動解析まで走らせる
export async function analyze(opts: {
  name: string;
  data: Buffer;
  baseAddress: number;
  processor?: string;
}): Promise<HeadlessResult> {
  const p = projectPaths(opts.name);
  fs.writeFileSync(p.binary, opts.data);

  // 同名の取り込みが残っていると -import が失敗するので作り直す
  for (const f of fs.readdirSync(p.dir)) {
    if (f.endsWith(".gpr") || f.endsWith(".rep") || f.endsWith(".lock") || f.endsWith(".lock~"))
      fs.rmSync(path.join(p.dir, f), { recursive: true, force: true });
  }

  const res = await runHeadless([
    p.dir,
    p.projectName,
    "-import",
    p.binary,
    "-processor",
    opts.processor ?? "ARM:LE:32:v5t",
    "-loader",
    "BinaryLoader",
    "-loader-baseAddr",
    "0x" + opts.baseAddress.toString(16),
    "-scriptPath",
    SCRIPT_DIR,
    "-analysisTimeoutPerFile",
    "1800",
  ]);

  writeMeta(opts.name, {
    name: opts.name,
    baseAddress: opts.baseAddress,
    size: opts.data.length,
    importedAt: new Date().toISOString(),
    binary: p.binary,
  });

  return res;
}

// 取り込み済みプロジェクトに対してスクリプトを流す
export async function runScript(
  name: string,
  script: string,
  scriptArgs: string[]
): Promise<HeadlessResult> {
  const p = projectPaths(name);
  const meta = readMeta(name);
  if (!meta) throw new Error(`プロジェクト ${name} がまだ無い。先に ryue_ghidra_analyze を実行`);

  return runHeadless([
    p.dir,
    p.projectName,
    "-process",
    path.basename(p.binary),
    "-noanalysis",
    "-scriptPath",
    SCRIPT_DIR,
    "-postScript",
    script,
    ...scriptArgs,
  ]);
}
