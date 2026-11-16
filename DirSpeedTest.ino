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
#define DUTY_TURN          65   // 左转 / 右转：只驱动外侧轮时的占空比
                                //   内侧轮是"断电自由滚动"（不是刹车），转弯半径由轮距决定，
                                //   所以这个值只决定**转得多快**：调小 = 点一下转的角度小。
                                //   下限约 80：再低要克服内侧轮拖滑阻力 + 左轮起转门槛（73），
                                //   会出现"按了不转、松手才动"。
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
#define SPEED_PERMILLE   1200

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
#define RAMP_START_PCT     40   // 刚按下时的目标 = 上限的 65%（120 → 78）
                                //   别低于 65：左轮起转门槛 73，再低会"按了不动"
#define RAMP_FULL_MS     2500   // 按住多久爬到上限（越大越线性，越小越蹿）
                                //   1200 = 急性子  1600 = 适中  2500 = 很线性

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
#define TRIM_LEFT        13    // 左轮"起步"补偿（谁先动的问题）
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
#define GAIN_RIGHT       225    // 右轮摩擦小、更容易转 → 压低它等于把车头往右掰

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
  { 0x00FFE01FUL, "-    左旋",  DIR_BACKWARD, DIR_FORWARD,  DUTY_SPIN },
  { 0x00FF906FUL, "EQ   右旋",  DIR_FORWARD,  DIR_BACKWARD, DUTY_SPIN },
};
const uint8_t KEY_COUNT = sizeof(KEY_TABLE) / sizeof(KEY_TABLE[0]);

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
  for (uint8_t i = 1; i < cmdLen; i++)
  {
    if (cmdBuf[i] < '0' || cmdBuf[i] > '9') { break; }
    v = (int16_t)(v * 10 + (cmdBuf[i] - '0'));
  }

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
  Serial.println(F("=============================================="));
#endif

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
 * -------------------------------------------------------------------------*/
uint8_t buildDuty(uint8_t cmd, uint8_t trim, uint16_t gain, uint8_t target)
{
  if (cmd == 0) { return 0; }

  // 用 32 位：gain 允许超过 256（把慢的那侧调快），
  // duty(≤295) × 400 会超出 uint16，用 16 位会溢出回绕成小值。
  uint32_t duty = cmd;

  // ---- ① 死区补偿 ----
  if (trim != 0)
  {
    uint8_t fadeEnd = (target > COMP_FADE_START) ? target : COMP_FADE_START;

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

  // ---- ② 增益补偿 ----
  if (gain != 256)
  {
    duty = duty * gain / 256;
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
  targetDuty   = scaleSpeed(rampedDuty(duty));   // 只给起点，之后由 serviceRamp 往上推
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
    Serial.println(currentDuty);
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
  if (currentDuty == targetDuty)
  {
    Serial.print(F("  [到位] duty="));
    Serial.println(currentDuty);
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

  // D. 查表
  for (uint8_t i = 0; i < KEY_COUNT; i++)
  {
    if (KEY_TABLE[i].code != code) { continue; }

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

  serviceRamp();
  serviceLed();
  serviceAutoStop();
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
