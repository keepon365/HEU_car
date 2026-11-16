package com.zyar20a.irremote2;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * 极简文件提供器 —— 唯一用途：把下载好的 APK 以 content:// 交给系统安装器。
 *
 * 为什么不直接用 androidx.core.content.FileProvider：
 *   本工程用 javac + android.jar 手工编译，没有 androidx 依赖，
 *   引不进来。所以自己继承 ContentProvider 实现 openFile 即可。
 *
 * 为什么不直接给 file:// URI：
 *   Android 7.0(N) 起 StrictMode 禁止把 file:// 暴露给别的应用，
 *   会抛 FileUriExposedException。必须 content:// + 临时授权。
 *
 * targetSdk 34 依然遵守这条规则。
 */
public class ApkProvider extends ContentProvider {

    public static final String AUTHORITY = "com.zyar20a.irremote2.apk";
    public static final String APK_NAME  = "update.apk";

    /** 系统安装器拿到的地址 */
    public static Uri uriFor() {
        return Uri.parse("content://" + AUTHORITY + "/" + APK_NAME);
    }

    /** APK 落盘位置：应用私有外部目录，卸载即清，不需要任何存储权限 */
    public static File apkFile(android.content.Context c) {
        return new File(c.getExternalFilesDir(null), APK_NAME);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        // 不管 uri 里写什么，只服务这一个文件，避免变成任意文件读取通道
        File f = apkFile(getContext());
        if (!f.exists() || f.length() <= 0) {
            throw new FileNotFoundException("apk not ready: " + f.getAbsolutePath());
        }
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        return "application/vnd.android.package-archive";
    }

    /* 下面四个本场景用不到，返回空实现即可 */

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
