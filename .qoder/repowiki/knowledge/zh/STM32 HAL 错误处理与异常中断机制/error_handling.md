本项目基于 STM32CubeMX 生成的嵌入式固件，采用 HAL 驱动层返回码 + 全局 Error_Handler 死循环 + Cortex-M 硬件异常向量表兜底的三层错误处理体系。

## 1. 系统/方法概述
- HAL 返回码检查：所有外设初始化与启动调用（如 HAL_ADCEx_MultiModeStart_DMA、HAL_RCC_OscConfig、HAL_ADC_ConfigChannel）均对返回值进行 != HAL_OK 判断，失败则跳转至 Error_Handler()
- 统一错误入口 Error_Handler()：位于 Core/Src/main.c，实现为关闭全局中断后进入无限空循环，使系统在不可恢复错误时冻结，便于调试定位
- Cortex-M 硬件异常向量：Core/Src/stm32g4xx_it.c 中 NMI/HardFault/MemManage/BusFault/UsageFault 等处理器异常 handler 均为空 while(1) 死循环，作为最后兜底
- USB CDC 层错误码：USB_Device/App/usbd_cdc_if.c 使用 USBD_OK / USBD_FAIL / USBD_BUSY 表示操作结果；应用侧在 Send_Signal_Over_UART() 中对 CDC_Transmit_FS 的 USBD_BUSY 做轮询重试
- 断言支持：通过 #ifdef USE_FULL_ASSERT 条件编译提供 assert_failed(file, line) 钩子，默认空实现，可替换为串口打印

## 2. 关键文件与位置
- Core/Src/main.c：Error_Handler() 定义、HAL 调用错误分支、assert_failed 钩子
- Core/Src/stm32g4xx_it.c：全部 Cortex-M 异常 ISR（HardFault 等）
- USB_Device/App/usbd_cdc_if.c：USB CDC 传输状态码与 USBD_BUSY 重试逻辑
- Drivers/STM32G4xx_HAL_Driver/Inc/stm32g4xx_hal_def.h：HAL 通用类型与 HAL_OK 等常量定义

## 3. 架构与约定
- 分层职责：应用层只关心 HAL_OK/USBD_OK 成功路径；失败一律下沉到 Error_Handler 或上层重试，不在业务函数内分散处理
- ISR 最小化：EXTI/DMA/USB 中断仅转发到 HAL 回调（HAL_GPIO_EXTI_IRQHandler、HAL_DMA_IRQHandler、HAL_PCD_IRQHandler），不直接执行业务逻辑，避免在中断上下文中出现复杂错误分支
- 无 panic/recover：Cortex-M 环境未使用 C++ 异常或语言级 recover 机制，错误以同步返回码 + 全局停机方式表达

## 4. 开发者应遵循的规则
1. 所有 HAL/LL 调用必须检查返回值，失败走 Error_Handler()，禁止吞掉错误码
2. 新增 ISR 仅做 HAL 转发，业务错误信号通过 volatile flag 通知主循环处理
3. USB CDC 发送前检查 TxState，遇到 USBD_BUSY 按现有模式轮询重试或上抛给调用方
4. 启用 USE_FULL_ASSERT 后重写 assert_failed，将断言信息输出到 USB CDC 以便运行时诊断
5. 不要在 Error_Handler 中执行阻塞 I/O；如需上报错误，应在进入死循环前完成一次性的非阻塞记录