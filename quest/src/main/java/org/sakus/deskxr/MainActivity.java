package org.sakus.deskxr;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class MainActivity extends Activity {
    static {
        System.loadLibrary("deskxr_quest");
    }

    private static final String PREFS = "deskxr";
    private static final String KEY_HOST = "host";
    private static final String KEY_PORT = "port";

    private final Handler handler = new Handler(Looper.getMainLooper());

    private EditText hostField;
    private EditText portField;
    private Button startButton;
    private TextView statusView;

    private final Runnable statusPoll = new Runnable() {
        @Override
        public void run() {
            final boolean running = nativeIsRunning();
            statusView.setText(nativeGetStatus());
            startButton.setText(running ? "Stop bridge" : "Start bridge");
            hostField.setEnabled(!running);
            portField.setEnabled(!running);
            handler.postDelayed(this, 350);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);

        final SharedPreferences prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setPadding(64, 48, 64, 48);
        root.setBackgroundColor(Color.rgb(18, 18, 18));

        TextView title = new TextView(this);
        title.setText("DeskXR Quest Bridge");
        title.setTextColor(Color.WHITE);
        title.setTextSize(28f);
        title.setGravity(Gravity.CENTER);
        root.addView(title, matchWrap());

        TextView subtitle = new TextView(this);
        subtitle.setText("Send Quest Touch 6DoF poses to the DeskXR SteamVR driver.");
        subtitle.setTextColor(Color.LTGRAY);
        subtitle.setTextSize(16f);
        subtitle.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams subtitleParams = matchWrap();
        subtitleParams.setMargins(0, 8, 0, 30);
        root.addView(subtitle, subtitleParams);

        hostField = new EditText(this);
        hostField.setHint("PC IPv4 address");
        hostField.setSingleLine(true);
        hostField.setText(prefs.getString(KEY_HOST, "192.168.1.2"));
        hostField.setTextColor(Color.WHITE);
        hostField.setHintTextColor(Color.GRAY);
        root.addView(hostField, fixedWidth(620));

        portField = new EditText(this);
        portField.setHint("UDP port");
        portField.setSingleLine(true);
        portField.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
        portField.setText(Integer.toString(prefs.getInt(KEY_PORT, 39742)));
        portField.setTextColor(Color.WHITE);
        portField.setHintTextColor(Color.GRAY);
        root.addView(portField, fixedWidth(620));

        startButton = new Button(this);
        startButton.setText("Start bridge");
        LinearLayout.LayoutParams buttonParams = fixedWidth(620);
        buttonParams.setMargins(0, 24, 0, 18);
        root.addView(startButton, buttonParams);

        statusView = new TextView(this);
        statusView.setText("Idle");
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(15f);
        statusView.setGravity(Gravity.CENTER);
        root.addView(statusView, matchWrap());

        TextView note = new TextView(this);
        note.setText(
                "Keep this app in the foreground. DeskXR currently sends controller pose/input only; "
                        + "video stays on the PC monitor.");
        note.setTextColor(Color.GRAY);
        note.setTextSize(13f);
        note.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams noteParams = fixedWidth(760);
        noteParams.setMargins(0, 26, 0, 0);
        root.addView(note, noteParams);

        setContentView(root);

        startButton.setOnClickListener(v -> {
            if (nativeIsRunning()) {
                nativeStop();
                return;
            }

            String host = hostField.getText().toString().trim();
            int port = parsePort(portField.getText().toString());

            if (host.isEmpty()) {
                statusView.setText("Enter the PC IPv4 address.");
                return;
            }

            prefs.edit()
                    .putString(KEY_HOST, host)
                    .putInt(KEY_PORT, port)
                    .apply();

            if (!nativeStart(this, host, port)) {
                statusView.setText(nativeGetStatus());
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.removeCallbacks(statusPoll);
        handler.post(statusPoll);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(statusPoll);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        nativeStop();
        super.onDestroy();
    }

    private static int parsePort(String value) {
        try {
            int port = Integer.parseInt(value);
            if (port >= 1 && port <= 65535) {
                return port;
            }
        } catch (NumberFormatException ignored) {
        }
        return 39742;
    }

    private static LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private static LinearLayout.LayoutParams fixedWidth(int width) {
        return new LinearLayout.LayoutParams(
                width,
                LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private static native boolean nativeStart(Activity activity, String host, int port);
    private static native void nativeStop();
    private static native boolean nativeIsRunning();
    private static native String nativeGetStatus();
}
