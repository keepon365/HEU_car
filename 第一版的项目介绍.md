# ZYAR20A 智能小车 · 红外遥控实验（重制版）

把光盘 `3.ZYAR20A程序\9、智能小车红外遥控实验\Remote_contorl` 里那份 2017 年的
Arduino 程序整理成了：**能编译、能看懂、能改** 的几份代码 + 七篇中文文档。

原版程序 213 行，没有一句"为什么"。这个仓库就是来补上那个"为什么"的。

---

## 30 秒上手

```powershell
# 1. 编译（不需要装 Arduino IDE，用你机器上 IDE 自带的 avr-gcc）
cd D:\DSH\PicScan\Arduino1
pwsh -File .\tools\build.ps1 -SketchDir .\02_annotated\RemoteControl_Annotated

# 2. 生成的可烧录固件在
#    build\RemoteControl_Annotated\RemoteControl_Annotated.hex
```

想用 Arduino IDE：把 `libraries\IRremote` 复制到 `我的文档\Arduino\libraries\`，
打开对应文件夹里的 `.ino`，板型选 **Arduino Uno**，点上传。
详细步骤和坑见 [`docs\00_先读我.md`](docs/00_先读我.md)。

---

## 几个程序，选一个用

| 目录 | 是什么 | 什么时候用 |
|---|---|---|
| `01_original\Remote_contorl\` | **原版，一字未改** | 想对照"改了什么"，或者就想要当年原样的效果 |
| `02_annotated\RemoteControl_Annotated\` | **详解注释版**，行为与原版完全一致 | 学习。每一行都有注释，坑都写在代码里 |
| `03_optimized\RemoteControl_Optimized\` | **我的优化版**，结构重写 + 新增可选功能 | 想继续开发，或者想学"同一件事还能怎么写" |
| `04_button_test\ButtonMotorTest\` | **按键+电机测试**：按住板上的 S1（接 A2）前进，松开就停 | 红外还没通时，先单独验证按钮和电机 |
| `04_pwm\RemoteControl_PWM\` | **带占空比调速**：遥控器 CH+/CH- 或数字键 1~9 调速度 | 想让车慢下来 / 想调占空比，见 `docs\06` |
| `05_dir_speed\DirSpeedTest\` | **测试程序**：基准速度 120、倒车 96，带起步防打滑曲线 + 死区补偿(TRIM) + 增益补偿(GAIN)，内置 SWAP_EN 处理 EN 交叉 | 验证 PWM、调起步同步与巡航直线 |
| `06_side_calib\SideCalib\` | **标定工具**（v2 分段变速，细区每格 400ms）：分别慢慢加左右轮的占空比 | 左右轮起步不同步时先跑它，读阈值填 TRIM |
| `07_side_diag\SideDiag\` | **判别工具**：四段循环，串口写明"现在应该谁动"，用来确认哪根线管哪个轮子 | 补偿加到很大也没效果时跑它（多半是 EN 接反） |

都实测编译通过（avr-gcc 7.3.0 + Arduino AVR core 1.8.8，目标 ATmega328P @16MHz）：

| 程序 | Flash | SRAM |
|---|---|---|
| `01_original` | 7874 B / 24.0% | 449 B / 21.9% |
| `02_annotated` | 7618 B / 23.2% | 410 B / 20.0% |
| `03_optimized` | **6836 B / 20.9%** | **388 B / 18.9%** |
| `03_optimized`（调试全开） | 9288 B / 28.3% | 567 B / 27.7% |
| `04_pwm` | 8036 B / 24.5% | 468 B / 22.9% |
| `05_dir_speed\DirSpeedTest`（串口开） | 10726 B / 32.7% | 585 B / 28.6% |
| `06_side_calib\SideCalib` | 4780 B / 14.6% | 206 B / 10.1% |
| `07_side_diag\SideDiag` | 4384 B / 13.4% | 188 B / 9.2% |

> 优化版比原版**小了约 1KB** —— 因为把七段几乎相同的电机控制代码合并成了一个函数。
>
> `04_pwm` 和 `DirSpeedTest` 都带**起步防打滑曲线**（起步占空比 → 保持 → 按斜率爬），
> 曲线可以用 `node tools\ramp_sim.js` 在 PC 上跑一遍验证。

---

## 手机遥控（`04_phone_remote`）

手机要是自带红外发射管（`ConsumerIrManager.hasIrEmitter()` 返回 true），原装遥控器就可以收起来了：
`04_phone_remote` 是一个 Android 工程（Kotlin，**零第三方依赖**），界面 5 个键
（前进 / 后退 / 左转 / 右转 / 停车），发出去的就是本仓库里那七个 NEC 键码，
**车端一行都不用改**。

手机端的编码器已经和车端的 `decodeNEC()` 做过端到端仿真：

```powershell
node 04_phone_remote\tools\nec_check.js
```

7 个键在 ±15% 时序抖动下 14000/14000 全部解对；故意漏发结尾 MARK 的负向测试也如实失败。
安装与排错见 [`04_phone_remote\README.md`](04_phone_remote/README.md)；
交接说明（已验证 / 未验证 / 踩过的坑）见 [`04_phone_remote\HANDOFF.md`](04_phone_remote/HANDOFF.md)。

---

## 优化版改了什么

1. **七段重复代码 → 一个 `setWheels(左轮, 右轮)`**：方向用 `+1 / 0 / -1` 表达，
   "左右电机镜像安装"这件事只在一个参数里出现一次，再也不会因为抄错一个电平让车倒着跑。
2. **一串 `if` → 常量按键表 `KEY_TABLE[]`**：加按键 = 加一行，表里顺便存了按键名字。
3. **正确处理 NEC 重发帧 `0xFFFFFFFF`**：原版当它不存在，优化版用它实现"松手自动停"。
4. **LED 改成 30ms 非阻塞脉冲**：原版是"翻转"，亮灭状态随机。
5. **新增 `AUTO_STOP_MS`**：设成 500 就是"按住走、松手停"，默认 0 = 保持原版锁存行为。
6. **`STOP_ON_UNKNOWN` 变成显式开关**：原版"噪声误停"的行为被摊开说明，不再神秘。
7. **串口调试变成工具**：`DEBUG_SERIAL=1` 后按任意键打印键码，遇到陌生键直接告诉你"抄这行进表"。
8. **上电先刹车**：避免上电瞬间引脚高阻导致轮子抖一下。

**关于调速**：原来以为做不到（那时没看原理图）。核对原理图 + 光盘里所有工程对引脚的使用之后定下来了：
**ENA→D5、ENB→D6**（D5 上那颗灯拔掉、两个红外避障模块拔掉，D5/D6 本来就是它们的信号脚 ZC/ZRC），
方向脚 D8~D11 一根不动，两路 PWM 同属 Timer0、同频 977Hz，红外库一个字不用改。
注意 **D3/D4 不能碰**——它们是循迹模块的网络（板子上"两个 D4"就是同一个网络引出两个座子），
插着循迹模块时正被驱动。代码 `#define PIN_PLAN_A` 默认就是这套接线。
唯一必做的前提：**拔掉 L298N 模块上 ENA/ENB 的跳线帽**。
细节与动手顺序见 [`docs\06_加装PWM调速.md`](docs/06_加装PWM调速.md)。

---

## 文档

| 文件 | 内容 | 建议什么时候看 |
|---|---|---|
| [`docs\00_先读我.md`](docs/00_先读我.md) | 目录说明、编译烧录步骤、上电前检查 | **先看这篇** |
| [`docs\01_Arduino入门_写给会STM32的你.md`](docs/01_Arduino入门_写给会STM32的你.md) | STM32 ↔ Arduino 概念对照、AVR 资源、定时器分配、库机制 | 进入代码之前 |
| [`docs\02_代码逐段讲解.md`](docs/02_代码逐段讲解.md) | 213 行逐段拆解 + 原版的 6 个问题 + 优化版的做法 | 看代码时对照 |
| [`docs\03_硬件与接线.md`](docs/03_硬件与接线.md) | 引脚表、L298N 真值表、红外接收头、为什么不能调速 | 接线 / 想改硬件 |
| [`docs\04_NEC红外协议.md`](docs/04_NEC红外协议.md) | 38kHz 调制、帧结构、位序（含手算示例）、重发帧 | 想吃透按键码 |
| [`docs\05_常见问题.md`](docs/05_常见问题.md) | 编译报错、上传失败、19 种现象排查、新旧库差异 | 出问题时 |
| [`docs\06_加装PWM调速.md`](docs/06_加装PWM调速.md) | ENA/ENB 接线、起步防打滑曲线、死区/增益补偿、EN 交叉 | 想调速时 |
| [`docs\90_交接文档.md`](docs/90_交接文档.md) | **状态快照 + 待办 + 不要做的事 + 常用命令** | 接手项目 / 隔一段时间回来 |

---

## 目录结构

```
Arduino1\
├─ README.md                      ← 你在这里
├─ docs\                          ← 7 篇中文讲解
├─ reference\                     ← 原厂资料（按键表、注意事项、扩展板原理图）
├─ libraries\IRremote\            ← 编译必需的库（Ken Shirriff v2 系）
├─ 01_original\Remote_contorl\Remote_contorl.ino
├─ 02_annotated\RemoteControl_Annotated\RemoteControl_Annotated.ino
├─ 03_optimized\RemoteControl_Optimized\RemoteControl_Optimized.ino
├─ 04_button_test\ButtonMotorTest\ButtonMotorTest.ino
├─ 04_pwm\RemoteControl_PWM\RemoteControl_PWM.ino   ← ENA→D5 / ENB→D6 调速
├─ 04_phone_remote\               ← 手机 App（自带红外的手机，车端零改动）
├─ tools\build.ps1                ← 命令行编译脚本（.ino → .hex）
└─ build\                         ← 编译产物（每个工程一个子目录）
```

---

## 这套硬件的一句话总结

> 遥控器发 38kHz 调制的 NEC 帧 → 接收头解调成"低电平=有载波"的数字信号 → 进 A4
> → IRremote 用 Timer2 每 50µs 采样一次、记录每段电平长度 → 主循环解出 32 位键码
> → 查表得到动作 → 按 L298N 真值表写 D8~D11 → 电机转。

硬件只有：ATmega328P（Uno 兼容板）、L298N 双 H 桥、38kHz 红外接收头、四个直流电机（左右各两并）。
