package com.zyar20a.irremote2;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * 自更新：查清单 → 下载 → 交给系统安装器。
 *
 * 分工刻意划清：
 *   · 网络与文件 IO 全在后台线程
 *   · 所有回调都 post 回主线程，调用方不用自己切线程
 *   · 安装这一下必须由系统弹框，App 无静默安装权限 —— 这是 Android 的限制，
 *     不是没实现。能自动的只有"发现 + 下载"。
 */
public class Updater {

    public interface Cb {
        /** 发现有新版（code 一定大于本地） */
        void onFound(int code, String name, String note, String base);
        void onProgress(int percent);
        /** 下载完且校验通过 */
        void onDone();
        void onFail(String why);
    }

    private static final Handler UI = new Handler(Looper.getMainLooper());

    private static void post(Runnable r) { UI.post(r); }

    /** 本机已装的 versionCode */
    public static int localCode(Context c) {
        try {
            PackageInfo p = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            return p.versionCode;
        } catch (Throwable t) {
            return 0;
        }
    }

    public static String localName(Context c) {
        try {
            PackageInfo p = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            return p.versionName;
        } catch (Throwable t) {
            return "?";
        }
    }

    /* ================= 检查更新 ================= */

    /**
     * 逐个源请求 version.json，**第一个报出比本地新的版本就采用并停止**。
     *
     * 为什么不等所有源返回后取最大值：
     *   6 个源串行最坏要 12 秒，"取最大"换来的收益几乎为零
     *   （镜像缓存最多晚一个版本，下次启动还会再提示）。
     * 任何一个源失败都只是跳过 —— 没网、镜像挂了都不该影响遥控。
     */
    public static void check(final Context c, final Cb cb) {
        new Thread(new Runnable() {
            @Override public void run() {
                final int mine = localCode(c);

                for (String base : UpdateConfig.SOURCES) {
                    String js = getText(base + UpdateConfig.FILE_LIST);
                    if (js == null) { continue; }
                    try {
                        JSONObject o = new JSONObject(js);
                        int code = o.optInt("versionCode", 0);
                        if (code > mine) {
                            final int fc = code;
                            final String fn = o.optString("versionName", "");
                            final String fnote = o.optString("note", "");
                            final String fb = base;
                            post(new Runnable() {
                                @Override public void run() {
                                    cb.onFound(fc, fn, fnote, fb);
                                }
                            });
                            return;                 // 命中了，不必再试后面的源
                        }
                    } catch (Throwable ignored) {
                        // 清单格式不对，当这个源不存在，继续下一个
                    }
                }
            }
        }).start();
    }

    /** 读一个 **APK 文件**（不是已安装的包）里面的 versionCode。
     *  -1 = 读不出来（文件损坏 / 根本不是 APK）。
     * 这是判断"下载到的到底是不是我要的那个版本"的唯一可靠办法 ——
     * 只看文件大小和 PK 头是不够的：镜像缓存旧包时两者都正常。 */
    public static int apkVersionCode(Context c, File f) {
        try {
            PackageInfo p = c.getPackageManager()
                    .getPackageArchiveInfo(f.getAbsolutePath(), 0);
            return (p == null) ? -1 : p.versionCode;
        } catch (Throwable t) {
            return -1;
        }
    }

    /** 本地已下好的安装包版本号（没有文件返回 -1） */
    public static int cachedApkCode(Context c) {
        File f = ApkProvider.apkFile(c);
        if (!f.exists() || f.length() < UpdateConfig.MIN_APK_BYTES) { return -1; }
        return apkVersionCode(c, f);
    }

    /* ================= 下载 ================= */

    public static void download(final Context c, final String base,
                                final int expectCode, final Cb cb) {
        new Thread(new Runnable() {
            @Override public void run() {
                final File out = ApkProvider.apkFile(c);
                HttpURLConnection con = null;
                try {
                    if (out.exists()) { out.delete(); }

                    /* ★ 带上 ?v=<目标版本>：
                     *   raw.githubusercontent.com 和镜像都有 CDN 缓存（数分钟），
                     *   刚推送完就去下，很可能拿到上一个版本的包。
                     *   换个 URL 就是换一个缓存键，能绕开旧缓存。 */
                    URL u = new URL(base + UpdateConfig.FILE_APK + "?v=" + expectCode);
                    con = (HttpURLConnection) u.openConnection();
                    con.setConnectTimeout(UpdateConfig.TIMEOUT_MS);
                    con.setReadTimeout(15000);
                    con.setInstanceFollowRedirects(true);
                    con.setRequestProperty("User-Agent", "ZYAR20A-Updater");
                    con.connect();

                    final int len = con.getContentLength();
                    InputStream in = con.getInputStream();
                    FileOutputStream fo = new FileOutputStream(out);

                    byte[] buf = new byte[8192];
                    long got = 0;
                    int r, lastPct = -1;
                    while ((r = in.read(buf)) > 0) {
                        fo.write(buf, 0, r);
                        got += r;
                        if (len > 0) {
                            int pct = (int) (got * 100 / len);
                            if (pct != lastPct) {
                                lastPct = pct;
                                final int p = pct;
                                post(new Runnable() {
                                    @Override public void run() { cb.onProgress(p); }
                                });
                            }
                        }
                    }
                    fo.close();
                    in.close();

                    // ---- 校验：长度 + ZIP 魔术字 ----
                    if (out.length() < UpdateConfig.MIN_APK_BYTES) {
                        throw new Exception("文件太小（" + out.length() + " 字节），可能是镜像返回了错误页");
                    }
                    if (!isZip(out)) {
                        throw new Exception("下载内容不是 APK（缺少 PK 头），镜像可能被劫持或返回了网页");
                    }

                    /* ★ 最关键的一道：包里的 versionCode 必须等于清单里说的那个。
                     *   少了这道，镜像缓存旧包时会被当成"下载成功"，
                     *   然后系统安装器拒绝 —— 用户看到的就是
                     *   "点了更新却说已安装更高版本"（2026-11-17 实际踩到）。 */
                    if (expectCode > 0) {
                        final int got = apkVersionCode(c, out);
                        if (got != expectCode) {
                            out.delete();
                            throw new Exception("下载到的是 v" + got + "，不是 v" + expectCode
                                    + "（镜像缓存还没刷新，稍等几分钟再试）");
                        }
                    }

                    post(new Runnable() {
                        @Override public void run() { cb.onDone(); }
                    });

                } catch (Throwable t) {
                    out.delete();
                    final String why = t.getClass().getSimpleName()
                            + (t.getMessage() == null ? "" : ": " + t.getMessage());
                    post(new Runnable() {
                        @Override public void run() { cb.onFail(why); }
                    });
                } finally {
                    if (con != null) { con.disconnect(); }
                }
            }
        }).start();
    }

    /* ================= 安装 ================= */

    /**
     * @return true = 已拉起安装界面；false = 缺权限（已跳设置页）或没有文件
     *
     * 注意"未知来源"权限：Android 8 起每个 App 单独授权，系统不会替你点。
     * 所以没权限时跳到设置页，用户允许后再点一次更新按钮即可 ——
     * 此时 APK 已经在本地，不会再走一遍网络。
     */
    public static boolean install(Activity a) {
        File f = ApkProvider.apkFile(a);
        if (!f.exists() || f.length() < UpdateConfig.MIN_APK_BYTES) { return false; }

        /* 装之前再确认一次：包里的 versionCode 必须比已装的新。
         * 否则装到一半被系统拒，用户只会看到一句"已安装更高版本"，无从判断。 */
        final int inApk = apkVersionCode(a, f);
        final int mine  = localCode(a);
        if (inApk > 0 && mine > 0 && inApk <= mine) {
            f.delete();                    // 旧包直接丢掉，下次会重新下
            return false;
        }

        if (Build.VERSION.SDK_INT >= 26) {
            boolean ok;
            try {
                ok = a.getPackageManager().canRequestPackageInstalls();
            } catch (Throwable t) {
                ok = true;                 // 系统太旧或接口异常，直接试
            }
            if (!ok) {
                try {
                    Intent s = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                            Uri.parse("package:" + a.getPackageName()));
                    a.startActivity(s);
                } catch (Throwable ignored) {
                    // 个别 ROM 没有这个设置页，只能让用户自己去"安装未知应用"里开
                }
                return false;
            }
        }

        try {
            Intent i = new Intent(Intent.ACTION_VIEW);
            i.setDataAndType(ApkProvider.uriFor(), "application/vnd.android.package-archive");
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
            a.startActivity(i);
            return true;
        } catch (Throwable t) {
            return false;
        }
    }

    /** 本地是否已有下载好的包（用于"允许权限后再点一次"时跳过下载） */
    public static boolean apkReady(Context c) {
        File f = ApkProvider.apkFile(c);
        return f.exists() && f.length() >= UpdateConfig.MIN_APK_BYTES && isZip(f);
    }

    /* ================= 工具 ================= */

    private static boolean isZip(File f) {
        FileInputStream in = null;
        try {
            in = new FileInputStream(f);
            int a = in.read(), b = in.read();
            return a == 'P' && b == 'K';        // 0x50 0x4B
        } catch (Throwable t) {
            return false;
        } finally {
            try { if (in != null) { in.close(); } } catch (Throwable ignored) { }
        }
    }

    /** GET 一个文本资源，失败返回 null。两侧超时都设死，绝不阻塞启动。 */
    private static String getText(String url) {
        HttpURLConnection con = null;
        try {
            con = (HttpURLConnection) new URL(url).openConnection();
            con.setConnectTimeout(UpdateConfig.TIMEOUT_MS);
            con.setReadTimeout(UpdateConfig.TIMEOUT_MS);
            con.setInstanceFollowRedirects(true);
            con.setRequestProperty("User-Agent", "ZYAR20A-Updater");
            if (con.getResponseCode() != 200) { return null; }

            InputStream in = con.getInputStream();
            java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[2048];
            int r, total = 0;
            while ((r = in.read(buf)) > 0) {
                total += r;
                if (total > 65536) { break; }    // 清单不可能这么大，防恶意大响应
                bo.write(buf, 0, r);
            }
            in.close();
            return new String(bo.toByteArray(), "UTF-8");
        } catch (Throwable t) {
            return null;
        } finally {
            if (con != null) { con.disconnect(); }
        }
    }
}
