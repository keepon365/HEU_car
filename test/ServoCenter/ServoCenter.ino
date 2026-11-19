/* ===========================================================================
 *  ServoCenter —— 180° 舵机对中 / 找零位 / 量行程
 *  ---------------------------------------------------------------------------
 *  上电直接把舵机打到 90°（也就是 180° 舵机的机械中位）。之后可以在窗口里
 *  用键盘一点点调，找到你真正想要的"零位"，读出来的 us 值就是标定结果。
 *
 *  ---------------------------------------------------------------------------
 *  ★ 三个引脚同时输出，你不用告诉我是哪个
 *      D2 / D7 / D12 都挂了一个虚拟舵机，输出角度完全一致。
 *      舵机接在这三个里任意一个都会跟着动。
 *      （D3 给红外，D4 给循迹，D5/D6 是电机 PWM，D9/D10 是电机方向，
 *        这几个都不能给舵机，会互相打架）
 *
 *  ★ 如果接上完全不动，先查供电再查信号
 *      舵机是吃电流的（SG90 空载约 100mA，堵转能到 700mA）。
 *      扩展板上那个舵机口的 5V 很可能和超声波口是同一路（我们已经测出那一路
 *      不供电）。所以：直接把舵机的红线接到 UNO 的 5V、棕/黑线接到 UNO 的 GND，
 *      橙/黄线（信号）接 D2 或 D7 或 D12。四根杜邦线，别走扩展板。
 *
 *  ★ 定时器冲突已确认无问题
 *      电机 PWM 在 D5/D6 走 Timer0，蜂鸣器 tone() 走 Timer2，
 *      Servo 库只占 Timer1。三者互不干扰。
 *      （代价：D9/D10 会失去 analogWrite 能力，但这两个脚在本项目里只当方向用）
 *
 *  ---------------------------------------------------------------------------
 *  窗口里敲字母（敲完直接生效，不用回车）
 *
 *      +  /  -        角度 ±5°
 *      ]  /  [        角度 ±1°   （微调找零位用这个）
 *
 *      0  m  z        直接跳到 0° / 90°(中位) / 180°
 *
 *      1  2  3        把输出切到 D2 / D7 / D12（默认三个都输出，一般不用切）
 *      4              切到 D9（⚠ 会和电机方向脚抢，调试电机时别用）
 *
 *      s              来回扫一遍 0-180，看有没有卡住/异响
 *      w              每 10° 停一下，打印对应 us（量行程用）
 *      ?              重印说明
 *
 *  ---------------------------------------------------------------------------
 *  ★ 对中的正确做法
 *      1) 上电，舵机应该立刻转到 90°。
 *      2) 这时候不要用螺丝刀去拧齿轮——先确认它真的是"正前方"。
 *         若方向偏了，用 + / - 找到你想要的朝向，记下屏幕上的 us 值。
 *      3) 那个 us 就是你的零位标定值，之后写避障代码直接用 writeMicroseconds()。
 *
 *  ★ 如果 0° 或 180° 处有"咔咔"齿轮声或持续嗡嗡响：说明超出了该舵机的
 *    实际行程（便宜的 180° 舵机常年做不到满 180°）。
 *    用 w 量一遍，把卡住的那一段砍掉，别硬顶，会烧。
 * =========================================================================*/

#include <Servo.h>

/* ---- 候选输出引脚（三个同时输出，接哪个都动） ---- */
const uint8_t PIN_SERVO_A = 2;
const uint8_t PIN_SERVO_B = 7;
const uint8_t PIN_SERVO_C = 12;
const uint8_t PIN_SERVO_D = 9;      // 备用，默认不挂

/* 脉宽范围。500~2500us 是大多数 180° 舵机的全行程；
 * 若两端有异响，把这两个数往中间收（例如 700~2300） */
const uint16_t US_MIN = 500;
const uint16_t US_MAX = 2500;

Servo sA, sB, sC, sD;
bool useA = true, useB = true, useC = true, useD = false;

int16_t angle = 90;                 // 当前角度（度）

/* ------------------------------------------------------------------------- */

uint16_t angleToUs(int16_t deg)
{
  if (deg < 0)   { deg = 0; }
  if (deg > 180) { deg = 180; }
  const uint32_t span = (uint32_t)(US_MAX - US_MIN);
  return (uint16_t)(US_MIN + span * (uint32_t)deg / 180UL);
}

void applyAngle()
{
  if (angle < 0)   { angle = 0; }
  if (angle > 180) { angle = 180; }

  const uint16_t us = angleToUs(angle);

  if (useA) { sA.writeMicroseconds(us); }
  if (useB) { sB.writeMicroseconds(us); }
  if (useC) { sC.writeMicroseconds(us); }
  if (useD) { sD.writeMicroseconds(us); }
}

void report()
{
  Serial.print(F(">> "));
  Serial.print(angle);
  Serial.print(F(" deg   "));
  Serial.print(angleToUs(angle));
  Serial.print(F("us   pins:"));
  if (useA) { Serial.print(F(" D2")); }
  if (useB) { Serial.print(F(" D7")); }
  if (useC) { Serial.print(F(" D12")); }
  if (useD) { Serial.print(F(" D9")); }
  Serial.println();
}

void help()
{
  Serial.println();
  Serial.println(F("# ==== SERVO CENTER ===="));
  Serial.println(F("# power: servo VCC -> UNO 5V, GND -> UNO GND"));
  Serial.println(F("# signal: D2 or D7 or D12 (all driven, any works)"));
  Serial.println(F("# keys:  + -  = 5deg     ] [  = 1deg"));
  Serial.println(F("#        0 m z = 0 / 90 / 180"));
  Serial.println(F("#        1 2 3 = only D2 / D7 / D12    4 = also D9"));
  Serial.println(F("#        s = sweep test   w = us table   ? = help"));
  Serial.println(F("# -------------------------------"));
}

/* 扫一遍全行程，看有没有卡死或者打齿 */
void sweepTest()
{
  Serial.println(F("# sweep 0 -> 180 -> 0"));
  for (int16_t d = 0; d <= 180; d += 10)
  {
    angle = d;
    applyAngle();
    delay(120);
  }
  for (int16_t d = 180; d >= 0; d -= 10)
  {
    angle = d;
    applyAngle();
    delay(120);
  }
  angle = 90;
  applyAngle();
  report();
}

/* 每 10° 打一行 us，用来量实际行程 */
void printTable()
{
  Serial.println(F("# ---- angle -> pulse width ----"));
  for (int16_t d = 0; d <= 180; d += 10)
  {
    Serial.print(F("# "));
    if (d < 100) { Serial.print(' '); }
    if (d < 10)  { Serial.print(' '); }
    Serial.print(d);
    Serial.print(F(" deg  ->  "));
    Serial.print(angleToUs(d));
    Serial.println(F("us"));
  }
  Serial.println(F("# ------------------------------"));
}

void handleSerial()
{
  while (Serial.available() > 0)
  {
    const char c = (char)Serial.read();
    bool changed = true;

    switch (c)
    {
      case '+': angle += 5;  break;
      case '-': angle -= 5;  break;
      case ']': angle += 1;  break;
      case '[': angle -= 1;  break;
      case '0': angle = 0;   break;
      case 'm': angle = 90;  break;
      case 'z': angle = 180; break;

      case '1':
        useA = true; useB = false; useC = false;
        break;
      case '2':
        useA = false; useB = true; useC = false;
        break;
      case '3':
        useA = false; useB = false; useC = true;
        break;
      case '4':
        useD = !useD;
        if (useD) { sD.attach(PIN_SERVO_D, US_MIN, US_MAX); }
        else      { sD.detach(); }
        break;

      case 's': sweepTest(); changed = false; break;
      case 'w': printTable(); changed = false; break;
      case '?': help();       changed = false; break;

      default: changed = false; break;
    }

    if (changed)
    {
      applyAngle();
      report();
    }
  }
}

void setup()
{
  Serial.begin(115200);

  sA.attach(PIN_SERVO_A, US_MIN, US_MAX);
  sB.attach(PIN_SERVO_B, US_MIN, US_MAX);
  sC.attach(PIN_SERVO_C, US_MIN, US_MAX);

  angle = 90;
  applyAngle();

  delay(200);
  help();
  report();
  Serial.println(F("# servo should now sit at 90 deg (center)"));
}

void loop()
{
  handleSerial();

  /* 心跳：每 2 秒打一行，证明窗口还活着、舵机还停在这个角度上。
     不做这件事的话，命令式固件在窗口里是"一片空白"，看起来像死了。 */
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat >= 2000)
  {
    lastBeat = millis();
    Serial.print(F("["));
    Serial.print(angle);
    Serial.print(F(" deg "));
    Serial.print(angleToUs(angle));
    Serial.print(F("us]"));
    if (useA) { Serial.print(F(" D2")); }
    if (useB) { Serial.print(F(" D7")); }
    if (useC) { Serial.print(F(" D12")); }
    if (useD) { Serial.print(F(" D9")); }
    Serial.println();
  }
}
