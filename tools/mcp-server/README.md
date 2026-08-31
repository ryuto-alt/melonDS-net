# RyuE MCP サーバ

RyuE(この DS エミュレータ)を AI から解析するための MCP サーバ。
動作中のゲームのメモリをリアルタイムで読み書きしつつ、ROM の静的解析と
逆アセンブル、Ghidra による逆コンパイルまでを一本の窓口にまとめてある。

```
Claude Code ──stdio──> このMCPサーバ(Node) ──TCP 127.0.0.1:8099──> RyuE の DebugBridge
                              │
                              ├─ ROM ファイルを直接読む(ヘッダ/FS/オーバーレイ/BLZ)
                              ├─ disasm.py (capstone) で逆アセンブル
                              └─ Ghidra headless で逆コンパイル
```

## 仕組み

エミュ側は `src/frontend/qt_sdl/DebugBridge.cpp`。エミュスレッドが**フレーム境界で**
コマンドを実行するので、`RunFrame` の途中で RAM を覗いて壊すレースが起きない。
ポートは 8099 から順に空きを探す(2窓なら 8099 と 8100)。ローカル(127.0.0.1)専用。

| 環境変数 | 意味 |
|---|---|
| `RYUE_BRIDGE_OFF=1` | ブリッジを立ち上げない |
| `RYUE_BRIDGE_PORT` | 開始ポートを変える(既定 8099) |
| `GHIDRA_HOME` | Ghidra の場所(既定は `~/tools/ghidra_*` を自動検出) |
| `RYUE_PYTHON` | capstone が入った python(既定 `python`) |

ブリッジは**エミュスレッドが走り出してから**立つ。つまり利用規約ダイアログが出ている
間はまだ繋がらない。`ryue_instances` が空なら、まず RyuE の画面を確認すること。

## ツール

### 実行時
| ツール | 用途 |
|---|---|
| `ryue_instances` | 動いている RyuE を探す(2窓のときどっちがどのポートか) |
| `ryue_status` | 実行状態 / フレーム数 / ROM 情報 |
| `ryue_read` | メモリを読む(hexdump / u8,u16,u32 配列 / ascii) |
| `ryue_write` | メモリに書く(数値 or バイト列) |
| `ryue_search` | RAM サーチ。new → (ゲームを動かす) → next → list |
| `ryue_watch` | 複数アドレスを時系列サンプリング |
| `ryue_freeze` | 値を毎フレーム書き戻して固定 |
| `ryue_regs` | ARM9/ARM7 のレジスタ(PC で今の実行位置が分かる) |
| `ryue_dump` | 大きな領域をファイルに書き出す |
| `ryue_input` | ボタン / タッチを注入 |
| `ryue_control` | pause / resume / reset / step |
| `ryue_screenshot` | 画面を PNG で取得(画像として返る) |
| `ryue_state` | セーブステートの保存・読み込み |

### 静的(ROM)
| ツール | 用途 |
|---|---|
| `ryue_rom_info` | ヘッダとバナー |
| `ryue_rom_files` | NitroFS のファイル一覧 |
| `ryue_rom_extract` | ファイル / ARM9 / ARM7 を取り出す(BLZ 解凍つき) |
| `ryue_rom_overlays` | オーバーレイテーブル |
| `ryue_strings` | 文字列抽出(ASCII / Shift-JIS / UTF-16LE) |

### 逆アセ・逆コンパイル
| ツール | 用途 |
|---|---|
| `ryue_disasm` | capstone で ARM/Thumb 逆アセンブル(RAM でも ROM でも) |
| `ryue_ghidra_analyze` | Ghidra に取り込んで自動解析(先に 1 回必要・数分〜) |
| `ryue_ghidra_functions` | 見つかった関数一覧 |
| `ryue_decompile` | 指定アドレスの関数を C 疑似コードに |

## 使いどころの型

**HP の番地を探す**
```
ryue_search {mode:"new", type:"u16", op:"eq", value:<今のHP>}
→ ダメージを受ける
ryue_search {mode:"next", op:"eq", value:<減った後のHP>}
→ 数件になったら ryue_search {mode:"list"} → ryue_freeze で固定して確認
```

**今動いているコードを読む**
```
ryue_regs → pc を得る
ryue_disasm {source:"ram", addr:<pc>, mode:"auto"}
ryue_ghidra_analyze {source:"ram"} → ryue_decompile {addr:<pc>}
```
`source:"ram"` はオーバーレイが載った実際の状態を丸ごと解析するので、
「今のシーンのコード」を読むならこちらが早い。ROM の ARM9 だけを見る
`source:"rom"` は、常駐コードの全体像を掴むとき向け。

## テスト

RyuE を起動して ROM を読み込んだ状態で:

```bash
node test.mjs            # 全ツールの疎通
node test.mjs search     # 名前に search を含むものだけ
```
