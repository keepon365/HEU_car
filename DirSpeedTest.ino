/* ============================================================================
 *  DirSpeedTest.ino   ——  “前进全速 / 倒车减速” 验证程序
 * ----------------------------------------------------------------------------
 *  这是一个**测试程序**，目的很简单：
 *      验证 ENA/ENB 的 PWM 真的生效了，而且占空比是按**动作**给的。
 *
 *  控制流程和 04_pwm 完全一致（同一套红外解码、查表、软启动、刹车），
 *  区别只有一处：占空比不再由"速度档"决定，而是**写死在按键表里**。
 *
 *  ---- 占空比规则（速度整体降下来了）----------------------------------------
 *
 *      按键      动作              左轮  右轮   占空比
 *      ---------------------------------------------------------------
 *      CH        前进              +1    +1    120  ← 基准速度（= 原来倒车的速度）
 *      +         后退              -1    -1     96  ← 比基准再慢 20%
 *      <<        左转（单边）      停    +1    120
 *      >||       右转（单边）      +1    停    120
 *      >>|       停车              停    停     -（短路制动）
 *      -         原地左旋          -1    +1    108
 *      EQ        原地右旋          +1    -1    108
 *
 *      三个数都在文件顶部（DUTY_BASE / DUTY_BACK / DUTY_SPIN），改完重烧即可。
 *      为什么旋转取 108（基准的 90%）：旋转时左右轮反向，如果一边快一边慢，
 *      车会绕偏移的圆心画歪圈；两边给同一个占空比才转得圆。
 *
 *  ---- 左右轮不同步（右侧比左侧早起 0.25 秒）------------------------------
 *
 *      软件上左右是完全对称的：同一个 setOneWheel()、同一次 tick、
 *      两次 analogWrite 之间只差几微秒。所以秒级的差异只可能来自硬件：
 *      **两边"刚好能转起来"所需的占空比不一样**（电机/减速箱阻力、轮胎压地、
 *      使能线接触电阻都会造成这个）。
 *
 *      按 300/s 的斜率反推：右侧 60 就转、左侧要到 ~110 才转，
 *      换算出来正好是 0.25 秒的起步时间差。
 *
 *      处理办法：先标定，再补偿。
 *
 *      【第一步 标定】烧 06_side_calib\SideCalib，把车垫起来，
 *      串口（9600）会分两次、慢慢地把左轮、右轮从 30 加到 180，
 *      你盯着轮子记下"刚开始转"时的那两个数。
 *
 *      【第二步 补偿】把差值填到下面（本机已按实测值填好）：
 *          #define TRIM_LEFT   (左侧阈值 - 右侧阈值)
 *          #define TRIM_RIGHT  0
 *      实测：左 73、右 60 → TRIM_LEFT = 13（已填好）。
 *
 *      补偿只在**起步阶段**生效（cmd ≤ DUTY_KICK 时给满补偿，
 *      到 DUTY_BASE 时淡出为 0），所以巡航时两侧占空比仍然一样、车不会跑偏。
 *      这跟"直接把左边加 18"是两回事 —— 那样巡航时左轮会比右轮快，车会画龙。
 *
 *      【如果两个阈值差不到 10】别急着补偿，先查接线（顺序见 SideCalib 开头）：
 *          ① 交换 ENA/ENB 两根信号线 → 延迟跟着线走 = 线/接触问题
 *          ② 交换左右电机输出线     → 延迟跟着轮子走 = 电机/机械问题
 *          ③ 量 ENA、ENB 对 GND 的电压，两边差应 < 0.3V
 *
 *      【TRIM 可以凭"看起来是否同时"微调，不一定要严格等于标定差值】
 *      两轮同时启动时电流会突然变大、电池电压被拉低，弱的那一侧容易被"拽回去"，
 *      所以实际需要的补偿往往比标定差值**大一些**。
 *      做法：先填标定差值，试车；左轮还是晚 → 每次 +3~5，直到两边一起动。
 *
 *  ---- 加速设计：起步别给大功率，一点点加上去 -------------------------------
 *
 *      要解决的问题是"一开始功率太大会打滑"。打滑的根源不是速度高，而是
 *      **扭矩上升太快**：占空比一瞬间跳到目标值 → 电机瞬间输出最大扭矩 →
 *      轮子超过地面附着极限 → 空转，车反而没走。
 *
 *      所以把"0 → 目标"拆成三段（参数见 DUTY_KICK / KICK_HOLD_MS / ACCEL_PER_SEC）：
 *
 *          占空比
 *          120 ┤                        ╭───────────  目标（巡航）
 *              │                    ╱
 *              │                ╱     ← 第三段：按 ACCEL_PER_SEC 匀速爬
 *           60 ┤━━━━━━━━━━━━        ← 第一段：直接给"起步占空比"（够转起来）
 *              │        ↑第二段：保持 KICK_HOLD_MS（让两个轮子先同步咬合）
 *            0 ┴────┬────┬────┬────┬──────────► 时间
 *                   0   80ms        ≈280ms
 *
 *      三个参数怎么调：
 *
 *        · DUTY_KICK（默认 60）：**这一步最关键**。含义是"刚好能让轮子从静止
 *          开始转"的占空比。太低 → 电机哼哼不转（堵转电流大还发热）；
 *          太高 → 一上来就打滑。调法：把车架空，从 40 开始往上加，直到轮子
 *          能稳定转起来、且用手轻轻一碰就能停住（说明扭矩不大）。
 *        · KICK_HOLD_MS（默认 80）：保持起步占空比多久。两个轮子的静摩擦、
 *          齿轮间隙都不一样，保持一下能让它们"同时咬上"再一起加速，
 *          避免一两个轮子先转、另外的还在打滑。
 *        · ACCEL_PER_SEC（默认 300）：爬升斜率，单位"占空比/秒"。
 *          300 = 1 秒涨 300 → 从 60 到 120 大约 0.2 秒。
 *          还打滑就降到 150~200；想更跟手就升到 500。
 *
 *      另外：**换向时也会重新走这条曲线**。前进↔后退、直行↔旋转，
 *      程序会先把占空比清零（ENA=0，桥臂关断），再按起步曲线重新加功率，
 *      避免电机反接瞬间的电流冲击和齿轮"磕"的一声。
 *
 *  ---- 按住加速：像手柄扳机，按得越久越快，到顶封住 -------------------------
 *
 *      上面那条曲线只管"从静止到能转"的 0.2 秒。之后车一直是同一个速度，
 *      直到松手 —— 这跟手柄的手感差得远：手柄是"越扣越深"。
 *
 *      现在的行为：按下瞬间给一个较低的目标（上限的 65%），随后**目标值随时长
 *      线性上升**，按住 RAMP_FULL_MS 之后到上限，再按也不会更快。
 *
 *          占空比
 *           120 ┤                          ╭────────────  上限（DUTY_BASE × 全局速度）
 *               │                      ╱
 *               │                  ╱
 *            78 ┤──────╱           ← 目标值：0ms 时 78，1600ms 到顶
 *               │   ╱
 *            40 ┤╱                 ← 起步 kick（防打滑那 0.2 秒还在，两条曲线串联）
 *              0 ┴──────┬──────────┬────────┬──────► 按住时长
 *                       80ms      1600ms   之后
 *
 *      为什么"目标"和"实际"分开：如果直接把 currentDuty 按时长往上推，
 *      起步那 0.2 秒的防打滑就被绕过去了（一上来就是 78，可能打滑）。
 *      现在 rampedDuty() 只算"该到哪"，serviceRamp() 仍然按 ACCEL_PER_SEC
 *      爬过去 —— 防打滑和越按越快互不干扰。
 *
 *      三个参数（RAMP_ON / RAMP_START_PCT / RAMP_FULL_MS）：
 *        · 上限不用另设：就是 DUTY_BASE（再乘全局速度）。想"最快到这也行"，
 *          直接把 DUTY_BASE 调小，整条曲线跟着降。想临时减半用 SPEED_PERMILLE。
 *        · RAMP_START_PCT 别低于 65：左轮起转门槛 73，低于它会出现"按了不走"。
 *        · RAMP_FULL_MS 是"摸到顶"的时间：1600 比较像手柄，2500 很线性，
 *          800 基本就是"一按就窜"。
 *
 *      ⚠️ 转弯（<< >>）和原地旋转（- EQ）**不参与**这条曲线：
 *         它们的 duty 本来就贴着起转门槛，再压到 65% 会"按了不转、松手才动"。
 *
 *      ⚠️ 计时只在"新按下 / 换动作"时重置，App 每 200ms 发来的重发帧不会重置，
 *         否则速度永远爬不上去（这是最容易写错的地方）。
 *
 *  ---- 接线（和 04_pwm 一模一样）-------------------------------------------
 *
 *      L298N          Arduino
 *      ENA  ────────►  D5   (OC0B)  PWM
 *      IN1  ────────►  D8
 *      IN2  ────────►  D9
 *      IN3  ────────►  D10
 *      IN4  ────────►  D11
 *      ENB  ────────►  D6   (OC0A)  PWM
 *
 *      前提：D5 上的灯 & 两个红外避障模块已拔掉；L298N 的 ENA/ENB 跳线帽已拔掉。
 *      （原因见 docs\06_加装PWM调速.md，这里不再重复）
 *
 *  ---- 怎么判断"测试通过" ---------------------------------------------------
 *
 *      1. 按 CH → 车往前冲，速度明显比按 "+" 快；串口（DEBUG_SERIAL=1）打印
 *         duty=120，按 "+" 打印 duty=96。
 *      2. 按 "1"~"9" 没反应（本程序没有档位功能）→ 说明你烧的是这个程序，不是 04_pwm。
 *      3. 按 ">>|" 车立刻停住、推不动（短路制动），不是慢慢溜停。
 *      4. 左右轮速度应该一致（两路 PWM 同属 Timer0，频率相同）。
 *         如果一边明显快，先查那个电机的机械阻力，再查 ENA/ENB 是否有一个没接好。
 *
 *  ---- 抬手即停（像真遥控器那样）--------------------------------------------
 *
 *      红外本身是"开关量"：一个键按一次发一帧，松手时**什么都不发**——
 *      没有"松手"这个信号可收。所以想做到松手就停，靠的不是收到什么，
 *      而是**多久没收到**：看门狗。
 *
 *      App 按住不放时会反复补发同一帧（holdTick），于是
 *      "还按着" = 帧一直来，"松手了" = 帧断了。
 *
 *      ---- 阈值为什么不能写死 ------------------------------------------------
 *
 *      写死一个数会两头不讨好：
 *        · 太小（比如 250）→ App 万一 300ms 才发一帧，按住也会被误判成松手；
 *        · 太大（比如 600）→ 松手后车还要多跑半秒，手感发飘。
 *      所以改成**按实测间隔自己算**：
 *
 *          阈值 = 平滑后的帧间隔 × 1.35，限制在 [240, 800] ms
 *
 *      开机用 200ms 打底；按住一两秒后就会收敛到你手机上真实的间隔。
 *      串口每帧打印 `[帧] 间隔=xxx  阈值=yyy`，松手后打印
 *      `[超时] 松手 268ms（阈值 270ms）→ 自动停车`，两个数一对就知道
 *      剩余延迟花在哪了。
 *
 *      想更跟手：DEADMAN_PCT 改 125（代价是丢一帧就停，会一顿一顿）。
 *      想抗干扰：改 150~170。彻底关掉：DEADMAN_FIXED_MS 改 0 且 DEADMAN_ADAPT 改 0。
 *
 *      两个配套细节（已改好，别改回去）：
 *        · 防抖分支也要刷新 lastCommandMs —— App 一帧里塞多组紧凑重发时，
 *          第 2、3 组会被 DEBOUNCE_MS 滤掉，不刷新就会把"按住"误判成"松手"，
 *          车会一顿一顿地停。
 *        · 只有本程序认得的动作键才续命（lastCodeMapped）。陌生键码不该让车一直跑。
 *
 *  ---- 编译 ----------------------------------------------------------------
 *      pwsh -File .\tools\build.ps1 -SketchDir .\05_dir_speed\DirSpeedTest
 * ==========================================================================*/

#include <IRremote.h>
#include <EEPROM.h>       // 存左右增益，改完不用重烧
#include <Servo.h>        // 遛狗模式：舵机扫描（占用 Timer1，与电机 PWM 的 Timer0 不冲突）

/* ---------------------------------------------------------------------------
 *  【可调参数】
 * -------------------------------------------------------------------------*/
#define DEBUG_SERIAL      1     // 测试程序默认开着：串口 9600 能看到每个动作的 duty
                                // 正式跑车（不需要看串口）时改回 0，响应会更快
#define STOP_ON_UNKNOWN   1     // 解不出协议的信号 → 停车
#define DEADMAN_ADAPT     1     // 抬手即停：1 = 阈值按**实测帧间隔**自动算（推荐）
#define DEADMAN_FIXED_MS  300   // DEADMAN_ADAPT=0 时用的固定阈值；0 = 关掉抬手即停
#define DEADMAN_PCT       135   // 自适应阈值 = 帧间隔 × 1.35（越小越跟手，越容易被丢帧误停）
#define DEADMAN_MIN_MS    450   // 自适应下限：低于它就会和帧周期本身打架
                                //   ★ 从 240 提到 450 的原因：App 每 200ms 一帧，
                                //     丢一帧就是 400ms 的空档。以前丢了无所谓（速度是
                                //     恒定的），现在一旦被看门狗停掉，再起步要重新爬
                                //     1.6 秒才回到全速 —— 表现成"跑着跑着突然慢下来"。
                                //     450 正好能吃住一次丢帧。
                                //     放心：新版 App 松手会**主动发停车帧**，所以这个
                                //     阈值只是"App 挂了/滑到后台"的兜底，不决定手感。
#define DEADMAN_MAX_MS    800   // 自适应上限：App 发得慢时也不至于永远不停
#define LED_PULSE_MS      30    // 收到有效信号后板载 LED 亮多久
#define DEBOUNCE_MS       120   // 同一键码在此时间内重复出现视为重发帧

// ---- ★ 三个"动作 → 占空比"的数，改这三个就够了 ----
#define DUTY_BASE         120   // 基准巡航速度（= 原来倒车的速度，255 的 47%）
                                //   前进 / 单边左转 / 单边右转 都用它
#define DUTY_BACK          96   // 后退：比基准再慢 20%
                                //   如果倒车"哼哼不转"，把它提到和 DUTY_BASE 一样
#define DUTY_SPIN         108   // 原地旋转：基准的 90%（左右同值，转得圆）
#define DUTY_TURN          85   // 左转 / 右转：只驱动外侧轮时的占空比
                                //   ⚠️ 原值 65 是个 bug：乘全局速度后只有 72，
                                //   低于左轮起转门槛 73，所以「按了完全没反应」。
                                //   现在 App 已把所有转向交给原地旋转，这个键只剩
                                //   旧版 App（v2.2 及以前）还在发，提到 85 让它至少能动。
                                //   85×1.1 = 94，够克服内侧轮拖滑，又不会太冲。
                                //   ⚠️ 左转靠右轮（门槛 60）、右转靠左轮（门槛 73），
                                //   同一个值下右转会略慢于左转，这是正常的。

// ---- ★ 全局速度（千分比，带一位以上小数精度）------------------------------
//   1000 = 1.000 倍（原始速度）   975 = 97.5%（慢 2.5%）   1050 = 105%
//   作用在"目标巡航占空比"上：前进 / 后退 / 转弯 / 旋转全都跟着缩放，
//   但起步 KICK（DUTY_KICK）不缩放 —— 那是"刚好能转起来"的门槛，不该跟着变。
//   范围 600 ~ 1400。
//   ⚠️ PWM 只有 8 位，duty=120 时 1 个 duty ≈ 0.83%。所以相邻 1~2 个千分点
//   在低速动作上可能算出同一个整数、看不出差别。这是硬件分辨率下限，
//   不是参数没生效 —— 要明显的变化，一次动 10~20（1~2%）。
#define SPEED_PERMILLE   1100

// ---- ★ 起步加速曲线（防打滑），三个参数见文件开头说明 ----
#define DUTY_KICK          40   // 起步占空比：刚好能让轮子从静止转起来
                                //   车架空从 40 往上试，直到"能稳定转、手一碰就停"
#define KICK_HOLD_MS       80   // 起步占空比保持多久（让两个轮子先同步咬合）
#define ACCEL_PER_SEC     300   // 爬升斜率：每秒增加多少占空比（越小越温柔）
#define DECEL_PER_SEC     900   // 下降斜率：每秒减少多少占空比（比加速快，收油更跟手）
#define RAMP_TICK_MS       10   // 曲线刷新周期（毫秒）

// ---- ★ 按住加速（像手柄扳机：按得越久越快，到顶封住）----------------------
//   前面的 DUTY_KICK/KICK_HOLD/ACCEL 解决的是"从静止到能转"这 0.2 秒的防打滑；
//   这三个参数解决的是"按住之后速度还往上走"——两者是串联的：
//
//      占空比
//        │                    ┌────── DUTY_BASE（上限，封顶不再涨）
//        │                 ╱
//        │              ╱       ← 目标值随时长线性上爬（这一段是新加的）
//        │           ╱
//        │  kick ──╱           ← 起步 0.2s：kick 40 → 目标起点
//        └───────────────────────────────► 按住时长
//        0        RAMP_FULL_MS
//
//   ⚠️ 上限就是 DUTY_BASE（再乘全局速度 SPEED_PERMILLE）：
//     "不快过某个速度"这件事由 DUTY_BASE 管，不用再另设一个数。
#define RAMP_ON            1    // 0 = 关掉，退回"按下即巡航速度"的老行为
#define RAMP_START_PCT     60   // 刚按下时的目标 = 上限的 60%（132 → 79）
                                // ★ 别低于 55：cmd=72 时左 85 / 右 63，两轮都已过门槛
                                //   （73 / 60），才是同时起步。压到 40 会让起始
                                //   cmd=57 掉到门槛之下，左轮先越线 → 车往右斜，
                                //   这就是 2026-11-16 那次"左轮先动"的直接原因。
#define RAMP_FULL_MS     2000   // 按住多久爬到上限（越大越线性，越小越蹿）
                                //   1200 = 急性子  1600 = 适中  2500 = 很线性

/* ============================================================================
 *  ★★ 遛狗模式（DOG MODE）—— 车自己去找「会动的东西」★★
 * ----------------------------------------------------------------------------
 *  需求原话：启动后，自动去离得最近的、移动的物体；如果没有，进行扫描；
 *            如果扫描也没有动的东西，就去最近的。
 *
 *  一轮循环 = 扫 5 档 → 挑目标 → 转向 → 前进，全非阻塞（每拍只推一步），
 *  所以任何时候按红外都能立刻接管，不会出现「按下去了但车不理你」。
 *
 *      SCAN  舵机 150→120→90→60→30，每档静置 150ms 再测（舵机自身震动会污染读数）
 *      MEAS  同一档连采 DOG_SAMPLE_N 次，记下最小值与最大值
 *      PICK  「最大-最小 ≥ DOG_MOVE_CM」即判为该方向有东西在动（单轮内比，车没动）
 *            → 在「在动的」里挑**最近**的；一个都没有 → 在「全部」里挑**最近**的
 *      TURN  目标偏离车头超过 DOG_DEAD_DEG 就原地旋转，时长按 DOG_TURN_DPS 开环估算
 *      GO    舵机回中，前进 DOG_GO_MS；期间每 DOG_GO_CHECK_MS 看一次正前方，
 *            ≤ DOG_STOP_CM(5cm) 立刻刹住 → 整机转 180° 换方向再扫；
 *            累计走满 DOG_CONFIRM_MS 也刹车重扫（纠偏）
 *
 *    ★ 遛狗模式**不走加速曲线**：指令即满速。手按遥控才需要"按得越久越快"，
 *      遛狗是程序自己在开，起步慢只会表现成"启动了却不动"。
 *
 *  ---- 安全阀（这几条缺一不可，别删）------------------------------------
 *    · DOG_MAX_MS 硬超时：到点必停，防止车跑丢或电池耗尽
 *    · 连续 DOG_LOST_MAX 次无回波 → 直接退出，防止探头坏了还在盲跑
 *    · 任何映射键（前进/后退/左旋/右旋/停车）都会立刻退出并执行该动作
 *    · 「追到了」（正前方 ≤ DOG_STOP_CM = 5cm）→ 刹住 + **整机转 180°** 换方向再扫，
 *      不会一头顶在墙上反复重试
 *
 *  ---- 调参从哪下手 -----------------------------------------------------
 *    · 追不动/太磨蹭 → DOG_GO_MS 加大（一次走久点）
 *    · 车头对不准 → 标定 DOG_TURN_DPS（实测 1 秒能转多少度）
 *    · 老是撞到才停   → DOG_STOP_CM 加大（5 → 8）
 *    · 把静止物当移动物 → DOG_MOVE_CM 加大（8 → 12）
 *    · 漏掉慢速移动物   → DOG_SAMPLE_GAP_MS 加大（40 → 60，观察窗口变长）
 *    · 左/右判反了    → 把 DOG_ANGLE_REVERSED 改成 1，或串口发 `V` 在线翻转
 * ==========================================================================*/
#define DOG_ENABLED        1     // 0 = 整个自动模式（遛狗 + 比赛）不编译进去

/* ---- 两种自动模式 ----
 * MODE_DOG  遛狗：优先追"会动的东西"，追到 5cm 转 180° 换方向
 * MODE_RACE 比赛：**只求最近的**，不管动没动、不管多远，一路顶过去；
 *            贴近后还要多推一段（车底有网，物块会被兜进去），
 *            因为物块一旦进底盘就落进盲区，车自然"看不见"了，可以继续收集。
 *            这也是比赛模式和遛狗模式唯一的本质差别。 */
#define MODE_DOG           1
#define MODE_RACE          2

#define RACE_GO_MS       1100    // 比赛：一次前进多久
                                 //   别太长：车没有编码器，走越久偏得越多。
                                 //   1100ms 走完立刻回扫描重新对准，这就是"多测测"。
#define RACE_PUSH_CM        8    // 比赛：贴到这个距离就开始"多推一段"
#define RACE_OVERRUN_MS  1200    // 比赛：贴近后再多推这么久，把物块兜进底盘
                                 //   ★ 走完一段后：推过东西 → 蜂鸣 + 180°；没推过 → 直接回扫描
#define DOG_SCAN_SETTLE_MS 150   // 遛狗：舵机到位后静置多久再测（太小 → 读到舵机震动）
#define RACE_SETTLE_MS      90   // 比赛：9 档 15°，每次只转 15°，到位快得多。
                                 //   150ms 会让一轮扫描拖到 3.1 秒，车走走停停像卡住。
#define RACE_SAMPLE_N        3   // 比赛：不判"移动"，每档采 3 次就够取最小值
#define RACE_SAMPLE_GAP_MS  35

/* ---- 单轮扫描 + 同角度多次采样 ----------------------------------------
 * 车在扫描期间是**静止的**，所以同一个舵机角度上连续几次读数，
 * 只有「被照到的东西自己在动」才会变。这就是判定运动的全部依据。
 *
 * ★ 早先的做法是「扫两轮，同角度跨轮比距离」，前提是车没转过弯 ——
 *   可车转过弯之后，同一舵机角度对应的世界方向已经变了，比的其实是两个地方，
 *   转得越多越不准。单轮内比就没有这个问题，因为全程车一动不动。
 *
 * 代价：时间窗口只有 DOG_SAMPLE_N × DOG_SAMPLE_GAP_MS（默认 200ms），
 *   移动得太慢的物体（200ms 内位移不到 DOG_MOVE_CM）测不出来。 */
#define DOG_SAMPLE_N        5    // 每档连续采几次（同一角度内）
#define DOG_SAMPLE_GAP_MS  40    // 两次采样之间隔多久（超声手册要求 ≥60ms 才不串波，
                                 //   但同一点连续测、只用最小值，40ms 足够稳）

#define DOG_MIN_CM          5    // 小于它不算「目标」（车底网里的物块就在 5cm 上下）
#define DOG_EDGE_DEG       45    // 偏角超过它算「侧边」
#define DOG_EDGE_CM        18    // ★ 侧边档位的门槛要比中间高得多。
                                 //   实测（2026-11-17）：舵机打到 150/30 时，
                                 //   车头零件和线束常年被读成 12~15cm —— 全局门槛放到
                                 //   5cm 之后，这些假读数就成了"最近目标"，
                                 //   车一路朝它转，看起来像"只会往一个方向转"。
                                 //   侧边 18cm 以内一律不算目标。
#define DOG_STOP_CM         5    // 前进时正前方 ≤ 它就刹住 → 整机转 180° 换方向
#define DOG_MOVE_CM         8    // 同一角度多次读数「最大-最小」超过它 → 判为在动
#define DOG_FWD_PCT        70    // 遛狗前进速度 = DUTY_BASE 的百分之几（别设太低，
                                 //   低于 60 会掉到左轮起转门槛 73 以下）
#define DOG_GO_MS         700    // 一次前进持续多久
#define DOG_GO_CHECK_MS    50    // 前进途中多久看一次正前方
                                 //   ★ 5cm 才刹，速度约 0.6m/s，100ms 就是 6cm 的盲走距离
                                 //   会直接撞上；50ms 压到 3cm，才来得及
#define DOG_GO_STREAK       3    // 连续直行几次后才重新全扫（省掉空扫的时间）

/* ★ 纠偏：连续前进累计到这么久，就必须刹车重新扫一轮，把车头重新对准目标。
 *   车本身有系统性左偏（右轮摩擦小，GAIN 只能压住大方向，压不干净），
 *   走得越久偏得越多。原来只靠 DOG_GO_STREAK 控制，最多能连走 3×700=2100ms，
 *   期间完全不看方向，偏移已经很明显。
 *   改成 1500ms 兜底：先到 1500 就停下重扫（比 streak 早），
 *   重扫会把目标的当前方位重新量一遍，等于强制修正一次航向。
 *   想更直就调小（1200），想少停顿就调大（2000，但会更歪）。*/
#define DOG_CONFIRM_MS   1500

/* ---- ★ 原地旋转角速度：整个遛狗模式唯一的「时间→角度」换算基准 ----
 * 车没有编码器，转多少度只能靠「转多久」估，所以这个数错了，
 * 下面两件事会同时错，而且错同一个倍数：
 *      ① 转向对准（mag / DPS → 时长）
 *      ② 启动 / 结束的提示转圈（360 / DPS × 圈数）
 *
 * 标定前的值是 90°/s，实测在 DUTY_SPIN 下转得比它快数倍 ——
 * 结果「提示转一圈」实际转了好几圈，等同于每次转向都转过头同样的倍数。
 *
 * 标定方法（不用重烧，串口直接调）：
 *      U<数值>   设定当前角速度（例：U380）
 *      K         按当前角速度转整整一圈，用眼睛数
 *      数出来多转了就调小 U，不到一圈就调大
 *      Y<毫秒>   不做换算，直接原地左旋这么久（纯测试）
 * 调准了把数值填回这里，再烧一次固化。
 *
 * 注意：转速跟电池电压有关（电量低会慢），标定请在满电时做。 */
#define DOG_TURN_DPS      290    // 实测标定：2800ms 转约 2.2 圈 → 792°/2.8s ≈ 283，取 290
#define DOG_TURN_MIN_MS    60    // 太短会原地抖一下，反而不走。
                                 //   细分到 15° 后，30° 只需 103ms，下限不能再是 90。
                                 //   ★ 15°（52ms）这种极短脉冲电机根本来不及起转，
                                 //     所以 20° 以内一律"不转、直接走"（见 DOG_DEAD_DEG）
#define DOG_TURN_MAX_MS   700
#define DOG_DEAD_DEG       20    // 目标偏离车头小于这个角度就直接走，不转
                                 //   取 20 是为了让 15° 那一档直接走：
                                 //   超声波锥角约 15°，偏 15° 目标仍在视野内，
                                 //   硬转 52ms 反而因为电机来不及起转而转不到位。
#define DOG_ESCAPE_MS     420    // 追到了/没目标时，转开多久
#define DOG_PAUSE_MS      180    // 刹停后等车身晃完

/* ---- 旋转提示（beacon）：给"启动了 / 结束了"一个看得见的回话 ----
 * 红外是单向的：车不知道手机按了什么，手机也不知道车收没收到。
 * 两侧唯一共享的通道就是眼睛，所以用原地转圈当确认信号：
 *      启动 → 向左转 DOG_BEACON_TURNS 圈
 *      结束 → 向右转 DOG_BEACON_TURNS 圈（含超时、探头失效等自动结束）
 * 转完自己停住，不需要任何按键。转圈期间照常解红外，按任意键立刻中断。 */
#define DOG_BEACON_ON        1    // 0 = 关掉这套提示
#define DOG_BEACON_TURNS     1    // 转几圈
/* 一圈的时长不再写死，由 DOG_TURN_DPS 换算（360/DPS 秒）。
 * 原来这里是独立的 2800ms（隐含 128°/s），跟转向用的 DPS 不是一个数，
 * 两边必然有一边是错的 —— 合并成一个基准，改一个数两边一起对。 */
#define DOG_BEACON_DUTY      DUTY_SPIN
#define DOG_MAX_MS      60000UL  // 硬超时，60 秒
#define DOG_LOST_MAX        6    // 连续无回波多少次就判定探头失效
#define DOG_ANGLE_REVERSED  0    // 1 = 角度小的那一侧在左（安装方向相反时才改）

// ---- ★ 左右不同步补偿（标定结果已填好）----
// TRIM = 该侧"起步阈值"比另一侧高出来的占空比。
//   SideCalib 实测：左轮 73 起转，右轮 60 起转 → 差值 13
//
//   ★ 历史：一开始 ENA/ENB（或电机输出）有一侧接反，补偿被加到了另一个轮子上，
//     所以当时把 TRIM 加到 50 都没用（越是加，另一侧越早）。
//     把线对调之后，TRIM 调回 10 就几乎同步了 —— 说明现在补偿落在正确的轮子上。
//     10 和理论差值 13 差 3，属于正常误差（电池电压、地面、轮胎压地）。
//
// 补偿的形状（三段）：
//      指令 ≤ FADE_START(95)  ：全额补 TRIM   ← 关键：让两侧在同一拍过阈值
//      FADE_START ~ 目标占空比 ：线性淡出到 0
//      指令 = 目标占空比       ：不补，两侧等占空比
//
// ★ 淡出的终点是**当前动作的目标占空比**，不是写死的 120。
//   因为倒车目标是 96、旋转是 108，都低于 120；如果淡出终点固定 120，
//   这两个动作永远淡不完 —— 补偿会一直加在左轮上，车就会持续往一边偏。
//
// 为什么不能从 kick 就开始淡出？看指令 60 那一拍：
//   右侧拿 60（= 它的阈值，刚好转）；
//   若补偿从 kick 就开始淡出，左侧只能拿到 60+13×(120-60)/60 = 73，
//   这次碰巧够（因为阈值差正好 13），但阈值差再大一点就会差若干格，
//   表现成"补偿了但还是晚一步"。所以一律按"先给满，过了阈值再淡出"写。
#define TRIM_LEFT        25    // 左轮"起步"补偿（谁先动的问题）
#define TRIM_RIGHT         0
#define COMP_FADE_START   95    // 低于这个指令值：给满 TRIM（比两个阈值高 20+ 余量）

// ---- ★ 增益补偿（巡航跑直线），256 = 1.0 倍 ----
// TRIM 解决"谁先动"，GAIN 解决"谁转得快"，两者别混用。
//
//   ★ 方向别搞反（本机就在这里错过一次，白调了很久）：
//       车往**左**偏 = 右轮单位时间跑得更远（左轮慢 / 右轮空转打滑）→ 压 **右**轮
//       车往**右**偏 = 左轮单位时间跑得更远                        → 压 **左**轮
//     口诀：车永远朝"慢"的那一侧拐，所以要调的是**快**的那一侧。
//     之前填的 GAIN_LEFT=236 是把左轮压到 92%，而左轮本来就是慢的一侧 —— 
//     等于在制造左偏，越调越歪。
//
//   256 = 不打折；250 ≈ 97.7%（每 2 米少走约 4.6cm）；240 ≈ 93.8%。
//   千分之几的差，跑 2 米就能看出十几厘米，所以每次动 ±2~4 就够，别一次跳 10。
#define GAIN_LEFT        256    // 左轮：不打折（左轮摩擦大，是"慢"的一侧）
#define GAIN_RIGHT       222    // 右轮摩擦小、更容易转 → 压低它等于把车头往右掰

// ---- ★ 增益只对"直行"生效 ----
// 单边转弯和原地旋转也压一轮，会把动作本身带歪，而且这种歪是白歪的：
//   · 原地旋转：两边给同一个 duty，压右轮 → 转出来是弧线，不是原地圆转
//   · 单边转弯：只驱动一只轮子，补偿在这里没有任何"纠偏"意义
// 立起车（轮子悬空）压 GAIN 时最明显：直行看着没问题，一按旋转就往右画圈。
// 改成 1 之后，转弯和旋转恢复左右对称，直行照样补偿。
#define GAIN_STRAIGHT_ONLY 1    // 1 = 仅直行/后退补偿（推荐）；0 = 所有动作都补偿（旧行为）

#if (COMP_FADE_START < DUTY_KICK)
#error "COMP_FADE_START 不能小于 DUTY_KICK，否则起步阶段补偿不完整"
#endif

/* ---------------------------------------------------------------------------
 *  【引脚】
 * -------------------------------------------------------------------------*/
const uint8_t RECV_PIN = A4;

/* 超声波 HC-SR04：直连 UNO（扩展板的 CSB1 插座 5V 不供电，实测走插座不通）。
 * 用 A0/A1 是因为那两个是模拟口，做数字 IO 一样好用，而且不占任何 PWM/中断脚。*/
const uint8_t US_TRIG = A0;
const uint8_t US_ECHO = A1;

/* ---- 云台舵机：**180° 位置舵机**，接 D12 ----
 * 扫描超声波时用它左右转。
 * ★ 早先三个脚（D2/D7/D12）同时输出同一角度，是为了排查接线时不用问。
 *   现在 D2/D7 被两颗 360° 舵机占了，**必须拆开** ——
 *   360 舵机收到同一个"角度"信号不会定位，只会按脉宽持续转，会乱转。
 * Servo 库占 Timer1，与 Timer0（电机 PWM / millis）和 Timer2（红外）不冲突。 */
const uint8_t PIN_CAM    = 12;      // 云台 180° 舵机
const uint8_t PIN_S360_L = 2;       // 左 360° 连续旋转
const uint8_t PIN_S360_R = 7;       // 右 360° 连续旋转

const uint8_t SERVO_PINS[] = { PIN_CAM };
const uint8_t SERVO_N      = sizeof(SERVO_PINS) / sizeof(SERVO_PINS[0]);

/* ---- 两颗 360° 舵机的标定值（2026-11-17 实测）----
 * 幅度 175us（脉宽 1325us）下，4.0 秒转 3.75 圈 → 337 度/秒
 * 极性（实测，与常见资料相反）：< 1500us 顺时针，> 1500us 逆时针，1500 停
 * ★ 360 舵机没有角度反馈，"转 N 度"= 角度 ÷ 角速度 算时间，开环。
 *   单次误差约 ±10~15 度，反复调用会累积；要绝对位置得加机械挡块。 */
#define S360_AMP   175              // 速度幅度（离 1500 的脉宽差）
#define S360_DPS   337              // 度/秒
#define S360_STOP  1500

/* ===========================================================================
 *  蜂鸣器（A3）—— 软件方波，**绝对不能用 tone()**
 * ---------------------------------------------------------------------------
 *  ★ 为什么不能用 tone()：
 *      AVR 上 IRremote 用的是 Timer2（TIMER2_COMPA_vect，见 IRremoteInt.h 的
 *      IR_USE_TIMER2），而 tone() 同样占 Timer2 —— 一响，红外接收立刻失效，
 *      车就不再听遥控了。所以这里用 micros() 自己翻转引脚，不碰任何定时器。
 *      Servo 库占 Timer1，电机 PWM 占 Timer0，互不影响。
 *
 *  音序非阻塞：按下标推进，主循环每拍调 serviceBuzzer() 翻电平 / 换音。
 *  代价是别处阻塞时音会抖（pulseIn 最长 25ms），提示音都很短，听感可接受。
 * =========================================================================*/
const uint8_t PIN_BUZZ = A3;

struct BuzzStep { uint16_t freq; uint16_t ms; };   // freq = 0 表示静音段

/* 启动：两声上行    结束：一声低    收集完成：三声急促 */
const BuzzStep SEQ_START[] = { { 1000, 90 }, { 1500, 130 } };
const BuzzStep SEQ_STOP[]  = { { 520, 240 } };
const BuzzStep SEQ_PICK[]  = { { 1600, 60 }, { 0, 35 }, { 1600, 60 }, { 0, 35 }, { 2000, 100 } };

#define SEQ_START_N  (uint8_t)(sizeof(SEQ_START) / sizeof(SEQ_START[0]))
#define SEQ_STOP_N   (uint8_t)(sizeof(SEQ_STOP)  / sizeof(SEQ_STOP[0]))
#define SEQ_PICK_N   (uint8_t)(sizeof(SEQ_PICK)  / sizeof(SEQ_PICK[0]))

const BuzzStep *buzzSeq     = 0;
uint8_t  buzzLen     = 0;
uint8_t  buzzPos     = 0;
uint16_t buzzHalfUs  = 0;          // 0 = 当前是静音段
uint32_t buzzStepEnd = 0;
uint32_t buzzToggle  = 0;
bool     buzzHigh    = false;

/* 播一段音序，返回总时长（ms），方便调用方决定要不要等它播完 */
uint32_t buzzPlay(const BuzzStep *seq, uint8_t n)
{
  if (n == 0) { return 0; }

  buzzSeq = seq;
  buzzLen = n;
  buzzPos = 0;

  const uint32_t now = micros();
  const BuzzStep &s = seq[0];
  buzzStepEnd = now + (uint32_t)s.ms * 1000UL;
  buzzHalfUs  = (s.freq == 0) ? 0 : (uint16_t)(500000UL / s.freq);
  buzzToggle  = now;
  buzzHigh    = false;
  digitalWrite(PIN_BUZZ, LOW);

  uint32_t total = 0;
  for (uint8_t i = 0; i < n; i++) { total += seq[i].ms; }
  return total;
}

void buzzStop()
{
  buzzSeq    = 0;
  buzzHigh   = false;
  buzzHalfUs = 0;
  digitalWrite(PIN_BUZZ, LOW);
}

/* 每拍调用：换段 / 翻转电平 */
void serviceBuzzer()
{
  if (buzzSeq == 0) { return; }

  const uint32_t now = micros();

  // ---- 当前段到期：停电平，进下一段 ----
  if ((int32_t)(now - buzzStepEnd) >= 0)
  {
    digitalWrite(PIN_BUZZ, LOW);
    buzzHigh = false;

    buzzPos++;
    if (buzzPos >= buzzLen) { buzzSeq = 0; buzzHalfUs = 0; return; }

    const BuzzStep &s = buzzSeq[buzzPos];
    buzzStepEnd = now + (uint32_t)s.ms * 1000UL;
    buzzHalfUs  = (s.freq == 0) ? 0 : (uint16_t)(500000UL / s.freq);
    buzzToggle  = now;
    return;
  }

  if (buzzHalfUs == 0) { return; }          // 静音段

  // ---- 到点翻转 ----
  if ((int32_t)(now - buzzToggle) >= 0)
  {
    buzzHigh = !buzzHigh;
    digitalWrite(PIN_BUZZ, buzzHigh ? HIGH : LOW);
    buzzToggle = now + buzzHalfUs;
  }
}

Servo srv360L;                      // D2
Servo srv360R;                      // D7

/* 两颗 360° 舵机都停住（脉宽 1500 才是停，写角度 90 在它们身上也是停，
 * 但明确用 writeMicroseconds 更不容易被误解） */
void s360Brake()
{
  srv360L.writeMicroseconds(S360_STOP);
  srv360R.writeMicroseconds(S360_STOP);
}

/* 让某一侧 360° 舵机转 deg 度（正 = 顺时针）。
 * ★ 这是**阻塞版**：内部按时间等它转完。90 度约 270ms，短动作够用；
 *   以后要做"边走边动"这种并行动作，得改成记到期时刻的非阻塞写法。
 *   which: 'L' 左(D2) / 'R' 右(D7) / 'B' 两颗 */
void s360Spin(char which, int16_t deg)
{
  if (deg == 0) { return; }

  const uint16_t a  = (uint16_t)((deg < 0) ? -deg : deg);
  uint32_t ms = (uint32_t)a * 1000UL / (uint32_t)S360_DPS;
  if (ms < 20) { ms = 20; }          // 太短舵机来不及响应

  const uint16_t us = (deg > 0) ? (uint16_t)(S360_STOP - S360_AMP)
                                : (uint16_t)(S360_STOP + S360_AMP);

  if (which == 'L' || which == 'B') { srv360L.writeMicroseconds(us); }
  if (which == 'R' || which == 'B') { srv360R.writeMicroseconds(us); }

  delay(ms);                         // 转到位的等待
  s360Brake();
}

/* ---------------------------------------------------------------------------
 *  ★ EN 交叉开关（实测发现的问题，务必看清）
 * ---------------------------------------------------------------------------
 *  物理接线（已确认）：
 *      OUT1/OUT2 = 左轮，OUT3/OUT4 = 右轮
 *      D8→IN1, D9→IN2（通道 A，驱动 OUT1/OUT2 = 左轮）
 *      D10→IN3, D11→IN4（通道 B，驱动 OUT3/OUT4 = 右轮）
 *  但 EN 这两根：**D5 实际接在 ENB 上、D6 实际接在 ENA 上**（接反了）。
 *
 *  证据（自洽地解释了之前所有怪现象）：
 *    · 车能正常往前开 —— 因为方向脚 IN1~IN4 没接反，两边通道都有正确方向；
 *    · 可 TRIM_LEFT 加到 40 也没用、甚至"补偿是反的" ——
 *      因为命令里的"左轮占空比"被 D5 送到了**通道 B（右轮）**；
 *    · SideDiag 第①段（只给代码左轮 = D8/D9 方向 + D5 使能）两个轮子都不转：
 *      通道 A 有方向没使能，通道 B 有使能但 IN3/IN4 都是低 = 制动。
 *
 *  两种修法，选一个：
 *    ① 硬件：把 D5、D6 两根杜邦线对调（最干净，代码不用动，SWAP_EN 置 0）
 *    ② 软件：保持接线，把 SWAP_EN 置 1（默认）—— 代码会主动把左轮的 PWM 发到 D6、
 *       右轮的 PWM 发到 D5，等效于替你换回来。
 *
 *  ⚠️ 注意这和"补偿方向反了就把 TRIM 填到另一边"不是一回事：
 *     那种做法只是把症状挪走，EN/IN 的配对仍然是错的，刹车/换向会出现半拍异常。
 * -------------------------------------------------------------------------*/
#define SWAP_EN   1     // 1 = D5 接的是 ENB、D6 接的是 ENA（本机现状）；0 = 接线正常

#if SWAP_EN
  const uint8_t LEFT_MOTOR_EN  = 6;   // 左轮的使能实际在 D6（= 模块上的 ENA 通过 D6 到）
  const uint8_t RIGHT_MOTOR_EN = 5;   // 右轮的使能实际在 D5
#else
  const uint8_t LEFT_MOTOR_EN  = 5;   // ENA → D5  (OC0B, Timer0)
  const uint8_t RIGHT_MOTOR_EN = 6;   // ENB → D6  (OC0A, Timer0)
#endif

const uint8_t LEFT_MOTOR_GO    = 8;   // IN1
const uint8_t LEFT_MOTOR_BACK  = 9;   // IN2
const uint8_t RIGHT_MOTOR_GO   = 10;  // IN3
const uint8_t RIGHT_MOTOR_BACK = 11;  // IN4

const uint8_t LED_PIN = 13;

/* ---------------------------------------------------------------------------
 *  【方向约定】
 * -------------------------------------------------------------------------*/
const int8_t DIR_STOP     =  0;
const int8_t DIR_FORWARD  = +1;
const int8_t DIR_BACKWARD = -1;

/* ---------------------------------------------------------------------------
 *  【按键表】每个动作自带占空比
 * ---------------------------------------------------------------------------
 *  duty = 0 表示"停车"，走刹车流程（四个 IN 全低 + 使能给满）。
 * -------------------------------------------------------------------------*/
struct KeyAction
{
  uint32_t code;
  const char *name;
  int8_t leftWheel;
  int8_t rightWheel;
  uint8_t duty;
};

const KeyAction KEY_TABLE[] =
{
  // 键码            名字          左轮          右轮          巡航占空比
  { 0x00FF629DUL, "CH   前进",  DIR_FORWARD,  DIR_FORWARD,  DUTY_BASE },
  { 0x00FFA857UL, "+    后退",  DIR_BACKWARD, DIR_BACKWARD, DUTY_BACK },
  { 0x00FF22DDUL, "<<   左转",  DIR_STOP,     DIR_FORWARD,  DUTY_TURN },
  { 0x00FFC23DUL, ">||  右转",  DIR_FORWARD,  DIR_STOP,     DUTY_TURN },
  { 0x00FF02FDUL, ">>|  停车",  DIR_STOP,     DIR_STOP,     0         },
  // 这两个键承担整车的转向功能（App 圆盘左右即映射到这里）。
  // 方向按实车实测反相过一次：上一版按"左"实际往右偏，故左右轮对调。
  // 若将来换电机接线，这里要再翻一次 —— 只改这两行，其余不用动。
  { 0x00FFE01FUL, "-    左转",  DIR_FORWARD,  DIR_BACKWARD, DUTY_SPIN },
  { 0x00FF906FUL, "EQ   右转",  DIR_BACKWARD, DIR_FORWARD,  DUTY_SPIN },
};
const uint8_t KEY_COUNT = sizeof(KEY_TABLE) / sizeof(KEY_TABLE[0]);

/* ---- 自动模式的触发键：开、关各一个码，**都幂等** ----
 * ★ 为什么不是"按一下切换"（2026-11-17 用户实测踩到）：
 *   红外是单向、无确认的。车一旦跑开、角度偏了，那一帧结束命令丢了，
 *   车就再也停不下来；而 App 为了提高命中率多补两帧，反而会把它来回切。
 *   所以改成「开」「关」两个不同码：同一条命令发一百遍，结果都一样。
 *      遛狗  开 = 数字键 0 (0x00FF6897)   关 = 数字键 1 (0x00FF30CF)
 *      比赛  开 = 数字键 2 (0x00FF18E7)   关 = 数字键 3 (0x00FF7A85)
 * 四个都不进 KEY_TABLE，语义和"按住才走"不同。 */
const uint32_t CODE_DOG_ON   = 0x00FF6897UL;
const uint32_t CODE_DOG_OFF  = 0x00FF30CFUL;
const uint32_t CODE_RACE_ON  = 0x00FF18E7UL;
const uint32_t CODE_RACE_OFF = 0x00FF7A85UL;

/* ---------------------------------------------------------------------------
 *  【全局状态】
 * -------------------------------------------------------------------------*/
IRrecv irrecv(RECV_PIN);
decode_results results;

int8_t   wheelLeft   = DIR_STOP;
int8_t   wheelRight  = DIR_STOP;
uint8_t  targetDuty  = 0;        // 最终要到达的占空比（由按键表给）
uint8_t  currentDuty = 0;        // 现在实际输出的占空比（正在爬）
uint32_t rampMs      = 0;        // 上次斜率更新的时刻
uint32_t holdUntilMs = 0;        // 起步保持期结束的时刻（0 = 不在保持期）
uint32_t holdStartMs = 0;        // 这一次"按住"从什么时候开始
bool     holding     = false;    // 是否正按着（单独用标志，别拿 holdStartMs==0 判断，
                                 //   上电后第 1 毫秒 millis() 就是 0，会误判成"没在按"）

uint32_t lastCommandMs = 0;
uint32_t lastCodeMs    = 0;
uint32_t lastCode      = 0;
bool     lastCodeMapped = false;   // 上一帧是否是本程序认得的动作键（防抖分支用它续命）
uint16_t frameGapAvg = 200;        // 平滑后的"帧到达间隔"，自适应阈值基于它
uint16_t deadmanMs   = DEADMAN_MIN_MS;   // 当前生效的抬手即停阈值（随实测帧间隔自动更新）

#if DOG_ENABLED
/* 放在这里而不是遛狗模块里：串口命令 handleSerialCmd() 要用它翻转，
 * 而那个函数在遛狗模块之前就定义好了。 */
bool angleReversed = (DOG_ANGLE_REVERSED != 0);
#endif

/* ---------------------------------------------------------------------------
 *  左右增益：运行时可调 + 存 EEPROM
 * ---------------------------------------------------------------------------
 *  为什么不在代码里改：每试一次都要重烧（十几秒）+ 拔线，跑完 2 米再回来看结果，
 *  一轮两分钟起步。改成串口发指令，改完立刻生效，按 W 存盘，下次上电还在。
 *
 *  串口监视器（9600）发送，带不带换行都行：
 *      L250   左轮增益改成 250        R248   右轮增益改成 248
 *      S975   全局速度 97.5%          T95    转弯外侧轮 duty=95
 *      ?      显示当前值              W      写入 EEPROM
 *      D      恢复代码里的默认值（并擦掉存档）
 * -------------------------------------------------------------------------*/
uint16_t gainLeft  = GAIN_LEFT;
uint16_t gainRight = GAIN_RIGHT;
uint16_t speedPermille = SPEED_PERMILLE;   // 全局速度，1000 = 原始速度
uint8_t  turnDuty      = DUTY_TURN;        // 单边转弯的占空比（运行时可变）
uint8_t  wheelCmdDuty  = 0;                // 当前动作的"标称"占空比（还没乘全局速度）
bool     moving        = false;            // 是否在运动（定义前移，串口改速度时要用）

#define EE_MAGIC_ADDR  0
#define EE_DATA_ADDR   2
#define EE_SPEED_ADDR  6
#define EE_TURN_ADDR   8
#define EE_MAGIC       0xA5

/* 把一个"标称占空比"按全局速度缩放：120 × 975/1000 = 117
 *
 * ★★ 必须用 uint32_t，不能用 uint16_t ★★
 *   AVR 的 int 只有 16 位，120 × 1100 = 132000 会回绕成 928，
 *   再 /1000 就变成 1 —— 车直接不动。而且只有 SPEED=1000 走上面
 *   的短路分支才幸免，所以 bug 只在改过速度后才暴露。
 *   教训：AVR 上任何 duty(≤255) × 三位数 都要显式提升到 uint32_t。 */
uint8_t scaleSpeed(uint8_t duty)
{
  if (speedPermille == 1000 || duty == 0) { return duty; }
  const uint32_t d = ((uint32_t)duty * speedPermille + 500) / 1000;   // 四舍五入
  return (d > 255) ? 255 : (uint8_t)d;
}

/* 串口命令 G 要开关遛狗模式，但那些符号定义在文件后半段 —— 在这前置声明。
 * （arduino-cli 的自动原型不覆盖 #if 块里的函数，所以必须手写。） */
#if DOG_ENABLED
extern bool dogOn;
extern uint8_t dogMode;
extern uint16_t turnDps;                                 // 运行时旋转角速度（度/秒）
void     dogServoWrite(uint8_t deg);                     // 云台（C 命令要用）
uint32_t beaconOneTurnMs();
void spinFor(int8_t side, uint32_t ms);
void dogStart(uint8_t mode);
void dogStop(const char *why, bool withBeacon = true);   // 默认带"结束"旋转提示
#endif

char    cmdBuf[10];
uint8_t cmdLen = 0;

void printCal()
{
  Serial.print(F("[校准] 左="));  Serial.print(gainLeft);
  Serial.print(F("  右="));       Serial.print(gainRight);
  Serial.print(F("  速度="));     Serial.print(speedPermille / 10);
  Serial.print('.');
  Serial.print(speedPermille % 10);
  Serial.print(F("%  转弯="));    Serial.print(scaleSpeed(turnDuty));
  Serial.print(F("  （右/左="));
  Serial.print((uint16_t)((uint32_t)gainRight * 1000 / gainLeft));
  Serial.println(F("/1000）"));

  // 机器可读的一行：烧录脚本（烧录.bat）靠它核对"实际生效值"。
  // 故意只用 ASCII，避免脚本按不同编码解析中文而出错。**别删这一行**。
  Serial.print(F("[CAL] L="));  Serial.print(gainLeft);
  Serial.print(F(" R="));       Serial.print(gainRight);
  Serial.print(F(" S="));       Serial.print(speedPermille);
  Serial.print(F(" FWD="));     Serial.print(scaleSpeed(DUTY_BASE));
  Serial.print(F(" TURN="));    Serial.println(scaleSpeed(turnDuty));
}

void loadCal()
{
  if (EEPROM.read(EE_MAGIC_ADDR) != EE_MAGIC) { return; }
  uint16_t gl = 0, gr = 0, sp = 0;
  EEPROM.get(EE_DATA_ADDR,      gl);
  EEPROM.get(EE_DATA_ADDR + 2,  gr);
  EEPROM.get(EE_SPEED_ADDR,     sp);
  if (gl >= 150 && gl <= 400) { gainLeft  = gl; }
  if (gr >= 150 && gr <= 400) { gainRight = gr; }
  if (sp >= 600 && sp <= 1400) { speedPermille = sp; }

  const uint8_t td = EEPROM.read(EE_TURN_ADDR);
  if (td >= 60 && td <= 160) { turnDuty = td; }
}

void saveCal()
{
  EEPROM.put(EE_DATA_ADDR,      gainLeft);
  EEPROM.put(EE_DATA_ADDR + 2,  gainRight);
  EEPROM.put(EE_SPEED_ADDR,     speedPermille);
  EEPROM.write(EE_TURN_ADDR,    turnDuty);
  EEPROM.write(EE_MAGIC_ADDR, EE_MAGIC);
}

void handleSerialCmd()
{
  cmdBuf[cmdLen] = '\0';
  char k = cmdBuf[0];
  if (k >= 'a' && k <= 'z') { k = (char)(k - 32); }   // 小写也认

  int16_t v = 0;
  uint8_t i = 1;
  bool    neg = false;
  if (i < cmdLen && cmdBuf[i] == '-') { neg = true; i++; }
  for (; i < cmdLen; i++)
  {
    if (cmdBuf[i] < '0' || cmdBuf[i] > '9') { break; }
    v = (int16_t)(v * 10 + (cmdBuf[i] - '0'));
  }
  if (neg) { v = (int16_t)(-v); }

  switch (k)
  {
    case 'L': if (v >= 150 && v <= 400) { gainLeft  = (uint16_t)v; } break;
    case 'R': if (v >= 150 && v <= 400) { gainRight = (uint16_t)v; } break;
    case 'S':
      if (v >= 600 && v <= 1400)
      {
        speedPermille = (uint16_t)v;
        // 正在走的话立刻按新速度重算目标（ramp 会平滑爬过去，不会突跳）
        if (moving) { targetDuty = scaleSpeed(wheelCmdDuty); }
      }
      break;
    case 'T': if (v >= 60 && v <= 160) { turnDuty = (uint8_t)v; } break;
    case 'W': saveCal(); Serial.println(F("[校准] 已写入 EEPROM")); break;
#if DOG_ENABLED
    /* 遛狗模式左右判反时不用重烧，串口发 V 就能翻过来。
     * 判法：把障碍物放车的左边，看 dogPrintMap 里是 150 的读数小还是 30 的读数小。 */
    case 'V':
      angleReversed = !angleReversed;
      Serial.print(F("[DOG] 左右判定已翻转 → 角度小的那一侧在"));
      Serial.println(angleReversed ? F("左") : F("右"));
      return;                      // 不打印校准行，这跟增益无关

    /* 串口也能开关遛狗模式：调参时不用一直举着手机按红外。
     * 和按遥控器上的数字键 0 完全等价。 */
    case 'G':
      if (dogOn) { dogStop("串口命令"); } else { dogStart(MODE_DOG); }
      return;

    /* B = 蜂鸣器自检。B 三声（收集音） / B1 启动音 / B2 结束音
     * 用来确认 A3 那根线接对了、发声正常。 */
    case 'B':
      if (v == 1)      { buzzPlay(SEQ_START, SEQ_START_N); Serial.println(F("[BUZZ] 启动音（两声上行）")); }
      else if (v == 2) { buzzPlay(SEQ_STOP,  SEQ_STOP_N);  Serial.println(F("[BUZZ] 结束音（一声低）")); }
      else             { buzzPlay(SEQ_PICK,  SEQ_PICK_N);  Serial.println(F("[BUZZ] 收集音（三声急促）")); }
      return;

    /* P = 比赛模式（只求最近的，贴近继续推）。调试时不用举手机。 */
    case 'P':
      if (dogOn && dogMode == MODE_RACE) { dogStop("串口命令"); }
      else
      {
        if (dogOn) { dogStop("切换模式", false); }
        dogStart(MODE_RACE);
      }
      return;

    /* ---- 三颗舵机的手动测试（标定/装机构时用）----
     *   X<度>  左 360° 舵机转多少度（正=顺时针，例 X90 / X-45）
     *   Z<度>  右 360° 舵机同上
     *   C<度>  云台 180° 舵机转到绝对角度（0~180） */
    case 'X':
      if (v != 0 && v >= -720 && v <= 720)
      {
        Serial.print(F("[360] 左舵机 "));
        Serial.print(v > 0 ? F("顺") : F("逆"));
        Serial.print(F("时针 "));
        Serial.println(v > 0 ? v : -v);
        s360Spin('L', v);
      }
      return;

    case 'Z':
      if (v != 0 && v >= -720 && v <= 720)
      {
        Serial.print(F("[360] 右舵机 "));
        Serial.print(v > 0 ? F("顺") : F("逆"));
        Serial.print(F("时针 "));
        Serial.println(v > 0 ? v : -v);
        s360Spin('R', v);
      }
      return;

    /* ---- 旋转角速度标定：一次烧录就能调准，不用反复重刷 ----
     *   U<数值>   设定角速度（度/秒），例 U380
     *   K         按当前角速度转「整整一圈」，用眼睛看是不是真的一圈
     *   Y<毫秒>   不做换算，直接原地左旋这么久（方便自己数）
     * 调准之后把数值写回 DOG_TURN_DPS 再烧一次固化。 */
    case 'U':
      if (v >= 60 && v <= 900)
      {
        turnDps = (uint16_t)v;
        Serial.print(F("[DOG] 旋转角速度="));
        Serial.print(turnDps);
        Serial.print(F(" deg/s   转一圈="));
        Serial.print(beaconOneTurnMs());
        Serial.println(F("ms   发 K 试一圈"));
        return;
      }
      Serial.println(F("[DOG] 用法 U<60~900>   例 U380"));
      return;

    case 'C':
      if (v >= 0 && v <= 180)
      {
        dogServoWrite((uint8_t)v);
        Serial.print(F("[云台] -> "));
        Serial.print(v);
        Serial.println(F(" 度"));
      }
      else
      {
        Serial.println(F("[云台] 用法 C<0~180>，例 C90"));
      }
      return;

    case 'K':
      spinFor(-1, beaconOneTurnMs());
      return;

    case 'Y':
      spinFor(-1, (v > 0) ? (uint32_t)v : 1000UL);
      return;
#endif
    case 'D':
      gainLeft  = GAIN_LEFT;
      gainRight = GAIN_RIGHT;
      speedPermille = SPEED_PERMILLE;
      turnDuty = DUTY_TURN;
      EEPROM.write(EE_MAGIC_ADDR, 0x00);              // 擦掉存档
      Serial.println(F("[校准] 已恢复默认值"));
      break;
    case '?': break;
    default: return;
  }
  printCal();
}

void serviceSerial()
{
#if DEBUG_SERIAL
  while (Serial.available() > 0)
  {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n')
    {
      if (cmdLen > 0) { handleSerialCmd(); cmdLen = 0; }
    }
    else if (cmdLen < sizeof(cmdBuf) - 1)
    {
      cmdBuf[cmdLen++] = c;
    }
  }
#endif
}
/* ===========================================================================
 *  遛狗模式的全局状态
 * =========================================================================*/
#if DOG_ENABLED
Servo dogServos[SERVO_N];

/* 原地旋转角速度（度/秒）。运行时可被串口 U 命令改，方便一次烧录就标定完。
 * 所有「转多久 = 转多少度」的换算都必须走这个变量，不要再另起炉灶写死毫秒。 */
uint16_t turnDps = DOG_TURN_DPS;

/* 扫描角度表。90° = 车头正前方，两张表都是「一侧最大角 → 另一侧最小角」，
 * 方向哪边是左由 DOG_ANGLE_REVERSED 决定（见 dogPick 里的 side 计算）。
 *
 *  ★ 为什么分两套（2026-11-17 用户要求）：
 *      遛狗：5 档 30° 间隔。目标是会动的人/宠物，块头大，粗一点没关系，扫描快。
 *      比赛：9 档 15° 间隔。物块很小，30° 步长会把"偏 20°"量化成"偏 30°"或
 *            "偏 0°"，转向要么转过头要么不动 —— 车就越走越歪。
 *            细分之后偏角最多 7.5°，对准精度直接翻倍。
 *  代价：9 档一轮扫描比 5 档多约 0.9 秒（每档 settle+采样）。
 *        比赛模式反正要停下来看，值得。 */
const uint8_t DOG_ANGLES[]  = { 150, 120, 90, 60, 30 };                        // 5 档 30°
const uint8_t RACE_ANGLES[] = { 150, 135, 120, 105, 90, 75, 60, 45, 30 };       // 9 档 15°

#define SCAN_MAX_N 9                   // 两张表里较大的那个，数组按它分配
const uint8_t *scanAngles = DOG_ANGLES;   // 当前生效的表（dogStart 按模式切换）
uint8_t        scanN      = 5;

/* 采样密度也按模式切换：比赛要快（扫得勤才能一直朝最近），
 * 遛狗要稳（要判断"动没动"，观察窗口不能短）。 */
uint16_t scanSettleMs = DOG_SCAN_SETTLE_MS;
uint8_t  scanSamples  = DOG_SAMPLE_N;
uint16_t scanGapMs    = DOG_SAMPLE_GAP_MS;

uint16_t dogProf[SCAN_MAX_N];         // 本轮各角度距离（cm；0 = 无回波）
/* 单轮内判定：这一档的多次读数波动是否够大。不再需要「上一轮」数组。 */
bool     dogMove[SCAN_MAX_N];         // true = 这个方向有东西在动

/* 同一档内的采样进度 */
uint8_t  dogSmpIdx     = 0;           // 本档已采几次
uint16_t dogSmpMin     = 0;           // 本档最小读数（0 = 还没采到有效值）
uint16_t dogSmpMax     = 0;           // 本档最大读数
uint8_t  dogSmpOk      = 0;           // 本档有效读数个数

bool     dogOn         = false;
uint8_t  dogMode       = 0;           // 0=关  1=MODE_DOG 遛狗  2=MODE_RACE 比赛
bool     dogPushDone   = false;       // 比赛模式：这一段的「多推」是否已经加过
// angleReversed 定义在上面（串口命令要先用到它）

uint8_t  dogPhase      = 0;           // 阶段常量见下面 DOG_IDLE 等
uint32_t dogPhaseMs    = 0;           // 进入当前阶段的时刻
uint32_t dogStartMs    = 0;           // 本次遛狗的开始时刻（硬超时用）
uint32_t dogGoStartMs  = 0;           // 这一串「连续前进」的起点（纠偏计时用）

/* 旋转提示状态：0=没有  1=左旋(启动)  2=右旋(结束)
 * 必须声明在 setWheels() 之前 —— 它要让转圈也走"直给满速"，别被加速曲线拖住。 */
uint8_t  beaconPhase = 0;
uint32_t beaconUntil = 0;
uint8_t  dogIdx        = 0;           // 扫描进度
uint8_t  dogLost       = 0;           // 连续无回波计数
uint32_t dogHoldMs     = 0;           // TURN / GO 要持续多久
uint32_t dogCheckMs    = 0;           // GO 阶段上次测前方的时刻
uint8_t  dogAfter      = 0;           // TURN 结束后去哪个阶段
uint8_t  dogStreak     = 0;           // 连续直行了几次

int8_t   dogTargetIdx  = -1;          // 选中目标在 DOG_ANGLES 里的下标
bool     dogTargetMoving = false;     // 这个目标是不是"会动"
uint16_t dogTargetCm   = 0;           // 目标距离
uint16_t dogFrontCm    = 0;           // GO 阶段最后一次测到的正前方距离
#endif

bool     ledOn         = false;
uint32_t ledOffAtMs    = 0;

/* ---------------------------------------------------------------------------
 *  【函数原型】
 * -------------------------------------------------------------------------*/
uint32_t nowMs();
void     setOneWheel(uint8_t pinGo, uint8_t pinBack, uint8_t pinEn,
                     int8_t dir, bool goHighOnForward, uint8_t duty);
void     applyOutput();
void     setWheels(int8_t left, int8_t right, uint8_t duty);
void     brakeCar();
uint8_t  rampedDuty(uint8_t baseDuty);
void     refreshRampedTarget();
void     serviceRamp();
void     pulseLed();
void     serviceLed();
void     serviceAutoStop();
void     handleFrame();

/* 遛狗模式（实现见 handleFrame 之前那一整段） */
void     dogServoWrite(uint8_t deg);
uint16_t dogMeasure();
void     dogPick();
void     dogTurn(uint32_t ms, int8_t side, uint8_t after);
void     dogGo(uint32_t ms);
void     dogStart(uint8_t mode);
void     dogStop(const char *why, bool withBeacon);
void     beaconCancel();
void     serviceBeacon();
void     serviceDog();

/* ---------------------------------------------------------------------------
 *  setup()
 * -------------------------------------------------------------------------*/
void setup()
{
  pinMode(LEFT_MOTOR_GO, OUTPUT);
  pinMode(LEFT_MOTOR_BACK, OUTPUT);
  pinMode(RIGHT_MOTOR_GO, OUTPUT);
  pinMode(RIGHT_MOTOR_BACK, OUTPUT);
  pinMode(LEFT_MOTOR_EN, OUTPUT);
  pinMode(RIGHT_MOTOR_EN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  // 注意：**不要动 Timer0 的预分频**（TCCR0B）。
  // D5/D6 的 PWM 就跑在 Timer0 上，预分频同时是 millis()/delay() 的时基。
  // 这里只需要 pinMode + analogWrite，频率固定 977Hz。

#if DEBUG_SERIAL
  Serial.begin(9600);
  loadCal();                     // 先把 EEPROM 里的左右增益读回来，再打印横幅
  Serial.println(F("=============================================="));
  Serial.println(F(" DirSpeedTest：降速 + 起步防打滑"));
  Serial.print (F("   全局速度="));      Serial.print(speedPermille / 10);
  Serial.print (F("."));                 Serial.print(speedPermille % 10);
  Serial.println(F("%"));
  Serial.print (F("   前进 duty=")); Serial.println(scaleSpeed(DUTY_BASE));
  Serial.print (F("   后退 duty=")); Serial.println(scaleSpeed(DUTY_BACK));
  Serial.print (F("   转弯 duty=")); Serial.println(scaleSpeed(turnDuty));
  Serial.print (F("   旋转 duty=")); Serial.println(scaleSpeed(DUTY_SPIN));
  Serial.print (F("   起步曲线 kick=")); Serial.print(DUTY_KICK);
  Serial.print (F("  hold="));          Serial.print(KICK_HOLD_MS);
  Serial.print (F("ms  accel="));       Serial.print(ACCEL_PER_SEC);
  Serial.println(F("/s"));
#if RAMP_ON
  Serial.print (F("   按住加速 从 ")); Serial.print(RAMP_START_PCT);
  Serial.print (F("% 爬到 100%（上限 ")); Serial.print(scaleSpeed(DUTY_BASE));
  Serial.print (F("），用时 ")); Serial.print(RAMP_FULL_MS);
  Serial.println(F("ms"));
#else
  Serial.println(F("   按住加速 已关闭（按下即巡航速度）"));
#endif
  Serial.print (F("   死区补偿 trim: 左=")); Serial.print(TRIM_LEFT);
  Serial.print (F("  右="));            Serial.println(TRIM_RIGHT);
  Serial.print (F("   增益 gain:     左=")); Serial.print(gainLeft);
  Serial.print (F("  右="));            Serial.println(gainRight);
  Serial.print (F("   EN 交叉="));       Serial.println(SWAP_EN ? F("是(左轮用D6/右轮用D5)") : F("否"));
  Serial.print (F("   抬手即停="));
#if DEADMAN_ADAPT
  Serial.print(F("自适应 1.35×帧间隔，起始阈值 "));
  Serial.print(deadmanMs);
  Serial.println(F("ms"));
#else
  Serial.print(DEADMAN_FIXED_MS);
  Serial.println(F("ms（固定阈值）"));
#endif
  Serial.println(F(" 按 CH=前进  +=后退  >>|=停车"));
#if DOG_ENABLED
  Serial.println(F("   自动模式：遛狗=数字0 比赛=数字2（关=1/3）  串口 G/P"));
  Serial.println(F("   蜂鸣器 A3：启动两声上行 / 结束一声低 / 收集完成三声"));
  Serial.println(F("   舵机 D12=云台180   D2=左360   D7=右360   超声 Trig=A0 Echo=A1"));
  Serial.print (F("   比赛扫描 "));
  Serial.print (F("9 档/15 度  遛狗 5 档/30 度"));
  Serial.print (F("   左右判定="));  Serial.println(angleReversed ? F("反向(角度小=左)") : F("正向(角度大=右)"));
  Serial.print (F("   旋转角速度=")); Serial.print(turnDps);
  Serial.print (F(" deg/s（一圈 ")); Serial.print(beaconOneTurnMs());
  Serial.println(F("ms）  串口 U<K值> 调，K 试一圈"));
#endif
  Serial.println(F("=============================================="));
#endif

  /* ---- 遛狗模式的外设：超声波 + 舵机 ---- */
  pinMode(US_TRIG, OUTPUT);
  pinMode(US_ECHO, INPUT);
  digitalWrite(US_TRIG, LOW);

#if DOG_ENABLED
  /* 云台一个舵机（D12）。D2/D7 已改成 360° 连续旋转舵机，不在这里管。 */
  for (uint8_t i = 0; i < SERVO_N; i++) { dogServos[i].attach(SERVO_PINS[i]); }
  dogServoWrite(90);     // 上电先回中：装探头、对方向时它不该自己扭走
  delay(250);            // 等它转到位（这点开机时间没有影响）
#endif

  /* 两颗 360° 舵机：**无论遛狗模式开没开都要接管并停住**。
   * 不给信号时它们是"自由"的，一碰就转，装车时会很别扭。 */
  srv360L.attach(PIN_S360_L);
  srv360R.attach(PIN_S360_R);
  s360Brake();

  /* 蜂鸣器：普通数字输出，靠 micros() 软件翻转发声。
   * ★ 不能用 tone()：它占 Timer2，而 IRremote 也用 Timer2，一响红外就废。 */
  pinMode(PIN_BUZZ, OUTPUT);
  digitalWrite(PIN_BUZZ, LOW);

  brakeCar();          // 上电先确保电机停着
  irrecv.enableIRIn(); // 红外接收开始（占用 Timer2，与 Timer0 的 PWM 互不影响）
}

/* ---------------------------------------------------------------------------
 *  单个轮子：方向 + 占空比
 * -------------------------------------------------------------------------*/
void setOneWheel(uint8_t pinGo, uint8_t pinBack, uint8_t pinEn,
                 int8_t dir, bool goHighOnForward, uint8_t duty)
{
  if (dir == DIR_STOP)
  {
    digitalWrite(pinGo, LOW);
    digitalWrite(pinBack, LOW);
    analogWrite(pinEn, 0);
    return;
  }

  bool forward  = (dir == DIR_FORWARD);
  bool goIsHigh = forward ? goHighOnForward : !goHighOnForward;

  digitalWrite(pinGo,   goIsHigh ? HIGH : LOW);
  digitalWrite(pinBack, goIsHigh ? LOW  : HIGH);
  analogWrite(pinEn, duty);
}

/* ---------------------------------------------------------------------------
 *  算一侧的最终占空比：死区补偿（offset）+ 增益补偿（scale）
 * ---------------------------------------------------------------------------
 *  cmd    : 当前指令占空比（就是爬升曲线上的那个数）
 *  trim   : 死区补偿量（谁先动）
 *  gain   : 增益（256 = 1.0 倍；谁转得快）
 *  target : 本次动作的巡航占空比（前进 120 / 后退 96 / 旋转 108）
 *
 *  处理顺序：先加死区补偿 → 再乘增益。两件事分开，互不干扰。
 *
 *      补偿量
 *   trim ┤██████████╮
 *        │           ╲
 *        │            ╲
 *        0 ┴────────────╲─────────► 指令占空比
 *        0        FADE_START   目标(target)
 *
 *  要点：
 *    ① FADE_START 必须高于两侧阈值（默认 95 > 73 > 60）。
 *       如果从 kick 就淡出，左轮在指令 60 只拿到 73，刚好卡在阈值上，
 *       阈值差再大一点就又会晚一步 —— 所以先给满。
 *    ② 淡出终点是**本次动作的目标值**，不是写死的 120。
 *       倒车目标 96、旋转目标 108 都低于 120，若终点固定 120，
 *       这两个动作永远淡不完，补偿会一直加着 → 持续跑偏。
 *    ③ 增益只在巡航时起作用（死区补偿已经淡出了），所以调 GAIN 不会
 *       影响起步同步；调 TRIM 也不会影响巡航直线。
 *       ★ 这条靠 buildDuty() 里"增益也按 cmd 淡入"来保证。上一版漏了这层
 *       淡入，GAIN 直接乘在起步段，结果右轮被压低 8 个 duty 才起转，
 *       表现为"左轮先动、车往右斜"（2026-11-16 实测）。改这里务必保留淡入。
 * -------------------------------------------------------------------------*/
uint8_t buildDuty(uint8_t cmd, uint8_t trim, uint16_t gain, uint8_t target)
{
  if (cmd == 0) { return 0; }

  // 用 32 位：gain 允许超过 256（把慢的那侧调快），
  // duty(≤295) × 400 会超出 uint16，用 16 位会溢出回绕成小值。
  uint32_t duty = cmd;

  // 淡出终点 = 本次动作的目标值。倒车 96、旋转 108 都低于 120，
  // 若终点写死 120，这两个动作永远淡不完，补偿会一直加着 → 持续跑偏。
  const uint8_t fadeEnd = (target > COMP_FADE_START) ? target : COMP_FADE_START;

  // ---- ① 死区补偿 ----
  if (trim != 0)
  {
    if (cmd <= COMP_FADE_START)
    {
      duty = (uint16_t)cmd + trim;                       // 全额补
    }
    else if (cmd < fadeEnd)
    {
      duty = (uint16_t)cmd
           + (uint16_t)trim * (uint16_t)(fadeEnd - cmd)
             / (uint16_t)(fadeEnd - COMP_FADE_START);    // 线性淡出
    }
    // cmd >= fadeEnd：不补
  }

  /* ---- ② 增益补偿 ------------------------------------------------------
   * ★ 必须和 ① 一样按 cmd 淡入，否则就是 2026-11-16 那次"左轮先动"的元凶：
   *
   *   TRIM 是**加法**（cmd + 13，把左轮门槛 73 拉平到右轮的 60），
   *   GAIN 是**乘法**（右轮 × 225/256）。
   *   两个一起上，右轮被压低 → 右轮要更高的 cmd 才起转：
   *       左轮 cmd ≥ 60 起转，右轮 cmd ≥ 68 起转 → 左轮早 8 个 duty，
   *   ACCEL 300/s 下就是约 27ms，肉眼可见"左轮先转、车往右斜"。
   *
   *   上方注释 ③ 早就写明"增益只在巡航时起作用，调 GAIN 不会影响起步同步"，
   *   但原实现无条件乘，根本没做这件事 —— 这里补齐：
   *       cmd ≤ COMP_FADE_START   → 不压（g = 256），起步段只允许 TRIM 说话
   *       cmd ≥ fadeEnd           → 全额按 GAIN 压，巡航行为完全不变
   *       中间                    → 线性过渡，不留突变点
   *
   *   注意 g 可以是负数方向（gain < 256 时是压），所以用 int32_t 算。
   * ------------------------------------------------------------------- */
  if (gain != 256)
  {
    int32_t g = (int32_t)gain;

    if (cmd <= COMP_FADE_START)
    {
      g = 256;                                   // 起步段：不参与
    }
    else if (cmd < fadeEnd)
    {
      const int32_t span = (int32_t)fadeEnd - (int32_t)COMP_FADE_START;
      if (span > 0)
      {
        g = 256 + ((int32_t)gain - 256)
                * ((int32_t)cmd - (int32_t)COMP_FADE_START) / span;
      }
    }

    if (g != 256 && g > 0) { duty = duty * (uint16_t)g / 256; }
  }

  return (duty > 255) ? 255 : (uint8_t)duty;
}

/* 把当前方向 + 当前占空比（含左右补偿）写到硬件 */
void applyOutput()
{
#if GAIN_STRAIGHT_ONLY
  // 只有两轮同向（前进/后退）才算直行；单边转弯、原地旋转不套 GAIN，
  // 否则旋转会画弧、左右转弯半径不一致。
  const bool straight = (wheelLeft == wheelRight && wheelLeft != DIR_STOP);
  const uint16_t gL = straight ? gainLeft  : (uint16_t)256;
  const uint16_t gR = straight ? gainRight : (uint16_t)256;
#else
  const uint16_t gL = gainLeft;
  const uint16_t gR = gainRight;
#endif

  const uint8_t dutyL = buildDuty(currentDuty, TRIM_LEFT,  gL, targetDuty);
  const uint8_t dutyR = buildDuty(currentDuty, TRIM_RIGHT, gR, targetDuty);

  setOneWheel(LEFT_MOTOR_GO,  LEFT_MOTOR_BACK,  LEFT_MOTOR_EN,
              wheelLeft,  false, wheelLeft  == DIR_STOP ? 0 : dutyL);
  setOneWheel(RIGHT_MOTOR_GO, RIGHT_MOTOR_BACK, RIGHT_MOTOR_EN,
              wheelRight, true,  wheelRight == DIR_STOP ? 0 : dutyR);
}

/* ---------------------------------------------------------------------------
 *  下发一个动作
 * ---------------------------------------------------------------------------
 *  duty 是"巡航占空比"，不会立刻生效 —— 由 serviceRamp() 从起步曲线爬上去。
 *
 *  ★ 换向（或从静止重新起步）时把 currentDuty 清零：
 *    这样 direction 脚翻转的瞬间 ENA=0（桥臂关断、电机不带电），
 *    既避免了反接冲击电流，也让"重新起步"必然走一遍防打滑曲线。
 * -------------------------------------------------------------------------*/
void setWheels(int8_t left, int8_t right, uint8_t duty)
{
  if (left == DIR_STOP && right == DIR_STOP)
  {
    brakeCar();
    return;
  }

  const bool dirChanged  = (left != wheelLeft) || (right != wheelRight);
  const bool freshPress  = dirChanged || !moving;   // 新的一次按下（含换动作）

  if (dirChanged)
  {
    currentDuty = 0;     // 换向先断电，再重新爬
    holdUntilMs = 0;
  }
  if (freshPress) { holdStartMs = nowMs(); holding = true; }   // 按住计时从这里重新开始

  wheelLeft  = left;
  wheelRight = right;
  moving     = true;
  wheelCmdDuty = duty;                 // 记住标称值：改全局速度时要用它重算

  /* ★ 遛狗模式不走「按住加速」曲线。
   *   原因：曲线起点是 RAMP_START_PCT（55%），而遛狗前进档的标称值只有
   *   DUTY_BASE×70% = 84，55% 只有 46 —— 低于左轮门槛 73、右轮门槛 60，
   *   于是每段起步的几百毫秒里两个轮子根本不转，表现为"启动了却不动"。
   *   再加上一段只走 700ms，2000ms 的曲线永远爬不到顶，速度一直上不去。
   *   这里直接给满，靠 TRIM/GAIN 的直线补偿控制方向就够。 */
#if DOG_ENABLED
  const bool dogActive = dogOn || (beaconPhase != 0);
#else
  const bool dogActive = false;
#endif
  targetDuty   = scaleSpeed(dogActive ? duty : rampedDuty(duty));

  lastCommandMs = nowMs();

  applyOutput();   // 方向立刻生效；占空比交给 serviceRamp() 爬向 targetDuty
}

/* ---------------------------------------------------------------------------
 *  停车 = 真刹车（四个 IN 全低 + 使能给满 → 电机两端短路）
 * -------------------------------------------------------------------------*/
void brakeCar()
{
  wheelLeft   = DIR_STOP;
  wheelRight  = DIR_STOP;
  moving      = false;
  targetDuty  = 0;
  currentDuty = 0;
  wheelCmdDuty = 0;
  holdUntilMs = 0;
  holdStartMs = 0;      // 松手 → 计时清零，下次按下从低速重新爬
  holding     = false;

  digitalWrite(LEFT_MOTOR_GO, LOW);
  digitalWrite(LEFT_MOTOR_BACK, LOW);
  digitalWrite(RIGHT_MOTOR_GO, LOW);
  digitalWrite(RIGHT_MOTOR_BACK, LOW);
  analogWrite(LEFT_MOTOR_EN, 255);
  analogWrite(RIGHT_MOTOR_EN, 255);
}

/* ---------------------------------------------------------------------------
 *  按住加速：目标占空比随着「这一次按住」的时长线性上升
 * ---------------------------------------------------------------------------
 *  baseDuty 是动作的巡航目标（前进 120 / 后退 96）。
 *      按住 0ms            → base × RAMP_START_PCT%（120 → 78，慢速蠕行）
 *      按住 RAMP_FULL_MS   → base 封顶（"不快过某个速度"就是这个 base）
 *
 *  ★ 它只改「目标」，不改「当前」：真正爬升的斜率仍由下面的起步曲线负责，
 *    所以 0~0.2 秒那一段防打滑不会被破坏，两条曲线是串联的。
 *  ★ holdStartMs 只在「新按下 / 换动作」时重置，重发帧不会重置 ——
 *    否则每 200ms 来一帧，速度永远爬不上去。
 * -------------------------------------------------------------------------*/
uint8_t rampedDuty(uint8_t baseDuty)
{
#if !RAMP_ON
  return baseDuty;
#else
  if (baseDuty == 0) { return 0; }

  uint16_t startDuty = (uint16_t)baseDuty * RAMP_START_PCT / 100;
  if (startDuty < DUTY_KICK) { startDuty = DUTY_KICK; }
  if (startDuty >= baseDuty) { return baseDuty; }        // 参数配歪时别算出怪值

  if (!holding) { return (uint8_t)startDuty; }

  const uint32_t held = nowMs() - holdStartMs;
  if (held >= (uint32_t)RAMP_FULL_MS) { return baseDuty; }

  const uint16_t span = (uint16_t)(baseDuty - startDuty);
  return (uint8_t)(startDuty + (uint16_t)((uint32_t)span * held / RAMP_FULL_MS));
#endif
}

/* 每拍刷新目标值（在 serviceRamp 的最前面调）。
 * 转弯 / 原地旋转不参与曲线：它们的 duty 本来就贴着起转门槛（约 80），
 * 再压到 65% 会「按了不转、松手才动」，手感反而更差。 */
void refreshRampedTarget()
{
  if (!moving || wheelCmdDuty == 0) { return; }

#if RAMP_ON
  const bool straight = (wheelLeft == wheelRight && wheelLeft != DIR_STOP);
  if (!straight) { return; }
#endif

  targetDuty = scaleSpeed(rampedDuty(wheelCmdDuty));
}

/* ---------------------------------------------------------------------------
 *  起步加速曲线（防打滑）—— 这是本程序的核心
 * ---------------------------------------------------------------------------
 *  把 "0 → 巡航占空比" 拆成三段，避免电机一上来就输出最大扭矩把轮子搓空：
 *
 *      ① 起步：currentDuty 从 0 直接给到 DUTY_KICK（够转、但扭矩不大）
 *      ② 保持：KICK_HOLD_MS 内不动，让两个轮子同步咬合
 *      ③ 爬升：之后按 ACCEL_PER_SEC 匀速加，直到 targetDuty
 *
 *  下降（收油 / 减速）走另一条斜率 DECEL_PER_SEC，比加速快，
 *  这样松手或者换向时车不会"迟钝"。
 *
 *  函数每 RAMP_TICK_MS 更新一次，并且用"实际经过的时间"算步进，
 *  所以即使 loop() 被串口打印之类的操作拖慢，曲线也不会变形。
 * -------------------------------------------------------------------------*/
void serviceRamp()
{
#if DOG_ENABLED
  /* 遛狗模式：指令即到位，不爬坡。
   * 不做 kick、不做 hold、不做手按加速 —— 走起来就是满速，
   * 刹车也走 applyOutput 的 normal 路径。*/
  if (dogOn || beaconPhase != 0)
  {
    if (currentDuty != targetDuty)
    {
      currentDuty = targetDuty;
      holdUntilMs = 0;
      applyOutput();
    }
    return;
  }
#endif

  refreshRampedTarget();          // 按住越久，目标值越高（封顶在 DUTY_BASE）

  if (currentDuty == targetDuty) { return; }

  const uint32_t t = nowMs();

  // ---- ① 从静止起步：直接给"起步占空比" ---------------------------------
  if (currentDuty == 0 && targetDuty > 0)
  {
    currentDuty = (targetDuty < DUTY_KICK) ? targetDuty : DUTY_KICK;
    holdUntilMs = t + KICK_HOLD_MS;
    rampMs      = t;
    applyOutput();
#if DEBUG_SERIAL
    Serial.print(F("  [起步] kick="));
    if (dogOn) { Serial.println(F("(dog)")); } else { Serial.println(currentDuty); }
#endif
    return;
  }

  // ---- ② 起步保持期：占空比按住不动 -------------------------------------
  if (holdUntilMs != 0)
  {
    if ((int32_t)(t - holdUntilMs) < 0) { return; }   // 还没到时间
    holdUntilMs = 0;
    rampMs = t;
    return;                                            // 这一拍只结束保持，下一拍开始爬
  }

  // ---- ③ 按斜率爬升 / 下降 ----------------------------------------------
  if (t - rampMs < (uint32_t)RAMP_TICK_MS) { return; }
  const uint32_t dt = t - rampMs;    // 实际间隔（毫秒），保证曲线不随时间漂移
  rampMs = t;

  if (currentDuty < targetDuty)
  {
    uint32_t step = (uint32_t)ACCEL_PER_SEC * dt / 1000;
    if (step == 0) { step = 1; }                     // 至少挪一格，避免卡住
    uint16_t next = (uint16_t)currentDuty + (uint16_t)step;
    currentDuty = (next > targetDuty) ? targetDuty : (uint8_t)next;
  }
  else
  {
    uint32_t step = (uint32_t)DECEL_PER_SEC * dt / 1000;
    if (step == 0) { step = 1; }
    uint16_t next = (currentDuty > step) ? (uint16_t)(currentDuty - step) : 0;
    currentDuty = (next < targetDuty) ? targetDuty : (uint8_t)next;
  }

  applyOutput();

#if DEBUG_SERIAL
  /* 遛狗模式每拍都在微调速度，这条会刷满屏幕 —— 那时闭嘴。 */
  if (currentDuty == targetDuty)
  {
#if DOG_ENABLED
    if (!dogOn)
#endif
    {
      Serial.print(F("  [到位] duty="));
      Serial.println(currentDuty);
    }
  }
#endif
}

/* ---------------------------------------------------------------------------
 *  LED / 超时停车
 * -------------------------------------------------------------------------*/
void pulseLed()
{
  digitalWrite(LED_PIN, HIGH);
  ledOn = true;
  ledOffAtMs = nowMs() + LED_PULSE_MS;
}

void serviceLed()
{
  if (ledOn && (long)(nowMs() - ledOffAtMs) >= 0)
  {
    digitalWrite(LED_PIN, LOW);
    ledOn = false;
  }
}

/* 用实测的帧间隔更新阈值：App 每 200ms 发一帧，走的就是这里。
 * 只采信 100~2000ms 的间隔 —— 小于 100 的是同一次按下的紧凑重发，
 * 大于 2000 的是"歇了一会儿又按"，都不该拿来当基准。 */
void noteFrameGap(uint32_t gap)
{
#if DEADMAN_ADAPT
  if (gap < 100 || gap > 2000) { return; }
  frameGapAvg = (uint16_t)(((uint32_t)frameGapAvg * 3 + gap) / 4);   // 一阶低通，抗单次抖动
  uint32_t th = (uint32_t)frameGapAvg * DEADMAN_PCT / 100;
  if (th < DEADMAN_MIN_MS) { th = DEADMAN_MIN_MS; }
  if (th > DEADMAN_MAX_MS) { th = DEADMAN_MAX_MS; }
  deadmanMs = (uint16_t)th;
#else
  (void)gap;
#endif
}

void serviceAutoStop()
{
  if (!moving) { return; }

  const uint16_t limit = DEADMAN_ADAPT ? deadmanMs : (uint16_t)DEADMAN_FIXED_MS;
  if (limit == 0) { return; }                       // 0 = 关掉抬手即停

  const uint32_t late = nowMs() - lastCommandMs;
  if (late > (uint32_t)limit)
  {
  #if DEBUG_SERIAL
    Serial.print(F("[超时] 松手 "));
    Serial.print(late);
    Serial.print(F("ms（阈值 "));
    Serial.print(limit);
    Serial.println(F("ms）→ 自动停车"));
  #endif
    brakeCar();
  }
}

uint32_t nowMs()
{
  return (uint32_t)millis();
}

/* ===========================================================================
 *  遛狗模式 —— 实现
 * ---------------------------------------------------------------------------
 *  设计要点：全程非阻塞。每个 case 只推进一步就返回，所以红外解码、
 *  加速曲线、LED 都在同一拍里正常跑 —— 按一下遥控器就能立刻接管。
 * =========================================================================*/
#if DOG_ENABLED

#define DOG_IDLE   0
#define DOG_SCAN   1   // 把舵机转到 dogIdx 那一档
#define DOG_MEAS   2   // 同一档连采 N 次（等舵机稳住 → 采 → 间隔 → 采…）
#define DOG_PICK   3   // 分析这一轮，选目标
#define DOG_TURN   4   // 原地旋转中
#define DOG_PAUSE  5   // 刹停后等车身晃完
#define DOG_GO     6   // 前进中
#define DOG_BEACON 7   // 启动提示：正在播放启动音，播完才开始扫描（不再转圈）
#define DOG_TURN_AWAY 8  // 只给 dogAfter 用：停稳后转 180° 换方向，不是真实阶段
#define DOG_TURN_AROUND 9 // 只给 dogAfter 用：比赛模式收集完成，停稳后响一声 + 转 180°
                          //   ★ 必须和 DOG_SCAN 分开，否则 PAUSE 分不清
                          //   "刚走完要换向" 和 "刚转完该扫描"，会死循环原地转。

void dogServoWrite(uint8_t deg)
{
  for (uint8_t i = 0; i < SERVO_N; i++) { dogServos[i].write(deg); }
}

/* 一次测距，单位 cm，0 = 没有回波。
 * 25ms 超时 ≈ 4.3m（HC-SR04 的极限）。小于 4cm 一律丢掉 ——
 * 那是发射余波直接耦合进接收头的假值，不是真的贴脸。 */
uint16_t dogMeasure()
{
  digitalWrite(US_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(US_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(US_TRIG, LOW);

  const unsigned long us = pulseIn(US_ECHO, HIGH, 25000UL);
  if (us == 0) { return 0; }

  const uint16_t cm = (uint16_t)(us / 58UL);
  if (cm < 4) { return 0; }
  return cm;
}

/* 这一档有没有东西在动 —— 由「单轮内多次采样」在 MEAS 阶段算好，直接读结果。
 * 判定条件：同一角度连续几次读数的「最大 - 最小」≥ DOG_MOVE_CM。
 * 车全程静止，所以这点波动只可能来自被照到的东西自己动了。 */
bool dogIsMoving(uint8_t i) { return dogMove[i]; }

/* 选目标：先在"在动的"里挑最近的，没有就在全部里挑最近的 */
/* 某一档的「有效目标」最小距离。
 * 侧边档位用更高的门槛，挡掉车体自身的回波（见 DOG_EDGE_CM 的说明）。 */
uint16_t dogMinCmFor(uint8_t i)
{
  const int16_t off = (int16_t)scanAngles[i] - 90;
  const int16_t mag = (off < 0) ? (int16_t)(-off) : off;
  return (mag >= DOG_EDGE_DEG) ? (uint16_t)DOG_EDGE_CM : (uint16_t)DOG_MIN_CM;
}

void dogPick()
{
  int8_t   bm  = -1;
  uint16_t bmd = 0xFFFF;
  int8_t   bs  = -1;
  uint16_t bsd = 0xFFFF;

  for (uint8_t i = 0; i < scanN; i++)
  {
    const uint16_t d = dogProf[i];
    if (d == 0 || d < dogMinCmFor(i)) { continue; }

    /* 比赛模式不看"动没动" —— 物块是死的，而且小到会钻进底盘盲区，
     * 只按"最近"来选。遛狗模式才优先挑会动的东西。 */
    if (dogMode != MODE_RACE && dogIsMoving(i) && d < bmd) { bm = (int8_t)i; bmd = d; }
    if (d < bsd)                                           { bs = (int8_t)i; bsd = d; }
  }

  if (bm >= 0)      { dogTargetIdx = bm; dogTargetMoving = true;  dogTargetCm = bmd; }
  else if (bs >= 0) { dogTargetIdx = bs; dogTargetMoving = false; dogTargetCm = bsd; }
  else              { dogTargetIdx = -1; dogTargetMoving = false; dogTargetCm = 0;   }
}

#if DEBUG_SERIAL
/* 打一轮扫描的地图：角度:距离，带 * 的是判为"在动"的档 */
void dogPrintMap()
{
  Serial.print(F("[DOG] "));
  for (uint8_t i = 0; i < scanN; i++)
  {
    Serial.print(scanAngles[i]);
    Serial.print(':');
    if (dogProf[i] == 0) { Serial.print(F("--")); }
    else                 { Serial.print(dogProf[i]); }
    if (dogIsMoving(i)) { Serial.print('*'); }
    /* ! = 读数存在但被"侧边门槛"挡掉（几乎都是车体自身零件，不是真目标） */
    if (dogProf[i] != 0 && dogProf[i] < dogMinCmFor(i)) { Serial.print('!'); }
    Serial.print(' ');
  }
  Serial.println();
}
#endif

/* 掉头（180°）需要多久。由 turnDps 换算，不写死 ——
 * 标定值一改，这里跟着变。上限 1.5s 只是防呆（turnDps 被设成 60 时会算出 3 秒）。 */
uint32_t turnMsFor(uint16_t deg)
{
  const uint16_t dps = turnDps ? turnDps : DOG_TURN_DPS;
  uint32_t ms = (uint32_t)deg * 1000UL / (uint32_t)dps;
  if (ms < DOG_TURN_MIN_MS) { ms = DOG_TURN_MIN_MS; }
  if (ms > 1500)            { ms = 1500; }
  return ms;
}

uint32_t dogTurn180Ms() { return turnMsFor(180); }

/* 原地旋转。after 决定转完去哪儿（对准目标 → DOG_GO；找目标 → DOG_SCAN）。
 * 用 DUTY_SPIN，和 App 圆盘左右是同一个动作、同一个占空比，走起来手感一致。 */
void dogTurn(uint32_t ms, int8_t side, uint8_t after)
{
  dogHoldMs  = ms;
  dogAfter   = after;
  dogPhaseMs = nowMs();
  dogPhase   = DOG_TURN;

  if (side < 0) { setWheels(DIR_FORWARD,  DIR_BACKWARD, DUTY_SPIN); }
  else          { setWheels(DIR_BACKWARD, DIR_FORWARD,  DUTY_SPIN); }

#if DEBUG_SERIAL
  Serial.print(F("[DOG] 转向 "));
  Serial.print(side < 0 ? F("左") : F("右"));
  Serial.print(' ');
  Serial.print(ms);
  Serial.println(F("ms"));
#endif
}

/* 前进一段。舵机必须先回中，否则测的不是车要走的那条路。 */
void dogGo(uint32_t ms)
{
  /* 从别的阶段进入 GO = 新的一串连续前进，纠偏计时从这里重新开始。
   * 连续段（dogStreak 累加）不会调用 dogGo，所以计时是累加的。 */
  if (dogPhase != DOG_GO) { dogGoStartMs = nowMs(); }

  dogPushDone = false;              // 每段重新计"多推"是否已触发
  dogHoldMs   = ms;
  dogPhaseMs = nowMs();
  dogCheckMs = nowMs();
  dogFrontCm = 0;
  dogPhase   = DOG_GO;

  dogServoWrite(90);

  const uint8_t d = (uint8_t)((uint16_t)DUTY_BASE * DOG_FWD_PCT / 100);
  setWheels(DIR_FORWARD, DIR_FORWARD, d);

#if DEBUG_SERIAL
  Serial.print(F("[DOG] 前进 duty="));
  Serial.print(scaleSpeed(d));
  Serial.print(F(" 直给(不走加速曲线)  已走 "));
  Serial.print((nowMs() - dogGoStartMs) / 1000);
  Serial.println(F("s"));
#endif
}

/* ---------------------------------------------------------------------------
 *  旋转提示：启动 / 结束的"回话"
 * ---------------------------------------------------------------------------
 *  非阻塞：只在 loop 里数时间，转圈期间照常解红外。
 *  任何按键都会先 beaconCancel()，所以它绝不会挡住你接管。
 * -------------------------------------------------------------------------*/

/* 转一圈要多久（ms）。由 turnDps 换算，和「转向对准」共用同一个基准 ——
 * 以前这里写死 2800ms，跟 DPS 差了三四倍，是「说转一圈、实际转好几圈」的根源。 */
uint32_t beaconOneTurnMs()
{
  const uint16_t dps = turnDps ? turnDps : DOG_TURN_DPS;
  return 360000UL / (uint32_t)dps;
}

/* 真正驱动电机的那层。非阻塞（只记截止时刻，由 serviceBeacon 收尾），
 * 所以标定测试命令和提示转圈共用它，按任意键都能立刻掐停。 */
void spinFor(int8_t side, uint32_t ms)
{
  beaconPhase = (side < 0) ? 1 : 2;      // 先置位：setWheels 看到它就不走加速曲线
  beaconUntil = nowMs() + ms;

  if (side < 0) { setWheels(DIR_FORWARD,  DIR_BACKWARD, DOG_BEACON_DUTY); }
  else          { setWheels(DIR_BACKWARD, DIR_FORWARD,  DOG_BEACON_DUTY); }

#if DEBUG_SERIAL
  /* ASCII 的 L/R 一起打：串口终端编码不对时中文会变问号，
   * 有了 L/R 至少能确认方向（排查"几乎不左转"时吃过这个亏）。 */
  Serial.print(F("[DOG] 原地旋转 "));
  Serial.print(side < 0 ? F("左 L") : F("右 R"));
  Serial.print(' ');
  Serial.print(ms);
  Serial.println(F("ms"));
#endif
}

void beaconStart(int8_t side, uint32_t ms)
{
#if DOG_BEACON_ON
  spinFor(side, ms);
#else
  (void)side;
  (void)ms;
#endif
}

/* 立刻中断提示旋转（按键接管时用），不让它继续占着电机 */
void beaconCancel()
{
  if (beaconPhase == 0) { return; }

  beaconPhase = 0;
  brakeCar();

#if DEBUG_SERIAL
  Serial.println(F("[DOG] 提示旋转被中断"));
#endif
}

/* 每拍调用：到点就停 */
void serviceBeacon()
{
  if (beaconPhase == 0) { return; }
  if ((int32_t)(nowMs() - beaconUntil) < 0) { return; }

  beaconPhase = 0;
  brakeCar();
}

void dogStart(uint8_t mode)
{
  beaconCancel();                 // 万一上一条提示还没转完
  buzzStop();

  dogMode         = mode;
  dogOn           = true;
  dogStartMs      = nowMs();
  dogPhaseMs      = nowMs();
  dogIdx          = 0;
  dogLost         = 0;
  dogStreak       = 0;
  dogTargetIdx    = -1;
  dogTargetMoving = false;
  dogTargetCm     = 0;

  /* 按模式选扫描表：比赛 9 档 15°（物块小，要对准），遛狗 5 档 30°（要快）。 */
  if (mode == MODE_RACE)
  {
    scanAngles   = RACE_ANGLES;
    scanN        = (uint8_t)(sizeof(RACE_ANGLES) / sizeof(RACE_ANGLES[0]));
    scanSettleMs = RACE_SETTLE_MS;
    scanSamples  = RACE_SAMPLE_N;
    scanGapMs    = RACE_SAMPLE_GAP_MS;
  }
  else
  {
    scanAngles   = DOG_ANGLES;
    scanN        = (uint8_t)(sizeof(DOG_ANGLES) / sizeof(DOG_ANGLES[0]));
    scanSettleMs = DOG_SCAN_SETTLE_MS;
    scanSamples  = DOG_SAMPLE_N;
    scanGapMs    = DOG_SAMPLE_GAP_MS;
  }

  // 历史清空 → 第一轮没有可比对象，全部按静止处理，直接去最近的
  for (uint8_t i = 0; i < SCAN_MAX_N; i++) { dogProf[i] = 0; dogMove[i] = false; }

  dogServoWrite(90);

#if DEBUG_SERIAL
  Serial.println(mode == MODE_RACE ? F("[DOG] ===== 比赛模式 启动 =====")
                                   : F("[DOG] ===== 遛狗模式 启动 ====="));
  Serial.print(F("[DOG] 前进 duty="));
  Serial.print(scaleSpeed((uint8_t)((uint16_t)DUTY_BASE * DOG_FWD_PCT / 100)));
  if (mode == MODE_RACE)
  {
    Serial.print(F("  贴近 "));
    Serial.print(RACE_PUSH_CM);
    Serial.print(F("cm 后多推 "));
    Serial.print(RACE_OVERRUN_MS);
    Serial.print(F("ms，然后 180 度换向"));
    Serial.print(F("  扫描 "));
    Serial.print(scanN);
    Serial.println(F(" 档 / 15 度"));
  }
  else
  {
    Serial.print(F("  停止距离 "));
    Serial.print(DOG_STOP_CM);
    Serial.print(F("cm  移动判定 "));
    Serial.print(DOG_MOVE_CM);
    Serial.print(F("cm  扫描 "));
    Serial.print(scanN);
    Serial.println(F(" 档 / 30 度"));
  }
#endif

  /* ★ 启动提示改成**蜂鸣**，不再原地转圈（2026-11-17 用户要求）。
   * DOG_BEACON 阶段保留，作用是「先等提示音播完再开始扫描」——
   * 边响边动会让人分不清它在响还是在走。 */
  dogPhase   = DOG_BEACON;
  dogHoldMs  = buzzPlay(SEQ_START, SEQ_START_N);
  dogPhaseMs = nowMs();
}

void dogStop(const char *why, bool withBeacon)
{
  if (!dogOn) { return; }

  const uint32_t ran = nowMs() - dogStartMs;

  dogOn    = false;
  dogMode  = 0;
  dogPhase = DOG_IDLE;
  brakeCar();
  dogServoWrite(90);

#if DEBUG_SERIAL
  Serial.print(F("[DOG] 停止："));
  Serial.println(why);
  Serial.print(F("[DOG] 共跑了 "));
  Serial.print(ran / 1000);
  Serial.println(F(" 秒"));
#else
  (void)why;
  (void)ran;
#endif

  /* 结束提示改成一声低音（原来是右转一圈）。
   * 手动接管那种情况不响：用户下一帧就要开车，提示音只会干扰。
   * ★ 这里不能调 tone() —— 它占 Timer2，会直接掐掉红外接收。 */
  if (withBeacon) { buzzPlay(SEQ_STOP, SEQ_STOP_N); }
}

void serviceDog()
{
  if (!dogOn) { return; }

  const uint32_t t = nowMs();

  if ((uint32_t)(t - dogStartMs) >= DOG_MAX_MS) { dogStop("硬超时"); return; }

  switch (dogPhase)
  {
    // ---- 启动提示：等左旋转完再开始扫描 ----------------------------------
    case DOG_BEACON:
      /* 只是等启动音播完（dogHoldMs 由 buzzPlay 的返回值给出），
       * 播完直接进扫描。不再转圈 —— 用户要求启动/结束都用蜂鸣提示。 */
      if ((uint32_t)(t - dogPhaseMs) >= dogHoldMs)
      {
        dogIdx     = 0;
        dogPhase   = DOG_SCAN;
        dogPhaseMs = t;
      }
      break;

    // ---- 把舵机转到这一档 ------------------------------------------------
    case DOG_SCAN:
      dogServoWrite(scanAngles[dogIdx]);
      dogSmpIdx = 0; dogSmpMin = 0; dogSmpMax = 0; dogSmpOk = 0;
      dogPhaseMs = t;
      dogPhase   = DOG_MEAS;
      break;

    // ---- 同一档连采 DOG_SAMPLE_N 次，记录最小 / 最大值 ---------------------
    case DOG_MEAS:
    {
      /* 第一次之前要等舵机停稳；之后每两次之间只等采样间隔。
       * 全程非阻塞（每次只采一发就退出），所以按键随时能接管。 */
      const uint32_t wait = (dogSmpIdx == 0) ? scanSettleMs : scanGapMs;
      if ((uint32_t)(t - dogPhaseMs) < wait) { break; }

      const uint16_t d = dogMeasure();
      if (d != 0)
      {
        if (dogSmpOk == 0 || d < dogSmpMin) { dogSmpMin = d; }
        if (d > dogSmpMax)                  { dogSmpMax = d; }
        dogSmpOk++;
      }

      dogPhaseMs = t;
      dogSmpIdx++;

      if (dogSmpIdx < scanSamples) { break; }   // 本档还没采够，下一拍接着采

      // ---- 本档采完：落库 + 判「有没有东西在动」----
      dogProf[dogIdx] = dogSmpMin;               // 距离取最小值（余波只会让读数偏小）
      /* 有效读数不足 2 个就没法比，一律当静止 —— 宁可漏判，也别把噪声当目标追。
       * 满足 2 个且「最大-最小 ≥ DOG_MOVE_CM」→ 这个方向有东西在动。 */
      dogMove[dogIdx] = (dogSmpOk >= 2) &&
                        ((uint16_t)(dogSmpMax - dogSmpMin) >= DOG_MOVE_CM);

      if (dogSmpOk == 0) { if (dogLost < 255) { dogLost++; } }
      else               { dogLost = 0; }

      dogSmpIdx = 0; dogSmpMin = 0; dogSmpMax = 0; dogSmpOk = 0;
      dogIdx++;
      dogPhase = (dogIdx < scanN) ? DOG_SCAN : DOG_PICK;
      break;
    }

    // ---- 选目标 ----------------------------------------------------------
    case DOG_PICK:
    {
    #if DEBUG_SERIAL
      dogPrintMap();
    #endif
      dogPick();
      dogIdx = 0;

      if (dogLost >= DOG_LOST_MAX) { dogStop("探头无回波"); return; }

      if (dogTargetIdx < 0)
      {
        dogStreak = 0;

        /* ★ 比赛模式：这一圈什么都没有 → **直走**，不要原地转。
         *   原来固定右转 420ms，空场时会一直顺时针打转（用户看到的"只会右转"
         *   有一半来自这里）。空场本来就该往前开去碰运气。 */
        if (dogMode == MODE_RACE)
        {
          dogAfter = DOG_SCAN;
          dogGo(RACE_GO_MS);
        #if DEBUG_SERIAL
          Serial.println(F("[RACE] 空场，直走一段再扫"));
        #endif
          break;
        }

        // 遛狗：没有目标时转开一段再找（追人的场景不适合盲目前冲）
        dogTurn(DOG_ESCAPE_MS, +1, DOG_SCAN);
        break;
      }

      const int16_t off = (int16_t)scanAngles[dogTargetIdx] - 90;
      const int16_t mag = (off < 0) ? (int16_t)(-off) : off;

      /* 追到了：正前方且已经贴到 DOG_STOP_CM(5cm) 以内
       * → 整机转 180° 换方向，别顶着目标反复重试。 */
      if (dogMode != MODE_RACE && mag <= DOG_DEAD_DEG && dogTargetCm <= DOG_STOP_CM)
      {
        dogStreak = 0;
        dogTurn(dogTurn180Ms(), +1, DOG_SCAN);
      #if DEBUG_SERIAL
        Serial.print(F("[DOG] 已贴到 "));
        Serial.print(dogTargetCm);
        Serial.print(F("cm，掉头 180° 用时 "));
        Serial.print(dogTurn180Ms());
        Serial.println(F("ms"));
      #endif
        break;
      }

      // 已经对准 → 直接走
      if (mag <= DOG_DEAD_DEG)
      {
        dogGo(dogMode == MODE_RACE ? RACE_GO_MS : DOG_GO_MS);
        break;
      }

      // 没对准 → 先转过去，转完再走
      {
        int8_t side = (off < 0) ? (int8_t)-1 : (int8_t)+1;
        if (angleReversed) { side = (int8_t)(-side); }

        uint32_t ms = (uint32_t)mag * 1000UL / (uint32_t)(turnDps ? turnDps : 1);
        if (ms < DOG_TURN_MIN_MS) { ms = DOG_TURN_MIN_MS; }
        if (ms > DOG_TURN_MAX_MS) { ms = DOG_TURN_MAX_MS; }

        dogStreak = 0;
        dogTurn(ms, side, DOG_GO);
      }
      break;
    }

    // ---- 旋转中 ----------------------------------------------------------
    case DOG_TURN:
      if ((uint32_t)(t - dogPhaseMs) >= dogHoldMs)
      {
        brakeCar();
        dogServoWrite(90);
        dogPhaseMs = t;
        dogPhase   = DOG_PAUSE;
      }
      break;

    // ---- 停稳，决定下一步 ------------------------------------------------
    case DOG_PAUSE:
      if ((uint32_t)(t - dogPhaseMs) < DOG_PAUSE_MS) { break; }

      if (dogAfter == DOG_GO)
      {
        dogGo(dogMode == MODE_RACE ? RACE_GO_MS : DOG_GO_MS);
      }
      else if (dogAfter == DOG_TURN_AWAY)
      {
        dogIdx = 0;
        dogTurn(dogTurn180Ms(), +1, DOG_SCAN);   // 贴到 5cm 了 → 掉头换方向
      }
      else if (dogAfter == DOG_TURN_AROUND)
      {
        /* 比赛：收集完成（这一趟确实推到东西了）。
         * 响一声告诉人"收了一个"，同时原地转 180° 换到相反方向继续找。
         * 蜂鸣是非阻塞的，转向期间正好播完。 */
        buzzPlay(SEQ_PICK, SEQ_PICK_N);
        dogIdx = 0;
        dogTurn(dogTurn180Ms(), +1, DOG_SCAN);
      #if DEBUG_SERIAL
        Serial.println(F("[RACE] 收集完成：蜂鸣 + 180 度换向"));
      #endif
      }
      else
      {
        dogIdx     = 0;
        dogPhase   = DOG_SCAN;
        dogPhaseMs = t;
      }
      break;

    // ---- 前进中 ----------------------------------------------------------
    case DOG_GO:
    {
      /* ================= 比赛模式 =================
       * 和遛狗最大的不同：**贴到东西也不停**。
       * 车底有一张网，物块比底盘还矮，一旦被推进底盘就落进超声波的盲区，
       * 于是"看不见了" —— 这正是我们要的，车可以继续收集下一个。
       * 所以贴近后不但不停，还要**再多推一段**（RACE_OVERRUN_MS），
       * 确保物块真的被兜进去，而不是刚好卡在边缘。 */
      if (dogMode == MODE_RACE)
      {
        if ((uint32_t)(t - dogCheckMs) >= DOG_GO_CHECK_MS)
        {
          dogCheckMs = t;
          const uint16_t d = dogMeasure();

          if (d != 0)
          {
            dogFrontCm = d;
            if (d <= RACE_PUSH_CM && !dogPushDone)
            {
              dogPushDone = true;
              dogHoldMs  += RACE_OVERRUN_MS;     // 这一段多走一会儿，把物块顶进底盘
            #if DEBUG_SERIAL
              Serial.print(F("[RACE] 贴到 "));
              Serial.print(d);
              Serial.print(F("cm，继续推 "));
              Serial.print(RACE_OVERRUN_MS);
              Serial.println(F("ms 把物块兜进底盘"));
            #endif
            }
          }
        }

        if ((uint32_t)(t - dogPhaseMs) < dogHoldMs) { break; }

        brakeCar();
        dogStreak  = 0;

        /* ★ 走完一段之后往哪走（2026-11-17 改，这是"几乎不左转"的根因）：
         *   原来固定右转 RACE_TURN_DEG(120°)，三次就是 360°，车一直在顺时针
         *   绕圈扫场 —— 目标几乎总出现在右侧，所以看起来"从来不会左转"。
         *   现在分两种：
         *     · 推过东西（dogPushDone）→ 收集完成：响一声 + 原地 180°，
         *       换到相反方向继续找（用户要求）
         *     · 只是走满没碰到东西 → **不转**，直接回扫描，
         *       由"最近的目标在哪"决定往左还是往右。
         *   这样左右完全由目标位置决定，不会再被固定方向带着绕圈。 */
        dogAfter = dogPushDone ? DOG_TURN_AROUND : DOG_SCAN;
        dogPhase = DOG_PAUSE;
        dogPhaseMs = t;
      #if DEBUG_SERIAL
        Serial.println(dogPushDone ? F("[RACE] 收集完成，准备 180 度换向")
                                   : F("[RACE] 这段没碰到东西，回扫描重新选最近"));
      #endif
        break;
      }

      // 定期看一眼前方：在撞上之前就停
      if ((uint32_t)(t - dogCheckMs) >= DOG_GO_CHECK_MS)
      {
        dogCheckMs = t;
        const uint16_t d = dogMeasure();

        if (d != 0)
        {
          dogFrontCm = d;
          if (d <= DOG_STOP_CM)
          {
            brakeCar();
            dogStreak  = 0;
            /* 先停稳（PAUSE）再掉头：走着的时候直接反向转，
             * 电流冲击大、齿轮也容易崩。 */
            dogAfter   = DOG_TURN_AWAY;
            dogPhase   = DOG_PAUSE;
            dogPhaseMs = t;
          #if DEBUG_SERIAL
            Serial.print(F("[DOG] 贴到 "));
            Serial.print(d);
            Serial.println(F("cm，刹住准备掉头"));
          #endif
            break;
          }
        }
      }

      /* ★ 纠偏：连续前进累计到 DOG_CONFIRM_MS 就刹车重扫一轮。
       *   重扫不是浪费时间 —— 它把目标此刻的方位重新量一遍，
       *   等于把「已经偏掉的车头」重新对准目标，是这里唯一有效的纠偏手段。 */
      if ((uint32_t)(t - dogGoStartMs) >= DOG_CONFIRM_MS)
      {
        brakeCar();
        dogStreak  = 0;
        dogAfter   = DOG_SCAN;
        dogPhase   = DOG_PAUSE;
        dogPhaseMs = t;
      #if DEBUG_SERIAL
        Serial.print(F("[DOG] 走满 "));
        Serial.print(DOG_CONFIRM_MS);
        Serial.println(F("ms，刹车重新确认方向"));
      #endif
        break;
      }

      if ((uint32_t)(t - dogPhaseMs) < dogHoldMs) { break; }

      /* 这一段走完：前方还空就接着走（不刹车，速度才不会一顿一顿），否则重扫。
       * dogFrontCm == 0 表示这一路都没回波 —— 那是「前方 4 米内什么都没有」，
       * 是最空的情况，当然接着走，不能停下来重扫白白浪费半秒。 */
      const bool frontClear = (dogFrontCm == 0) ||
                              (dogFrontCm > (uint16_t)(DOG_STOP_CM + 6));
      if (frontClear && dogStreak < DOG_GO_STREAK)
      {
        dogStreak++;
        dogPhaseMs = t;
      }
      else
      {
        brakeCar();
        dogStreak  = 0;
        dogAfter   = DOG_SCAN;
        dogPhase   = DOG_PAUSE;
        dogPhaseMs = t;
      }
      break;
    }

    default:
      dogIdx     = 0;
      dogPhase   = DOG_SCAN;
      dogPhaseMs = t;
      break;
  }
}

#endif  // DOG_ENABLED

/* ---------------------------------------------------------------------------
 *  处理一帧已解码的红外数据
 * -------------------------------------------------------------------------*/
void handleFrame()
{
  const uint32_t code = (uint32_t)results.value;
  const uint32_t t    = nowMs();

  // A. NEC 重发帧（按住不放时遥控器发这个）
  if (code == 0xFFFFFFFFUL || results.bits == 0)
  {
    lastCommandMs = t;      // 重发帧同样算"还按着"
    return;
  }

  // B. 解不出协议（噪声 / 陌生遥控器）
  if (results.decode_type == UNKNOWN)
  {
#if DEBUG_SERIAL
    Serial.print(F("[未知] code=0x"));
    Serial.print(code, HEX);
    Serial.print(F("  rawlen="));
    Serial.println(results.rawlen);
#endif
#if STOP_ON_UNKNOWN
    brakeCar();
#endif
    return;
  }

  // C. 防抖
  if (code == lastCode && (t - lastCodeMs) < DEBOUNCE_MS)
  {
    lastCodeMs = t;
    // 同一动作键的紧凑重发（App 一帧里塞几组）也算"还按着"，
    // 否则看门狗会把按住误判成松手，车会一顿一顿地停。
    if (lastCodeMapped) { lastCommandMs = t; }
    return;
  }
  lastCode   = code;
  lastCodeMs = t;

#if DOG_ENABLED
  /* ---- 自动模式：开、关各一个码，两条命令都是幂等的 ----
   * 红外没有回执，只能靠"重复发同一条命令不会产生副作用"来保证可靠。
   * 不用"按一下切换"的原因见 CODE_DOG_ON 上面的说明。 */
  if (code == CODE_DOG_ON || code == CODE_DOG_OFF ||
      code == CODE_RACE_ON || code == CODE_RACE_OFF)
  {
    pulseLed();
    lastCodeMapped = true;

    if (code == CODE_DOG_OFF || code == CODE_RACE_OFF)
    {
      if (dogOn) { dogStop("收到关闭命令"); }
      else       { Serial.println(F("[DOG] 已经是停止状态")); }
    }
    else
    {
      const uint8_t want = (code == CODE_RACE_ON) ? MODE_RACE : MODE_DOG;
      if (dogOn && dogMode == want) { /* 重复帧，什么都不做 */ }
      else
      {
        if (dogOn) { dogStop("切换模式", false); }   // 切模式不转圈
        dogStart(want);
      }
    }
    return;
  }
#endif

  // D. 查表
  for (uint8_t i = 0; i < KEY_COUNT; i++)
  {
    if (KEY_TABLE[i].code != code) { continue; }

#if DOG_ENABLED
    // 任何动作键都立刻接管：先掐掉提示旋转，再退出遛狗，最后照常执行这个动作
    beaconCancel();
    if (dogOn) { dogStop("手动接管", false); }   // 接管时不转圈，免得和按键抢电机
#endif

    pulseLed();
#if DEBUG_SERIAL
    Serial.print(F("[动作] "));
    Serial.print(KEY_TABLE[i].name);
    Serial.print(F("   L="));
    Serial.print(KEY_TABLE[i].leftWheel);
    Serial.print(F(" R="));
    Serial.print(KEY_TABLE[i].rightWheel);
    Serial.print(F("   duty="));
    Serial.println(KEY_TABLE[i].duty);
#endif
    lastCodeMapped = true;

    // 按住期间的帧间隔 → 自动定阈值。moving 是"这一帧之前"在不在走。
    const uint32_t gap = t - lastCommandMs;
    if (moving) { noteFrameGap(gap); }
  #if DEBUG_SERIAL
    if (moving && gap < 2000)
    {
      Serial.print(F("[帧] 间隔=")); Serial.print(gap);
      Serial.print(F("ms  阈值="));  Serial.print(deadmanMs);
      Serial.println(F("ms"));
    }
  #endif

    // 单边转弯（恰好一只轮子为 STOP）用运行时可调的 turnDuty；其余用表里的值
    const int8_t  lw = KEY_TABLE[i].leftWheel;
    const int8_t  rw = KEY_TABLE[i].rightWheel;
    const bool  isTurn = ((lw == DIR_STOP) != (rw == DIR_STOP));
    setWheels(lw, rw, isTurn ? turnDuty : KEY_TABLE[i].duty);
    return;
  }

  // E. 键码合法但表里没有（本程序没有速度档，所以数字键会走到这里）
#if DEBUG_SERIAL
  Serial.print(F("[未映射] 0x"));
  Serial.print(code, HEX);
  Serial.println(F("  （本测试程序只有七个动作键）"));
#endif
  lastCodeMapped = false;
  pulseLed();
}

/* ---------------------------------------------------------------------------
 *  loop()
 * -------------------------------------------------------------------------*/
void loop()
{
  serviceSerial();        // 串口调参：L250 / R248 / W 存盘 / D 恢复默认

  if (irrecv.decode(&results))
  {
    handleFrame();
    irrecv.resume();      // 漏了这句就只响应第一次按键
  }

  /* 蜂鸣器每拍都要服务：它是软件方波，靠主循环翻转电平。
   * 放在最前面，保证提示音不被后面的逻辑拖慢。 */
  serviceBuzzer();

#if DOG_ENABLED
  /* 提示旋转独立于遛狗模式运行：它要在"已结束"之后还能转右圈，
   * 所以不能塞进 serviceDog（那时 dogOn 已经是 false 了）。
   * ★ 启动/结束提示已改成蜂鸣，这里只剩 K/Y 标定命令还在用它。 */
  serviceBeacon();

  /* 遛狗模式自己会调 setWheels，所以必须跑在 serviceRamp 之前，
   * 让这一拍就能把新速度算进曲线里。 */
  if (dogOn) { serviceDog(); }
#endif

  serviceRamp();
  serviceLed();

#if DOG_ENABLED
  /* 遛狗模式**不能**走抬手即停：它是"按一下就自己跑"，中间没有帧到达，
   * 看门狗会在 450ms 后把车刹掉 —— 表现成"启动了但只动一下"。
   * 它的安全由 DOG_MAX_MS 硬超时 + 距离刹停 + 无回波退出负责。
   *
   * ★ 提示旋转同样必须免检：结束提示是在 dogOn 已经变成 false 之后才开始转的，
   *   否则 450ms 就被看门狗刹住，右旋只转五分之一圈。 */
  if (!dogOn && beaconPhase == 0) { serviceAutoStop(); }
#else
  serviceAutoStop();
#endif
}

/* ============================================================================
 *  测完想回到"能自己调速度"的版本
 * ----------------------------------------------------------------------------
 *  用 04_pwm\RemoteControl_PWM（CH+/CH-/数字键 1~9 调档）。
 *  两个程序的引脚、接线、红外解析、软启动、刹车逻辑完全一样，
 *  区别只有"占空比从哪来"：这里写死，那边由档位算。
 *
 *  想再加一种规则，比如"转弯时内侧轮减速"，就在 KEY_TABLE 里给那两行填不同的 duty
 *  ——不过那需要 applyOutput() 支持左右两个不同占空比，改法见文件末尾 04_pwm 的注释。
 * ==========================================================================*/
