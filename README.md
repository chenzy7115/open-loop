# ESP32 FOC 开环驱动项目备忘

## 1. 项目环境

| 项 | 值 |
|---|---|
| 框架 | PlatformIO（编译命令：`pio run`， pio 不在 PATH 时用 `~\.platformio\penv\Scripts\pio.exe`） |
| 平台 | `platform espressif32 @ 8.0.0` |
| 开发板 | `lolin32_lite`（ESP32，240MHz，4MB Flash） |
| **Arduino Core 版本** | **3.3.12**（`esp_arduino_version.h` 中 `ESP_ARDUINO_VERSION_MAJOR = 3`） |
| 依赖库 | 无（`platformio.ini` 没有 `lib_deps`，未安装 SimpleFOC） |

## 2. 核心结论：LEDC API 版本差异（踩坑记录）

### 2.1 core 2.x 与 core 3.x 的 LEDC API 对照表

**core 3.x（当前环境）中 `ledcSetup` / `ledcAttachPin` 已被整体删除**，
调用它们必然报 `'xxx' was not declared in this scope`。对照关系：

| 功能 | 旧写法（core 2.x，已不可用） | 新写法（core 3.x，本项目使用） |
|---|---|---|
| 初始化 PWM | `ledcSetup(通道, freq, res)` + `ledcAttachPin(引脚, 通道)` 两步 | `ledcAttach(引脚, freq, res)` 一步，**通道自动分配** |
| 指定通道初始化 | — | `ledcAttachChannel(引脚, freq, res, 通道)` |
| 设置占空比 | `ledcWrite(通道, duty)` 按**通道** | `ledcWrite(引脚, duty)` 按**引脚** |
| 读取占空比 | `ledcRead(通道)` | `ledcRead(引脚)` |
| 解除绑定 | `ledcDetachPin(引脚)` | `ledcDetach(引脚)` |

其余 3.x 可用接口：`ledcWriteTone`、`ledcWriteNote`、`ledcChangeFrequency`、
`ledcOutputInvert`、`ledcFade` 系列等。

### 2.2 本项目 setup() 中的 PWM 初始化写法

三相 PWM 引脚：U = GPIO32，V = GPIO33，W = GPIO25；频率 30 kHz，分辨率 8 bit（0~255）。

```cpp
pinMode(pwmA, OUTPUT);
pinMode(pwmB, OUTPUT);
pinMode(pwmC, OUTPUT);
ledcAttach(pwmA, 30000, 8); // pwm引脚, 30kHz, 8bit
ledcAttach(pwmB, 30000, 8);
ledcAttach(pwmC, 30000, 8);
```

注意：`pinMode(..., OUTPUT)` 必须在 `ledcAttach` **之前**调用（attach 之后再 pinMode 会破坏 PWM 输出）。

### 2.3 loop() 中输出 SVPWM 占空比的写法

`dc_a / dc_b / dc_c` 为 0~1 的浮点占空比，写入前限幅并换算到 8 bit 量程：

```cpp
ledcWrite(pwmA, (uint32_t)(_constrain(dc_a, 0, 1.0) * 255));
ledcWrite(pwmB, (uint32_t)(_constrain(dc_b, 0, 1.0) * 255));
ledcWrite(pwmC, (uint32_t)(_constrain(dc_c, 0, 1.0) * 255));
```

（教程中的旧写法 `ledcWrite(0, dc_a * 255)` 按通道号写，在 3.x 下必须换成按引脚写。）

## 3. 电机不转的原因（与灯哥教程代码对比结论）

将灯哥开源的教程代码与本项目能正常运转的代码逐行对比，功能上只差**驱动板使能脚 EN** 一处；
另外教程代码如果被改回按“通道号”写 `ledcWrite`，也是常见的静默失效点。两者都**不报任何错误**。

### 3.1 差异一：驱动板 EN（使能 / nSLEEP）未拉高

教程代码的 `setup()` 里没有下面两行（本项目的代码里有）：

```cpp
pinMode(12, OUTPUT);     // 驱动板 EN / DRV8313 的 nSLEEP
digitalWrite(12, HIGH);  // 高电平使能功率级
```

机制：

- DRV8313 的 `nSLEEP`、L6234 / SimpleFOCShield 的 `EN`，低电平或悬空即功率级关断（高阻）；
- 此时 MCU 的 GPIO32/33/25 上 PWM 波形**完全正常**（示波器/万用表都看得到），
  但信号进不了功率级，线圈**无电流 → 无转矩**；
- 仅 `pinMode(12, OUTPUT)` 不够：GPIO 输出锁存默认值为 0，必须再 `digitalWrite(12, HIGH)`。

为什么教程原版能跑而照抄不行：灯哥配套驱动板通常已在硬件上把 `nSLEEP` 上拉/短接到高电平，
所以使能行可有可无；若自己的板子把 EN 留给软件控制，就必须在 `setup()` 里主动拉高。
EN 若为低有效，则改为 `digitalWrite(12, LOW)`。

> 注意：GPIO12 是 ESP32 启动 strapping 脚（MTDI，影响 flash 电压），代码在启动后才拉高没问题，
> 但若驱动板在 EN 上加了到 3.3V 的上拉电阻，可能造成上电启动异常，排查时可留意。

### 3.2 差异二：`ledcWrite` 写成按“通道号”

教程原版是 `ledcWrite(0, dc_a * 255)`（core 2.x 按通道写）。在 core 3.x 下第一参数是**引脚号**，
照抄会把占空比“写给” GPIO0/1/2：

```c
ledc_channel_handle_t *bus = perimanGetPinBus(pin, ESP32_BUS_TYPE_LEDC);
if (bus != NULL) { /* 真正写占空比 */ }
return false;   // 引脚没 attach 到 LEDC 就静默返回，不报错、不打印
```

结果：GPIO0/1/2 未挂 LEDC → **静默返回 false**；真正 attach 过的 32/33/25 占空比恒为 0%，
三相全部 0% → 三个端子被低边短接 → 无相电压差 → 无电流 → 电机纹丝不动。

### 3.3 现象对照诊断表

| 现象 | 最可能原因 |
|---|---|
| 完全无反应、不抖不响；但 MCU 引脚上有正常 PWM | 驱动板 EN / nSLEEP 未使能（第 3.1 节） |
| MCU 三个 PWM 引脚直流电压恒为 0 V | `ledcWrite` 目标写错（第 3.2 节） |
| 原地抖动、嗡响、断续跳步，不连续转 | 极对数（代码写死 7）与电机不符，或相序接错 |
| 有保持扭矩但就是不转 | `loop()` 没调用 `velocityOpenloop`，或角度未累加 |
| 运行十几秒后开始轻微抖动 | `open_loop_timestamp` 用 `float` 存微秒，精度不足（建议改 `uint32_t`） |

### 3.4 静态锁定测试（判断功率级是否真的工作）

把 `loop()` 临时改成固定电角度输出：

```cpp
void loop() {
  setPhaseVoltage(voltage_power_supply / 2, 0, 0); // 固定电角度 0，Uq 取最大
  delay(10);
}
```

- 电机被磁力锁住、手拧有保持扭矩 → 功率级、接线、使能都正常，问题在“旋转”环节；
- 完全无反应、线圈无电流 → 使能 / 电源 / 接线问题。

## 4. 备选方案：降级到 core 2.x

如果希望原封不动运行 2023 年以前的教程代码（`ledcSetup` + `ledcAttachPin`），
可在 `platformio.ini` 中把平台钉到旧版：

```ini
platform = espressif32@6.9.0   ; 对应 Arduino-ESP32 core 2.x
```

本项目目前**未采用**该方案，推荐留在新版本上按第 2 节改写代码。

## 5. 排查方法备忘

- 查看当前 core 版本：`pio pkg list`（`framework-arduinoespressif32 @ 4.30312.x` 即 core 3.3.x），
  或看代码运行打印的 `ESP_ARDUINO_VERSION_MAJOR`。
- 确认某函数是否存在于本机 core：在
  `%USERPROFILE%\.platformio\packages\framework-arduinoespressif32\cores\esp32\`
  下搜索函数名（LEDC 头文件为 `esp32-hal-ledc.h`）。
- 编译器报错中紧随 `ledcSetup` 等旧函数出现的 `ledcAttach` 提示，即官方的新 API 替代。
