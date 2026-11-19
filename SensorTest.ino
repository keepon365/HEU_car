/* ===========================================================================
 *  SensorTest —— 传感器 / 执行器自检固件
 *  ---------------------------------------------------------------------------
 *  它存在的唯一目的：在写"循迹 + 避障 + 超声"的业务逻辑之前，
 *  先把每个模块单独验明正身。
 *
 *  为什么需要它（这是三条硬理由，不是形式主义）：
 *    1) 硬件好 / 代码对，是两件独立的事。混在一起调，出了问题无从判断。
 *    2) 循迹模块的极性不统一：有的"看到黑线输出 LOW"，有的输出 HIGH。
 *       模块上丝印也不写极性，只能实测。猜错则车会"看着黑线往反方向拐"。
 *    3) 舵机的可用脉宽区间每个都不一样。直接给 500us 可能顶到机械限位，
 *       塑料齿轮舵机会立刻堵转发热，几十秒就能烧掉。
 *
 *  ★ 本固件完全不驱动电机。轮子不会转，可以放心架空或放桌上测。
 *
 *  ---------------------------------------------------------------------------
 *  引脚（来自光盘《扩展板原理图.pdf》，已核对与本车固件无冲突）
 *    D4  -> 循迹 左        D3  -> 循迹 右
 *    D7  -> 避障 左        D12 -> 避障 右
 *    A0  -> 超声波 Trig    A1  -> 超声波 Echo
 *    D2  -> 舵机（云台）
 *
 *  注意：原厂把避障接在 D5/D6，但本车用 L298N 带 ENA/ENB，
 *        D5/D6 是电机使能，已被占用。所以改到 D7/D12（原厂的两个舵机口）。
 *        循迹(D3/D4)、超声(A0/A1)、舵机(D2) 在本车全部空闲，不需要改。
 *
 *  ---------------------------------------------------------------------------
 *  串口命令（9600，换行结尾或不用结尾都行）
 *    0   停止所有测试
 *    1   测循迹      —— 打印 D4/D3 电平，只在跳变时输出
 *    2   测避障      —— 打印 D7/D12 电平，只在跳变时输出
 *    3   测超声波    —— 每 100ms 报一次距离（cm）
 *    4   舵机回中位 1500us，并保持
 *    a   舵机自动往返扫动（每 400ms 走 50us）
 *    +   脉宽 +50us        -   脉宽 -50us
 *    m   立即回 1500us
 *    [   软限位下限 -100us   ]   软限位上限 +100us
 *    ?   重新打印引脚表与命令表
 * =========================================================================*/

#include <Arduino.h>

/* ---------------- 引脚 ---------------- */
const uint8_t PIN_TRACK_L = 4;
const uint8_t PIN_TRACK_R = 3;
const uint8_t PIN_AVOID_L = 7;
const uint8_t PIN_AVOID_R = 12;
const uint8_t PIN_US_TRIG = A0;
const uint8_t PIN_US_ECHO = A1;
const uint8_t PIN_SERVO   = 2;

/* ---------------- 舵机 ---------------- */
/* 软限位从"安全区"起步：1000~2000us 几乎所有舵机都能承受。
   用 [ / ] 一点点往外扩，听到嗡嗡声（堵转）就立刻记下该值、退回一格。
   真正的机械极限不是 500/2500，是"开始抖或响"的前一格。 */
uint16_t servoUs  = 1500;
int16_t  servoMin = 1000;
int16_t  servoMax = 2000;

#define SERVO_PERIOD_US   20000UL   // 50Hz
#define SERVO_HARD_MIN     400
#define SERVO_HARD_MAX    2600

volatile bool     servoHigh    = false;
volatile uint32_t servoHighAt  = 0;
uint32_t          servoRiseAt  = 0;

/* ---------------- 运行状态 ---------------- */
char mode = '0';

/* 上一次读到的电平，255 = 还没读过（用于打印初值） */
uint8_t lastTrackL = 255, lastTrackR = 255;
uint8_t lastAvoidL = 255, lastAvoidR = 255;

uint32_t usNextMs = 0;

/* ========================================================================= */

void banner()
{
  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" SensorTest  传感器自检  (不驱动电机)"));
  Serial.println(F("========================================"));
  Serial.println(F("  D4  循迹左      D3  循迹右"));
  Serial.println(F("  D7  避障左      D12 避障右"));
  Serial.println(F("  A0  超声Trig    A1  超声Echo"));
  Serial.println(F("  D2  舵机"));
  Serial.println(F("----------------------------------------"));
  Serial.println(F("  0 停止    1 循迹   2 避障"));
  Serial.println(F("  3 超声波  4 舵机中位   a 自动扫"));
  Serial.println(F("  + / -  脉宽加减 50us    m 回中位"));
  Serial.println(F("  [ / ]  软限位放宽 100us"));
  Serial.println(F("----------------------------------------"));
  Serial.print  (F("  舵机脉宽 = "));
  Serial.print  (servoUs);
  Serial.print  (F("us   软限位 "));
  Serial.print  (servoMin);
  Serial.print  (F(" ~ "));
  Serial.print  (servoMax);
  Serial.println(F("us"));
  Serial.println(F("  提示：先只接一个模块，测完拔掉再接下"));
  Serial.println(F("  一个，避免接错线互相干扰导致误判。"));
  Serial.println(F("========================================"));
  Serial.println();
}

void setup()
{
  Serial.begin(9600);

  pinMode(PIN_TRACK_L, INPUT);
  pinMode(PIN_TRACK_R, INPUT);
  pinMode(PIN_AVOID_L, INPUT);
  pinMode(PIN_AVOID_R, INPUT);

  pinMode(PIN_US_TRIG, OUTPUT);
  pinMode(PIN_US_ECHO, INPUT);
  digitalWrite(PIN_US_TRIG, LOW);

  pinMode(PIN_SERVO, OUTPUT);
  digitalWrite(PIN_SERVO, LOW);

  delay(300);
  banner();
}

/* ---------------------------------------------------------------------------
 *  舵机脉冲：20ms 周期，高电平宽度 = servoUs。
 *  写成非阻塞（micros 计时），这样扫动时主循环还能读串口、读传感器。
 *  UNO 上没有 Servo 库也能跑，不占 Timer1。
 * ------------------------------------------------------------------------- */
void serviceServo()
{
  const uint32_t now = micros();

  if (servoHigh)
  {
    if (now - servoHighAt >= servoUs)
    {
      digitalWrite(PIN_SERVO, LOW);
      servoHigh = false;
    }
    return;
  }

  if (now - servoRiseAt >= SERVO_PERIOD_US)
  {
    servoRiseAt   = now;
    servoHighAt   = now;
    servoHigh     = true;
    digitalWrite(PIN_SERVO, HIGH);
  }
}

bool servoActive() { return (mode == '4' || mode == 'a'); }

void printServo()
{
  Serial.print(F("舵机 "));
  Serial.print(servoUs);
  Serial.print(F("us   ("));
  if (servoUs < servoMin || servoUs > servoMax) { Serial.print(F("超出软限位!")); }
  else                                          { Serial.print(F("ok")); }
  Serial.println(F(")"));
}

/* ---------------------------------------------------------------------------
 *  循迹：两块模块极性可能相反，所以不猜。只打原始电平 + 跳变次数，
 *  由用户把车放白纸上读一次、放黑线上读一次，自己比对出来。
 * ------------------------------------------------------------------------- */
void doTrack()
{
  uint8_t l = digitalRead(PIN_TRACK_L);
  uint8_t r = digitalRead(PIN_TRACK_R);

  if (l != lastTrackL)
  {
    lastTrackL = l;
    Serial.print(F("循迹 左(D4) -> "));
    Serial.print(l ? F("HIGH 1") : F("LOW  0"));
    Serial.println();
  }
  if (r != lastTrackR)
  {
    lastTrackR = r;
    Serial.print(F("循迹 右(D3) -> "));
    Serial.print(r ? F("HIGH 1") : F("LOW  0"));
    Serial.println();
  }
}

void doAvoid()
{
  uint8_t l = digitalRead(PIN_AVOID_L);
  uint8_t r = digitalRead(PIN_AVOID_R);

  if (l != lastAvoidL)
  {
    lastAvoidL = l;
    Serial.print(F("避障 左(D7) -> "));
    Serial.print(l ? F("HIGH 1 (空旷)") : F("LOW  0 (有障碍)"));
    Serial.println();
  }
  if (r != lastAvoidR)
  {
    lastAvoidR = r;
    Serial.print(F("避障 右(D12) -> "));
    Serial.print(r ? F("HIGH 1 (空旷)") : F("LOW  0 (有障碍)"));
    Serial.println();
  }
}

/* ---------------------------------------------------------------------------
 *  超声波：trig 拉高 10us 触发，echo 高电平宽度 / 58 = 厘米。
 *  超时 25ms 约等于 4.3m，超过就报"无回波"（线松了或模块没供电）。
 * ------------------------------------------------------------------------- */
void doUltrasonic()
{
  if (millis() - usNextMs < 100) { return; }
  usNextMs = millis();

  digitalWrite(PIN_US_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(PIN_US_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_US_TRIG, LOW);

  const unsigned long w = pulseIn(PIN_US_ECHO, HIGH, 25000UL);

  if (w == 0)
  {
    Serial.println(F("超声波 无回波（检查 VCC/GND，或超出 4m）"));
    return;
  }

  const float cm = (float)w / 58.0f;
  Serial.print(F("超声波 "));
  Serial.print(cm, 1);
  Serial.print(F(" cm   ("));
  Serial.print(w);
  Serial.println(F("us)"));
}

void handleSerial()
{
  while (Serial.available() > 0)
  {
    const char c = (char)Serial.read();
    switch (c)
    {
      case '0':
        mode = '0';
        Serial.println(F("[模式] 空闲，全部停止"));
        break;

      case '1':
        mode = '1'; lastTrackL = 255; lastTrackR = 255;
        Serial.println(F("[模式] 循迹 —— 白纸上读一次，黑线上读一次，对比极性"));
        break;

      case '2':
        mode = '2'; lastAvoidL = 255; lastAvoidR = 255;
        Serial.println(F("[模式] 避障 —— 手在探头前 5~15cm 晃动，看电平跳变"));
        break;

      case '3':
        mode = '3'; usNextMs = 0;
        Serial.println(F("[模式] 超声波 —— 每 100ms 一次，拿手或书本站远处试"));
        break;

      case '4':
        mode = '4'; servoUs = 1500;
        Serial.println(F("[模式] 舵机中位 1500us（舵机开始保持扭矩，这是正常现象）"));
        printServo();
        break;

      case 'a':
        mode = 'a';
        Serial.println(F("[模式] 舵机自动往返 —— 盯着它，听到嗡嗡声立刻按 0"));
        printServo();
        break;

      case '+':
        if ((int32_t)servoUs + 50 <= SERVO_HARD_MAX) { servoUs += 50; }
        printServo();
        break;

      case '-':
        if ((int32_t)servoUs - 50 >= SERVO_HARD_MIN) { servoUs -= 50; }
        printServo();
        break;

      case 'm':
        servoUs = 1500;
        printServo();
        break;

      case '[':
        if (servoMin - 100 >= SERVO_HARD_MIN) { servoMin -= 100; }
        Serial.print(F("软限位 ")); Serial.print(servoMin);
        Serial.print(F(" ~ "));    Serial.println(servoMax);
        break;

      case ']':
        if (servoMax + 100 <= SERVO_HARD_MAX) { servoMax += 100; }
        Serial.print(F("软限位 ")); Serial.print(servoMin);
        Serial.print(F(" ~ "));    Serial.println(servoMax);
        break;

      case '?':
        banner();
        break;

      case '\r':
      case '\n':
        break;

      default:
        break;
    }
  }
}

void loop()
{
  handleSerial();
  serviceServo();

  /* 超声波用 pulseIn 会阻塞 10~25ms，这段时间舵机会漏脉冲、开始抖。
     所以两者互斥：测超声波时不驱动舵机，反之亦然。 */
  if (servoActive())
  {
    if (mode == 'a')
    {
      static uint32_t sweepMs = 0;
      static int8_t   dir     = 1;
      if (millis() - sweepMs >= 400)
      {
        sweepMs = millis();
        int32_t v = (int32_t)servoUs + dir * 50;
        if (v >= (int32_t)servoMax) { v = servoMax; dir = -1; }
        if (v <= (int32_t)servoMin) { v = servoMin; dir =  1; }
        servoUs = (uint16_t)v;
        printServo();
      }
    }
    return;   // 舵机模式下不跑其它传感器，避免互相干扰
  }

  if      (mode == '1') { doTrack(); }
  else if (mode == '2') { doAvoid(); }
  else if (mode == '3') { doUltrasonic(); }
}
