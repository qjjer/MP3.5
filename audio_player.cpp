/*
 * audio_player.cpp — 音频播放实现 (ESP32-audioI2S 3.4.x)
 */
#include "audio_player.h"
#include "config.h"
#include "display.h"
#include "sd_fastread.h"
#include "sd_mutex.h"

// 关键诊断直接走 Serial(log_i 受 Core Debug Level 控制, 默认不打印)
#ifndef BOOTLOG
#define BOOTLOG(fmt, ...)                    \
  do {                                       \
    Serial.printf(fmt "\n", ##__VA_ARGS__);    \
    Serial.flush();                          \
  } while (0)
#endif

#include <SD.h>
#include <string.h>

#include "Audio.h"

// ---------------------------------------------------------------------------
// 关于"音频读卡要不要拿 sd_mutex" —— 结论与边界(重要)
//
// 本库(3.4.x)通过 m_audiofile.read(buff, len) 一次性读一大块, 而 SD 库把整个块读
// 包在一个 beginTransaction()/endTransaction() 里, 核心 HAL 的 SPI_MUTEX_LOCK
// 保证**这一个块读不会和别的 SPI 事务交错** —— 也就是说音频读卡在事务层面是原子的。
//
// 但 TFT_eSPI 在 ESP32-S3 上是直接写 SPI 寄存器(SPI_CMD_REG(2)), 绕过了 SPIClass,
// 所以 HAL 锁保护不到"整帧刷屏"。因此两者仍可能在毫秒级上互相插入。
//
// 我们试过给库加"块读前后取锁"的钩子, 但该库把读文件写成 Audio 的私有成员并在
// 头文件里可见, 加弱符号钩子会被编译器直接内联成空操作(实测 ELF 里根本不产生调用,
// 详见 README 的"已知边界")。所以这里不做假装能锁的事, 而是用两条实际措施把影响
// 压到最小:
//   1) 音频任务优先级(2) 高于 UI 任务(1), 且都在 Core 1 —— 音频要读卡时直接抢占 UI;
//   2) 整帧推送持 sd_mutex, 一帧只占一次总线, 时长可预测(40MHz 下约 24ms);
//   3) 库自带约 640KB 缓冲(PSRAM), 足以吸收偶尔的等待。
// 我们的**自有**代码(SD 打开/读文件/字库)全部走 sd_mutex, 这部分是严格互斥的。
// ---------------------------------------------------------------------------

static Audio *s_audio = nullptr;
static Track *s_tracks = nullptr;
static int s_trackCount = 0;
static int s_trackIndex = -1;
static uint8_t s_volume = AUDIO_VOLUME_INIT;
static volatile bool s_eofFlag = false;
static bool s_autoNext = true;
static volatile bool s_paused = false;
static uint32_t s_lastElapsed = 0;   // 最近一次查到的播放位置, 用来判断"是否真的播过"

// 【静音底噪-状态】空闲时主动停掉 I2S 时钟(见 audioIdleMuteTick 的说明)
static bool     s_i2sIdleStopped = false;   // true = 当前 I2S 时钟是我们关掉的
static uint32_t s_idleSinceMs = 0;          // 连续空闲的起点(millis; 0 = 不空闲)
#define AUDIO_I2S_IDLE_STOP_MS 3000         // 连续空闲多久才停时钟(避开切歌/暂停的间隙)

// 【修复-1 状态】最近一次"选中却没能播"的原因, 给播放界面显示(见 audio_player.h)
static char s_notice[48] = {0};
static bool s_lastAttemptFailed = false;
// 【修复-2 状态】最近一首"确实放出过声"的曲目。新选的这首秒失败(evt_eof 且没播过)时
//   用它把上一首接回来 —— 因为 connecttoFS() 已经把上一首 stop 掉了, 不接回来就是静音。
static int s_lastGoodIndex = -1;
// 【诊断】每首只打印一次"真实解码参数"(下面对比 audioPlayIndex 里那次必然读到默认值的打印)
static bool s_paramsLogged = false;
// 喂数据任务是否已启动(只能启动一次)
static bool s_audioTaskStarted = false;

static void *allocPreferPsram(size_t n) {
#if defined(ARDUINO_ARCH_ESP32)
  void *p = ps_malloc(n);
  if (p) {
    return p;
  }
#endif
  return malloc(n);
}

// 曲目扫描诊断的计数器(每次扫描重置, 最多打 40 行避免刷屏)
static int s_scanDbg = 0;

static bool hasAudioExt(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot) {
    return false;
  }
  return (strcasecmp(dot, ".mp3") == 0) || (strcasecmp(dot, ".flac") == 0) || (strcasecmp(dot, ".wav") == 0) ||
         (strcasecmp(dot, ".m4a") == 0) || (strcasecmp(dot, ".aac") == 0) || (strcasecmp(dot, ".ogg") == 0);
}

static void titleFromPath(const char *path, char *out, size_t outLen) {
  const char *slash = strrchr(path, '/');
  const char *base = slash ? (slash + 1) : path;
  snprintf(out, outLen, "%s", base);   // snprintf 明确截断, 不会漏 '\0'
  char *dot = strrchr(out, '.');
  if (dot) {
    *dot = 0;
  }
}

// ---------------------------------------------------------------------------
// 【修复-1】播放前的"这首到底能不能播"最小预检
//
// 为什么必需 —— 这是"一进播放界面就没声"的根治点(读库源码得到):
//   Audio::connecttoFS() 第一步就是 setDefaults() -> stopSong():
//     Audio.cpp:1062  setDefaults()
//     Audio.cpp:439   stopSong()                    <- 正在播的那首在这里被停
//     Audio.cpp:3254-3266  m_f_running=false / m_audiofile.close() / destroy_decoder()
//     Audio.cpp:442/448    InBuff.reset() / m_outBuff.clear()  <- 缓冲里的音频也全丢
//   它的返回值只表示"文件能打开"(Audio.cpp:1087-1091); 解码器和文件头要等之后
//   主循环里的 Audio::loop() -> processLocalFile() 才解析。解析失败时库只把
//   m_f_running 置 false 并打一行 "Processing stopped due to invalid audio header"
//   (Audio.cpp:1520-1522), 然后 Audio::loop() 从下次起直接在 Audio.cpp:3485 返回
//   —— **既不会把上一首还回来, 也不会发 evt_eof**(evt_eof 只在"数据读完"的
//   m_f_eof 分支里发: Audio.cpp:4285-4287)。app 侧因此连"自动切下一首"都不会触发,
//   就是彻底静音。
//   => "选中一个库不支持的容器" == "把正在放的那首杀掉"。和界面切换无关。
//
// 判据全部复刻库自己的检查, 只拦"库明确会拒绝"的情况:
//   - MP4/M4A: 首个原子必须是 ftyp, 且品牌是 'M4A '/'isom'/'mp42'
//              (Audio.cpp:2477 / 2492-2500), 且扩展名必须是 .m4a
//              (connecttoFS 只按扩展名选解码器: Audio.cpp:1064-1078);
//   - FLAC/Ogg: "fLaC"/"OggS"; ID3v2 开头(mp3 或 ID3+fLaC 的 flac)也放行;
//   - WAV: "RIFF" + "WAVE";
//   - MP3/AAC(ADTS): 头 32 字节里能找到帧同步 0xFFEx;
//   - 打不开 / 空文件 / 上面全不匹配 -> 同样判"库播不了", 一律不调 connecttoFS。
// 返回 false 时 why 里是给界面显示的一句话(必须短: 半宽字库, 240px 屏最多约 19 个汉字)。
// ---------------------------------------------------------------------------
static bool nameEndsWith(const char *path, const char *e1, const char *e2 = nullptr, const char *e3 = nullptr,
                         const char *e4 = nullptr) {
  const char *dot = strrchr(path, '.');
  if (!dot) {
    return false;
  }
  const char *e[4] = {e1, e2, e3, e4};
  for (int i = 0; i < 4; i++) {
    if (e[i] && strcasecmp(dot, e[i]) == 0) {
      return true;
    }
  }
  return false;
}

static bool magicAt(const uint8_t *h, size_t n, size_t off, const char *m) {
  const size_t l = strlen(m);
  return (off + l <= n) && memcmp(h + off, m, l) == 0;
}

// 4 字节品牌 -> 可打印 ASCII(必须干净: 这串会画到屏上, 也可能是任意二进制)
static void brandToText(const uint8_t *b, size_t n, char *out, size_t outLen) {
  size_t k = 0;
  for (size_t i = 0; i < 4 && i < n && k + 1 < outLen; i++) {
    out[k++] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
  }
  while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '.')) {
    k--;   // 去掉尾部空格/占位点
  }
  out[k] = 0;
  if (k == 0) {
    snprintf(out, outLen, "????");
  }
}

static bool headerLooksPlayable(const char *path, const uint8_t *h, size_t n, char *why, size_t whyLen) {
  if (n < 12) {
    snprintf(why, whyLen, "文件为空或读不到头");
    return false;
  }

  // ---- MP4 / M4A(iso5 之类的"库不支持"就是在这里被拦下的)----
  if (magicAt(h, n, 4, "ftyp")) {
    char brand[8];
    brandToText(h + 8, n - 8, brand, sizeof(brand));
    const bool brandOk = magicAt(h, n, 8, "M4A ") || magicAt(h, n, 8, "isom") || magicAt(h, n, 8, "mp42");
    if (!nameEndsWith(path, ".m4a")) {
      // 库只按扩展名选解码器: 一个 mp4 容器挂 .mp3 名字, 会被当 MP3 解码 -> 必然失败
      snprintf(why, whyLen, "mp4容器,扩展名不对");
      return false;
    }
    if (!brandOk) {
      // 与库 Audio.cpp:2496-2500 的判据完全一致
      snprintf(why, whyLen, "m4a不支持(%s)", brand);
      return false;
    }
    return true;
  }

  // ---- FLAC / Ogg ----
  if (magicAt(h, n, 0, "fLaC") || magicAt(h, n, 0, "OggS")) {
    if (nameEndsWith(path, ".flac", ".ogg", ".oga", ".opus")) {
      return true;
    }
    snprintf(why, whyLen, "扩展名与文件头不符");
    return false;
  }

  // ---- WAV ----
  if (magicAt(h, n, 0, "RIFF") && magicAt(h, n, 8, "WAVE")) {
    if (nameEndsWith(path, ".wav")) {
      return true;
    }
    snprintf(why, whyLen, "扩展名与文件头不符");
    return false;
  }

  // ---- ID3v2 开头: mp3, 或"ID3 在 fLaC 前面"的 flac(库 Audio.cpp:1686 支持)----
  if (magicAt(h, n, 0, "ID3")) {
    if (nameEndsWith(path, ".mp3", ".flac")) {
      return true;
    }
    snprintf(why, whyLen, "扩展名与文件头不符");
    return false;
  }

  // ---- MP3 / AAC-ADTS: 头 32 字节里找帧同步 0xFFEx(允许前面有零星垃圾)----
  for (size_t i = 0; i + 1 < n; i++) {
    if (h[i] == 0xFF && (h[i + 1] & 0xE0) == 0xE0) {
      if (nameEndsWith(path, ".mp3", ".aac")) {
        return true;
      }
      snprintf(why, whyLen, "扩展名与文件头不符");
      return false;
    }
  }

  snprintf(why, whyLen, "认不出音频格式");
  return false;
}

int audioScanTracks(void) {
  static const char *dirs[] = {"/mp3", "/music", "/audio", "/"};
  if (!s_tracks) {
    s_tracks = (Track *)allocPreferPsram(sizeof(Track) * AUDIO_MAX_TRACKS);
    if (!s_tracks) {
      log_e("[audio] 播放列表内存不足");
      return 0;
    }
  }
  s_trackCount = 0;
  s_scanDbg = 0;

  SdLockGuard g(SD_LOCK_TIMEOUT_MS);
  if (!g.ok()) {
    log_e("[audio] 扫描音乐时等 SD 锁超时");
    return 0;
  }

  for (int d = 0; d < 4 && s_trackCount == 0; d++) {
    File dir = SD.open(dirs[d]);
    if (!dir || !dir.isDirectory()) {
      if (dir) {
        dir.close();
      }
      continue;
    }
    File f;
    while ((f = dir.openNextFile()) && s_trackCount < AUDIO_MAX_TRACKS) {
      const bool isDir = f.isDirectory();
      // ★ 名字缓冲必须够大 ★
      //   实测(2026-10-03): 用户卡上的文件名长这样 ——
      //     【5小时不重复纯享版】_想念变成空气在叹息，… - 1.【高音质】….mp3
      //   原来 nm[64] 直接把它截断, **.mp3 后缀被切掉**, 于是 hasAudioExt() 判定
      //   "不是音频文件" -> 整个曲目列表是空的 -> 界面报"未找到音频文件"。
      //   现在给 256, 和 Track::path 一致, 保证后缀一定还在。
      char nm[256];
      const char *rawName = f.name();
      strncpy(nm, rawName ? rawName : "", sizeof(nm) - 1);
      nm[sizeof(nm) - 1] = 0;
      f.close();
      if (isDir) {
        continue;
      }
      const bool accepted = hasAudioExt(nm);
      // ★ 诊断: 逐文件打一行(最多 40 行) ★
      //   这一行就是定位"列表空/播不出"的关键证据: 它同时给出**截断后的完整名字**
      //   和"收/跳过"。之前 nm[64] 的 bug 就是靠它一眼看出来的 ——
      //   日志里所有名字都恰好断在 63 个字符、而且都没有 .mp3。
      if (s_scanDbg < 40) {
        s_scanDbg++;
        Serial.printf("[music] %s/%s -> %s%s\n", dirs[d], nm, accepted ? "收" : "跳过",
                      accepted ? "" : "(扩展名不是 mp3/flac/wav/m4a/aac/ogg)");
        Serial.flush();
      }
      if (!accepted) {
        continue;
      }
      Track &t = s_tracks[s_trackCount];
      snprintf(t.path, sizeof(t.path), "%s/%s", strcmp(dirs[d], "/") == 0 ? "" : dirs[d], nm);
      titleFromPath(t.path, t.title, sizeof(t.title));
      s_trackCount++;
    }
    dir.close();
  }

  log_i("[audio] 找到 %d 首曲目", s_trackCount);
  BOOTLOG("[music] 曲目扫描结束: 共 %d 首 (扫描目录 /mp3 /music /audio /)", s_trackCount);
  return s_trackCount;
}

int audioTrackCount(void) {
  return s_trackCount;
}

const char *audioTrackTitle(int index) {
  if (index < 0 || index >= s_trackCount) {
    return "";
  }
  return s_tracks[index].title;
}

// 回调运行在音频任务(Core 1)里, 只做最轻量的动作
//
// 关于日志: 音频库把诊断分成两类
//   - info(*this, evt_info, ...)  不受编译期宏控制, 会走这个回调
//   - AUDIO_LOG_*                 受 CORE_DEBUG_LEVEL 控制, 默认 0 时在库内部
//                                 就被 return 掉了(见 Audio.h 的 AUDIO_LOG_IMPL),
//                                 我们无法从外部打开, 所以不要依赖它。
static void audioInfoCallback(Audio::msg_t m) {
  if (m.e == Audio::evt_eof) {
    s_eofFlag = true;
    return;
  }
  // 限流: 音频任务里直接 printf 会拖慢解码, 也可能淹没串口
  static uint32_t logged = 0;
  if (logged >= 250) {
    return;
  }
  logged++;
  const char *tag = "?";
  switch (m.e) {
    case Audio::evt_info:
      tag = "info";
      break;
    case Audio::evt_log:
      tag = "log";
      break;
    case Audio::evt_bitrate:
      tag = "bitrate";
      break;
    case Audio::evt_streamtitle:
      tag = "title";
      break;
    default:
      break;
  }
  Serial.printf("[audiolib/%s] %s\n", tag, m.msg ? m.msg : "(no msg)");
  Serial.flush();
}

bool audioInit(void) {
  if (s_audio) {
    return true;
  }

  Audio::audio_info_callback = audioInfoCallback;

  // ★ PCM5102A 的 XSMT(软静音)必须先拉高, 否则芯片一直静音 ★
  //   现象: 解码正常、I2S 数据在发、串口说"开始播放", 但一点声音都没有。
  //   模块上 XSMT 常悬空 -> 默认静音。这里用一个 GPIO 主动拉高。
  //   config.h 里 PIN_I2S_XSMT = -1 表示不改, 那就需要手动把 XSMT 接到 3.3V。
#if PIN_I2S_XSMT >= 0
  pinMode(PIN_I2S_XSMT, OUTPUT);
  digitalWrite(PIN_I2S_XSMT, HIGH);
  BOOTLOG("[audio] 已把 XSMT(GPIO%d)拉高, 解除 PCM5102A 静音", PIN_I2S_XSMT);
#else
  BOOTLOG("[audio] 提示: 若完全没有声音, 检查 PCM5102A 的 XSMT 脚是否接到了 3.3V");
  BOOTLOG("[audio]       (XSMT 悬空时芯片一直静音, 但串口会显示播放正常)");
  BOOTLOG("[audio]       模块的 SCK 脚也必须接 GND");
#endif

  s_audio = new Audio();
  if (!s_audio) {
    log_e("[audio] 无法创建 Audio 对象(内存不足)");
    return false;
  }

  // ★【FASTREAD】把共享的 SPI 实例注入给"原始多扇区读"模块 ★
  //   必须和 SD 库 / TFT 用同一个 SPIClass —— 我们的 SD 挂在 TFT_eSPI 的私有
  //   HSPI(SPI3) 实例上(见 display.cpp 开头那段说明), 用别的话 SPI 外设就错了,
  //   轻则读不到数据, 重则和刷屏抢总线。
  //   注入失败也没关系: sd_fastread 会拒绝打开, 音频自动回落库路径。
  // ★ FASTREAD 总开关在 sd_fastread.h 里的 SD_FASTREAD_ENABLE, **默认 0 = 关闭** ★
  //   关闭时不引用 sd_fastread 的任何符号 -> 整个模块被 --gc-sections 丢掉, 回收 Flash;
  //   音频走库路径 —— 库路径已被 sd_diskio.cpp 的 delay 补丁修好(实测 >1000 KB/s,
  //   而 FLAC 只需要 205 KB/s)。要恢复快路径: 把那个宏改成 1 重编。
#if SD_FASTREAD_ENABLE
  {
    SPIClass *bus = displaySpiInstance();
    if (bus) {
      sdFastSetSpi(bus);
      BOOTLOG("[audio] FASTREAD: 已注入共享 SPI 实例, 本地文件将走原始多扇区读");
    } else {
      BOOTLOG("[audio] FASTREAD: 拿不到共享 SPI 实例 -> 保持库路径(会慢)");
    }
  }
#else
  BOOTLOG("[audio] FASTREAD: 已关闭(默认) -> 本地文件走库路径(SD 驱动已修, 够快)");
#endif

  // ★【v10】关掉库里两个"每个输出帧都算一次、而我们完全不需要"的后处理 ★
  //   库的 playChunk() 对**每一个输出帧**都跑(Audio.cpp:3421-3425):
  //     if (settings.VU_LEVEL)     calculateVUlevel(&m_outBuff[i*2]);   // 每帧: 浮点除法/比较/写延迟线
  //     if (settings.IIR_FILTER)   IIR_filter(&m_outBuff[i*2]);         // 每帧: 2 通道 × 3 个 float biquad
  //   而这两个开关的默认值都是 **true**(Audio.h:367-368) —— 也就是说, 即使我们既没有
  //   VU 表也没有均衡器界面, 库仍然按 44100 帧/秒做这些浮点运算(S3 上浮点是软件模拟,
  //   很贵)。更要命的是三档 EQ 的增益默认全是 0dB(audiolib_structs.hpp:345-347),
  //   所以那个 IIR 在数学上就是**直通**, 纯属白烧 CPU。
  //   关掉它对音质没有任何影响(直通 × 去掉 = 去掉直通), 却把每秒 44100 帧的浮点开销
  //   从库解码任务(Core 0, 优先级 2)身上拿掉 —— 这些 CPU 正是"喂饱 I2S"所需的。
  //   VOLUME_CONTROL 保持打开: 音量是 user 要用的, 只是一个 int×float 乘法。
  //   SPECTRUM 本来就是 false, 这里显式写出来, 防止以后库改默认值。
  //   还原方法: 把下面三行删掉(或改成 true), 即回到库的默认行为。
  s_audio->settings.VU_LEVEL = false;
  s_audio->settings.IIR_FILTER = false;
  s_audio->settings.SPECTRUM = false;

  // ★ setPinout 的返回值必须检查 ★
  //   它不只设引脚, 还负责分配输出缓冲 + i2s_channel_init + reconfig_gpio,
  //   并把结果存进 m_f_I2S_init。而音频任务是这样的:
  //       while (running) { if (m_f_I2S_init) performAudioTask(); }
  //   —— m_f_I2S_init 为 false 时, 任务在跑但**一个采样都不解码**:
  //   现象正是"串口显示开始播放、running=1, 但 elapsed=0/total=0、没有声音、
  //   库的日志一条都不出现"。所以这里失败必须立刻报出来。
  const bool pinOk = s_audio->setPinout(PIN_I2S_BCK, PIN_I2S_LCK, PIN_I2S_DIN);
  BOOTLOG("[audio] setPinout(BCK=%d LRCK=%d DIN=%d) 返回 %s", PIN_I2S_BCK, PIN_I2S_LCK, PIN_I2S_DIN,
          pinOk ? "true(成功)" : "false(失败!)");
  if (!pinOk) {
    BOOTLOG("[audio] !! I2S 初始化失败 -> m_f_I2S_init=false -> 解码任务不会工作");
    BOOTLOG("[audio] !! 常见原因: PSRAM 未启用 / 缓冲分配失败 / I2S 引脚被占用");
    BOOTLOG("[audio] !! 可用堆: %u 字节, PSRAM: %u 字节", (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getFreePsram());
    return false;
  }

  // 库内解码任务钉在 AUDIO_TASK_CORE(config.h = 0)。喂数据任务必须去**另一个**核,
  // 否则它那几十毫秒的阻塞读会把解码任务按在核外 -> I2S 欠载 -> "一顿一顿"(见 v10 说明)。
  s_audio->setAudioTaskCore(AUDIO_TASK_CORE);
  s_audio->setVolume(s_volume, 0);

  // ★ 固定输出 44.1kHz (用户要求"把输出固定") ★
  //
  // 原来写的是 SR_ORIGIN(跟随源): 源是多少就出多少, 不重采样。听起来最"位完美",
  // 但实测有问题: 换一首 48k/96k 的音源就会**静默走进重采样器**, 而重采样是纯
  // 额外 CPU 开销 —— 在本机 CPU 已经很紧的情况下(实测解码只有实时的 13.5%),
  // 这个开销是不确定的。
  //
  // 固定成 SR_44100 之后行为完全确定:
  //   44.1k 源 -> 不重采样, 直通(和原来一样)
  //   48k/96k 源 -> 统一重采样到 44.1k
  // 代价: 96k 音源会降到 44.1k(听感上基本无差); 收益: 行为可预测, 不再有"某首歌
  // 特别卡"的随机性。PCM5102A 只接 BCK/LRCK/DIN、SCK 接 GND, 完全跟得上 44.1k。
  s_audio->setOutputSampleRate(Audio::SR_44100);

  BOOTLOG("[audio] 就绪: BCK=%d LRCK=%d DIN=%d, 任务 Core %d, 缓冲 %u 字节", PIN_I2S_BCK, PIN_I2S_LCK,
          PIN_I2S_DIN, AUDIO_TASK_CORE, (unsigned)s_audio->getInBufferSize());
  BOOTLOG("[audio] 输出采样率: 固定 44.1kHz (SR_44100), 重采样策略确定");
  return true;
}

bool audioPlayIndex(int index) {
  if (!s_audio || index < 0 || index >= s_trackCount) {
    return false;
  }
  s_lastAttemptFailed = false;
  s_notice[0] = 0;

  // 播放前: 打印文件在 SD 上的实际大小 + 看一眼文件头能不能播。
  // connecttoFS 的返回值**只表示"文件能打开"**, 不表示解码成功(见库源码:
  // 它打开文件后就 res=true, 之后解析文件头失败时只会把 m_f_running 置 false,
  // 返回值仍然是 true)。所以要分开看这两件事。
  //
  // ★ 这个预检还负责"别把正在放的那首害死": connecttoFS() 会先 stopSong()
  //   把当前曲目停掉/关文件/销毁解码器, 之后才发现新文件解不了 —— 那时已经晚了。
  //   所以"库明确不支持"的文件在这里就拦下, 一个字节都不碰当前播放。
  char why[40] = {0};
  {
    uint32_t fsz = 0;
    uint8_t hdr[32] = {0};
    int hdrLen = -1;   // -1 = 没读到(打不开/锁超时)
    bool lockOk = false;
    {
      SdLockGuard g(SD_LOCK_TIMEOUT_MS);
      if (g.ok()) {
        lockOk = true;
        File f = SD.open(s_tracks[index].path, FILE_READ);
        if (f) {
          fsz = f.size();
          hdrLen = f.read(hdr, sizeof(hdr));
          if (hdrLen < 0) {
            hdrLen = 0;
          }
          f.close();
        }
      }
    }
    BOOTLOG("[audio] 准备播放: %s (SD 上 %lu 字节)", s_tracks[index].path, (unsigned long)fsz);

    bool usable = false;
    if (!lockOk) {
      snprintf(why, sizeof(why), "SD忙,请重试");
    } else if (hdrLen <= 0) {
      snprintf(why, sizeof(why), "打不开或空文件");
    } else {
      usable = headerLooksPlayable(s_tracks[index].path, hdr, (size_t)hdrLen, why, sizeof(why));
    }

    if (!usable) {
      // ★ 不调用 connecttoFS: 保住正在播的那首 ★
      BOOTLOG("[audio] !! 不改当前播放: [%d/%d] %s 不能播 (%s)", index + 1, s_trackCount,
              s_tracks[index].title, why[0] ? why : "未知原因");
      snprintf(s_notice, sizeof(s_notice), "%s", why[0] ? why : "格式不支持");
      s_lastAttemptFailed = true;
      return false;
    }
  }

  // 到这里才真正要换歌: 记录"当前这首是否确实在放"(供秒失败时接回), 再重置状态
  if (s_trackIndex >= 0 && s_trackIndex != index) {
    const bool wasAudible = (s_lastElapsed > 0) || (s_audio->getAudioCurrentTime() > 0);
    s_lastGoodIndex = wasAudible ? s_trackIndex : -1;
    if (wasAudible) {
      BOOTLOG("[audio] 换曲前记录可回退曲目: [%d/%d] %s (播到 %lu 秒)", s_trackIndex + 1, s_trackCount,
              s_tracks[s_trackIndex].title, (unsigned long)s_lastElapsed);
    }
  }
  s_eofFlag = false;
  s_paused = false;
  s_trackIndex = index;
  s_lastElapsed = 0;   // 换曲后重新判断"这一首有没有真的播过"
  s_paramsLogged = false;

  // 【静音底噪】要出声了 -> 先解除静音(见 config.h 的 AUDIO_I2S_IDLE_MUTE)。
  //   ★ 走"停 I2S 时钟"那条路时这里必须显式恢复 ★ 库只在 I2S_init() 里开过一次通道
  //   (:6089), connecttoFS/playChunk 都不会重新打开; 通道停着就一个字都不会响。
  if (s_i2sIdleStopped) {
#if AUDIO_I2S_IDLE_MUTE
    s_audio->I2Sstart();
    BOOTLOG("[audio] 开始播放: 已重新打开 I2S 时钟");
#elif PIN_I2S_XSMT >= 0
    digitalWrite(PIN_I2S_XSMT, HIGH);
    BOOTLOG("[audio] 开始播放: 已解除 XSMT 软静音");
#endif
    s_i2sIdleStopped = false;
    s_idleSinceMs = 0;
  }

  // ★【FASTREAD】换曲必须持 sd_mutex ★
  //   connecttoFS() 里会调 m_fastfile.open() —— 那是**直接操作 SPI 的原始读**
  //   (读 MBR/BPB/FAT/目录项)。如果不持锁, 它会和刷屏的整帧推送在同一根总线上
  //   交错, 两边数据都会坏(而且坏得很隐蔽)。原来这里没有锁, 是个定时炸弹。
  //   锁是递归的, 且库内 stopSong() 等的是"解码任务"的锁(不是我们的 sd_mutex),
  //   所以不会死锁。
  bool ok = false;
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    if (!g.ok()) {
      BOOTLOG("[audio] !! 换曲时等 sd_mutex 超时, 本次跳过(保持现状)");
      return false;
    }
    ok = s_audio->connecttoFS(SD, s_tracks[index].path);
  }
  if (ok) {
    // 等一下再读状态: 解码器开始工作后才会有采样率/时长
    // 注意: 库的 loop() 还没跑过, 这里读到的**通常还是默认值**(48000/16/2、total=0),
    //       真正解析出来的参数在 audioLoop() 里打印(见那里的"实际解码参数")。
    delay(120);
    BOOTLOG("[audio] connecttoFS 返回 true; 解码状态: running=%d rate=%lu bits=%u ch=%u total=%lu",
            s_audio->isRunning() ? 1 : 0, (unsigned long)s_audio->getSampleRate(),
            (unsigned)s_audio->getBitsPerSample(), (unsigned)s_audio->getChannels(),
            (unsigned long)s_audio->getAudioFileDuration());
    s_audio->setVolume(s_volume, 0);
    // 打印实际音频参数: 采样率/位深/声道。96kHz 的 FLAC 应显示 96000Hz/24bit
    BOOTLOG("[audio] 播放 [%d/%d] %s", index + 1, s_trackCount, s_tracks[index].title);
    BOOTLOG("[audio] 源参数: %lu Hz / %u bit / %u ch", (unsigned long)s_audio->getSampleRate(),
            (unsigned)s_audio->getBitsPerSample(), (unsigned)s_audio->getChannels());
  } else {
    // connecttoFS 返回 false 时库里已经 stopSong() 过了, 这里只报告
    log_e("[audio] 打不开 %s", s_tracks[index].path);
    snprintf(s_notice, sizeof(s_notice), "文件打不开");
    s_lastAttemptFailed = true;
  }
  return ok;
}

// 从 fromIdx 出发按 step 方向找第一首"预检能过"的曲目。
// 用途: 自动切歌/上一首下一首时跳过已知不支持的格式(例如 QQ音乐那种 iso5 的 m4a),
//       否则每次都会撞上同一堵墙 —— 先静音, 再被 evt_eof 保护挡住不切歌。
// 最多扫满一圈, 扫不到就返回 false(调用者保持现状, 不会来回乱跳)。
static bool playScanning(int fromIdx, int step) {
  if (s_trackCount <= 0) {
    return false;
  }
  for (int i = 1; i <= s_trackCount; i++) {
    int idx = (fromIdx + step * i) % s_trackCount;
    if (idx < 0) {
      idx += s_trackCount;
    }
    if (audioPlayIndex(idx)) {
      return true;
    }
  }
  return false;
}

bool audioPlayNext(void) {
  if (s_trackCount == 0) {
    return false;
  }
  if (s_trackCount == 1) {
    return audioPlayIndex(s_trackIndex < 0 ? 0 : s_trackIndex);   // 只有一首: 重新从头放
  }
  return playScanning(s_trackIndex, 1);
}

bool audioPlayPrev(void) {
  if (s_trackCount == 0) {
    return false;
  }
  if (s_trackCount == 1) {
    return audioPlayIndex(s_trackIndex < 0 ? 0 : s_trackIndex);
  }
  return playScanning(s_trackIndex, -1);
}

void audioTogglePause(void) {
  if (!s_audio || s_trackIndex < 0) {
    return;
  }
  if (s_audio->pauseResume()) {
    s_paused = !s_paused;
    // 【静音底噪】从暂停恢复 -> 解除静音(暂停也算空闲, 会被静音掉)
    if (!s_paused && s_i2sIdleStopped) {
#if AUDIO_I2S_IDLE_MUTE
      s_audio->I2Sstart();
      BOOTLOG("[audio] 恢复播放: 已重新打开 I2S 时钟");
#elif PIN_I2S_XSMT >= 0
      digitalWrite(PIN_I2S_XSMT, HIGH);
      BOOTLOG("[audio] 恢复播放: 已解除 XSMT 软静音");
#endif
      s_i2sIdleStopped = false;
      s_idleSinceMs = 0;
    }
  }
}

void audioStop(void) {
  if (s_audio) {
    s_audio->stopSong();
  }
  s_paused = false;
  s_eofFlag = false;
}

uint8_t audioSetVolume(uint8_t vol) {
  if (vol > AUDIO_VOLUME_MAX) {
    vol = AUDIO_VOLUME_MAX;
  }
  s_volume = vol;
  if (s_audio) {
    s_audio->setVolume(s_volume, 0);
  }
  return s_volume;
}

uint8_t audioGetVolume(void) {
  return s_volume;
}

uint8_t audioVolumeUp(int8_t delta) {
  int32_t v = (int32_t)s_volume + delta;
  if (v < 0) {
    v = 0;
  }
  if (v > AUDIO_VOLUME_MAX) {
    v = AUDIO_VOLUME_MAX;
  }
  return audioSetVolume((uint8_t)v);
}

void audioSetAutoNext(bool en) {
  s_autoNext = en;
}

AudioStatus audioGetStatus(void) {
  AudioStatus st;
  st.volume = s_volume;
  st.trackIndex = s_trackIndex;
  st.trackCount = s_trackCount;
  st.paused = s_paused;
  // 上一次"选中却没播起来"的原因(界面要显示; 见 ui.cpp 的 drawPlayer)
  snprintf(st.notice, sizeof(st.notice), "%s", s_notice);
  st.lastAttemptFailed = s_lastAttemptFailed;

  if (s_trackIndex >= 0 && s_trackIndex < s_trackCount) {
    snprintf(st.title, sizeof(st.title), "%.*s", (int)(sizeof(st.title) - 1), s_tracks[s_trackIndex].title);
    st.loaded = true;
  }

  if (s_audio) {
    st.playing = s_audio->isRunning() && !s_paused;
    st.elapsedSec = s_audio->getAudioCurrentTime();
    st.totalSec = s_audio->getAudioFileDuration();
    if (st.elapsedSec > s_lastElapsed) {
      s_lastElapsed = st.elapsedSec;   // 只增不减: 记录"确实播到过的位置"
    }
  }

  // ★ 每秒一次的音频自检 ★
  //   用来回答"数据到底有没有送出去": 如果 elapsedSec 一直是 0, 说明解码器
  //   根本没产出采样 —— 那不是功放的问题, 是解码/I2S 没跑起来。
  static uint32_t lastDbg = 0;
  const uint32_t now = millis();
  if (s_audio && (now - lastDbg) >= 1000) {
    lastDbg = now;
    BOOTLOG("[adiag] running=%d pause=%d vol=%u(库内=%u) elapsed=%lu total=%lu rate=%lu bits=%u ch=%u",
            s_audio->isRunning() ? 1 : 0, s_paused ? 1 : 0, (unsigned)s_volume, (unsigned)s_audio->getVolume(),
            (unsigned long)st.elapsedSec, (unsigned long)st.totalSec, (unsigned long)s_audio->getSampleRate(),
            (unsigned)s_audio->getBitsPerSample(), (unsigned)s_audio->getChannels());
  }

  if (s_eofFlag && s_autoNext) {
    st.playing = false;
  }
  return st;
}

// ---------------------------------------------------------------------------
// 【v10】"音频一顿一顿"的机理(读库源码得到, 行号 = D:\...\libraries\ESP32-audioI2S\src\Audio.cpp)
//
// 声音**只由库自己的解码任务**喂给 I2S, 没有第二条路:
//   Audio.cpp:7927  Audio::audioTask()   while(运行中) { vTaskDelay(1ms); performAudioTask(); }
//   Audio.cpp:7935  performAudioTask()   -> playAudioData()      (优先级 2, 钉在 AUDIO_TASK_CORE)
//   Audio.cpp:4770  playAudioData()        从 InBuff 取数据 -> sendBytes() -> m_validSamples
//   Audio.cpp:3406  playChunk()            i2s_channel_write(..., 超时 5 ticks)
// 也就是说: **这个优先级 2 的任务一旦拿不到 CPU, I2S 就没有新数据**。而通道是
// auto_clear=true(Audio.cpp:5946) —— 没数据时硬件自动补 0, 听感就是"断一下"。
// DMA 深度 = 32 段 × 256 帧(Audio.h:359-360) = 8192 帧 ≈ 44.1kHz 下 186ms,
// 所以凡能让解码任务连续上百毫秒拿不到 CPU 的事, 都会变成可听见的空洞。
//
// v9 恰好就做了这样一件事: 喂数据任务和库的解码任务**用的是同一个核**
// (都取 AUDIO_TASK_CORE), 而喂数据任务优先级更高(3 > 2)。它每次调 Audio::loop()
// 都可能在核上连续跑很久, 因为里面是一次**阻塞式 SD 读**:
//   Audio.cpp:4137  audioFileRead(InBuff.getWritePtr(), min(availableBytes, UINT16_MAX))
//   单轮最多 65535 字节(m_maxRet = UINT16_MAX, Audio.cpp:72)
// 25MHz SPI 下这一读就是几十毫秒(实测值看 [afeed] 的 busy=最长耗时)。这几十毫秒里
// 解码任务**一个采样都产不出来**。百分比看着不大, 但 FLAC 24bit/44.1kHz 的解码余量
// 本来就薄, 被啃掉几个百分点就足以把它压到实时速率以下 -> 周期性欠载 -> "一顿一顿"。
//
// 这也解释了 Lead 已经试过的两件事为什么"无效":
//   - 喂数据间隔 50ms->20ms->2ms 无效: 瓶颈不是"喂得多勤", 而是"每次喂的时候核被占多久";
//   - 音频任务 Core1 -> Core0 无效: 这两个任务都跟着 AUDIO_TASK_CORE 一起搬, 永远同核,
//     所以换核根本没改变"谁抢谁"的关系(库作者那条建议针对的是**库内解码任务**, 那条已满足)。
//
// 【v10 的两条对策】
//   1) 喂数据任务搬到**与解码任务相反的核**: 解码在 AUDIO_TASK_CORE, 喂数据就去另一个核。
//      这样它读卡读多久都不可能让解码任务饿死。放 UI 核还额外有个好处: 抢 sd_mutex 的
//      两方(刷屏与喂数据)在同一个核上, 不存在跨核优先级反转。
//   2) 关掉库里两个"每个输出帧都跑、而我们完全不需要"的后处理(VU 表 + 三档 EQ),
//      把 Core 0 的每帧开销还给 FLAC 解码器(见 audioInit 里的说明)。
//
// 【仪表】每秒一行 [afeed](打印在喂数据任务里, 只 printf 不 flush):
//   loop/s  这一秒 Audio::loop() 真正执行了多少次(2ms 间隔时理论 ~500)
//   to      等 sd_mutex 超时的次数(>0 说明喂数据被锁饿死过 3 秒)
//   wait=次数/最长ms      需要等锁的次数 / 单次等锁最久 —— 这个数就是"刷屏持锁多久"
//   sum=等锁总ms          这一秒里有多少毫秒花在等锁上(被 UI 拿走的时间)
//   busy=总ms/最长ms      持锁跑 Audio::loop() 的时间; **最长**才是关键:
//                         它就是"这一核被独占多久", 超过 ~150ms 就足以造成一次可听的空洞
//   rt      解码实时率(千分比, 累计): 库报的已解码秒数增量 ÷ 墙上时钟增量 × 1000
//           =1000 刚好实时; <1000 => 解码跟不上, I2S 必然周期性欠载(这就是"一顿一顿"本身)
//   inbuf   库输入缓冲里现在有多少字节(容量 655350); 长期几百 KB = 输入侧不是瓶颈
// ---------------------------------------------------------------------------
#define AUDIO_LOOP_INTERVAL_MS 2

// 喂数据任务所在核 = 与库内解码任务相反的核(AUDIO_TASK_CORE 只可能是 0/1)。
#define AUDIO_FEED_CORE ((AUDIO_TASK_CORE) == 0 ? 1 : 0)
// 优先级 2: 高于 loopTask(1), 拿到 sd_mutex 后能一口气把卡读完、尽快把锁还给刷屏;
// 又**不与解码任务同核**(解码是 Core AUDIO_TASK_CORE 上的优先级 2), 所以抢不到它的 CPU。
// 若上机发现界面被拖慢(wait 最大值明显变大), 可把它降到 1 让 FreeRTOS 按时间片轮转。
#define AUDIO_FEED_PRIO 2

// ---------------------------------------------------------------------------
// 【仪表】累加器: 只有喂数据任务读写, 无需加锁
// ---------------------------------------------------------------------------
struct AudioFeedDiag {
  uint32_t loopCalls;     // Audio::loop() 执行次数
  uint32_t lockTimeouts;  // 等 sd_mutex 超时次数
  uint32_t waitCount;     // 需要等锁(>0.2ms)的次数
  uint32_t waitUsSum;     // 等锁总耗时
  uint32_t waitUsMax;     // 单次等锁最久
  uint32_t busyUsSum;     // 持锁执行 Audio::loop() 的总耗时
  uint32_t busyUsMax;     // 单次 Audio::loop() 最久 <- "这一核被独占多久"
  uint32_t winMs;         // 当前统计窗口起点(millis)
};
static AudioFeedDiag s_feed = {};
static int s_rtTrack = -2;       // rt 统计基准: 曲目号
static uint32_t s_rtWallMs = 0;  // rt 统计基准: 墙上时钟
static uint32_t s_rtElMs = 0;    // rt 统计基准: 库报的播放位置(ms)

// ===========================================================================
// 【静音底噪】空闲时停掉 I2S 时钟 —— 消除待机时耳机里的电流声/嘶嘶声
// ===========================================================================
// 症状: 不放音乐停在主菜单时, 耳机里一直有嘶嘶声。
// 机理: PCM5102A 的 XSMT 已经被拉高(解除静音), 而 I2S 的 BCK/LRCK **从开机起一直
//       在跑**(库 I2S_init 里 i2s_channel_enable 之后就没人关过), 芯片就一直把时钟
//       转成模拟底噪。耳机的灵敏度下就听得见。
// 修法: 真正空闲(没在播 / 已暂停 / 已放完)持续 AUDIO_I2S_IDLE_STOP_MS 之后, 调库的
//       公开接口 I2Sstop() -> i2s_channel_disable() **停掉时钟输出**; 要出声再 I2Sstart()。
//
// ★ 为什么"恢复"必须显式做 ★
//   我把库翻了一遍: I2Sstart() 在 Audio.cpp 里**只有 I2S_init() 一处调用**(:6089),
//   connecttoFS()/playChunk() 都不会重新打开通道; pauseResume() 也完全不碰 I2S
//   (grep I2Sstart()/I2Sstop() 全库只有 :6084/:6089 两处)。
//   所以时钟一旦被我们停掉, **不显式 I2Sstart() 就一个字都放不出来** —— 这是本改动
//   最大的风险点, 三处都做了恢复: audioPlayIndex() 开始播放前、audioTogglePause()
//   恢复播放时、以及这个 tick 里的兜底(每 2ms 检查一次)。
//
// ★ 为什么放在这个任务里、且不持锁 ★
//   audioLoopOnce() 每 2ms 被喂数据任务调一次, 天然就是个计时器; 而且 SdLockGuard
//   只包住 s_audio->loop() 那一小段, 到这里锁已经放了 —— I2S 和 SD 无关, 不该占着
//   sd_mutex 调它(见任务要求)。
//
// I2Sstop() 内部会 clear m_outBuff/m_resamplesBuff, 所以只在"确实没在出声"时调,
// 否则会把正在播的缓冲丢掉。
static void audioIdleMuteTick(void) {
  if (!s_audio) {
    return;
  }

  // ① "应该在出声"吗? 正在播 + 没暂停 + 没放完  =>  确保静音解除
  const bool wantAudio = s_audio->isRunning() && !s_paused && !s_eofFlag;
  if (wantAudio) {
    if (s_i2sIdleStopped) {
#if AUDIO_I2S_IDLE_MUTE
      s_audio->I2Sstart();
      BOOTLOG("[audio] 恢复出声: 已重新打开 I2S 时钟");
#elif PIN_I2S_XSMT >= 0
      digitalWrite(PIN_I2S_XSMT, HIGH);
      BOOTLOG("[audio] 恢复出声: 已解除 XSMT 软静音");
#endif
      s_i2sIdleStopped = false;
    }
    s_idleSinceMs = 0;   // 不空闲, 重新计时
    return;
  }

  // ② 连续空闲计时(用 millis, 不阻塞)
  const uint32_t now = millis();
  if (s_idleSinceMs == 0) {
    s_idleSinceMs = now ? now : 1;
    return;
  }
  if ((uint32_t)(now - s_idleSinceMs) < AUDIO_I2S_IDLE_STOP_MS) {
    return;
  }
  if (s_i2sIdleStopped) {
    return;   // 已经静音了, 不用重复
  }

  // ③ 静音(两条路线, 见 config.h 的 AUDIO_I2S_IDLE_MUTE 说明)
#if AUDIO_I2S_IDLE_MUTE
  // ① 停 I2S 时钟: BCK/LRCK 一停, PCM5102A 就没有东西可转 -> 底噪消失。最彻底。
  //    需要先给库加 2 行 public: 补丁(库把 I2Sstart/I2Sstop 声明在 private 里)。
  s_audio->I2Sstop();
  s_i2sIdleStopped = true;
  BOOTLOG("[audio] 空闲 %lu 秒, 已停 I2S 时钟(消除耳机底噪)", (unsigned long)(AUDIO_I2S_IDLE_STOP_MS / 1000));
#elif PIN_I2S_XSMT >= 0
  // ② 拉低 XSMT 让芯片软静音。不需要改库, 但需要把 XSMT 从 3.3V 改接到这个 GPIO。
  digitalWrite(PIN_I2S_XSMT, LOW);
  s_i2sIdleStopped = true;
  BOOTLOG("[audio] 空闲 %lu 秒, 已拉低 XSMT(GPIO%d)软静音(消除耳机底噪)",
          (unsigned long)(AUDIO_I2S_IDLE_STOP_MS / 1000), PIN_I2S_XSMT);
#else
  // 两条路都没开: 什么都不做(见 config.h 里 AUDIO_I2S_IDLE_MUTE 的说明)。
  //   不打印, 免得刷屏; 需要确认时把 AUDIO_DIAG_LOG 打开即可。
#endif
}

// 内部实现: 真正去调库的 loop()
static void audioLoopOnce(void) {
  if (!s_audio) {
    return;
  }

  const uint32_t t0 = micros();
  bool got = false;
  uint32_t t1 = t0;
  {
    SdLockGuard g(SD_LOCK_TIMEOUT_MS);
    t1 = micros();   // 拿到锁的时刻(或超时返回的时刻)
    got = g.ok();
    if (got) {
      s_audio->loop();   // 可能是一次几十毫秒的阻塞 SD 读; 但本任务已不在解码任务的核上
    }
  }
  const uint32_t t2 = micros();

  const uint32_t waitUs = t1 - t0;
  if (waitUs > s_feed.waitUsMax) {
    s_feed.waitUsMax = waitUs;
  }
  s_feed.waitUsSum += waitUs;
  if (!got) {
    s_feed.lockTimeouts++;
  } else {
    if (waitUs > 200) {
      s_feed.waitCount++;
    }
    const uint32_t busyUs = t2 - t1;
    s_feed.loopCalls++;
    s_feed.busyUsSum += busyUs;
    if (busyUs > s_feed.busyUsMax) {
      s_feed.busyUsMax = busyUs;
    }
  }

  // 【诊断】每首只打印一次"库真正解析出来的参数"。
  //   为什么需要: audioPlayIndex() 里那行"源参数"是在 connecttoFS 之后 120ms 打的,
  //   那时库的 loop() 还没跑过, 读到的永远是默认值(48000/16/2、total=0),
  //   光看那行没法判断"文件头到底解析成功没有"。这里在读到时长为正时打印一次。
  if (!s_paramsLogged && s_audio->isRunning() && s_audio->getAudioFileDuration() > 0) {
    s_paramsLogged = true;
    BOOTLOG("[audio] 实际解码参数: 曲目=%d/%d %lu Hz / %u bit / %u ch, 时长=%lu 秒", s_trackIndex + 1,
            s_trackCount, (unsigned long)s_audio->getSampleRate(), (unsigned)s_audio->getBitsPerSample(),
            (unsigned)s_audio->getChannels(), (unsigned long)s_audio->getAudioFileDuration());
  }

  // 【静音底噪】空闲超时就停 I2S 时钟。放在最后: 此时 sd_mutex 已经释放(见函数开头
  // 的 SdLockGuard 作用域), 不会影响 SD/刷屏的持锁时间。
  audioIdleMuteTick();
}

// 每秒一行 [afeed]。用 Serial.printf 而不是 BOOTLOG: BOOTLOG 里有 Serial.flush(),
// 一次 flush 会阻塞十几毫秒; 这里一秒才 ~200 字节, 不 flush 就不会拖慢音频路径。
static void audioFeedReport(void) {
  if (!s_audio) {
    return;
  }
  const uint32_t now = millis();
  if (s_feed.winMs == 0) {
    s_feed.winMs = now;
    return;
  }
  if ((uint32_t)(now - s_feed.winMs) < 1000) {
    return;
  }

  // 解码实时率(累计, 千分比): 库报的"已解码秒数"增量 ÷ 墙上时钟增量 × 1000。
  const uint32_t elMs = s_audio->getAudioCurrentTime() * 1000u;
  if (s_trackIndex != s_rtTrack) {   // 换曲/停止 -> 重新取基准
    s_rtTrack = s_trackIndex;
    s_rtWallMs = now;
    s_rtElMs = elMs;
  }
  if (elMs < s_rtElMs) {   // 库把位置清零了(停播) -> 重新取基准, 避免下溢
    s_rtWallMs = now;
    s_rtElMs = elMs;
  }
  const uint32_t dWall = now - s_rtWallMs;
  const uint32_t dEl = elMs - s_rtElMs;
  const uint32_t rt = dWall ? (uint32_t)(((uint64_t)dEl * 1000u) / dWall) : 0;

  // ---- 默认(安静)模式: 只在真的异常时才打一行 ---------------------------------
  //   判据取"解码实时率 rt < 900"(= 跟不上播放速度, 也就是用户会听到的一顿一顿)
  //   或抢锁超时。健康时一行都不打; 一旦真的出问题, 不用重编就有证据。
  //   AUDIO_DIAG_LOG>=1 时改成每秒一行, 和排查时看到的一样。
#if AUDIO_DIAG_LOG < 1
  const bool abnormal = (s_audio->isRunning() && rt < 900) || (s_feed.lockTimeouts > 0);
  if (!abnormal) {
    s_feed.loopCalls = 0;
    s_feed.lockTimeouts = 0;
    s_feed.waitCount = 0;
    s_feed.waitUsSum = 0;
    s_feed.waitUsMax = 0;
    s_feed.busyUsSum = 0;
    s_feed.busyUsMax = 0;
    s_feed.winMs = now;
    return;
  }
  Serial.printf("[afeed] !! rt=%lu (低于900=跟不上播放) el=%lu/%lu to=%lu busy=%lums\n", (unsigned long)rt,
                (unsigned long)(elMs / 1000u), (unsigned long)s_audio->getAudioFileDuration(),
                (unsigned long)s_feed.lockTimeouts, (unsigned long)(s_feed.busyUsMax / 1000u));
#else
  Serial.printf("[afeed] core=%d loop/s=%lu to=%lu wait=%lu/%lums sum=%lums busy=%lums/%lums rt=%lu "
                "el=%lu/%lu inbuf=%lu run=%d\n",
                AUDIO_FEED_CORE, (unsigned long)s_feed.loopCalls, (unsigned long)s_feed.lockTimeouts,
                (unsigned long)s_feed.waitCount, (unsigned long)(s_feed.waitUsMax / 1000u),
                (unsigned long)(s_feed.waitUsSum / 1000u), (unsigned long)(s_feed.busyUsSum / 1000u),
                (unsigned long)(s_feed.busyUsMax / 1000u), (unsigned long)rt, (unsigned long)(elMs / 1000u),
                (unsigned long)s_audio->getAudioFileDuration(), (unsigned long)s_audio->inBufferFilled(),
                s_audio->isRunning() ? 1 : 0);
#endif

  s_feed.loopCalls = 0;
  s_feed.lockTimeouts = 0;
  s_feed.waitCount = 0;
  s_feed.waitUsSum = 0;
  s_feed.waitUsMax = 0;
  s_feed.busyUsSum = 0;
  s_feed.busyUsMax = 0;
  s_feed.winMs = now;
}

// 音频喂数据任务: 每 2ms 喂一次; 跑在**与解码任务相反的核**上(见上面的 v10 说明)。
static void audioTask(void *arg) {
  (void)arg;
  for (;;) {
    audioLoopOnce();
    audioFeedReport();
    vTaskDelay(AUDIO_LOOP_INTERVAL_MS / portTICK_PERIOD_MS > 0 ? AUDIO_LOOP_INTERVAL_MS / portTICK_PERIOD_MS : 1);
  }
}

void audioStartTask(void) {
  if (s_audioTaskStarted) {
    return;
  }
  s_audioTaskStarted = true;
  // 栈给足(库的 loop 里会解析文件头、做字符串处理)。
  const BaseType_t ok = xTaskCreatePinnedToCore(audioTask, "audiofeed", 4096, nullptr, AUDIO_FEED_PRIO, nullptr,
                                                AUDIO_FEED_CORE);
  BOOTLOG("[audio] 喂数据任务: Core %d (库解码任务在 Core %d), 优先级 %d, 间隔 %dms (%s)", AUDIO_FEED_CORE,
          AUDIO_TASK_CORE, AUDIO_FEED_PRIO, AUDIO_LOOP_INTERVAL_MS, ok == pdPASS ? "成功" : "失败");
  BOOTLOG("[audio] 每秒一行 [afeed] 诊断: loop/s=喂数据次数 to=抢锁超时 wait=次数/最长ms sum=等锁总ms "
          "busy=持锁总/最长ms rt=解码实时率(千分比,1000=刚好) inbuf=输入缓冲字节");
}

// 兼容旧调用点: 主循环不再调用它喂数据(改由 audioTask 负责)。
// 保留这个空实现是为了不破坏别的调用点, 也避免误以为"必须在主循环里喂"。
void audioLoop(void) {
  // 不再在主循环里喂数据。真正的喂数据在 audioStartTask() 建的任务里。
}

// 自动下一首的保护计数
static uint8_t s_consecFastEof = 0;   // 连续"秒结束"的次数
static bool s_autoNextSuspended = false;

void audioPollEndOfTrack(void) {
  if (!s_eofFlag) {
    return;
  }
  s_eofFlag = false;

  // ★ 防止"歌名乱跳" ★
  //   如果音频根本没解码成功, 库会立刻报告 evt_eof, 而自动下一首会马上切歌,
  //   新歌又立刻 EOF…… 结果曲目在 1<->2 之间无限循环, 界面上就是歌名不停乱跳。
  //   判据: 上一首是否真的播放过(elapsed > 0)。没播过就不是"放完了", 而是故障,
  //   这时不切歌, 连续几次后直接停掉自动切换并明确报错。
  const bool reallyPlayed = (s_lastElapsed > 0) || (s_audio && s_audio->getAudioCurrentTime() > 0);

  // ★ 关键诊断 ★ 每次收到 eof 都记录: 哪一首、播到第几秒、库报的时长,
  //   以及"是否真的播过"。这样就能区分三种情况:
  //     - 真的放完了(播过 + elapsed≈total)
  //     - 半路被停掉(播过但 elapsed << total) —— 解码器出错 / 被抢占
  //     - 从没播过(elapsed=0)                 —— 格式不被支持 / 文件头没解析
  BOOTLOG("[audio] 收到 evt_eof: 曲目=%d/%d 播到=%lu秒 时长=%lu秒 上次峰值=%lu秒 -> %s",
          s_trackIndex + 1, s_trackCount, (unsigned long)(s_audio ? s_audio->getAudioCurrentTime() : 0),
          (unsigned long)(s_audio ? s_audio->getAudioFileDuration() : 0), (unsigned long)s_lastElapsed,
          reallyPlayed ? "算正常结束, 准备切下一首" : "没播过, 判为故障(不切歌/接回上一首)");

  if (!reallyPlayed) {
    s_consecFastEof++;
    BOOTLOG("[audio] !! 曲目立刻报结束(没播过) -> 第 %u 次, 不切歌",
            (unsigned)s_consecFastEof);

    // ★【修复-2】把"刚才确实放过声的那首"接回来 ★
    //   为什么需要: 库在 connecttoFS() 里已经把上一首 stop 掉了(见本文件顶部说明),
    //   新这首又秒失败 —— 上面"不切歌"的保护只做到"不再乱跳", 结果是彻底静音,
    //   用户看到的就是"一进播放界面就没声"(其实是按下选中那一刻就没声了)。
    //   这里只恢复一次(恢复后把 s_lastGoodIndex 清掉), 且连续失败 2 次以上就不再恢复,
    //   所以不可能出现"两首之间来回跳"的老问题。
    if (s_lastGoodIndex >= 0 && s_lastGoodIndex != s_trackIndex && s_consecFastEof <= 2) {
      const int back = s_lastGoodIndex;
      s_lastGoodIndex = -1;
      BOOTLOG("[audio] 秒失败 -> 自动接回上一首: [%d/%d] %s", back + 1, s_trackCount, s_tracks[back].title);
      audioPlayIndex(back);
      return;
    }

    if (s_consecFastEof >= 3) {
      s_autoNextSuspended = true;
      BOOTLOG("[audio] !! 连续 %u 次都无法播放, 已停止自动切歌。", (unsigned)s_consecFastEof);
      BOOTLOG("[audio] !! 这说明解码器没能认出/播放这些文件(不是'放完了')。");
    }
    return;
  }
  s_consecFastEof = 0;

  if (s_autoNext && !s_autoNextSuspended && s_trackCount > 0) {
    audioPlayNext();
  }
}

void audioShutdown(void) {
  if (!s_audio) {
    return;
  }
  s_audio->stopSong();   // 先停播并关闭文件句柄
  s_audio->setVolume(0, 0);
  delay(50);             // 给音频任务时间退出解码循环
  log_i("[audio] 已停止播放, SD 卡可以安全断电");
}
