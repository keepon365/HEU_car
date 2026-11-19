/* ===========================================================================
 *  DiagUS —— 超声波"无回波"自动诊断
 *  ---------------------------------------------------------------------------
 *  背景：接上模块后固件一直报 NO ECHO。可能的原因有四种，肉眼分不出来：
 *    ① 模块没供电（VCC/GND 断、插反）
 *    ② Trig 或 Echo 断了 / 插错位置
 *    ③ Trig 和 Echo 接反了
 *    ④ 模块坏了
 *
 *  这个固件做三件事：
 *    1) 测两个脚的静止电平占比（悬空会浮在 20~80%，接死会是 0% 或 100%）
 *    2) 自动尝试两种 Trig/Echo 组合，看哪一组能测出回波
 *    3) 用高速采样抓"触发后有没有上升沿"，区分"模块没反应"和"模块有反应但脉宽不对"
 *
 *  ★ 不驱动电机。
 * =========================================================================*/

#include <Arduino.h>

const uint8_t A_PIN = A0;
const uint8_t B_PIN = A1;

/* 静止电平占比（0~100） */
uint8_t staticRatio(uint8_t pin)
{
  pinMode(pin, INPUT);
  delay(5);
  uint16_t hi = 0;
  for (uint16_t i = 0; i < 300; i++)
  {
    hi += (digitalRead(pin) == HIGH) ? 1 : 0;
    delayMicroseconds(300);          // 共约 90ms
  }
  return (uint8_t)((hi * 100UL) / 300);
}

/* 发一次触发，返回回波宽度 us；0 = 没收到 */
uint32_t ping(uint8_t trig, uint8_t echo)
{
  pinMode(trig, OUTPUT);
  pinMode(echo, INPUT);
  digitalWrite(trig, LOW);
  delayMicroseconds(3);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  return pulseIn(echo, HIGH, 25000UL);
}

/* 触发后 30ms 内是否有上升沿（用来区分"模块完全不动"和"动了但宽度不对"） */
bool sawRise(uint8_t trig, uint8_t echo, uint32_t *riseUs)
{
  pinMode(trig, OUTPUT);
  pinMode(echo, INPUT);
  digitalWrite(trig, LOW);
  delayMicroseconds(3);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);

  const uint32_t t0 = micros();
  while (micros() - t0 < 30000UL)
  {
    if (digitalRead(echo) == HIGH)
    {
      if (riseUs) { *riseUs = micros() - t0; }
      return true;
    }
  }
  return false;
}

const char* judgeStatic(uint8_t r)
{
  if (r >= 97)  { return "常高(100%)  -> 可能短到5V / 插错脚"; }
  if (r <= 3)   { return "常低(0%)    -> 正常 或 线断(悬空通常不是0%)"; }
  if (r >= 20 && r <= 80) { return "浮空乱跳    -> 这个脚没接线!"; }
  return "不稳定      -> 接触不良";
}

void testPair(uint8_t trig, uint8_t echo, const char *name)
{
  Serial.println();
  Serial.println(F("----------------------------------------"));
  Serial.print  (F("组合: "));
  Serial.println(name);

  /* 采样前先把 trig 设成输出低，避免它悬空干扰 */
  pinMode(trig, OUTPUT);
  digitalWrite(trig, LOW);
  delay(5);

  const uint8_t rTrigStatic = staticRatio(trig);
  const uint8_t rEchoStatic = staticRatio(echo);

  Serial.print(F("  静止电平  Trig脚="));
  Serial.print(rTrigStatic);
  Serial.print(F("%   Echo脚="));
  Serial.print(rEchoStatic);
  Serial.println(F("%"));

  delay(5);

  uint32_t rise = 0;
  const bool  got  = sawRise(trig, echo, &rise);
  const uint32_t us = ping(trig, echo);

  Serial.print(F("  触发后上升沿: "));
  if (got)
  {
    Serial.print(F("有，出现在 "));
    Serial.print(rise);
    Serial.println(F("us 处"));
  }
  else
  {
    Serial.println(F("没有"));
  }

  Serial.print(F("  回波宽度: "));
  if (us > 0)
  {
    Serial.print(us);
    Serial.print(F("us  ≈ "));
    Serial.print((float)us / 58.0f, 1);
    Serial.println(F(" cm   <<< 这一组能用!"));
  }
  else
  {
    Serial.println(F("0 (超时)"));
  }
}

void setup()
{
  Serial.begin(9600);
  delay(600);

  Serial.println();
  Serial.println(F("======== 超声波自动诊断 ========"));

  /* 先看两个脚在"完全不驱动"时是什么状态 */
  Serial.println(F("[1] 两个脚都设为 INPUT，看悬空情况"));
  pinMode(A_PIN, INPUT);
  pinMode(B_PIN, INPUT);
  delay(20);
  Serial.print(F("    A0 静止 "));
  Serial.print(staticRatio(A_PIN));
  Serial.println(F("%"));
  Serial.print(F("    A1 静止 "));
  Serial.print(staticRatio(B_PIN));
  Serial.println(F("%"));

  /* 两种接线组合各试一次 */
  Serial.println();
  Serial.println(F("[2] 组合测试（正常接线是第 1 组）"));
  testPair(A_PIN, B_PIN, "A0=Trig   A1=Echo");

  delay(80);
  testPair(B_PIN, A_PIN, "A1=Trig   A0=Echo  (接反的情况)");

  /* 结论 */
  Serial.println();
  Serial.println(F("======== 判定 ========"));
  const uint32_t okAB = ping(A_PIN, B_PIN);
  delay(80);
  const uint32_t okBA = ping(B_PIN, A_PIN);

  if (okAB > 0)
  {
    Serial.println(F("OK  A0=Trig / A1=Echo 工作正常"));
    Serial.print  (F("    距离 ≈ "));
    Serial.print  ((float)okAB / 58.0f, 1);
    Serial.println(F(" cm"));
    Serial.println(F("    -> 接线没错，之前无回波是别的原因(见下方提示)"));
  }
  else if (okBA > 0)
  {
    Serial.println(F("!!  Trig / Echo 接反了"));
    Serial.print  (F("    把 A0 和 A1 两根线对调即可，距离 ≈ "));
    Serial.print  ((float)okBA / 58.0f, 1);
    Serial.println(F(" cm"));
  }
  else
  {
    Serial.println(F("XX  两组都测不到回波。按顺序查："));
    Serial.println(F("   1) 模块 VCC 是否 5V、GND 是否通（量电压）"));
    Serial.println(F("   2) 模块上有红灯/小灯吗？触发时是否闪一下"));
    Serial.println(F("   3) 4P 座线序：5V|Trig|Echo|GND，不是 5V|GND|.."));
    Serial.println(F("   4) 探头前方 20~100cm 内要有平整硬物，别对着空气"));
    Serial.println(F("   5) 换一组杜邦线，杜邦线断芯很常见"));
  }
  Serial.println(F("================================"));
  Serial.println(F("(诊断结束，此后不再输出，按 RST 可重跑)"));
}

void loop()
{
  /* 诊断只跑一次，避免刷屏 */
  delay(1000);
}
