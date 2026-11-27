package com.hibreak.einkcenter;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;

/** Receives cold/warm frontlight values from the SystemUI e-ink shade plugin. */
public class LightControlReceiver extends BroadcastReceiver {
    public static final String ACTION_SET_LIGHT = "com.hibreak.einkcenter.SET_LIGHT";
    private static final String LM = "/sys/devices/platform/*/i2c-*/2-0036/lm3630a_";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !ACTION_SET_LIGHT.equals(intent.getAction())) return;
        int cold = clamp(intent.getIntExtra("cold", -1));
        int warm = clamp(intent.getIntExtra("warm", -1));
        if (cold < 0 && warm < 0) return;

        SharedPreferences prefs = context.getSharedPreferences("eink", Context.MODE_PRIVATE);
        SharedPreferences.Editor edit = prefs.edit();
        if (cold >= 0) edit.putInt("light_cold", cold);
        if (warm >= 0) edit.putInt("light_warm", warm);
        if ((cold > 0) || (warm > 0)) edit.putBoolean("light_on", true);
        edit.apply();

        StringBuilder command = new StringBuilder();
        if (cold >= 0) {
            command.append("P=$(ls ").append(LM).append("cold_light 2>/dev/null | head -1); ")
                    .append("[ -n \"$P\" ] && echo ").append(cold).append(" > \"$P\"; ");
        }
        if (warm >= 0) {
            command.append("P=$(ls ").append(LM).append("warm_light 2>/dev/null | head -1); ")
                    .append("[ -n \"$P\" ] && echo ").append(warm).append(" > \"$P\"");
        }
        RootShell.get().cmd(command.toString());
    }

    private static int clamp(int value) {
        return value < 0 || value > 222 ? -1 : value;
    }
}
