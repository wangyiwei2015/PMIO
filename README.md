# PMIO — STM32G474 超声信号采集固件
PMUT AFE

基于 STM32G474 的裸机超声信号采集固件，用于验证双 ADC 交错模式下的高速信号触发捕获与数据上报。

## 功能
- **双 ADC 交错采样**：ADC1 + ADC2 以 8 MSPS 有效速率采样 kHz~MHz 超声信号
- **硬件触发采集**：PA4 GPIO EXTI 上升沿触发，精确捕获触发时刻
- **零拷贝环形缓冲**：120 元素 DMA 循环缓冲区（240 样本），预触发 10 µs + 后触发 20 µs
- **USB CDC 数据上报**：采集完成后通过 USB 虚拟串口发送 ADC 原始值（每样本一行十进制数）
- **超声回波抑制**：传输期间丢弃所有 EXTI 边沿，仅捕获首次到达信号
- **PC13 LED 指示**：开漏输出，active low
- 下一步实现：硬件集成THS4551、OPA657、电源、发射模块MOS、收发切换保护

## Contributors
- STM32CubeMX配置工程师：Gemini (Gemini 3.5 Flash)
- STM32嵌入式C++工程师：Qoder (Qwen3.7-Max)

## 硬件要求

| 项目 | 规格 |
|------|------|
| MCU | STM32G474 (Cortex-M4F) |
| ADC 输入 | PA2/PA3 (ADC1 IN3 差分), PA6/PA7 (ADC2 IN3 差分) |
| 触发输入 | PA4 (EXTI4, 上升沿) |
| LED | PC13 (开漏, active low) |
| USB | USB FS (CDC 虚拟串口) |

## 构建

本项目使用 STM32CubeIDE 2.1.1 内置工具链

> 工具链位于 STM32CubeIDE 安装目录的插件子目录中，默认未加入系统 PATH。

```powershell
# 配置
cmake --preset Debug

# 编译
cmake --build --preset Debug
```

输出文件位于 `build/Debug/`：

| 文件 | 用途 |
|------|------|
| `G4test.elf` | 调试（含符号表） |
| `G4test.hex` | 烧录 (Intel HEX) |
| `G4test.bin` | 烧录 (纯二进制) |
| `G4test.map` | 内存映射分析 |

## 代码结构

```
Core/
├── Src/
│   ├── main.c                 # 用户逻辑（触发、解包、发送）
│   ├── stm32g4xx_hal_msp.c    # CubeMX 生成的外设底层初始化
│   ├── stm32g4xx_it.c         # 中断入口（EXTI4, DMA1_Channel1）
│   └── system_stm32g4xx.c     # 系统启动配置
├── Inc/
│   ├── main.h
│   ├── stm32g4xx_hal_conf.h
│   └── stm32g4xx_it.h
USB_Device/                    # CubeMX 生成的 USB CDC 设备栈
Middlewares/                   # ST USB Device Library
Drivers/                       # STM32G4xx HAL/LL 驱动
cmake/                         # CMake 工具链与构建配置
```

## CubeMX 注意事项

- 所有用户代码必须写在 `/* USER CODE BEGIN ... */` 与 `/* USER CODE END ... */` 之间
- CubeMX 重新生成代码后，`while(1)` 的闭合花括号可能丢失，需手动修复
