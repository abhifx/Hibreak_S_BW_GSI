package com.hibreak.einkcenter;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Mode-only per-app profiles, with launchable apps loaded away from the UI thread. */
public class ProfileActivity extends Activity {
    private final ExecutorService loader = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "eink-app-list");
        t.setDaemon(true);
        return t;
    });
    private AppAdapter adapter;
    private TextView status;
    private Switch enabled;
    private AlertDialog choiceDialog;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        EinkUi.prepareActivity(this);
        // The vendor/HWC remains in charge of waveform selection.
        loader.execute(() -> RootShell.get().cmd("setprop vendor.debug.hwc.shim.mode -1"));

        ListView list = new ListView(this);
        list.setBackgroundColor(EinkUi.WHITE);
        list.setCacheColorHint(EinkUi.WHITE);
        list.setSelector(new ColorDrawable(EinkUi.WHITE));
        list.setDivider(new ColorDrawable(EinkUi.BLACK));
        list.setDividerHeight(Math.max(1, EinkUi.dp(this, 1)));
        list.setOverScrollMode(View.OVER_SCROLL_NEVER);
        setContentView(list);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(EinkUi.dp(this, 16), EinkUi.dp(this, 12), EinkUi.dp(this, 16), EinkUi.dp(this, 16));
        list.addHeaderView(root);

        TextView title = EinkUi.text(this, "Per-app modes", 25, EinkUi.BLACK, true);
        title.setPadding(0, EinkUi.dp(this, 8), 0, EinkUi.dp(this, 4));
        root.addView(title);
        status = EinkUi.text(this, "", 12, EinkUi.GRAY, false);
        status.setPadding(0, 0, 0, EinkUi.dp(this, 10));
        root.addView(status);

        enabled = EinkUi.switchControl(this, "Enable per-app profiles",
                ProfileStore.profilesEnabled(this), value -> {
                    ProfileStore.setProfilesEnabled(this, value);
                    refreshStatus();
                    if (adapter != null) adapter.notifyDataSetChanged();
                });
        root.addView(enabled, fullWidth());

        LinearLayout note = EinkUi.card(this, "How this works");
        TextView explanation = EinkUi.text(this,
                "Android and the e-ink composer expose one global refresh mode. "
                        + "The accessibility service switches that single mode when the foreground app changes; "
                        + "it cannot keep different apps in different modes at the same time.\n\n"
                        + "HWC debug mode stays at -1, so the vendor pipeline chooses how each mode is committed.",
                13, EinkUi.BLACK, false);
        explanation.setLineSpacing(EinkUi.dp(this, 2), 1f);
        note.addView(explanation);
        root.addView(note, fullWidth());

        TextView listTitle = EinkUi.text(this, "Launchable apps", 14, EinkUi.BLACK, true);
        listTitle.setPadding(0, EinkUi.dp(this, 4), 0, EinkUi.dp(this, 8));
        root.addView(listTitle);
        status.setText("Loading apps…");
        adapter = new AppAdapter();
        list.setAdapter(adapter);

        loader.execute(() -> {
            List<AppItem> items = loadApps();
            Collections.sort(items, Comparator.comparing(item -> item.label.toLowerCase()));
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                adapter.setItems(items);
                refreshStatus();
            });
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshStatus();
        if (adapter != null) adapter.notifyDataSetChanged();
    }

    @Override
    protected void onDestroy() {
        loader.shutdownNow();
        super.onDestroy();
    }

    private void refreshStatus() {
        if (status == null) return;
        int count = 0;
        for (AppItem item : adapter == null ? new ArrayList<AppItem>() : adapter.items) {
            if (ProfileStore.profileMode(this, item.pkg) != null) count++;
        }
        status.setText(ProfileStore.profilesEnabled(this)
                ? "Profiles on · Global: " + EinkController.label(ProfileStore.globalMode(this))
                        + (count == 0 ? "" : " · " + count + " customized")
                : "Profiles off · Global: " + EinkController.label(ProfileStore.globalMode(this)));
    }

    private List<AppItem> loadApps() {
        List<AppItem> result = new ArrayList<>();
        PackageManager pm = getPackageManager();
        Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        List<ResolveInfo> resolved = pm.queryIntentActivities(launcher, 0);
        Set<String> seen = new HashSet<>();
        for (ResolveInfo info : resolved) {
            if (info == null || info.activityInfo == null) continue;
            String pkg = info.activityInfo.packageName;
            if (pkg == null || pkg.equals(getPackageName()) || !seen.add(pkg)) continue;
            try {
                ApplicationInfo app = pm.getApplicationInfo(pkg, 0);
                String label = pm.getApplicationLabel(app).toString();
                Drawable icon = app.loadIcon(pm);
                if (icon != null) {
                    icon = icon.mutate();
                    icon.setTint(Color.BLACK);
                }
                result.add(new AppItem(label, pkg, icon));
            } catch (Exception ignored) { }
        }
        return result;
    }

    private void showModeDialog(AppItem item) {
        String current = ProfileStore.profileMode(this, item.pkg);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setBackgroundColor(EinkUi.WHITE);
        content.setPadding(EinkUi.dp(this, 12), EinkUi.dp(this, 8), EinkUi.dp(this, 12), EinkUi.dp(this, 8));
        ScrollView choices = new ScrollView(this);
        choices.setOverScrollMode(View.OVER_SCROLL_NEVER);
        choices.addView(content);

        String inherit = current == null
                ? "Inherit global · " + EinkController.label(ProfileStore.globalMode(this))
                : "Remove profile · inherit " + EinkController.label(ProfileStore.globalMode(this));
        addChoice(content, inherit, current == null, () -> saveProfile(item.pkg, null));
        for (String mode : EinkController.modes()) {
            addChoice(content, EinkController.label(mode), mode.equals(current), () -> saveProfile(item.pkg, mode));
        }

        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(item.label)
                .setMessage(item.pkg)
                .setView(choices)
                .setNegativeButton("Cancel", null)
                .create();
        dialog.getWindow().setWindowAnimations(0);
        dialog.setOnShowListener(d -> {
            Button negative = dialog.getButton(AlertDialog.BUTTON_NEGATIVE);
            if (negative != null) EinkUi.styleButton(negative, false, false);
        });
        choiceDialog = dialog;
        dialog.setOnDismissListener(d -> {
            if (choiceDialog == dialog) choiceDialog = null;
        });
        dialog.show();
    }

    private void addChoice(LinearLayout parent, String label, boolean selected, Runnable action) {
        Button button = EinkUi.button(this, label, () -> {
            action.run();
            adapter.notifyDataSetChanged();
            refreshStatus();
            if (choiceDialog != null) choiceDialog.dismiss();
        });
        EinkUi.styleButton(button, selected, false);
        button.setContentDescription(label + (selected ? ", selected" : ""));
        parent.addView(button, fullWidth());
    }

    private void saveProfile(String pkg, String mode) {
        if (mode == null) ProfileStore.removeProfile(this, pkg);
        else ProfileStore.setProfile(this, pkg, mode);
    }

    private LinearLayout.LayoutParams fullWidth() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private static final class AppItem {
        final String label;
        final String pkg;
        final Drawable icon;

        AppItem(String label, String pkg, Drawable icon) {
            this.label = label;
            this.pkg = pkg;
            this.icon = icon;
        }
    }

    private final class AppAdapter extends BaseAdapter {
        final List<AppItem> items = new ArrayList<>();

        void setItems(List<AppItem> value) {
            items.clear();
            items.addAll(value);
            notifyDataSetChanged();
        }

        @Override
        public int getCount() {
            return items.size();
        }

        @Override
        public AppItem getItem(int position) {
            return items.get(position);
        }

        @Override
        public long getItemId(int position) {
            return getItem(position).pkg.hashCode();
        }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            Holder holder;
            if (convertView == null) {
                LinearLayout row = new LinearLayout(ProfileActivity.this);
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setBackground(EinkUi.box(EinkUi.WHITE, EinkUi.BLACK,
                        Math.max(1, EinkUi.dp(ProfileActivity.this, 1)), 0));
                row.setPadding(EinkUi.dp(ProfileActivity.this, 10), EinkUi.dp(ProfileActivity.this, 10),
                        EinkUi.dp(ProfileActivity.this, 10), EinkUi.dp(ProfileActivity.this, 10));
                row.setMinimumHeight(EinkUi.dp(ProfileActivity.this, 76));
                ImageView icon = new ImageView(ProfileActivity.this);
                icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
                LinearLayout text = new LinearLayout(ProfileActivity.this);
                text.setOrientation(LinearLayout.VERTICAL);
                TextView label = EinkUi.text(ProfileActivity.this, "", 16, EinkUi.BLACK, true);
                TextView pkg = EinkUi.text(ProfileActivity.this, "", 11, EinkUi.GRAY, false);
                TextView summary = EinkUi.text(ProfileActivity.this, "", 13, EinkUi.BLACK, false);
                text.addView(label);
                text.addView(pkg);
                text.addView(summary);
                TextView arrow = EinkUi.text(ProfileActivity.this, "›", 25, EinkUi.BLACK, false);
                arrow.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
                row.addView(icon, new LinearLayout.LayoutParams(EinkUi.dp(ProfileActivity.this, 44),
                        EinkUi.dp(ProfileActivity.this, 44)));
                row.addView(text, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
                row.addView(arrow);
                holder = new Holder(icon, label, pkg, summary);
                row.setTag(holder);
                convertView = row;
            } else {
                holder = (Holder) convertView.getTag();
            }
            AppItem item = getItem(position);
            holder.icon.setImageDrawable(item.icon);
            holder.label.setText(item.label);
            holder.pkg.setText(item.pkg);
            holder.summary.setText(ProfileStore.profileSummary(ProfileActivity.this, item.pkg));
            convertView.setContentDescription(item.label + ", " + holder.summary.getText() + ", " + item.pkg);
            convertView.setOnClickListener(v -> showModeDialog(item));
            return convertView;
        }
    }

    private static final class Holder {
        final ImageView icon;
        final TextView label;
        final TextView pkg;
        final TextView summary;

        Holder(ImageView icon, TextView label, TextView pkg, TextView summary) {
            this.icon = icon;
            this.label = label;
            this.pkg = pkg;
            this.summary = summary;
        }
    }
}
