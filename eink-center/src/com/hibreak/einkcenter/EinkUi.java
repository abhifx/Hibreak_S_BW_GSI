package com.hibreak.einkcenter;

import android.app.Activity;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

/** Small programmatic monochrome UI kit shared by the new e-ink-friendly surfaces. */
final class EinkUi {
    static final int BLACK = 0xFF000000;
    static final int WHITE = 0xFFFFFFFF;
    static final int GRAY = 0xFF555555;

    private EinkUi() { }

    static int dp(Context context, int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    static float sp(Context context, float value) {
        return value * context.getResources().getDisplayMetrics().scaledDensity;
    }

    static void prepareActivity(Activity activity) {
        activity.getWindow().setBackgroundDrawable(new ColorDrawable(WHITE));
        activity.getWindow().setWindowAnimations(0);
        activity.getWindow().setStatusBarColor(WHITE);
        activity.getWindow().setNavigationBarColor(WHITE);
        int flags = activity.getWindow().getDecorView().getSystemUiVisibility();
        flags |= View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
        activity.getWindow().getDecorView().setSystemUiVisibility(flags);
    }

    static GradientDrawable box(int fill, int stroke, int strokePx, int radiusPx) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fill);
        drawable.setCornerRadius(radiusPx);
        drawable.setStroke(strokePx, stroke);
        return drawable;
    }

    static void styleButton(Button button, boolean selected, boolean solid) {
        int fill = solid || selected ? BLACK : WHITE;
        int text = solid || selected ? WHITE : BLACK;
        button.setBackground(box(fill, BLACK, Math.max(1, dp(button.getContext(), 1)), dp(button.getContext(), 8)));
        button.setTextColor(text);
        button.setAllCaps(false);
        button.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        button.setStateListAnimator(null);
        button.setElevation(0);
        button.setMinHeight(dp(button.getContext(), 48));
        button.setMinimumHeight(dp(button.getContext(), 48));
        button.setPadding(dp(button.getContext(), 12), dp(button.getContext(), 8),
                dp(button.getContext(), 12), dp(button.getContext(), 8));
        button.jumpDrawablesToCurrentState();
    }

    static Button button(Context context, String label, Runnable action) {
        Button button = new Button(context);
        button.setText(label);
        styleButton(button, false, false);
        button.setOnClickListener(v -> action.run());
        return button;
    }

    static Button solidButton(Context context, String label, Runnable action) {
        Button button = button(context, label, action);
        styleButton(button, false, true);
        return button;
    }

    static TextView text(Context context, String value, int sizeSp, int color, boolean bold) {
        TextView view = new TextView(context);
        view.setText(value);
        view.setTextSize(sizeSp);
        view.setTextColor(color);
        if (bold) view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        view.setIncludeFontPadding(false);
        return view;
    }

    static LinearLayout card(Context context, String title) {
        LinearLayout card = new LinearLayout(context);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(context, 14), dp(context, 12), dp(context, 14), dp(context, 12));
        card.setBackground(box(WHITE, BLACK, Math.max(1, dp(context, 1)), dp(context, 14)));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.bottomMargin = dp(context, 12);
        if (title != null) {
            TextView heading = text(context, title, 13, BLACK, true);
            heading.setLetterSpacing(0.06f);
            heading.setPadding(dp(context, 2), 0, dp(context, 2), dp(context, 6));
            card.addView(heading);
        }
        return card;
    }

    static LinearLayout row(Context context) {
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(android.view.Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(context, 4), 0, dp(context, 4));
        return row;
    }

    static void addWeighted(ViewGroup parent, View view, float weight) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, weight);
        params.setMargins(dp(parent.getContext(), 2), 0, dp(parent.getContext(), 2), 0);
        parent.addView(view, params);
    }

    static void styleSwitch(Switch view) {
        view.setTextColor(BLACK);
        view.setBackgroundColor(WHITE);
        view.setMinHeight(dp(view.getContext(), 48));
        view.setMinimumHeight(dp(view.getContext(), 48));
        view.setPadding(dp(view.getContext(), 4), dp(view.getContext(), 8),
                dp(view.getContext(), 4), dp(view.getContext(), 8));
        view.setThumbTintList(ColorStateList.valueOf(WHITE));
        view.setTrackTintList(ColorStateList.valueOf(BLACK));
        view.setStateListAnimator(null);
        view.setElevation(0);
    }

    static void styleSeekBar(SeekBar view) {
        view.setBackgroundColor(WHITE);
        view.setMinHeight(dp(view.getContext(), 48));
        view.setMinimumHeight(dp(view.getContext(), 48));
        view.setProgressTintList(ColorStateList.valueOf(BLACK));
        view.setSecondaryProgressTintList(ColorStateList.valueOf(BLACK));
        view.setThumbTintList(ColorStateList.valueOf(BLACK));
        view.setStateListAnimator(null);
        view.setElevation(0);
    }

    static void styleEditText(EditText view) {
        view.setTextColor(BLACK);
        view.setHintTextColor(BLACK);
        view.setBackground(box(WHITE, BLACK, Math.max(1, dp(view.getContext(), 1)), dp(view.getContext(), 8)));
        view.setMinHeight(dp(view.getContext(), 48));
        view.setMinimumHeight(dp(view.getContext(), 48));
        view.setPadding(dp(view.getContext(), 10), dp(view.getContext(), 8),
                dp(view.getContext(), 10), dp(view.getContext(), 8));
        view.setStateListAnimator(null);
        view.setElevation(0);
    }

    interface Change { void accept(boolean value); }

    static Switch switchControl(Context context, String label, boolean checked, Change change) {
        Switch view = new Switch(context);
        view.setText(label);
        view.setTextSize(15);
        styleSwitch(view);
        view.setChecked(checked);
        view.jumpDrawablesToCurrentState();
        final boolean[] syncing = {false};
        view.setOnCheckedChangeListener((button, value) -> {
            view.jumpDrawablesToCurrentState();
            if (!syncing[0]) change.accept(value);
        });
        view.setTag(syncing);
        return view;
    }

    static void syncSwitch(Switch view, boolean checked) {
        boolean[] syncing = (boolean[]) view.getTag();
        syncing[0] = true;
        view.setChecked(checked);
        view.jumpDrawablesToCurrentState();
        syncing[0] = false;
    }

    static View divider(Context context) {
        View view = new View(context);
        view.setBackgroundColor(BLACK);
        return view;
    }
}
