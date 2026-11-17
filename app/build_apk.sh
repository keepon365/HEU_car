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

# 版本号可由外部注入（发布脚本用），不传就用这里的默认值
# ★ 每次发版把这两个默认值一起改掉，免得手动构建时打出旧号
VCODE="${VCODE:-7}"
VNAME="${VNAME:-2.5}"
NOTE="${NOTE:-}"
DIST="$(cd "$APP/.." && pwd)/dist"

rm -rf "$B"
mkdir -p "$B/classes" "$B/dex" "$B/gen"

echo "[1/6] 编译资源 (aapt2 compile)"
"$BT/aapt2.exe" compile --dir "$APP/res" -o "$B/res.zip"

echo "[2/6] 链接资源与清单 (aapt2 link)  versionCode=$VCODE  versionName=$VNAME"

# ★ aapt2 的 --version-code / --version-name 只在清单「缺少该属性」时才注入。
#   清单里写死 android:versionCode 的话，命令行参数会被完全忽略，打出来的包
#   还是旧版本号，而 version.json 却按新号写 —— 手机就永远提示同一个版本、
#   装完还是旧的。2026-11-16 踩过这个坑（发了 2.4，APK 里实际是 5/2.3）。
#   所以这里生成一份临时清单，把两个属性替换成实际值，别再依赖 aapt2 的注入。
MANIFEST_TMP="$B/AndroidManifest.xml"
python - "$APP/AndroidManifest.xml" "$MANIFEST_TMP" "$VCODE" "$VNAME" <<'PY'
import re, sys
src, dst, code, name = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
with open(src, encoding="utf-8") as f:
    t = f.read()
if re.search(r'android:versionCode\s*=', t):
    t = re.sub(r'android:versionCode\s*=\s*"\d+"', 'android:versionCode="%s"' % code, t, count=1)
else:
    t = t.replace('<manifest ', '<manifest android:versionCode="%s" ' % code, 1)
if re.search(r'android:versionName\s*=', t):
    t = re.sub(r'android:versionName\s*=\s*"[^"]*"', 'android:versionName="%s"' % name, t, count=1)
else:
    t = t.replace('<manifest ', '<manifest android:versionName="%s" ' % name, 1)
with open(dst, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("   临时清单已写入 versionCode=%s versionName=%s" % (code, name))
PY

"$BT/aapt2.exe" link \
  -o "$B/base.apk" \
  -I "$AJAR" \
  --manifest "$MANIFEST_TMP" \
  -R "$B/res.zip" \
  --java "$B/gen" \
  --min-sdk-version 21 \
  --target-sdk-version 34 \
  --version-code "$VCODE" \
  --version-name "$VNAME" \
  --auto-add-overlay

echo "[3/6] 编译 Java (javac)"
find "$APP/src" -name "*.java" | while read -r f; do cygpath -w "$f"; done > "$B/sources.txt"

# ★ javac 的退出码必须检查，不能只看某个 class 有没有生成。
#   2026-11-17 踩过：Updater.java 有个变量重名错误，javac 失败但已部分产出，
#   MainActivity.class 照样存在 → 构建继续 → 打出的 dex 缺 Updater.class
#   → 手机装上直接闪退，而且 version.json 还报了新版本号。
set +e
"$JDK/javac.exe" -J-Duser.language=en -J-Duser.country=US -encoding UTF-8 \
  -classpath "$AJAR" -d "$B/classes" @"$B/sources.txt" 2>&1 | grep -v "^Note:"
JC=${PIPESTATUS[0]}
set -e
if [ "$JC" -ne 0 ]; then
  echo "✗ javac 编译失败（exit $JC），构建中止，不生成 APK/version.json"
  exit 1
fi

# 再逐个数一遍：每个 .java 都必须有同名 .class，缺一个就说明编译不完整
# 注意 sources.txt 里是 cygpath -w 出来的**反斜杠**路径，先统一成正斜杠再截取
MISSING=0
while read -r f; do
  cls="$(echo "$f" | sed 's|\\|/|g; s|.*/src/||; s|\.java$|.class|')"
  if [ ! -f "$B/classes/$cls" ]; then echo "✗ 缺少 $cls"; MISSING=1; fi
done < "$B/sources.txt"
if [ "$MISSING" -ne 0 ]; then
  echo "✗ 有源文件没编译成 class，构建中止"
  exit 1
fi

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

# ★ 硬校验：装进手机的那个号，必须和我们要发的号一致。不一致就直接失败，
#   绝不生成 version.json —— 否则又会出现"提示有新版本、装完还是旧的"死循环。
BADGE="$("$BT/aapt2.exe" dump badging "$OUT" 2>/dev/null | head -1)"
echo "$BADGE"
case "$BADGE" in
  *"versionCode='$VCODE'"*"versionName='$VNAME'"*)
    echo "   [OK] 清单版本号与发布号一致" ;;
  *)
    echo "!! APK 清单里的版本号与 VCODE=$VCODE / VNAME=$VNAME 不一致，已中止发布"
    exit 1 ;;
esac

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

# 中文提交说明：在 cmd 批处理里直接写中文会按 GBK 存进去，推上去是乱码。
# 这里用 python 写成 UTF-8，发布.bat 用 git commit -F 引用即可。
MSG="$(cd "$APP/.." && pwd)/.git/RELEASE_MSG.txt"
python - "$MSG" "$VCODE" "$VNAME" "$NOTE" <<'PY'
import sys
path, code, name, note = sys.argv[1:5]
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write("发布 v%s（versionCode %s）\n" % (name, code))
    if note.strip():
        f.write("\n" + note.strip() + "\n")
print("   提交说明 -> .git/RELEASE_MSG.txt")
PY

echo "把 dist/ 里的 app.apk 和 version.json 两个文件，作为 Release 的 asset 上传即可。"
