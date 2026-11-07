/* Decompile specific addresses (and the function that contains each) and print
 * the C. Built for targeted work: the register-level lift the recompiler emits is
 * faithful but nearly unreadable when the question is "what is the contract of
 * this function", and a full-image decompile pass is minutes of work for one or
 * two answers.
 *
 * Usage (reopen an analysed project, no re-analysis):
 *   analyzeHeadless <proj> shadow -process shadow.img -noanalysis \
 *     -scriptPath <dir> -postScript DecompileAt.java 0x25810 0x17a80 ...
 *
 * @category XboxRecomp
 */
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {

    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.toggleCCode(true);
        d.toggleSyntaxTree(true);
        if (!d.openProgram(currentProgram)) {
            println("decompiler refused the program: " + d.getLastMessage());
            return;
        }
        for (String s : getScriptArgs()) {
            long off;
            try {
                off = Long.parseLong(s.replaceFirst("^0[xX]", ""), 16);
            } catch (Exception e) {
                println("=== bad address: " + s);
                continue;
            }
            Address a = currentProgram.getAddressFactory()
                    .getDefaultAddressSpace().getAddress(off);
            Function f = currentProgram.getFunctionManager().getFunctionContaining(a);
            if (f == null) {
                println("=== 0x" + Long.toHexString(off) + ": no function contains it");
                continue;
            }
            println("");
            println("/* ================= " + f.getName() + " @ "
                    + f.getEntryPoint() + "  (asked for 0x"
                    + Long.toHexString(off) + ") ================= */");
            DecompileResults r = d.decompileFunction(f, 90, monitor);
            if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null)
                println(r.getDecompiledFunction().getC());
            else
                println("/* decompile failed: "
                        + (r != null ? r.getErrorMessage() : "no result") + " */");
        }
        d.dispose();
    }
}
