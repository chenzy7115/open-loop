# ESP32 FOC 开环驱动项目备忘

## 1. 项目环境

| 项 | 值 |
|---|---|
| 框架 | PlatformIO（编译命令：`pio run`， pio 不在 PATH 时用 `~\.platformio\penv\Scripts\pio.exe`） |
| 平台 | `platform espressif32 @ 8.0.0` |
| 开发板 | `lolin32_lite`（ESP32，240MHz，4MB Flash） |
| **Arduino Core 版本** | **3.3.12**（`esp_arduino_version.h` 中 `ESP_ARDUINO_VERSION_MAJOR = 3`） |
| 依赖库 | 无外部 `lib_deps`；本地库见第 6 节：`lib/AS5600`（编码器）、`lib/DengFOC`（FOC 封装） |

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

## 6. 项目结构与 DengFOC 库封装

### 6.1 目录结构

```
open-loop/
├── lib/
│   ├── AS5600/          编码器驱动（灯哥教程原版：BeginSensor / getAngle / getAngle_Without_track）
│   └── DengFOC/         本项目对 FOC 的封装
│       ├── DengFOC.h    函数声明（接口命名与灯哥 DengFOC 库 v0.1 一致）
│       └── DengFOC.cpp  全部 FOC 实现
├── src/
│   └── main.cpp         只剩应用逻辑：初始化序列 + 位置闭环 + 串口解析
├── platformio.ini
└── README.md
```

封装目标：把 PWM 输出、帕克/克拉克逆变换、零位标定、机械角↔电角度换算这些底层细节从 `main.cpp` 抽出来，接口名与灯哥 DengFOC 库 v0.1 保持一致，方便对照教程和后续复用。

| 原 `main.cpp` 中的内容 | 现在的位置 | 对外名字 |
|---|---|---|
| PWM 引脚、`voltage_power_supply`、`PP`、`DIR`、`zero_electric_angle` | `DengFOC.cpp` 内部状态 | 同名全局变量（`DengFOC.h` 未导出） |
| `#define _constrain`、`#define _3PI_2` | `DengFOC.cpp` | 同名宏 |
| PWM 初始化 + 母线电压设置 | 库 | `DFOC_Vbus(float)` |
| 3 秒零位标定 | 库 | `DFOC_alignSensor(_PP, _DIR)` |
| `setPhaseVoltage(Uq, Ud, angle_el)` | 库 | `setTorque(Uq, angle_el)` |
| `setPwm` / `_normalizeAngle` / `_electricalAngle()` | 库 | 同名 |
| `getAngle()` 调用点 | 库 | `DFOC_M0_Angle()` |
| `serialReceiveUserCommand` / `motor_target` | 库 | `serialReceiveUserCommand` / `serial_motor_target` |
| —（新增） | 库 | `DFOC_setTarget(float)`：代码里直接设目标角 |

### 6.2 库 API 一览

| 函数 | 作用 |
|---|---|
| `void DFOC_Vbus(float power_supply)` | 设置母线电压，初始化 GPIO32/33/25 的 30kHz/8bit PWM，并调用 `BeginSensor()` 初始化 I2C 编码器 |
| `void DFOC_alignSensor(int _PP, int _DIR)` | 零位标定：记录极对数与方向系数 → 施加 3V 矢量到 `_3PI_2` → 停 3 秒 → 读回 `zero_electric_angle` → 撤电压 |
| `void setTorque(float Uq, float angle_el)` | 按电角度施加电压矢量：`Uq` 内部限幅 ±Vbus/2，再做帕克逆变换 + 克拉克逆变换 → `setPwm` |
| `void setPwm(float Ua, float Ub, float Uc)` | 三相电压 → 0~1 占空比 → `ledcWrite(引脚, 0~255)` |
| `float _electricalAngle()` | 当前电角度（0~2π）= `归一化(DIR·PP·机械角 − zero_electric_angle)` |
| `float _normalizeAngle(float angle)` | 任意角度归一化到 [0, 2π) |
| `float DFOC_M0_Angle()` | 当前机械角 `getAngle()`，单位 rad，**累积多圈** |
| `void DFOC_setTarget(float target)` | 直接设置目标机械角（rad） |
| `float serial_motor_target()` | 读取当前目标机械角 |
| `String serialReceiveUserCommand()` | 解析串口：一条以换行结束的数值即目标角，收到后打印 `目标已设置: xxx` |

> 关键约定：`setTorque()` 的 `angle_el` 必须是**已做零位补偿**的 `_electricalAngle()` 返回值，库里**不会**再加 `zero_electric_angle`。写成“两处都补偿”会互相抵消，力矩方向随机反转，现象是电机一直转、停不住（详见第 8 节）。

### 6.3 与灯哥 DengFOC 库 v0.1 的 4 处差异

| # | 参考库写法 | 本项目写法 | 原因 |
|---|---|---|---|
| 1 | `ledcSetup(通道,…)` + `ledcAttachPin(引脚,通道)`，`ledcWrite(通道, duty)` | `ledcAttach(引脚, 30000, 8)` + `ledcWrite(引脚, duty)` | core 3.3.12 已删除旧 API，旧写法编译失败或静默无输出（第 2 节） |
| 2 | `setTorque` 内 `angle_el = _normalizeAngle(angle_el)` | 同上 | 参考库本身正确，项目早期版本曾多加一次 `zero_electric_angle`，已固化为正确写法 |
| 3 | 只有全局 `motor_target`，只能靠串口改 | 新增 `DFOC_setTarget()` | 需要在标定后立刻锁住上电位置 |
| 4 | 例程写 `Sensor_DIR = -1` | `Sensor_DIR = 1` | 方向系数取决于编码器磁铁安装与相序，必须实测，不能照抄 |

## 7. 库的使用方法

### 7.1 最小可用程序（`src/main.cpp` 实际内容）

```cpp
#include <Arduino.h>
#include "DengFOC.h"

int Sensor_DIR = 1; // 方向系数：本项目实测 +1
int Motor_PP = 7;   // 极对数：2804=7，2208 常见 11

void setup()
{
  Serial.begin(115200);

  pinMode(12, OUTPUT);    // 驱动板使能脚（库不负责，必须在应用层拉高）
  digitalWrite(12, HIGH);

  DFOC_Vbus(12.6);                        // 母线电压 + PWM + 编码器
  DFOC_alignSensor(Motor_PP, Sensor_DIR); // 零位标定，约 3 秒

  DFOC_setTarget(Sensor_DIR * DFOC_M0_Angle()); // 上电锁住当前位置
}

void loop()
{
  float Kp = 0.133;
  float Sensor_Angle = DFOC_M0_Angle();
  float target = serial_motor_target();

  // 位置闭环：误差(度) × Kp → Uq；setTorque 内部再限幅到 ±Vbus/2
  setTorque(Kp * (target - Sensor_DIR * Sensor_Angle) * 180 / PI, _electricalAngle());

  serialReceiveUserCommand(); // 解析串口新目标
}
```

### 7.2 初始化顺序（不能乱）

1. `Serial.begin(115200)` —— 库里的 `DFOC_Vbus()` / `DFOC_alignSensor()` 都会打印信息；
2. **使能脚拉高** —— 与灯哥库一致，库不管 EN，必须在 `setup()` 里 `pinMode(12, OUTPUT)` + `digitalWrite(12, HIGH)`，否则功率级关断、电机不动；
3. `DFOC_Vbus(V)` —— 必须先设好母线电压，后面的占空比换算都依赖它；
4. `DFOC_alignSensor(PP, DIR)` —— 会占住 3 秒做对齐，必须在 PWM 初始化之后；
5. `DFOC_setTarget(...)` —— 标定完再给目标，否则上电瞬间会被拉到 0 rad。

### 7.3 位置闭环怎么写

```cpp
setTorque(Kp * (目标角 - DIR * 当前角) * 180 / PI, _electricalAngle());
```

- 误差乘以 `180/PI` 只是把弧度换成“度”，让 `Kp` 落在 0.1 量级；
- `setTorque(Uq, _electricalAngle())` 中 `Uq` 只决定力矩大小，换相角永远来自编码器；
- 平衡点：`当前角 = DIR × 目标角`，所以想锁在上电位置要 `DFOC_setTarget(DIR * DFOC_M0_Angle())`。

### 7.4 串口操作

波特率 115200，发送一个数值 + 回车即可（单位 rad，对应累积多圈角度）：

```
12.5<回车>
```

- 电机转到 `DIR × 12.5` 的位置并锁住；
- 打印节奏：上电/每次设目标/每次到位各打印一次，`loop()` 里不刷屏（到位判定阈值见 7.5）。

### 7.5 常用可调参数

| 参数 | 位置 | 默认值 | 说明 |
|---|---|---|---|
| `Sensor_DIR` | `src/main.cpp` | `1` | 力矩方向；闭环变正反馈时改成 `-1` |
| `Motor_PP` | `src/main.cpp` | `7` | 极对数，填错会抖动/换相错位 |
| `Kp` | `src/main.cpp` loop | `0.133` | 位置刚度；超调就降，软弱就升 |
| 到位阈值 `0.05f` | `src/main.cpp` loop | `0.05` rad（≈2.9°） | 只影响“已到位”那行打印，不影响控制 |
| 母线电压 | `DFOC_Vbus(12.6)` 实参 | `12.6` V | 与实测电源电压一致，影响占空比换算 |
| 力矩上限 | `DengFOC.cpp` 中 `setTorque` | ±Vbus/2（±6.3 V） | 想限制最大力矩就改这里的限幅 |
| PWM 引脚 / 频率 | `DengFOC.cpp` 中 `pwmA/B/C`、`ledcAttach` | 32/33/25、30kHz、8bit | 换板子改这里 |
| I2C 引脚 | `lib/AS5600/AS5600.cpp` 的 `Wire.begin(19,18,400000)` | GPIO19=SDA、18=SCL | 与驱动板接线一致 |

### 7.6 上电串口输出示例

```
完成PWM初始化设置
0电角度：3.42
已锁定当前位置：-1.57
串口发送目标角度（单位 rad，回车结束）即可让它转到该位置
已到位，当前角: 1.57
目标已设置: 3.14
已到位，当前角: 3.14
```

### 7.7 症状 → 排查

| 症状 | 原因 | 处理 |
|---|---|---|
| 完全不转、引脚上有 PWM | 使能脚没拉高 | 检查 `digitalWrite(12, HIGH)` |
| 一直高速转、停不住 | 力矩方向反了（正反馈） | 把 `Sensor_DIR` 取反 |
| 原地抖动、`Uq` 正负跳、角度不收敛 | 极对数不对 | 核对电机 `PP`（2208 常用 11） |
| 能锁住但围绕目标来回晃 | `Kp` 偏大、电压饱和 | 先降到 0.03~0.05 |
| 上电瞬间猛地转一大段 | 没在标定后锁住当前位置 | 调用 `DFOC_setTarget(DIR * DFOC_M0_Angle())` |
| 一直不打印“已到位” | 静摩擦导致稳态误差大于阈值 | 把 `0.05f` 放宽到 `0.1f` |

## 8. 位置闭环“一直转”的坑（已修复，勿回退）

早期版本 `main.cpp` 里 `_electricalAngle()` 减了 `zero_electric_angle`，`setPhaseVoltage()` 里又加了一次，两次补偿互相抵消，等价于没做零位标定：

```
理想：电压矢量超前转子磁极 90°
实际：超前 90° + zero_electric_angle
力矩 ∝ Uq · cos(zero_electric_angle)
```

`zero_electric_angle` 取决于磁铁粘在轴上的角度，是 0~2π 的随机值：落在 (-90°, 90°) 时力矩方向没变（只是被削弱），教程硬件恰好属于这种情况；落在 (90°, 270°) 时**力矩整体反号**，位置负反馈变成正反馈，`Uq` 一直饱和，电机恒定高速旋转。

现在 `zero_electric_angle` **只在 `_electricalAngle()` 里减一次**，`setTorque()` 里只做 `_normalizeAngle()`。串口打印的 `0电角度` 若落在 1.57~4.71 rad，就是这类问题的特征数据。
