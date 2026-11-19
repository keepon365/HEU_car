/* ===========================================================================
 *  Servo360 —— 左 / 右 360° 舵机（连续旋转）角度控制
 *  ---------------------------------------------------------------------------
 *  ★ 接线（已实测确认，2026-11-17）
 *        左舵机 = D2
 *        右舵机 = D7
 *        D12 备用，目前空着
 *
 *  ★ 360° 舵机没有位置反馈，所谓「转多少度」是**时间算出来的**：
 *        时长(ms) = 角度 × 1000 ÷ dps          dps = 每秒转多少度
 *    所以必须先把 dps 标定准，否则每次都差一点，转多次会累积。
 *    标定方法见下面命令 k。
 *
 *  ★ 极性（实测，和常见资料相反，别按资料改）
 *        1500us      = 停
 *        < 1500us    = 顺时针
 *        > 1500us    = 逆时针
 *    代码里「正角度 = 顺时针」，内部自动映射，调用方不用记。
 *
 *  ---------------------------------------------------------------------------
 *  串口 9600，命令立即生效：
 *
 *      L90        左舵机 顺时针转 90 度
 *      L-45       左舵机 逆时针转 45 度
 *      R90        右舵机 顺时针转 90 度
 *      R-45       右舵机 逆时针转 45 度
 *      LR90       两颗同时转 90 度
 *
 *      k          标定：左舵机顺时针转 3000ms（数转了几圈，圈数×360÷3 = dps）
 *      t1500      不换算，直接让左舵机转 1500ms
 *      u300       设 dps（60~900）
 *
 *      + / -      速度幅度 ±25us（100~500，越大越快）
 *      s          全停
 *      ?          状态    h  帮助
 *
 *  ⚠ 如果装的是 180° 舵机，别用本固件 —— 持续给偏离 1500 的脉宽会顶死打齿。
 * =========================================================================*/

#include <Servo.h>

const uint8_t PIN_LEFT  = 2;
const uint8_t PIN_RIGHT = 7;
const uint8_t PIN_CAM   = 12;    // 云台：180° 舵机（和上面两颗不是一回事）

Servo srvL;
Servo srvR;
Servo srvCam;

const uint16_t US_STOP = 1500;
const uint16_t AMP_MIN = 100;
const uint16_t AMP_MAX = 500;

/* ★ 标定结果（2026-11-17 实测）
 *      幅度 175us（脉宽 1325us）下，左舵机 4.0 秒转 3.75 圈
 *      → 3.75 × 360 ÷ 4 = 337.5 度/秒      取 337
 *  ⚠ dps 只在「标定时那个幅度」下成立。改了 +/- 幅度，转速就变了。
 *    这里按幅度线性缩放做近似补偿（ampCal 记的就是标定时的幅度），
 *    小幅调整（±25us）够用，大改仍建议重标一次。 */
uint16_t usAmp   = 175;    // 工作幅度：离 1500 的脉宽差，越大转得越快
uint16_t ampCal  = 175;    // 标定时用的幅度
uint16_t dpsCal  = 337;    // 该幅度下的实测角速度（度/秒）

uint32_t untilL = 0;       // 0 = 停；否则为应该停下的时刻
uint32_t untilR = 0;

uint8_t  camDeg = 90;      // 云台当前目标角度（180° 舵机专用）

char     cmdBuf[12];
uint8_t  cmdLen = 0;

/* 顺时针 = 1500 以下，逆时针 = 1500 以上 */
uint16_t pulseCW(uint16_t amp)  { return (uint16_t)(US_STOP > amp ? US_STOP - amp : 1000); }
uint16_t pulseCCW(uint16_t amp) { return (uint16_t)(US_STOP + amp); }

void stopL() { srvL.writeMicroseconds(US_STOP); untilL = 0; }
void stopR() { srvR.writeMicroseconds(US_STOP); untilR = 0; }

/* 当前幅度下的等效角速度（按幅度线性缩放，近似） */
uint16_t dpsNow()
{
  if (ampCal == 0) { return dpsCal; }
  uint32_t d = (uint32_t)dpsCal * usAmp / ampCal;
  if (d <  30) { d =  30; }
  if (d > 1200) { d = 1200; }
  return (uint16_t)d;
}

/* 角度 → 毫秒。deg 正 = 顺时针，负 = 逆时针 */
uint32_t degToMs(int16_t deg)
{
  const uint16_t a = (uint16_t)((deg < 0) ? -deg : deg);
  const uint16_t d = dpsNow();
  uint32_t ms = (uint32_t)a * 1000UL / (uint32_t)d;
  if (ms < 20) { ms = 20; }          // 太短舵机来不及响应
  return ms;
}

void spinDeg(char which, int16_t deg)
{
  if (deg == 0) { return; }
  const uint16_t us   = (deg > 0) ? pulseCW(usAmp) : pulseCCW(usAmp);
  const uint32_t ms   = degToMs(deg);
  const uint32_t till = millis() + ms;

  if (which == 'L' || which == 'B')
  {
    srvL.writeMicroseconds(us);
    untilL = till;
  }
  if (which == 'R' || which == 'B')
  {
    srvR.writeMicroseconds(us);
    untilR = till;
  }

  Serial.print(F("[360] "));
  if (which == 'L')      { Serial.print(F("左")); }
  else if (which == 'R') { Serial.print(F("右")); }
  else                   { Serial.print(F("左右")); }
  Serial.print(F("舵机 "));
  Serial.print(deg > 0 ? F("顺") : F("逆"));
  Serial.print(F("时针 "));
  Serial.print(deg > 0 ? deg : -deg);
  Serial.print(F("度  "));
  Serial.print(us);
  Serial.print(F("us x "));
  Serial.print(ms);
  Serial.println(F("ms"));
}

void report()
{
  Serial.print(F("[360] 左=D"));
  Serial.print(PIN_LEFT);
  Serial.print(F(" 右=D"));
  Serial.print(PIN_RIGHT);
  Serial.print(F("  幅度="));
  Serial.print(usAmp);
  Serial.print(F("us  dps="));
  Serial.print(dpsNow());
  Serial.print(F("  左:"));
  Serial.print(untilL ? F("转") : F("停"));
  Serial.print(F(" 右:"));
  Serial.print(untilR ? F("转") : F("停"));
  Serial.print(F("  云台="));
  Serial.print(camDeg);
  Serial.println(F("度"));
}

void help()
{
  Serial.println(F("=============================================="));
  Serial.println(F(" 左舵机=D2   右舵机=D7   正角度=顺时针"));
  Serial.println(F(" L90 / L-45     左舵机 顺/逆时针 转 90/45 度"));
  Serial.println(F(" R90 / R-45     右舵机"));
  Serial.println(F(" LR90           两颗同时"));
  Serial.println(F(" k              标定：左转 3000ms（数圈数）"));
  Serial.println(F(" t1500          不换算，直接转 1500ms"));
  Serial.println(F(" u300           设 dps（度/秒）"));
  Serial.println(F(" C90            云台(D12,180舵机) 转到 90 度"));
  Serial.println(F(" +/-  调速   s  全停   ?  状态"));
  Serial.println(F("=============================================="));
}

void setup()
{
  Serial.begin(9600);
  delay(200);

  srvL.attach(PIN_LEFT);
  srvR.attach(PIN_RIGHT);
  srvCam.attach(PIN_CAM);
  srvL.writeMicroseconds(US_STOP);
  srvR.writeMicroseconds(US_STOP);
  srvCam.write(camDeg);          // 云台先回中位

  help();
  report();
}

void handleCmd()
{
  cmdBuf[cmdLen] = '\0';
  cmdLen = 0;

  if (cmdBuf[0] == '\0') { return; }

  const char k0 = (char)((cmdBuf[0] >= 'a' && cmdBuf[0] <= 'z') ? cmdBuf[0] - 32 : cmdBuf[0]);

  /* 数字部分（支持负号） */
  int16_t v = 0;
  uint8_t i = 1;
  bool neg = false;
  bool hasNum = false;
  if (cmdBuf[i] == '-') { neg = true; i++; }
  for (; cmdBuf[i] >= '0' && cmdBuf[i] <= '9'; i++)
  {
    v = (int16_t)(v * 10 + (cmdBuf[i] - '0'));
    hasNum = true;
  }
  if (neg) { v = (int16_t)(-v); }

  switch (k0)
  {
    case 'L':
      /* LR90 这种两字母写法 */
      if (cmdBuf[1] == 'R' || cmdBuf[1] == 'r')
      {
        int16_t w = 0;
        uint8_t j = 2;
        bool n2 = false;
        if (cmdBuf[j] == '-') { n2 = true; j++; }
        for (; cmdBuf[j] >= '0' && cmdBuf[j] <= '9'; j++) { w = (int16_t)(w * 10 + (cmdBuf[j] - '0')); }
        if (n2) { w = (int16_t)(-w); }
        spinDeg('B', w);
        break;
      }
      spinDeg('L', v);
      break;

    case 'R': spinDeg('R', v); break;

    case 'T':
      if (hasNum && v > 0)
      {
        srvL.writeMicroseconds(pulseCW(usAmp));
        untilL = millis() + (uint32_t)v;
        Serial.print(F("[360] 左舵机 直接转 "));
        Serial.print(v);
        Serial.println(F("ms"));
      }
      break;

    case 'K':
      srvL.writeMicroseconds(pulseCW(usAmp));
      untilL = millis() + 3000UL;
      Serial.println(F("[360] 标定：左舵机顺时针转 3000ms，数一下转了几圈"));
      Serial.println(F("[360]   dps = 圈数 x 360 / 3    例：2 圈半 -> 300"));
      break;

    case 'C':
      /* 云台是 180° 舵机，参数是绝对角度，不是脉宽 */
      if (hasNum)
      {
        int16_t a = v;
        if (a < 0)   { a = 0;   }
        if (a > 180) { a = 180; }
        camDeg = (uint8_t)a;
        srvCam.write(camDeg);
        Serial.print(F("[360] 云台(D12) -> "));
        Serial.print(camDeg);
        Serial.println(F(" 度"));
      }
      else
      {
        Serial.print(F("[360] 云台当前 "));
        Serial.print(camDeg);
        Serial.println(F(" 度，用 C<0~180> 设定"));
      }
      break;

    case 'U':
      if (hasNum && v >= 60 && v <= 1200)
      {
        dpsCal = (uint16_t)v;
        ampCal = usAmp;          // 在「当前幅度」下给出的实测值
        report();
      }
      break;

    case 'S': stopL(); stopR(); report(); break;

    case '+':
      if (usAmp + 25 <= AMP_MAX) { usAmp = (uint16_t)(usAmp + 25); }
      report();
      Serial.println(F("[360] 幅度变了，dps 已按比例近似换算；要精确就重发 k 重标"));
      break;
    case '-':
      if (usAmp >= AMP_MIN + 25) { usAmp = (uint16_t)(usAmp - 25); }
      report();
      Serial.println(F("[360] 幅度变了，dps 已按比例近似换算；要精确就重发 k 重标"));
      break;

    case '?': report(); break;
    case 'H': help(); break;
    default: break;
  }
}

void loop()
{
  while (Serial.available() > 0)
  {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n')
    {
      if (cmdLen > 0) { handleCmd(); }
    }
    else if (cmdLen < sizeof(cmdBuf) - 1)
    {
      cmdBuf[cmdLen++] = c;
    }
  }

  const uint32_t now = millis();
  if (untilL && (int32_t)(now - untilL) >= 0) { stopL(); }
  if (untilR && (int32_t)(now - untilR) >= 0) { stopR(); }
}
