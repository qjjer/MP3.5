/*
 * audio_player.h — MP3 / FLAC 播放（基于 ESP32-audioI2S 3.4.x）
 *
 * 【双核分工】(v10 更新: 喂数据任务已搬到解码任务的**另一个**核)
 *   Core 0 : 库内解码任务(priority 2, xTaskCreateStaticPinnedToCore, 核由
 *            setAudioTaskCore(AUDIO_TASK_CORE) 决定, 当前 config.h = 0)
 *            —— 它每 1ms 跑一次, 把 InBuff 解成 PCM 并写 I2S。**喂饱它 = 不断音**。
 *   Core 1 : loopTask(UI/刷屏/输入, priority 1) + 输入任务(2) + 我们的喂数据任务(2)
 *            —— 喂数据任务只做"抢 sd_mutex -> Audio::loop()", 每次最多几十毫秒的 SD 读,
 *               和刷屏共用同一把锁, 所以和刷屏放在同一个核上最合理(无跨核优先级反转)。
 * 关键点: 喂数据任务**绝不能**和解码任务同核。同核时它那几十毫秒的阻塞读会把解码任务
 *         按在核外, I2S 就会欠载(auto_clear=true 自动补 0) -> 听感就是"一顿一顿"。
 *
 * 【为什么音频读卡不需要拿 sd_mutex —— 这点很容易搞反，务必看完】
 * 本库通过 m_audiofile.read(buff, len) 一次性读一大块，而 SD 库把**整个块读**
 * 包在一个 beginTransaction()/endTransaction() 里，底层 HAL 锁(SPI_MUTEX_LOCK)
 * 保证这一个块读不会和别的 SPI 事务交错 —— 也就是说"读块"本身已经是原子的。
 *
 * 真正会被"插队打碎"的只有 TFT 的整帧推送：TFT_eSPI 在 ESP32-S3 上直接写
 * SPI 寄存器（SPI_CMD_REG(2)），绕过了 SPIClass，所以 HAL 锁保护不到它，
 * 只能靠 sd_mutex 把"整帧推送"变成原子操作。
 *
 * 结论：sd_mutex 是给 UI 的"刷屏"用的；音频库内部读卡既没有钩子、也不需要它。
 *       （本库的 fs::FS / fs::File 都不是虚函数，包一层 FS 也拦不到 File::read()，
 *         所以不要去做"假装能拦"的包装。）
 *
 * 【96kHz / 24bit FLAC】
 * 库的 I2S 输出固定重采样到 48kHz（上限），24bit 位深是支持的。所以 96kHz FLAC
 * 能正常播放，但到 DAC 时是 48kHz。这是库的设计限制，不是配置问题。
 */
#pragma once

#include <Arduino.h>

// 播放列表里的一项
//
// ★ 缓冲为什么这么大(实测定下来的) ★
//   用户卡上的音乐文件名形如:
//     /music/【5小时不重复纯享版】_想念变成空气在叹息，... - 1.【高音质】....mp3
//   中文长名 + "第 N 首"前缀, 轻松超过 100 字符。原来 path[96] 会**把 .mp3 后缀截掉**,
//   于是扩展名检查判定"不是音频文件" -> 曲目列表空 -> 界面报"未找到音频文件"。
//   现在 path 给到 256, 足够放下真实的长文件名 + 目录前缀。
struct Track {
  char path[256];   // 完整路径(必须放得下超长文件名, 见上面说明)
  char title[128];  // 只用于显示; 界面一行最多约 19 个汉字, 128 够截断显示了
};

struct AudioStatus {
  bool playing = false;      // 正在播放（非暂停）
  bool paused = false;       // 暂停中
  bool loaded = false;       // 有已载入的曲目
  uint8_t volume = 0;        // 0~21
  uint32_t elapsedSec = 0;   // 当前播放位置（秒）
  uint32_t totalSec = 0;     // 总时长（秒，0 = 未知/流）
  int trackIndex = -1;       // 当前曲目序号
  int trackCount = 0;
  char title[48] = {0};
  // 最近一次"选中某首却没能播起来"的原因(给播放界面显示)。
  // 为什么必须有: 预检拦下来时, 上一首还在放, 界面若不说明, 用户只会看到
  // 播放页显示"停止/00:00"--:--", 误以为是"进了这个界面就没声"。
  // 空字符串 = 没有失败信息。
  char notice[48] = {0};
  bool lastAttemptFailed = false;   // true = 上一次 audioPlayIndex() 被拒/失败
};

// 初始化 I2S + 音频任务。返回 false 表示初始化失败。
// 必须在 sdLockInit() 之后调用。
bool audioInit(void);

// 扫描 SD 卡上的音频文件建播放列表（会在瞬间持 sd_mutex）。
// 目录优先级：/mp3 -> /music -> / （只扫一层）
int audioScanTracks(void);
int audioTrackCount(void);
const char *audioTrackTitle(int index);

// 播放控制
bool audioPlayIndex(int index);
bool audioPlayNext(void);
bool audioPlayPrev(void);
void audioTogglePause(void);
void audioStop(void);

// 音量 0~21，返回实际生效值
uint8_t audioSetVolume(uint8_t vol);
uint8_t audioGetVolume(void);
// 相对调整（旋转编码器用），返回调整后的音量
uint8_t audioVolumeUp(int8_t delta);

// 状态查询（内部只读，无锁）
AudioStatus audioGetStatus(void);

// ★ 必须由主循环高频调用 ★
//
// ESP32-audioI2S 的设计是"库自身不建处理任务", 所有文件读取/解码器创建/文件头解析
// 都在 Audio::loop() 里完成 —— 库的二十多个示例, 每一个的 loop() 里都有 audio.loop()。
// 如果没人调用它, 现象非常隐蔽:
//     setPinout 成功 -> 音频任务启动(running=1) -> connecttoFS 返回 true
//     但 processLocalFile() 永不执行 -> 解码器永不创建 -> InBuff 永不填充
//     -> performAudioTask() 只会在"没有解码器"的分支里空转(反复 gain_ramp)
//     -> elapsed=0 / total=0 / 一点声音都没有
// 这就是"串口说开始播放、播放界面也有、但完全没声音"的根本原因。
//
// ★ 现在改由独立任务负责, 主循环不用再调它 ★
//   见 audioStartTask(): 喂数据的 audio.loop() 跑在专用任务里(2ms 一次),
//   位置在**与库内解码任务相反的核**上(解码在 AUDIO_TASK_CORE -> 喂数据去另一核),
//   和参考项目 ESP32_LVGL_MusicPlayer 的做法一致(独立任务里高频喂, 不占主循环)。
//   audioLoop() 保留为空实现, 只是为了不破坏历史调用点。
void audioLoop(void);

// 启动"音频喂数据"任务(2ms 一次, 优先级 2, 核 = 与解码任务相反的那个)。
// 必须在 audioInit() 成功之后调用一次(可重复调用, 内部只启动一次)。
// 启动后会每秒打印一行 [afeed] 诊断(loop 次数 / 抢锁失败 / 等锁与持锁耗时 /
// 解码实时率 / 输入缓冲字节数), 用来判断"一顿一顿"是被谁拖的。
void audioStartTask(void);

// 由主循环调用：把"曲目自然结束"事件转成"切下一首"
void audioPollEndOfTrack(void);

// 是否需要"曲目结束自动下一首"
void audioSetAutoNext(bool en);

// 硬关机前的收尾：停播 + 关文件 + 等 DMA 排空
void audioShutdown(void);
