package com.hibreak.einkcenter;

import java.util.UUID;

/** Thin wrapper around phh's su, backed by the process-wide root shell. */
public final class Root {

    private Root() { }

    public static String run(String cmd) {
        if (cmd == null || cmd.isEmpty()) return "";
        String marker = "__ROOT_STATUS_" + UUID.randomUUID() + "__";
        try {
            String out = RootShell.get().query(
                    cmd + "\n__root_rc=$?; printf '\\n" + marker + "%d\\n' \"$__root_rc\"",
                    8_000);
            int at = out.indexOf(marker);
            if (at < 0) return "ERR root command timed out";
            String stdout = out.substring(0, at).trim();
            int rc = Integer.parseInt(out.substring(at + marker.length()).trim());
            if (rc == 0) return stdout;
            return stdout.isEmpty() ? "ERR exit " + rc : stdout + "\nERR exit " + rc;
        } catch (Exception e) {
            return "ERR " + e;
        }
    }
}
