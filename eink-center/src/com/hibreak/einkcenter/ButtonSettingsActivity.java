package com.hibreak.einkcenter;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.HashMap;
import java.util.Map;

/** Six independent action slots for the physical scan-647 button. */
public class ButtonSettingsActivity extends Activity {
    private final Map<String, TextView> summaries = new HashMap<>();
    private TextView status;
    private AlertDialog choiceDialog;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        EinkUi.prepareActivity(this);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(EinkUi.dp(this, 16), EinkUi.dp(this, 12), EinkUi.dp(this, 16), EinkUi.dp(this, 24));
        ScrollView scroll = new ScrollView(this);
        scroll.setOverScrollMode(View.OVER_SCROLL_NEVER);
        scroll.addView(root);
        setContentView(scroll);

        TextView title = EinkUi.text(this, "Extra hardware button", 25, EinkUi.BLACK, true);
        title.setPadding(0, EinkUi.dp(this, 8), 0, EinkUi.dp(this, 4));
        root.addView(title);
        status = EinkUi.text(this, "", 12, EinkUi.GRAY, false);
        status.setPadding(0, 0, 0, EinkUi.dp(this, 12));
        root.addView(status);

        LinearLayout note = EinkUi.card(this, "Physical key · scan 647 / PMIC");
        note.addView(EinkUi.text(this,
                "This remaps the extra button's scan-647 events and its mtk-pmic-keys "
                         + "Volume Up events. The real Volume Up rocker on gpio_keys is untouched. "
                         + "Keep the E-ink accessibility service enabled so it can consume the "
                         + "extra-button events before Android's volume dispatch.",
                 13, EinkUi.BLACK, false));
        root.addView(note, fullWidth());

        addSection(root, false, "Screen on");
        addSection(root, true, "Screen off");

        Button reset = EinkUi.solidButton(this, "Reset defaults", () -> {
            ProfileStore.resetButtonActions(this);
            refreshRows();
        });
        root.addView(reset, fullWidth());
        root.addView(EinkUi.text(this,
                "Defaults: single = Full refresh, double = Open E-ink menu, long = Nothing. "
                        + "Every screen-off action is Nothing.",
                12, EinkUi.GRAY, false), fullWidth());
        refreshStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshStatus();
        refreshRows();
    }

    private void addSection(LinearLayout root, boolean screenOff, String title) {
        LinearLayout card = EinkUi.card(this, title);
        root.addView(card, fullWidth());
        addRow(card, screenOff, ProfileStore.PRESS_SINGLE, "Single press", "One short press");
        addRow(card, screenOff, ProfileStore.PRESS_DOUBLE, "Double press", "Two presses within 300 ms");
        addRow(card, screenOff, ProfileStore.PRESS_LONG, "Long press", "Hold for 700 ms");
    }

    private void addRow(LinearLayout parent, boolean screenOff, String press, String title, String detail) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(android.view.Gravity.CENTER_VERTICAL);
        row.setBackground(EinkUi.box(EinkUi.WHITE, EinkUi.BLACK,
                Math.max(1, EinkUi.dp(this, 1)), EinkUi.dp(this, 8)));
        row.setPadding(EinkUi.dp(this, 12), EinkUi.dp(this, 10), EinkUi.dp(this, 12), EinkUi.dp(this, 10));
        row.setMinimumHeight(EinkUi.dp(this, 72));
        LinearLayout labels = new LinearLayout(this);
        labels.setOrientation(LinearLayout.VERTICAL);
        labels.addView(EinkUi.text(this, title, 16, EinkUi.BLACK, true));
        labels.addView(EinkUi.text(this, detail, 11, EinkUi.GRAY, false));
        TextView summary = EinkUi.text(this, "", 14, EinkUi.BLACK, false);
        summary.setGravity(android.view.Gravity.END);
        labels.addView(summary);
        row.addView(labels, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView arrow = EinkUi.text(this, "›", 26, EinkUi.BLACK, false);
        arrow.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        row.addView(arrow);
        row.setOnClickListener(v -> showActionDialog(screenOff, press, title));
        parent.addView(row, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        LinearLayout.LayoutParams params = (LinearLayout.LayoutParams) row.getLayoutParams();
        params.topMargin = EinkUi.dp(this, 6);
        summaries.put(ProfileStore.actionKey(screenOff, press), summary);
    }

    private void showActionDialog(boolean screenOff, String press, String title) {
        String current = ProfileStore.buttonAction(this, screenOff, press);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setBackgroundColor(EinkUi.WHITE);
        content.setPadding(EinkUi.dp(this, 12), EinkUi.dp(this, 8), EinkUi.dp(this, 12), EinkUi.dp(this, 8));
        ScrollView choices = new ScrollView(this);
        choices.setOverScrollMode(View.OVER_SCROLL_NEVER);
        choices.addView(content);
        for (String action : ProfileStore.actions()) {
            String label = ProfileStore.actionLabel(action);
            Button button = EinkUi.button(this, label, () -> {
                ProfileStore.setButtonAction(this, screenOff, press, action);
                refreshRows();
                if (choiceDialog != null) choiceDialog.dismiss();
            });
            EinkUi.styleButton(button, action.equals(current), false);
            button.setContentDescription(label + (action.equals(current) ? ", selected" : ""));
            content.addView(button, fullWidth());
        }
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(title + (screenOff ? " · Screen off" : " · Screen on"))
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

    private void refreshRows() {
        for (boolean off : new boolean[]{false, true}) {
            for (String press : new String[]{ProfileStore.PRESS_SINGLE, ProfileStore.PRESS_DOUBLE, ProfileStore.PRESS_LONG}) {
                TextView summary = summaries.get(ProfileStore.actionKey(off, press));
                if (summary != null) summary.setText(ProfileStore.actionLabel(ProfileStore.buttonAction(this, off, press)));
            }
        }
    }

    private void refreshStatus() {
        if (status == null) return;
        status.setText(EinkAccessibilityService.enabledInSettings(this)
                ? (EinkAccessibilityService.isRunning() ? "Accessibility service connected" : "Service enabled · waiting for connection")
                : "Accessibility service not enabled · open E-ink Center to enable it");
    }

    private LinearLayout.LayoutParams fullWidth() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }
}
