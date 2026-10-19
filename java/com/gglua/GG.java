package com.gglua;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.util.ArrayList;

public class GG {
    private static final Handler UI = new Handler(Looper.getMainLooper());
    private static final StringBuilder LOG = new StringBuilder();
    private static final ArrayList<String> hist = new ArrayList<String>();
    private static boolean loaded = false;
    private static String loadErr = "";
    private static Activity cur;
    private static View btn, panel;
    private static TextView out;
    private static ScrollView sv;
    private static EditText in;
    private static int hi = 0;

    private static native void nativeInit(String dir);
    private static native String nativeRun(String code);

    public static void init(final Activity act) {
        UI.postDelayed(new Runnable() {
            public void run() {
                try { attach(act); } catch (Throwable t) { }
            }
        }, 2500);
    }

    private static int dp(int v) {
        return (int) (v * cur.getResources().getDisplayMetrics().density + 0.5f);
    }

    private static void attach(Activity act) {
        cur = act;
        ViewGroup decor = (ViewGroup) act.getWindow().getDecorView();
        if (btn != null && btn.getParent() == decor) return;

        if (!loaded) {
            try {
                System.loadLibrary("gglua");
                File[] md = act.getExternalMediaDirs();
                String base = (md != null && md.length > 0 && md[0] != null)
                        ? md[0].getAbsolutePath() : act.getFilesDir().getAbsolutePath();
                String dir = base + "/gg";
                new File(dir).mkdirs();
                nativeInit(dir);
                loaded = true;
                LOG.append("gglua siap. dir: ").append(dir).append("\n");
            } catch (Throwable t) {
                loadErr = String.valueOf(t);
                LOG.append("gagal muat: ").append(loadErr).append("\n");
            }
        }

        LinearLayout p = new LinearLayout(act);
        p.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xE60E1116);
        bg.setCornerRadius(dp(10));
        p.setBackground(bg);
        p.setPadding(dp(8), dp(8), dp(8), dp(8));
        p.setClickable(true);

        out = new TextView(act);
        out.setTextColor(0xFFB8F5B8);
        out.setTypeface(Typeface.MONOSPACE);
        out.setTextSize(11);
        out.setTextIsSelectable(true);
        out.setText(LOG);
        sv = new ScrollView(act);
        sv.addView(out);
        p.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1f));

        LinearLayout row = new LinearLayout(act);
        row.setOrientation(LinearLayout.HORIZONTAL);
        in = new EditText(act);
        in.setTextColor(Color.WHITE);
        in.setHintTextColor(0xFF888888);
        in.setHint("lua> ");
        in.setTypeface(Typeface.MONOSPACE);
        in.setTextSize(13);
        in.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        in.setMaxLines(3);
        in.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { ime(true); }
        });
        row.addView(in, new LinearLayout.LayoutParams(0, -2, 1f));
        row.addView(mk("Run", new View.OnClickListener() {
            public void onClick(View v) { execute(); }
        }));
        row.addView(mk("^", new View.OnClickListener() {
            public void onClick(View v) {
                if (hist.isEmpty()) return;
                hi = Math.max(0, Math.min(hi, hist.size()) - 1);
                in.setText(hist.get(hi));
                in.setSelection(in.length());
            }
        }));
        row.addView(mk("Clr", new View.OnClickListener() {
            public void onClick(View v) { LOG.setLength(0); out.setText(""); }
        }));
        row.addView(mk("X", new View.OnClickListener() {
            public void onClick(View v) { toggle(); }
        }));
        p.addView(row, new LinearLayout.LayoutParams(-1, -2));

        FrameLayout.LayoutParams pl = new FrameLayout.LayoutParams(-1,
                (int) (act.getResources().getDisplayMetrics().heightPixels * 0.5f));
        pl.gravity = Gravity.TOP;
        pl.setMargins(dp(6), dp(6), dp(6), 0);
        p.setVisibility(View.GONE);
        decor.addView(p, pl);
        panel = p;

        TextView b = new TextView(act);
        b.setText("GG");
        b.setTextColor(Color.WHITE);
        b.setTextSize(12);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setGravity(Gravity.CENTER);
        GradientDrawable bb = new GradientDrawable();
        bb.setShape(GradientDrawable.OVAL);
        bb.setColor(0xCC1E88E5);
        b.setBackground(bb);
        b.setOnTouchListener(new View.OnTouchListener() {
            float dx, dy, sx, sy;
            boolean moved;
            public boolean onTouch(View v, MotionEvent e) {
                switch (e.getAction()) {
                    case MotionEvent.ACTION_DOWN:
                        sx = e.getRawX(); sy = e.getRawY();
                        dx = v.getX() - sx; dy = v.getY() - sy;
                        moved = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        if (Math.abs(e.getRawX() - sx) > 12 || Math.abs(e.getRawY() - sy) > 12) moved = true;
                        if (moved) { v.setX(e.getRawX() + dx); v.setY(e.getRawY() + dy); }
                        return true;
                    case MotionEvent.ACTION_UP:
                        if (!moved) toggle();
                        return true;
                }
                return false;
            }
        });
        FrameLayout.LayoutParams bl = new FrameLayout.LayoutParams(dp(44), dp(44));
        bl.gravity = Gravity.TOP | Gravity.LEFT;
        bl.leftMargin = dp(10);
        bl.topMargin = dp(80);
        decor.addView(b, bl);
        btn = b;
    }

    private static Button mk(String t, View.OnClickListener l) {
        Button b = new Button(cur);
        b.setText(t);
        b.setTextSize(11);
        b.setAllCaps(false);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        b.setPadding(dp(8), 0, dp(8), 0);
        b.setOnClickListener(l);
        return b;
    }

    private static void toggle() {
        if (panel.getVisibility() == View.VISIBLE) {
            panel.setVisibility(View.GONE);
            ime(false);
        } else {
            panel.setVisibility(View.VISIBLE);
            in.requestFocus();
            UI.postDelayed(new Runnable() {
                public void run() { ime(true); }
            }, 250);
        }
    }

    private static void ime(boolean show) {
        try {
            InputMethodManager m = (InputMethodManager) cur.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (show) m.showSoftInput(in, InputMethodManager.SHOW_IMPLICIT);
            else m.hideSoftInputFromWindow(in.getWindowToken(), 0);
        } catch (Throwable t) { }
    }

    private static void append(String s) {
        LOG.append(s);
        if (LOG.length() > 30000) LOG.delete(0, LOG.length() - 20000);
        if (out != null) {
            out.setText(LOG);
            sv.post(new Runnable() {
                public void run() { sv.fullScroll(View.FOCUS_DOWN); }
            });
        }
    }

    private static void execute() {
        final String code = in.getText().toString().trim();
        if (code.length() == 0) return;
        hist.add(code);
        hi = hist.size();
        in.setText("");
        append("> " + code + "\n");
        new Thread(new Runnable() {
            public void run() {
                String r;
                try {
                    r = loaded ? nativeRun(code) : "native belum dimuat: " + loadErr + "\n";
                } catch (Throwable t) {
                    r = "error: " + t + "\n";
                }
                final String rr = r;
                UI.post(new Runnable() {
                    public void run() { append(rr); }
                });
            }
        }).start();
    }
}
