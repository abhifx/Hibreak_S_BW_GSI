package com.hibreak.einkcenter;

import android.os.SystemClock;
import android.util.Log;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.util.UUID;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * A single long-lived root shell.
 *
 * <p>{@link #cmd(String)} remains fire-and-forget for the daemon. Queries share
 * the same shell and use a unique stdout sentinel, so mode reads do not start a
 * new {@code su} process. A dead shell or a timed-out query is closed and retried
 * once with a fresh shell.</p>
 */
public final class RootShell {
    private static final String TAG = "EinkRootShell";
    private static RootShell instance;
    private static int generation;

    private Process process;
    private BufferedWriter stdin;
    private final BlockingQueue<String> stdout = new LinkedBlockingQueue<>();

    private RootShell() { }

    public static synchronized RootShell get() {
        if (instance == null) instance = new RootShell();
        return instance;
    }

    public synchronized void cmd(String command) {
        try {
            ensureStarted();
            stdin.write(command);
            stdin.newLine();
            stdin.flush();
        } catch (Exception e) {
            Log.w(TAG, "command failed; root shell will restart", e);
            close();
        }
    }

    public synchronized String query(String command) {
        return query(command, 5_000);
    }

    /**
     * Runs a command and returns stdout before its unique sentinel. Stderr is
     * drained separately so it cannot block the shell. Empty output means either
     * a genuinely empty result or failure; callers that need acknowledgement put
     * their own status line in stdout.
     */
    public synchronized String query(String command, long timeoutMs) {
        if (command == null || command.isEmpty()) return "";
        for (int attempt = 0; attempt < 2; attempt++) {
            String sentinel = "__EINK_QUERY_" + UUID.randomUUID() + "__";
            try {
                ensureStarted();
                stdout.clear();
                stdin.write(command);
                stdin.newLine();
                stdin.write("printf '\\n" + sentinel + "\\n'");
                stdin.newLine();
                stdin.flush();

                long deadline = SystemClock.elapsedRealtime() + Math.max(100, timeoutMs);
                StringBuilder result = new StringBuilder();
                while (true) {
                    long remaining = deadline - SystemClock.elapsedRealtime();
                    if (remaining <= 0) throw new java.io.IOException("root query timed out");
                    String line = stdout.poll(remaining, TimeUnit.MILLISECONDS);
                    if (line == null) continue;
                    if (sentinel.equals(line)) return result.toString().trim();
                    result.append(line).append('\n');
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                close();
                return "";
            } catch (Exception e) {
                Log.w(TAG, "query failed (attempt " + (attempt + 1) + ")", e);
                close();
            }
        }
        return "";
    }

    private void ensureStarted() throws Exception {
        if (stdin == null || process == null || !process.isAlive()) {
            close();
            start();
        }
    }

    private void start() throws Exception {
        process = Runtime.getRuntime().exec(new String[]{"su", "-c", "sh"});
        stdin = new BufferedWriter(new OutputStreamWriter(process.getOutputStream(), "UTF-8"));
        int id = ++generation;
        drain(process.getInputStream(), stdout, "root-shell-stdout-" + id);
        drain(process.getErrorStream(), null, "root-shell-stderr-" + id);
    }

    private void drain(java.io.InputStream in, BlockingQueue<String> queue, String name) {
        new Thread(() -> {
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(in, "UTF-8"))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    if (queue != null) queue.offer(line);
                }
            } catch (Exception ignored) {
                // A closed/replaced shell ends this reader.
            }
        }, name).start();
    }

    private void close() {
        try {
            if (stdin != null) stdin.close();
        } catch (Exception ignored) { }
        if (process != null) process.destroy();
        stdin = null;
        process = null;
        stdout.clear();
    }
}
