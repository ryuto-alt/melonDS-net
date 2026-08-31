// 指定アドレスを含む関数を逆コンパイルして、指定ファイルに C 疑似コードを書き出す。
// 使い方: -postScript RyueDecompile.java <hexAddr> <outFile>
//@category RyuE
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class RyueDecompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("RyueDecompile: 引数が足りない");
            return;
        }

        long value = Long.parseLong(args[0].replaceFirst("^0[xX]", ""), 16);
        Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(value);
        String outPath = args[1];

        Function func = getFunctionContaining(addr);
        if (func == null) {
            func = createFunction(addr, null);
        }

        try (PrintWriter w = new PrintWriter(outPath, "UTF-8")) {
            if (func == null) {
                w.println("// " + args[0] + " に関数が見つからず、作成もできなかった");
                return;
            }

            DecompInterface di = new DecompInterface();
            di.openProgram(currentProgram);
            DecompileResults res = di.decompileFunction(func, 180, monitor);

            w.println("// function: " + func.getName() + " @ " + func.getEntryPoint());
            w.println("// size: " + func.getBody().getNumAddresses() + " bytes");
            if (res != null && res.decompileCompleted()) {
                w.print(res.getDecompiledFunction().getC());
            } else {
                w.println("// 逆コンパイル失敗: " + (res == null ? "null" : res.getErrorMessage()));
            }
            di.dispose();
        }
    }
}
