package com.meshcore.plusplus;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.media.ExifInterface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.InputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;

public class MeshCoreActivity extends Activity {

    static {
        System.loadLibrary("meshcorepp");
    }

    // Native C++20 JNI bridge (crypto.hpp + protocol.hpp + image_encoder.hpp)
    private native int nativeInit(String callsign);
    private native String nativeAddRainbowChannel(String channelName);
    private native byte[] nativeBuildInitCommand(int step);
    private native byte[] nativeBuildSelfAdvert(boolean flood);
    private native byte[] nativeSyncChannelSlot(String channelName);
    private native byte[][] nativeBuildChannelTextFrames(String channelName, String callsign, String text);
    private native byte[][] nativeEncodeAndBuildImageFrames(int[] argbPixels, int width, int height, String channelName);
    private native int[] nativeUnpackMonoBitmapToArgb(byte[] mono180, boolean lightMode);
    private native String nativeDecodeBleFrame(byte[] rawFrame);

    // Nordic UART Service (NUS) UUIDs used by MeshCore Companion firmware
    private static final UUID NUS_SERVICE_UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    private static final UUID NUS_TX_CHAR_UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
    private static final UUID NUS_RX_CHAR_UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
    private static final UUID CCCD_UUID        = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    private static final int REQ_PERMS = 1001;
    private static final int REQ_PICK_IMAGE = 1002;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private String callsign = "MobileOp";
    private String activeChannel = "Public";
    private int activeMobileTab = 0; // 0=Chat, 1=Nodes, 2=Sniffer, 3=Channels, 4=Radio
    private boolean showRoomParticipants = true;

    // Radio state
    private BluetoothAdapter btAdapter;
    private BluetoothGatt activeGatt;
    private BluetoothGattCharacteristic nusTxChar;
    private boolean bleConnected = false;
    private boolean bleScanning = false;
    private String statusText = "DISCONNECTED - Tap [BLE Scan] to connect node";
    private String radioModel = "MeshCore Companion Node";
    private String radioParams = "910.525 MHz | SF7 | BW 62.5 kHz | CR 5";
    private int batteryMv = 0;
    private int batteryPct = 0;
    private int negotiatedMtu = 185;
    private long lastBleTxMs = 0;

    // BLE TX pacing queue (serializes GATT writes + post-idle wake pulse)
    private final ArrayDeque<byte[]> bleTxQueue = new ArrayDeque<>();
    private boolean bleTxInFlight = false;

    public static class ChatMsgItem {
        public String time;
        public String sender;
        public String text;
        public boolean hasRf;
        public int rssi;
        public float snr;
        public int hops;
        public boolean isSelf;
        public boolean isSystem;
        public byte[] monoImage180; // 180 bytes = 48x30 1-bit bitmap (opcode 0x49)
    }

    public static class NodeItem {
        public String name;
        public String keyHex;
        public int type;
        public int hops;
        public int rssi;
        public float snr;
        public int latE6;
        public int lonE6;
    }

    public static class SnifferItem {
        public String name;
        public String hashHex;
        public String pskHex;
        public int msgCount;
        public int lastRssi;
        public float lastSnr;
    }

    public static class BleDeviceItem {
        public String name;
        public String address;
        public int rssi;
        public BluetoothDevice device;
    }

    private final List<String> joinedChannels = new ArrayList<>(Arrays.asList(
            "Public", "#meshcore", "#emcomm", "#weather", "#general", "#hamradio"
    ));
    private final Map<String, List<ChatMsgItem>> channelMessages = new LinkedHashMap<>();
    private final Map<String, NodeItem> meshNodes = new LinkedHashMap<>();
    private final Map<String, SnifferItem> snifferChannels = new LinkedHashMap<>();
    private final Map<String, BleDeviceItem> discoveredBle = new LinkedHashMap<>();
    private final String[] hwSlots = new String[8];
    private final ArrayDeque<String> recentDedup = new ArrayDeque<>();
    private int totalRainbowCandidates = 0;

    // UI Views
    private TextView tvTopStatus;
    private TextView tvTopTelemetry;
    private LinearLayout channelPillsContainer;
    private LinearLayout mainContentFrame;
    private ScrollView chatScrollView;
    private LinearLayout chatFeedLayout;
    private EditText etChatInput;
    private final Button[] bottomTabButtons = new Button[5];

    private int dp(int v) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private static GradientDrawable roundBg(int fillColor, int strokeColor, int strokeWidthPx, int radiusPx) {
        GradientDrawable gd = new GradientDrawable();
        gd.setColor(fillColor);
        gd.setCornerRadius(radiusPx);
        if (strokeWidthPx > 0) {
            gd.setStroke(strokeWidthPx, strokeColor);
        }
        return gd;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        SharedPreferences prefs = getSharedPreferences("meshcore_prefs", MODE_PRIVATE);
        callsign = prefs.getString("callsign", "MobileOp");

        hwSlots[0] = "Public (0x11)";
        hwSlots[1] = "#meshcore";
        hwSlots[2] = "#emcomm";
        hwSlots[3] = "#weather";
        hwSlots[4] = "#general";
        for (int i = 5; i < 8; i++) hwSlots[i] = "<empty>";

        totalRainbowCandidates = nativeInit(callsign);
        for (String ch : Arrays.asList("#meshcore", "#emcomm", "#weather", "#general", "#hamradio", "#sar", "#test", "#tactical")) {
            registerSnifferChannel(ch);
        }

        for (String ch : joinedChannels) {
            channelMessages.put(ch, new ArrayList<ChatMsgItem>());
        }
        appendSystemMessage("Public", "MeshCore++ Tactical Mobile (Android C++20 JNI) initialized — "
                + totalRainbowCandidates + " rainbow channels across 256 SHA-256 buckets.");
        appendSystemMessage("Public", "Tap [Img] next to composer to dither & auto-send a single-packet 48x30 1-bit bitmap (opcode 0x49).");
        appendSystemMessage("Public", "Public ROOM USERS shows every station heard on the mesh. Double-tap any user in ROOM USERS to open a Direct Message (@user)!");

        BluetoothManager bm = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        if (bm != null) btAdapter = bm.getAdapter();

        buildMobileLayout();
        requestBlePermissionsIfNeeded();
    }

    private void registerSnifferChannel(String ch) {
        String info = nativeAddRainbowChannel(ch);
        if (info != null && !info.isEmpty()) {
            String[] parts = info.split("\t");
            if (parts.length >= 3) {
                SnifferItem it = snifferChannels.get(parts[0]);
                if (it == null) {
                    it = new SnifferItem();
                    it.name = parts[0];
                    it.hashHex = parts[1];
                    it.pskHex = parts[2];
                    snifferChannels.put(parts[0], it);
                }
            }
        }
    }

    private void buildMobileLayout() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.parseColor("#090D16"));

        // 1. Top Status Bar
        LinearLayout topBar = new LinearLayout(this);
        topBar.setOrientation(LinearLayout.VERTICAL);
        topBar.setBackgroundColor(Color.parseColor("#0B111E"));
        topBar.setPadding(dp(12), dp(10), dp(12), dp(8));

        LinearLayout topRow1 = new LinearLayout(this);
        topRow1.setOrientation(LinearLayout.HORIZONTAL);
        topRow1.setGravity(Gravity.CENTER_VERTICAL);

        TextView tvBrand = new TextView(this);
        tvBrand.setText("MESHCORE++");
        tvBrand.setTextColor(Color.parseColor("#38BDF8"));
        tvBrand.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        tvBrand.setTypeface(Typeface.DEFAULT_BOLD);
        topRow1.addView(tvBrand);

        tvTopStatus = new TextView(this);
        tvTopStatus.setText("  ● " + statusText);
        tvTopStatus.setTextColor(Color.parseColor("#94A3B8"));
        tvTopStatus.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        tvTopStatus.setSingleLine(true);
        LinearLayout.LayoutParams stLp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        tvTopStatus.setLayoutParams(stLp);
        topRow1.addView(tvTopStatus);

        Button btnBleQuick = new Button(this);
        btnBleQuick.setText("BLE Scan");
        btnBleQuick.setTextColor(Color.parseColor("#38BDF8"));
        btnBleQuick.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        btnBleQuick.setAllCaps(false);
        btnBleQuick.setPadding(dp(10), dp(2), dp(10), dp(2));
        btnBleQuick.setBackground(roundBg(Color.parseColor("#112236"), Color.parseColor("#38BDF8"), dp(1), dp(6)));
        btnBleQuick.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startBleScanAndAutoConnect();
            }
        });
        topRow1.addView(btnBleQuick, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(30)));
        topBar.addView(topRow1);

        tvTopTelemetry = new TextView(this);
        tvTopTelemetry.setText("Callsign: " + callsign + "  |  " + radioParams + "  |  0x49 48x30 1-Bit Ready");
        tvTopTelemetry.setTextColor(Color.parseColor("#64748B"));
        tvTopTelemetry.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        tvTopTelemetry.setPadding(0, dp(3), 0, 0);
        topBar.addView(tvTopTelemetry);

        root.addView(topBar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // 2. Main Tab Content Area
        mainContentFrame = new LinearLayout(this);
        mainContentFrame.setOrientation(LinearLayout.VERTICAL);
        root.addView(mainContentFrame, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        // 3. Bottom 5-Tab Bar
        LinearLayout bottomNav = new LinearLayout(this);
        bottomNav.setOrientation(LinearLayout.HORIZONTAL);
        bottomNav.setBackgroundColor(Color.parseColor("#0B111E"));
        bottomNav.setPadding(dp(6), dp(6), dp(6), dp(6));

        final String[] tabNames = {"Chat", "Nodes", "Sniffer", "Channels", "Radio"};
        for (int i = 0; i < 5; i++) {
            final int tabIdx = i;
            Button b = new Button(this);
            b.setText(tabNames[i]);
            b.setAllCaps(false);
            b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
            b.setPadding(dp(4), dp(4), dp(4), dp(4));
            b.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    switchMobileTab(tabIdx);
                }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(42), 1f);
            lp.setMargins(dp(3), 0, dp(3), 0);
            bottomNav.addView(b, lp);
            bottomTabButtons[i] = b;
        }
        root.addView(bottomNav, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        setContentView(root);
        switchMobileTab(0);
    }

    private void updateHeaderUi() {
        if (tvTopStatus == null) return;
        tvTopStatus.setText("  ● " + statusText);
        tvTopStatus.setTextColor(bleConnected ? Color.parseColor("#10B981")
                : (bleScanning ? Color.parseColor("#38BDF8") : Color.parseColor("#F59E0B")));
        String batStr = batteryMv > 0 ? String.format(Locale.US, " | %.2fV (%d%%)", batteryMv / 1000.0, batteryPct) : "";
        tvTopTelemetry.setText("Callsign: " + callsign + " | " + radioParams + batStr);
    }

    private void switchMobileTab(int idx) {
        activeMobileTab = idx;
        for (int i = 0; i < 5; i++) {
            boolean sel = (i == activeMobileTab);
            bottomTabButtons[i].setTextColor(sel ? Color.parseColor("#38BDF8") : Color.parseColor("#94A3B8"));
            bottomTabButtons[i].setTypeface(sel ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
            bottomTabButtons[i].setBackground(roundBg(
                    sel ? Color.parseColor("#132842") : Color.parseColor("#0F172A"),
                    sel ? Color.parseColor("#38BDF8") : Color.parseColor("#1E293B"),
                    dp(1), dp(8)));
        }

        mainContentFrame.removeAllViews();
        if (idx == 0) {
            buildChatTabView();
        } else if (idx == 1) {
            buildNodesTabView();
        } else if (idx == 2) {
            buildSnifferTabView();
        } else if (idx == 3) {
            buildChannelsTabView();
        } else {
            buildRadioTabView();
        }
    }

    private void buildChatTabView() {
        // Horizontal Channel Pill Bar
        HorizontalScrollView hsv = new HorizontalScrollView(this);
        hsv.setHorizontalScrollBarEnabled(false);
        hsv.setBackgroundColor(Color.parseColor("#0D1526"));
        hsv.setPadding(dp(8), dp(6), dp(8), dp(6));

        channelPillsContainer = new LinearLayout(this);
        channelPillsContainer.setOrientation(LinearLayout.HORIZONTAL);
        for (final String ch : joinedChannels) {
            boolean sel = ch.equalsIgnoreCase(activeChannel);
            Button pill = new Button(this);
            pill.setText(ch);
            pill.setAllCaps(false);
            pill.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
            pill.setTextColor(sel ? Color.parseColor("#38BDF8") : Color.parseColor("#CBD5E1"));
            pill.setPadding(dp(12), dp(2), dp(12), dp(2));
            pill.setBackground(roundBg(
                    sel ? Color.parseColor("#142C46") : Color.parseColor("#111B2E"),
                    sel ? Color.parseColor("#38BDF8") : Color.parseColor("#1E293B"),
                    dp(1), dp(14)));
            pill.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    selectChannel(ch);
                }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, dp(32));
            lp.setMargins(0, 0, dp(6), 0);
            channelPillsContainer.addView(pill, lp);
        }

        Button btnAddCh = new Button(this);
        btnAddCh.setText("+ #Join");
        btnAddCh.setAllCaps(false);
        btnAddCh.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        btnAddCh.setTextColor(Color.parseColor("#10B981"));
        btnAddCh.setPadding(dp(10), dp(2), dp(10), dp(2));
        btnAddCh.setBackground(roundBg(Color.parseColor("#0D2822"), Color.parseColor("#10B981"), dp(1), dp(14)));
        btnAddCh.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                promptJoinHashtagChannel();
            }
        });
        LinearLayout.LayoutParams addChLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, dp(32));
        addChLp.setMargins(0, 0, dp(6), 0);
        channelPillsContainer.addView(btnAddCh, addChLp);

        // Count unique participants in activeChannel (always includes self + speakers + Public mesh chat nodes)
        final Map<String, Integer> roomNicks = new LinkedHashMap<>();
        List<ChatMsgItem> curMsgs = channelMessages.get(activeChannel);
        if (curMsgs != null) {
            for (int i = curMsgs.size() - 1; i >= 0; --i) {
                ChatMsgItem m = curMsgs.get(i);
                if (!m.isSystem && m.sender != null && !m.sender.isEmpty() && !m.sender.equalsIgnoreCase(callsign)) {
                    Integer prev = roomNicks.get(m.sender);
                    roomNicks.put(m.sender, prev == null ? 1 : prev + 1);
                }
            }
        }
        if ("Public".equalsIgnoreCase(activeChannel)) {
            for (List<ChatMsgItem> allM : channelMessages.values()) {
                for (int i = allM.size() - 1; i >= 0; --i) {
                    ChatMsgItem m = allM.get(i);
                    if (!m.isSystem && m.sender != null && !m.sender.isEmpty()
                            && !m.sender.equalsIgnoreCase(callsign) && !roomNicks.containsKey(m.sender)) {
                        roomNicks.put(m.sender, 1);
                    }
                }
            }
            for (NodeItem nd : meshNodes.values()) {
                if (nd.name != null && !nd.name.isEmpty()
                        && !nd.name.equalsIgnoreCase(callsign) && !roomNicks.containsKey(nd.name)) {
                    roomNicks.put(nd.name, 0);
                }
            }
        }
        final int roomUserCount = 1 + roomNicks.size();

        Button btnUsers = new Button(this);
        btnUsers.setText("Users: " + roomUserCount);
        btnUsers.setAllCaps(false);
        btnUsers.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        btnUsers.setTextColor(showRoomParticipants ? Color.parseColor("#10B981") : Color.parseColor("#38BDF8"));
        btnUsers.setPadding(dp(10), dp(2), dp(10), dp(2));
        btnUsers.setBackground(roundBg(
                showRoomParticipants ? Color.parseColor("#0D2822") : Color.parseColor("#111B2E"),
                showRoomParticipants ? Color.parseColor("#10B981") : Color.parseColor("#38BDF8"),
                dp(1), dp(14)));
        btnUsers.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showRoomParticipants = !showRoomParticipants;
                switchMobileTab(activeMobileTab);
            }
        });
        channelPillsContainer.addView(btnUsers, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, dp(32)));

        hsv.addView(channelPillsContainer);
        mainContentFrame.addView(hsv, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // Middle Content Area: Scrollable Message Feed + Optional mIRC-Style Room Participant Sidebar
        LinearLayout middleRow = new LinearLayout(this);
        middleRow.setOrientation(LinearLayout.HORIZONTAL);

        chatScrollView = new ScrollView(this);
        chatScrollView.setFillViewport(true);
        chatFeedLayout = new LinearLayout(this);
        chatFeedLayout.setOrientation(LinearLayout.VERTICAL);
        chatFeedLayout.setPadding(dp(10), dp(8), dp(10), dp(8));
        chatScrollView.addView(chatFeedLayout);
        middleRow.addView(chatScrollView, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));

        if (showRoomParticipants) {
            ScrollView nickScroll = new ScrollView(this);
            nickScroll.setBackgroundColor(Color.parseColor("#0B111E"));
            LinearLayout nickCol = new LinearLayout(this);
            nickCol.setOrientation(LinearLayout.VERTICAL);
            nickCol.setPadding(dp(6), dp(6), dp(6), dp(6));

            final TextView tvHdr = new TextView(this);
            tvHdr.setText("ROOM USERS (" + roomUserCount + ")");
            tvHdr.setTextColor(Color.parseColor("#00E5FF"));
            tvHdr.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
            tvHdr.setTypeface(Typeface.DEFAULT_BOLD);
            tvHdr.setPadding(dp(4), dp(2), dp(4), dp(4));
            nickCol.addView(tvHdr);

            // Search input box for ROOM USERS
            final EditText etNickSearch = new EditText(this);
            etNickSearch.setHint("Search users...");
            etNickSearch.setHintTextColor(Color.parseColor("#64748B"));
            etNickSearch.setTextColor(Color.parseColor("#E2E8F0"));
            etNickSearch.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
            etNickSearch.setSingleLine(true);
            etNickSearch.setPadding(dp(6), dp(4), dp(6), dp(4));
            etNickSearch.setBackground(roundBg(Color.parseColor("#0D1829"), Color.parseColor("#38BDF8"), dp(1), dp(6)));
            LinearLayout.LayoutParams searchLp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            searchLp.setMargins(0, 0, 0, dp(6));
            nickCol.addView(etNickSearch, searchLp);

            // Row 0: Self ([YOU])
            TextView tvSelf = new TextView(this);
            tvSelf.setText("● +" + callsign + " [YOU]");
            tvSelf.setTextColor(Color.parseColor("#10B981"));
            tvSelf.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
            tvSelf.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
            tvSelf.setPadding(dp(6), dp(5), dp(6), dp(5));
            tvSelf.setBackground(roundBg(Color.parseColor("#0D2822"), Color.parseColor("#10B981"), dp(1), dp(6)));
            LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            slp.setMargins(0, 0, 0, dp(4));
            nickCol.addView(tvSelf, slp);

            final List<TextView> userViews = new ArrayList<>();
            final List<String> userNames = new ArrayList<>();
            for (Map.Entry<String, Integer> entry : roomNicks.entrySet()) {
                final String uName = entry.getKey();
                final int mCnt = entry.getValue();
                NodeItem nd = meshNodes.get(uName);
                String prefix = (nd != null && nd.type == 3) ? "@" : (nd != null && nd.type == 2) ? "^" : "+";
                int uCol = (nd != null && nd.type == 3) ? Color.parseColor("#C084FC")
                         : (nd != null && nd.type == 2) ? Color.parseColor("#38BDF8")
                                                        : Color.parseColor("#E2E8F0");

                TextView tvU = new TextView(this);
                tvU.setText("● " + prefix + uName + " (" + mCnt + "m)");
                tvU.setTextColor(uCol);
                tvU.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
                tvU.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
                tvU.setPadding(dp(6), dp(5), dp(6), dp(5));
                tvU.setBackground(roundBg(Color.parseColor("#111B2E"), Color.parseColor("#1E293B"), dp(1), dp(6)));
                tvU.setOnClickListener(new View.OnClickListener() {
                    private long lastTapMs = 0;
                    @Override
                    public void onClick(View v) {
                        long now = System.currentTimeMillis();
                        if (now - lastTapMs <= 650) {
                            lastTapMs = 0;
                            String dmTab = "@" + uName;
                            if (!joinedChannels.contains(dmTab)) {
                                joinedChannels.add(dmTab);
                                channelMessages.put(dmTab, new ArrayList<ChatMsgItem>());
                                appendSystemMessage(dmTab, "Direct Message session with " + uName);
                            }
                            activeChannel = dmTab;
                            switchMobileTab(0);
                            return;
                        }
                        lastTapMs = now;
                        if (etChatInput != null) {
                            String cur = etChatInput.getText().toString();
                            String tag = "@[" + uName + "] ";
                            if (!cur.startsWith(tag)) {
                                etChatInput.setText(tag + cur);
                                etChatInput.setSelection(etChatInput.getText().length());
                            }
                        }
                    }
                });
                LinearLayout.LayoutParams ulp = new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                ulp.setMargins(0, 0, 0, dp(4));
                nickCol.addView(tvU, ulp);
                userViews.add(tvU);
                userNames.add(uName.toLowerCase(Locale.US));
            }

            etNickSearch.addTextChangedListener(new TextWatcher() {
                @Override
                public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                @Override
                public void onTextChanged(CharSequence s, int start, int before, int count) {
                    String q = s == null ? "" : s.toString().trim().toLowerCase(Locale.US);
                    int shown = 1;
                    for (int i = 0; i < userViews.size(); i++) {
                        boolean match = q.isEmpty() || userNames.get(i).contains(q);
                        userViews.get(i).setVisibility(match ? View.VISIBLE : View.GONE);
                        if (match) shown++;
                    }
                    tvHdr.setText(q.isEmpty()
                            ? ("ROOM USERS (" + roomUserCount + ")")
                            : ("ROOM USERS (" + shown + "/" + roomUserCount + ")"));
                }
                @Override
                public void afterTextChanged(Editable s) {}
            });

            nickScroll.addView(nickCol);
            middleRow.addView(nickScroll, new LinearLayout.LayoutParams(
                    dp(145), ViewGroup.LayoutParams.MATCH_PARENT));
        }

        mainContentFrame.addView(middleRow, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        renderChatMessages();

        // Bottom Composer Bar: [Img] + EditText + [TX]
        LinearLayout composer = new LinearLayout(this);
        composer.setOrientation(LinearLayout.HORIZONTAL);
        composer.setGravity(Gravity.CENTER_VERTICAL);
        composer.setBackgroundColor(Color.parseColor("#0D1526"));
        composer.setPadding(dp(8), dp(6), dp(8), dp(6));

        Button btnImg = new Button(this);
        btnImg.setText("[Img]");
        btnImg.setAllCaps(false);
        btnImg.setTextColor(Color.parseColor("#38BDF8"));
        btnImg.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        btnImg.setTypeface(Typeface.DEFAULT_BOLD);
        btnImg.setPadding(dp(10), dp(4), dp(10), dp(4));
        btnImg.setBackground(roundBg(Color.parseColor("#10243A"), Color.parseColor("#38BDF8"), dp(1), dp(8)));
        btnImg.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openImagePicker();
            }
        });
        LinearLayout.LayoutParams imgLp = new LinearLayout.LayoutParams(dp(58), dp(40));
        imgLp.setMargins(0, 0, dp(6), 0);
        composer.addView(btnImg, imgLp);

        etChatInput = new EditText(this);
        etChatInput.setHint("Message " + activeChannel + "...");
        etChatInput.setHintTextColor(Color.parseColor("#64748B"));
        etChatInput.setTextColor(Color.parseColor("#F1F5F9"));
        etChatInput.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        etChatInput.setSingleLine(true);
        etChatInput.setPadding(dp(10), dp(6), dp(10), dp(6));
        etChatInput.setBackground(roundBg(Color.parseColor("#060A12"), Color.parseColor("#1E293B"), dp(1), dp(8)));
        LinearLayout.LayoutParams etLp = new LinearLayout.LayoutParams(0, dp(40), 1f);
        etLp.setMargins(0, 0, dp(6), 0);
        composer.addView(etChatInput, etLp);

        Button btnTx = new Button(this);
        btnTx.setText("TX");
        btnTx.setAllCaps(false);
        btnTx.setTextColor(Color.parseColor("#060A12"));
        btnTx.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        btnTx.setTypeface(Typeface.DEFAULT_BOLD);
        btnTx.setBackground(roundBg(Color.parseColor("#38BDF8"), Color.parseColor("#38BDF8"), 0, dp(8)));
        btnTx.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sendCurrentChatText();
            }
        });
        composer.addView(btnTx, new LinearLayout.LayoutParams(dp(54), dp(40)));

        mainContentFrame.addView(composer, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    private void renderChatMessages() {
        if (chatFeedLayout == null) return;
        chatFeedLayout.removeAllViews();
        List<ChatMsgItem> list = channelMessages.get(activeChannel);
        if (list == null) return;

        for (ChatMsgItem m : list) {
            LinearLayout card = new LinearLayout(this);
            card.setOrientation(LinearLayout.VERTICAL);
            card.setPadding(dp(10), dp(7), dp(10), dp(7));
            int bgCol = m.isSelf ? Color.parseColor("#0F243C")
                    : (m.isSystem ? Color.parseColor("#0D1524") : Color.parseColor("#111B2E"));
            int borderCol = m.isSelf ? Color.parseColor("#38BDF8")
                    : (m.isSystem ? Color.parseColor("#1E293B") : Color.parseColor("#23324C"));
            card.setBackground(roundBg(bgCol, borderCol, dp(1), dp(8)));

            // Header row: [time] <sender> + RF badge
            TextView tvHdr = new TextView(this);
            String rfBadge = m.hasRf
                    ? String.format(Locale.US, "  [%d dBm | %+.1f dB | %dh]", m.rssi, m.snr, m.hops)
                    : (m.isSelf ? "  [TX RF]" : "");
            tvHdr.setText("[" + m.time + "]  <" + m.sender + ">" + rfBadge);
            tvHdr.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
            tvHdr.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
            tvHdr.setTextColor(m.isSelf ? Color.parseColor("#38BDF8")
                    : (m.isSystem ? Color.parseColor("#94A3B8") : Color.parseColor("#10B981")));
            card.addView(tvHdr);

            // Body text
            if (m.text != null && !m.text.isEmpty()) {
                TextView tvBody = new TextView(this);
                tvBody.setText(m.text);
                tvBody.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
                tvBody.setTextColor(Color.parseColor("#E2E8F0"));
                tvBody.setPadding(0, dp(3), 0, 0);
                card.addView(tvBody);
            }

            // Step 4 Critical Display Requirement: 48x30 1-bit bitmap scaled 5x (240x150 dp)
            // using strictly Nearest-Neighbor interpolation (filter = false)
            if (m.monoImage180 != null && m.monoImage180.length == 180) {
                int[] argb48x30 = nativeUnpackMonoBitmapToArgb(m.monoImage180, false);
                if (argb48x30 != null && argb48x30.length == 48 * 30) {
                    Bitmap rawBmp = Bitmap.createBitmap(argb48x30, 48, 30, Bitmap.Config.ARGB_8888);
                    int targetW = dp(240);
                    int targetH = dp(150);
                    Bitmap scaledNearest = Bitmap.createScaledBitmap(rawBmp, targetW, targetH, false);

                    ImageView iv = new ImageView(this);
                    iv.setImageBitmap(scaledNearest);
                    iv.setScaleType(ImageView.ScaleType.FIT_START);
                    iv.setPadding(dp(2), dp(6), dp(2), dp(2));
                    LinearLayout.LayoutParams ivLp = new LinearLayout.LayoutParams(targetW, targetH);
                    ivLp.topMargin = dp(4);
                    card.addView(iv, ivLp);
                }
            }

            LinearLayout.LayoutParams cardLp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            cardLp.bottomMargin = dp(6);
            chatFeedLayout.addView(card, cardLp);
        }

        if (chatScrollView != null) {
            chatScrollView.post(new Runnable() {
                @Override
                public void run() {
                    chatScrollView.fullScroll(View.FOCUS_DOWN);
                }
            });
        }
    }

    private void buildNodesTabView() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(12), dp(10), dp(12), dp(10));

        LinearLayout hdr = new LinearLayout(this);
        hdr.setOrientation(LinearLayout.HORIZONTAL);
        hdr.setGravity(Gravity.CENTER_VERTICAL);

        TextView tvTitle = new TextView(this);
        tvTitle.setText("HEARD MESH NODES (" + meshNodes.size() + ")");
        tvTitle.setTextColor(Color.parseColor("#38BDF8"));
        tvTitle.setTypeface(Typeface.DEFAULT_BOLD);
        tvTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        hdr.addView(tvTitle, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Button btnAdvert = new Button(this);
        btnAdvert.setText("TX Self Advert");
        btnAdvert.setAllCaps(false);
        btnAdvert.setTextColor(Color.parseColor("#10B981"));
        btnAdvert.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        btnAdvert.setBackground(roundBg(Color.parseColor("#0D2822"), Color.parseColor("#10B981"), dp(1), dp(8)));
        btnAdvert.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                byte[] pkt = nativeBuildSelfAdvert(true);
                enqueueBleFrame(pkt);
                Toast.makeText(MeshCoreActivity.this, "Broadcasted Flood Self Advert (0x07)", Toast.LENGTH_SHORT).show();
            }
        });
        hdr.addView(btnAdvert, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(36)));
        box.addView(hdr);

        ScrollView sv = new ScrollView(this);
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(0, dp(8), 0, dp(8));

        if (meshNodes.isEmpty()) {
            TextView empty = new TextView(this);
            empty.setText("No nodes heard yet. Connect your BLE radio or tap TX Self Advert.");
            empty.setTextColor(Color.parseColor("#64748B"));
            empty.setPadding(0, dp(12), 0, 0);
            list.addView(empty);
        } else {
            for (NodeItem nd : meshNodes.values()) {
                LinearLayout row = new LinearLayout(this);
                row.setOrientation(LinearLayout.VERTICAL);
                row.setPadding(dp(10), dp(8), dp(10), dp(8));
                row.setBackground(roundBg(Color.parseColor("#111B2E"), Color.parseColor("#1E293B"), dp(1), dp(8)));

                String role = (nd.type == 2) ? "REPEATER" : (nd.type == 3 ? "ROOM" : "NODE");
                TextView t1 = new TextView(this);
                t1.setText(nd.name + "  [" + nd.keyHex + "]  • " + role);
                t1.setTextColor(Color.parseColor("#F1F5F9"));
                t1.setTypeface(Typeface.DEFAULT_BOLD);
                t1.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
                row.addView(t1);

                TextView t2 = new TextView(this);
                String gps = (nd.latE6 != 0 || nd.lonE6 != 0)
                        ? String.format(Locale.US, " | GPS %.5f, %.5f", nd.latE6 / 1e6, nd.lonE6 / 1e6) : "";
                t2.setText(String.format(Locale.US, "RSSI %d dBm | SNR %+.1f dB | Hops: %d%s",
                        nd.rssi, nd.snr, nd.hops, gps));
                t2.setTextColor(Color.parseColor("#38BDF8"));
                t2.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
                row.addView(t2);

                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                lp.bottomMargin = dp(6);
                list.addView(row, lp);
            }
        }
        sv.addView(list);
        box.addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        mainContentFrame.addView(box, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private void buildSnifferTabView() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(12), dp(10), dp(12), dp(10));

        LinearLayout hdr = new LinearLayout(this);
        hdr.setOrientation(LinearLayout.HORIZONTAL);
        hdr.setGravity(Gravity.CENTER_VERTICAL);

        TextView tvTitle = new TextView(this);
        tvTitle.setText("256-BUCKET RAINBOW SNIFFER (" + totalRainbowCandidates + " keys)");
        tvTitle.setTextColor(Color.parseColor("#38BDF8"));
        tvTitle.setTypeface(Typeface.DEFAULT_BOLD);
        tvTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        hdr.addView(tvTitle, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Button btnAdd = new Button(this);
        btnAdd.setText("+ #Hashtag");
        btnAdd.setAllCaps(false);
        btnAdd.setTextColor(Color.parseColor("#10B981"));
        btnAdd.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        btnAdd.setBackground(roundBg(Color.parseColor("#0D2822"), Color.parseColor("#10B981"), dp(1), dp(8)));
        btnAdd.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                promptJoinHashtagChannel();
            }
        });
        hdr.addView(btnAdd, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(36)));
        box.addView(hdr);

        ScrollView sv = new ScrollView(this);
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(0, dp(8), 0, dp(8));

        for (final SnifferItem it : snifferChannels.values()) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.VERTICAL);
            row.setPadding(dp(10), dp(8), dp(10), dp(8));
            row.setBackground(roundBg(Color.parseColor("#111B2E"), Color.parseColor("#1E293B"), dp(1), dp(8)));

            TextView t1 = new TextView(this);
            t1.setText(it.name + "   [Hash " + it.hashHex + "]   • " + it.msgCount + " RF pkts");
            t1.setTextColor(Color.parseColor("#38BDF8"));
            t1.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
            t1.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            row.addView(t1);

            TextView t2 = new TextView(this);
            t2.setText("AES-128 PSK: " + it.pskHex + "  (Tap to Join & Sync Slot)");
            t2.setTextColor(Color.parseColor("#94A3B8"));
            t2.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
            row.addView(t2);

            row.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    joinAndSelectChannel(it.name);
                }
            });

            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.bottomMargin = dp(6);
            list.addView(row, lp);
        }

        sv.addView(list);
        box.addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        mainContentFrame.addView(box, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private void buildChannelsTabView() {
        ScrollView sv = new ScrollView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(12), dp(10), dp(12), dp(10));

        TextView tvSlots = new TextView(this);
        tvSlots.setText("HARDWARE RADIO CHANNEL SLOTS (0..7)");
        tvSlots.setTextColor(Color.parseColor("#38BDF8"));
        tvSlots.setTypeface(Typeface.DEFAULT_BOLD);
        tvSlots.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        box.addView(tvSlots);

        for (int i = 0; i < 8; i++) {
            final String slotCh = hwSlots[i];
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setPadding(dp(10), dp(8), dp(10), dp(8));
            row.setBackground(roundBg(Color.parseColor("#111B2E"), Color.parseColor("#1E293B"), dp(1), dp(8)));

            TextView t = new TextView(this);
            t.setText("Slot " + i + ":  " + slotCh);
            t.setTextColor(Color.parseColor("#E2E8F0"));
            t.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
            t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            row.addView(t);

            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(6);
            box.addView(row, lp);
        }

        sv.addView(box);
        mainContentFrame.addView(sv, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private void buildRadioTabView() {
        ScrollView sv = new ScrollView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(12), dp(10), dp(12), dp(10));

        TextView tvTitle = new TextView(this);
        tvTitle.setText("BLE NUS RADIO & OPERATOR SETTINGS");
        tvTitle.setTextColor(Color.parseColor("#38BDF8"));
        tvTitle.setTypeface(Typeface.DEFAULT_BOLD);
        tvTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        box.addView(tvTitle);

        // Callsign row
        LinearLayout csRow = new LinearLayout(this);
        csRow.setOrientation(LinearLayout.HORIZONTAL);
        csRow.setGravity(Gravity.CENTER_VERTICAL);
        csRow.setPadding(0, dp(8), 0, dp(8));

        final EditText etCs = new EditText(this);
        etCs.setText(callsign);
        etCs.setTextColor(Color.parseColor("#F1F5F9"));
        etCs.setSingleLine(true);
        etCs.setPadding(dp(10), dp(6), dp(10), dp(6));
        etCs.setBackground(roundBg(Color.parseColor("#060A12"), Color.parseColor("#1E293B"), dp(1), dp(8)));
        csRow.addView(etCs, new LinearLayout.LayoutParams(0, dp(40), 1f));

        Button btnSaveCs = new Button(this);
        btnSaveCs.setText("Set Callsign");
        btnSaveCs.setAllCaps(false);
        btnSaveCs.setTextColor(Color.parseColor("#38BDF8"));
        btnSaveCs.setBackground(roundBg(Color.parseColor("#10243A"), Color.parseColor("#38BDF8"), dp(1), dp(8)));
        btnSaveCs.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                String vStr = etCs.getText().toString().trim();
                if (!vStr.isEmpty()) {
                    callsign = vStr;
                    getSharedPreferences("meshcore_prefs", MODE_PRIVATE)
                            .edit().putString("callsign", callsign).apply();
                    updateHeaderUi();
                    Toast.makeText(MeshCoreActivity.this, "Callsign updated: " + callsign, Toast.LENGTH_SHORT).show();
                }
            }
        });
        LinearLayout.LayoutParams saveLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(40));
        saveLp.leftMargin = dp(8);
        csRow.addView(btnSaveCs, saveLp);
        box.addView(csRow);

        Button btnScan = new Button(this);
        btnScan.setText(bleConnected ? "Rescan BLE NUS Radios" : "Scan & Connect BLE NUS Radio");
        btnScan.setAllCaps(false);
        btnScan.setTextColor(Color.parseColor("#060A12"));
        btnScan.setTypeface(Typeface.DEFAULT_BOLD);
        btnScan.setBackground(roundBg(Color.parseColor("#38BDF8"), Color.parseColor("#38BDF8"), 0, dp(8)));
        btnScan.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startBleScanAndAutoConnect();
            }
        });
        box.addView(btnScan, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(42)));

        TextView tvDisc = new TextView(this);
        tvDisc.setText("DISCOVERED BLE PERIPHERALS (" + discoveredBle.size() + "):");
        tvDisc.setTextColor(Color.parseColor("#94A3B8"));
        tvDisc.setPadding(0, dp(12), 0, dp(6));
        box.addView(tvDisc);

        for (final BleDeviceItem d : discoveredBle.values()) {
            Button b = new Button(this);
            b.setAllCaps(false);
            b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            b.setPadding(dp(12), dp(8), dp(12), dp(8));
            b.setText(d.name + "  [" + d.address + "]  (" + d.rssi + " dBm)");
            b.setTextColor(Color.parseColor("#38BDF8"));
            b.setBackground(roundBg(Color.parseColor("#111B2E"), Color.parseColor("#38BDF8"), dp(1), dp(8)));
            b.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    connectToBleDevice(d.device, d.name);
                }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.bottomMargin = dp(6);
            box.addView(b, lp);
        }

        sv.addView(box);
        mainContentFrame.addView(sv, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private void selectChannel(String ch) {
        if (!channelMessages.containsKey(ch)) {
            channelMessages.put(ch, new ArrayList<ChatMsgItem>());
        }
        activeChannel = ch;
        switchMobileTab(0);
    }

    private void joinAndSelectChannel(String rawName) {
        String ch = rawName.trim();
        if (ch.isEmpty()) return;
        if (!ch.equalsIgnoreCase("Public") && !ch.startsWith("#") && !ch.startsWith("@")) {
            ch = "#" + ch.toLowerCase(Locale.US);
        }
        if (!joinedChannels.contains(ch)) {
            joinedChannels.add(ch);
        }
        if (!channelMessages.containsKey(ch)) {
            channelMessages.put(ch, new ArrayList<ChatMsgItem>());
        }
        registerSnifferChannel(ch);
        byte[] syncPkt = nativeSyncChannelSlot(ch);
        if (syncPkt != null && syncPkt.length > 0) {
            enqueueBleFrame(syncPkt);
        }
        activeChannel = ch;
        switchMobileTab(0);
    }

    private void promptJoinHashtagChannel() {
        final EditText input = new EditText(this);
        input.setInputType(InputType.TYPE_CLASS_TEXT);
        input.setText("#");
        input.setSelection(1);
        new AlertDialog.Builder(this)
                .setTitle("Join / Sniff #Hashtag Channel")
                .setView(input)
                .setPositiveButton("Join & Sync", (dialog, which) -> {
                    String val = input.getText().toString().trim();
                    if (!val.isEmpty()) {
                        joinAndSelectChannel(val);
                    }
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private static String nowTimeStr() {
        return new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date());
    }

    private void appendSystemMessage(String ch, String text) {
        List<ChatMsgItem> list = channelMessages.get(ch);
        if (list == null) {
            list = new ArrayList<>();
            channelMessages.put(ch, list);
        }
        ChatMsgItem m = new ChatMsgItem();
        m.time = nowTimeStr();
        m.sender = "*";
        m.text = text;
        m.isSystem = true;
        list.add(m);
    }

    private void sendCurrentChatText() {
        if (etChatInput == null) return;
        String text = etChatInput.getText().toString().trim();
        if (text.isEmpty()) return;
        etChatInput.setText("");

        if (text.startsWith("/join ")) {
            joinAndSelectChannel(text.substring(6));
            return;
        }

        String wireText = callsign + ": " + text;
        byte[][] frames = nativeBuildChannelTextFrames(activeChannel, callsign, wireText);
        if (frames != null) {
            for (byte[] f : frames) {
                enqueueBleFrame(f);
            }
        }

        ChatMsgItem m = new ChatMsgItem();
        m.time = nowTimeStr();
        m.sender = callsign;
        m.text = text;
        m.isSelf = true;
        List<ChatMsgItem> list = channelMessages.get(activeChannel);
        if (list == null) {
            list = new ArrayList<>();
            channelMessages.put(activeChannel, list);
        }
        list.add(m);
        rememberDedup(activeChannel + "|" + callsign + "|" + text);
        renderChatMessages();
    }

    // ========================================================================
    // Step 1-4: Photo Picker -> C++ 48x30 1-Bit Dither -> Single-Packet 0x49 TX
    // ========================================================================
    private void openImagePicker() {
        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.setType("image/*");
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        startActivityForResult(Intent.createChooser(intent, "Select Image for 48x30 1-Bit RF TX"), REQ_PICK_IMAGE);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK_IMAGE && resultCode == RESULT_OK && data != null && data.getData() != null) {
            handlePickedImageUri(data.getData());
        }
    }

    private void handlePickedImageUri(final Uri uri) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    int orientation = ExifInterface.ORIENTATION_NORMAL;
                    if (Build.VERSION.SDK_INT >= 24) {
                        try (InputStream exifIn = getContentResolver().openInputStream(uri)) {
                            if (exifIn != null) {
                                ExifInterface exif = new ExifInterface(exifIn);
                                orientation = exif.getAttributeInt(
                                        ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
                            }
                        } catch (Throwable ignored) {}
                    }

                    Bitmap bmp;
                    try (InputStream in = getContentResolver().openInputStream(uri)) {
                        bmp = BitmapFactory.decodeStream(in);
                    }
                    if (bmp == null) return;

                    // Apply EXIF rotation before passing to C++ minimal-crop resizer
                    Matrix mtx = new Matrix();
                    if (orientation == ExifInterface.ORIENTATION_ROTATE_90) mtx.postRotate(90);
                    else if (orientation == ExifInterface.ORIENTATION_ROTATE_180) mtx.postRotate(180);
                    else if (orientation == ExifInterface.ORIENTATION_ROTATE_270) mtx.postRotate(270);
                    if (!mtx.isIdentity()) {
                        bmp = Bitmap.createBitmap(bmp, 0, 0, bmp.getWidth(), bmp.getHeight(), mtx, true);
                    }

                    int w = bmp.getWidth();
                    int h = bmp.getHeight();
                    int[] argb = new int[w * h];
                    bmp.getPixels(argb, 0, w, 0, 0, w, h);

                    final byte[][] res = nativeEncodeAndBuildImageFrames(argb, w, h, activeChannel);
                    if (res == null || res.length < 3) return;

                    final byte[] mono180 = res[0];
                    final byte[] syncFrame = res[1];
                    final byte[] imgFrame = res[2];

                    mainHandler.post(new Runnable() {
                        @Override
                        public void run() {
                            if (syncFrame != null && syncFrame.length > 0) {
                                enqueueBleFrame(syncFrame);
                            }
                            if (imgFrame != null && imgFrame.length > 0) {
                                enqueueBleFrame(imgFrame);
                            }

                            ChatMsgItem m = new ChatMsgItem();
                            m.time = nowTimeStr();
                            m.sender = callsign;
                            m.text = "[1-Bit Tactical Bitmap 48x30 (180B / Opcode 0x49)]";
                            m.isSelf = true;
                            m.monoImage180 = mono180;

                            List<ChatMsgItem> list = channelMessages.get(activeChannel);
                            if (list == null) {
                                list = new ArrayList<>();
                                channelMessages.put(activeChannel, list);
                            }
                            list.add(m);
                            rememberDedup(activeChannel + "|" + callsign + "|" + bytesToHexPrefix(mono180, 24));
                            if (activeMobileTab == 0) {
                                renderChatMessages();
                            }
                        }
                    });
                } catch (final Exception e) {
                    mainHandler.post(new Runnable() {
                        @Override
                        public void run() {
                            Toast.makeText(MeshCoreActivity.this,
                                    "Image encode failed: " + e.getMessage(), Toast.LENGTH_LONG).show();
                        }
                    });
                }
            }
        }).start();
    }

    private static String bytesToHexPrefix(byte[] b, int maxLen) {
        if (b == null) return "";
        int n = Math.min(b.length, maxLen);
        StringBuilder sb = new StringBuilder(n * 2);
        for (int i = 0; i < n; i++) {
            sb.append(String.format(Locale.US, "%02X", b[i] & 0xFF));
        }
        return sb.toString();
    }

    private static byte[] hexToBytes(String hex) {
        if (hex == null || hex.length() < 360) return null;
        byte[] out = new byte[180];
        for (int i = 0; i < 180; i++) {
            int hi = Character.digit(hex.charAt(i * 2), 16);
            int lo = Character.digit(hex.charAt(i * 2 + 1), 16);
            if (hi < 0 || lo < 0) return null;
            out[i] = (byte) ((hi << 4) | lo);
        }
        return out;
    }

    private boolean rememberDedup(String key) {
        if (recentDedup.contains(key)) return false;
        recentDedup.addLast(key);
        while (recentDedup.size() > 64) recentDedup.removeFirst();
        return true;
    }

    // ========================================================================
    // BLE NUS Scanner & Gatt Client
    // ========================================================================
    private boolean servicesDiscoveryStarted = false;

    private void requestBlePermissionsIfNeeded() {
        if (Build.VERSION.SDK_INT < 23) {
            startBleScanAndAutoConnect();
            return;
        }
        List<String> needed = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= 31) {
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.BLUETOOTH_SCAN);
            }
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.BLUETOOTH_CONNECT);
            }
        } else {
            if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.ACCESS_FINE_LOCATION);
            }
        }
        if (!needed.isEmpty()) {
            requestPermissions(needed.toArray(new String[0]), REQ_PERMS);
        } else {
            startBleScanAndAutoConnect();
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_PERMS) {
            startBleScanAndAutoConnect();
        }
    }

    private void startBleScanAndAutoConnect() {
        if (btAdapter == null || !btAdapter.isEnabled()) {
            statusText = "Bluetooth is off — enable Bluetooth";
            updateHeaderUi();
            return;
        }
        BluetoothLeScanner scanner = btAdapter.getBluetoothLeScanner();
        if (scanner == null) return;

        bleScanning = true;
        statusText = "Scanning BLE NUS for MeshCore Companion Nodes...";
        updateHeaderUi();

        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();
        try {
            scanner.startScan(null, settings, bleScanCallback);
            mainHandler.postDelayed(new Runnable() {
                @Override
                public void run() {
                    stopBleScan();
                }
            }, 10000);
        } catch (SecurityException e) {
            statusText = "BLE permission required";
            updateHeaderUi();
        }
    }

    private void stopBleScan() {
        if (!bleScanning || btAdapter == null) return;
        bleScanning = false;
        try {
            BluetoothLeScanner scanner = btAdapter.getBluetoothLeScanner();
            if (scanner != null) scanner.stopScan(bleScanCallback);
        } catch (Exception ignored) {}
        if (!bleConnected) {
            statusText = discoveredBle.isEmpty()
                    ? "No BLE radio found — tap [BLE Scan] to retry"
                    : "Select radio in Radio tab or tap [BLE Scan]";
            updateHeaderUi();
        }
    }

    private final ScanCallback bleScanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            if (result == null || result.getDevice() == null) return;
            BluetoothDevice dev = result.getDevice();
            String name = "";
            try {
                if (dev.getName() != null) name = dev.getName();
            } catch (SecurityException ignored) {}
            if ((name == null || name.isEmpty()) && result.getScanRecord() != null) {
                name = result.getScanRecord().getDeviceName();
            }
            if (name == null || name.isEmpty()) return;

            String low = (name != null) ? name.toLowerCase(Locale.US) : "";
            boolean isMeshCandidate = low.contains("meshcore") || low.contains("companion")
                    || low.contains("t1000") || low.contains("sensecap") || low.contains("heltec")
                    || low.contains("rak") || low.contains("xiao") || low.contains("whisper")
                    || low.contains("lilygo") || low.contains("t-echo") || low.contains("t-deck")
                    || low.contains("t-beam") || low.contains("lora");
            if (result.getScanRecord() != null && result.getScanRecord().getServiceUuids() != null) {
                for (android.os.ParcelUuid pu : result.getScanRecord().getServiceUuids()) {
                    if (NUS_SERVICE_UUID.equals(pu.getUuid())) {
                        isMeshCandidate = true;
                        break;
                    }
                }
            }
            if ((name == null || name.isEmpty()) && !isMeshCandidate) return;

            final boolean autoConnectCandidate = isMeshCandidate;
            final BleDeviceItem item = new BleDeviceItem();
            item.name = (name != null && !name.isEmpty()) ? name : ("MeshCore BLE (" + dev.getAddress() + ")");
            item.address = dev.getAddress();
            item.rssi = result.getRssi();
            item.device = dev;

            mainHandler.post(new Runnable() {
                @Override
                public void run() {
                    discoveredBle.put(item.address, item);
                    if (activeMobileTab == 4) buildRadioTabView();
                    if (!bleConnected && autoConnectCandidate) {
                        stopBleScan();
                        connectToBleDevice(item.device, item.name);
                    }
                }
            });
        }
    };

    private void connectToBleDevice(BluetoothDevice device, String name) {
        try {
            if (activeGatt != null) {
                activeGatt.close();
                activeGatt = null;
            }
            servicesDiscoveryStarted = false;
            statusText = "Connecting BLE NUS -> " + name + "...";
            updateHeaderUi();
            if (Build.VERSION.SDK_INT >= 23) {
                activeGatt = device.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE);
            } else {
                activeGatt = device.connectGatt(this, false, gattCallback);
            }
        } catch (SecurityException e) {
            statusText = "BLE Connect permission denied";
            updateHeaderUi();
        }
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(final BluetoothGatt gatt, int status, int newState) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                servicesDiscoveryStarted = false;
                boolean mtuRequested = false;
                try {
                    mtuRequested = gatt.requestMtu(247);
                } catch (Throwable ignored) {}
                if (!mtuRequested) {
                    servicesDiscoveryStarted = true;
                    try {
                        gatt.discoverServices();
                    } catch (Throwable ignored) {}
                } else {
                    mainHandler.postDelayed(new Runnable() {
                        @Override
                        public void run() {
                            if (!servicesDiscoveryStarted && activeGatt == gatt) {
                                servicesDiscoveryStarted = true;
                                try {
                                    gatt.discoverServices();
                                } catch (Throwable ignored) {}
                            }
                        }
                    }, 900);
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                mainHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        bleConnected = false;
                        nusTxChar = null;
                        bleTxQueue.clear();
                        bleTxInFlight = false;
                        statusText = "BLE Disconnected — tap [BLE Scan]";
                        updateHeaderUi();
                    }
                });
            }
        }

        @Override
        public void onMtuChanged(BluetoothGatt gatt, int mtu, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS && mtu > 23) {
                negotiatedMtu = mtu;
            }
            if (!servicesDiscoveryStarted) {
                servicesDiscoveryStarted = true;
                try {
                    gatt.discoverServices();
                } catch (SecurityException ignored) {}
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            BluetoothGattService svc = gatt.getService(NUS_SERVICE_UUID);
            if (svc == null) return;
            nusTxChar = svc.getCharacteristic(NUS_TX_CHAR_UUID);
            BluetoothGattCharacteristic rxChar = svc.getCharacteristic(NUS_RX_CHAR_UUID);
            if (rxChar != null) {
                try {
                    gatt.setCharacteristicNotification(rxChar, true);
                    BluetoothGattDescriptor cccd = rxChar.getDescriptor(CCCD_UUID);
                    if (cccd != null) {
                        cccd.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                        gatt.writeDescriptor(cccd);
                    }
                } catch (SecurityException ignored) {}
            }
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt gatt, BluetoothGattDescriptor descriptor, int status) {
            mainHandler.post(new Runnable() {
                @Override
                public void run() {
                    bleConnected = true;
                    statusText = "BLE NUS Connected (" + radioModel + ")";
                    updateHeaderUi();
                    startCompanionInitSequence();
                }
            });
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic) {
            final byte[] raw = characteristic.getValue();
            if (raw == null || raw.length == 0) return;
            final String ev = nativeDecodeBleFrame(raw);
            if (ev == null || ev.isEmpty()) return;
            mainHandler.post(new Runnable() {
                @Override
                public void run() {
                    handleDecodedBleEvent(ev);
                }
            });
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, int status) {
            mainHandler.postDelayed(new Runnable() {
                @Override
                public void run() {
                    bleTxInFlight = false;
                    pumpBleTxQueue();
                }
            }, 35);
        }
    };

    private void startCompanionInitSequence() {
        for (int step = 0; step <= 6; step++) {
            enqueueBleFrame(nativeBuildInitCommand(step));
        }
        for (String ch : Arrays.asList("#meshcore", "#emcomm", "#weather", "#general")) {
            enqueueBleFrame(nativeSyncChannelSlot(ch));
        }
    }

    private void enqueueBleFrame(byte[] raw) {
        if (raw == null || raw.length == 0) return;
        long now = System.currentTimeMillis();
        // If BLE link was idle >8s and this is a channel TX (0x03) or slot sync (0x20),
        // prepend a lightweight CMD_GET_BAT_VARS (0x14) wake pulse first.
        if (bleConnected && lastBleTxMs > 0 && (now - lastBleTxMs) > 8000
                && (raw[0] == 0x03 || raw[0] == 0x20) && bleTxQueue.isEmpty()) {
            byte[] wake = nativeBuildInitCommand(4);
            if (wake != null && wake.length > 0) bleTxQueue.addLast(wake);
        }
        lastBleTxMs = now;
        bleTxQueue.addLast(raw);
        pumpBleTxQueue();
    }

    private void pumpBleTxQueue() {
        if (bleTxInFlight || !bleConnected || activeGatt == null || nusTxChar == null) return;
        byte[] next = bleTxQueue.pollFirst();
        if (next == null) return;
        try {
            bleTxInFlight = true;
            nusTxChar.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            nusTxChar.setValue(next);
            if (!activeGatt.writeCharacteristic(nusTxChar)) {
                bleTxInFlight = false;
            }
        } catch (SecurityException e) {
            bleTxInFlight = false;
        }
    }

    private void handleDecodedBleEvent(String ev) {
        String[] cols = ev.split("\t", -1);
        if (cols.length == 0) return;
        String tag = cols[0];

        if ("CHAT".equals(tag) && cols.length >= 8) {
            String ch = cols[1].isEmpty() ? "Public" : cols[1];
            String sender = cols[2].isEmpty() ? "Station" : cols[2];
            String text = cols[3];
            int rssi = parseIntSafe(cols[4], -80);
            float snr = parseFloatSafe(cols[5], 0f);
            int hops = parseIntSafe(cols[6], 1);
            String imgHex = cols[7];

            byte[] mono180 = (!imgHex.isEmpty()) ? hexToBytes(imgHex) : null;
            String dedupKey = ch + "|" + sender + "|" + (mono180 != null ? imgHex.substring(0, 48) : text);
            if (!rememberDedup(dedupKey)) return;

            if (ch.startsWith("#")) {
                registerSnifferChannel(ch);
                SnifferItem sn = snifferChannels.get(ch);
                if (sn != null) {
                    sn.msgCount++;
                    sn.lastRssi = rssi;
                    sn.lastSnr = snr;
                }
                if (!joinedChannels.contains(ch)) {
                    joinedChannels.add(ch);
                }
            }

            List<ChatMsgItem> list = channelMessages.get(ch);
            if (list == null) {
                list = new ArrayList<>();
                channelMessages.put(ch, list);
            }
            ChatMsgItem m = new ChatMsgItem();
            m.time = nowTimeStr();
            m.sender = sender;
            m.text = text;
            m.hasRf = true;
            m.rssi = rssi;
            m.snr = snr;
            m.hops = hops;
            m.monoImage180 = mono180;
            list.add(m);

            if (activeMobileTab == 0 && ch.equalsIgnoreCase(activeChannel)) {
                renderChatMessages();
            } else if (activeMobileTab == 2) {
                buildSnifferTabView();
            }
        } else if ("NODE".equals(tag) && cols.length >= 9) {
            NodeItem nd = new NodeItem();
            nd.name = cols[1];
            nd.keyHex = cols[2];
            nd.type = parseIntSafe(cols[3], 1);
            nd.hops = parseIntSafe(cols[4], 0);
            nd.rssi = parseIntSafe(cols[5], -80);
            nd.snr = parseFloatSafe(cols[6], 0f);
            nd.latE6 = parseIntSafe(cols[7], 0);
            nd.lonE6 = parseIntSafe(cols[8], 0);
            meshNodes.put(nd.keyHex, nd);
            if (activeMobileTab == 1) buildNodesTabView();
        } else if ("SELF".equals(tag) && cols.length >= 7) {
            int freqKhz = parseIntSafe(cols[2], 910525);
            int bwHz = parseIntSafe(cols[3], 62500);
            int sf = parseIntSafe(cols[4], 7);
            int cr = parseIntSafe(cols[5], 5);
            radioParams = String.format(Locale.US, "%.3f MHz | SF%d | BW %.1f kHz | CR %d",
                    freqKhz / 1000.0, sf, bwHz / 1000.0, cr);
            updateHeaderUi();
        } else if ("DEVICE".equals(tag) && cols.length >= 3) {
            radioModel = cols[1] + " " + cols[2];
            statusText = "BLE NUS Connected (" + radioModel.trim() + ")";
            updateHeaderUi();
        } else if ("BATTERY".equals(tag) && cols.length >= 3) {
            batteryMv = parseIntSafe(cols[1], 0);
            batteryPct = parseIntSafe(cols[2], 0);
            updateHeaderUi();
        } else if ("SLOT".equals(tag) && cols.length >= 3) {
            int slot = parseIntSafe(cols[1], -1);
            if (slot >= 0 && slot < 8 && !cols[2].isEmpty()) {
                hwSlots[slot] = cols[2];
                if (activeMobileTab == 3) buildChannelsTabView();
            }
        }
    }

    private static int parseIntSafe(String s, int def) {
        try { return Integer.parseInt(s.trim()); } catch (Exception e) { return def; }
    }

    private static float parseFloatSafe(String s, float def) {
        try { return Float.parseFloat(s.trim()); } catch (Exception e) { return def; }
    }
}

