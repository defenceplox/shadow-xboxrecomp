/* Seed Ghidra with the recompiler's known function entry points BEFORE analysis.
 *
 * Java port of SeedFunctions.py (Ghidra 12.x dropped Jython). The flat raw image
 * has no symbol table, so auto-analysis only finds what is reachable from the
 * entry point and from prologue patterns -- measured here: 17,789 functions,
 * against the 39,345 this project's own disassembler already identified. Seeding
 * those exact addresses gives Function ID properly-bounded bodies at the
 * addresses the recompiler keys on, which is what makes names transfer.
 *
 * It also applies the naming-relevant analysis options, because analyzeHeadless
 * takes ONE -preScript and this pass needs both seeding and configuration.
 *
 * Arg0 (optional): path to functions.json. Default is resolved from the current
 * working directory as ../disasm/output/functions.json, so pass it explicitly.
 *
 * @category XboxRecomp
 */
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;

public class SeedFunctions extends GhidraScript {

    /* The recompiler's functions.json is machine-generated: pull the `start`
     * values with a regex rather than pulling a JSON parser in. */
    private static final Pattern START =
            Pattern.compile("\"start\"\\s*:\\s*\"?(?:0x)?([0-9a-fA-F]+)\"?");

    private Set<Long> readStarts(File f) throws Exception {
        String text = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
        Set<Long> out = new LinkedHashSet<>();
        Matcher m = START.matcher(text);
        while (m.find()) {
            try {
                out.add(Long.parseUnsignedLong(m.group(1), 16));
            } catch (Exception e) {
                /* skip a malformed record */
            }
        }
        return out;
    }

    private void enable(Map<String, String> opts, String name) {
        if (opts.containsKey(name)) {
            setAnalysisOption(currentProgram, name, "true");
            println("[pre] enabled analyzer: " + name);
        }
    }

    private void disable(Map<String, String> opts, String name) {
        if (opts.containsKey(name)) {
            setAnalysisOption(currentProgram, name, "false");
            println("[pre] DISABLED (slow, names not needed): " + name);
        }
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File fj = (args.length >= 1 && !args[0].isEmpty())
                ? new File(args[0])
                : new File("../disasm/output/functions.json");

        if (!fj.exists()) {
            println("[seed] functions.json not found: " + fj.getAbsolutePath()
                    + " (skipping seed)");
        } else {
            Set<Long> starts = readStarts(fj);
            println("[seed] read " + starts.size() + " function starts from "
                    + fj.getAbsolutePath());

            AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
            int created = 0, already = 0, skipped = 0, disas = 0, n = 0;

            for (Long off : starts) {
                n++;
                if (monitor.isCancelled())
                    break;
                Address addr = space.getAddress(off & 0xFFFFFFFFL);
                MemoryBlock block = currentProgram.getMemory().getBlock(addr);
                if (block == null || !block.isInitialized()) {
                    skipped++;
                    continue;
                }
                Function existing = currentProgram.getFunctionManager().getFunctionAt(addr);
                if (existing != null) {
                    already++;
                    continue;
                }
                try {
                    if (currentProgram.getListing().getInstructionAt(addr) == null) {
                        /* Ghidra 12.2: disassemble(Address) returns boolean, not
                         * an AddressSetView as it did in older releases. */
                        if (disassemble(addr))
                            disas++;
                    }
                    if (createFunction(addr, null) != null)
                        created++;
                } catch (Exception e) {
                    /* not every address in the list is a valid instruction start */
                }
                if ((n % 4000) == 0)
                    println("[seed] progress " + n + "/" + starts.size()
                            + " (created=" + created + ", existing=" + already + ")");
            }
            println("[seed] done: created=" + created + ", pre-existing=" + already
                    + ", skipped(no-init-mem)=" + skipped + ", disassembled=" + disas);
        }

        /* Naming-relevant analyzers. Decompiler analyzers are deliberately off:
         * they dominate the run and change prototypes, not names. */
        Map<String, String> opts = getCurrentAnalysisOptionsAndValues(currentProgram);
        enable(opts, "Function ID");
        enable(opts, "Library Identification");
        enable(opts, "Demangler Microsoft");
        enable(opts, "Windows x86 PE RTTI Analyzer");
        enable(opts, "RTTI Analyzer");
        enable(opts, "Scalar Operand References");
        enable(opts, "Reference");
        enable(opts, "Shared Return Calls");
        disable(opts, "Decompiler Parameter ID");
        disable(opts, "Decompiler Switch Analysis");
        println("[pre] seeding + analysis options complete.");
    }
}
