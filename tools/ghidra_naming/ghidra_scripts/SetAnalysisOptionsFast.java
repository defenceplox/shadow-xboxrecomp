/* Pre-analysis script: recover NAMES fast, skip the slow decompiler analyzers.
 *
 * SetAnalysisOptions.java enables everything useful, including
 * "Decompiler Parameter ID", which decompiles every function to infer
 * signatures. On a 3.9 MB .text (39 k functions) that dominates the run, and it
 * changes signatures, not names.
 *
 * This variant enables the naming analyzers and explicitly DISABLES the two
 * decompiler-dependent ones, which is the right trade when the goal is
 * "replace sub_XXXXXXXX with real symbols" and not "improve prototypes".
 *
 * @category XboxRecomp
 */
import java.util.Map;

import ghidra.app.script.GhidraScript;

public class SetAnalysisOptionsFast extends GhidraScript {

    private void enable(Map<String, String> opts, String name) {
        if (opts.containsKey(name)) {
            setAnalysisOption(currentProgram, name, "true");
            println("[pre] enabled analyzer: " + name);
        } else {
            println("[pre] analyzer not present (skipped): " + name);
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
        Map<String, String> opts = getCurrentAnalysisOptionsAndValues(currentProgram);

        /* naming-critical */
        enable(opts, "Function ID");
        enable(opts, "Library Identification");
        enable(opts, "Demangler Microsoft");

        /* RTTI / vftable names (analyzer name varies by version; try all) */
        enable(opts, "Windows x86 PE RTTI Analyzer");
        enable(opts, "RTTI Analyzer");

        /* code discovery / references for a flat raw image */
        enable(opts, "Aggressive Instruction Finder");
        enable(opts, "Scalar Operand References");
        enable(opts, "Reference");
        enable(opts, "Shared Return Calls");

        /* the expensive pair, off */
        disable(opts, "Decompiler Parameter ID");
        disable(opts, "Decompiler Switch Analysis");

        println("[pre] fast analysis option setup complete.");
    }
}
