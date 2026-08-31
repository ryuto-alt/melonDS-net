// 解析済みプログラムの関数一覧を TSV で書き出す。
// 使い方: -postScript RyueFunctions.java <outFile> [nameFilter]
//@category RyuE
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.PrintWriter;

public class RyueFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("RyueFunctions: 出力パスがない");
            return;
        }
        String outPath = args[0];
        String filter = args.length >= 2 ? args[1].toLowerCase() : null;

        try (PrintWriter w = new PrintWriter(outPath, "UTF-8")) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                if (filter != null && !f.getName().toLowerCase().contains(filter)) continue;
                w.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + f.getName());
            }
        }
    }
}
