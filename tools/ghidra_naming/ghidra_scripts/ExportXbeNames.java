/* Export Xbox XBE analysis from Ghidra (headless post-script).
 *
 * Java port of ExportXbeNames.py. Ghidra 12.x removed Jython, so the Python 2
 * script the toolkit shipped can no longer run; this is the same export with
 * the same files and the same JSON shapes, so merge_names.py and the
 * recompiler's functions.json reader work unchanged.
 *
 * Exports into the output directory:
 *   functions.json  - address/name/signature/calling_convention/param_count/
 *                     is_thunk/namespace for every function
 *   symbols.json    - address/name/type/namespace/source/primary
 *   decompiled.json - per-function C (only when arg1 is "decompile")
 *
 * Script args (positional, all optional):
 *   arg0 outDir        output directory (default: ./ghidra_export)
 *   arg1 doDecompile   "decompile" | "nodecompile"      (default nodecompile)
 *   arg2 decompLimit   integer, or "all"                (default 4000)
 *   arg3 decompTimeout per-function seconds             (default 30)
 *
 * @category XboxRecomp
 */
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

public class ExportXbeNames extends GhidraScript {

    /* ---- tiny JSON helpers: the shapes are flat, a dependency is not worth it */

    private static String esc(String s) {
        if (s == null)
            return "";
        StringBuilder b = new StringBuilder(s.length() + 8);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n");  break;
                case '\r': b.append("\\r");  break;
                case '\t': b.append("\\t");  break;
                default:
                    if (c < 0x20 || c > 0x7E)
                        b.append(String.format("\\u%04x", (int) c));
                    else
                        b.append(c);
            }
        }
        return b.toString();
    }

    /** Address as "0x%08X", matching the recompiler's functions.json `start`. */
    private static String hexaddr(Address a) {
        return a == null ? "" : String.format("0x%08X", a.getOffset());
    }

    private String nsOf(Function f) {
        try {
            if (f.getParentNamespace() == null)
                return "";
            String n = f.getParentNamespace().getName(true);
            return ("Global".equals(n) || n == null) ? "" : n;
        } catch (Exception e) {
            return "";
        }
    }

    private String nsOf(Symbol s) {
        try {
            if (s.getParentNamespace() == null)
                return "";
            String n = s.getParentNamespace().getName(true);
            return ("Global".equals(n) || n == null) ? "" : n;
        } catch (Exception e) {
            return "";
        }
    }

    private PrintWriter writer(File dir, String name) throws Exception {
        return new PrintWriter(Files.newBufferedWriter(
                new File(dir, name).toPath(), StandardCharsets.UTF_8));
    }

    private void writeArray(File dir, String name, List<String> rows) throws Exception {
        try (PrintWriter w = writer(dir, name)) {
            w.println("[");
            for (int i = 0; i < rows.size(); i++)
                w.println(rows.get(i) + (i + 1 < rows.size() ? "," : ""));
            w.println("]");
        }
    }

    private void exportFunctions(File dir) throws Exception {
        List<String> rows = new ArrayList<>();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            try {
                String sig;
                try {
                    sig = f.getPrototypeString(false, false);
                } catch (Exception e) {
                    sig = "";
                }
                String cc;
                try {
                    cc = f.getCallingConventionName();
                } catch (Exception e) {
                    cc = "";
                }
                int pcount;
                try {
                    pcount = f.getParameterCount();
                } catch (Exception e) {
                    pcount = -1;
                }
                rows.add(" {\n"
                        + "  \"address\": \"" + hexaddr(f.getEntryPoint()) + "\",\n"
                        + "  \"name\": \"" + esc(f.getName()) + "\",\n"
                        + "  \"signature\": \"" + esc(sig) + "\",\n"
                        + "  \"calling_convention\": \"" + esc(cc) + "\",\n"
                        + "  \"param_count\": " + pcount + ",\n"
                        + "  \"is_thunk\": " + (f.isThunk() ? "true" : "false") + ",\n"
                        + "  \"namespace\": \"" + esc(nsOf(f)) + "\"\n"
                        + " }");
            } catch (Exception e) {
                println("  [func export warn] " + e);
            }
        }
        writeArray(dir, "functions.json", rows);
        println("Exported " + rows.size() + " functions -> functions.json");
    }

    private void exportSymbols(File dir) throws Exception {
        List<String> rows = new ArrayList<>();
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Symbol s = it.next();
            try {
                Address a = s.getAddress();
                if (a == null)
                    continue;
                String type;
                try {
                    type = String.valueOf(s.getSymbolType());
                } catch (Exception e) {
                    type = "Unknown";
                }
                rows.add(" {\n"
                        + "  \"address\": \"" + hexaddr(a) + "\",\n"
                        + "  \"name\": \"" + esc(s.getName()) + "\",\n"
                        + "  \"type\": \"" + esc(type) + "\",\n"
                        + "  \"namespace\": \"" + esc(nsOf(s)) + "\",\n"
                        + "  \"source\": \"" + esc(String.valueOf(s.getSource())) + "\",\n"
                        + "  \"primary\": " + (s.isPrimary() ? "true" : "false") + "\n"
                        + " }");
            } catch (Exception e) {
                /* a symbol we cannot describe is not worth failing the export */
            }
        }
        writeArray(dir, "symbols.json", rows);
        println("Exported " + rows.size() + " symbols -> symbols.json");
    }

    private void exportDecompiled(File dir, int limit, int timeout) throws Exception {
        ghidra.app.decompiler.DecompInterface decomp =
                new ghidra.app.decompiler.DecompInterface();
        decomp.toggleCCode(true);
        decomp.toggleSyntaxTree(true);
        if (!decomp.openProgram(currentProgram)) {
            println("decompiler refused the program: " + decomp.getLastMessage());
            return;
        }
        List<String> rows = new ArrayList<>();
        int processed = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            if (limit > 0 && processed >= limit) {
                println("Reached decompile limit (" + limit + ") for this run.");
                break;
            }
            String c;
            if (f.isThunk()) {
                c = "/* thunk */";
            } else {
                try {
                    ghidra.app.decompiler.DecompileResults res =
                            decomp.decompileFunction(f, timeout, monitor);
                    c = "";
                    if (res != null && res.decompileCompleted()
                            && res.getDecompiledFunction() != null)
                        c = res.getDecompiledFunction().getC();
                    if (c == null || c.isEmpty())
                        c = "/* decompile failed: "
                                + (res != null ? res.getErrorMessage() : "no result") + " */";
                } catch (Exception e) {
                    c = "/* exception: " + e + " */";
                }
            }
            rows.add(" {\n  \"address\": \"" + hexaddr(f.getEntryPoint()) + "\",\n"
                    + "  \"name\": \"" + esc(f.getName()) + "\",\n"
                    + "  \"decompiled_c\": \"" + esc(c) + "\"\n }");
            processed++;
            if ((processed % 200) == 0)
                println("  ... " + processed + " decompiled this run");
        }
        decomp.dispose();
        writeArray(dir, "decompiled.json", rows);
        println("Decompile: processed " + processed + " functions.");
    }

    private static int intArg(String s, int dflt) {
        try {
            return Integer.parseInt(s.trim());
        } catch (Exception e) {
            return dflt;
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = (args.length >= 1 && !args[0].isEmpty()) ? args[0] : null;
        boolean doDecompile = args.length >= 2 && "decompile".equalsIgnoreCase(args[1]);
        int limit = 4000;
        if (args.length >= 3 && !args[2].isEmpty())
            limit = "all".equalsIgnoreCase(args[2]) ? 0 : intArg(args[2], 4000);
        int timeout = args.length >= 4 && !args[3].isEmpty() ? intArg(args[3], 30) : 30;

        File dir = new File(outDir != null ? outDir : "ghidra_export");
        if (!dir.exists() && !dir.mkdirs())
            throw new RuntimeException("cannot create " + dir);

        println("=== ExportXbeNames (Java) ===");
        println("Program:   " + currentProgram.getName());
        println("Out dir:   " + dir.getAbsolutePath());
        println("Decompile: " + doDecompile + " (limit=" + limit
                + ", timeout=" + timeout + "s)");

        exportFunctions(dir);
        exportSymbols(dir);
        if (doDecompile)
            exportDecompiled(dir, limit, timeout);
        else
            println("Decompilation skipped (pass 'decompile' as arg1 to enable).");
        println("=== Export complete ===");
    }
}


