# bookreader — ESP32-S3 便携小说阅读器 + Hi-Fi 播放器

**当前进度：三步全部完成。**
① TXT 阅读器 ② 音频播放 ③ EPUB 解析 —— 均通过编译与数据校验。

实测状态：屏幕已点亮、启动流程跑通到 SD 卡挂载（未插卡会在屏幕上提示并可按键重试）。

- 硬件目标：ESP32-S3-N16R8（16MB Flash + 8MB OPI PSRAM）
- 显示：ST7789 240×320 SPI，与 MicroSD **共用 SPI 总线**
- 音频：ESP32-audioI2S 3.4.7 驱动 PCM5102A（I2S 独立外设，不占 SPI 带宽）
- 状态：`arduino-cli` 实测编译通过（821KB / 3MB APP，静态 RAM 41KB），**项目自身零告警**
- 已验证：
  - 用 382KB 真实中文小说跑通分页，**117875 个字符一个不丢**、页边界字节首尾相接
  - 音频库的实际任务核/优先级、TFT 锁覆盖情况由 `tools/verify_concurrency.py` 机械校验

> ⚠ **先读第 6.6 节"已知边界"**：音频库内部的读卡无法被我们的锁约束，这是本步最重要的一条限制，
> 不要误以为已经彻底解决。

---

## 1. 目录结构

```
bookreader/
  bookreader.ino     主程序：启动自检、找书、主循环（Arduino 会要求 .ino 与文件夹同名）
  config.h           引脚定义、排版参数、音频参数、编译期开关（改配置看这里）
  sd_mutex.h/.cpp    共享 SPI 总线的高层互斥锁（★ 本项目最关键的一块）
  display.h/.cpp     屏幕抽象层：TFT_eSPI / Adafruit 双后端 + PSRAM 整屏帧缓冲
  audio_player.h/.cpp 音频播放：I2S 初始化、播放列表、播放/暂停/切曲、音量、状态
  font.h/.cpp        24×24 点阵字库加载、Unicode 二分查找、字形缓存、汉字渲染
  text.h/.cpp        UTF-8/GBK 解码、自动折行、标点避头尾与悬挂、跨页字节回退
  txt_reader.h/.cpp  TXT 分页、翻页、章节扫描与跳转
  epub_parser.h/.cpp EPUB 解析：ZIP 中央目录 + ROM miniz 解压 + XHTML 提纯 + 图片提取
  image_view.h/.cpp  插图页渲染：TJpg_Decoder(JPEG 降采样) + PNGdec(PNG) -> 帧缓冲
  bus_init.h         屏幕与 SD 共用的 SPI 总线初始化（两个 sketch 共用同一段代码）
  miniz.h 等 3 个     ESP32-S3 ROM 的原版头文件（从 esp_rom/include 复制，非改写）
  ui.h/.cpp          阅读界面、章节目录、提示界面、状态栏（含音乐播放状态）
  input.h/.cpp       EC11 旋转编码器解码、按键短按/长按、电池电压
  tft_setup.h        TFT_eSPI 的 sketch 本地配置（不用改库文件）
  tools/
    build_font.py       电脑端：TTF → 24×24 点阵字库 .f24（★ 必须先跑这个）
    check_font.py       电脑端：校验 .f24 并把字形用字符画出来肉眼确认
    convert_txt.py      电脑端：GBK → UTF-8 批量转换
    validate_layout.py  电脑端：复刻设备分页算法做压力测试
    verify_engine.py    电脑端：字节级守恒验证（定位吞字/重复用）
    verify_concurrency.py 电脑端：音频并发安全验证（TFT 锁覆盖 + 任务优先级）
    validate_epub.py    电脑端：EPUB 提取链路验证（解压 + 标签剥离 + 排版预览）
  third_party/
    ESP32-audioI2S/     音频库副本（3.4.7，保持未修改；仅作离线参考/装库来源）
```

---

## 2. 接线

严格按你的方案，不需要改：

| 模块 | 引脚 | ESP32-S3 | 说明 |
|---|---|---|---|
| ST7789 | SDA/MOSI | GPIO 11 | **与 SD 共用** |
| | SCL/SCK | GPIO 12 | **与 SD 共用** |
| | CS | GPIO 10 | |
| | DC | GPIO 9 | |
| | RST | GPIO 8 | |
| | BL | 3.3V | 常亮 |
| | SDO | 悬空 | **所以不要定义 TFT_MISO** |
| MicroSD | MOSI | GPIO 11 | 共用 |
| | SCK | GPIO 12 | 共用 |
| | MISO | GPIO 13 | 仅 SD 使用 |
| | CS | GPIO 14 | |
| EC11 | A / B | GPIO 1 / 2 | 内部上拉，公共端接 GND |
| | 按键 | GPIO 3 | 另一端接 GND |
| 电池 | 分压中点 | GPIO 6 | 正极 →10k→ GPIO6 →10k→ GND |

`GPIO 11/12/13` 在 ESP32-S3 上属于普通 IO，可以自由用；`GPIO 19/20`（USB）、`GPIO 26~32`（OPI PSRAM）不要占用。

---

## 3. 软件准备

### 3.1 Arduino IDE 设置（对应 esp32-esp32 3.3.11，已实测）

| 项目 | 取值 |
|---|---|
| 开发板 | **ESP32S3 Dev Module** |
| PSRAM | **OPI PSRAM** ← 不开的话整屏帧缓冲（150KB）分配不出来 |
| Flash Size | 16MB |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| USB CDC On Boot | Enabled（否则串口看不到打印） |
| Flash Mode | QIO 80MHz |
| CPU Frequency | 240MHz |

> 关于 2.0.17：项目约束里写 arduino-esp32 2.0.17 兼容性最好，但本机装的是 **3.3.11**，代码已按它验证通过。
> 本步骤的代码在两版核心上都能编译（`SD.begin(cs, spi, freq)` 等 API 两版一致）。
> 建议**先别换**，等第二步接音频时如果 ESP32-audioI2S 有编译问题再降到 2.0.17。

### 3.2 依赖库

| 库 | 版本（实测） | 装法 |
|---|---|---|
| TFT_eSPI | 2.5.43 | 库管理器搜 "TFT_eSPI" |
| ESP32-audioI2S | **3.4.7** | 库管理器搜不到, 用下面第 3.4 节的办法装 |
| TJpg_Decoder | 1.1.0 | 库管理器搜 "TJpg_Decoder"（EPUB 插图 JPEG 解码） |
| PNGdec | 1.1.6 | 库管理器搜 "PNGdec"（EPUB 插图 PNG 解码） |
| （备选）Adafruit ST7735/ST7789 + Adafruit GFX | 1.11.0 / 1.12.6 | 只在用备选后端时才需要 |

> ⚠ 音频库**必须用 3.4.x**（本项目按 3.4.7 验证）。master 分支已经是 4.0.0 的新 API
> （`Audio::msg_t` 回调、任务内部自建），和 3.x 的用法不兼容，混用会编译不过。
> **保持库为未修改的原版**：本项目早期试过给库加"SD 访问钩子"补丁, 已放弃并回退（原因见 6.6）。

### 3.3 安装 ESP32-audioI2S 3.4.7

库管理器里搜不到这个版本（名字是 `ESP32-audioI2S-master`，且版本可能不对），直接下载 tag：

```powershell
:: 先确认你的库目录（IDE: 文件 -> 首选项 -> 项目文件夹位置; 脚本里可能不同）
:: 下面假设是 C:\Users\<你>\Documents\Arduino\libraries
$lib = "$env:USERPROFILE\Documents\Arduino\libraries\ESP32-audioI2S"

git clone --depth 1 --branch 3.4.7 https://github.com/schreibfaul1/ESP32-audioI2S.git $lib
Remove-Item "$lib\.git" -Recurse -Force
```

没有 git 也可以用浏览器下载 <https://github.com/schreibfaul1/ESP32-audioI2S/archive/refs/tags/3.4.7.zip>，
解压后把文件夹改名成 `ESP32-audioI2S` 放进 libraries 目录。

装完可以用本项目的校验脚本确认版本和优先级都读得到：

```powershell
cd bookreader\tools
python verify_concurrency.py
```

### 3.4 TFT_eSPI 配置：**不用改库文件**

`tft_setup.h` 放在 sketch 目录里，TFT_eSPI 会自动优先加载它（`TFT_eSPI.h` 里有 `__has_include(<tft_setup.h>)`），
所以你的 `User_Setup.h` 保持原样即可，互不干扰。

`tft_setup.h` 里**绝对不能**定义 `USE_HSPI_PORT` 或 `USE_FSPI_PORT`，原因见第 5 节。

---

## 4. SD 卡准备

```
SD 卡（FAT32）
 ├─ fonts/
 │   └─ 任意名字.f24      ← 必须有，24×24 点阵字库（名字随你取，见下）
 ├─ txt/
 │   └─ 你的小说.txt      ← UTF-8 编码
 ├─ mp3/                 ← 音乐放这里（也支持 /music 或根目录）
 │   ├─ 001.mp3
 │   └─ 002.flac
 ├─ cache/               ← EPUB 解压出的纯文本缓存（程序自动创建，可随时删）
 └─ （根目录也可以放 .txt / .epub）
```

音乐目录按 `/mp3` → `/music` → `/audio` → 根目录 的顺序找，**只扫一层**，最多 256 首，
按文件名顺序播放。放进 `/mp3` 后用 `001_曲名.mp3` 这种补零命名可以保证顺序。

### 4.1 生成字库（必做）

需要 Python + Pillow（`pip install pillow`）：

```powershell
cd bookreader\tools
python build_font.py --font C:\Windows\Fonts\simhei.ttf --out gbk24.f24
```

实测输出：**21886 个字形，1,582 KB**，覆盖 GBK 全集（含全部常用/次常用/繁体汉字与全角标点）。

生成后**先校验再拷卡**：

> **文件名随你取**：程序启动时会扫 `/fonts/` 下的**第一个 `.f24` 文件**，所以叫
> `gbk24.f24`、`sourcehan24.f24`、`黑体.f24` 都行。同一个目录下**只放一个** `.f24`，
> 放多个时用哪一个是不确定的。想换字体只要替换那个文件，**不用重新编译烧录**。
> 实在一个都没找到时，才会回退去找 `/fonts/gbk24.f24`。


```powershell
python check_font.py gbk24.f24 --text "永书"
```

它会做格式与索引检查（升序无重复、常用字齐全），并把字形用 `#`/`.` 画出来，你肉眼确认没问题再拷进 `/fonts/`。

常用参数：

```powershell
python build_font.py --bpp 4          # 4bit 灰度抗锯齿, 笔画多的字更饱满(文件大 4 倍)
python build_font.py --threshold 90   # 笔画太细就调小, 太糊就调大(默认 110)
```

### 4.1.1 字体怎么选（三种，都实测零缺字）

| 字体 | 位深 | 大小 | 特点 |
|---|---|---|---|
| SimHei 黑体 | 1bit | 1695 KB | 笔画轻盈，24×24 下最"透气" |
| 思源宋体 SC Bold | 1bit | 1695 KB | 笔画饱满、占满格，衬线体更有书卷气 |
| 思源宋体 SC Bold | **4bit 灰度** | 6644 KB | 带抗锯齿，边缘更细腻；每页多传约 37KB，翻页略慢 |

三种都用同一份字库生成流程产出，**对 TXT 和 EPUB 正文都是零缺字**（实测 2279 / 2278 个
不重复字符全覆盖）。建议先拷 1bit 版确认一切正常，再试 4bit 看效果。

注意 **24×24 是硬性要求**：`config.h` 里的 `FONT_W/FONT_H` 必须与字库一致，程序启动时会
校验，不一致会直接报错（这是为了防止"字库对了但尺寸错了"导致花屏）。

### 4.2 TXT 编码

设备端**只认 UTF-8**。原因：arduino-esp32 3.x 的核心移除了 GBK↔Unicode 转换表（2.0.x 里有 `_GBK2UNICODE`），
本项目没有内嵌约 4 万字节的转换表。遇到 GBK 文件屏幕会明确提示，不会静默显示乱码。

```powershell
python convert_txt.py D:\novels          # 批量转 UTF-8, 输出到 novels_utf8
```

---

## 5. 操作方式

**阅读界面**

| 操作 | 功能 |
|---|---|
| 旋转 | 翻页（1 档 = 1 页） |
| 短按 | 打开章节目录 |
| 长按 | 开/关**音乐面板**（开了之后旋转调音量、短按播放暂停；再长按关闭） |

**章节目录**

| 操作 | 功能 |
|---|---|
| 旋转 | 上下选章节 |
| 短按 | 跳到该章 |
| 长按 | 返回阅读 |

**状态栏**（底部）：电量百分比 · 当前页号与阅读进度 · 右侧显示音乐状态
（`>` 播放中 / `||` 暂停 + `分:秒` + `V音量`），没有音乐时显示临时提示。

**音乐**：开机自动扫描并播放第一首；一首放完自动下一首。阅读和音乐是**同时进行**的。

---

## 6. 设计说明（为什么这么写）

### 6.1 ★ 屏幕和 SD 共用 SPI 总线：为什么要自己加锁

这是本项目唯一真正棘手的地方，结论基于实际读核心源码：

1. `SD/src/sd_diskio.cpp` 的 `sdcard_init()` **只保存 `SPIClass*`、不初始化总线**，每次读写用
   `beginTransaction()` / `~AcquireSPI()` 包住。所以屏幕和 SD 可以共用**同一个 `SPIClass` 对象**。
2. `SPI.cpp` 的 `beginTransaction()` 里有 `SPI_PARAM_LOCK()`，**单个 SPI 事务**是串行的；
   而且 `SPIClass::begin()` 是幂等的（`if (_spi) return true`），所以初始化顺序可以自己控制：
   `SPI.begin(SCK, MISO, MOSI, -1)` → `tft.init()` → `SD.begin(CS, SPI, freq)`。
3. **但 TFT_eSPI 在 ESP32-S3 上是直接写 SPI 寄存器的**：
   ```c
   #define SPI_PORT 2
   volatile uint32_t* _spi_cmd = (volatile uint32_t*)(SPI_CMD_REG(SPI_PORT));
   ```
   它推像素时不走 `SPIClass`，**核心那把总线锁管不到它**。
4. 于是会出现：刷一屏要几百个事务，音频任务在这中间插进来读一个 SD 扇区 → **花屏**。

所以 `sd_mutex` 提供的不是"重复的锁"，而是**唯一的**串行化手段，粒度是"一整帧"：

```c
// display.cpp — 整帧一次锁 + 一次传输
SdLockGuard g(SD_LOCK_TIMEOUT_MS);
tft.startWrite();
tft.pushImageDMA(0, 0, TFT_W, TFT_H, s_frameBuf);
tft.endWrite();
```

**规则：任何访问 SD 或刷屏的代码都必须包在 `SdLockGuard` 作用域内。**
锁是递归的，超时 3 秒（`SD_LOCK_TIMEOUT_MS`），超时会打印 `[sd] lock timeout` 并跳过本帧，不会死等。

### 6.2 为什么渲染要走整屏帧缓冲

先在 PSRAM 里把整页画完（`Canvas16`，240×320×2 = 150KB），再一次性刷上去：

- 不会出现"画到一半被读卡插队"的撕裂；
- SPI 总线只被占用一次，音频任务等待时间可预测（40MHz 下一帧约 24ms）；
- 字体渲染直接写帧缓冲的像素数组（`canvas.pixels()`），不走 `drawPixel` 虚函数，快 3~5 倍。

### 6.3 中文字库方案

- 电脑端把 TTF 渲染成 24×24 点阵，**按 Unicode 码点升序**建索引表。
- 设备端只需对索引表做二分查找，**因此不需要在 MCU 里放 GBK↔Unicode 大表**。
- 索引表（约 42KB）启动时整表读进 PSRAM，之后查找纯内存操作，不碰 SD。
- 字形位图按需读 + 256 槽直接映射缓存。一页小说常用字重复率高，命中率通常 70%+。
- 缺字会画一个**空心方框**占位，一眼就能看出是字库没收录，而不是显示成空白。

### 6.4 中文排版

`text.cpp` 实现了几条中文排版必备规则：

- **自动折行**：断点要求"前一个字能收尾 且 后一个字能起头"；西文尽量不在词中间断。
- **标点避头尾**：`。，！？）》」` 等不能出现在行首；`《「（` 等不能出现在行尾。
- **标点悬挂**：行尾放不下的禁则标点，挤在本行行尾（该行多占一格），而不是掉到下一行行首。
- **全角空格缩进**：`U+3000` 不允许出现在行首、也不允许做行尾，必须跟后面的字粘在一起。
- **跨页不吞字**：分页器内部有字节缓冲，翻页时用"绝对文件位置"回退多读的字节（见下）。

> **踩过的坑（已修）**：最初用缓冲区内的相对偏移（`bufPos_`）做回退，但 `fill()` 每次都会把
> 已消费的字节挤出去并把 `bufPos_` 归零，导致排版一行期间若跨了多次 `fill()`，回退量就会少算。
> 在 382KB 真实小说上表现为**丢失 34812 字节（约 11604 个汉字，10%）**。
> 现在改为用 `file.position() - unconsumed()` 换算**绝对位置**再回退，并新增
> `Paginator::trailingEndOffset()` 供分页调用方使用（它会把"悬挂标点待退回的字节"也算进去，
> 否则页尾的悬挂标点会在下一页被重复输出一次）。

### 6.5 章节识别

打开书时扫描文件（最多 8MB，分块读，屏幕显示进度，按一下按钮可打断），
识别形如 `第X章 / 第X节 / 第X卷 / 第X回 / 第X篇 / 第X部 / 第X集` 的行作为章节，
最多记 512 章。UI 里旋转选择、短按跳转。

### 6.6 ★ 已知边界：音频库内部的读卡无法加锁（务必知悉）

先说结论：**「我方读卡」与「我方刷屏」是严格互斥的；但「音频库内部读卡」不受这把锁约束。**
这是本库的接口限制，不是配置问题。

原因链条：

1. 音频库把读文件写成 `Audio` 的**私有成员** `m_audiofile.read(buff, len)`，且实现在 `Audio.h` 里可见，
   没有回调、没有虚函数、没有钩子点。
2. 本项目试过**给库加弱符号钩子**（在块读前后取/放 `sd_mutex`）。结果：因为钩子的空函数体在头文件里
   可见，编译器把调用点**直接内联成空操作**——ELF 里根本不产生调用，`noinline`+`used` 也救不回来。
   该方案已**放弃并完整回退**，`third_party/ESP32-audioI2S` 保持与上游一致。

现状与缓解（README 之所以强调这条，是因为它决定"会不会偶尔花屏"）：

| 层面 | 保护手段 | 强度 |
|---|---|---|
| 我方 SD 读（字库、TXT） | `sd_mutex` | 严格互斥 ✓ |
| 我方刷屏（整帧推送） | `sd_mutex`（整帧一次锁） | 严格互斥 ✓ |
| 音频库内部读卡 | 核心 HAL 锁（保证**单次块读**原子）+ 音频任务优先级更高 | 尽力而为 |

具体影响：音频读卡与整帧刷屏之间仍可能在毫秒级互相插入。缓解措施有三条：

1. **音频任务优先级（2）高于 UI 任务（1）** —— 音频要读卡时直接抢占 UI，不会因为刷屏而卡顿；
2. **一帧只占一次总线**，时长可预测（40MHz 下约 24ms），不是几百个小事务；
3. **库自带约 640KB 缓冲**（`m_mainBuffSize = UINT16_MAX*10`，分配在 PSRAM），足以吸收偶尔的等待。

如果实际使用中出现可见花屏，可按顺序尝试：把 `SPI_FREQUENCY` 降到 27MHz → 关掉 `DISPLAY_USE_DMA`。
若要从根上消除，只有两条路（都不在本步范围内）：给库打**真正生效的源码补丁**（不是弱符号），
或让屏幕走**独立 SPI 总线**（需要额外引脚）。

### 6.7 音频架构与几个关键数字

- **任务分核**：UI/`loop()` 在 **Core 1**（Arduino 默认的 `loopTask`），音频任务被
  `audio.setAudioTaskCore(1)` 也钉在 **Core 1**，但优先级 **2 > 1**，所以是"音频抢占 UI"。
  这是库作者推荐的用法（库注释：*If the ARDUINO RUNNING CORE is 1, the audio task should be core 0 or vice versa*，
  本质是"别让两者争同一优先级"）。
- **缓冲**：库内 `AudioBuffer` 主缓冲 `UINT16_MAX*10 ≈ 640KB` + 保留 `64KB`，经 `ps_ptr` 分配到 PSRAM。
  远超 64KB 的要求。
- **采样率**：库的 I2S 输出**固定重采样到 48kHz**（上限），24bit 位深支持。
  所以 **96kHz FLAC 能正常播放，但送到 DAC 时是 48kHz**。这是库的设计，不是配置问题。
- **格式**：mp3 / flac / wav / m4a / aac / ogg 均可（本项目的播放列表只收这几种扩展名）。
- **音量**：库的范围是 **0~21**，映射到 EC11 的每一档 ±1。
- **硬关机**：`audioShutdown()` 先 `stopSong()` 关闭文件句柄、再等 50ms 让解码循环退出，
  之后才允许断 VCC —— 顺序反了有损坏 SD 卡的风险。

---

## 7. 电脑端验证（烧录前就能查错）

设备端和电脑端无法互相验证，所以我把关键算法在 Python 里复刻了一份做对拍：

```powershell
cd bookreader\tools

:: 1) 分页压力测试：用内置样本或你的真实小说
python validate_layout.py --file D:\novels\13.txt
python validate_layout.py                 # 用内置样本

:: 2) 字节级守恒验证：断言"输入字节 == 输出字节"，能精确抓出吞字/重复
python verify_engine.py

:: 3) 音频并发安全：审计所有 TFT 写操作是否在锁内 + 从库源码读任务核/优先级
python verify_concurrency.py
```

这两步在开发中真的抓出了三个会毁掉阅读体验的 bug（换行符吞字、悬挂标点字节泄漏、跨页回退少算），
建议你改动 `text.cpp` 后都跑一遍。

---

## 8. 排错

| 现象 | 原因 / 处理 |
|---|---|
| 串口打印 `PSRAM 没开` | 工具菜单里把 PSRAM 设为 **OPI PSRAM**，否则 150KB 帧缓冲分配不出来 |
| `字库加载失败` + 详细原因 | 屏幕会直接显示 `font.cpp` 报出的具体原因（打不开 / magic 不对 / 尺寸不匹配 / 文件不完整） |
| 显示"缺字方框" | 该字不在字库。你用的是 GBK 全集字库，通常只剩 GB18030 扩展区的极生僻字 |
| `这个 TXT 不是 UTF-8` | 用 `tools/convert_txt.py` 转一下 |
| 花屏 / 撕裂 | 说明有代码绕过了 `SdLockGuard` 直接访问了 SPI 或 SD |
| 屏幕有雪花、错位 | `tft_setup.h` 里把 `SPI_FREQUENCY` 从 40000000 降到 27000000 或 20000000 |
| 颜色红蓝反了 | `tft_setup.h` 里把 `TFT_RGB_ORDER TFT_BGR` 改成 `TFT_RGB` |
| 画面整体偏移 | 改 `config.h` 的 `TFT_X_OFFSET` / `TFT_Y_OFFSET` |
| 编译报 `USE_HSPI_PORT` 相关断言 | `tft_setup.h` 里删掉 `USE_HSPI_PORT` / `USE_FSPI_PORT`（S3 上会切到 SPI3，和 OPI PSRAM 冲突） |

串口（115200）会打印字库字形数、章节数、字形缓存命中率，可用于判断字库是否正常。

---

## 9. EPUB 解析（已完成）

### 9.1 实现方式：解压成文本缓存，复用 TXT 阅读器

**为什么不直接流式分页**：第一步的排版引擎（`text.cpp` 的 `Paginator`）是用 38 万字真实小说
验证过"字符一个不丢"的，里面那套缓冲区/回退逻辑踩过三个真 bug 才修好。EPUB 要直接流式喂给它，
就得改动这套核心逻辑，风险太高。

所以走"先解压成纯文本缓存，之后完全复用 TXT 阅读器"的路线：

```
首次打开 某本书.epub
   -> 读 META-INF/container.xml 拿到 content.opf
   -> 读 content.opf 的 manifest/spine，按 spine 顺序取 26 个 xhtml
   -> 逐个解压（跳过 17MB 图片）-> 剥离标签 -> 拼成纯文本
   -> 写入 /cache/某本书.txt
以后打开
   -> 直接读缓存，秒开
```

用你的真实 EPUB 实测：**26 个 spine 项、487KB XHTML、提取 150121 字、0 个标签残留**。

### 9.2 解压用什么

ESP32-S3 的 **ROM 里自带 miniz**（`tinfl_decompress` 在 `esp32s3.rom.ld` 里），不需要额外库。
但 ROM 头文件 `esp_rom/include/miniz.h` 不在 Arduino 的 include 搜索路径上，所以把 3 个头文件
（`miniz.h` / `esp_rom_caps.h` / `esp_rom_crc.h`）**原版复制**到 sketch 目录即可 —— 没有改写，
也不用改任何编译配置。

### 9.3 标签剥离的几个要点

用你的文件确认过结构：正文页是 `<p>` 分段、缩进用**全角空格 U+3000**（不是 CSS）、
`<br>` 作空行、**没有 `<ruby>` 注音**、没有 HTML 实体。剥离规则：

| 处理 | 规则 |
|---|---|
| `<br> <p> </p> <div> <h1..h6> <li>` | 转换成换行 |
| `<script> <style>` | 连同**内容**一起丢弃（不能只丢标签，否则脚本会混进正文） |
| `<head>` | 整个跳过（否则书名会混进正文开头） |
| `<img>` | 记为换行；整页图片的 spine 项由图片页机制显示 |
| 行首 ASCII 空格/制表符 | 去掉 |
| **全角空格 U+3000** | **保留**（那是中文段落缩进） |
| 连续空行 | 最多压到一个 |

> 最后两条是实测踩出来的：XHTML 里 `<p>` 标签之间本来就有换行+缩进，剥离标签后会留下大量
> "只含 ASCII 空格的假空行"，一页能白占好几行。用 `tools/validate_epub.py` 的排版预览能看到。

### 9.4 验证方式

```powershell
cd bookreader\tools
python validate_epub.py "D:\books\某本书.epub" --show 2
```

它会打印：ZIP 链路是否正常、spine 项数、提取耗时、**残留标签数（应为 0）**、
提取结果的抽样，以及按 9 列 × 11 行排出来的前几页预览。

### 9.5 插图显示

**流程**：解压 EPUB 时，正文里的 `<img>` / `<image xlink:href>` 引用会被替换成
`@<索引>@` 这样一整行标记，同时把对应图片解压到 `/cache/<书名>/img_NN.jpg`（或 `.png`）。
阅读翻到那一页时，界面检测到标记行就改成整页渲染图片。

**解压器**：

| 格式 | 库 | 说明 |
|---|---|---|
| JPEG | [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) 1.1.0 | 支持**解码阶段降采样** `setJpgScale(1/2/4/8)` |
| PNG | [PNGdec](https://github.com/bitbank2/PNGdec) 1.1.6 | 逐行回调，转 RGB565 后写入帧缓冲 |

**降采样是必须的**：书里的插画是 1442×2048 / 2048×1453，全尺寸解码成 RGB565 要 **8MB** 内存。
用 1/8 降采样后 2048 → 256 像素，正好接近屏宽，内存和速度都可接受。选择档位的规则是
"取第一个能让宽度 ≤ 240 的档位"，然后居中（letterbox）。

**为什么标记用 `@12@` 而不是 `\f`**：这是踩出来的坑。最初用 ASCII 0x0C(Form Feed) 做标记，
但文本整理阶段会把行首的 0x0C 当空白**去掉**，结果标记被拆成"只剩数字"和"空行"两行，
界面永远匹配不上，插图页一张都不会显示。换成可打印的 `@` 后，标签剥离和行整理都不会动它。

**校验方式**（`tools/validate_epub.py` 会检查）：

```
正文里插入的标记数 : 22
去重后的图片数     : 21
含标记的行数       : 22
格式正确的标记行   : 22
残留标签           : 0 个
```

要求是"整行必须正好是 `@数字@`"——不是的话说明有正文混进了标记行，分页器会把它们一起
当成插图页，必须修。

### 9.6 已知限制

- 加密 EPUB（DRM）不支持，会明确提示；
- 只支持 `store`(0) 和 `deflate`(8) 两种压缩方式，其他方式会报错而不是静默出错；
- 每本书首次打开需要解压一遍（你的 16MB 文件里正文只有 487KB，实测约 0.07 秒/电脑端，
  设备上因 SD 读慢会久一些，屏幕上有进度条）；
- 竖排 EPUB 的**排版顺序**不处理（只取文本流），你的文件元数据虽写竖排，正文仍是横排可读。
