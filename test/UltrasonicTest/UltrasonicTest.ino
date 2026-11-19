/* ===========================================================================
 *  UltrasonicTest —— 超声波模块能力测试（专测 ZYXJ03 / HC-SR04）
 *  ---------------------------------------------------------------------------
 *  它比 SensorTest 里那个"顺便测一下"的版本强在三点：
 *    1) 带滑动窗口统计（min / max / 极差 / 标准差），能客观看出稳不稳
 *    2) 抛出"无回波"而不是打 0，避免把盲区误读成"距离 0cm"
 *    3) 打印节奏受控，9600 波特率下不会撑爆串口（否则你会看到乱码）
 *
 *  ★ 不驱动电机，轮子不会转。
 *
 *  ---------------------------------------------------------------------------
 *  接线（来自光盘《扩展板原理图.pdf》CSB1 4P 座）
 *      5V | Trig = A0 | Echo = A1 | GND
 *    如果自己插杜邦线：VCC->5V, Trig->A0, Echo->A1, GND->GND
 *    ⚠ 线序和"循迹模块"的 4P 座不一样，别照插座形状猜
 *
 *  ---------------------------------------------------------------------------
 *  ★ 怎么判断成功 —— 听声音，别数灯（UNO 板上 4 个灯，TX 灯会跟着串口狂闪，
 *    肉眼很难分辨谁是谁）：
 *
 *      一段"嘀——嘀——"  低音，2 秒一次   = 一直没收到回波（查供电/接线）
 *      "嘀嘀嘀嘀"       高音，连成一片   = 正在稳定测距（成功！）
 *      上电时三声急促高音               = 固件已启动
 *
 *    蜂鸣器在 A3（ZYV6 扩展板的 beep 电路）。若你的板子没有蜂鸣器，
 *    就退回看 D13 的板载 LED：无回波慢闪、有回波常亮。
 *
 *  ---------------------------------------------------------------------------
 *  串口 9600，输出格式：
 *      52.3 cm  D0.9  s0.2        正常读数（D=窗口极差，s=标准差）
 *      NO ECHO                    连续 3 次没收到回波（接线松/超量程/贴太近）
 *      z  清零统计    u  切换原始微秒    ?  重印说明
 * =========================================================================*/

#include <Arduino.h>

const uint8_t PIN_TRIG   = A0;
const uint8_t PIN_ECHO   = A1;
const uint8_t PIN_LED    = 13;    // 板载 LED
const uint8_t PIN_BUZZER = A3;    // 蜂鸣器（扩展板 beep 电路）

/* HC-SR04 手册要求两次触发间隔 >= 60ms，否则上一次的余波会污染下一次 */
const uint16_t GAP_MS      = 60;
const uint16_t PRINT_MS    = 200;     // 打印节奏：5Hz，9600 波特率下不会爆缓冲
const uint8_t  WIN         = 16;      // 统计窗口：16 个样本 ≈ 覆盖 960ms

float    buf[WIN];
uint8_t  idx = 0;
uint8_t  cnt = 0;

uint32_t nextMeasureMs = 0;
uint32_t nextPrintMs   = 0;
uint16_t lastUs        = 0;
uint16_t echoMisses    = 0;      // 连续无回波次数（uint16 防回绕误判）
bool     showUs        = false;

/* ========================================================================= */

void banner()
{
  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" 超声波测距测试   (不驱动电机)"));
  Serial.println(F("========================================"));
  Serial.println(F(" Trig=A0  Echo=A1  VCC=5V  GND  beep=A3"));
  Serial.println(F("----------------------------------------"));
  Serial.println(F(" 听声音判断状态（比数灯可靠）："));
  Serial.println(F("   低音 '嘀——'  2秒一次  = 没回波"));
  Serial.println(F("   高音 '嘀嘀嘀' 连成片   = 测到了"));
  Serial.println(F("   上电三声急促高音       = 已启动"));
  Serial.println(F(" 没有蜂鸣器就看 D13：慢闪=没回波 常亮=成功"));
  Serial.println(F("----------------------------------------"));
  Serial.println(F(" 52.3 cm  D0.9 s0.2"));
  Serial.println(F("   ^距离   ^极差 ^标准差"));
  Serial.println(F(" z 清零   u 显示原始us   ? 说明"));
  Serial.println(F("========================================"));
  Serial.println();
}

void setup()
{
  Serial.begin(9600);

  pinMode(PIN_LED, OUTPUT);

  /* 启动自检：三声急促高音 + LED 快闪。
     听到"嘀嘀嘀"说明固件已接管；完全没声说明 A3 上没有蜂鸣器。 */
  for (uint8_t i = 0; i < 3; i++)
  {
    digitalWrite(PIN_LED, HIGH);
    tone(PIN_BUZZER, 2200, 70);
    delay(110);
    digitalWrite(PIN_LED, LOW);
    delay(90);
  }

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  delay(200);
  banner();
}

/* ---------------------------------------------------------------------------
 *  状态反馈：声音为主，LED 为辅
 *    无回波：低音"嘀——" 每 2 秒一次，LED 慢闪
 *    有回波：高音"嘀嘀嘀" 每 200ms 一次，LED 常亮
 *    tone() 走 Timer2，和 pulseIn 用的 Timer0 不冲突，不会拖慢测距。
 * ------------------------------------------------------------------------- */
void serviceStatus(bool gotEcho)
{
  static uint32_t tMs   = 0;
  static bool     ledOn = false;

  if (gotEcho)
  {
    digitalWrite(PIN_LED, HIGH);

    if (millis() - tMs >= 200)
    {
      tMs = millis();
      tone(PIN_BUZZER, 1800, 50);       // 高音、密集 = 成功
    }
    return;
  }

  /* ---- 无回波 ---- */
  if (millis() - tMs >= 2000)
  {
    tMs   = millis();
    ledOn = true;
    digitalWrite(PIN_LED, HIGH);
    tone(PIN_BUZZER, 420, 120);         // 低音、稀疏 = 没找到
  }
  else if (ledOn && millis() - tMs >= 600)
  {
    ledOn = false;
    digitalWrite(PIN_LED, LOW);
  }
}

/* ---------------------------------------------------------------------------
 *  返回厘米；-1 表示无回波。
 *  超时 25000us ≈ 4.3m（声速 340m/s，来回 2 倍路程）
 * ------------------------------------------------------------------------- */
int16_t measure()
{
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  const unsigned long us = pulseIn(PIN_ECHO, HIGH, 25000UL);
  if (us == 0) { return -1; }

  lastUs = (us > 65535UL) ? 65535 : (uint16_t)us;
  return (int16_t)((float)us / 58.0f + 0.5f);
}

void pushSample(float v)
{
  buf[idx] = v;
  idx = (uint8_t)((idx + 1) % WIN);
  if (cnt < WIN) { cnt++; }
}

void clearStats()
{
  idx = 0;
  cnt = 0;
}

/* 打印一次窗口统计 */
void printStats(int16_t inst, bool gotEcho)
{
  if (!gotEcho)
  {
    Serial.print(F("NO ECHO"));
    if (showUs) { Serial.print(F("   (pulseIn 超时)")); }
    Serial.println();
    return;
  }

  float mn = 9999, mx = -1, sum = 0;
  for (uint8_t i = 0; i < cnt; i++)
  {
    const float v = buf[i];
    if (v < mn) { mn = v; }
    if (v > mx) { mx = v; }
    sum += v;
  }
  const float avg = (cnt > 0) ? (sum / (float)cnt) : (float)inst;

  /* 标准差：反映抖动程度，比"看起来稳不稳"客观 */
  float var = 0;
  if (cnt > 1)
  {
    for (uint8_t i = 0; i < cnt; i++)
    {
      const float d = buf[i] - avg;
      var += d * d;
    }
    var /= (float)(cnt - 1);
  }

  Serial.print(inst);
  Serial.print(F(" cm  "));

  if (cnt > 1)
  {
    Serial.print(F("D"));                 // 极差
    Serial.print(mx - mn, 1);
    Serial.print(F("  s"));               // 标准差
    Serial.print(sqrt(var), 2);
  }
  else
  {
    Serial.print(F("(取样中)"));
  }

  if (showUs)
  {
    Serial.print(F("   "));
    Serial.print(lastUs);
    Serial.print(F("us"));
  }
  Serial.println();
}

void handleSerial()
{
  while (Serial.available() > 0)
  {
    switch ((char)Serial.read())
    {
      case 'z':
        clearStats();
        echoMisses = 0;
        Serial.println(F("[统计已清零]"));
        break;

      case 'u':
        showUs = !showUs;
        Serial.print(F("[原始us "));
        Serial.print(showUs ? F("开") : F("关"));
        Serial.println(F("]"));
        break;

      case '?':
        banner();
        break;

      default:
        break;
    }
  }
}

void loop()
{
  handleSerial();

  /* ---- 测距 ---- */
  if (millis() - nextMeasureMs >= GAP_MS)
  {
    nextMeasureMs = millis();

    const int16_t cm = measure();

    if (cm < 0) { if (echoMisses < 60000) { echoMisses++; } }
    else
    {
      echoMisses = 0;
      pushSample((float)cm);
    }
  }

  const bool haveEcho = (echoMisses == 0 && cnt > 0);

  serviceStatus(haveEcho);

  /* ---- 打印（单独节拍，和测距解耦，避免刷屏撑爆串口） ---- */
  if (millis() - nextPrintMs >= PRINT_MS)
  {
    nextPrintMs = millis();

    if (!haveEcho)
    {
      printStats(0, false);

      /* 连续 8 秒收不到回波，插播一次排查清单（只插一次，不刷屏） */
      static bool hinted = false;
      if (echoMisses > 130 && !hinted)
      {
        hinted = true;
        Serial.println(F("-------- 8 秒无回波，按顺序查 --------"));
        Serial.println(F(" 1) 模块 VCC 是否真的接到 5V（扩展板 5VO 位）"));
        Serial.println(F(" 2) GND 是否共地（UNO 的 GND 必须连到模块 GND）"));
        Serial.println(F(" 3) 4P 线序：5V|Trig|Echo|GND，不是 5V|GND|.."));
        Serial.println(F(" 4) 探头前 20~50cm 放一本书或纸板，别对空气"));
        Serial.println(F(" 5) 最省事的验证：甩开扩展板，4 根线直插 UNO"));
        Serial.println(F("    （VCC->5V, Trig->A0, Echo->A1, GND->GND）"));
        Serial.println(F(" 6) 直接量模块 VCC-GND：应该是 5V，0V 就是没供电"));
        Serial.println(F("--------------------------------------"));
      }
    }
    else
    {
      printStats((int16_t)buf[(idx + WIN - 1) % WIN], true);
    }
  }
}
