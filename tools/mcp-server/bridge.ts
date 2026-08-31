import net from "node:net";

// RyuE 内の DebugBridge (127.0.0.1:8099〜8106) に喋るクライアント。
//
// プロトコルは行指向:
//   送信 "<id> <cmd> [args...]\n"
//   受信 "<id> ok <json>\n" / "<id> err <message>\n"
// エミュ側はフレーム境界で処理するので、1 リクエストあたり最大 1 フレーム分待つ。

export const BASE_PORT = Number(process.env.RYUE_BRIDGE_PORT || 8099);
export const PORT_SPAN = 8;

export class BridgeError extends Error {}

type Pending = {
  resolve: (v: any) => void;
  reject: (e: Error) => void;
  timer: NodeJS.Timeout;
};

class Connection {
  private sock: net.Socket | null = null;
  private buf = "";
  private nextId = 1;
  private pending = new Map<number, Pending>();
  readonly port: number;

  constructor(port: number) {
    this.port = port;
  }

  private connect(): Promise<net.Socket> {
    if (this.sock && !this.sock.destroyed) return Promise.resolve(this.sock);

    return new Promise((resolve, reject) => {
      const s = net.createConnection({ host: "127.0.0.1", port: this.port });
      s.setNoDelay(true);

      const onError = (e: Error) => {
        this.teardown(e);
        reject(new BridgeError(`RyuE に接続できない (127.0.0.1:${this.port}): ${e.message}`));
      };

      s.once("error", onError);
      s.once("connect", () => {
        s.removeListener("error", onError);
        s.on("error", (e) => this.teardown(e));
        s.on("close", () => this.teardown(new BridgeError("接続が閉じられた")));
        s.on("data", (d) => this.onData(d));
        this.sock = s;
        resolve(s);
      });
    });
  }

  private teardown(err: Error)
  {
    for (const p of this.pending.values()) {
      clearTimeout(p.timer);
      p.reject(err instanceof BridgeError ? err : new BridgeError(err.message));
    }
    this.pending.clear();
    if (this.sock) {
      this.sock.destroy();
      this.sock = null;
    }
    this.buf = "";
  }

  private onData(chunk: Buffer) {
    this.buf += chunk.toString("utf8");
    for (;;) {
      const nl = this.buf.indexOf("\n");
      if (nl < 0) break;
      const line = this.buf.slice(0, nl);
      this.buf = this.buf.slice(nl + 1);

      const sp1 = line.indexOf(" ");
      if (sp1 < 0) continue;
      const id = Number(line.slice(0, sp1));
      const rest = line.slice(sp1 + 1);
      const sp2 = rest.indexOf(" ");
      const kind = sp2 < 0 ? rest : rest.slice(0, sp2);
      const payload = sp2 < 0 ? "" : rest.slice(sp2 + 1);

      const p = this.pending.get(id);
      if (!p) continue;
      this.pending.delete(id);
      clearTimeout(p.timer);

      if (kind === "ok") {
        try {
          p.resolve(payload ? JSON.parse(payload) : {});
        } catch (e: any) {
          p.reject(new BridgeError(`応答 JSON が壊れている: ${e.message}`));
        }
      } else {
        p.reject(new BridgeError(payload || "unknown error"));
      }
    }
  }

  async send(cmd: string, timeoutMs = 15000): Promise<any> {
    const sock = await this.connect();
    const id = this.nextId++;

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new BridgeError(`タイムアウト: ${cmd.slice(0, 60)}`));
      }, timeoutMs);

      this.pending.set(id, { resolve, reject, timer });
      sock.write(`${id} ${cmd}\n`);
    });
  }

  close() {
    this.teardown(new BridgeError("closed"));
  }
}

const conns = new Map<number, Connection>();

function conn(port: number): Connection {
  let c = conns.get(port);
  if (!c) {
    c = new Connection(port);
    conns.set(port, c);
  }
  return c;
}

// 生きているインスタンスを探す。port 未指定なら 8099 から順に ping。
export async function discover(): Promise<{ port: number; info: any }[]> {
  const found: { port: number; info: any }[] = [];
  for (let p = BASE_PORT; p < BASE_PORT + PORT_SPAN; p++) {
    try {
      const info = await conn(p).send("ping", 1200);
      found.push({ port: p, info });
    } catch {
      conns.get(p)?.close();
      conns.delete(p);
    }
  }
  return found;
}

let cachedPort: number | null = null;

export async function resolvePort(port?: number): Promise<number> {
  if (port) return port;
  if (cachedPort !== null) {
    try {
      await conn(cachedPort).send("ping", 1200);
      return cachedPort;
    } catch {
      cachedPort = null;
    }
  }
  const found = await discover();
  if (!found.length)
    throw new BridgeError(
      "動いている RyuE が見つからない。RyuE を起動して ROM を読み込んでから、もう一度どうぞ " +
      `(探したポート: ${BASE_PORT}-${BASE_PORT + PORT_SPAN - 1})`
    );
  cachedPort = found[0].port;
  return cachedPort;
}

export async function send(cmd: string, port?: number, timeoutMs?: number): Promise<any> {
  const p = await resolvePort(port);
  return conn(p).send(cmd, timeoutMs);
}
