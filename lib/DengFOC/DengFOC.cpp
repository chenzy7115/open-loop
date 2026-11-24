//灯哥开源，遵循GNU协议，转载请著名版权！
//GNU开源协议（GNU General Public License, GPL）是一种自由软件许可协议，保障用户能够自由地使用、研究、分享和修改软件。
//该协议的主要特点是，要求任何修改或衍生的作品必须以相同的方式公开发布，即必须开源。此外，该协议也要求在使用或分发软件时，必须保留版权信息和许可协议。GNU开源协议是自由软件基金会（FSF）制定和维护的一种协议，常用于GNU计划的软件和其他自由软件中。
//仅在DengFOC官方硬件上测试过，欢迎硬件购买/支持作者，淘宝搜索店铺：灯哥开源
//你的支持将是接下来做视频和持续开源的经费，灯哥在这里先谢谢大家了

#include <Arduino.h>
#include "DengFOC.h"
#include "AS5600.h"

// 限幅宏：把 amt 约束在 [low, high]（Arduino 自带 constrain，这里加下划线区分）
#define _constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))

// 3π/2（=270°）电角度：零位标定时把电压矢量固定到这个角度，
// 电机先被磁力“吸”到一个已知位置，再用编码器读数反推出 zero_electric_angle
#define _3PI_2 4.71238898038f

// ---------------- 库内部状态 ----------------
float voltage_power_supply = 12.6; // 母线电压 V，由 DFOC_Vbus() 设置
float zero_electric_angle = 0;     // 编码器零位与电机磁极零位的偏移，由 DFOC_alignSensor() 标定
int PP = 7;                        // 极对数，由 DFOC_alignSensor() 设置
int DIR = 1;                       // 方向系数 ±1，由 DFOC_alignSensor() 设置
float Ualpha = 0, Ubeta = 0;       // α-β 轴电压
float Ua = 0, Ub = 0, Uc = 0;      // 三相电压
float motor_target = 0;            // 目标机械角（rad，累积多圈）
int commaPosition = 0;             // 串口命令里换行符的位置

// PWM 引脚（U / V / W）
int pwmA = 32;
int pwmB = 33;
int pwmC = 25;

// 归一化角度到 [0,2PI]
float _normalizeAngle(float angle)
{
  float a = fmod(angle, 2 * PI); // 取余运算可以用于归一化
  return a >= 0 ? a : (a + 2 * PI);
  // fmod 的余数符号与除数相同：angle 为负时余数为负，加上 2π 即可落到 [0, 2π)
}

// 设置 PWM 到控制器输出
void setPwm(float Ua, float Ub, float Uc)
{
  // 计算占空比，限制在 0~1
  float dc_a = _constrain(Ua / voltage_power_supply, 0.0f, 1.0f);
  float dc_b = _constrain(Ub / voltage_power_supply, 0.0f, 1.0f);
  float dc_c = _constrain(Uc / voltage_power_supply, 0.0f, 1.0f);

  // core 3.x 按【引脚】写占空比（旧版才是按通道号 0/1/2 写，写错不报错但无任何输出）
  ledcWrite(pwmA, dc_a * 255);
  ledcWrite(pwmB, dc_b * 255);
  ledcWrite(pwmC, dc_c * 255);
}

// 按电角度 angle_el 施加 Uq 电压矢量：帕克逆变换 + 克拉克逆变换
// 注意：angle_el 必须是已经做过零位补偿的电角度（即 _electricalAngle() 的返回值），
// 这里绝不能再加一次 zero_electric_angle，否则零位补偿被抵消、矢量与磁极错开，
// 力矩方向可能整体反转，表现为电机不受控地一直转、停不到目标位置
void setTorque(float Uq, float angle_el)
{
  Uq = _constrain(Uq, -voltage_power_supply / 2, voltage_power_supply / 2);
  angle_el = _normalizeAngle(angle_el);

  // 帕克逆变换
  Ualpha = -Uq * sin(angle_el);
  Ubeta = Uq * cos(angle_el);

  // 克拉克逆变换
  Ua = Ualpha + voltage_power_supply / 2;
  Ub = (sqrt(3) * Ubeta - Ualpha) / 2 + voltage_power_supply / 2;
  Uc = (-Ualpha - sqrt(3) * Ubeta) / 2 + voltage_power_supply / 2;
  setPwm(Ua, Ub, Uc);
}

// 设置母线电压，并完成三相 PWM 与编码器的初始化
void DFOC_Vbus(float power_supply)
{
  voltage_power_supply = power_supply;

  pinMode(pwmA, OUTPUT);
  pinMode(pwmB, OUTPUT);
  pinMode(pwmC, OUTPUT);

  // core 3.x：ledcAttach 一步完成频率/精度设置与引脚绑定，取代旧版 ledcSetup + ledcAttachPin
  ledcAttach(pwmA, 30000, 8); // pwm引脚, 30kHz, 8bit
  ledcAttach(pwmB, 30000, 8);
  ledcAttach(pwmC, 30000, 8);

  Serial.println("完成PWM初始化设置");
  BeginSensor();
}

// 有感闭环专用的电角度：直接读编码器当前机械角，换算出“当前电角度”（0~2π）
// 电角度 = 归一化( DIR·PP · 机械角 − 零位偏移 )
//   用 getAngle_Without_track()（0~2π 单圈）即可，电角度本身只需单圈范围
float _electricalAngle()
{
  return _normalizeAngle((float)(DIR * PP) * getAngle_Without_track() - zero_electric_angle);
}

// 零位标定：先把电压矢量固定到 _3PI_2，让转子被磁力吸到已知位置，
// 再读编码器反推出 zero_electric_angle，标定完撤掉电压
void DFOC_alignSensor(int _PP, int _DIR)
{
  PP = _PP;
  DIR = _DIR;
  setTorque(3, _3PI_2);
  delay(3000);
  zero_electric_angle = _electricalAngle();
  setTorque(0, _3PI_2);
  Serial.print("0电角度：");
  Serial.println(zero_electric_angle);
}

// 当前机械角（累积多圈，rad）
float DFOC_M0_Angle()
{
  return getAngle();
}

//==============串口接收==============
String serialReceiveUserCommand()
{
  // a string to hold incoming data
  static String received_chars;

  String command = "";

  while (Serial.available())
  {
    // get the new byte:
    char inChar = (char)Serial.read();
    // add it to the string buffer:
    received_chars += inChar;

    // end of user input
    if (inChar == '\n')
    {
      // execute the user command
      command = received_chars;

      commaPosition = command.indexOf('\n'); // 检测字符串中的换行符
      if (commaPosition != -1)               // 找到换行符就向下执行
      {
        motor_target = command.substring(0, commaPosition).toDouble(); // 电机角度
        Serial.print("目标已设置: ");
        Serial.println(motor_target);
      }
      // reset the command buffer
      received_chars = "";
    }
  }
  return command;
}

// 直接设置目标机械角（rad），例如上电时锁住当前位置
void DFOC_setTarget(float target)
{
  motor_target = target;
}

// 读取当前目标机械角
float serial_motor_target()
{
  return motor_target;
}