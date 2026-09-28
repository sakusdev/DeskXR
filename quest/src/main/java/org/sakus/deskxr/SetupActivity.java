package org.sakus.deskxr;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public final class SetupActivity extends Activity {
    private static final String PREFS = "deskxr";
    private static final String KEY_HOST = "host";
    private static final String KEY_PORT = "port";
    private static final String KEY_HEAD_HEIGHT = "head_height";
    private static final String KEY_CAMERA_DISTANCE = "camera_distance";
    private static final String KEY_CAMERA_Y = "camera_y";
    private static final String KEY_FACING_USER = "facing_user";

    private EditText hostField;
    private EditText portField;
    private EditText headHeightField;
    private EditText cameraDistanceField;
    private EditText cameraYField;
    private CheckBox facingUserField;
    private TextView statusView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        final SharedPreferences prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE);

        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(48, 40, 48, 40);
        root.setBackgroundColor(Color.rgb(18, 18, 18));
        scroll.addView(root);

        TextView title = label("DeskXR Setup", 28f, Color.WHITE);
        title.setGravity(Gravity.CENTER);
        root.addView(title, matchWrap());

        TextView subtitle = label(
                "Configure the PC bridge first. Starting the bridge will switch into OpenXR mode.",
                15f,
                Color.LTGRAY);
        subtitle.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams subtitleParams = fixedWidth(760);
        subtitleParams.setMargins(0, 8, 0, 28);
        root.addView(subtitle, subtitleParams);

        root.addView(section("Connection"), fixedWidth(720));

        hostField = textField(
                "PC IPv4 address",
                prefs.getString(KEY_HOST, "192.168.1.2"),
                false);
        root.addView(hostField, fixedWidth(720));

        portField = textField(
                "UDP port",
                Integer.toString(prefs.getInt(KEY_PORT, 39742)),
                true);
        root.addView(portField, fixedWidth(720));

        root.addView(section("Initial mount transform"), fixedWidth(720));

        headHeightField = textField(
                "Virtual head height (m)",
                Float.toString(prefs.getFloat(KEY_HEAD_HEIGHT, 1.65f)),
                true);
        root.addView(headHeightField, fixedWidth(720));

        cameraDistanceField = textField(
                "Quest distance in front of head (m)",
                Float.toString(prefs.getFloat(KEY_CAMERA_DISTANCE, 0.70f)),
                true);
        root.addView(cameraDistanceField, fixedWidth(720));

        cameraYField = textField(
                "Quest vertical offset from head (m)",
                Float.toString(prefs.getFloat(KEY_CAMERA_Y, -0.30f)),
                true);
        root.addView(cameraYField, fixedWidth(720));

        facingUserField = new CheckBox(this);
        facingUserField.setText("Quest cameras face me (typical monitor mount)");
        facingUserField.setTextColor(Color.WHITE);
        facingUserField.setChecked(prefs.getBoolean(KEY_FACING_USER, true));
        root.addView(facingUserField, fixedWidth(720));

        Button startButton = new Button(this);
        startButton.setText("Start DeskXR OpenXR bridge");
        LinearLayout.LayoutParams startParams = fixedWidth(720);
        startParams.setMargins(0, 24, 0, 12);
        root.addView(startButton, startParams);

        statusView = label(
                "After pressing Start, the Quest display may go black because DeskXR renders on the PC monitor.",
                13f,
                Color.GRAY);
        statusView.setGravity(Gravity.CENTER);
        root.addView(statusView, fixedWidth(760));

        TextView help = label(
                "For the intended headset-free workflow, you can also launch DeskXR from the Windows DeskXR.ps1 launcher over ADB. "
                        + "That path passes the PC address automatically and enables the unworn proximity override.",
                13f,
                Color.GRAY);
        help.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams helpParams = fixedWidth(760);
        helpParams.setMargins(0, 24, 0, 0);
        root.addView(help, helpParams);

        setContentView(scroll);

        startButton.setOnClickListener(v -> startBridge(prefs));
    }

    private void startBridge(SharedPreferences prefs) {
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

        Intent bridge = new Intent(this, MainActivity.class);
        bridge.putExtra("deskxr_host", host);
        bridge.putExtra("deskxr_port", port);
        bridge.putExtra("deskxr_autostart", true);
        startActivity(bridge);
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
        LinearLayout.LayoutParams params = fixedWidth(720);
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
}
