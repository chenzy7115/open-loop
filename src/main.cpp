// 有感 FOC 位置闭环示例：所有 FOC 底层细节都在 lib/DengFOC 里，这里只写应用逻辑
// 接口命名与灯哥 DengFOC 库 v0.1 一致：DFOC_Vbus / DFOC_alignSensor / setTorque /
// _electricalAngle / DFOC_M0_Angle / serialReceiveUserCommand / serial_motor_target

#include <Arduino.h>
#include "DengFOC.h"

int Sensor_DIR = 1; // 编码器方向系数：本项目实测 +1；灯哥教程板为 -1，不能照抄
int Motor_PP = 7;   // 电机极对数：2804 万向节电机是 7，2208 常见是 11

void setup()
{
  Serial.begin(115200);

  pinMode(12, OUTPUT);    // 驱动板使能脚
  digitalWrite(12, HIGH); // pinMode 后默认低电平，不拉高功率级一直关断

  DFOC_Vbus(12.6);                        // 母线电压 + 三相 PWM + AS5600 初始化
  DFOC_alignSensor(Motor_PP, Sensor_DIR); // 零位标定，约 3 秒

  // 闭环平衡点是 θ = DIR × target，所以乘 DIR 才能锁在上电时的位置，
  // 而不是被拉到 0 rad（否则上电瞬间电机会突然转一大段）
  DFOC_setTarget(Sensor_DIR * DFOC_M0_Angle());
  Serial.print("已锁定当前位置：");
  Serial.println(serial_motor_target());
  Serial.println("串口发送目标角度（单位 rad，回车结束）即可让它转到该位置");
}

void loop()
{
  float Kp = 0.133;
  float Sensor_Angle = DFOC_M0_Angle();    // 位置误差和换相角用同一个角度
  float target = serial_motor_target();

  // 位置闭环：误差(换算成度) × Kp → Uq，setTorque 内部会把 Uq 限幅到 ±Vbus/2
  setTorque(Kp * (target - Sensor_DIR * Sensor_Angle) * 180 / PI, _electricalAngle());

  // 到位后只打印一次：记住上一轮是否已在位，静止时不再刷屏
  static bool last_in_position = false;
  bool in_position = fabs(target - Sensor_DIR * Sensor_Angle) < 0.05f; // 误差 < 0.05 rad（约 2.9°）算到位
  if (in_position && !last_in_position)
  {
    Serial.print("已到位，当前角: ");
    Serial.println(Sensor_Angle);
  }
  last_in_position = in_position;

  serialReceiveUserCommand(); // 解析串口新目标
}