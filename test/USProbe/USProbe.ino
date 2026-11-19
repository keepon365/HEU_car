/* ===========================================================================
 *  USProbe —— 超声波"到底是哪一环坏了"的分层诊断
 *  ---------------------------------------------------------------------------
 *  UltrasonicTest 只能告诉你"没回波"，本固件把"没回波"拆成 5 个可判定的问题：
 *
 *    [V]   UNO 自身 5V 是否正常（借内部 1.1V 基准反算 VCC）
 *    [P]   Echo 那根线是不是短到 GND 了（拉内部上拉仍读 0 = 短路）
 *    [I]   不触发时 Echo 有没有自己乱跳（长时间 0 说明线真接着东西，
 *          乱跳说明是根悬空天线，根本没插到模块）
 *    [T]   10us / 20us / 50us 三种触发脉宽各打 5 发，看有没有上升沿
 *    [L]   自动识别"回环假象"：Trig 和 Echo 被短接时也会立刻出现一个极短的
 *          高电平，这种情况会被单独标出来，不会误判成测距成功
 *
 *  串口 115200，每 1.5 秒打一行：
 *      V=4980 PULL=1 FLOAT=0 IDLE=0/300 T10=0/5 T20=0/5 T50=0/5
 *
 *  正常（接好模块、前面有东西）应该看到： V=49xx  PULL=1  T10=5/5
 *  完全没接东西：                          IDLE>0（悬空乱跳）  T*=0/5
 *  VCC 只有 3V 左右 → 供电不足，模块不会工作
 *
 *  ★ 不驱动电机，轮子不会转。
 * =========================================================================*/

#include <Arduino.h>

const uint8_t PIN_TRIG = A0;
const uint8_t PIN_ECHO = A1;
const uint8_t PIN_LED  = 13;

const uint8_t  TRIES    = 5;
const uint16_t ECHO_TO  = 25000;   // us，约 4.3m
const uint16_t CYCLE_MS = 1500;

/* ---------------------------------------------------------------------------
 *  用内部 1.1V 带隙基准反算 VCC（mV）。
 *  原理：ADC 参考选 1.1V 内部基准，测它的读数 adc，
 *        则 VCC = 1.1V * 1024 / adc。不需要任何外部元件。
 * ------------------------------------------------------------------------- */
long readVccMv()
{
  ADMUX  = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  delay(2);
  ADCSRA |= _BV(ADSC);
  while (bit_is_set(ADCSRA, ADSC)) { }
  const uint16_t adc = ADC;
  if (adc == 0) { return 0; }
  return 1125300L / (long)adc;
}

/* 拉上拉读一次：返回 0 表示这根线被硬拉在 GND（短路 / 模块把脚拉住） */
uint8_t probePullup()
{
  pinMode(PIN_ECHO, INPUT_PULLUP);
  delay(3);
  return (uint8_t)digitalRead(PIN_ECHO);
}

/* 释放上拉后读一次：悬空线常见结果是随机 0/1 */
uint8_t probeFloat()
{
  digitalWrite(PIN_ECHO, LOW);   // 清掉上拉锁存
  pinMode(PIN_ECHO, INPUT);
  delay(2);
  return (uint8_t)digitalRead(PIN_ECHO);
}

/* 检查 Trig 这一路电气状态：拉上拉后读。
 *   1 = 正常（接的是模块的高阻输入，或被外部上拉）
 *   0 = 这根线被硬拉在 GND —— Trig 永远是低电平，模块根本不会发射，
 *       表现就是"完全没回波"。此时最大嫌疑是插座线序把 A0 排到了 GND 上。 */
uint8_t probeTrig()
{
  pinMode(PIN_TRIG, INPUT_PULLUP);
  delay(3);
  const uint8_t v = (uint8_t)digitalRead(PIN_TRIG);
  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  return v;
}

/* ---------------------------------------------------------------------------
 *  关键判据：给引脚开内部上拉（约 30k 到 5V），再读模拟量。
 *  digitalRead 只能分出"0 或 1"，但 0 有两种成因，处理方式完全不同：
 *
 *    读数 ~0     (0.00V)  → 这根线被【硬短到 GND】。线序错、插反、焊点搭地。
 *    读数 ~100~200(0.5~1V) → 芯片【没通电】。上拉电流经内部 ESD 保护二极管
 *                            灌进它那条 0V 的 VCC 轨，被钳在 0.7V 左右。
 *                            → 模块 VCC 没接上 / 5V 没送过来。
 *    读数 ~1000  (5V)     → 线是通的、对端是高阻输入，电气上正常。
 *
 *  这个区别是本次排查的核心：一个要改线序，一个要查供电。
 * ------------------------------------------------------------------------- */
uint16_t pullupAdc(uint8_t pin)
{
  pinMode(pin, INPUT_PULLUP);
  delay(5);
  return (uint16_t)analogRead(pin);
}

void reportLine(uint16_t adc, bool isEcho)
{
  if (adc <= 20)
  {
    /* Echo 空闲时本来就被推挽驱动拉低，所以 0 不代表短路 */
    Serial.print(isEcho ? F("推挽拉低") : F("短地"));
  }
  else if (adc <= 400) { Serial.print(F("无供电钳位")); }
  else                 { Serial.print(F("正常高阻")); }
}

/* 不触发，纯采样：数 300 次里有多少次读到 HIGH */
uint16_t probeIdle()
{
  uint16_t hi = 0;
  for (uint16_t i = 0; i < 300; i++)
  {
    if (digitalRead(PIN_ECHO)) { hi++; }
    delayMicroseconds(20);
  }
  return hi;
}

/* 打一发，返回回波宽度（us）；0 = 完全没上升沿 */
uint16_t fire(uint8_t trigUs)
{
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(trigUs);
  digitalWrite(PIN_TRIG, LOW);

  const unsigned long us = pulseIn(PIN_ECHO, HIGH, ECHO_TO);
  return (us > 65535UL) ? 65535 : (uint16_t)us;
}

/* 同一脉宽连打 TRIES 发，返回命中的次数，并记录最短/最长脉宽 */
uint8_t burst(uint8_t trigUs, uint16_t &minUs, uint16_t &maxUs)
{
  uint8_t hit = 0;
  minUs = 65535;
  maxUs = 0;
  for (uint8_t i = 0; i < TRIES; i++)
  {
    const uint16_t us = fire(trigUs);
    if (us > 0)
    {
      hit++;
      if (us < minUs) { minUs = us; }
      if (us > maxUs) { maxUs = us; }
    }
    delay(70);                   // 手册要求间隔 >= 60ms
  }
  if (hit == 0) { minUs = 0; maxUs = 0; }
  return hit;
}

void setup()
{
  Serial.begin(115200);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);

  delay(400);
  Serial.println();
  Serial.println(F("### US PROBE v1 ###"));
  Serial.println(F("Trig=A0  Echo=A1  VCC=5V  GND"));
  Serial.println(F("V= mVolt  PULL=1正常/0短地  IDLE=悬空跳变数"));
  Serial.println(F("T10/T20/T50 = 各5发里收到几次回波"));
  Serial.println(F("------------------------------------------"));
}

void loop()
{
  const long vcc = readVccMv();

  /* 先做电气体检：开上拉 + 读模拟量，区分"短地"与"模块无供电" */
  const uint16_t adcTrig = pullupAdc(PIN_TRIG);
  const uint16_t adcEcho = pullupAdc(PIN_ECHO);

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);
  delay(5);

  const uint16_t idle = probeIdle();

  uint16_t mn10, mx10, mn20, mx20, mn50, mx50;
  const uint8_t h10 = burst(10, mn10, mx10);
  const uint8_t h20 = burst(20, mn20, mx20);
  const uint8_t h50 = burst(50, mn50, mx50);

  Serial.print(F("V="));
  Serial.print(vcc);
  Serial.print(F(" Trig="));
  Serial.print(adcTrig);
  Serial.print(F("("));
  reportLine(adcTrig, false);
  Serial.print(F(") Echo="));
  Serial.print(adcEcho);
  Serial.print(F("("));
  reportLine(adcEcho, true);
  Serial.print(F(") IDLE="));
  Serial.print(idle);
  Serial.print(F("/300 T10="));
  Serial.print(h10);
  Serial.print(F("/5 T20="));
  Serial.print(h20);
  Serial.print(F("/5 T50="));
  Serial.print(h50);
  Serial.print(F("/5"));

  const uint8_t best = (h10 > h20) ? ((h10 > h50) ? h10 : h50)
                                   : ((h20 > h50) ? h20 : h50);

  if (best > 0)
  {
    digitalWrite(PIN_LED, HIGH);

    const uint16_t mn = (h50 > 0) ? mn50 : ((h20 > 0) ? mn20 : mn10);
    const uint16_t mx = (h50 > 0) ? mx50 : ((h20 > 0) ? mx20 : mx10);

    Serial.print(F("  ECHO "));
    Serial.print(mn);
    Serial.print(F("-"));
    Serial.print(mx);
    Serial.print(F("us ~ "));
    Serial.print(mn / 58.0f, 1);
    Serial.print(F("-"));
    Serial.print(mx / 58.0f, 1);
    Serial.print(F("cm"));

    /* 回环假象：脉宽极短且几乎等于触发宽度，多半是 Trig/Echo 被短接 */
    if (mx < 40) { Serial.print(F("  [LOOP? Trig/Echo 可能被短接]")); }
    Serial.println();
  }
  else
  {
    digitalWrite(PIN_LED, LOW);
    Serial.print(F("  NO ECHO"));

    if (vcc < 4300)                 { Serial.print(F("  <UNO 供电不足>")); }

    if (adcTrig <= 400)
    {
      /* Trig 被钳在 0.5~2V：模块电源轨是 0V，被上拉电流充起来的 */
      Serial.print(F("  <模块无供电: Trig 被钳位, 查 VCC 那根线>"));
    }
    else if (adcTrig >= 700)
    {
      Serial.print(F("  <Trig 线路正常>"));
      if (adcEcho <= 20)
      {
        Serial.print(F("  <Echo 被推挽拉低=模块已通电, 但触发后无上升沿>"));
      }
      else
      {
        Serial.print(F("  <Echo 无上升沿>"));
      }
    }

    if (idle > 20 && adcTrig >= 700) { Serial.print(F("  <Echo 悬空: 线没插到模块>")); }
    Serial.println();
  }

  delay(CYCLE_MS);
}
