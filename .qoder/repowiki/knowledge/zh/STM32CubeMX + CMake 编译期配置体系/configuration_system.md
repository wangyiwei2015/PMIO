本项目不存在运行时配置文件加载机制，所有“配置”均通过**编译期宏定义、头文件常量与 CMake 构建变量**完成，形成三层静态配置体系：

### 1. HAL/外设层配置（`Core/Inc/stm32g4xx_hal_conf.h`）
- 通过 `HAL_<MODULE>_MODULE_ENABLED` 宏开关选择启用的 HAL 模块（ADC、DMA、EXTI、PCD、GPIO、RCC、FLASH、PWR、CORTEX 等），未使用的模块直接注释掉以裁剪代码体积。
- 使用 `USE_HAL_<MODULE>_REGISTER_CALLBACKS=0U` 禁用回调注册功能，固定使用默认回调入口，减少 RAM 占用。
- 振荡器频率（HSE_VALUE、HSI_VALUE、HSI48_VALUE、LSE_VALUE）、系统电压（VDD_VALUE）、Tick 优先级、Cache/Prefetch 等通过 `#define` 集中声明，供 RCC/Flash HAL 在初始化时读取。
- 该文件由 STM32CubeMX 生成，修改后需重新生成或手动维护，属于**不可自动合并的“用户代码区”**。

### 2. USB 设备栈配置（`USB_Device/Target/usbd_conf.h`、`USB_Device/App/usbd_desc.h`）
- `usbd_conf.h` 通过 `USBD_MAX_NUM_INTERFACES`、`USBD_MAX_NUM_CONFIGURATION`、`USBD_DEBUG_LEVEL`、`USBD_LPM_ENABLED`、`USBD_SELF_POWERED` 等宏控制 CDC 类行为与内存分配策略（`USBD_malloc/free` 映射到静态分配函数）。
- `usbd_desc.h` 暴露 `DEVICE_ID1/2/3` 与 `USB_SIZ_STRING_SERIAL` 等描述符相关常量，用于生成 USB 设备字符串描述符。
- 这些宏同样由 CubeMX 生成，位于 `USER CODE BEGIN/END` 保护段内。

### 3. 应用级硬编码常量（`Core/Src/main.c`）
- 采样参数全部以内联 `#define` 形式硬编码：`CIRCULAR_BUFFER_SIZE=120`、`TOTAL_SAMPLES=240`、`PRE_TRIGGER_SAMPLES=80`、`POST_TRIGGER_SAMPLES=160`，以及 DMA 缓冲区 `adc_raw_buffer`、解码结果 `decoded_signal` 的大小。
- 中断优先级、引脚号（PA4 EXTI、PC13 LED）、ADC 通道（ADC_CHANNEL_3）、时钟源（HSI+HSI48+PLL）均在 `main.c` 的 `MX_*_Init()` 与 `SystemClock_Config()` 中以结构体字面量直接赋值，无外部配置接口。

### 4. CMake 构建期配置（`CMakeLists.txt`、`cmake/stm32cubemx/CMakeLists.txt`、`CMakePresets.json`）
- 顶层 `CMakeLists.txt` 仅做最小化设置（C11、Debug/Release、链接 stm32cubemx 目标），具体源码与包含路径集中在 `cmake/stm32cubemx/CMakeLists.txt` 的 `MX_Defines_Syms`、`MX_Include_Dirs`、`STM32_Drivers_Src`、`USB_Device_Library_Src` 等变量中。
- `CMakePresets.json` 提供 Debug/Release 两套预设，通过 `toolchainFile` 指向 `cmake/gcc-arm-none-eabi.cmake`，并注入 `CMAKE_BUILD_TYPE` 缓存变量；`stm32cubemx/CMakeLists.txt` 利用 `$<$<CONFIG:Debug>:DEBUG>` 条件表达式为 Debug 构建追加 `DEBUG` 宏。
- 链接阶段移除错误依赖 `ob`，并在 POST_BUILD 调用 `objcopy` 生成 `.hex`/`.bin` 及 `size` 报告。

### 设计决策与约束
- **零运行时配置**：项目不解析任何 JSON/YAML/INI/env 文件，也不支持 OTA 更新配置；所有行为差异必须通过切换分支或修改宏重新编译实现。
- **CubeMX 驱动优先**：硬件相关配置集中在 CubeMX 生成的文件中，应用逻辑尽量不触碰寄存器，遵循 HAL 抽象。
- **CMake 作为唯一构建入口**：禁止直接使用 IDE 工程文件进行独立构建，所有交叉编译选项通过 Preset 与工具链文件统一注入。

开发者应遵循的规则：
- 新增外设时，先在 CubeMX 中勾选对应 HAL 模块，再在 `stm32g4xx_hal_conf.h` 确认 `HAL_<X>_MODULE_ENABLED` 已启用。
- 调整 USB CDC 行为时，仅修改 `USB_Device/Target/usbd_conf.h` 中的 `USBD_*` 宏，不要改动库源码。
- 修改采样率、缓冲区大小等算法参数时，同步更新 `main.c` 中对应的 `#define` 与数组声明，确保三者一致。
- 新增编译期宏应通过 `target_compile_definitions()` 写入 `CMakeLists.txt`，避免散落在源文件顶部。