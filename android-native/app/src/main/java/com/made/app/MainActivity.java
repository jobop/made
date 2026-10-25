package com.made.app;

import android.app.Activity;
import android.animation.Animator;
import android.app.AlertDialog;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.text.TextUtils;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.animation.LinearInterpolator;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

/**
 * 码得 · 原生安卓端。协议与 ESP32 固件一致（连桥接 / 对话 / 精灵 / 语音），
 * 交互三层：连接页（内网自动扫描 + 手动兜底）→ 配对卡（大字配对码）→ 主页
 * （顶栏双行 + 精灵状态胶囊 + 任务卡 + 底部输入栏 / 按住说话）。
 */
public class MainActivity extends Activity {
    private static final String TAG = "made.app";
    private static final int REQ_MIC = 1;

    private final Bridge bridge = new Bridge();
    private final Model model = new Model();
    private Pairing pairing;
    private Poller poller;
    private final VoiceRecorder voice = new VoiceRecorder();
    private final LanScanner scanner = new LanScanner();
    private final Handler ui = new Handler(Looper.getMainLooper());

    // ---- 屏幕容器 ----
    private FrameLayout root;
    private View connectScreen, homeScreen;

    // ---- 连接页 ----
    private ScanRadar radar;
    private TextView scanStatus, rescanBtn;
    private LinearLayout foundList;
    private LinearLayout scanCard, manualCard, pairingCard;
    private EditText hostEdit, portEdit;
    private CheckBox httpsCheck;
    private TextView pairName, pairCode, pairMsg, pairRetry, pairCancel;
    private String lastHost = "", lastName = "";
    private int lastPort = 8788;
    private int foundCount = 0;

    // ---- 主页 ----
    private TextView providerLabel, sessionLabel, connBanner, menuBtn;
    private SpritePager pager;
    private View statusDot;
    private TextView statusChipLabel;
    private StatusPill statusPill;
    private LinearLayout chip, jobCard, jobActions, jobHead;
    private ScrollView jobScroll;
    private View jobDot;
    private TextView jobStatusLabel, jobInstruction, jobResult, jobError;
    private TextView confirmBtn, cancelBtn;
    private EditText inputEdit;
    private TextView micBtn, sendBtn;
    private LinearLayout normalRow, recordingRow;
    private TextView recLabel;

    private boolean suppressProviderPick = false;
    private long lastSpriteTapMs = 0;
    private float swipeDownX = 0, swipeDownY = 0;
    private String lastJobId = "";
    private boolean suppressNextJobCard = false; // 切助手/会话后不弹旧任务卡
    private boolean emptySheetShown = false;     // 空态「暂无任务」面板已展开过一次
    private volatile boolean voiceUploading = false;
    private int gestureMode = 0; // 0 未定 1 横滑（助手）；上下滑不触发切换
    private long gestureDownMs = 0;
    private FrameLayout spriteAreaHost; // 任务面板的宿主（精灵区）
    private int sheetState = SHEET_PEEK; // 任务面板当前档位
    private int dragStartH = 0; // 把手拖动起点高度
    private boolean sheetAnimating = false;

    private static final int SHEET_PEEK = 0;  // 只露边缘
    private static final int SHEET_HALF = 1;  // 半屏
    private static final int SHEET_FULL = 2;  // 全屏
    private volatile long recStartMs = 0;
    private volatile boolean recCancelled = false;
    private float recDownY = 0;
    private Runnable recTick;

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        pairing = new Pairing(bridge, this);
        poller = new Poller(bridge, model);
        poller.setCommandSink(this::handleBoardCommand);
        buildUi();
        if (bridge.isPaired()) {
            showHome();
            new Thread(() -> {
                boolean valid = pairing.verifySaved();
                if (!valid) {
                    ui.post(() -> {
                        pairing.forget();
                        showConnect("电脑已解除配对，重新连接一下");
                    });
                }
            }, "made-verify").start();
        } else {
            showConnect("");
        }
    }

    @Override protected void onDestroy() {
        super.onDestroy();
        scanner.cancel();
        poller.stop();
        voice.cancel();
    }

    // ================= UI 构建 =================

    private int dp(float v) { return Ui.dp(this, v); }

    private LinearLayout.LayoutParams lp(int w, int h) {
        return new LinearLayout.LayoutParams(w, h);
    }

    private void buildUi() {
        root = new FrameLayout(this);
        root.setBackgroundColor(Ui.BG);
        setContentView(root, new FrameLayout.LayoutParams(-1, -1));
        connectScreen = buildConnectScreen();
        homeScreen = buildHomeScreen();
        root.addView(connectScreen, new FrameLayout.LayoutParams(-1, -1));
        root.addView(homeScreen, new FrameLayout.LayoutParams(-1, -1));
        homeScreen.setVisibility(View.GONE);
    }

    // ---- 连接页 ----

    private View buildConnectScreen() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setScrollbarFadingEnabled(false);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(24), dp(56), dp(24), dp(32));
        scroll.addView(page, lp(-1, -2));

        // 品牌
        TextView brand = Ui.text(this, "码得", 34, Ui.INK, true);
        page.addView(brand, lp(-2, -2));
        TextView tagline = Ui.text(this, "把手机连到你的电脑助手", 14, Ui.SUB, false);
        tagline.setPadding(0, dp(4), 0, dp(24));
        page.addView(tagline, lp(-2, -2));

        // 扫描卡
        scanCard = new LinearLayout(this);
        scanCard.setOrientation(LinearLayout.VERTICAL);
        scanCard.setBackground(Ui.card(this, Ui.SURFACE, 20, Ui.LINE, 1));
        scanCard.setPadding(dp(18), dp(18), dp(18), dp(18));

        LinearLayout scanHead = new LinearLayout(this);
        scanHead.setOrientation(LinearLayout.HORIZONTAL);
        scanHead.setGravity(Gravity.CENTER_VERTICAL);
        radar = new ScanRadar(this);
        scanHead.addView(radar, lp(dp(38), dp(38)));
        TextView scanTitle = Ui.text(this, "附近的电脑", 16, Ui.INK, true);
        scanTitle.setPadding(dp(12), 0, 0, 0);
        scanHead.addView(scanTitle, new LinearLayout.LayoutParams(0, -2, 1f));
        rescanBtn = Ui.text(this, "重新扫描", 13, Ui.PRIMARY, true);
        rescanBtn.setPadding(dp(10), dp(6), dp(2), dp(6));
        rescanBtn.setOnClickListener(v -> startScan());
        Ui.press(rescanBtn, 0.92f);
        scanHead.addView(rescanBtn, lp(-2, -2));
        scanCard.addView(scanHead, lp(-1, -2));

        scanStatus = Ui.text(this, "正在扫描附近的电脑…", 13, Ui.SUB, false);
        scanStatus.setPadding(0, dp(10), 0, dp(6));
        scanCard.addView(scanStatus, lp(-1, -2));

        foundList = new LinearLayout(this);
        foundList.setOrientation(LinearLayout.VERTICAL);
        scanCard.addView(foundList, lp(-1, -2));
        page.addView(scanCard, lp(-1, -2));

        // 分隔
        TextView or = Ui.text(this, "找不到？手动添加", 12, Ui.SUB, false);
        or.setGravity(Gravity.CENTER);
        or.setPadding(0, dp(20), 0, dp(10));
        page.addView(or, lp(-1, -2));

        // 手动卡
        manualCard = new LinearLayout(this);
        manualCard.setOrientation(LinearLayout.VERTICAL);
        manualCard.setBackground(Ui.card(this, Ui.SURFACE, 20, Ui.LINE, 1));
        manualCard.setPadding(dp(18), dp(18), dp(18), dp(18));

        hostEdit = styledEdit("电脑地址（IP 或域名）");
        manualCard.addView(hostEdit, lp(-1, -2));
        LinearLayout portRow = new LinearLayout(this);
        portRow.setOrientation(LinearLayout.HORIZONTAL);
        portEdit = styledEdit("8788");
        portEdit.setInputType(InputType.TYPE_CLASS_NUMBER);
        portRow.addView(portEdit, new LinearLayout.LayoutParams(0, -2, 1f));
        httpsCheck = new CheckBox(this);
        httpsCheck.setText("HTTPS");
        httpsCheck.setTextSize(13);
        httpsCheck.setTextColor(Ui.SUB);
        httpsCheck.setPadding(dp(10), 0, 0, 0);
        portRow.addView(httpsCheck, lp(-2, -2));
        manualCard.addView(portRow, lp(-1, -2));

        TextView connectBtn = Ui.pillButton(this, "连接", true);
        connectBtn.setOnClickListener(v -> startManualPair());
        LinearLayout.LayoutParams cblp = lp(-1, -2);
        cblp.topMargin = dp(12);
        manualCard.addView(connectBtn, cblp);
        page.addView(manualCard, lp(-1, -2));

        // 配对卡（选桥后替换扫描/手动卡显示）
        pairingCard = new LinearLayout(this);
        pairingCard.setOrientation(LinearLayout.VERTICAL);
        pairingCard.setGravity(Gravity.CENTER_HORIZONTAL);
        pairingCard.setBackground(Ui.card(this, Ui.SURFACE, 20, Ui.LINE, 1));
        pairingCard.setPadding(dp(20), dp(26), dp(20), dp(22));
        pairingCard.setVisibility(View.GONE);

        pairName = Ui.text(this, "", 15, Ui.INK, true);
        pairName.setGravity(Gravity.CENTER);
        pairingCard.addView(pairName, lp(-1, -2));

        pairCode = new TextView(this);
        pairCode.setTextSize(52);
        pairCode.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        pairCode.setTextColor(Ui.PRIMARY);
        pairCode.setLetterSpacing(0.18f);
        pairCode.setGravity(Gravity.CENTER);
        pairCode.setPadding(0, dp(18), 0, dp(10));
        pairingCard.addView(pairCode, lp(-1, -2));

        pairMsg = Ui.text(this, "", 13, Ui.SUB, false);
        pairMsg.setGravity(Gravity.CENTER);
        pairingCard.addView(pairMsg, lp(-1, -2));

        TextView hint = Ui.text(this, "在电脑工作台点「添加设备」，输入同一个配对码",
                12, Ui.SUB, false);
        hint.setGravity(Gravity.CENTER);
        hint.setPadding(0, dp(8), 0, dp(14));
        pairingCard.addView(hint, lp(-1, -2));

        LinearLayout pairBtns = new LinearLayout(this);
        pairBtns.setOrientation(LinearLayout.HORIZONTAL);
        pairCancel = Ui.pillButton(this, "取消", false);
        pairBtns.addView(pairCancel, new LinearLayout.LayoutParams(0, -2, 1f));
        pairRetry = Ui.pillButton(this, "重试", true);
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(0, -2, 1f);
        rlp.leftMargin = dp(10);
        pairBtns.addView(pairRetry, rlp);
        pairingCard.addView(pairBtns, lp(-1, -2));
        pairRetry.setVisibility(View.GONE);
        pairCancel.setOnClickListener(v -> {
            pairing.cancelPending();
            showConnect("");
        });
        pairRetry.setOnClickListener(v -> startPairing(lastHost, lastPort, lastName));
        page.addView(pairingCard, lp(-1, -2));
        return scroll;
    }

    private EditText styledEdit(String hint) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setTextSize(15);
        e.setTextColor(Ui.INK);
        e.setHintTextColor(Ui.SUB);
        e.setSingleLine();
        e.setBackground(Ui.fill(this, Ui.SOFT, 12));
        e.setPadding(dp(14), dp(11), dp(14), dp(11));
        LinearLayout.LayoutParams p = lp(-1, -2);
        p.bottomMargin = dp(10);
        e.setLayoutParams(p);
        return e;
    }

    /**
     * 精灵头顶状态图标（对齐固件 updateStatusIcon 的五态）：
     * 0 空闲灰点 · 1 录音红麦（800ms 呼吸脉冲）· 2 上传声波（160ms 双帧交替）
     * · 3 待确认黄「!」· 4 执行中旋转缺口弧（1s/圈）。
     */
    private final class StatusPill extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int state = 0;
        private final Runnable tick = new Runnable() {
            @Override public void run() {
                if (state == 1 || state == 2 || state == 4) {
                    invalidate();
                    ui.postDelayed(this, 33);
                }
            }
        };

        StatusPill(android.content.Context c) { super(c); }

        void setPillState(int s) {
            if (s == state) return;
            boolean wasActive = state == 1 || state == 2 || state == 4;
            state = s;
            invalidate();
            if (!wasActive && (s == 1 || s == 2 || s == 4)) ui.postDelayed(tick, 33);
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            int w = getWidth();
            float cx = w / 2f, cy = getHeight() / 2f;
            long t = System.currentTimeMillis();
            // 白底胶囊 + 描边
            p.setStyle(Paint.Style.FILL);
            p.setColor(0xFFFFFFFF);
            p.setAlpha(235);
            canvas.drawCircle(cx, cy, w / 2f - dp(1), p);
            p.setAlpha(255);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(dp(1));
            p.setColor(Ui.LINE);
            canvas.drawCircle(cx, cy, w / 2f - dp(1.5f), p);

            float r = dp(4);
            switch (state) {
                case 1: { // 录音：红色麦克风 + 呼吸脉冲
                    int alpha = (int) (255 - (0.5 + 0.5 * Math.sin(
                            2 * Math.PI * (t % 1600) / 1600.0)) * (255 - 140));
                    p.setStyle(Paint.Style.FILL);
                    p.setColor(Ui.ERR);
                    p.setAlpha(alpha);
                    float mw = dp(5.5f), mh = dp(9f);
                    RectF body = new RectF(cx - mw / 2, cy - mh / 2 - dp(2.5f),
                            cx + mw / 2, cy + mh / 2 - dp(2.5f));
                    canvas.drawRoundRect(body, mw / 2, mw / 2, p);
                    p.setStyle(Paint.Style.STROKE);
                    p.setStrokeWidth(dp(1.4f));
                    canvas.drawArc(body.centerX() - dp(6), body.bottom - dp(2.5f),
                            body.centerX() + dp(6), body.bottom + dp(4.5f),
                            0, 180, false, p);
                    canvas.drawLine(cx, body.bottom + dp(4.5f), cx,
                            body.bottom + dp(6.5f), p);
                    break;
                }
                case 2: { // 上传：声波双帧交替（160ms）
                    p.setColor(Ui.PRIMARY);
                    p.setStyle(Paint.Style.STROKE);
                    p.setStrokeWidth(dp(1.6f));
                    boolean alt = (t / 160) % 2 == 0;
                    float[] radii = alt ? new float[]{dp(2.5f), dp(5.5f)}
                            : new float[]{dp(4f), dp(7.5f)};
                    for (float rr : radii) {
                        canvas.drawArc(cx - rr - dp(2), cy - rr, cx - dp(2) + rr,
                                cy + rr, 40, -80, false, p);
                        canvas.drawArc(cx + dp(2) - rr, cy - rr, cx + dp(2) + rr,
                                cy + rr, 140, 80, false, p);
                    }
                    p.setStyle(Paint.Style.FILL);
                    canvas.drawCircle(cx, cy, dp(1.6f), p);
                    break;
                }
                case 3: { // 待确认：黄「!」
                    p.setStyle(Paint.Style.FILL);
                    p.setColor(Ui.WARN);
                    p.setStrokeWidth(dp(2f));
                    canvas.drawLine(cx, cy - dp(6), cx, cy + dp(1.5f), p);
                    canvas.drawCircle(cx, cy + dp(5.5f), dp(1.4f), p);
                    break;
                }
                case 4: { // 执行中：旋转缺口弧
                    p.setColor(Ui.PRIMARY);
                    p.setStyle(Paint.Style.STROKE);
                    p.setStrokeWidth(dp(2.2f));
                    float start = (t % 1000) / 1000f * 360f;
                    canvas.drawArc(cx - r, cy - r, cx + r, cy + r,
                            start, 270, false, p);
                    break;
                }
                default: { // 空闲：灰点
                    p.setStyle(Paint.Style.FILL);
                    p.setColor(Ui.SUB);
                    p.setAlpha(150);
                    canvas.drawCircle(cx, cy, dp(3f), p);
                    p.setAlpha(255);
                    break;
                }
            }
        }
    }

    /** 扫描雷达动效：三圈扩散 + 中心点（仅 transform/alpha，动画克制）。 */
    private final class ScanRadar extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private float t = 0;
        private android.animation.ValueAnimator anim;

        ScanRadar(android.content.Context c) { super(c); }

        @Override protected void onDraw(Canvas canvas) {
            int s = Math.min(getWidth(), getHeight());
            float cx = getWidth() / 2f, cy = getHeight() / 2f;
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(1.6f));
            for (int i = 0; i < 3; i++) {
                float phase = anim != null ? (t + i / 3f) % 1f : 0.25f + i * 0.25f;
                float r = s * 0.16f + s * 0.3f * phase;
                paint.setColor(Ui.PRIMARY);
                paint.setAlpha(anim != null ? (int) (170 * (1 - phase)) : 60);
                canvas.drawCircle(cx, cy, r, paint);
            }
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Ui.PRIMARY);
            paint.setAlpha(255);
            canvas.drawCircle(cx, cy, dp(4.5f), paint);
        }

        void start() {
            stop();
            anim = android.animation.ValueAnimator.ofFloat(0, 1);
            anim.setDuration(1800);
            anim.setRepeatCount(android.animation.ValueAnimator.INFINITE);
            anim.setInterpolator(new LinearInterpolator());
            anim.addUpdateListener(a -> {
                t = (float) a.getAnimatedValue();
                invalidate();
            });
            anim.start();
        }

        void stop() {
            if (anim != null) { anim.cancel(); anim = null; }
            invalidate();
        }
    }

    // ---- 主页 ----

    private View buildHomeScreen() {
        LinearLayout home = new LinearLayout(this);
        home.setOrientation(LinearLayout.VERTICAL);

        // 顶栏：助手 + 会话（双行可点），右侧菜单。
        LinearLayout appbar = new LinearLayout(this);
        appbar.setOrientation(LinearLayout.HORIZONTAL);
        appbar.setGravity(Gravity.CENTER_VERTICAL);
        appbar.setBackgroundColor(Ui.SURFACE);
        appbar.setPadding(dp(18), dp(10), dp(8), dp(10));

        LinearLayout titleCol = new LinearLayout(this);
        titleCol.setOrientation(LinearLayout.VERTICAL);
        titleCol.setPadding(dp(4), dp(2), 0, dp(2));
        providerLabel = Ui.text(this, "…", 17, Ui.INK, true);
        titleCol.addView(providerLabel, lp(-2, -2));
        sessionLabel = Ui.text(this, "", 12, Ui.SUB, false);
        sessionLabel.setSingleLine();
        sessionLabel.setEllipsize(TextUtils.TruncateAt.END);
        sessionLabel.setPadding(0, dp(2), 0, 0);
        titleCol.addView(sessionLabel, lp(-2, -2));
        appbar.addView(titleCol, new LinearLayout.LayoutParams(0, -2, 1f));
        // 顶栏不响应点击：左右滑切助手 / 上下滑切会话（与精灵区同一套手势）。
        appbar.setOnTouchListener(this::onSpriteAreaTouch);

        menuBtn = Ui.text(this, "⋯", 24, Ui.SUB, false);
        menuBtn.setPadding(dp(14), dp(4), dp(14), dp(4));
        menuBtn.setOnClickListener(v -> showMenu());
        appbar.addView(menuBtn, lp(-2, -2));
        home.addView(appbar, lp(-1, -2));
        View line = new View(this);
        line.setBackgroundColor(Ui.LINE);
        home.addView(line, lp(-1, dp(1)));

        // 断连横幅
        connBanner = Ui.text(this, "", 13, 0xFFFFFFFF, false);
        connBanner.setBackgroundColor(Ui.ERR);
        connBanner.setPadding(dp(18), dp(8), dp(18), dp(8));
        connBanner.setVisibility(View.GONE);
        home.addView(connBanner, lp(-1, -2));

        // 精灵分页区（三屏预载、跟手）+ 头顶状态图标 + 底部状态胶囊
        FrameLayout spriteArea = new FrameLayout(this);
        pager = new SpritePager(this, bridge);
        pager.setActionsListener(names -> announceCapabilities(names));
        spriteArea.addView(pager, new FrameLayout.LayoutParams(-1, -1));
        statusPill = new StatusPill(this);
        FrameLayout.LayoutParams pillLp = new FrameLayout.LayoutParams(
                dp(34), dp(34), Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        pillLp.topMargin = dp(6);
        spriteArea.addView(statusPill, pillLp);
        chip = new LinearLayout(this);
        chip.setOrientation(LinearLayout.HORIZONTAL);
        chip.setGravity(Gravity.CENTER_VERTICAL);
        chip.setBackground(Ui.fill(this, 0xF2FFFFFF, 999));
        chip.setPadding(dp(12), dp(6), dp(12), dp(6));
        statusDot = new View(this);
        statusDot.setBackground(Ui.circle(Ui.OK));
        LinearLayout.LayoutParams dotLp = lp(dp(8), dp(8));
        chip.addView(statusDot, dotLp);
        statusChipLabel = Ui.text(this, "待机", 12, Ui.INK, false);
        statusChipLabel.setPadding(dp(6), 0, 0, 0);
        chip.addView(statusChipLabel, lp(-2, -2));
        FrameLayout.LayoutParams chipLp = new FrameLayout.LayoutParams(
                -2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        chipLp.bottomMargin = dp(10);
        spriteArea.addView(chip, chipLp);
        // 双击/左右滑：双击=touch.double 事件，左右滑=切换助手（对齐固件手势语义）。
        spriteArea.setOnTouchListener(this::onSpriteAreaTouch);
        home.addView(spriteArea, new LinearLayout.LayoutParams(-1, 0, 1f));

        // 任务卡 = 可拖拽底部面板：把手（跟手上拉/下拉，档位 1/4 半屏 3/4 全屏）
        // + 内容区（ScrollView 跟手滚动 + 滚动条）+ 操作行。挂在精灵区内、
        // 底部对齐，高度即面板展开量（peek 只露边缘）。
        jobCard = new LinearLayout(this);
        jobCard.setOrientation(LinearLayout.VERTICAL);
        jobCard.setBackground(Ui.card(this, Ui.SURFACE, 16, Ui.LINE, 1));
        jobCard.setPadding(dp(16), dp(8), dp(16), dp(12));
        jobCard.setVisibility(View.GONE);
        jobCard.setElevation(dp(6));

        // 把手：顶部小横条 + 状态行，整块按住拖动
        LinearLayout dragHandle = new LinearLayout(this);
        dragHandle.setOrientation(LinearLayout.VERTICAL);
        dragHandle.setGravity(Gravity.CENTER_HORIZONTAL);
        dragHandle.setPadding(0, dp(6), 0, dp(6));
        View grabber = new View(this);
        grabber.setBackground(Ui.fill(this, Ui.LINE, 999));
        dragHandle.addView(grabber, lp(dp(36), dp(4)));

        jobHead = new LinearLayout(this);
        jobHead.setOrientation(LinearLayout.HORIZONTAL);
        jobHead.setGravity(Gravity.CENTER_VERTICAL);
        jobHead.setPadding(0, dp(4), 0, 0);
        jobDot = new View(this);
        jobDot.setBackground(Ui.circle(Ui.WARN));
        jobHead.addView(jobDot, lp(dp(8), dp(8)));
        jobStatusLabel = Ui.text(this, "", 12, Ui.SUB, true);
        jobStatusLabel.setPadding(dp(6), 0, 0, 0);
        jobHead.addView(jobStatusLabel, lp(-2, -2));
        dragHandle.addView(jobHead, lp(-1, -2));
        jobCard.addView(dragHandle, lp(-1, -2));
        dragHandle.setOnTouchListener(this::onSheetHandleTouch);

        jobScroll = new ScrollView(this);
        jobScroll.setVerticalScrollBarEnabled(true);
        jobScroll.setScrollbarFadingEnabled(false);
        LinearLayout scrollCol = new LinearLayout(this);
        scrollCol.setOrientation(LinearLayout.VERTICAL);
        jobInstruction = Ui.text(this, "", 15, Ui.INK, true);
        jobInstruction.setPadding(0, dp(6), 0, 0);
        scrollCol.addView(jobInstruction, lp(-1, -2));
        jobResult = Ui.text(this, "", 14, Ui.INK, false);
        jobResult.setPadding(0, dp(6), 0, 0);
        scrollCol.addView(jobResult, lp(-1, -2));
        jobError = Ui.text(this, "", 13, Ui.ERR, false);
        jobError.setPadding(0, dp(4), 0, 0);
        scrollCol.addView(jobError, lp(-1, -2));
        jobScroll.addView(scrollCol, new ScrollView.LayoutParams(-1, -2));
        jobCard.addView(jobScroll, new LinearLayout.LayoutParams(-1, 0, 1f));

        jobActions = new LinearLayout(this);
        jobActions.setOrientation(LinearLayout.HORIZONTAL);
        cancelBtn = Ui.pillButton(this, "取消", false);
        jobActions.addView(cancelBtn, new LinearLayout.LayoutParams(0, -2, 1f));
        confirmBtn = Ui.pillButton(this, "确认执行", true);
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(0, -2, 1f);
        clp.leftMargin = dp(10);
        jobActions.addView(confirmBtn, clp);
        LinearLayout.LayoutParams alp = lp(-1, -2);
        alp.topMargin = dp(10);
        jobCard.addView(jobActions, alp);

        confirmBtn.setOnClickListener(v -> act("confirm"));
        cancelBtn.setOnClickListener(v -> act("cancel"));
        // 挂进精灵区、底部对齐（不会被输入栏挡住；盖住精灵下半部属预期）
        spriteAreaHost = spriteArea;
        FrameLayout.LayoutParams sheetLp = new FrameLayout.LayoutParams(
                -1, dp(52), Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        sheetLp.setMargins(dp(12), 0, dp(12), dp(6));
        jobCard.setLayoutParams(sheetLp);
        spriteArea.addView(jobCard);

        // 底部输入栏
        LinearLayout inputBar = new LinearLayout(this);
        inputBar.setOrientation(LinearLayout.VERTICAL);
        inputBar.setBackgroundColor(Ui.SURFACE);
        inputBar.setPadding(dp(12), dp(8), dp(12), dp(10));
        View topLine = new View(this);
        topLine.setBackgroundColor(Ui.LINE);
        LinearLayout.LayoutParams lineLp = lp(-1, dp(1));
        lineLp.setMargins(-dp(12), 0, -dp(12), dp(8));
        inputBar.addView(topLine, lineLp);

        normalRow = new LinearLayout(this);
        normalRow.setOrientation(LinearLayout.HORIZONTAL);
        normalRow.setGravity(Gravity.BOTTOM);
        inputEdit = new EditText(this);
        inputEdit.setHint("给助手发个任务…");
        inputEdit.setTextSize(15);
        inputEdit.setTextColor(Ui.INK);
        inputEdit.setHintTextColor(Ui.SUB);
        inputEdit.setMaxLines(4);
        inputEdit.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_SEND);
        inputEdit.setInputType(InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        inputEdit.setOnEditorActionListener((v, actionId, event) -> {
            if (actionId == android.view.inputmethod.EditorInfo.IME_ACTION_SEND) {
                sendTask();
                return true;
            }
            return false;
        });
        inputEdit.setBackground(Ui.fill(this, Ui.SOFT, 20));
        inputEdit.setPadding(dp(16), dp(10), dp(16), dp(10));
        normalRow.addView(inputEdit, new LinearLayout.LayoutParams(0, -2, 1f));

        micBtn = new TextView(this);
        micBtn.setText("🎙");
        micBtn.setTextSize(19);
        micBtn.setGravity(Gravity.CENTER);
        micBtn.setBackground(Ui.circle(Ui.PRIMARY_SOFT));
        normalRow.addView(micBtn, circleBtnLp());
        sendBtn = new TextView(this);
        sendBtn.setText("↑");
        sendBtn.setTextSize(19);
        sendBtn.setTextColor(0xFFFFFFFF);
        sendBtn.setGravity(Gravity.CENTER);
        sendBtn.setTypeface(Typeface.DEFAULT_BOLD);
        sendBtn.setBackground(Ui.circle(Ui.PRIMARY));
        normalRow.addView(sendBtn, circleBtnLp());
        sendBtn.setOnClickListener(v -> sendTask());
        Ui.press(micBtn, 0.9f);
        Ui.press(sendBtn, 0.9f);
        micBtn.setOnTouchListener(this::handleTalkTouch);
        inputBar.addView(normalRow, lp(-1, -2));

        recordingRow = new LinearLayout(this);
        recordingRow.setOrientation(LinearLayout.HORIZONTAL);
        recordingRow.setGravity(Gravity.CENTER);
        recordingRow.setBackground(Ui.fill(this, Ui.PRIMARY_SOFT, 24));
        recordingRow.setPadding(dp(16), dp(12), dp(16), dp(12));
        recordingRow.setVisibility(View.GONE);
        recLabel = Ui.text(this, "● 00:00 · 松开发送，上滑取消", 14, Ui.PRIMARY_DEEP, true);
        recordingRow.addView(recLabel, lp(-1, -2));
        inputBar.addView(recordingRow, lp(-1, -2));
        home.addView(inputBar, lp(-1, -2));
        return home;
    }

    private LinearLayout.LayoutParams circleBtnLp() {
        LinearLayout.LayoutParams p = lp(dp(42), dp(42));
        p.leftMargin = dp(8);
        return p;
    }

    // ================= 页面切换 =================

    private void showConnect(String message) {
        poller.stop();
        scanner.cancel();
        connectScreen.setVisibility(View.VISIBLE);
        homeScreen.setVisibility(View.GONE);
        if (!TextUtils.isEmpty(message)) scanStatus.setText(message);
        // 恢复上次手动地址
        String host = pairing.savedHost();
        if (!host.isEmpty()) hostEdit.setText(host);
        String port = pairing.savedPort();
        portEdit.setText(port.isEmpty() ? "8788" : port);
        startScan();
    }

    private void showHome() {
        scanner.cancel();
        connectScreen.setVisibility(View.GONE);
        homeScreen.setVisibility(View.VISIBLE);
        announceCapabilities(pager.currentCommandNames());
        renderModel();
        poller.start(new Poller.Listener() {
            @Override public void onModelUpdated() { renderModel(); }
            @Override public void onUnpaired() {
                pairing.forget();
                showConnect("电脑已解除配对，重新连接一下");
            }
        });
    }

    // ================= 扫描与配对 =================

    private void startScan() {
        scanner.cancel();
        foundList.removeAllViews();
        foundCount = 0;
        scanCard.setVisibility(View.VISIBLE);
        manualCard.setVisibility(View.VISIBLE);
        pairingCard.setVisibility(View.GONE);
        radar.start();
        scanStatus.setText("正在扫描附近的电脑…");
        scanStatus.setTextColor(Ui.SUB);
        rescanBtn.setEnabled(true);
        scanner.scan(this, new LanScanner.Listener() {
            @Override public void onFound(LanScanner.Found f) {
                foundCount++;
                addFoundRow(f);
                scanStatus.setText("找到 " + foundCount + " 台，点一下即连接");
                scanStatus.setTextColor(Ui.OK);
            }
            @Override public void onDone(int count) {
                radar.stop();
                if (count == 0 && !TextUtils.isEmpty(model.connectionError)) return;
                if (count == 0) {
                    scanStatus.setText("附近没发现桥接器。确认电脑工作台已打开，"
                            + "且手机与电脑连同一个 Wi-Fi。");
                    scanStatus.setTextColor(Ui.SUB);
                }
            }
        });
    }

    private void addFoundRow(LanScanner.Found f) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setBackground(Ui.card(this, 0xFFFAF8F5, 14, Ui.LINE, 1));
        row.setPadding(dp(14), dp(12), dp(14), dp(12));
        row.setOnClickListener(v -> startPairing(f.ip, f.port, f.name));
        Ui.press(row, 0.98f);

        View dot = new View(this);
        dot.setBackground(Ui.circle(Ui.OK));
        row.addView(dot, lp(dp(9), dp(9)));

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        TextView name = Ui.text(this, f.name, 15, Ui.INK, true);
        name.setSingleLine();
        name.setEllipsize(TextUtils.TruncateAt.END);
        col.addView(name, lp(-2, -2));
        TextView addr = Ui.text(this, f.ip + " · " + f.port, 12, Ui.SUB, false);
        col.addView(addr, lp(-2, -2));
        LinearLayout.LayoutParams colLp = new LinearLayout.LayoutParams(0, -2, 1f);
        colLp.leftMargin = dp(10);
        row.addView(col, colLp);

        TextView chevron = Ui.text(this, "›", 20, Ui.SUB, false);
        row.addView(chevron, lp(-2, -2));

        LinearLayout.LayoutParams rowLp = lp(-1, -2);
        rowLp.topMargin = dp(6);
        foundList.addView(row, rowLp);
    }

    private void startManualPair() {
        String host = hostEdit.getText().toString().trim();
        int port;
        try {
            port = Integer.parseInt(portEdit.getText().toString().trim());
        } catch (NumberFormatException e) {
            port = -1;
        }
        if (host.isEmpty() || port <= 0 || port > 65535) {
            scanStatus.setText("请输入有效的电脑地址和端口");
            scanStatus.setTextColor(Ui.ERR);
            return;
        }
        startPairing(host, port, host);
    }

    private void startPairing(String host, int port, String name) {
        lastHost = host;
        lastPort = port;
        lastName = name;
        scanner.cancel();
        radar.stop();
        scanCard.setVisibility(View.GONE);
        manualCard.setVisibility(View.GONE);
        pairingCard.setVisibility(View.VISIBLE);
        pairName.setText(name);
        pairCode.setVisibility(View.GONE);
        pairRetry.setVisibility(View.GONE);
        pairCancel.setText("取消");
        pairMsg.setText("正在连接电脑…");
        pairMsg.setTextColor(Ui.SUB);
        pairing.startManualPair(host, port, false, (phase, message, code) -> ui.post(() -> {
            switch (phase) {
                case WAITING_APPROVAL:
                    pairCode.setText(code);
                    pairCode.setVisibility(View.VISIBLE);
                    pairMsg.setText("已找到电脑，等你在电脑上确认");
                    pairMsg.setTextColor(Ui.SUB);
                    break;
                case PAIRED:
                    Toast.makeText(this, "配对成功", Toast.LENGTH_SHORT).show();
                    showHome();
                    break;
                case ERROR:
                    pairCode.setVisibility(View.GONE);
                    pairMsg.setText(message);
                    pairMsg.setTextColor(Ui.ERR);
                    pairRetry.setVisibility(View.VISIBLE);
                    pairCancel.setText("返回");
                    break;
                default:
                    break;
            }
        }));
    }

    // ================= 板侧事件与命令（对齐固件 vibe_app_board_events/commands） =================

    private static final String[][] BOARD_EVENTS = {
            {"boot.click", "edge"}, {"boot.double", "edge"}, {"boot.triple", "edge"},
            {"motion.shake", "edge"}, {"touch.double", "edge"},
    };

    /** 能力清单上报：事件固定五件套 + 动作命令（素材 commands 表 A1–A9 / 老素材语义名）。 */
    private void announceCapabilities(java.util.List<String> actionNames) {
        if (!bridge.isPaired()) return;
        try {
            StringBuilder body = new StringBuilder("{\"events\":[");
            for (int i = 0; i < BOARD_EVENTS.length; i++) {
                if (i > 0) body.append(',');
                body.append("{\"name\":\"").append(BOARD_EVENTS[i][0])
                        .append("\",\"cadence\":\"").append(BOARD_EVENTS[i][1]).append("\"}");
            }
            body.append("],\"commands\":[");
            int n = 0;
            for (String name : actionNames) {
                if (!name.matches("[A-Za-z][A-Za-z0-9_]{0,15}")) continue;
                if (n++ > 0) body.append(',');
                body.append("{\"name\":\"action.").append(name).append("\"}");
            }
            body.append("]}");
            new Thread(() -> bridge.postJson("/device/capabilities", body.toString()),
                    "made-caps").start();
        } catch (Exception ignored) { }
    }

    /**
     * 手势区（精灵区 + 顶栏共用）：必须消费事件流，否则 DOWN 之后 MOVE/UP
     * 不再派发给本视图，滑动永远收不到结尾（v4 的教训）。
     * 双击（450ms 两击）= touch.double；横滑 = 助手分页跟手；纵滑 = 切会话。
     */
    private boolean onSpriteAreaTouch(View v, MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                swipeDownX = e.getRawX();
                swipeDownY = e.getRawY();
                gestureMode = 0;
                gestureDownMs = System.currentTimeMillis();
                hideKeyboard();
                long now = System.currentTimeMillis();
                if (lastSpriteTapMs != 0 && now - lastSpriteTapMs <= 450) {
                    lastSpriteTapMs = 0;
                    sendBoardEvent("touch.double");
                } else {
                    lastSpriteTapMs = now;
                }
                return true;
            case MotionEvent.ACTION_MOVE: {
                if (gestureMode == 0) {
                    float ddx = e.getRawX() - swipeDownX;
                    float ddy = e.getRawY() - swipeDownY;
                    if (Math.abs(ddx) > dp(12) && Math.abs(ddx) > Math.abs(ddy)) {
                        gestureMode = 1; // 只认横滑；上下滑不做任何切换
                        lastSpriteTapMs = 0;
                        pager.ensureNeighbors(); // 邻页未就绪时立刻补齐
                    }
                }
                if (gestureMode == 1) {
                    pager.setDrag(e.getRawX() - swipeDownX); // 跟手
                }
                return true;
            }
            case MotionEvent.ACTION_UP: {
                float dx = e.getRawX() - swipeDownX;
                if (gestureMode == 1) {
                    pager.endDrag(); // 先结束拖动态，吸附才能接管
                    int target = model.selectedProvider;
                    boolean quickFlick = System.currentTimeMillis() - gestureDownMs < 260
                            && Math.abs(dx) > dp(56);
                    if ((Math.abs(dx) > pager.getWidth() * 0.38f || quickFlick)
                            && model.providers.size() > 1) {
                        // 与固件 chooseProvider 一致：纯相邻环形，不跳过不可用
                        int step = dx < 0 ? 1 : -1;
                        int n = model.providers.size();
                        target = (model.selectedProvider + step + n) % n;
                    }
                    applyProviderSwitch(target);
                }
                gestureMode = 0;
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                if (gestureMode == 1) {
                    pager.endDrag();
                    pager.snapTo(model.selectedProvider); // 回弹
                }
                gestureMode = 0;
                return true;
            }
            default:
                return true;
        }
    }

    /** 提交助手切换：模型先行，Pager 吸附动画收尾（target==当前即回弹）。 */
    private void applyProviderSwitch(int target) {
        if (model.providers.isEmpty()) {
            pager.snapTo(model.selectedProvider);
            return;
        }
        target = Math.max(0, Math.min(target, model.providers.size() - 1));
        int old = model.selectedProvider;
        pager.snapTo(target);
        if (target == old) return;
        Model.Provider p = model.providers.get(target);
        model.selectedProvider = target;
        model.selectedSessionId = "";
        model.sessions.clear();
        model.jobs.clear();
        suppressNextJobCard = true; // 切过去只看精灵，旧任务卡别弹
        poller.kick();
        renderModel();
        Toast.makeText(this, p.available ? p.label
                : p.label + "（不可用：" + p.reason + "）", Toast.LENGTH_LONG).show();
    }

    /** 上报板侧事件（带当前助手范围字段），随后立刻拉一轮命令让动作快跟。 */
    private void sendBoardEvent(String name) {
        Model.Provider p = model.provider();
        if (p == null || p.id.isEmpty()) return;
        JSONObject body = new JSONObject();
        try {
            body.put("name", name);
            JSONObject fields = new JSONObject();
            fields.put("agent", p.id);
            body.put("fields", fields);
        } catch (Exception ignored) { }
        new Thread(() -> {
            bridge.postJson("/device/events", body.toString());
            poller.kick();
        }, "made-event").start();
    }

    /** 板侧命令：动作即命令（action.<名>，playAction 按 manifest commands 映射）；其余手机端忽略。 */
    private void handleBoardCommand(String id, String name, String target, String direction) {
        if (name == null) return;
        if (name.startsWith("action.") && name.length() > 7) {
            SpriteView sv = pager.current();
            if (sv != null) sv.playAction(name.substring(7));
            return;
        }
        if (name.equals("agent.select")) {
            int idx = -1;
            if (!target.isEmpty()) {
                for (int i = 0; i < model.providers.size(); i++) {
                    if (model.providers.get(i).id.equals(target)) { idx = i; break; }
                }
            } else if ("next".equals(direction) || "prev".equals(direction)) {
                int step = "next".equals(direction) ? 1 : -1;
                if (!model.providers.isEmpty() && model.selectedProvider >= 0) {
                    idx = (model.selectedProvider + step + model.providers.size())
                            % model.providers.size();
                }
            }
            if (idx >= 0 && idx != model.selectedProvider) {
                applyProviderSwitch(idx); // 与固件一致：不检查 available
            }
        }
    }

    // ================= 主页渲染 =================

    private void renderModel() {
        Model.Provider p = model.provider();
        if (p != null) {
            providerLabel.setText(p.label + (p.available ? "" : "（不可用）"));
            // 幂等同步：当前页版本/文案；三屏预载由 Pager 自己保证
            pager.sync(model.providers, model.selectedProvider);
        } else {
            providerLabel.setText("选择助手");
        }
        Model.Session s = model.session();
        sessionLabel.setText(s == null || TextUtils.isEmpty(s.title)
                ? "未选择会话" : s.title);

        if (TextUtils.isEmpty(model.connectionError)) {
            connBanner.setVisibility(View.GONE);
        } else {
            connBanner.setText(model.connectionError);
            connBanner.setVisibility(View.VISIBLE);
        }

        // 精灵状态
        Model.Job job = model.jobs.isEmpty() ? null : model.jobs.get(0);
        String state = "idle";
        if (voice.isRecording()) state = "talking";
        else if (job != null) {
            switch (job.status) {
                case "running": case "queued": state = "running"; break;
                case "waiting_confirmation": state = "waiting"; break;
                case "completed": state = "success"; break;
                case "error": case "failed": case "cancelled": state = "sad"; break;
                default: state = "idle";
            }
        }
        int dotColor, chipColor;
        String chipText;
        switch (state) {
            case "talking": dotColor = Ui.PRIMARY; chipColor = Ui.PRIMARY; chipText = "正在听你说"; break;
            case "running": dotColor = Ui.PRIMARY; chipColor = Ui.PRIMARY; chipText = "工作中"; break;
            case "waiting": dotColor = Ui.WARN; chipColor = Ui.WARN; chipText = "等你确认"; break;
            case "success": dotColor = Ui.OK; chipColor = Ui.OK; chipText = "完成了"; break;
            case "sad": dotColor = Ui.ERR; chipColor = Ui.ERR; chipText = "出错了"; break;
            default: dotColor = Ui.OK; chipColor = Ui.SUB; chipText = "待机"; break;
        }
        statusDot.getBackground().setColorFilter(dotColor, android.graphics.PorterDuff.Mode.SRC_IN);
        statusChipLabel.setText(chipText);
        statusChipLabel.setTextColor(chipColor);
        SpriteView sv = pager.current();
        if (sv != null) sv.setStatus(state);

        // 头顶状态图标（固件五态优先级：说话 > 任务状态）
        int pill = 0;
        if (voice.isRecording()) pill = 1;
        else if (voiceUploading) pill = 2;
        else if (job != null) {
            if ("waiting_confirmation".equals(job.status)) pill = 3;
            else if ("running".equals(job.status) || "queued".equals(job.status)) pill = 4;
        }
        statusPill.setPillState(pill);

        // 任务面板：始终露边缘（把手是一致的可供性）；无任务时空态。
        // 切助手/会话后首轮旧任务 → peek；用户新发任务 → 半屏。
        boolean canSend = p != null && p.available && !model.selectedSessionId.isEmpty();
        sendBtn.setAlpha(canSend ? 1f : 0.4f);
        micBtn.setAlpha(canSend && model.voiceAvailable ? 1f : 0.4f);
        if (job == null) {
            lastJobId = "";
            jobCard.setVisibility(View.VISIBLE);
            jobInstruction.setText("暂无任务");
            jobInstruction.setVisibility(View.VISIBLE);
            jobResult.setVisibility(View.GONE);
            jobError.setVisibility(View.GONE);
            jobStatusLabel.setText("空");
            jobDot.getBackground().setColorFilter(Ui.SUB,
                    android.graphics.PorterDuff.Mode.SRC_IN);
            jobActions.setVisibility(View.GONE);
            if (!emptySheetShown) {
                emptySheetShown = true;
                snapToSheet(SHEET_PEEK);
            }
            return;
        }
        emptySheetShown = false;
        boolean isNewJob = !job.id.equals(lastJobId);
        if (isNewJob) {
            lastJobId = job.id;
            jobScroll.scrollTo(0, 0);
        }
        jobCard.setVisibility(View.VISIBLE);
        if (suppressNextJobCard) {
            suppressNextJobCard = false;
            snapToSheet(SHEET_PEEK);
        } else if (isNewJob) {
            snapToSheet(SHEET_HALF);
        }
        jobInstruction.setText(job.instruction);
        int sColor;
        String sText;
        switch (job.status) {
            case "waiting_confirmation": sColor = Ui.WARN; sText = "等你确认"; break;
            case "queued": sColor = Ui.SUB; sText = "排队中"; break;
            case "running": sColor = Ui.PRIMARY; sText = "执行中"; break;
            case "completed": sColor = Ui.OK; sText = "已完成"; break;
            case "error": case "failed": sColor = Ui.ERR; sText = "出错"; break;
            case "cancelled": sColor = Ui.SUB; sText = "已取消"; break;
            default: sColor = Ui.SUB; sText = job.status; break;
        }
        jobDot.getBackground().setColorFilter(sColor, android.graphics.PorterDuff.Mode.SRC_IN);
        jobStatusLabel.setText(sText + (TextUtils.isEmpty(job.progress)
                ? "" : " · " + job.progress));
        jobStatusLabel.setTextColor(sColor);
        jobResult.setText(job.result);
        jobResult.setVisibility(job.result.isEmpty() ? View.GONE : View.VISIBLE);
        jobError.setText(job.error);
        jobError.setVisibility(job.error.isEmpty() ? View.GONE : View.VISIBLE);
        // peek 档藏操作行（只露边缘）；确认/取消在半屏/全屏可见
        jobActions.setVisibility("waiting_confirmation".equals(job.status)
                && sheetState != SHEET_PEEK ? View.VISIBLE : View.GONE);
    }

    // ================= 任务面板（底部拖拽三档） =================

    private int sheetPeekH() { return dp(52); }

    private int sheetHalfH() {
        return Math.max(dp(120), Math.round(spriteAreaHost.getHeight() * 0.45f));
    }

    private int sheetFullH() {
        return Math.max(sheetHalfH() + dp(40),
                Math.round(spriteAreaHost.getHeight() * 0.88f));
    }

    private void setSheetHeight(int h) {
        FrameLayout.LayoutParams lp2 = (FrameLayout.LayoutParams) jobCard.getLayoutParams();
        lp2.height = h;
        jobCard.setLayoutParams(lp2);
    }

    private int nearestSheetState() {
        int h = jobCard.getHeight();
        int peek = sheetPeekH(), half = sheetHalfH(), full = sheetFullH();
        if (Math.abs(h - peek) <= Math.abs(h - half) && Math.abs(h - peek) <= Math.abs(h - full)) {
            return SHEET_PEEK;
        }
        return Math.abs(h - half) <= Math.abs(h - full) ? SHEET_HALF : SHEET_FULL;
    }

    /** 松手吸附（180ms 高度动画）。 */
    private void snapToSheet(int state) {
        sheetState = state;
        int target = state == SHEET_PEEK ? sheetPeekH()
                : state == SHEET_HALF ? sheetHalfH() : sheetFullH();
        final int from = jobCard.getHeight();
        if (sheetAnimating || Math.abs(from - target) < dp(2)) {
            setSheetHeight(target);
            renderModel();
            return;
        }
        sheetAnimating = true;
        android.animation.ValueAnimator a =
                android.animation.ValueAnimator.ofInt(from, target);
        a.setDuration(180);
        a.addUpdateListener(anim -> setSheetHeight((int) anim.getAnimatedValue()));
        a.addListener(new android.animation.AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(Animator animation) {
                sheetAnimating = false;
                setSheetHeight(target);
                renderModel();
            }
        });
        a.start();
    }

    /** 点击精灵/顶栏：把弹出的虚拟键盘收回去。 */
    private void hideKeyboard() {
        android.view.inputmethod.InputMethodManager imm =
                (android.view.inputmethod.InputMethodManager)
                        getSystemService(INPUT_METHOD_SERVICE);
        View focus = getCurrentFocus();
        if (imm != null && focus != null) {
            imm.hideSoftInputFromWindow(focus.getWindowToken(), 0);
        }
    }

    /**
     * 把手拖动：跟手改面板高度。松手吸附到**最近档位**（档位锚点
     * peek/半屏/全屏的中点恰为 1/4 与 3/4 屏，与交互规则一致；
     * 从任一档出发两个方向都能拉到头）。
     */
    private boolean onSheetHandleTouch(View v, MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                swipeDownX = e.getRawX();
                swipeDownY = e.getRawY();
                dragStartH = jobCard.getHeight();
                return true;
            case MotionEvent.ACTION_MOVE: {
                if (sheetAnimating) return true;
                float delta = swipeDownY - e.getRawY(); // 上划为正
                int rawH = dragStartH + (int) delta;
                int newH;
                if (rawH > sheetFullH()) {
                    newH = sheetFullH() + (rawH - sheetFullH()) / 4; // 顶端阻尼
                } else if (rawH < sheetPeekH()) {
                    newH = sheetPeekH() - (sheetPeekH() - rawH) / 4; // 底端阻尼
                } else {
                    newH = rawH;
                }
                newH = Math.min(newH, spriteAreaHost.getHeight());
                setSheetHeight(newH);
                return true;
            }
            case MotionEvent.ACTION_UP: {
                if (sheetAnimating) return true;
                snapToSheet(nearestSheetState());
                return true;
            }
            default:
                return true;
        }
    }

    // ================= 主页操作 =================

    private void pickSession() {
        String[] items = new String[model.sessions.size() + 1];
        items[0] = "＋ 新建会话";
        int selected = -1;
        for (int i = 0; i < model.sessions.size(); i++) {
            Model.Session s = model.sessions.get(i);
            items[i + 1] = TextUtils.isEmpty(s.title) ? "会话 " + (i + 1) : s.title;
            if (s.id.equals(model.selectedSessionId)) selected = i + 1;
        }
        new AlertDialog.Builder(this)
                .setTitle("会话")
                .setSingleChoiceItems(items, selected, (d, which) -> {
                    d.dismiss();
                    if (which == 0) { createSession(); return; }
                    if (which - 1 < model.sessions.size()) {
                        String sid = model.sessions.get(which - 1).id;
                        if (!sid.equals(model.selectedSessionId)) {
                            model.selectedSessionId = sid;
                            model.jobs.clear();
                            poller.kick();
                            renderModel();
                        }
                    }
                })
                .show();
    }

    private void showMenu() {
        new AlertDialog.Builder(this)
                .setItems(new String[]{"切换会话", "删除会话", "新建会话", "断开连接"}, (d, which) -> {
                    if (which == 0) pickSession();
                    else if (which == 1) deleteCurrentSession();
                    else if (which == 2) createSession();
                    else {
                        pairing.forget();
                        model.sessions.clear();
                        model.jobs.clear();
                        model.selectedSessionId = "";
                        showConnect("已断开。重新扫描或手动添加电脑");
                    }
                })
                .show();
    }

    /** 删除当前会话（POST /device/sessions/<id>/delete），先确认再动手。 */
    private void deleteCurrentSession() {
        Model.Session s = model.session();
        if (s == null) {
            Toast.makeText(this, "当前没有会话", Toast.LENGTH_SHORT).show();
            return;
        }
        String title = TextUtils.isEmpty(s.title) ? "会话 " + s.id.substring(0, 6) : s.title;
        new AlertDialog.Builder(this)
                .setTitle("删除会话")
                .setMessage("确定删除「" + title + "」？删除后电脑上的对话也会一并结束。")
                .setPositiveButton("删除", (d, w) -> new Thread(() -> {
                    Bridge.Resp r = bridge.postJson("/device/sessions/" + s.id + "/delete", "{}");
                    ui.post(() -> {
                        if (r.ok()) {
                            Toast.makeText(this, "已删除", Toast.LENGTH_SHORT).show();
                        } else {
                            Toast.makeText(this, "删除失败：" + r.errorText(),
                                    Toast.LENGTH_SHORT).show();
                        }
                        model.selectedSessionId = "";
                        model.sessions.clear();
                        model.jobs.clear();
                        suppressNextJobCard = true;
                        poller.kick();
                        renderModel();
                    });
                }, "made-del").start())
                .setNegativeButton("取消", null)
                .show();
    }

    private void createSession() {
        Model.Provider p = model.provider();
        if (p == null) return;
        String project = model.defaultProject;
        new Thread(() -> {
            Bridge.Resp r = bridge.postJson("/device/sessions?provider=" + p.id
                    + (project.isEmpty() ? "" : "&projectId=" + project), "{}");
            if (r.ok()) {
                try {
                    JSONObject o = new JSONObject(r.body);
                    model.selectedSessionId = o.optString("id");
                    ui.post(() -> {
                        model.sessions.clear(); // 触发下轮重建
                        poller.kick();
                        Toast.makeText(this, "已新建会话", Toast.LENGTH_SHORT).show();
                    });
                } catch (Exception ignored) { }
            } else {
                ui.post(() -> Toast.makeText(this, "新建失败：" + r.errorText(),
                        Toast.LENGTH_SHORT).show());
            }
        }, "made-new-session").start();
    }

    private void sendTask() {
        String text = inputEdit.getText().toString().trim();
        if (text.isEmpty()) return;
        if (model.selectedSessionId.isEmpty()) {
            Toast.makeText(this, "还没有会话，先点顶部新建一个", Toast.LENGTH_SHORT).show();
            return;
        }
        inputEdit.setText("");
        suppressNextJobCard = false;
        JSONObject body = new JSONObject();
        try {
            body.put("instruction", text);
            body.put("sessionId", model.selectedSessionId);
        } catch (Exception ignored) { }
        new Thread(() -> {
            Bridge.Resp r = bridge.postJson("/device/tasks", body.toString());
            if (!r.ok()) {
                ui.post(() -> Toast.makeText(this, "发送失败：" + r.errorText(),
                        Toast.LENGTH_SHORT).show());
            }
            poller.kick();
        }, "made-send").start();
    }

    private void act(String verb) {
        if (model.jobs.isEmpty()) return;
        String id = model.jobs.get(0).id;
        new Thread(() -> {
            bridge.postJson("/device/tasks/" + id + "/" + verb, "");
            poller.kick();
        }, "made-act").start();
    }

    // ================= 语音（按住说话，上滑取消） =================

    private boolean handleTalkTouch(View v, MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                        != PackageManager.PERMISSION_GRANTED) {
                    requestPermissions(new String[]{android.Manifest.permission.RECORD_AUDIO},
                            REQ_MIC);
                    return true;
                }
                if (!model.voiceAvailable) {
                    Toast.makeText(this, "电脑端未配置语音模型", Toast.LENGTH_SHORT).show();
                    return true;
                }
                if (!voice.start()) {
                    Toast.makeText(this, "麦克风不可用", Toast.LENGTH_SHORT).show();
                    return true;
                }
                recStartMs = System.currentTimeMillis();
                recCancelled = false;
                recDownY = e.getRawY();
                normalRow.setVisibility(View.GONE);
                recordingRow.setVisibility(View.VISIBLE);
                startRecTick();
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                if (e.getRawY() < recDownY - dp(90) && !recCancelled) {
                    recCancelled = true;
                    recLabel.setText("● 松开取消");
                } else if (!recCancelled) {
                    long secs = (System.currentTimeMillis() - recStartMs) / 1000;
                    recLabel.setText(String.format("● 00:%02d · 松开发送，上滑取消", secs));
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (!voice.isRecording()) return true;
                stopRecTick();
                normalRow.setVisibility(View.VISIBLE);
                recordingRow.setVisibility(View.GONE);
                final boolean cancelled = recCancelled;
                new Thread(() -> {
                    byte[] wav = voice.stopAndGetWav();
                    if (cancelled) return;
                    if (wav == null) {
                        ui.post(() -> Toast.makeText(this, "说话时间太短",
                                Toast.LENGTH_SHORT).show());
                        return;
                    }
                    Model.Provider p = model.provider();
                    Model.Session s = model.session();
                    if (p == null || s == null) return;
                    suppressNextJobCard = false;
                                voiceUploading = true;
                    ui.post(this::renderModel);
                    String path = "/device/voice?provider=" + p.id
                            + "&projectId=" + s.projectId + "&sessionId=" + s.id
                            + "&uiLocale=zh-CN";
                    Bridge.Resp r = bridge.postBytes(path, wav, "audio/wav", 90000);
                    voiceUploading = false;
                    if (r.ok()) {
                        try {
                            String transcript =
                                    new JSONObject(r.body).optString("transcript");
                            ui.post(() -> Toast.makeText(this, "识别：" + transcript,
                                    Toast.LENGTH_LONG).show());
                        } catch (Exception ignored) { }
                    } else {
                        ui.post(() -> Toast.makeText(this, "识别失败：" + r.errorText(),
                                Toast.LENGTH_LONG).show());
                    }
                    poller.kick();
                }, "made-upload").start();
                return true;
            }
            default:
                return false;
        }
    }

    private void startRecTick() {
        recTick = new Runnable() {
            @Override public void run() {
                if (!voice.isRecording()) return;
                long secs = (System.currentTimeMillis() - recStartMs) / 1000;
                if (!recCancelled) {
                    recLabel.setText(String.format("● 00:%02d · 松开发送，上滑取消", secs));
                    ui.postDelayed(this, 250);
                }
            }
        };
        ui.postDelayed(recTick, 250);
    }

    private void stopRecTick() {
        if (recTick != null) {
            ui.removeCallbacks(recTick);
            recTick = null;
        }
    }

    @Override public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        super.onRequestPermissionsResult(code, perms, results);
        if (code == REQ_MIC && results.length > 0
                && results[0] == PackageManager.PERMISSION_GRANTED) {
            Toast.makeText(this, "已授权，再按住麦克风说话", Toast.LENGTH_SHORT).show();
        } else if (code == REQ_MIC) {
            Toast.makeText(this, "没有麦克风权限，无法语音", Toast.LENGTH_SHORT).show();
        }
    }
}
