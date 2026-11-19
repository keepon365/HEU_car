/* ===========================================================================
 *  SonarScan —— 舵机带超声波左右扫描，把前方画成字符地图
 *  ---------------------------------------------------------------------------
 *  ★ 这个版本只扫、不走。电机完全不驱动，轮子不会转。
 *    目的是让你先看清"舵机能看多远、哪个方向有死角、多少厘米该转向"，
 *    阈值定下来之后，再写真正会跑的避障。
 *
 *  ---------------------------------------------------------------------------
 *  接线（舵机和超声波共用 UNO，不走扩展板）
 *      舵机    红线 -> 5V      棕/黑 -> GND     橙/黄 -> D2（或 D7 / D12）
 *      超声波  VCC  -> 5V      GND   -> GND     Trig -> A0    Echo -> A1
 *      （D2 / D7 / D12 三脚同时输出同一角度，接哪个都行）
 *
 *  ⚠ 舵机 + 超声波同时工作，电流峰值能到 700mA。USB 口带不动会让板子重启
 *    （表现：窗口突然断开重连、地图刷到一半没了）。真出现就把舵机单独供电。
 *
 *  ---------------------------------------------------------------------------
 *  输出长这样（每扫完一轮打一块）：
 *
 *      [ 150  135  120  105   90   75   60   45   30 ]
 *      [   -    -    +    *    #    *    +    -    - ]
 *      nearest  #  12cm @ 90deg        forward  BLOCKED
 *
 *      符号： # <20cm 危险   * 20-40 近   + 40-80 中   - 80-150 远   . >150/无回波
 *
 *  ---------------------------------------------------------------------------
 *  窗口里敲字母
 *      c   连续扫描 开/关（默认开）
 *      s   单次扫描
 *      m   停止扫描，舵机回中位 90° 并保持（装车对位时用）
 *      w   打印每 15° 的实测脉宽，标定舵机行程
 *      ?   重印说明
 * =========================================================================*/

#include <Servo.h>

/* ---- 舵机 ---- */
const uint8_t SERVO_PINS[] = { 2, 7, 12 };   // 三脚同时输出，接哪个都行
const uint8_t SERVO_N = 3;
Servo servo[SERVO_N];

const uint16_t US_MIN = 500;
const uint16_t US_MAX = 2500;

/* ---- 超声波 ---- */
const uint8_t  PIN_TRIG = A0;
const uint8_t  PIN_ECHO = A1;
const int16_t  BLIND_CM = 4;              // 盲区以下丢弃
const uint16_t TIMEOUT_US = 25000;        // ≈4.3m

/* ---- 扫描参数 ---- */
const int16_t  SCAN_FROM  = 150;          // 起始角（度）
const int16_t  SCAN_TO    = 30;           // 结束角
const int16_t  SCAN_STEP  = 15;           // 步进
const uint16_t SETTLE_MS  = 140;          // 舵机到位后等它稳住的静置时间
const uint8_t  POS_N      = 9;            // (150-30)/15 + 1

int16_t posAngle[POS_N];
int16_t posCm[POS_N];                     // -1 = 无回波

/* 上电先停在 90° 4 秒（足够对位/看中位），然后自动开始扫描。
 * 这样即使键盘输入送不进来，也能直接看到扫描结果。
 * 想让它一直保持中位就按 m。 */
const uint32_t AUTO_SCAN_MS = 4000;
uint32_t bootMs = 0;

bool     scanOn  = false;
bool     holdMid = false;

/* ========================================================================= */

void servoWrite(int16_t deg)
{
  if (deg < 0)   { deg = 0; }
  if (deg > 180) { deg = 180; }

  const uint32_t span = (uint32_t)(US_MAX - US_MIN);
  const uint16_t us = (uint16_t)(US_MIN + span * (uint32_t)deg / 180UL);
  for (uint8_t i = 0; i < SERVO_N; i++) { servo[i].writeMicroseconds(us); }
}

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
  if (cm < BLIND_CM) { return -1; }
  return cm;
}

char symbol(int16_t cm)
{
  if (cm < 0)   { return '.'; }
  if (cm < 20)  { return '#'; }
  if (cm < 40)  { return '*'; }
  if (cm < 80)  { return '+'; }
  if (cm < 150) { return '-'; }
  return '.';
}

void printPadded(int16_t v, bool numeric)
{
  if (!numeric)
  {
    Serial.print(' ');
    Serial.print(symbol(v));
    Serial.print(F("  "));
    return;
  }

  if (v < 0)          { Serial.print(F("  -")); }
  else if (v < 10)    { Serial.print(F("  ")); Serial.print(v); }
  else if (v < 100)   { Serial.print(' '); Serial.print(v); }
  else                { Serial.print(v); }
  Serial.print(' ');
}

void printMap()
{
  /* 角度行 */
  Serial.print(F("["));
  for (uint8_t i = 0; i < POS_N; i++)
  {
    if (posAngle[i] < 100) { Serial.print(' '); }
    if (posAngle[i] < 10)  { Serial.print(' '); }
    Serial.print(posAngle[i]);
    if (i + 1 < POS_N) { Serial.print(' '); }
  }
  Serial.println(F("]"));

  /* 符号行 */
  Serial.print(F("["));
  for (uint8_t i = 0; i < POS_N; i++)
  {
    Serial.print(' ');
    Serial.print(symbol(posCm[i]));
    if (i + 1 < POS_N) { Serial.print(F("   ")); }
  }
  Serial.println(F("]"));

  /* 最近点 */
  int16_t bestCm = 9999;
  int16_t bestA  = 0;
  for (uint8_t i = 0; i < POS_N; i++)
  {
    if (posCm[i] > 0 && posCm[i] < bestCm) { bestCm = posCm[i]; bestA = posAngle[i]; }
  }

  Serial.print(F("nearest  "));
  if (bestCm > 9000)
  {
    Serial.print(F("none"));
  }
  else
  {
    Serial.print(symbol(bestCm));
    Serial.print(' ');
    Serial.print(bestCm);
    Serial.print(F("cm @ "));
    Serial.print(bestA);
    Serial.print(F("deg"));
  }

  /* 正前方通路：只看 75~105 这三个角度 */
  int16_t frontCm = 9999;
  for (uint8_t i = 0; i < POS_N; i++)
  {
    if (posAngle[i] >= 75 && posAngle[i] <= 105)
    {
      if (posCm[i] < 0) { continue; }
      if (posCm[i] < frontCm) { frontCm = posCm[i]; }
    }
  }

  Serial.print(F("   forward  "));
  if (frontCm > 9000)      { Serial.print(F("CLEAR")); }
  else if (frontCm < 20)   { Serial.print(F("BLOCKED")); }
  else
  {
    Serial.print(frontCm);
    Serial.print(F("cm"));
  }
  Serial.println();
  Serial.println();
}

void doScan()
{
  uint8_t k = 0;
  for (int16_t a = SCAN_FROM; a >= SCAN_TO; a -= SCAN_STEP)
  {
    if (k >= POS_N) { break; }
    posAngle[k] = a;

    servoWrite(a);
    delay(SETTLE_MS);

    /* 每个角度测两发取小的那个：第一发常被舵机自己的震动污染 */
    const int16_t c1 = measureCm();
    delay(10);
    const int16_t c2 = measureCm();
    if (c1 < 0)      { posCm[k] = c2; }
    else if (c2 < 0) { posCm[k] = c1; }
    else             { posCm[k] = (c1 < c2) ? c1 : c2; }

    k++;
  }
  printMap();
}

void help()
{
  Serial.println();
  Serial.println(F("# ==== SONAR SCAN (no motors) ===="));
  Serial.println(F("# servo RED->5V BROWN->GND ORANGE->D2/D7/D12"));
  Serial.println(F("# sonar VCC->5V GND->GND Trig->A0 Echo->A1"));
  Serial.println(F("# keys: c=continuous on/off  s=one scan  m=center hold"));
  Serial.println(F("#       w=us table  ?=help"));
  Serial.println(F("# map:  # <20cm  * 20-40  + 40-80  - 80-150  . clear"));
  Serial.println(F("# -------------------------------"));
}

void printTable()
{
  Serial.println(F("# ---- servo angle -> pulse width ----"));
  for (int16_t d = 0; d <= 180; d += 15)
  {
    Serial.print(F("# "));
    if (d < 100) { Serial.print(' '); }
    if (d < 10)  { Serial.print(' '); }
    Serial.print(d);
    Serial.print(F(" deg -> "));
    Serial.print(US_MIN + (uint32_t)(US_MAX - US_MIN) * (uint32_t)d / 180UL);
    Serial.println(F("us"));
  }
  Serial.println(F("# -----------------------------------"));
}

void handleSerial()
{
  while (Serial.available() > 0)
  {
    switch ((char)Serial.read())
    {
      case 'c':
        scanOn = !scanOn;
        Serial.print(F("# scan "));
        Serial.println(scanOn ? F("ON") : F("OFF"));
        break;

      case 's':
        scanOn = false;
        holdMid = false;
        doScan();
        break;

      case 'm':
        scanOn = false;
        holdMid = true;
        servoWrite(90);
        Serial.println(F("# hold center 90deg"));
        break;

      case 'w': printTable(); break;
      case '?': help();       break;
      default: break;
    }
  }
}

void setup()
{
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  for (uint8_t i = 0; i < SERVO_N; i++)
  {
    servo[i].attach(SERVO_PINS[i], US_MIN, US_MAX);
  }
  servoWrite(90);
  delay(400);

  bootMs = millis();
  help();
  Serial.println(F("# holding 90deg for 4s, then auto scan.  m=stop  c=toggle"));
}

void loop()
{
  handleSerial();

  /* 上电静置时间过了就自动开扫。按 m 会置 holdMid，就不会自动开扫了 */
  if (!scanOn && !holdMid && (millis() - bootMs >= AUTO_SCAN_MS))
  {
    scanOn = true;
    Serial.println(F("# auto scan start"));
  }

  if (!scanOn)
  {
    /* 静止时也给个心跳，证明窗口没死 */
    static uint32_t beat = 0;
    if (millis() - beat >= 3000)
    {
      beat = millis();
      Serial.println(F("[holding center 90deg]  press c to scan"));
    }
    return;
  }

  doScan();
}
