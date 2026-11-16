package com.zyar20a.irremote2;

/**
 * ★ 更新源配置 —— 换地址只改这个文件。
 *
 * 仓库：https://github.com/keepon365/HEU_car（必须 public）
 *
 * 两组源，任意一组能用就行：
 *
 *  ① raw 直取 dist/ 目录（推荐）
 *     发布 = 把 dist/app.apk + dist/version.json 提交并 push，就完事，
 *     不用去网页开 Release。镜像对 raw 支持最好，实测最稳。
 *     直链：raw.githubusercontent.com/keepon365/HEU_car/main/dist/app.apk
 *
 *  ② GitHub Release
 *     releases/latest/download/ 固定指向最新正式版，发新版后链接不变。
 *     注意不能勾 pre-release，否则 latest 不会指过去。
 *
 * 每个源是一个"目录"，里面放两个文件，名字必须一字不差、纯 ASCII：
 *     version.json   版本清单
 *     app.apk        安装包（中文文件名在 URL 里要编码，容易翻车，一律不用）
 *
 * 数组顺序 = 尝试顺序。哪个源报的 versionCode 比本地大就立刻采用并停止，
 * 所以正常只发一次请求；全挂就安静跳过，绝不影响遥控。
 *
 * version.json 内容：
 * {
 *   "versionCode": 4,
 *   "versionName": "2.2",
 *   "note": "修正速度缩放溢出"
 * }
 */
public final class UpdateConfig {

    private static final String OWNER  = "keepon365";
    private static final String REPO   = "HEU_car";
    private static final String BRANCH = "main";

    /** ① raw 直取仓库里的 dist/ 目录 */
    private static final String RAW =
            "https://raw.githubusercontent.com/" + OWNER + "/" + REPO + "/" + BRANCH + "/dist/";

    /** ② Release 固定链接，永远指向最新正式版 */
    private static final String REL =
            "https://github.com/" + OWNER + "/" + REPO + "/releases/latest/download/";

    public static final String[] SOURCES = {
            "https://ghfast.top/" + RAW,        // 镜像 ①-raw（实测最稳，优先）
            "https://gh-proxy.com/" + RAW,      // 镜像 ②-raw
            "https://ghfast.top/" + REL,        // 镜像 ③-release
            "https://gh-proxy.com/" + REL,      // 镜像 ④-release
            RAW,                                 // 直连 raw（镜像全挂时兜底）
            REL,                                 // 直连 release
    };

    public static final String FILE_LIST = "version.json";
    public static final String FILE_APK  = "app.apk";

    /**
     * 单个源连不上就放弃。串行最坏 12 秒，但全在后台线程；
     * 正常情况第一个源就命中，不会真等满。
     */
    public static final int TIMEOUT_MS = 2000;

    /** 低于这个字节数肯定不是完整 APK */
    public static final int MIN_APK_BYTES = 8192;

    private UpdateConfig() { }
}
