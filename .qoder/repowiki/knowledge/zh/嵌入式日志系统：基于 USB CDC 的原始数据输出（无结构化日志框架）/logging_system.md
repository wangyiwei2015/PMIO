本仓库未实现通用的结构化日志框架。应用层仅通过自定义函数将 ADC 采样数据以纯文本形式经 USB CDC 虚拟串口输出，错误处理采用 `Error_Handler` 死循环，断言宏仅留注释占位，不存在日志级别、格式化模板或统一 sink 抽象。

1. 使用的系统与工具链
- 标准 I/O：包含 `<stdio.h>`，但项目未重定向 `printf/scanf`；`Core/Src/syscalls.c` 中仅保留 Picolibc 的标准 I/O 指针注释，实际未实现 `_write/_read` 钩子。
- HAL UART/USART：`Core/Inc/stm32g4xx_hal_conf.h` 中 `HAL_UART_MODULE_ENABLED` 与 `HAL_USART_MODULE_ENABLED` 均被注释关闭，未启用任何硬件 UART。
- USB FS CDC：通过 STM32CubeMX 生成的 `USB_Device/App/usbd_cdc_if.c` 暴露 `CDC_Transmit_FS`，作为唯一的数据出口。

2. 关键文件与位置
- `Core/Src/main.c`：定义 `Send_Signal_Over_UART()`，把 240 个 16bit 采样值逐行格式化为十进制字符串并通过 `CDC_Transmit_FS` 发送；`Error_Handler` 为全局错误入口（关中断 + 无限循环）；`assert_failed` 仅含示例注释，未实现。
- `USB_Device/App/usbd_cdc_if.c`：封装 `CDC_Transmit_FS`，内部调用 `USBD_CDC_TransmitPacket`，返回 `USBD_BUSY` 时由上层轮询重试。
- `Core/Src/syscalls.c`：Picolibc 标准 I/O 重定向桩，当前为空实现，`printf` 不可用。
- `Core/Inc/stm32g4xx_hal_conf.h`：UART/USART HAL 模块全部禁用。

3. 架构与约定
- 无日志等级：没有 DEBUG/INFO/WARN/ERROR 等分级，所有“日志”都是业务数据流。
- 无结构化字段：输出为每行一个整数的纯文本，无时间戳、源文件名/行号、任务 ID 等元数据。
- 单 Sink 模式：唯一输出通道是 USB CDC IN endpoint，通过阻塞式 `while(CDC_Transmit_FS(...) != USBD_OK)` 等待端点缓冲区空闲。
- 错误路径：`Error_Handler` 直接 `__disable_irq()` 后进入死循环，不产生任何可观测输出。

4. 开发者应遵循的规则
- 如需调试信息，可在 `main.c` 中复用 `Send_Signal_Over_UART` 的缓冲+逐字节拼接方式，再调用 `CDC_Transmit_FS` 发送；注意在 ISR 中避免阻塞式 CDC 发送。
- 不要依赖 `printf`：`syscalls.c` 未实现 `_write`，调用会挂起或崩溃。
- 若需引入正式日志框架，应在 `Core/Inc/` 下新增 `log.h` 抽象层，提供多级别宏与可插拔 sink（如同时写 USB CDC 与 SWO），并在 `stm32g4xx_hal_conf.h` 中按需开启 UART 作为备用 sink。
- 当前设计假设上位机以固定波特率读取每行整数，新增日志时应保持向后兼容，例如在数据帧前后插入分隔标记。