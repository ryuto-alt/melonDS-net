"""capstone で ARM/Thumb を逆アセンブルする小さなヘルパ。

MCP サーバ(Node)から stdin に JSON を投げて、stdout に JSON を返す。
  入力: {"data": "<base64>", "addr": 0x02000000, "mode": "arm"|"thumb", "count": 64}
  出力: {"insns": [{"addr":..,"bytes":"..","mnemonic":"..","op":".."}, ...]}
"""

import base64
import json
import sys

try:
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN
except ImportError:
    print(json.dumps({"error": "capstone が入っていない。`pip install capstone` を実行してください"}))
    sys.exit(0)


def main() -> None:
    req = json.load(sys.stdin)
    data = base64.b64decode(req["data"])
    addr = int(req.get("addr", 0))
    mode = req.get("mode", "arm")
    count = int(req.get("count", 64))

    cs_mode = CS_MODE_THUMB if mode == "thumb" else CS_MODE_ARM
    md = Cs(CS_ARCH_ARM, cs_mode | CS_MODE_LITTLE_ENDIAN)
    md.detail = False

    insns = []
    for ins in md.disasm(data, addr, count):
        insns.append({
            "addr": ins.address,
            "bytes": ins.bytes.hex(),
            "mnemonic": ins.mnemonic,
            "op": ins.op_str,
        })

    print(json.dumps({"insns": insns, "mode": mode}))


if __name__ == "__main__":
    main()
