本仓库采用 STM32CubeMX 生成的源码加 CMake 构建系统来管理嵌入式第三方依赖，不依赖任何包管理器（如 Conan、vcpkg），而是以本地子目录加 OBJECT 库的方式静态链接 HAL/CMSIS/USB 设备栈。

1. 使用的系统与工具链
- 构建系统：CMake 3.22，顶层 CMakeLists.txt 仅声明项目、启用 C/ASM 语言、创建可执行目标，并通过 add_subdirectory(cmake/stm32cubemx) 引入 CubeMX 封装层。
- 交叉编译工具链：通过两个独立 toolchain 文件提供 gcc-arm-none-eabi 与 starm-clang 两套方案，由 CMakePresets.json 选择；toolchain 中固定 MCU 为 Cortex-M4（-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard），并指向同一份 STM32G474XX_FLASH.ld 链接脚本。
- 标准库：gcc 工具链使用 --specs=nano.specs（newlib-nano），starm-clang 支持 STARM_HYBRID / NEWLIB / PICOLIBC 三种配置，默认 STARM_PICOLIBC。

2. 核心依赖来源与组织
- CMSIS 内核与设备头文件：Drivers/CMSIS/Include 与 Drivers/CMSIS/Device/ST/STM32G4xx/Include，作为包含路径的一部分被 stm32cubemx INTERFACE 库统一暴露。
- HAL/LL 驱动：Drivers/STM32G4xx_HAL_Driver/Src 下按外设拆分源文件，在 cmake/stm32cubemx/CMakeLists.txt 中以显式列表加入 STM32_Drivers OBJECT 库，未启用自动扫描，避免无关外设代码进入链接。
- USB FS CDC 设备栈：Middlewares/ST/STM32_USB_Device_Library/Core 与 Class/CDC 下的 usbd_core/usbd_ctlreq/usbd_ioreq/usbd_cdc 四个源文件单独聚合为 USB_Device_Library OBJECT 库，仅链接 CDC 类所需的最小集。
- 应用与启动文件：Core/Src/main.c、startup_stm32g474xx.s、system_stm32g4xx.c 以及 USB_Device/App 与 USB_Device/Target 中的用户适配层，直接作为 G4test 可执行目标的私有源。

3. 架构与约定
- 分层接口库：stm32cubemx 是 INTERFACE 库，集中维护 MX_Include_Dirs、MX_Defines_Syms（USE_HAL_DRIVER、STM32G474xx、DEBUG）等跨模块共享属性；STM32_Drivers 与 USB_Device_Library 均 PUBLIC 链接该接口库，保证 include 路径与宏向下传递。
- 对象库而非预编译库：HAL 与 USB 栈均以 OBJECT 库形式参与最终链接，使 CMake 能进行全局死代码消除（配合 -Wl,--gc-sections），减少 Flash/RAM 占用。
- 依赖显式化：所有被编译的源文件都在 CMake 中逐行列出，不存在隐式 glob 或外部 find_package 拉取；新增外设驱动时需在对应变量追加源文件路径。
- 工具链解耦：编译器、objcopy、size、链接器前缀、优化等级、调试符号等全部集中在 cmake/gcc-arm-none-eabi.cmake 与 cmake/starm-clang.cmake，顶层 CMakeLists 不感知具体工具链细节。

4. 开发者应遵循的规则
- 新增 HAL 外设驱动：将对应 Src/*.c 加入 cmake/stm32cubemx/CMakeLists.txt 的 STM32_Drivers_Src 列表，不要修改 Drivers/ 下原始文件。
- 新增 USB 功能：在 USB_Device_Library_Src 中追加相应源文件，并确保其头文件已在 MX_Include_Dirs 中。
- 切换工具链：通过 CMakePresets.json 选择 preset，或在命令行指定 -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake 或 cmake/starm-clang.cmake，不要改动 toolchain 文件中的 MCU 标志。
- 保持 C11 兼容：顶层强制 CMAKE_C_STANDARD=11，且对 C90/C99 报错，新增源文件需遵循此标准。
- 避免引入外部包管理器：本项目未使用任何远程依赖解析机制，所有第三方源码均以只读子目录形式随仓库分发，升级版本需手动替换 Drivers/、Middlewares/ 目录内容。