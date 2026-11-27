package com.koushikdutta.superuser;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

/** Probe: logs whatever phh's su hands to the Superuser app. */
public class SuReceiver extends BroadcastReceiver {

    @Override
    public void onReceive(Context context, Intent intent) {
        Log.i("EINKSU", "action=" + intent.getAction() + " data=" + intent.getData()
                + " comp=" + intent.getComponent() + " flags=" + intent.getFlags());
        Bundle b = intent.getExtras();
        if (b != null) {
            for (String k : b.keySet()) {
                Object v = b.get(k);
                Log.i("EINKSU", "extra " + k + " = " + v + " (" + (v == null ? "null" : v.getClass().getName()) + ")");
            }
        } else {
            Log.i("EINKSU", "no extras");
        }
    }
}
