package com.koushikdutta.superuser;

import android.app.Activity;
import android.content.Intent;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Bundle;
import android.util.Log;

import java.io.InputStream;
import java.io.OutputStream;

/**
 * Minimal Superuser responder for phh-su.
 *
 * phh-su starts this activity with an extra "socket", writes a flattened
 * request into that unix socket, then waits for a decision string. It accepts
 * exactly "ALLOW" or "DENY" (upper case); anything else is logged by su as
 * "unknown response from Superuser Requestor".
 *
 * This responder grants root only to the E-ink Center app and denies everyone
 * else, so the su prompt cannot be used to escalate from other apps.
 */
public class RequestActivity extends Activity {

    private static final String TAG = "EINKSU";
    private static final String TRUSTED_PACKAGE = "com.hibreak.einkcenter";

    /** UID of the trusted app, so the grant survives reinstalls. */
    private int expectedUid() {
        try {
            return getPackageManager().getPackageUid(TRUSTED_PACKAGE, 0);
        } catch (Exception e) {
            return -1;
        }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final Intent i = getIntent();
        final String path = i.getStringExtra("socket");
        if (path == null) {
            finish();
            return;
        }
        new Thread(() -> {
            LocalSocket s = new LocalSocket();
            try {
                s.connect(new LocalSocketAddress(path, LocalSocketAddress.Namespace.FILESYSTEM));
                s.setSoTimeout(1500);

                StringBuilder req = new StringBuilder();
                InputStream in = s.getInputStream();
                byte[] buf = new byte[2048];
                int n;
                try {
                    while ((n = in.read(buf)) > 0) {
                        req.append(new String(buf, 0, n));
                        if (req.indexOf("eof") >= 0) break;
                    }
                } catch (Exception ignored) {
                    // timeout is normal once the request is complete
                }

                int expected = expectedUid();
                java.util.regex.Matcher m = java.util.regex.Pattern
                        .compile("from\\.uid[^0-9]{0,8}([0-9]+)")
                        .matcher(req.toString());
                boolean allow = expected > 0 && m.find()
                        && Integer.parseInt(m.group(1)) == expected;

                OutputStream out = s.getOutputStream();
                out.write((allow ? "ALLOW" : "DENY").getBytes());
                out.flush();
                Log.i(TAG, "uid-match=" + allow + " -> " + (allow ? "ALLOW" : "DENY"));
            } catch (Exception e) {
                Log.e(TAG, "socket failed", e);
            } finally {
                try {
                    Thread.sleep(120);
                } catch (InterruptedException ignored) { }
                try { s.close(); } catch (Exception ignored) { }
            }
        }).start();
        finish();
    }
}
