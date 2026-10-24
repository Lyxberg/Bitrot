# Bitrot (prototype 0.3)

频谱域的码流损伤模拟器：不跑真 MP3 编码器，而是在 STFT 域做**分频带量化**
（多带 bit crusher）＋ 噪声替换 ＋ 丢包/重复，把压缩音频的听感做出来。

> 名字说明：之前叫 SpectralLossy，太直球。Bitrot（数据腐朽）是真实术语，
> 也正好是这件事的本质——数据慢慢烂掉。

- 目录：`D:\AudioDev\Bitrot`
- 产物：`dist\Bitrot.vst3`、`dist\Bitrot.exe`（Standalone）
- 工具链：`JUCE\` 和 `.tools\` 是指向
  `D:\AudioDev\RiderSystemKits\_toolchain\` 的 junction，别删 RiderSystemKits。

## 构建

```powershell
cd D:\AudioDev\Bitrot
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

## 离线自检

```powershell
.\build\BitrotTest_artefacts\Release\BitrotTest.exe
```

检查延迟对齐、Loss=0 透明、Mix=0 直通、五种 FFT 各自的脉冲对齐、丢包模式、
噪声替换、Mono 折叠、Joint 稳定性、极端参数是否 NaN。当前全过。

## 信号流

```
输入 ──► STFT (FFT = 128..2048 / hop = N/4 / sqrt-Hann，75% overlap)
          │
          ├─ 32 个 Bark 均分带 ─► 每带 峰值 peak + RMS
          │                      │
          │                      ├─ eff = Loss · (1.45 − 0.45·w(f))   ← 高频先烂
          │                      ├─ bits = 14·e^(−4·eff) + 0.8·activity·eff
          │                      └─ 对 bits 做一阶平滑（Loss Speed）
          │
          ├─ 带中心 > 带宽上限      ──► 整带清零（高频断崖）
          ├─ 否则：q = peak·2^(−bits)，m < q 归零，其余 round(m/q)·q
          │        相位原样保留
          ├─ Noise Fill：bits < 2 的带按 (2−bits)/2 比例换成同能量噪声
          │
          └─ ISTFT ─► 丢包/重复（帧级）─► 干湿混合（干路已按延迟对齐）
```

四个关键决定：

1. **量化基准是频带峰值（peak），不是 RMS。** 这是 MP3 的 scalefactor 逻辑。
   用 RMS 当基准时，一个强的分音几乎永远不会被量化到，听起来只是“加了一层很轻的噪声”。
2. **带宽上限。** 低码率编码器直接不编码某个频率以上的带：
   `bw = nyquist · (1 − 1.15·Loss)`（25%≈17 kHz，50%≈10 kHz，100%≈1.4 kHz）。
3. **Noise Fill（PNS 思路）。** 预算耗尽的带不再变成真空，而是换成同能量噪声，
   频谱上留下连续的沙沙底噪，这是“码流感”的关键。
4. **Joint Stereo。** L/R → M/S，S 通道共用 M 的比特分配，M 塌的时候 S 一起塌，
   声场才会像低码率 MP3 那样凹进去。

## 参数

| 参数 | 作用 |
|---|---|
| **Loss** | 比特预算。0% 透明，25% 左右≈很烂的真实 MP3，再往上就是“不存在的编码器” |
| **Loss Speed** | 比特分配跟随信号的速度。100% 最快、最碎；0% 几乎冻结、最 smear |
| **Fill** | 死掉的频带用同能量噪声顶替的比例。0% = 真空洞，100% = 连续沙沙 |
| **FFT** | 128 / 256 / 512 / 1024 / 2048。**频谱分辨率就是性格旋钮**，同时决定延迟 |
| **Mix** | 干湿比（干路延迟已对齐，不会梳状） |
| **Output** | 输出增益，±24 dB |
| **Mode** | Standard / Inverse / Packet Loss / Packet Repeat |
| **Stereo** | Stereo（左右独立）/ Joint（M/S 共用比特分配）/ Mono |
| **Weighting** | Perceptual（按听觉加权）/ Flat（所有频带一视同仁，更脏） |

### FFT 档位的听感差异

| FFT | 延迟 @48k | 频带宽度 | 性格 |
|---|---|---|---|
| 128 | 2.7 ms | 375 Hz | 电话/机器人，低频糊成块，瞬态保留 |
| 256 | 5.3 ms | 188 Hz | 粗颗粒，像很烂的语音编码 |
| 512 | 10.7 ms | 94 Hz | 折中，最像早期流媒体 |
| 1024 | 21.3 ms | 47 Hz | 细腻，smear 明显 |
| 2048 | 42.7 ms | 23 Hz | 最“频谱”，pre-echo 和拖尾最重 |

> 切 FFT 会改变上报给宿主的延迟（`setLatencySamples`），宿主会自动重新对齐。
> 建议当音色开关用，不要在播放中自动化它。

### 试听建议

| 想听到 | 设置 |
|---|---|
| KaZaA 时代的 128k MP3 | Loss 14%、Speed 100%、Fill 40%、Perceptual、FFT 2048 |
| 水管视频 / 手机串流 | Loss 30%、Flat、Fill 80%、FFT 512 |
| 水下大教堂 | Loss 65%、Speed 25%、Fill 60%、FFT 1024 |
| 唱玻璃杯 | Loss 50%、Speed 5%、Fill 20%、FFT 2048 |
| Ghost in the MP3 | Inverse、Loss 40%（已 +12 dB 补偿） |
| 坏连接 | Packet Loss、Loss 70% |

## 界面

粉红皮肤（内置绘制，不依赖图片）：`#F5007D` 底、`#FF6FA8` 频谱面板、
`#1A1A1A` 控制条、`#B3005C` 旋钮。标题用 `Segoe Script`。

皮肤系统仍然可用：往 `%APPDATA%\AudioDev\Skins\Bitrot\default\` 放 `bg.png`
就会覆盖背景。

## 已知限制

- 没有 auto gain（Fill 拉满时电平会掉一点，测试里约 −5 dB）。
- 没有 block switching（瞬态仍用长窗，鼓点会有点糊）。
- Phase Jitter 模式还没写。
- 没有 gate / dither（dither 故意不加：加了就变干净白噪声，味道没了）。

## 想继续调的声音旋钮（都在代码里）

- `PluginProcessor.cpp` → `buildTables()` 里的 `0.30 + 0.95 * exp(...)` 是感知加权曲线。
- `renderFrame()` 里的 `14·e^(−4·eff) + 0.8·activity·eff` 是比特分配公式。
- `processBlock()` 里 `p.smoothing = pow(speed, 3)` 是 Speed 的响应曲线。
- 带宽上限 `1.0f - 1.15f * p.loss` 决定高频被砍到什么程度。

## 分析工具

```powershell
# 频谱图 + 残差图 + 统计（可选：走一遍插件）
.\build\BitrotTest_artefacts\Release\BitrotTest.exe analyze <in.wav> <outPrefix> [loss speed mode weighting fft fill stereo]
```
