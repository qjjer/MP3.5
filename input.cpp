/*
 * input.cpp - EC11 旋转编码器 + 按键 + 电池电压 的实现
 */
#include "input.h"
#include "config.h"

// ---------------------------------------------------------------------------
// EC11 正交解码
//
// 机械编码器每转过一档会产生多次抖动边沿, 用经典的 4x4 状态表: 只有"合法的一
// 步"(上一状态到当前状态只变化 1 位)才计数, 抖动会被自动吃掉。
// 表索引 = (上一状态 << 2) | 当前状态, 状态 = (A << 1) | B
// ---------------------------------------------------------------------------
static const int8_t ENC_TABLE[16] = {
    0, -1, 1, 0,   // 00 -> 00 01 10 11
    1, 0, 0, -1,   // 01
    -1, 0, 0, 1,   // 10
    0, 1, -1, 0    // 11
};

static uint8_t s_encLast = 0;
static int8_t s_encAcc = 0;
static volatile int32_t s_encDelta = 0;
static uint32_t s_encLastMs = 0;
// 旋钮灵敏度相关(见 config.h 的 ENCODER_STEPS_PER_DETENT / ENCODER_SETTLE_MS)
static uint32_t s_encQuietUntil = 0;   // 稳定窗口截止时刻
#if ENCODER_DEBUG
static uint32_t s_encDbgMs = 0;
static int32_t s_encEvents = 0;
#endif

void inputInit(void) {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  // 开发板自带的 BOOT 键(GPIO0)。不用额外接线就能当第二个按键用,
  // 方便在旋钮没接好/不够用时也能操作菜单。
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  s_encLast = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
}

// BOOT 键(GPIO0)的状态 —— 必须声明在 bootButtonPoll() 之前
static bool s_bootDown = false;
static uint32_t s_bootChangeMs = 0;
static uint32_t s_bootDownMs = 0;
static bool s_bootLongFired = false;

// 轮询 BOOT 键(GPIO0), 按下接地
static BtnEvent bootButtonPoll(void) {
  const uint32_t now = millis();
  const bool raw = (digitalRead(PIN_BTN_BOOT) == LOW);

  if (raw != s_bootDown) {
    if ((now - s_bootChangeMs) < 20) {
      return BTN_NONE;   // BOOT 键抖动比编码器大, 多等一会
    }
    s_bootChangeMs = now;
    s_bootDown = raw;
    if (raw) {
      s_bootDownMs = now;
      s_bootLongFired = false;
    } else if (!s_bootLongFired && (now - s_bootDownMs) > 40) {
      return BTN_BOOT;
    }
    return BTN_NONE;
  }
  if (s_bootDown) {
    if (!s_bootLongFired && (now - s_bootDownMs) >= BTN_LONG_MS) {
      s_bootLongFired = true;
      return BTN_BOOT;
    }
  }
  return BTN_NONE;
}

void encoderPoll(void) {
  const uint32_t now = millis();
  if (now - s_encLastMs < 1) {
    return;  // 最多 1kHz 采样, 足够跟上手速
  }
  s_encLastMs = now;

  const uint8_t cur = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  if (cur == s_encLast) {
    return;
  }
  const uint8_t idx = (uint8_t)((s_encLast << 2) | cur);
  s_encLast = cur;

  const int8_t d = ENC_TABLE[idx & 0x0F];
  if (d == 0) {
    return;
  }

  // ★ 稳定窗口: 刚发过一步就先不吃新跳变 ★
  //   机械编码器在定位点附近触点会抖动, 抖动产生的跳变在状态表里也是"合法的",
  //   于是"转一格"会被算成两格 —— 这是"太灵敏"的第二个来源(第一个是阈值太小)。
  //   发完一步后静默 ENCODER_SETTLE_MS 毫秒, 抖动直接被吃掉。
#if ENCODER_SETTLE_MS > 0
  if (s_encQuietUntil && (int32_t)(now - s_encQuietUntil) < 0) {
    return;
  }
#endif

  // ★ EC11 旋转方向 ★
  // 方向由 A/B 两相的接线顺序决定。实测本机是"反的"(顺时针转, 光标往上走),
  // 所以这里整体取反。如果你重新接线或换了编码器, 只要改这一个符号即可。
  // (界面侧的语义: 正数 = 往下/往后, 负数 = 往上/往前)
  s_encAcc -= d;

  // ★ 累积到 ENCODER_STEPS_PER_DETENT 才算一档 ★
  //   原来是写死的 ±2。对"每档 4 个正交跳变"的 EC11(最常见), 2 意味着
  //   **每档发 2 步** —— 用户实测"太灵敏", 就是这个。见 config.h 的详细说明。
  const int8_t need = ENCODER_STEPS_PER_DETENT;
  if (s_encAcc >= need) {
    s_encAcc = 0;
    s_encDelta += 1;
#if ENCODER_DEBUG
    s_encEvents++;
#endif
#if ENCODER_SETTLE_MS > 0
    s_encQuietUntil = now + ENCODER_SETTLE_MS;
#endif
  } else if (s_encAcc <= -need) {
    s_encAcc = 0;
    s_encDelta -= 1;
#if ENCODER_DEBUG
    s_encEvents++;
#endif
#if ENCODER_SETTLE_MS > 0
    s_encQuietUntil = now + ENCODER_SETTLE_MS;
#endif
  }

#if ENCODER_DEBUG
  // 每秒一行: 用来标定 ENCODER_STEPS_PER_DETENT。
  // 转一大格(手感"咔"一下)看这一行涨了多少 —— 应该正好等于 1。
  // 涨 2 = 阈值太小(把它加倍); 涨 0 = 阈值太大(减半)。
  if (now - s_encDbgMs >= 1000) {
    s_encDbgMs = now;
    if (s_encEvents) {
      Serial.printf("[enc] 旋钮: 事件=%ld (阈值=%d, 稳定窗=%dms)\n", (long)s_encEvents,
                    (int)ENCODER_STEPS_PER_DETENT, (int)ENCODER_SETTLE_MS);
      s_encEvents = 0;
    }
  }
#endif
}

int32_t encoderTakeDelta(void) {
  const int32_t d = s_encDelta;
  s_encDelta = 0;
  return d;
}

// ---------------------------------------------------------------------------
// 按键: 消抖 + 短按/长按/长按连发
//
// 为什么要有独立任务: 主循环会被刷屏、解压 EPUB 等耗时操作阻塞几百毫秒甚至几秒,
// 如果只在主循环里轮询按键, 用户按一下很可能**完全没反应**。这里用 1ms 的任务轮询,
// 事件进队列, 主循环只负责取。
// ---------------------------------------------------------------------------
static volatile BtnEvent s_btnQueue[16];
static volatile uint8_t s_btnHead = 0, s_btnTail = 0;
static portMUX_TYPE s_btnMux = portMUX_INITIALIZER_UNLOCKED;

static void btnPush(BtnEvent e) {
  if (e == BTN_NONE) {
    return;
  }
  portENTER_CRITICAL(&s_btnMux);
  const uint8_t next = (uint8_t)((s_btnTail + 1) % 16);
  if (next != s_btnHead) {
    s_btnQueue[s_btnTail] = e;
    s_btnTail = next;
  }
  portEXIT_CRITICAL(&s_btnMux);
}

BtnEvent buttonTakeEvent(void) {
  BtnEvent e = BTN_NONE;
  portENTER_CRITICAL(&s_btnMux);
  if (s_btnHead != s_btnTail) {
    e = s_btnQueue[s_btnHead];
    s_btnHead = (uint8_t)((s_btnHead + 1) % 16);
  }
  portEXIT_CRITICAL(&s_btnMux);
  return e;
}

// 按键轮询的内部状态
static bool s_btnDown = false;
static uint32_t s_btnChangeMs = 0;
static uint32_t s_btnDownMs = 0;
static bool s_btnLongFired = false;
static uint32_t s_btnLastRepeatMs = 0;

BtnEvent buttonPoll(void) {
  const uint32_t now = millis();
  const bool raw = (digitalRead(PIN_ENC_SW) == LOW);   // 按下 = 接地

  if (raw != s_btnDown) {
    if ((now - s_btnChangeMs) < 8) {
      return BTN_NONE;                 // 抖动, 再等等
    }
    s_btnChangeMs = now;
    s_btnDown = raw;
    if (raw) {
      s_btnDownMs = now;
      s_btnLongFired = false;
      s_btnLastRepeatMs = 0;
    } else {
      // 松开: 如果没触发过长按, 就是短按
      if (!s_btnLongFired && (now - s_btnDownMs) > 30) {
        return BTN_SHORT;
      }
    }
    return BTN_NONE;
  }

  if (s_btnDown) {
    const uint32_t held = now - s_btnDownMs;
    if (!s_btnLongFired && held >= BTN_LONG_MS) {
      s_btnLongFired = true;
      s_btnLastRepeatMs = now;
      return BTN_LONG;
    }
    if (s_btnLongFired && (now - s_btnLastRepeatMs) >= BTN_REPEAT_MS) {
      s_btnLastRepeatMs = now;
      return BTN_REPEAT;
    }
  }
  return BTN_NONE;
}

void inputPrintPinStates(void) {
  Serial.printf("[input] 引脚原始电平: A(GPIO%d)=%d B(GPIO%d)=%d SW(GPIO%d)=%d  (SW 平时应为1, 按下应为0)\n",
                PIN_ENC_A, digitalRead(PIN_ENC_A), PIN_ENC_B, digitalRead(PIN_ENC_B), PIN_ENC_SW,
                digitalRead(PIN_ENC_SW));
}

// ---------------------------------------------------------------------------
// 输入任务: 1ms 轮询编码器与按键
// ---------------------------------------------------------------------------
static void inputTask(void *arg) {
  (void)arg;
  uint32_t lastDiag = 0;
  for (;;) {
    encoderPoll();
    btnPush(buttonPoll());
    btnPush(bootButtonPoll());
    // 每 5 秒打印一次引脚电平, 方便排查接线(按下旋钮时能立刻在串口看到变化)
    const uint32_t now = millis();
    if (now - lastDiag >= 5000) {
      lastDiag = now;
      if (digitalRead(PIN_ENC_SW) == LOW) {
        Serial.println(F("[input] 检测到旋钮被按住"));
      }
    }
    vTaskDelay(1);
  }
}

void inputStartTask(void) {
  // 输入任务放在主循环同一个核(Core 1), 优先级比 loopTask 高一级。
  //   - 按键/旋转永远能及时响应, 不会被刷屏挡住;
  //   - 它每轮只做"读两个 GPIO + 入队", 极轻, 不会拖慢主循环;
  //   - 不去抢 Core 0, 音频解码那边就不受干扰(见 config.h 的分工说明)。
  xTaskCreatePinnedToCore(inputTask, "input", 3072, nullptr, INPUT_TASK_PRIO, nullptr, INPUT_TASK_CORE);
}

// ---------------------------------------------------------------------------
// 电池电压(ADC1_CH5 / GPIO6, 10k:10k 分压)
// ---------------------------------------------------------------------------
void batteryInit(void) {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT_ADC, BAT_ADC_ATTEN);
}

BatteryInfo batteryRead(void) {
  BatteryInfo info;
  const int SAMPLES = 16;
  uint32_t acc = 0;
  for (int i = 0; i < SAMPLES; i++) {
    acc += analogReadMilliVolts(PIN_BAT_ADC);
    delayMicroseconds(200);
  }
  const uint32_t mv = (uint32_t)((acc / SAMPLES) * BAT_DIVIDER);
  info.millivolts = (uint16_t)mv;

  const float v = mv / 1000.0f;
  float pct = (v - BAT_EMPTY) / (BAT_FULL - BAT_EMPTY) * 100.0f;
  if (pct < 0) {
    pct = 0;
  }
  if (pct > 100) {
    pct = 100;
  }
  info.percent = (uint8_t)pct;
  info.valid = (mv > 1500);
  return info;
}
