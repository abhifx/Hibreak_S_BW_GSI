package com.hibreak.einkcenter;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;

/**
 * A single long-lived root shell.
 *
 * phh's su authorises once per session, and every new `su -c` also costs a
 * stub round-trip, so the daemon keeps one shell open and writes commands to
 * its stdin.
 */
public final class RootShell {

    private static RootShell instance;

    private Process process;
    private BufferedWriter stdin;

    private RootShell() { }

    public static synchronized RootShell get() {
        if (instance == null) instance = new RootShell();
        return instance;
    }

    public synchronized void cmd(String command) {
        try {
            if (stdin == null) start();
            stdin.write(command);
            stdin.write("\n");
            stdin.flush();
        } catch (Exception e) {
            close();
        }
    }

    private void start() throws Exception {
        process = Runtime.getRuntime().exec(new String[]{"su", "-c", "sh"});
        stdin = new BufferedWriter(new OutputStreamWriter(process.getOutputStream()));
        drain(process.getInputStream());
        drain(process.getErrorStream());
    }

    private void drain(java.io.InputStream in) {
        new Thread(() -> {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(in))) {
                while (r.readLine() != null) {
                    // discard
                }
            } catch (Exception ignored) {
                // shell gone
            }
        }, "root-shell-drain").start();
    }

    private void close() {
        try {
            if (stdin != null) stdin.close();
        } catch (Exception ignored) { }
        if (process != null) process.destroy();
        stdin = null;
        process = null;
    }
}
