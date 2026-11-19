/* ===========================================================================
 *  PinScan —— 超声波"线到底接在哪个脚"自动扫描
 *  ---------------------------------------------------------------------------
 *  什么时候用：固件报 NO ECHO，但你不确定线有没有插到 A0/A1。
 *
 *  原理：把每一对可能空闲的引脚都当成 (Trig, Echo) 试一遍，
 *        哪个组合能收到回波，线就在那两个脚上。
 *
 *  能解决的三种情况：
 *    · 线插错了一排（比如插到 A2/A3）
 *    · 扩展板不是 ZYV6，A0/A1 的标注和实际不符
 *    · Trig / Echo 接反（会同时报出 A1=Trig A0=Echo 的组合）
 *
 *  ★ 不驱动电机。扫描只在数字脚上发 10us 脉冲，不会让轮子转。
 *    已刻意跳过 A4(红外接收) 与 D5/D6/D8-D11(电机)，避免任何误动作。
 *
 *  接线前提：模块 VCC 必须真的接着 5V、GND 必须通，否则扫不出来。
 * =========================================================================*/

#include <Arduino.h>

/* 候选引脚：A0~A3、A5 与常用数字脚，全部在本车空闲 */
const uint8_t CAND[] = { A0, A1, A2, A3, A5, 2, 3, 4, 7, 12, 13 };
const uint8_t NCAND  = sizeof(CAND) / sizeof(CAND[0]);

const uint32_t GAP_MS = 60;      // 组间间隔（HC-SR04 需要 >=60ms 消余波）

uint8_t foundTrig = 255, foundEcho = 255;
uint32_t foundUs  = 0;
uint8_t hits      = 0;

uint32_t ping(uint8_t trig, uint8_t echo)
{
  pinMode(trig, OUTPUT);
  digitalWrite(trig, LOW);
  pinMode(echo, INPUT);
  delayMicroseconds(3);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  return pulseIn(echo, HIGH, 25000UL);
}

void setup()
{
  Serial.begin(9600);

  /* 留 8 秒，让你有时间打开串口监视器并摆好物体 */
  for (int8_t i = 8; i > 0; i--)
  {
    Serial.print(F("PinScan 将在 "));
    Serial.print(i);
    Serial.println(F(" 秒后开始 —— 请保持接线不动，探头前方 50cm 内放本书或纸板"));
    delay(1000);
  }

  Serial.println();
  Serial.println(F("======== 开始扫描 ========"));
  Serial.println(F("组合数 = 11 x 10 = 110，约 7 秒"));
  Serial.println();

  uint8_t tried = 0;

  for (uint8_t i = 0; i < NCAND; i++)
  {
    for (uint8_t j = 0; j < NCAND; j++)
    {
      if (i == j) { continue; }

      const uint8_t t = CAND[i];
      const uint8_t e = CAND[j];

      const uint32_t us = ping(t, e);
      tried++;

      if (us > 0)
      {
        hits++;
        Serial.print(F("  >>> 命中  Trig="));
        Serial.print(t);
        Serial.print(F("  Echo="));
        Serial.print(e);
        Serial.print(F("  ->  "));
        Serial.print((float)us / 58.0f, 1);
        Serial.print(F(" cm  ("));
        Serial.print(us);
        Serial.println(F("us)"));

        if (hits == 1)
        {
          foundTrig = t;
          foundEcho = e;
          foundUs   = us;
        }
      }

      /* 每 20 组打一个进度点，避免你以为卡死了 */
      if (tried % 20 == 0)
      {
        Serial.print(F("... 已试 "));
        Serial.print(tried);
        Serial.println(F(" 组"));
      }

      delay(GAP_MS);
    }
  }

  /* ---------------- 结论 ---------------- */
  Serial.println();
  Serial.println(F("======== 结论 ========"));

  if (hits == 0)
  {
    Serial.println(F("XX  110 组全部无回波，说明问题不在 Arduino 侧："));
    Serial.println(F("    · 模块没供电：量 VCC-GND 之间是不是 5V"));
    Serial.println(F("      （扩展板 5VO 那一位，插反就是 0V）"));
    Serial.println(F("    · 模块 4P 线里 VCC 或 GND 那根断了（杜邦线断芯极常见）"));
    Serial.println(F("    · 模块本身坏了：换一个模块，或把 Trig/Echo 短接"));
    Serial.println(F("      到 A0/A1 手测（Trig 接 A0、Echo 接 A1 是标准接法）"));
    Serial.println(F("    · 探头前方真的没有反射物（>4m），拿本书贴近 30cm 再扫"));
  }
  else if (hits == 1)
  {
    Serial.println(F("OK  唯一命中的组合就是真实接线："));
    Serial.print  (F("    Trig = "));
    Serial.print  (foundTrig);
    Serial.print  (F("   Echo = "));
    Serial.println(foundEcho);
    Serial.print  (F("    距离 = "));
    Serial.print  ((float)foundUs / 58.0f, 1);
    Serial.println(F(" cm"));

    if (foundTrig == A1 && foundEcho == A0)
    {
      Serial.println(F("    !! 这是接反的情况，把 A0/A1 两根线对调即可"));
    }
    else if (foundTrig != A0 || foundEcho != A1)
    {
      Serial.println(F("    !! 不是 A0/A1，说明线插在了别的脚上，"));
      Serial.println(F("       或者这块扩展板不是 ZYV6。按上面报的脚改代码即可。"));
    }
    else
    {
      Serial.println(F("    与原理图一致，无回波是别的原因（探物体/距离）"));
    }
  }
  else
  {
    Serial.println(F("??  多个组合都能测到（串扰），优先采信第一个："));
    Serial.print  (F("    Trig = "));
    Serial.print  (foundTrig);
    Serial.print  (F("   Echo = "));
    Serial.println(foundEcho);
    Serial.println(F("    通常说明这两个脚之间有飞线/短接，或模块 Echo 接了多个脚。"));
  }

  Serial.println(F("======================"));
  Serial.println(F("(扫描结束，按 RST 可重跑)"));
}

void loop()
{
  delay(1000);
}
