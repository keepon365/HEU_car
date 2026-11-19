# HEU_car 项目长期约定

## 发布与推送（用户明确要求，2026-11-16）

- **只有 APK 版本号真正变化时才 push。** 固件（DirSpeedTest.ino）改动只编译 + 烧录，
  不推远程 —— 固件根本不经过 GitHub，推上去对手机端毫无作用。
- **推送必须走近 127.0.0.1:7890 代理，直连必被 reset**（2026-11-16 实测纠正，
  之前记的"代理失效要绕过"是错的，按那条做导致 push 连续失败）：
  `git -c http.proxy=http://127.0.0.1:7890 -c https.proxy=http://127.0.0.1:7890 push`
  自检：`curl -m 3 -x http://127.0.0.1:7890 -o /dev/null -w %{http_code} https://github.com` 应为 200。
- 允许本地 commit 累积，下次真要发 APK 时一次性推上去。
- `app/build/` 是编译中间产物，变动噪音大，提交时一律排除（用显式路径 add，不用 `git add -A`）。
- **提交说明一律用中文**（用户明确要求，2026-11-16）。commit message、GitHub Release 说明、
  更新日志 note 都用中文写，不要英文。写完可用 `git commit --amend -F <文件>` 改，文件存 UTF-8。

## APK 版本号（2026-11-16 踩坑）

- **aapt2 的 `--version-code` / `--version-name` 只在清单缺少该属性时才注入。**
  `AndroidManifest.xml` 里写死 `android:versionCode` 时命令行参数被完全忽略，
  于是打出来的包还是旧号、`version.json` 却是新号 → 手机反复提示更新、装完还是旧版。
  现在的做法：`build_apk.sh` 链接前生成临时清单替换版本号，签名后用
  `aapt2 dump badging` 读回校验，不符直接失败且不生成 version.json。别再改回去。
- 发版必查三处一致：`aapt2 dump badging app.apk` 的 versionCode、
  `dist/version.json` 的 versionCode、手机已装版本号（必须严格大于）。
- 版本号撞过车（2.4 实际是 5/2.3），所以直接跳到 7/2.5，不要复用 6。

## 编译/烧录命令（已验证可用）

固件：
```
arduino-cli compile --fqbn arduino:avr:uno --build-path <win路径> <win路径>
arduino-cli upload -p COM9 --fqbn arduino:avr:uno --build-path ... ...
```
可执行文件在 `C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`，
路径参数必须 `cygpath -w` 转换。

APK：`VCODE=N VNAME=x.y NOTE="..." bash app/build_apk.sh`
（走 aapt2 → javac → d8 → zipalign → apksigner，keystore 在 app/zyar.keystore，别名 zyar）

## 关键参数不可随意改

- `TRIM_LEFT=13`：左轮起转门槛 73 与右轮 60 的实测差值。**绝对值补偿，不能缩放。**
- `GAIN_R=225`：右轮摩擦小、跑得快，压制它解决左偏。用户实测 225 是当前最优。
- 这两个是**两种不同补偿**：TRIM 管起步同步（加法），GAIN 管巡航直线（乘法）。
  `buildDuty()` 里 GAIN 必须按 cmd 淡入，否则两者在低速段叠加 → 左轮先动。
  注释 ③ 早已写明该意图，实现曾漏掉，2026-11-16 已修，别改回去。
- 用户通过 `烧录.bat` 改的宏会写进固件，但 **EEPROM 里的运行时参数优先于 #define**。
  读回 `[CAL]` 行才是真正生效值。
