#!/bin/bash
# 从零构建 ZYAR20A 遥控 APK（不依赖 Gradle）
set -e

SDK="/c/Users/zwm/AppData/Local/Android/Sdk"
BT="$SDK/build-tools/35.0.0"
AJAR="$SDK/platforms/android-35/android.jar"
JDK="/c/Program Files/Java/jdk-17/bin"

APP="$(cd "$(dirname "$0")" && pwd)"
B="$APP/build"
OUT="/c/Users/zwm/Desktop/HEU_car/ZYAR20A遥控.apk"

# 版本号可由外部注入（发布脚本用），不传就用 Manifest 里写的
VCODE="${VCODE:-4}"
VNAME="${VNAME:-2.2}"
NOTE="${NOTE:-}"
DIST="$(cd "$APP/.." && pwd)/dist"

rm -rf "$B"
mkdir -p "$B/classes" "$B/dex" "$B/gen"

echo "[1/6] 编译资源 (aapt2 compile)"
"$BT/aapt2.exe" compile --dir "$APP/res" -o "$B/res.zip"

echo "[2/6] 链接资源与清单 (aapt2 link)  versionCode=$VCODE  versionName=$VNAME"
"$BT/aapt2.exe" link \
  -o "$B/base.apk" \
  -I "$AJAR" \
  --manifest "$APP/AndroidManifest.xml" \
  -R "$B/res.zip" \
  --java "$B/gen" \
  --min-sdk-version 21 \
  --target-sdk-version 34 \
  --version-code "$VCODE" \
  --version-name "$VNAME" \
  --auto-add-overlay

echo "[3/6] 编译 Java (javac)"
find "$APP/src" -name "*.java" | while read -r f; do cygpath -w "$f"; done > "$B/sources.txt"
set +e
"$JDK/javac.exe" -J-Duser.language=en -J-Duser.country=US -encoding UTF-8 \
  -classpath "$AJAR" -d "$B/classes" @"$B/sources.txt" 2>&1 | grep -v "^Note:"
set -e
test -f "$B/classes/com/zyar20a/irremote2/MainActivity.class" || { echo "编译失败"; exit 1; }

echo "[4/6] 转 dex (d8)"
find "$B/classes" -name "*.class" | while read -r f; do cygpath -w "$f"; done > "$B/classes.txt"
"$JDK/java.exe" -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 \
  --lib "$AJAR" --min-api 21 --output "$B/dex" \
  $(cat "$B/classes.txt")

echo "[5/6] 打包 + 对齐"
python - "$B/base.apk" "$B/dex/classes.dex" "$B/unsigned.apk" <<'PY'
import sys, zipfile, shutil
base, dex, out = sys.argv[1], sys.argv[2], sys.argv[3]
shutil.copyfile(base, out)
z = zipfile.ZipFile(out, 'a', zipfile.ZIP_DEFLATED)
z.write(dex, 'classes.dex')
z.close()
print("   classes.dex 已加入")
PY
"$BT/zipalign.exe" -f -p 4 "$B/unsigned.apk" "$B/aligned.apk"

echo "[6/6] 签名 (apksigner)"
KS="$APP/zyar.keystore"
if [ ! -f "$KS" ]; then
  "$JDK/keytool.exe" -genkeypair -keystore "$KS" -alias zyar \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -storepass zyar1234 -keypass zyar1234 \
    -dname "CN=ZYAR20A, O=HEU, C=CN" >/dev/null 2>&1
fi
"$BT/apksigner.bat" sign \
  --ks "$KS" --ks-key-alias zyar \
  --ks-pass pass:zyar1234 --key-pass pass:zyar1234 \
  --v1-signing-enabled true --v2-signing-enabled true --v4-signing-enabled false \
  --out "$OUT" "$B/aligned.apk"

echo
echo "=== 校验 ==="
"$BT/apksigner.bat" verify --print-certs "$OUT" 2>&1 | head -6
ls -l "$OUT"

# ---- 生成 GitHub Release 需要的两个 asset ----
# 文件名必须是纯 ASCII：GitHub 的 asset URL 用文件名拼路径，中文要百分号编码，
# 手机端 URL 构造容易出错，所以一律用 app.apk / version.json。
mkdir -p "$DIST"
cp "$OUT" "$DIST/app.apk"

python - "$DIST/version.json" "$VCODE" "$VNAME" "$NOTE" "$DIST/app.apk" <<'PY'
import json, sys, hashlib
path, code, name, note, apk = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
data = {
    "versionCode": code,
    "versionName": name,
    "note": note,
    "size": __import__("os").path.getsize(apk),
    "md5": hashlib.md5(open(apk, "rb").read()).hexdigest(),
}
# 必须无 BOM：Android 的 org.json 遇到 \ufeff 会直接抛异常
with open(path, "w", encoding="utf-8") as f:
    json.dump(data, f, ensure_ascii=False, indent=2)
print("   version.json ->", json.dumps(data, ensure_ascii=False))
PY

echo
echo "=== 发布产物 ==="
ls -l "$DIST"
echo "把 dist/ 里的 app.apk 和 version.json 两个文件，作为 Release 的 asset 上传即可。"
