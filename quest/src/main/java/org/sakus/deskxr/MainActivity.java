package org.sakus.deskxr;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public final class MainActivity extends Activity {
    static {
        System.loadLibrary("deskxr_quest");
    }

    private static final String PREFS = "deskxr";
    private static final String KEY_HOST = "host";
    private static final String KEY_PORT = "port";
    private static final String KEY_HEAD_HEIGHT = "head_height";
    private static final String KEY_CAMERA_DISTANCE = "camera_distance";
    private static final String KEY_CAMERA_Y = "camera_y";
    private static final String KEY_FACING_USER = "facing_user";

    private final Handler handler = new Handler(Looper.getMainLooper());

    private EditText hostField;
    private EditText portField;
    private EditText headHeightField;
    private EditText cameraDistanceField;
    private EditText cameraYField;
    private CheckBox facingUserField;
    private Button startButton;
    private Button quickCalibrateButton;
    private TextView statusView;

    private final Runnable statusPoll = new Runnable() {
        @Override
        public void run() {
            final boolean running = nativeIsRunning();
            statusView.setText(nativeGetStatus());
            startButton.setText(running ? "Stop bridge" : "Start bridge");
            setConfigurationEnabled(!running);
            quickCalibrateButton.setEnabled(running);
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

        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(64, 48, 64, 48);
        root.setBackgroundColor(Color.rgb(18, 18, 18));
        scroll.addView(root);

        TextView title = label("DeskXR Quest Bridge", 28f, Color.WHITE);
        title.setGravity(Gravity.CENTER);
        root.addView(title, matchWrap());

        TextView subtitle = label(
                "Quest Touch -> SteamVR, while the VR image stays on the PC monitor.",
                16f,
                Color.LTGRAY);
        subtitle.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams subtitleParams = matchWrap();
        subtitleParams.setMargins(0, 8, 0, 28);
        root.addView(subtitle, subtitleParams);

        root.addView(section("Connection"), fixedWidth(700));

        hostField = textField("PC IPv4 address", prefs.getString(KEY_HOST, "192.168.1.2"), false);
        root.addView(hostField, fixedWidth(700));

        portField = textField("UDP port", Integer.toString(prefs.getInt(KEY_PORT, 39742)), true);
        root.addView(portField, fixedWidth(700));

        root.addView(section("Mount calibration"), fixedWidth(700));

        headHeightField = textField(
                "Virtual head height (m)",
                Float.toString(prefs.getFloat(KEY_HEAD_HEIGHT, 1.65f)),
                true);
        root.addView(headHeightField, fixedWidth(700));

        cameraDistanceField = textField(
                "Quest distance in front of head (m)",
                Float.toString(prefs.getFloat(KEY_CAMERA_DISTANCE, 0.70f)),
                true);
        root.addView(cameraDistanceField, fixedWidth(700));

        cameraYField = textField(
                "Quest vertical offset from head (m)",
                Float.toString(prefs.getFloat(KEY_CAMERA_Y, -0.30f)),
                true);
        root.addView(cameraYField, fixedWidth(700));

        facingUserField = new CheckBox(this);
        facingUserField.setText("Quest cameras face me (typical monitor mount)");
        facingUserField.setTextColor(Color.WHITE);
        facingUserField.setChecked(prefs.getBoolean(KEY_FACING_USER, true));
        root.addView(facingUserField, fixedWidth(700));

        TextView calibrationNote = label(
                "Manual values provide the starting transform. For a quick runtime calibration, "
                        + "start the bridge, hold both controllers shoulder-width apart in front of your upper chest, "
                        + "then press Quick calibrate.",
                13f,
                Color.GRAY);
        LinearLayout.LayoutParams calibrationParams = fixedWidth(700);
        calibrationParams.setMargins(0, 4, 0, 12);
        root.addView(calibrationNote, calibrationParams);

        quickCalibrateButton = new Button(this);
        quickCalibrateButton.setText("Quick calibrate from both controllers");
        quickCalibrateButton.setEnabled(false);
        LinearLayout.LayoutParams calibrateButtonParams = fixedWidth(700);
        calibrateButtonParams.setMargins(0, 4, 0, 8);
        root.addView(quickCalibrateButton, calibrateButtonParams);

        startButton = new Button(this);
        startButton.setText("Start bridge");
        LinearLayout.LayoutParams buttonParams = fixedWidth(700);
        buttonParams.setMargins(0, 20, 0, 16);
        root.addView(startButton, buttonParams);

        statusView = label("Idle", 15f, Color.WHITE);
        statusView.setGravity(Gravity.CENTER);
        root.addView(statusView, fixedWidth(760));

        TextView note = label(
                "Keep DeskXR in the foreground. The Quest only supplies tracking/input; "
                        + "VRChat rendering remains on the PC.",
                13f,
                Color.GRAY);
        note.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams noteParams = fixedWidth(760);
        noteParams.setMargins(0, 24, 0, 0);
        root.addView(note, noteParams);

        setContentView(scroll);

        quickCalibrateButton.setOnClickListener(v -> {
            if (!nativeRequestQuickCalibration()) {
                statusView.setText("Quick calibration requires an active OpenXR session.");
            }
        });

        startButton.setOnClickListener(v -> {
            if (nativeIsRunning()) {
                nativeStop();
                return;
            }

            final String host = hostField.getText().toString().trim();
            final int port = parsePort(portField.getText().toString());
            final float headHeight = parseFloat(headHeightField.getText().toString(), 1.65f);
            final float cameraDistance = Math.max(
                    0.0f,
                    parseFloat(cameraDistanceField.getText().toString(), 0.70f));
            final float cameraY = parseFloat(cameraYField.getText().toString(), -0.30f);
            final boolean facingUser = facingUserField.isChecked();

            if (host.isEmpty()) {
                statusView.setText("Enter the PC IPv4 address.");
                return;
            }

            prefs.edit()
                    .putString(KEY_HOST, host)
                    .putInt(KEY_PORT, port)
                    .putFloat(KEY_HEAD_HEIGHT, headHeight)
                    .putFloat(KEY_CAMERA_DISTANCE, cameraDistance)
                    .putFloat(KEY_CAMERA_Y, cameraY)
                    .putBoolean(KEY_FACING_USER, facingUser)
                    .apply();

            if (!nativeStart(
                    this,
                    host,
                    port,
                    headHeight,
                    cameraDistance,
                    cameraY,
                    facingUser)) {
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

    private void setConfigurationEnabled(boolean enabled) {
        hostField.setEnabled(enabled);
        portField.setEnabled(enabled);
        headHeightField.setEnabled(enabled);
        cameraDistanceField.setEnabled(enabled);
        cameraYField.setEnabled(enabled);
        facingUserField.setEnabled(enabled);
    }

    private EditText textField(String hint, String value, boolean numeric) {
        EditText field = new EditText(this);
        field.setHint(hint);
        field.setSingleLine(true);
        field.setText(value);
        field.setTextColor(Color.WHITE);
        field.setHintTextColor(Color.GRAY);
        if (numeric) {
            field.setInputType(
                    InputType.TYPE_CLASS_NUMBER
                            | InputType.TYPE_NUMBER_FLAG_DECIMAL
                            | InputType.TYPE_NUMBER_FLAG_SIGNED);
        }
        return field;
    }

    private TextView section(String text) {
        TextView view = label(text, 17f, Color.WHITE);
        LinearLayout.LayoutParams params = fixedWidth(700);
        params.setMargins(0, 16, 0, 4);
        view.setLayoutParams(params);
        return view;
    }

    private TextView label(String text, float size, int color) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
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

    private static float parseFloat(String value, float fallback) {
        try {
            float parsed = Float.parseFloat(value);
            return Float.isFinite(parsed) ? parsed : fallback;
        } catch (NumberFormatException ignored) {
            return fallback;
        }
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

    private static native boolean nativeStart(
            Activity activity,
            String host,
            int port,
            float headHeight,
            float cameraDistance,
            float cameraYOffset,
            boolean facingUser);

    private static native void nativeStop();
    private static native boolean nativeRequestQuickCalibration();
    private static native boolean nativeIsRunning();
    private static native String nativeGetStatus();
}
