## 1. 构建系统与工具链

本项目采用 **CMake + Ninja** 作为跨平台构建系统，通过两套交叉编译工具链支持 ARM Cortex-M4 目标：

- **GCC 工具链**：`arm-none-eabi-gcc`（路径前缀 `arm-none-eabi-`），位于 `cmake/gcc-arm-none-eabi.cmake`
- **STARM Clang 工具链**：`starm-clang`（路径前缀 `starm-`），位于 `cmake/starm-clang.cmake`，支持三种配置模式：`STARM_HYBRID`、`STARM_NEWLIB`、`STARM_PICOLIBC`

项目使用 `CMakePresets.json` 定义默认生成器为 Ninja，并通过 `--preset` 参数管理 Debug/Release 两种构建类型。

## 2. 核心构建文件与目录结构

### 顶层入口
- `CMakeLists.txt`：主构建脚本，声明 C11 标准、启用 ASM/C 语言、创建可执行目标 `G4test`，并调用子目录 `cmake/stm32cubemx`
- `STM32G474XX_FLASH.ld`：链接脚本，定义 FLASH(0x8000000, 512K) 和 RAM(0x20000000, 128K) 内存布局，设置堆栈大小（Heap: 0x200, Stack: 0x400）

### 工具链配置
- `cmake/gcc-arm-none-eabi.cmake`：GCC 交叉编译工具链，设置 `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`，启用 `-fdata-sections -ffunction-sections` 进行段裁剪，链接时使用 `nano.specs` 精简 libc
- `cmake/starm-clang.cmake`：Clang 工具链，根据 `STARM_TOOLCHAIN_CONFIG` 选择不同 C 库配置，同样启用段裁剪和内存使用统计

### CubeMX 集成层
- `cmake/stm32cubemx/CMakeLists.txt`：封装 STM32CubeMX 生成代码的构建接口，将源码分为三类：
  - `MX_Application_Src`：应用层源文件（main.c、USB 设备栈等）
  - `STM32_Drivers_Src`：HAL/LL 驱动源文件（ADC、DMA、RCC、GPIO、EXTI、PCD 等）
  - `USB_Device_Library_Src`：USB 设备库源文件（usbd_core、usbd_cdc 等）

## 3. 构建架构与设计决策

### 分层库组织
构建系统采用三层静态库架构：
1. **stm32cubemx (INTERFACE)**：仅暴露包含路径和编译宏（`USE_HAL_DRIVER`、`STM32G474xx`、`DEBUG`）
2. **STM32_Drivers (OBJECT)**：HAL/LL 驱动实现，依赖 stm32cubemx
3. **USB_Device_Library (OBJECT)**：USB CDC 设备栈，依赖 stm32cubemx

顶层目标 `G4test` 链接这三个库，形成清晰的依赖关系。

### 交叉编译配置
- 目标处理器：ARM Cortex-M4，启用硬件浮点（FPV4-SPI-D16）
- 链接阶段启用 `--gc-sections` 进行死代码消除，输出 `.map` 文件和内存使用统计
- 调试版本：`-O0 -g3`，发布版本：`-Os -g0`（GCC）或 `-Oz -g0`（Clang）

### 后处理流程
构建完成后自动执行：
- 使用 `objcopy` 生成 Intel HEX 和原始二进制文件
- 使用 `size` 命令输出目标文件大小统计

## 4. 开发者构建规范

### 标准构建流程
```bash
# 配置并构建 Debug 版本
cmake --preset Debug
cmake --build --preset Debug

# 或直接指定工具链
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
cmake --build build
```

### 关键约定
- **C 标准**：强制要求 C11，低于此版本会报错
- **编译器扩展**：启用 GNU 扩展（`CMAKE_C_EXTENSIONS ON`）
- **头文件组织**：所有包含路径由 `stm32cubemx` 接口库统一管理，用户代码不应直接修改
- **符号定义**：通过 `target_compile_definitions` 集中管理，新增宏应添加到 `MX_Defines_Syms`
- **源文件添加**：新文件需加入对应列表（`MX_Application_Src`、`STM32_Drivers_Src` 或 `USB_Device_Library_Src`）

### 工具链切换
通过 `CMAKE_TOOLCHAIN_FILE` 参数切换工具链：
- GCC：`-DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake`
- Clang：`-DCMAKE_TOOLCHAIN_FILE=cmake/starm-clang.cmake`

## 5. 限制与注意事项

- 无自动化 CI/CD 流水线，构建完全依赖本地环境
- 未提供 Makefile 或 shell 脚本封装，直接使用 CMake 命令
- 无 Docker 容器化构建方案
- 版本号管理未在构建系统中体现，需外部维护
- 链接脚本固定针对 STM32G474RETx，更换芯片需修改 `STM32G474XX_FLASH.ld`