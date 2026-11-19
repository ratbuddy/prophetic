// Export function entry points and body address ranges: "entry\tname\tstart\tend" per range.
// Usage (headless): ... -process <prog> -noanalysis -readOnly -scriptPath tools/ghidra
//                   -postScript KpFuncRanges.java <out.tsv>
//@category Prophecy
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class KpFuncRanges extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		try (PrintWriter w = new PrintWriter(args.length > 0 ? args[0] : "func_ranges.tsv", "UTF-8")) {
			w.println("entry\tname\tstart\tend");
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			int n = 0;
			while (it.hasNext()) {
				Function f = it.next();
				for (AddressRange r : f.getBody()) {
					w.printf("%06x\t%s\t%06x\t%06x%n", f.getEntryPoint().getOffset(), f.getName(),
						r.getMinAddress().getOffset(), r.getMaxAddress().getOffset());
				}
				n++;
			}
			println("KpFuncRanges: " + n + " functions");
		}
	}
}
