package com.zyar20a.irremote2;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.app.AlertDialog;
import android.bluetooth.BluetoothDevice;
import android.content.DialogInterface;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.Locale;

/**
 * ZYAR20A 蓝牙遥控 · v3.0
 *
 * 通信从红外改成经典蓝牙（HC-05，SPP 串口透传），发的是单字符命令：
 *   F 前进  B 后退  L 左旋  R 右旋  S 停  G/g 遛狗开关  P/p 比赛开关
 *
 * 与红外版的关键差别：
 *   · 有连接、有方向：发送失败能立刻知道，而不是猜"车怎么不动"
 *   · 断连即停：send() 失败会关掉 socket，车端 450ms 看门狗兜底刹车
 *   · 不再需要连发多帧抗丢包，一条命令发一次就够
 */
public class MainActivity extends Activity
{
    /* ---------- 动作 ID ----------
     * 蓝牙改造后这些不再是红外码，只是界面内部的"动作编号"，
     * 由 cmdOf() 映射成发给车的单个字母。保留名字是为了不动 UI 和轮盘回调。 */
    private static final int CODE_FWD   = 1;
    private static final int CODE_BACK  = 2;
    private static final int CODE_LEFT  = 3;    // 单边左转：已废弃
    private static final int CODE_RIGHT = 4;    // 单边右转：已废弃
    private static final int CODE_STOP  = 5;
    private static final int CODE_SPINL = 6;
    private static final int CODE_SPINR = 7;

    /* ---------- 自动模式：开、关各一个码，车端都是幂等的 ----------
     * ★ 为什么不用"按一下切换"（v2.6 改的，用户实测踩到）：
     *   红外是单向的，车跑开、角度偏了，那一帧命令丢一次就再也停不下来；
     *   而多发几帧提高命中率，在"切换"语义下会把车来回开关。
     *   拆成开/关两个码之后，同一条命令发一百遍结果都一样，才能放心连发。
     *     遛狗  开 = 数字键 0      关 = 数字键 1
     *     比赛  开 = 数字键 2      关 = 数字键 3   */
    private static final int CODE_DOG_ON    = 8;
    private static final int CODE_DOG_OFF   = 9;
    private static final int CODE_RACE_ON   = 10;
    private static final int CODE_RACE_OFF  = 11;

    /* ---------- 配色 ---------- */
    private static final int C_BG     = 0xFF080B10;
    private static final int C_CARD   = 0xFF141B24;
    private static final int C_LINE   = 0xFF232D3A;
    private static final int C_ACCENT = 0xFF35E0A1;   // 遛狗：绿
    private static final int C_RACE   = 0xFF4FA8E8;   // 比赛：蓝
    private static final int C_TEXT   = 0xFFE9F0F7;
    private static final int C_DIM    = 0xFF7C8B9B;
    private static final int C_DANGER = 0xFFFF5E6C;

    /* 按住时的重发周期。蓝牙是点对点连接、不会像红外那样丢帧，
     * 80ms 一次足够维持车端 450ms 的看门狗，同时把延迟压到最低。 */
    private static final int REPEAT_MS = 80;

    /* 帧数参数保留（调用点没改），蓝牙下只发一次 —— 连接本身可靠。 */
    private static final int FRAMES_FIRST = 1;
    private static final int FRAMES_KEEP  = 1;
    private static final int FRAMES_STOP  = 1;
    private static final int FRAME_GAP_MS = 0;

    private static final int LOG_SHOW  = 9;     // 日志区显示几行

    private BtLink bt;
    private Vibrator vib;

    private WheelPad pad;
    private TextView pill;
    private TextView logView;
    private TextView banner;              // 更新提示条，默认隐藏
    private TextView dogBtn;              // 遛狗模式按钮

    /* 遛狗模式是「按一下开、再按一下关」的切换语义。
     * 红外是单向的，车不会回报状态，所以只能在这里自己记着 ——
     * 一旦用户手动操作（按圆盘/停车），车端会退出遛狗，这里也必须同步清掉。 */
    private boolean dogOn = false;
    private boolean raceOn = false;
    private TextView raceBtn;             // 比赛模式按钮

    /* ---- 自更新状态 ---- */
    private int     upCode      = 0;      // 发现的新版 versionCode
    private String  upName      = "";
    private String  upBase      = null;   // 命中的更新源
    private boolean upDownloaded = false; // 包已落盘、校验通过
    private boolean upBusy       = false; // 正在下载，防止连点

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ArrayList<String> logs = new ArrayList<String>();
    private final SimpleDateFormat tf =
            new SimpleDateFormat("HH:mm:ss", Locale.US);

    /* 发射必须串行：ConsumerIrManager.transmit 不能两条线程同时调，
       否则两帧波形会互相打断。旧版（能用）也是单条 HandlerThread 排队发。 */
    private HandlerThread txThread;
    private Handler tx;

    private int     activeCode      = 0;
    private String  activeName      = "";
    private boolean activeNeedsStop = false;

    private final Runnable repeatTask = new Runnable() {
        @Override public void run() {
            if (activeCode != 0) {
                fire(activeCode, activeName, false, FRAMES_KEEP);
                ui.postDelayed(this, REPEAT_MS);
            }
        }
    };

    /* ================= 生命周期 ================= */

    @Override
    protected void onCreate(Bundle b)
    {
        super.onCreate(b);

        Window w = getWindow();
        w.setStatusBarColor(C_BG);
        w.setNavigationBarColor(C_BG);
        w.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        bt  = new BtLink();
        vib = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);

        txThread = new HandlerThread("bt-tx");
        txThread.start();
        tx = new Handler(txThread.getLooper());

        setContentView(buildUi());

        log("蓝牙: " + (bt.available() ? (bt.enabled() ? "已打开" : "未打开") : "本机不支持"));
        log("未连接 —— 点右上角「连接」选 HC-05");
        log("v3.0 蓝牙遥控：圆盘=原地转向，按住每 " + REPEAT_MS + "ms 发一次");

        /* Android 12+ 访问已配对设备需要运行时权限，否则 getBondedDevices 直接抛异常 */
        if (Build.VERSION.SDK_INT >= 31
                && checkSelfPermission("android.permission.BLUETOOTH_CONNECT")
                   != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] { "android.permission.BLUETOOTH_CONNECT" }, 1001);
        }

        // 后台查更新：没网/镜像挂了都只是安静跳过，不影响遥控
        startUpdateCheck();
    }

    @Override
    protected void onPause()
    {
        /* 遛狗模式是车自己在跑，退到后台必须显式叫停。
         * 红外是单向的，App 一关就没法再发指令，只能等车端 60 秒硬超时 ——
         * 那期间车会一直追着东西跑，太危险。 */
        if (dogOn || raceOn) {
            if (dogOn)  { fireSlow(CODE_DOG_OFF,  "退出遛狗", 5, 110); dogOn  = false; renderDogBtn();  }
            if (raceOn) { fireSlow(CODE_RACE_OFF, "退出比赛", 5, 110); raceOn = false; renderRaceBtn(); }
            fire(CODE_STOP, "退出自动模式", true, FRAMES_STOP);
            log("自动模式：App 退到后台，已叫停");
        }
        release();                       // 退到后台一定先刹车
        super.onPause();
    }

    @Override
    protected void onDestroy()
    {
        ui.removeCallbacksAndMessages(null);
        if (tx != null) { tx.removeCallbacksAndMessages(null); }
        if (txThread != null) { txThread.quitSafely(); }
        super.onDestroy();
    }

    /* ================= 自更新 ================= */

    /** 更新提示条的统一出口：文案 + 可点性 + 颜色 */
    private void setBanner(String text, boolean clickable, int color)
    {
        if (banner == null) { return; }
        banner.setText(text);
        banner.setTextColor(color);
        banner.setVisibility(View.VISIBLE);
        banner.setClickable(clickable);
        banner.setAlpha(clickable ? 1f : 0.7f);
    }

    private void startUpdateCheck()
    {
        Updater.check(this, updateCb);
    }

    private final Updater.Cb updateCb = new Updater.Cb()
    {
        @Override public void onFound(int code, String name, String note, String base)
        {
            upCode = code;
            upName = name;
            upBase = base;
            final String label = (name != null && name.length() > 0) ? ("v" + name) : ("#" + code);
            setBanner("↑  发现新版 " + label + " · 点此更新", true, C_ACCENT);
            log("↑ 发现新版 " + label + "（" + code + "）");
        }

        @Override public void onProgress(int pct)
        {
            setBanner("↓  下载中 " + pct + "%", false, C_DIM);
        }

        @Override public void onDone()
        {
            upDownloaded = true;
            upBusy = false;
            setBanner("✓  下载完成 · 点此安装", true, C_ACCENT);
            log("✓ 更新包已就绪（" + upName + "），点横幅安装");
            // 立刻尝试拉起安装器；缺权限时 Updater 会跳系统设置页
            if (!Updater.install(MainActivity.this)) {
                /* 两种可能：没给"安装未知应用"权限，或者包版本不达标被丢掉。
                 * 后者会把文件删掉，所以横幅改成"点此重下"，别让用户一直点装。 */
                if (Updater.cachedApkCode(MainActivity.this) < 0) {
                    upDownloaded = false;
                    setBanner("✗  更新包版本不对 · 点此重下", true, C_DANGER);
                    log("✗ 下载到的包版本不达标，已丢弃。镜像可能还没刷新，稍后再点");
                } else {
                    setBanner("→  允许「安装未知应用」后，再点这里", true, C_ACCENT);
                    log("需要允许安装未知应用：设置 → 应用 → 本应用 → 安装未知应用");
                }
            }
        }

        @Override public void onFail(String why)
        {
            upDownloaded = false;
            upBusy = false;
            setBanner("✗  更新失败 · 点此重试", true, C_DANGER);
            log("✗ 更新失败 " + why);
        }
    };

    private void onBannerTap()
    {
        if (upBase == null) { return; }

        /* 包已在本地：先确认它确实是"要装的那个版本"。
         * ★ 不能直接复用 —— 镜像缓存旧包时这里会一直是旧版本，
         *   点多少次都是"已安装更高版本"，永远下不到新的（2026-11-17 踩到）。 */
        if (upDownloaded || Updater.apkReady(this)) {
            final int local = Updater.cachedApkCode(this);
            if (local > 0 && upCode > 0 && local != upCode) {
                log("本地包是 v" + local + "，不是 v" + upCode + "，丢掉重下");
                upDownloaded = false;                  // 走下面的下载流程
            } else {
                upDownloaded = true;
                if (!Updater.install(this)) {
                    setBanner("→  允许「安装未知应用」后，再点这里", true, C_ACCENT);
                    log("请在系统设置里允许安装未知应用，然后回来再点一次横幅");
                }
                return;
            }
        }

        if (upBusy) { return; }        // 连点保护
        upBusy = true;
        setBanner("↓  连接更新源…", false, C_DIM);
        Updater.download(this, upBase, upCode, updateCb);
    }

    /* ================= 界面 ================= */

    private View buildUi()
    {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackground(gradient(C_BG, 0xFF05070A));
        int p = dp(18);
        root.setPadding(p, dp(14), p, dp(14));

        /* ---- 顶栏 ---- */
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);

        LinearLayout titleBox = new LinearLayout(this);
        titleBox.setOrientation(LinearLayout.VERTICAL);

        TextView title = new TextView(this);
        title.setText("ZYAR20A");
        title.setTextColor(C_TEXT);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 21);
        title.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        title.setLetterSpacing(0.08f);

        TextView sub = new TextView(this);
        sub.setText("蓝牙遥控 · HC-05 SPP");
        sub.setTextColor(C_DIM);
        sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        sub.setPadding(0, dp(2), 0, 0);

        titleBox.addView(title);
        titleBox.addView(sub);

        pill = new TextView(this);
        pill.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        pill.setPadding(dp(12), dp(6), dp(12), dp(6));
        pill.setTextColor(C_ACCENT);
        pill.setBackground(round(0x1A35E0A1, 0x5535E0A1, dp(20)));
        /* 状态块直接当"连接"按钮用：未连接时点它弹设备列表 */
        pill.setClickable(true);
        pill.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (bt != null && bt.connected()) {
                    log("已连接 " + bt.deviceLabel());
                } else {
                    pickBtDevice();
                }
            }
        });
        renderPill();

        LinearLayout.LayoutParams lpTitle = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        head.addView(titleBox, lpTitle);
        head.addView(pill);
        root.addView(head, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        /* ---- 更新提示条（默认隐藏，发现有新版才出现）---- */
        banner = new TextView(this);
        banner.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        banner.setTextColor(C_ACCENT);
        banner.setPadding(dp(14), dp(10), dp(14), dp(10));
        banner.setBackground(round(0x1A35E0A1, 0x5535E0A1, dp(12)));
        banner.setGravity(Gravity.CENTER);
        banner.setVisibility(View.GONE);
        banner.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { onBannerTap(); }
        });
        LinearLayout.LayoutParams lpBanner = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lpBanner.topMargin = dp(10);
        root.addView(banner, lpBanner);

        /* ---- 圆盘 ---- */
        FrameLayout padHolder = new FrameLayout(this);
        pad = new WheelPad(this);
        pad.setListener(new WheelPad.Listener() {
            @Override public void onMove(int code, String name) {
                press(code, name, true);
            }
            @Override public void onUp() {
                release();
            }
            @Override public void onStop() {
                press(CODE_STOP, "停车", false);
            }
        });
        FrameLayout.LayoutParams lpPad = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT);
        lpPad.gravity = Gravity.CENTER;
        padHolder.addView(pad, lpPad);
        root.addView(padHolder, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        /* ---- 转向已并入圆盘左右 ----
           原来这里有一排「原地左旋 / 原地右旋」长按按钮。现在圆盘的左右
           直接发旋转键码，功能重合，删掉整排，把纵向空间留给圆盘。
           键码常量 CODE_SPINL / CODE_SPINR 仍然保留在上面。 */

        /* ---- 遛狗模式 ----
           车端逻辑：启动后自己扫一圈，去追离得最近的「会动的东西」；
           没有会动的，就在扫到的目标里挑最近的过去。
           它是切换式，所以这里只要发一帧，接下来车自己跑。 */
        dogBtn = new TextView(this);
        dogBtn.setGravity(Gravity.CENTER);
        dogBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14.5f);
        dogBtn.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        dogBtn.setPadding(0, dp(13), 0, dp(13));
        dogBtn.setClickable(true);
        dogBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { toggleDog(); }
        });
        renderDogBtn();

        LinearLayout.LayoutParams lpDog = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lpDog.topMargin = dp(10);
        root.addView(dogBtn, lpDog);

        /* ---- 比赛模式 ----
           车端逻辑：只认"最近"，不管动没动、不论多远，一路顶过去；
           贴近后还要多推一段（车底有网，物块会被兜进底盘），然后换向继续找。 */
        raceBtn = new TextView(this);
        raceBtn.setGravity(Gravity.CENTER);
        raceBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14.5f);
        raceBtn.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        raceBtn.setPadding(0, dp(13), 0, dp(13));
        raceBtn.setClickable(true);
        raceBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { toggleRace(); }
        });
        renderRaceBtn();

        LinearLayout.LayoutParams lpRace = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lpRace.topMargin = dp(8);
        root.addView(raceBtn, lpRace);

        /* ---- 日志 ---- */
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(round(C_CARD, C_LINE, dp(14)));
        card.setPadding(dp(14), dp(10), dp(14), dp(12));
        card.setMinimumHeight(dp(120));

        LinearLayout cardHead = new LinearLayout(this);
        cardHead.setOrientation(LinearLayout.HORIZONTAL);
        cardHead.setGravity(Gravity.CENTER_VERTICAL);

        TextView cardTitle = new TextView(this);
        cardTitle.setText("发送记录");
        cardTitle.setTextColor(C_DIM);
        cardTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        cardTitle.setLetterSpacing(0.06f);

        TextView clear = new TextView(this);
        clear.setText("清空");
        clear.setTextColor(C_DIM);
        clear.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        clear.setPadding(dp(10), dp(4), 0, dp(4));
        clear.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                logs.clear();
                renderLog();
            }
        });

        LinearLayout.LayoutParams lpSpacer = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        cardHead.addView(cardTitle, lpSpacer);
        cardHead.addView(clear);

        logView = new TextView(this);
        logView.setTextColor(0xFF9FB0C0);
        logView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        logView.setTypeface(Typeface.MONOSPACE);
        logView.setLineSpacing(dp(3), 1f);
        logView.setPadding(0, dp(8), 0, 0);

        card.addView(cardHead);
        card.addView(logView);

        LinearLayout.LayoutParams lpCard = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lpCard.topMargin = dp(12);
        root.addView(card, lpCard);

        /* ---- 底部说明 ---- */
        TextView foot = new TextView(this);
        foot.setText("按住每 " + REPEAT_MS + " ms 发送  ·  松手立即刹车  ·  断连自动停车");
        foot.setTextColor(0xFF4F5B69);
        foot.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        foot.setGravity(Gravity.CENTER);
        foot.setPadding(0, dp(10), 0, 0);
        root.addView(foot);

        renderLog();
        return root;
    }

    /** 长按即持续发、松手即停的按钮。当前界面未使用（转向已并入圆盘），
     *  留作以后加功能键的现成模板 —— 删掉也不影响编译。 */
    private TextView makeHoldButton(String label, final int code, final String name)
    {
        TextView tv = new TextView(this);
        tv.setText(label);
        tv.setTextColor(C_TEXT);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        tv.setGravity(Gravity.CENTER);
        tv.setBackground(round(C_CARD, C_LINE, dp(14)));
        tv.setClickable(true);

        tv.setOnTouchListener(new View.OnTouchListener() {
            @Override public boolean onTouch(View v, MotionEvent e)
            {
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        v.setBackground(round(0x2635E0A1, C_ACCENT, dp(14)));
                        v.animate().scaleX(0.97f).scaleY(0.97f).setDuration(70).start();
                        press(code, name, true);
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        v.setBackground(round(C_CARD, C_LINE, dp(14)));
                        v.animate().scaleX(1f).scaleY(1f).setDuration(90).start();
                        release();
                        return true;
                }
                return false;
            }
        });
        return tv;
    }

    /* ================= 发送 ================= */

    /** 从系统报告的载波范围里挑出 38000 能用的那个，挑不到就退到最近边界 */
    /* ================= 遛狗模式 ================= */

    private void renderDogBtn()
    {
        if (dogBtn == null) { return; }

        if (dogOn) {
            dogBtn.setText("●  遛狗中 · 点此停止");
            dogBtn.setTextColor(0xFF06210F);
            dogBtn.setBackground(round(C_ACCENT, C_ACCENT, dp(14)));
        } else {
            dogBtn.setText("遛狗模式  ·  自动追最近的移动物体");
            dogBtn.setTextColor(C_ACCENT);
            dogBtn.setBackground(round(0x1435E0A1, 0x5535E0A1, dp(14)));
        }
    }

    private void renderRaceBtn()
    {
        if (raceBtn == null) { return; }

        if (raceOn) {
            raceBtn.setText("●  比赛中 · 点此停止");
            raceBtn.setTextColor(0xFF0A1A24);
            raceBtn.setBackground(round(C_RACE, C_RACE, dp(14)));
        } else {
            raceBtn.setText("比赛模式  ·  一直朝最近的东西推过去");
            raceBtn.setTextColor(C_RACE);
            raceBtn.setBackground(round(0x1422A6D8, 0x5522A6D8, dp(14)));
        }
    }

    private void toggleDog()
    {
        if (bt == null || !bt.connected()) { log("✗ 蓝牙未连接，先点右上角状态块连接"); return; }

        if (raceOn) {                       // 两种自动模式互斥
            raceOn = false;
            renderRaceBtn();
            fireSlow(CODE_RACE_OFF, "比赛模式 关", 4, 120);
        }

        dogOn = !dogOn;
        renderDogBtn();
        buzz(dogOn ? 35 : 15);

        if (dogOn) {
            fireSlow(CODE_DOG_ON, "遛狗模式 开", 3, 90);
            log("遛狗：先追会动的东西；没有就扫一圈，再去找最近的");
        } else {
            /* 关闭命令连发 6 帧、间隔 130ms：车这时可能已经跑远或转开，
             * 单帧很容易打不中。命令是幂等的，多打几次没有副作用。 */
            fireSlow(CODE_DOG_OFF, "遛狗模式 关", 6, 130);
            log("遛狗：已停止（关闭命令连发 6 帧，防丢帧）");
        }
    }

    private void toggleRace()
    {
        if (bt == null || !bt.connected()) { log("✗ 蓝牙未连接，先点右上角状态块连接"); return; }

        if (dogOn) {                        // 两种自动模式互斥
            dogOn = false;
            renderDogBtn();
            fireSlow(CODE_DOG_OFF, "遛狗模式 关", 4, 120);
        }

        raceOn = !raceOn;
        renderRaceBtn();
        buzz(raceOn ? 35 : 15);

        if (raceOn) {
            fireSlow(CODE_RACE_ON, "比赛模式 开", 3, 90);
            log("比赛：只挑最近的，贴近后多推一段把物块兜进底盘，再换方向");
        } else {
            fireSlow(CODE_RACE_OFF, "比赛模式 关", 6, 130);
            log("比赛：已停止（关闭命令连发 6 帧，防丢帧）");
        }
    }

    private void press(int code, String name, boolean needsStop)
    {
        if (bt == null || !bt.connected()) {
            log("✗ 蓝牙未连接，点右上角状态块连接 HC-05");
            renderPill();
            return;
        }
        if (activeCode == code && needsStop) { return; }   // 同一动作重复按下不重发

        /* 手动接管：车端收到任何动作键都会退出自动模式并执行该动作，
         * 这里同步把按钮状态清掉，否则界面会一直显示"遛狗中"但车其实早就不听了。 */
        if (dogOn || raceOn) {
            if (dogOn)  { dogOn  = false; renderDogBtn();  }
            if (raceOn) { raceOn = false; renderRaceBtn(); }
            log("自动模式：手动接管，已退出");
        }

        activeCode      = code;
        activeName      = name;
        activeNeedsStop = needsStop;

        ui.removeCallbacks(repeatTask);
        fire(code, name, true, FRAMES_FIRST);
        if (needsStop) { ui.postDelayed(repeatTask, REPEAT_MS); }
    }

    private void release()
    {
        ui.removeCallbacks(repeatTask);
        boolean needStop = activeNeedsStop;

        activeCode      = 0;
        activeName      = "";
        activeNeedsStop = false;

        // 关键：松手马上补发停车帧，不用等车端的看门狗超时
        if (needStop) { fire(CODE_STOP, "松手即停", true, FRAMES_STOP); }
    }

    /** 动作 ID → 发给车的单字符命令（与固件 runMotion 一一对应） */
    private static String cmdOf(int code)
    {
        switch (code) {
            case CODE_FWD:      return "F";
            case CODE_BACK:     return "B";
            case CODE_STOP:     return "S";
            case CODE_SPINL:    return "L";
            case CODE_SPINR:    return "R";
            case CODE_DOG_ON:   return "G";
            case CODE_DOG_OFF:  return "g";
            case CODE_RACE_ON:  return "P";
            case CODE_RACE_OFF: return "p";
            default:            return null;
        }
    }

    /* 下面两个是旧红外接口，保留签名让调用点不用改；
     * 蓝牙是字节流连接，不像红外那样需要连发多帧抗丢包，各发一次即可。 */
    private void fire(final int code, final String name, final boolean record,
                      final int frames)
    {
        sendCmd(code, name, record);
    }

    private void fireSlow(final int code, final String name,
                          final int frames, final int gapMs)
    {
        sendCmd(code, name, true);
    }

    /**
     * 真正发送。失败意味着连接断了 —— 此时什么都不做，
     * 车端 450ms 看门狗会自己刹车，比在这里补发更可靠。
     */
    private void sendCmd(final int code, final String name, final boolean record)
    {
        final String cmd = cmdOf(code);
        if (cmd == null) { return; }

        if (bt == null || !bt.connected()) {
            log("✗ 蓝牙未连接，发不出去（点右上角连接）");
            renderPill();
            return;
        }
        if (tx == null) { log("✗ 发送线程没起来"); return; }

        tx.removeCallbacksAndMessages(null);     // 丢掉积压的旧指令，只发最新这条
        tx.post(new Runnable() {
            @Override public void run() {
                final boolean ok = bt.send(cmd);
                ui.post(new Runnable() {
                    @Override public void run() {
                        if (!ok) {
                            log("✗ 发送失败，连接已断开");
                            renderPill();
                        } else if (record) {
                            log("→ " + name + "  [" + cmd + "]");
                        }
                    }
                });
            }
        });

        if (record) { buzz(10); }
    }

    /* ================= 蓝牙连接 ================= */

    /** 顶部状态块：颜色和文案跟着连接状态走 */
    private void renderPill()
    {
        if (pill == null) { return; }

        if (bt == null || !bt.available()) {
            pill.setText("●  无蓝牙");
            pill.setTextColor(C_DANGER);
            pill.setBackground(round(0x1AFF5E6C, 0x55FF5E6C, dp(20)));
        } else if (!bt.enabled()) {
            pill.setText("●  蓝牙未开");
            pill.setTextColor(C_DANGER);
            pill.setBackground(round(0x1AFF5E6C, 0x55FF5E6C, dp(20)));
        } else if (bt.connected()) {
            pill.setText("●  已连接");
            pill.setTextColor(C_ACCENT);
            pill.setBackground(round(0x1A35E0A1, 0x5535E0A1, dp(20)));
        } else {
            pill.setText("●  未连接");
            pill.setTextColor(C_DIM);
            pill.setBackground(round(0x1A7C8B9B, 0x557C8B9B, dp(20)));
        }
    }

    /** 弹已配对设备列表让用户选 */
    private void pickBtDevice()
    {
        if (bt == null || !bt.available()) { log("✗ 这台设备没有经典蓝牙"); return; }
        if (!bt.enabled()) { log("✗ 蓝牙没打开，先去系统设置里打开"); return; }

        final java.util.List<android.bluetooth.BluetoothDevice> ds = bt.paired();
        if (ds.isEmpty()) {
            log("✗ 没有已配对设备：先去系统设置 → 蓝牙 里配对 HC-05（默认密码 1234 或 0000）");
            return;
        }

        String[] names = new String[ds.size()];
        for (int i = 0; i < ds.size(); i++) { names[i] = BtLink.nameOf(ds.get(i)); }

        new AlertDialog.Builder(this)
            .setTitle("选择蓝牙设备")
            .setItems(names, new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int which) {
                    doConnect(ds.get(which));
                }
            })
            .setNegativeButton("取消", null)
            .show();
    }

    /** 建连会阻塞几秒，必须丢到子线程 */
    private void doConnect(final android.bluetooth.BluetoothDevice dev)
    {
        if (tx == null) { return; }
        log("正在连接 " + BtLink.nameOf(dev) + " …");

        tx.post(new Runnable() {
            @Override public void run() {
                final String err = bt.connect(dev);
                ui.post(new Runnable() {
                    @Override public void run() {
                        if (err == null) {
                            log("✓ 已连接 " + bt.deviceLabel());
                            buzz(30);
                        } else {
                            log("✗ 连接失败 " + err);
                        }
                        renderPill();
                    }
                });
            }
        });
    }

    /**
     * 一帧 NEC：67 个 int，单位 µs —— 长度、顺序、末尾补位全部照抄旧版。
     *
     * 【位序是本 App 唯一容易踩死人的地方】
     * 车端用 Arduino-IRremote 的 decodeNEC 解码，它是
     *      data = (data << 1) | bit
     * 也就是「最先收到的位进最高位」。所以要让车端解出 0x00FF629D，
     * 发送端必须按 bit31 → bit0（高位先发）的顺序发。
     * 按 bit0 → bit31 发，车端会解成 0xB946FF00，KEY_TABLE 一条都对不上，
     * 表现就是「App 日志显示发送成功、车一动不动」。
     */
    private void buzz(int ms)
    {
        if (vib == null || !vib.hasVibrator()) { return; }
        try {
            if (Build.VERSION.SDK_INT >= 26) {
                vib.vibrate(VibrationEffect.createOneShot(ms,
                        VibrationEffect.DEFAULT_AMPLITUDE));
            } else {
                vib.vibrate(ms);
            }
        } catch (Throwable ignored) { }
    }

    /* ================= 日志 ================= */

    private void log(String s)
    {
        logs.add(0, tf.format(new Date()) + "   " + s);
        while (logs.size() > 40) { logs.remove(logs.size() - 1); }
        renderLog();
    }

    private void renderLog()
    {
        if (logView == null) { return; }
        if (logs.isEmpty()) {
            logView.setText("（还没有发送过）");
            return;
        }
        StringBuilder sb = new StringBuilder();
        int n = Math.min(LOG_SHOW, logs.size());
        for (int i = 0; i < n; i++) {
            if (i > 0) { sb.append('\n'); }
            sb.append(logs.get(i));
        }
        logView.setText(sb.toString());
    }

    /* ================= 工具 ================= */

    private int dp(float v)
    {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private GradientDrawable round(int fill, int stroke, int radius)
    {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.RECTANGLE);
        d.setColor(fill);
        d.setCornerRadius(radius);
        if (stroke != 0) { d.setStroke(dp(1), stroke); }
        return d;
    }

    private GradientDrawable gradient(int top, int bottom)
    {
        GradientDrawable d = new GradientDrawable(
                GradientDrawable.Orientation.TOP_BOTTOM, new int[]{ top, bottom });
        return d;
    }

    /* ================= 圆盘控件 ================= */

    static class WheelPad extends View
    {
        interface Listener {
            void onMove(int code, String name);
            void onUp();
            void onStop();
        }

        private static final int Z_NONE = 0, Z_UP = 1, Z_DOWN = 2,
                                 Z_LEFT = 3, Z_RIGHT = 4, Z_STOP = 5;

        private Listener listener;
        private final Paint fill   = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path  path   = new Path();
        private int zone = Z_NONE;
        private float density = 2f;

        WheelPad(Context c)
        {
            super(c);
            density = c.getResources().getDisplayMetrics().density;
            stroke.setStyle(Paint.Style.STROKE);
            stroke.setStrokeCap(Paint.Cap.ROUND);
            stroke.setStrokeJoin(Paint.Join.ROUND);
            setClickable(true);
        }

        void setListener(Listener l) { listener = l; }

        private float dp(float v) { return v * density; }

        @Override protected void onMeasure(int ws, int hs)
        {
            int w = MeasureSpec.getSize(ws);
            int h = MeasureSpec.getSize(hs);

            // 不铺满整宽：留出边距，比例才稳（真机屏更宽也不会顶满）
            int s = Math.min(w, h);
            s = Math.min(s, Math.round(w * 0.86f));
            s = Math.min(s, Math.round(dp(330)));

            setMeasuredDimension(s, s);
        }

        @Override protected void onDraw(Canvas c)
        {
            float cx = getWidth() / 2f;
            float cy = getHeight() / 2f;
            float R  = Math.min(getWidth(), getHeight()) / 2f - dp(3);

            // 盘面
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(0xFF101720);
            c.drawCircle(cx, cy, R, fill);

            // 按下方向的扇区高亮（用粗弧带模拟）
            if (zone >= Z_UP && zone <= Z_RIGHT) {
                int[] colors = { 0, 0x4035E0A1, 0x4035E0A1, 0x4035E0A1, 0x4035E0A1 };
                float[] degs = { 0, -90, 90, 180, 0 };
                stroke.setStyle(Paint.Style.STROKE);
                stroke.setStrokeWidth(R * 0.34f);
                stroke.setColor(colors[zone]);
                RectF arc = new RectF(cx - R * 0.66f, cy - R * 0.66f,
                                      cx + R * 0.66f, cy + R * 0.66f);
                c.drawArc(arc, degs[zone] - 40f, 80f, false, stroke);
            }

            // 外环
            stroke.setStyle(Paint.Style.STROKE);
            stroke.setStrokeWidth(dp(1.4f));
            stroke.setColor(0xFF2C3846);
            c.drawCircle(cx, cy, R, stroke);

            // 内环
            stroke.setStrokeWidth(dp(1f));
            stroke.setColor(0xFF1D2731);
            c.drawCircle(cx, cy, R * 0.40f, stroke);

            // 四条方向箭头
            drawChevron(c, cx, cy, R, 0, -1, zone == Z_UP);
            drawChevron(c, cx, cy, R, 0,  1, zone == Z_DOWN);
            drawChevron(c, cx, cy, R, -1, 0, zone == Z_LEFT);
            drawChevron(c, cx, cy, R,  1, 0, zone == Z_RIGHT);

            // 中心停车键
            float rs = R * 0.285f;
            fill.setColor(zone == Z_STOP ? 0x4DFF5E6C : 0x1FFF5E6C);
            c.drawCircle(cx, cy, rs, fill);
            stroke.setStrokeWidth(dp(1.6f));
            stroke.setColor(C_DANGER);
            c.drawCircle(cx, cy, rs, stroke);

            float s = rs * 0.40f;
            fill.setColor(C_DANGER);
            RectF box = new RectF(cx - s, cy - s, cx + s, cy + s);
            c.drawRoundRect(box, dp(2f), dp(2f), fill);
        }

        private void drawChevron(Canvas c, float cx, float cy, float R,
                                 float ux, float uy, boolean on)
        {
            float r  = R * 0.70f;
            float px = cx + ux * r;
            float py = cy + uy * r;

            float tx = -uy, ty = ux;
            float len = R * 0.098f;

            path.reset();
            path.moveTo(px - ux * len * 0.45f + tx * len,
                        py - uy * len * 0.45f + ty * len);
            path.lineTo(px + ux * len * 0.85f,
                        py + uy * len * 0.85f);
            path.lineTo(px - ux * len * 0.45f - tx * len,
                        py - uy * len * 0.45f - ty * len);

            stroke.setStyle(Paint.Style.STROKE);
            stroke.setStrokeWidth(on ? dp(5.6f) : dp(4.6f));
            stroke.setColor(on ? C_ACCENT : 0xFF8FA0B0);
            c.drawPath(path, stroke);
        }

        /* ---------- 触摸 ---------- */

        @Override public boolean onTouchEvent(MotionEvent e)
        {
            float cx = getWidth() / 2f;
            float cy = getHeight() / 2f;
            float R  = Math.min(getWidth(), getHeight()) / 2f - dp(3);
            float dx = e.getX() - cx;
            float dy = e.getY() - cy;

            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_MOVE:
                    if (e.getActionMasked() == MotionEvent.ACTION_MOVE && zone == Z_NONE) {
                        return true;
                    }
                    setZone(hit(dx, dy, R));
                    return true;

                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    setZone(Z_NONE);
                    if (listener != null) { listener.onUp(); }
                    return true;
            }
            return super.onTouchEvent(e);
        }

        private int hit(float dx, float dy, float R)
        {
            float d = (float) Math.sqrt(dx * dx + dy * dy);
            if (d < R * 0.33f) { return Z_STOP; }

            double deg = Math.toDegrees(Math.atan2(dy, dx));   // -180..180，向下为正
            if (deg > -45 && deg <= 45)   { return Z_RIGHT; }
            if (deg > 45 && deg <= 135)   { return Z_DOWN; }
            if (deg > 135 || deg <= -135) { return Z_LEFT; }
            return Z_UP;
        }

        private void setZone(int z)
        {
            if (z == zone) { return; }
            int prev = zone;
            zone = z;

            if (listener != null) {
                switch (z) {
                    case Z_UP:    listener.onMove(CODE_FWD,   "前进"); break;
                    case Z_DOWN:  listener.onMove(CODE_BACK,  "后退"); break;
                    // 左右两个方向改为发"原地旋转"键码。
                    // 原来发的是单边转弯码（CODE_LEFT / CODE_RIGHT），
                    // 但那个动作在实车上根本推不动（占空比低于起转门槛），
                    // 而原地旋转转得干净、半径小，用户实测效果更好，故取而代之。
                    case Z_LEFT:  listener.onMove(CODE_SPINL, "左转"); break;
                    case Z_RIGHT: listener.onMove(CODE_SPINR, "右转"); break;
                    case Z_STOP:  listener.onStop(); break;
                    case Z_NONE:
                        // 抬手由 onTouchEvent 统一处理，这里不重复触发
                        break;
                }
            }
            invalidate();
        }
    }
}
