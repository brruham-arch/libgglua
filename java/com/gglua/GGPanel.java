package com.gglua;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

class GGPanel {
    static final String[] TYPES = {"i32", "i8", "i16", "i64", "f32", "f64"};
    static final String[] REGS = {"ca+a", "all", "anon", "native", "ch", "cb", "cd", "cs", "j", "o"};
    static final String LIST = "local t={} for i,h in ipairs(gg.results(50,0)) do "
            + "t[#t+1]=string.format('%x|%s',h.addr,tostring(h.value)) end return table.concat(t,'\\n')";

    interface CB { void done(String out); }

    final Activity a;
    final Handler ui = new Handler(Looper.getMainLooper());
    final ExecutorService ex = Executors.newSingleThreadExecutor();
    final ArrayList<View> btns = new ArrayList<View>();
    LinearLayout guiTab, list;
    TextView status;
    EditText val, wval;
    Spinner typeSp, regSp;
    String curType = "i32";
    boolean busy;

    static LinearLayout build(Activity act) { return new GGPanel(act).guiTab; }

    int dp(int v) { return (int) (v * a.getResources().getDisplayMetrics().density + 0.5f); }

    void run(final String code, final CB cb) {
        ex.execute(new Runnable() {
            public void run() {
                String r;
                try { r = GG.nativeRun(code); } catch (Throwable t) { r = "error: " + t; }
                final String f = r;
                ui.post(new Runnable() { public void run() { cb.done(f); } });
            }
        });
    }

    void setBusy(boolean b) {
        busy = b;
        for (View v : btns) v.setEnabled(!b);
        if (b) status.setText("memproses...");
    }

    void action(String code) {
        if (busy) return;
        setBusy(true);
        run(code, new CB() { public void done(String out) {
            status.setText(out.trim());
            run(LIST, new CB() { public void done(String o2) { setBusy(false); fill(o2); } });
        }});
    }

    void fill(String out) {
        list.removeAllViews();
        for (String ln : out.split("\n")) {
            int p = ln.indexOf('|');
            if (p <= 0 || !ln.substring(0, p).matches("[0-9a-f]+")) continue;
            final String ad = ln.substring(0, p);
            TextView t = new TextView(a);
            t.setText("0x" + ad + "  =  " + ln.substring(p + 1));
            t.setTextColor(0xFFB8F5B8);
            t.setTypeface(Typeface.MONOSPACE);
            t.setTextSize(12);
            t.setPadding(dp(6), dp(8), dp(6), dp(8));
            t.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { edit(ad); } });
            list.addView(t);
        }
    }

    String num(EditText e) {
        String s = e.getText().toString().replaceAll("[^0-9.\\-]", "");
        return s.isEmpty() ? "0" : s;
    }

    String clean(EditText e) { return e.getText().toString().replaceAll("[^0-9.~\\-]", ""); }

    void edit(final String ad) {
        final EditText e = new EditText(a);
        e.setHint("nilai baru");
        e.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        new AlertDialog.Builder(a).setTitle("0x" + ad).setView(e)
                .setPositiveButton("Tulis", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) { action("return gg.write(0x" + ad + "," + num(e) + ",'" + curType + "')"); } })
                .setNeutralButton("Freeze", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) { action("gg.freeze(0x" + ad + "," + num(e) + ",'" + curType + "')"); } })
                .setNegativeButton("Unfreeze", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) { action("gg.unfreeze(0x" + ad + ")"); } })
                .show();
    }

    Button btn(String t, final String code) {
        return btn(t, new View.OnClickListener() { public void onClick(View v) { action(code); } });
    }

    Button btn(String t, View.OnClickListener c) {
        Button b = new Button(a);
        b.setText(t); b.setAllCaps(false); b.setTextSize(11);
        b.setMinWidth(0); b.setMinimumWidth(0); b.setMinHeight(dp(34)); b.setMinimumHeight(dp(34));
        b.setPadding(dp(2), 0, dp(2), 0);
        b.setOnClickListener(c);
        btns.add(b);
        return b;
    }

    LinearLayout row(View... vs) {
        LinearLayout r = new LinearLayout(a);
        for (View v : vs) r.addView(v, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        return r;
    }

    EditText field(String hint) {
        EditText e = new EditText(a);
        e.setHint(hint); e.setHintTextColor(0xFF888888); e.setTextColor(Color.WHITE);
        e.setTextSize(13); e.setSingleLine(true);
        return e;
    }

    Spinner spin(String[] items) {
        Spinner s = new Spinner(a);
        s.setAdapter(new ArrayAdapter<String>(a, android.R.layout.simple_spinner_item, items) {
            public View getView(int p, View c, ViewGroup g) {
                TextView t = (TextView) super.getView(p, c, g);
                t.setTextColor(Color.WHITE); t.setTextSize(12);
                return t;
            }
            public View getDropDownView(int p, View c, ViewGroup g) {
                TextView t = (TextView) super.getDropDownView(p, c, g);
                t.setTextColor(Color.BLACK); t.setBackgroundColor(Color.WHITE);
                return t;
            }
        });
        return s;
    }

    GGPanel(Activity act) {
        a = act;
        guiTab = new LinearLayout(a);
        guiTab.setOrientation(LinearLayout.VERTICAL);
        val = field("nilai atau a~b");
        typeSp = spin(TYPES);
        regSp = spin(REGS);
        wval = field("nilai untuk edit/freeze all");
        wval.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        status = new TextView(a);
        status.setTextColor(0xFFCCCCCC); status.setTextSize(11);
        list = new LinearLayout(a);
        list.setOrientation(LinearLayout.VERTICAL);
        ScrollView sv = new ScrollView(a);
        sv.addView(list);

        guiTab.addView(row(val, typeSp, regSp));
        guiTab.addView(row(
                btn("Search", new View.OnClickListener() { public void onClick(View v) {
                    curType = TYPES[typeSp.getSelectedItemPosition()];
                    action("gg.ranges('" + REGS[regSp.getSelectedItemPosition()] + "') return gg.search('" + clean(val) + "','" + curType + "')");
                }}),
                btn("Refine", new View.OnClickListener() { public void onClick(View v) {
                    action("return gg.refine('" + clean(val) + "')");
                }})));
        guiTab.addView(row(
                btn("Changed", "return gg.changed()"),
                btn("Unchanged", "return gg.unchanged()"),
                btn("Inc", "return gg.increased()"),
                btn("Dec", "return gg.decreased()")));
        guiTab.addView(wval);
        guiTab.addView(row(
                btn("Edit all", new View.OnClickListener() { public void onClick(View v) { action("return gg.editAll(" + num(wval) + ")"); } }),
                btn("Freeze all", new View.OnClickListener() { public void onClick(View v) { action("return gg.freezeAll(" + num(wval) + ")"); } }),
                btn("Unfreeze", "gg.unfreezeAll()"),
                btn("Clear", "gg.clear()")));
        guiTab.addView(status);
        guiTab.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1f));
    }
}
