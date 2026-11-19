// Decompile every function of the current program into one C file per function plus an index.
// Usage (headless): analyzeHeadless <proj dir> <proj> -process <prog> -noanalysis
//                   -scriptPath tools/ghidra -postScript KpDecompileAll.java <out dir>
// Output is firmware-derived: point <out dir> under data/ (gitignored).
//@category Prophecy
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class KpDecompileAll extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File out = new File(args.length > 0 ? args[0] : "decomp");
		out.mkdirs();
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		int ok = 0, fail = 0;
		try (PrintWriter index = new PrintWriter(new File(out, "index.tsv"), "UTF-8")) {
			index.println("addr\tname\tstatus\tlines");
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				String addr = String.format("%06x", f.getEntryPoint().getOffset());
				DecompileResults r = di.decompileFunction(f, 60, monitor);
				if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null) {
					String c = r.getDecompiledFunction().getC();
					try (PrintWriter w = new PrintWriter(new File(out, addr + "_" + f.getName() + ".c"), "UTF-8")) {
						w.print(c);
					}
					index.printf("%s\t%s\tok\t%d%n", addr, f.getName(), c.split("\n").length);
					ok++;
				} else {
					index.printf("%s\t%s\tFAIL %s\t0%n", addr, f.getName(), r == null ? "null" : r.getErrorMessage());
					fail++;
				}
			}
		}
		di.dispose();
		println("KpDecompileAll: " + ok + " ok, " + fail + " failed -> " + out.getAbsolutePath());
	}
}
