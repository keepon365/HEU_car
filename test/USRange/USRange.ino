/* ===========================================================================
 *  USRange —— 连续测距观察器（看"范围 / 死角"）
 *  ---------------------------------------------------------------------------
 *  ★ 双击项目根目录的「看距离.bat」即可，不需要打开 Arduino IDE。
 *    那个 bat 会起一个黑窗口，里面持续刷新读数，一直输出到你关掉它。
 *
 *  ---------------------------------------------------------------------------
 *  默认输出（一行一次采样，带距离条，扫场时最好用）：
 *
 *      15.4cm [#########.........................]  min14 max17
 *      ----   [...................................]  NO ECHO
 *
 *  串口命令（在窗口里直接敲字母回车）
 *      h   打印距离直方图 —— 找死角神器：扫完场地按一下，
 *          哪一格是 0，那段距离就是这个模块在你场地的盲区
 *      z   清零统计和直方图
 *      b   蜂鸣器开/关（默认关，不吵）
 *      p   切换绘图器模式（Arduino 串口绘图器用，一行 key:value）
 *      ?   重印说明
 *
 *  ---------------------------------------------------------------------------
 *  已知能力边界（别白试）
 *      硬盲区      2~4cm     探头自身声学耦合，低于 4cm 一律丢弃
 *      可靠量程    4~250cm   标称 4m，室内 2m 外开始零星丢波
 *      锥角        约 15°    细杆、桌腿常"看不见"
 *      测不到      布/海绵/绒毛   吸声
 *      测不准      光滑斜面 >45°  镜面反射跑偏，读数突然变很远
 *
 *  ★ 不驱动电机，轮子不会转。
 *  ★ 接线：VCC->5V  Trig->A0  Echo->A1  GND->GND   beep=A3
 * =========================================================================*/

#include <Arduino.h>

const uint8_t PIN_TRIG = A0;
const uint8_t PIN_ECHO = A1;
const uint8_t PIN_LED  = 13;
const uint8_t PIN_BUZ  = A3;

/* ★ 默认不出声。想开就敲 b */
bool buzzerOn = false;

/* 两次触发间隔。手册下限 60ms，留余量到 70ms，否则上一发余波会被当成新回波 */
const uint16_t SAMPLE_MS  = 70;
const uint16_t TIMEOUT_US = 25000;      // 25000us ≈ 4.3m
const uint8_t  WIN = 32;                // 滚动窗口 32 个样本 ≈ 2.2 秒

/* 盲区：低于这个值当作无效（探头声学耦合会给出 2~4cm 的假读数） */
const int16_t  BLIND_CM = 4;

/* 直方图：0~400cm，每 10cm 一格 */
const uint8_t  BIN_CM = 10;
const uint8_t  BIN_N  = 40;
uint16_t hist[BIN_N];

float    ring[WIN];
uint8_t  ringIdx = 0, ringCnt = 0;

uint32_t nextSampleMs = 0;
uint8_t  printDiv     = 0;              // 每 3 次采样打印一次（约 210ms）

bool     plotMode  = false;
float    lastCm    = 0;
float    lastRawCm = 0;
uint16_t lostTotal = 0, okTotal = 0;
uint16_t recentLost = 0, recentOk = 0;  // 最近一段的丢波比例

/* ========================================================================= */

int16_t measureCm()
{
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  const unsigned long us = pulseIn(PIN_ECHO, HIGH, TIMEOUT_US);
  if (us == 0) { return -1; }

  const int16_t cm = (int16_t)(us / 58.0f + 0.5f);
  if (cm < BLIND_CM) { return -2; }
  return cm;
}

/* ---- 三点中值：抗单次假回波，比平均值更不容易被一个坏点带偏 ---- */
int16_t rawRing[3] = { -1, -1, -1 };
uint8_t rawIdx = 0;

void pushRaw(int16_t cm)
{
  rawRing[rawIdx] = cm;
  rawIdx = (uint8_t)((rawIdx + 1) % 3);
}

int16_t medianValid()
{
  int16_t v[3];
  uint8_t n = 0;
  for (uint8_t i = 0; i < 3; i++)
  {
    if (rawRing[i] > 0) { v[n++] = rawRing[i]; }
  }
  if (n == 0) { return 0; }
  if (n == 1) { return v[0]; }
  if (n == 2) { return (int16_t)((v[0] + v[1]) / 2); }

  if (v[0] > v[1]) { const int16_t t = v[0]; v[0] = v[1]; v[1] = t; }
  if (v[1] > v[2]) { const int16_t t = v[1]; v[1] = v[2]; v[2] = t; }
  if (v[0] > v[1]) { const int16_t t = v[0]; v[0] = v[1]; v[1] = t; }
  return v[1];
}

void pushRing(float v)
{
  ring[ringIdx] = v;
  ringIdx = (uint8_t)((ringIdx + 1) % WIN);
  if (ringCnt < WIN) { ringCnt++; }
}

/* 窗口内有效值的最小/最大 */
void windowRange(float &mn, float &mx)
{
  mn = 9999;
  mx = 0;
  for (uint8_t i = 0; i < ringCnt; i++)
  {
    if (ring[i] <= 0) { continue; }
    if (ring[i] < mn) { mn = ring[i]; }
    if (ring[i] > mx) { mx = ring[i]; }
  }
  if (mn > 9000) { mn = 0; }
}

void resetStats()
{
  ringIdx = 0;
  ringCnt = 0;
  lostTotal = 0;
  okTotal = 0;
  recentLost = 0;
  recentOk = 0;
  for (uint8_t i = 0; i < BIN_N; i++) { hist[i] = 0; }
  Serial.println(F("# stats cleared"));
}

/* ---------------------------------------------------------------------------
 *  蜂鸣器：默认关。开了之后靠近才响，越近越急。
 * ------------------------------------------------------------------------- */
uint32_t lastBeepMs = 0;

void serviceBuzzer(float cm, bool valid)
{
  if (!buzzerOn) { return; }
  if (!valid || cm <= 0)
  {
    noTone(PIN_BUZ);
    return;
  }

  uint16_t period, freq, dur;
  if (cm < 20)       { period = 0;    freq = 2400; dur = 1000; }
  else if (cm < 60)  { period = 150;  freq = 2000; dur = 60;   }
  else if (cm < 150) { period = 500;  freq = 1400; dur = 60;   }
  else               { period = 1200; freq = 800;  dur = 60;   }

  if (period == 0)
  {
    tone(PIN_BUZ, freq);
    return;
  }
  noTone(PIN_BUZ);
  if (millis() - lastBeepMs >= period)
  {
    lastBeepMs = millis();
    tone(PIN_BUZ, freq, dur);
  }
}

/* ---------------------------------------------------------------------------
 *  一行读数：距离 + 距离条 + 窗口极值
 *  条子一格 2cm，40 格到 80cm 封顶，超过就全满
 * ------------------------------------------------------------------------- */
void printOneLine()
{
  if (lastCm <= 0)
  {
    Serial.print(F("  ----"));
  }
  else
  {
    if (lastCm < 100) { Serial.print(' '); }
    if (lastCm < 10)  { Serial.print(' '); }
    Serial.print(lastCm, 1);
    Serial.print(F("cm"));
  }
  Serial.print(' ');

  Serial.print('[');
  uint8_t n = (lastCm > 0) ? (uint8_t)(lastCm / 2.0f) : 0;
  if (n > 40) { n = 40; }
  for (uint8_t i = 0; i < 40; i++) { Serial.print(i < n ? '#' : '.'); }
  Serial.print(F("]  "));

  if (lastCm <= 0)
  {
    Serial.print(F("NO ECHO"));
  }
  else
  {
    float mn, mx;
    windowRange(mn, mx);
    Serial.print(F("min"));
    Serial.print((int)mn);
    Serial.print(F(" max"));
    Serial.print((int)mx);
  }

  /* 最近 32 次的丢波率：能看出这块材料/角度是不是在"半通" */
  const uint16_t tot = (uint16_t)(recentOk + recentLost);
  if (tot >= 32)
  {
    const uint8_t pct = (uint8_t)((uint32_t)recentLost * 100UL / tot);
    if (pct > 0)
    {
      Serial.print(F(" lose"));
      Serial.print(pct);
      Serial.print('%');
    }
    recentOk = 0;
    recentLost = 0;
  }

  Serial.println();
}

void printHelp()
{
  Serial.println();
  Serial.println(F("# ==== US RANGE ===="));
  Serial.println(F("# VCC=5V  Trig=A0  Echo=A1  GND     beep=A3 (off)"));
  Serial.println(F("# keys:  h=histogram  z=clear  b=buzzer  p=plotter  ?=help"));
  Serial.println(F("# layout: 15.4cm [#########....]  min14 max17  lose3%"));
  Serial.println(F("# -------------------------------"));
}

void printHistogram()
{
  Serial.println();
  Serial.println(F("# ---- DISTANCE HISTOGRAM (10cm bins) ----"));
  Serial.println(F("# bins with count 0 = blind / never reached"));

  bool anyData = false;

  for (uint8_t i = 0; i < BIN_N; i++)
  {
    if (hist[i] == 0) { continue; }

    anyData = true;
    const uint16_t lo = (uint16_t)i * BIN_CM;

    Serial.print(F("# "));
    if (lo < 100) { Serial.print(' '); }
    if (lo < 10)  { Serial.print(' '); }
    Serial.print(lo);
    Serial.print(F("-"));
    Serial.print(lo + BIN_CM - 1);
    Serial.print(F("  "));
    if (hist[i] < 10) { Serial.print(' '); }
    Serial.print(hist[i]);
    Serial.print(' ');

    uint8_t bar = (uint8_t)(hist[i] / 4);
    if (bar > 50) { bar = 50; }
    for (uint8_t k = 0; k < bar; k++) { Serial.print('*'); }
    Serial.println();
  }

  if (!anyData) { Serial.println(F("# (no data yet)")); }

  Serial.print(F("# echo samples : "));
  Serial.println(okTotal);
  Serial.print(F("# no-echo      : "));
  Serial.println(lostTotal);
  Serial.print(F("# loss rate    : "));
  if ((uint16_t)(okTotal + lostTotal) > 0)
  {
    Serial.print((uint32_t)lostTotal * 100UL / (uint32_t)(okTotal + lostTotal));
    Serial.print('%');
  }
  else { Serial.print(F("n/a")); }
  Serial.println();
  Serial.println(F("# gaps between bins = distances never measured"));
  Serial.println(F("# ---------------------------------------"));
  Serial.println();
}

void handleSerial()
{
  while (Serial.available() > 0)
  {
    switch ((char)Serial.read())
    {
      case 'h': printHistogram(); break;
      case 'z': resetStats();     break;
      case '?': printHelp();      break;

      case 'b':
        buzzerOn = !buzzerOn;
        if (!buzzerOn) { noTone(PIN_BUZ); }
        Serial.print(F("# buzzer "));
        Serial.println(buzzerOn ? F("ON") : F("OFF"));
        break;

      case 'p':
        plotMode = !plotMode;
        Serial.print(F("# mode="));
        Serial.println(plotMode ? F("plotter") : F("line"));
        break;

      default: break;
    }
  }
}

void setup()
{
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  digitalWrite(PIN_LED, LOW);
  noTone(PIN_BUZ);                 // 上电别乱响

  resetStats();
  delay(300);
  printHelp();
}

void loop()
{
  handleSerial();

  if (millis() - nextSampleMs < SAMPLE_MS) { return; }
  nextSampleMs = millis();

  const int16_t raw = measureCm();

  if (raw > 0)
  {
    pushRaw(raw);
    if (okTotal < 60000) { okTotal++; }
    recentOk++;
  }
  else
  {
    if (lostTotal < 60000) { lostTotal++; }
    recentLost++;
  }

  const int16_t med = medianValid();
  lastCm    = (float)med;
  lastRawCm = (raw > 0) ? (float)raw : 0;

  pushRing(lastCm);
  serviceBuzzer(lastCm, med > 0);

  /* 太近时点亮板载 LED，纯视觉提示，不出声 */
  digitalWrite(PIN_LED, (lastCm > 0 && lastCm < 20) ? HIGH : LOW);

  if (med > 0)
  {
    const uint8_t bin = (uint8_t)(med / BIN_CM);
    if (bin < BIN_N && hist[bin] < 60000) { hist[bin]++; }
  }

  /* ---- 输出节流：每 3 次采样打一行（约 210ms） ---- */
  printDiv++;
  if (printDiv < 3) { return; }
  printDiv = 0;

  if (plotMode)
  {
    float mn, mx;
    windowRange(mn, mx);
    Serial.print(F("dist:"));
    Serial.print(lastCm, 1);
    Serial.print(F(" raw:"));
    Serial.print(lastRawCm, 1);
    Serial.print(F(" min:"));
    Serial.print(mn, 1);
    Serial.print(F(" max:"));
    Serial.print(mx, 1);
    Serial.println();
  }
  else
  {
    printOneLine();
  }
}
