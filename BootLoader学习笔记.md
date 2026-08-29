# 嵌入式 Bootloader 学习笔记

> 学习主线：ARM Cortex-M 单片机 Bootloader。BIOS/x86 内容仅用于帮助理解“引导”这一通用概念，不是重点。

## 0. 本课程的目标

从零写出一个能接收新固件、写入 Flash、校验固件并跳转到应用程序的嵌入式 Bootloader。

建议的学习顺序：

1. 理解 MCU 上电、复位、Flash 和启动向量表
2. 理解 Bootloader 与应用程序的 Flash 分区和链接地址
3. 学会从 Bootloader 正确跳转到应用程序
4. 通过串口（UART）接收新的固件
5. 擦除、写入、读取片内 Flash
6. 用 CRC / 哈希校验固件完整性
7. 设计升级标志、版本、失败恢复与回滚
8. 认识 CAN、USB、以太网、OTA 与安全启动

### 0.1 嵌入式 Bootloader 与 BIOS Boot Loader 的区别

| 项目 | PC BIOS 引导 | MCU 嵌入式 Bootloader |
| --- | --- | --- |
| 典型设备 | PC | STM32、NXP、GD32 等单片机 |
| 初始代码来源 | BIOS 读取磁盘 | 芯片复位后直接从片内 Flash 取向量表 |
| 待加载对象 | 操作系统内核 | 用户应用固件（App） |
| 常用升级通道 | 磁盘 | UART、CAN、USB、蓝牙、以太网、OTA |
| 最常见任务 | 加载系统 | 更新、校验和启动应用 |

接下来课程将以嵌入式 Bootloader 为准。

---

## 第 1 课（STM32F407）：复位后发生什么？

### 1.1 本课程的目标布局

以下布局适用于 Flash 为 512 KB 或 1 MB 的 STM32F407 型号；请以芯片完整料号确认实际容量。

| 区域 | 地址范围 | 用途 |
| --- | --- | --- |
| Bootloader | `0x08000000` ～ `0x0801FFFF` | 前 128 KB；放升级、校验、跳转代码。 |
| Application（App） | 从 `0x08020000` 开始 | 用户主程序。 |

这个边界恰好位于 F407 的 Flash 扇区边界：Sector 0～3 各 16 KB、Sector 4 为 64 KB，合计 128 KB。因此擦除应用区时不会误擦 Bootloader。

### 1.2 上电后的向量表

STM32F407 是 Cortex-M4。正常从用户 Flash 启动时，复位后 CPU 从 `0x08000000` 开始读取**向量表**的前两个 32 位字：

```text
地址          内容                         含义
0x08000000    初始主栈指针（MSP）           CPU 将它装入 MSP
0x08000004    Reset_Handler 的入口地址      CPU 跳转到此处执行
0x08000008    NMI_Handler 的入口地址        NMI 中断入口
0x0800000C    HardFault_Handler 的入口地址  硬故障中断入口
...           其他异常与外设中断入口
```

这就是为什么 App 不能只“跳到 `main()`”：正确启动 App 必须读取它自己的向量表，设置它自己的栈，并跳转到它的 `Reset_Handler`。

> Cortex-M 只执行 Thumb 指令，所以向量表中函数地址的最低位应为 `1`。该位是 Thumb 标志，不是实际地址的一部分。

### 1.3 Bootloader 跳转到 App 的完整动作

当 Bootloader 确认 App 有效后，应依次：

1. 关闭全局中断，停止 SysTick；
2. 关闭并清除 NVIC 中已经使能或挂起的中断；
3. 将 `SCB->VTOR` 改为 App 的向量表地址 `0x08020000`；
4. 读取 `0x08020000` 的初始 MSP，写入 MSP；
5. 读取 `0x08020004` 的 Reset_Handler 地址，跳转执行。

参考实现（使用 CMSIS 头文件）：

```c
#include "stm32f4xx.h"

#define APP_BASE 0x08020000UL
typedef void (*app_entry_t)(void);

void boot_jump_to_app(void)
{
    uint32_t app_msp   = *(volatile const uint32_t *)APP_BASE;
    uint32_t app_reset = *(volatile const uint32_t *)(APP_BASE + 4U);

    /* 先做最基本的有效性检查。完整项目还应校验长度、CRC 和版本。 */
    if ((app_msp & 0x2FFE0000UL) != 0x20000000UL ||
        (app_reset & 1UL) == 0UL) {
        return;
    }

    __disable_irq();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    for (uint32_t i = 0; i < 8U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    SCB->VTOR = APP_BASE;
    __DSB();
    __ISB();
    __set_MSP(app_msp);
    ((app_entry_t)app_reset)();

    while (1) { }
}
```

### 1.4 App 工程必须同步修改的两处

Bootloader 能跳转正确，还要求 App 自己被链接到 `0x08020000`：

1. **链接脚本 / IDE 的 Flash 起始地址**：从 `0x08000000` 改为 `0x08020000`；
2. **向量表偏移**：将 `SCB->VTOR` 设为 `0x08020000`（CubeMX 工程通常可在 `system_stm32f4xx.c` 的 `VECT_TAB_OFFSET` 中设置）。

只改其中一项都会导致 HardFault、进入错误的中断处理函数，或启动后表现异常。

### 1.5 本课必须记住

1. F407 的用户 Flash 默认从 `0x08000000` 映射。
2. 向量表第 0 项是初始 MSP，第 1 项是 `Reset_Handler`。
3. 将 App 放到 `0x08020000` 后，Bootloader 和 App 都要知道这个地址。
4. 跳转 App 前要切换 `VTOR`、MSP 和执行入口，并处理旧中断状态。

---

## 第 2 课（STM32F407 探索者）：Flash 分区与 VTOR

### 2.1 推荐的 Flash 分区

正点原子 STM32F407 探索者常见配置为 1 MB 片内 Flash。推荐将前 128 KB（扇区 0～4）留给 Bootloader，App 从 `0x08020000` 起：

```text
Flash（典型 1 MB，地址 0x08000000 ～ 0x080FFFFF）

0x08000000 ┌──────────────────────────────────────┐
           │ Sector 0～3：4 × 16 KB                │
0x08010000 ├──────────────────────────────────────┤
           │ Sector 4：64 KB                        │
0x08020000 ├──────────────────────────────────────┤ ← App 的向量表 / App 起始地址
           │ Sector 5～11：7 × 128 KB               │
0x08100000 └──────────────────────────────────────┘
           Bootloader：0x08000000 ～ 0x0801FFFF（128 KB）
           App：       0x08020000 ～ 0x080FFFFF（896 KB）
```

**为什么不把 App 放在 `0x08010000`？** 该地址是 Sector 4 的开头，而 Sector 4 有 64 KB。若 Bootloader 只使用前 64 KB，擦除 Bootloader 所在扇区时会同时擦掉 App 的开头；若保留完整 Sector 4，实际分区又不直观。因此把前 5 个扇区都分给 Bootloader，边界定在 `0x08020000` 最安全、最容易维护。

> Flash 的擦除单位是“扇区”，不是任意字节范围。任何分区边界都必须选择扇区边界。

### 2.2 App 工程的地址设置

App 被放在 `0x08020000` 后，以下两处必须一起修改。

#### A. 链接地址

编译器必须将 App 的向量表和代码链接到新地址。

| 开发环境 | 设置 |
| --- | --- |
| Keil MDK | `Options for Target` → `Target` → `IROM1`：Start=`0x08020000`，Size=`0x000E0000`。 |
| STM32CubeIDE | `.ld` 链接脚本的 `FLASH`：`ORIGIN = 0x08020000`，`LENGTH = 896K`。 |

#### B. 向量表地址

在 App 的 `system_stm32f4xx.c` 中，将：

```c
#define VECT_TAB_OFFSET  0x00020000U
```

这样 App 初始化时会把 `SCB->VTOR` 指向 `0x08020000`。

### 2.3 VTOR 是什么？

**VTOR（Vector Table Offset Register，向量表偏移寄存器）**是 Cortex-M 内核的一个寄存器，位于 `SCB`（系统控制块）中。它告诉 CPU：发生异常或外设中断时，应从哪一张向量表中查找处理函数地址。

```text
没有 Bootloader：
VTOR = 0x08000000  → 中断时查询主程序默认向量表

有 Bootloader，且已跳到 App：
VTOR = 0x08020000  → 中断时查询 App 自己的向量表
```

例如 USART1 收到数据触发中断时，CPU 会：

```text
读取 VTOR
  ↓
按 USART1 的中断编号，在“VTOR 指向的向量表”中找到函数地址
  ↓
跳转到对应的 USART1_IRQHandler
```

如果跳转 App 后忘记设置 VTOR，App 虽可能执行到 `main()`，但任意中断发生后 CPU 仍会跳进 Bootloader 的中断表，常见结果是卡死或 HardFault。

### 2.4 本课必须记住

1. Flash 分区边界必须按扇区划分。
2. 推荐 Bootloader 占前 128 KB，App 从 `0x08020000` 开始。
3. VTOR 决定 CPU 遇到异常或中断时使用哪张向量表。
4. App 的链接地址和 `VECT_TAB_OFFSET` 必须同步修改。

---

## 第 3 课：从 Bootloader 正确跳转到 App

### 3.1 App 开头并不是 `main()`

一个 STM32 App 的起始地址 `0x08020000` 处放的是向量表，而不是第一条普通 C 代码：

```text
App_BASE = 0x08020000

地址          32 位内容                           含义
0x08020000    0x2001xxxx                         初始 MSP（主栈指针）
0x08020004    0x0802xxxx | 1                     Reset_Handler 入口
0x08020008    0x0802xxxx | 1                     NMI_Handler 入口
0x0802000C    0x0802xxxx | 1                     HardFault_Handler 入口
...           ...                                 外设中断处理函数入口
```

复位时，Cortex-M 硬件自动做两件事：

```text
MSP = *(uint32_t *)0x08000000
PC  = *(uint32_t *)0x08000004
```

而 Bootloader 跳 App 时必须手动复现同样的关键动作，只是地址变为 `0x08020000`。

### 3.2 Reset_Handler 做了哪些事？

`Reset_Handler` 通常由启动文件（如 `startup_stm32f407xx.s`）提供。它在调用 `main()` 前完成：

1. 初始化 `.data`：把 Flash 中有初值的全局变量复制到 RAM；
2. 清零 `.bss`：将无初值的全局变量置零；
3. 调用 `SystemInit()`：配置基础时钟、FPU 等；
4. 调用 C 库初始化；
5. 调用 `main()`。

因此，跳转目标必须是 `Reset_Handler`，不能直接跳转 `main()`。

### 3.3 跳转前需要清理什么？

Bootloader 使用过 SysTick、串口、DMA 或其他中断后，App 不应继承这些运行状态。最低限度应做到：

1. 关闭全局中断；
2. 停止 SysTick；
3. 禁止并清除 NVIC 的中断；
4. 将 VTOR 改到 App 向量表；
5. 将 MSP 改为 App 的初始 MSP；
6. 跳转 App 的 Reset_Handler。

完整项目还应按实际外设关闭 DMA、串口、定时器，或调用相应的反初始化函数。

### 3.4 跳转函数（CMSIS 版本）

将下列代码放入 Bootloader 工程。它使用 CMSIS 定义的 `SCB`、`NVIC`、`SysTick` 与内建函数；STM32Cube HAL 和 Keil 工程通常已包含这些定义。

```c
#include "stm32f4xx.h"

#define APP_BASE  0x08020000UL
#define APP_END   0x08100000UL
#define SRAM_BASE 0x20000000UL
#define SRAM_END  0x20020000UL

typedef void (*app_entry_t)(void);

static int boot_app_is_valid(void)
{
    uint32_t app_msp   = *(volatile const uint32_t *)APP_BASE;
    uint32_t app_reset = *(volatile const uint32_t *)(APP_BASE + 4U);
    uint32_t reset_addr = app_reset & ~1UL;

    if (app_msp < SRAM_BASE || app_msp > SRAM_END ||
        (app_msp & 7UL) != 0UL) {
        return 0;
    }

    if ((app_reset & 1UL) == 0UL) {
        return 0;
    }

    if (reset_addr < APP_BASE || reset_addr >= APP_END) {
        return 0;
    }

    return 1;
}

void boot_jump_to_app(void)
{
    uint32_t app_msp;
    uint32_t app_reset;

    app_msp   = *(volatile const uint32_t *)APP_BASE;
    app_reset = *(volatile const uint32_t *)(APP_BASE + 4U);

    if (!boot_app_is_valid()) {
        return;
    }

    __disable_irq();

    /* 停止系统节拍定时器。 */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    /* F407 有 82 个外设中断：ceil(82 / 32) = 3 组寄存器。 */
    for (uint32_t i = 0; i < 3U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFUL; /* 禁止中断 */
        NVIC->ICPR[i] = 0xFFFFFFFFUL; /* 清除挂起标志 */
    }

    SCB->VTOR = APP_BASE;  /* 后续中断使用 App 的向量表 */
    __DSB();                /* 保证寄存器写入在继续前生效 */
    __ISB();

    __set_CONTROL(0U);      /* Thread 模式使用 MSP，而非 PSP */
    __ISB();
    __set_MSP(app_msp);    /* 切换到 App 的栈 */
    __enable_irq();        /* 恢复芯片复位时的全局中断状态 */
    ((app_entry_t)app_reset)(); /* 进入 App 的 Reset_Handler；正常不会返回 */

    while (1) { }
}
```

### 3.5 每一行最容易误解的地方

| 代码 | 作用 | 常见错误 |
| --- | --- | --- |
| `APP_BASE + 0` | 读取 App 的栈顶地址 | 误以为这是代码入口。 |
| `APP_BASE + 4` | 读取 Reset_Handler 地址 | 直接写死 `main()` 地址。 |
| `SCB->VTOR = APP_BASE` | 切换中断向量表 | 忘记切换，导致 App 一开中断就异常。 |
| `__set_MSP(app_msp)` | 切换主栈 | 不切栈，App 的函数调用和中断会破坏 Bootloader 栈。 |
| `app_reset` 最低位 | Thumb 状态位，应为 `1` | 清除最低位或没有检查它。 |

### 3.6 当前阶段的验证方法

先不写升级功能，完成一个“跳转验证”：

1. 创建 Bootloader 工程，链接地址保持 `0x08000000`；
2. 创建 App 工程，按第 2 课改为 `0x08020000`；
3. App 的 `main()` 中点亮一个 LED，或通过串口周期性打印 `APP RUNNING`；
4. Bootloader 延时约 1 秒后调用 `boot_jump_to_app()`；
5. 观察 LED / 串口是否进入 App 的行为。

> 这个验证通过，才开始做 UART 接收和 Flash 写入。先确保“跳转链路”可靠，定位问题会容易很多。

---

## 第 4 课：栈、MSP，以及向量表的第一项

### 4.1 栈是什么？

栈（stack）是 RAM 中的一块临时工作区。CPU 在这里保存函数调用现场、局部变量和中断现场。

例如调用函数时：

```text
main() 调用 task()
  ↓
CPU 在栈中保存“task() 执行完后应回到哪里”
  ↓
进入 task()
  ↓
task() 返回，CPU 从栈中取回返回位置
```

如果没有正确的栈地址，任何函数调用、中断、局部变量操作都可能覆盖未知内存，程序会立刻异常。

### 4.2 栈顶与向下增长

对 Cortex-M 来说，栈从高地址向低地址增长。假定可用 RAM 的末端是 `0x20020000`：

```text
高地址
0x20020000  ← 初始 MSP（栈顶；第一个数据写入前的位置）
     ↓
     ↓  每次压栈，MSP 变小
     ↓
0x20000000  ← RAM 起始地址
低地址
```

初始值可以是 RAM 末地址之后的 `0x20020000`，因为 CPU 第一次压栈时会先减小 MSP，再写入数据。

### 4.3 MSP 是什么？

MSP 是 **Main Stack Pointer（主栈指针）**，也就是 CPU 当前主栈顶的位置。Cortex-M 有两个栈指针：

| 名称 | 全称 | 常见用途 |
| --- | --- | --- |
| MSP | Main Stack Pointer | 复位、异常、中断、裸机程序的默认栈。 |
| PSP | Process Stack Pointer | RTOS 中常用于任务自己的栈。 |

裸机 Bootloader 和普通裸机 App 默认使用 MSP，因此 Bootloader 跳到 App 前必须把 MSP 改为 App 向量表第 0 项的值。

### 4.4 为什么 App 需要自己的 MSP？

Bootloader 和 App 都会调用函数、处理串口中断，都会使用栈。若跳转时继续使用 Bootloader 的 MSP：

- App 的局部变量会覆盖 Bootloader 原来留下的数据；
- App 的启动代码与中断现场不在它预期的栈区域；
- 发生中断时容易出现难以定位的异常。

因此 App 的链接脚本会把自己的栈顶写进 App 向量表第 0 项。Bootloader 不需要猜栈地址，只需读取这一项。

### 4.5 这一概念如何变成一行代码？

当我们确定“App 的第 0 项是初始 MSP”后，读取它就是：

```c
uint32_t app_msp = *(volatile const uint32_t *)APP_BASE;
```

这不是神秘写法：

```text
APP_BASE = 0x08020000
把它看成 uint32_t 地址
取出该地址保存的 4 字节
得到 App 希望使用的初始 MSP
```

下一课将以同样的方式解释第 1 项 `Reset_Handler`，再把这两项连成完整跳转。

---

## 第 5 课：Reset_Handler，以及为什么不能直接跳 `main()`

### 5.1 从复位到 `main()` 的真实路径

我们平时写的是 `main()`，但 MCU 复位时并不知道 `main()` 在哪里。它只认识向量表：

```text
复位
  ↓
从向量表第 0 项读取初始 MSP
  ↓
从向量表第 1 项读取 Reset_Handler 地址
  ↓
执行 Reset_Handler
  ↓
初始化 RAM、时钟和 C 运行环境
  ↓
调用 main()
```

`Reset_Handler` 定义在启动文件中（例如 `startup_stm32f407xx.s`），一般不需要我们手写。

### 5.2 Reset_Handler 必须完成的准备工作

在 `main()` 之前，C 程序需要一系列准备：

| 准备工作 | 目的 | 未完成的后果 |
| --- | --- | --- |
| 复制 `.data` 段 | 将“有初值的全局变量”从 Flash 拷到 RAM。 | `int n = 3;` 的 `n` 值不可靠。 |
| 清零 `.bss` 段 | 将“未初始化的全局变量”置零。 | 全局变量初始值随机。 |
| `SystemInit()` | 配置 FPU、时钟等基础硬件。 | 时钟或浮点相关行为错误。 |
| C 库初始化 | 准备库代码所需环境。 | 部分库函数无法正常使用。 |
| 调用 `main()` | 进入用户业务逻辑。 | — |

所以 Bootloader 若直接调用 `main()`，App 的全局变量、运行环境和中断相关初始化都可能不正确。

### 5.3 向量表的第二项

App 位于 `APP_BASE = 0x08020000` 时：

```text
0x08020000：初始 MSP
0x08020004：Reset_Handler 地址
```

读取第二项的代码：

```c
uint32_t app_reset = *(volatile const uint32_t *)(APP_BASE + 4U);
```

逐步理解：

```text
APP_BASE + 4U
    ↓
0x08020000 + 4
    ↓
0x08020004（向量表第二项的地址）
    ↓
读取此地址中的 32 位数
    ↓
得到 App 的 Reset_Handler 入口地址
```

### 5.4 为什么地址最低位是 1？

Cortex-M 只运行 Thumb 指令。函数地址最低位的 `1` 是 Thumb 状态标志：

```text
向量表中看到：0x080203D1
实际指令所在位置：0x080203D0
最低位 1：表示以 Thumb 状态执行
```

因此 Bootloader 可用 `(app_reset & 1U) != 0U` 作为 App 向量表的一个基本有效性检查。不要手动清掉这个最低位后再跳转。

### 5.5 本课必须记住

1. App 的第 1 个可执行入口是 `Reset_Handler`，不是 `main()`。
2. `Reset_Handler` 初始化 C 程序运行环境，然后调用 `main()`。
3. App 向量表第 1 项位于 `APP_BASE + 4`。
4. Cortex-M 函数入口最低位为 `1`，表示 Thumb 状态。

### 5.6 常见地址混淆

`0x08020004` 是“向量表第二项所在的位置”，而不是 `Reset_Handler` 的代码地址。该位置中保存的 32 位内容才是入口地址：

```text
内存位置（槽位）               该位置存放的内容
0x08020000                      0x20020000（示例：初始 MSP）
0x08020004                      0x080203D1（示例：Reset_Handler）
                                               ↓
                                    实际指令地址为 0x080203D0
                                    最低位 1 为 Thumb 状态标志
```

---

## 第 6 课：中断为什么需要切换 VTOR？

### 6.1 中断也需要“函数地址表”

CPU 执行 App 的过程中，串口收到数据、定时器超时等事件会产生中断。CPU 必须知道应跳转到哪个函数处理事件，例如：

```text
USART1 发生中断  →  USART1_IRQHandler()
SysTick 到期     →  SysTick_Handler()
```

这些处理函数的入口地址也存放在向量表中。VTOR 指向哪张向量表，CPU 就从哪张表查找中断处理函数。

### 6.2 为什么不能继续使用 Bootloader 的向量表？

启动时，VTOR 默认指向 Bootloader 的向量表：

```text
VTOR = 0x08000000
```

但跳到 App 后，App 有自己的 `USART1_IRQHandler`、`SysTick_Handler` 等函数；它们的地址都在 App 向量表 `0x08020000` 开始的位置。

```text
Bootloader 运行时：VTOR = 0x08000000
App 运行时：       VTOR = 0x08020000
```

若忘记切换，App 主循环可能暂时正常；一旦启用串口、定时器或 SysTick，CPU 会按 Bootloader 的表跳转，轻则进错函数，重则进入默认死循环或 HardFault。

### 6.3 为什么要先关闭中断？

切换过程包含“停止旧 SysTick、改 VTOR、换 MSP、跳转”多个动作。若中断在中途发生，CPU 可能：

- 用 Bootloader 的向量表，却使用了 App 的 MSP；
- 或用 App 的向量表，却还处在 Bootloader 的运行环境。

两种情况都会破坏程序状态。因此必须先关闭全局中断，完成切换后由 App 自己在初始化完成时重新开启中断。

### 6.4 跳转的概念顺序

```text
Bootloader 正在运行
  ↓
关闭中断，停止自己使用的定时器
  ↓
VTOR 指向 App 向量表
  ↓
MSP 改为 App 的初始栈顶
  ↓
跳到 App 的 Reset_Handler
  ↓
App 自己初始化，并在合适时机开启中断
```

### 6.5 本课必须记住

1. 向量表不仅有复位入口，还保存每个中断的处理函数地址。
2. VTOR 决定 CPU 在中断时使用哪一张向量表。
3. 从 Bootloader 跳入 App 后，VTOR 必须改为 `APP_BASE`。
4. 中断应在切换前关闭，并由 App 在初始化后重新开启。

---

## 第 7 课：为什么 Bootloader 要先检查 App 是否有效？

### 7.1 空白 Flash 不是一个 App

刚擦除的 STM32 Flash 字节通常为 `0xFF`，于是 App 区开头会是：

```text
0x08020000：0xFFFFFFFF
0x08020004：0xFFFFFFFF
```

它们显然不是合法的栈顶和函数入口。若 Bootloader 不检查便照此设置 MSP、跳转，通常立即进入异常。

### 7.2 第一层：向量表格式检查

Bootloader 在跳转前至少检查两项：

```text
第 0 项（MSP）必须指向 F407 的主 SRAM 区域：
0x20000000 ～ 0x20020000

第 1 项（Reset_Handler）必须：
1. 最低位为 1（Thumb 标志）；
2. 实际代码地址位于 App Flash 区内。
```

对本课程使用的分区而言，Reset_Handler 的实际地址应落在：

```text
0x08020000 ～ 0x080FFFFF
```

> 检查时可用 `app_reset & ~1UL` 去掉 Thumb 标志后再比较地址范围；但实际跳转时仍要保留原始的 `app_reset`，使最低位保持为 `1`。

### 7.3 第二层：完整性校验

地址范围检查只能证明“看起来像 App”，不能证明 App 没有在传输或掉电过程中损坏。因此真正的升级 Bootloader 还要保存固件的：

- 长度；
- CRC32（或更强的哈希）；
- 版本号；
- 有效标志。

Bootloader 计算 App 的 CRC，与保存的 CRC 对比；相同才允许启动。

### 7.4 第三层：安全启动（进阶）

CRC 只能防止意外损坏，不能证明固件来自可信来源。若产品有安全需求，还要验证数字签名，并启用读保护、写保护等机制。这是后续进阶内容。

### 7.5 本课必须记住

1. 空白 Flash 读出常为 `0xFFFFFFFF`，不能直接跳转。
2. 最低限度要检查 MSP、Reset_Handler 的 Thumb 位和地址范围。
3. 实用 Bootloader 还必须用 CRC 检查固件完整性。
4. 检查入口地址时可暂时屏蔽最低位；跳转时必须保留它。

---

## 第 8 课：把跳转概念写成代码

以下代码放在 Bootloader 工程中，依赖 CMSIS 的 `stm32f4xx.h`。它对应前面已经理解的五件事：检查 App、关闭中断、切换 VTOR、切换 MSP、进入 Reset_Handler。

```c
#include "stm32f4xx.h"

#define APP_BASE  0x08020000UL
#define APP_END   0x08100000UL
#define SRAM_BASE 0x20000000UL
#define SRAM_END  0x20020000UL

typedef void (*app_entry_t)(void);

static int boot_app_is_valid(void)
{
    uint32_t app_msp   = *(volatile const uint32_t *)APP_BASE;
    uint32_t app_reset = *(volatile const uint32_t *)(APP_BASE + 4U);
    uint32_t reset_addr = app_reset & ~1UL;

    if (app_msp < SRAM_BASE || app_msp > SRAM_END ||
        (app_msp & 7UL) != 0UL) {
        return 0;
    }
    if ((app_reset & 1UL) == 0UL) {
        return 0;
    }
    if (reset_addr < APP_BASE || reset_addr >= APP_END) {
        return 0;
    }
    return 1;
}

void boot_jump_to_app(void)
{
    uint32_t app_msp   = *(volatile const uint32_t *)APP_BASE;
    uint32_t app_reset = *(volatile const uint32_t *)(APP_BASE + 4U);

    if (!boot_app_is_valid()) {
        return;
    }

    __disable_irq();

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    for (uint32_t i = 0; i < 3U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    SCB->VTOR = APP_BASE;
    __DSB();
    __ISB();

    __set_CONTROL(0U);
    __ISB();
    __set_MSP(app_msp);
    __enable_irq();
    ((app_entry_t)app_reset)();

    while (1) { }
}
```

### 8.1 先读懂检查函数

```c
uint32_t app_msp = *(volatile const uint32_t *)APP_BASE;
```

读取 App 向量表第 0 项，即 App 的初始 MSP。

```c
uint32_t app_reset = *(volatile const uint32_t *)(APP_BASE + 4U);
```

读取 App 向量表第 1 项，即带 Thumb 标志的 Reset_Handler 值。

```c
uint32_t reset_addr = app_reset & ~1UL;
```

`~1UL` 表示除最低位外其余位全为 1；按位与后可得到纯代码地址，用来做 Flash 范围比较。`app_reset` 原值没有被改动，跳转时仍保留 Thumb 标志。

三个 `if` 分别验证：栈在 SRAM、入口是 Thumb 函数、入口代码在 App Flash。任一条件不成立就返回 `0`，Bootloader 留在升级模式或报错，而不贸然跳转。

### 8.2 再读懂跳转函数

```c
__disable_irq();
```

避免切换过程被中断打断。

```c
SysTick->CTRL = 0U;
```

停止 Bootloader 的系统节拍，防止它在 App 中继续触发。

```c
NVIC->ICER[i] = 0xFFFFFFFFUL;
NVIC->ICPR[i] = 0xFFFFFFFFUL;
```

分别禁用所有外设中断、清除已挂起的外设中断。F407 有 82 个外设中断，每组寄存器管理 32 个，因此循环 3 组。

```c
SCB->VTOR = APP_BASE;
```

之后的异常和中断从 App 的向量表找入口。

```c
__set_CONTROL(0U);
__set_MSP(app_msp);
```

确保 Thread 模式选用 MSP，并切到 App 自己的栈。

```c
__enable_irq();
((app_entry_t)app_reset)();
```

恢复与硬件复位一致的全局中断状态，然后以函数入口的形式跳入 App 的 `Reset_Handler`。此时所有 NVIC 外设中断仍是禁用的；App 会在自己的初始化中按需启用它们。

### 8.3 现在不要急于烧录

代码理解完成后，下一步才是创建两个工程、修改 App 链接地址，并做“跳转后点亮 LED”的最小验证。先确认自己能解释每个步骤的原因。

---

## 第 9 课：串口升级前，先设计固件包

### 9.1 串口收到的是字节流，不是“一个文件”

UART 只能陆续收到字节：

```text
第 1 次：若干字节
第 2 次：若干字节
...
```

它不知道“固件从哪里开始、何时结束、是否完整”。因此 Bootloader 不能看到数据就立刻当作可启动 App；必须先有通信协议和固件描述信息。

### 9.2 最小固件包应有什么信息？

为后续学习，我们采用一个简单的逻辑包：

```text
┌───────────────┬──────────────────────────────────┐
│ 字段          │ 作用                             │
├───────────────┼──────────────────────────────────┤
│ Magic         │ 固定识别码，确认这是我们的固件包 │
│ Length        │ App 固件的实际字节数             │
│ CRC32         │ App 固件的完整性校验值           │
│ Version       │ 固件版本号（后续可用于升级策略） │
│ Payload       │ 真正的 App 二进制内容            │
└───────────────┴──────────────────────────────────┘
```

### 9.3 为什么每项都需要？

```text
Magic：防止把普通串口数据误当作升级包。
Length：Bootloader 才知道要接收、计算 CRC、写入多少字节。
CRC32：检测传输错误或中途掉电导致的内容损坏。
Version：后续可阻止旧固件覆盖新固件。
Payload：最终写入 0x08020000 的 App 程序。
```

### 9.4 正确升级流程

```text
收到升级命令
  ↓
读取并检查包头（Magic、Length）
  ↓
擦除 App 所在的 Flash 扇区
  ↓
分块接收 Payload，分块写入 Flash
  ↓
对 Flash 中写入的内容计算 CRC32
  ↓
CRC 正确 → 标记 App 有效 → 复位 / 跳转
CRC 错误 → 不标记有效，留在 Bootloader 等待重传
```

### 9.5 一个重要原则

**不能先声明 App 有效，再写入固件。**

若写入期间掉电，Flash 中只有半份 App。正确做法是：全部写入成功、CRC 校验通过之后，才设置“有效”状态。下一课会设计这个状态存放的位置。

---

## 第 10 课：固件信息应存在哪里？

### 10.1 不能与 App 混在一起

App 区被擦除、写入时，其中的旧数据都会消失。若把“App 是否有效、长度、CRC”也放在 App 区，会在升级过程中一起被擦掉，逻辑难以保证掉电安全。

也不能把状态数据写在与 Bootloader 代码共用的 Flash 扇区：Flash 只能按扇区擦除，更新一小段状态可能会擦掉 Bootloader 自身。

### 10.2 本课程采用的三段分区

F407 前 128 KB 正好包含四个 16 KB 扇区和一个 64 KB 扇区。我们将它细分：

```text
0x08000000 ┌────────────────────────────────────┐
           │ Bootloader 代码：64 KB              │
           │ Sector 0 ~ Sector 3                 │
0x08010000 ├────────────────────────────────────┤
           │ Firmware Metadata：64 KB            │
           │ Sector 4（独立擦除，保存状态）      │
0x08020000 ├────────────────────────────────────┤
           │ App：896 KB                          │
           │ Sector 5 ~ Sector 11                │
0x08100000 └────────────────────────────────────┘
```

App 起始地址依然是 `0x08020000`，不需要修改；之后只需将 Bootloader 工程可用 Flash 上限从 128 KB 收紧为 64 KB，防止它长大后侵入 Metadata 扇区。

### 10.3 Metadata 里保存什么？

Metadata 固定存放在 `0x08010000`。逻辑结构如下：

```c
typedef struct {
    uint32_t magic;       /* 固定识别码 */
    uint32_t state;       /* 擦除中 / 已完成 */
    uint32_t app_length;  /* App 字节数 */
    uint32_t app_crc32;   /* App CRC32 */
    uint32_t version;     /* 固件版本 */
} firmware_info_t;
```

### 10.4 掉电安全的写入顺序

Flash 擦除后每一位为 `1`，编程只能把位从 `1` 改为 `0`。因此采用：

```text
擦除 Metadata 扇区
  ↓
擦除 App 扇区并接收、写入 App
  ↓
计算并确认 App CRC 正确
  ↓
写入长度、CRC、版本等 Metadata 内容
  ↓
最后写入 state = VALID
```

`state = VALID` 是最后一步。任意前一步掉电时，Metadata 都不是有效状态；Bootloader 留在升级模式，而不会启动半成品 App。

### 10.5 本课必须记住

1. Flash 以扇区擦除，状态数据应独占一个扇区。
2. 对 F407，本课程使用 Sector 4（`0x08010000`）保存 Metadata。
3. Bootloader 代码、Metadata、App 三者必须位于不同的擦除区。
4. `VALID` 标志必须最后写入。

---

## 第 11 课：让 Metadata 参与 App 有效性判断

### 11.1 从“像 App”到“确认是完整 App”

此前的检查只读取 App 向量表：栈地址是否在 RAM、复位入口是否在 App Flash。这只能说明 Flash 内容“看起来像程序”。

加入 Metadata 后，Bootloader 启动 App 的条件变为：

```text
Metadata 的 magic 正确
且 Metadata 的 state 为 VALID
且记录的长度合法
且 App 向量表合法
且计算出的 CRC32 等于 Metadata 中记录的 CRC32
```

前四项较快；CRC 需要遍历 App 内容，通常在真正进入 App 前完成。

### 11.2 固件信息结构与常量

在 Bootloader 中，Metadata 的数据结构可定义为：

```c
#define META_BASE       0x08010000UL
#define META_MAGIC      0x4657494DU  /* ASCII：FWIM */
#define META_STATE_VALID 0x00000000UL

typedef struct {
    uint32_t magic;
    uint32_t state;
    uint32_t app_length;
    uint32_t app_crc32;
    uint32_t version;
} firmware_info_t;
```

`FWIM` 只是选定的识别码；不要求你把它当文本使用。关键是升级端和 Bootloader 都使用同一个固定值。

### 11.3 为什么 VALID 用 `0x00000000`？

STM32 Flash 擦除后是 `0xFFFFFFFF`，写 Flash 时只能把位从 `1` 写为 `0`，不能把 `0` 改回 `1`（要改回必须先擦除整个扇区）。

因此：

```text
擦除后的 state：0xFFFFFFFF（不是 VALID）
完成后的 state：0x00000000（VALID）
```

最后把 `state` 写成全 0，适合“最后提交”的掉电安全操作。

### 11.4 如何读取 Metadata？

Metadata 已在 Flash 固定地址，读取方式和读取向量表相同；只是把该地址解释为一个结构体指针：

```c
const firmware_info_t *info =
    (const firmware_info_t *)META_BASE;
```

之后：

```c
info->magic
info->state
info->app_length
```

分别表示 Flash 中该结构体的对应字段。`->` 表示“通过结构体指针访问成员”。

### 11.5 需要注意的过渡阶段

你当前已下载的 App 不是通过本 Bootloader 升级写入的，没有 Metadata。因此**现在不能立刻要求 Metadata 有效**，否则 Bootloader 会拒绝已有 App，测试 LED 不再进入 App。

先学习并写入 Metadata 的读检查函数；等 UART 接收、Flash 擦写和 CRC 都做好后，才将它接入 `boot_app_is_valid()`。

---

## 第 12 课：如何选择“进入升级”还是“启动 App”？

### 12.1 Bootloader 不能无限等待

若 Bootloader 永远卡在 `HAL_UART_Receive()` 等待串口数据，设备每次上电都无法自动进入正常 App；但若它立即跳 App，又没有时间让电脑发升级命令。

因此使用一个有限窗口，例如 2 秒：

```text
上电
  ↓
初始化 USART1
  ↓
向电脑提示“BOOTLOADER READY”
  ↓
在 2 秒窗口内等待命令
  ├─ 收到正确命令 UPDATE → 留在 Bootloader，进入升级流程
  └─ 超时或命令不正确 → 跳转 App
```

### 12.2 为什么选择字符串 `UPDATE`？

它只是我们定义的协议起点：电脑与 Bootloader 都约定这 6 个 ASCII 字节表示“我要升级”。

```text
U  P  D  A  T  E
55 50 44 41 54 45（十六进制 ASCII）
```

后面还会定义更多命令（发送固件包头、发送固件数据、完成通知）；当前先只验证“收命令并作选择”。

### 12.3 阻塞接收与超时

HAL 提供：

```c
HAL_UART_Receive(&huart1, buffer, length, timeout_ms);
```

它的含义是：在最长 `timeout_ms` 毫秒内，尝试收到 `length` 个字节。

```text
收到完整命令 → 返回 HAL_OK
时间耗尽 → 返回 HAL_TIMEOUT
硬件异常 → 返回 HAL_ERROR
```

第一版先用这种阻塞、带超时的方式，最容易理解；后续做大固件传输时，再学习中断和 DMA 接收。

### 12.4 命令比较

串口接收的数据只是字节数组。收到 6 个字节后，需要与期望命令比对：

```c
uint8_t command[6];

/* 收到后，比较 command 是否就是 UPDATE。 */
```

只有全部 6 个字节都匹配，才进入升级模式；这避免任意杂讯或电脑误发的字符触发升级。

---

## 第 1 课：Boot Loader 是什么？

### 1.1 定义

**Boot Loader（引导加载程序）**是计算机启动早期运行的一小段程序。它将操作系统内核或下一阶段程序从磁盘读入内存，然后把 CPU 的执行权交给它。

它不是运行在 Windows 或 Linux 上的普通程序；它运行时，操作系统尚未启动。

### 1.2 传统 BIOS 启动过程

```text
按下电源
  ↓
CPU 从固定位置开始执行 BIOS 固件
  ↓
BIOS 检查并选择一个可启动设备
  ↓
BIOS 将该设备的第一个扇区读到内存地址 0x7C00
  ↓
BIOS 跳转到 0x7C00，执行其中的 Boot Loader
  ↓
Boot Loader 读入内核（或下一阶段引导程序）
  ↓
跳转到内核，操作系统开始运行
```

### 1.3 BIOS 为我们做了什么？

在传统 BIOS 模式中，BIOS 只保证以下几件事：

- 找到一个启动设备；
- 读取其第一个扇区；
- 将该扇区放到内存的 `0x7C00`；
- 开始执行该地址的代码。

内核在哪里、如何读取文件系统、如何配置内存，都是 Boot Loader 后续要处理的事情。

### 1.4 为什么是 512 字节？

传统磁盘的一个扇区大小为 **512 字节**。BIOS 初始只读取一个扇区，所以第一阶段 Boot Loader 的总大小不能超过 512 字节。

其中末尾两个字节必须是启动标志：

```text
偏移 0 ～ 509：代码和数据（共 510 字节）
偏移 510：     0x55
偏移 511：     0xAA
```

如果扇区末尾不是 `0x55AA`，BIOS 通常会认定这个设备不可启动。

### 1.4.1 为什么加载到 `0x7C00`？

`0x7C00` 不是 x86 CPU 强制规定的地址，而是早期 IBM PC BIOS 留下并延续至今的**启动约定**。

在实模式下，较低地址的内存已被 BIOS 数据区、中断向量表等占用；同时 BIOS 还要为自身工作和启动过程保留一部分内存。`0x7C00`（十进制 31,744，约 31 KiB）位于当时可安全使用的低端常规内存区域，放下 512 字节引导扇区后仍有空间供引导程序继续工作。

BIOS 将第一个扇区放入物理地址范围 `0x7C00` ～ `0x7DFF`，然后开始执行它。实模式地址可写作“段:偏移”，因此这个物理位置既可以表示为：

```text
0000:7C00
07C0:0000
```

它们都指向同一物理地址：`段值 × 16 + 偏移 = 0x7C00`。

不同 BIOS 进入引导扇区时设置的段寄存器细节可能不同，因此实际 Boot Loader 通常会在开头主动设置 `DS`、`ES`、`SS` 和栈，而不盲目假设所有寄存器的初值。

### 1.5 运行环境的限制

刚进入 Boot Loader 时，通常没有：

- 操作系统；
- 文件系统接口；
- C 标准库；
- 已建立好的栈；
- 自动配置好的内存管理。

因此，早期 Boot Loader 通常使用汇编语言编写，并借助 BIOS 提供的中断服务访问屏幕和磁盘。

### 1.6 关键术语

| 术语 | 含义 |
| --- | --- |
| BIOS | 主板固件；传统启动模式中最先运行的软件。 |
| 引导扇区（Boot Sector） | 启动设备的第一个 512 字节扇区。 |
| `0x7C00` | BIOS 通常将引导扇区加载到的物理内存地址。 |
| Boot Loader | 负责继续加载系统的引导程序。 |
| 内核（Kernel） | 操作系统的核心，负责管理 CPU、内存、设备和进程。 |

### 1.7 本课必须记住

1. Boot Loader 是操作系统启动前最早执行的程序之一。
2. 在传统 BIOS 启动中，BIOS 将第一个磁盘扇区加载到 `0x7C00` 并执行。
3. 引导扇区大小为 512 字节，最后两个字节必须是 `0x55AA`。
4. 第一阶段程序空间很小，因此通常只负责加载更大的下一阶段或内核。

### 1.8 自测题

1. 为什么 Boot Loader 不能直接依赖 Windows/Linux 的功能？
2. BIOS 初始会从磁盘加载多少数据？加载到哪里？
3. `0x55AA` 的作用是什么？
4. 为什么复杂的 Boot Loader 常分为多个阶段？

### 1.9 下一课预告

下一课将写出第一个可启动的汇编程序：让 Boot Loader 在屏幕上显示 `Hello, Bootloader!`。届时会学习寄存器、段地址、BIOS 中断 `int 0x10`，以及如何生成 512 字节镜像。

## 第 13 课：为什么跳转后 App 会在时钟配置处卡死？

App 单独复位时，STM32 处于默认时钟状态：系统时钟来自 HSI，PLL 尚未用作系统时钟。此时 App 的 `SystemClock_Config()` 可以安全地启动并配置 PLL。

但 Bootloader 跳转不是硬件复位。若 Bootloader 已经把系统时钟切到 PLL（本项目为 168 MHz），App 进入 `SystemClock_Config()` 后又试图配置 PLL，HAL 可能返回错误；随后代码进入 `Error_Handler()` 的死循环。串口看到的一小段乱码常发生在两个程序交接、时钟状态改变的瞬间。

解决方法是在 Bootloader 跳转前恢复 RCC 默认状态：

```c
HAL_RCC_DeInit();   /* 切回 HSI，关闭 PLL/HSE，恢复默认时钟环境 */
__disable_irq();
SysTick->CTRL = 0U;
/* 清除 NVIC 使能和挂起位 */
SCB->VTOR = APP_BASE;
__set_MSP(app_msp);
((app_entry_t)app_reset)();
```

`HAL_RCC_DeInit()` 的目的不是“重启芯片”，而是让 App 得到一个接近真正复位后的时钟起点。之后 App 自己的 `SystemClock_Config()` 重新把系统配置为 168 MHz。

注意顺序：`HAL_RCC_DeInit()` 内部可能用 `HAL_GetTick()` 判断超时，所以它必须在关闭 SysTick **之前**执行。反过来写，程序可能永远停在时钟切换过程。

### 用 LED 定位启动卡点

串口可能在交接时钟的瞬间出现几个乱码，因此不能仅凭乱码判断程序卡在哪里。一个可靠做法是临时把 App 的 `Error_Handler()` 改为让两个 LED 快速闪烁：

- 两灯快速闪烁：已经进入 App，但 App 的某个 HAL 配置（本例重点是时钟）失败；
- 两灯不闪、App 正常 LED 状态也不出现：问题仍在 Bootloader 跳转流程或 App 映像；
- LED0 常亮：App 已正常进入主循环。

这是嵌入式调试的基本方法：在没有调试器或串口不可靠时，使用 GPIO 作为“程序跑到这里了”的可见标记。

### 探索者板的 CH340 与 DTR/RTS 陷阱

正点原子 STM32F407 探索者板的板载 CH340 不仅连接 USART1 的数据线；它还使用 DTR、RTS 经过 Q2/Q3 三极管电路控制 STM32 的 `NRST` 与 `BOOT0`，以支持“一键下载”。因此串口助手一打开若自动改变或保持 DTR/RTS 电平，MCU 可能反复复位，或以 `BOOT0=1` 进入芯片自带下载模式。

这时即使程序最早期只闪 LED，也不会运行；并且 P10 的 PA9/PA10 跳帽对该问题无效，因为 P10 只断开 UART 数据线，不能断开 DTR/RTS 控制电路。

正常调试时应让串口工具禁用 DTR/RTS（不使用硬件流控、关闭自动复位）；如果工具不能控制这两个信号，应换用能关闭 DTR/RTS 的串口终端。

实际案例：网页工具“波特律动串口助手”（`serial.baud-dance.com`）的开源仓库 TODO 中明确写有“修复 RTS 拉高部分板子 boot0 的问题”。它与探索者板的 CH340 一键下载电路组合使用时，会使 BOOT0 被拉高，因此不适合当前这块板子的普通串口调试。

### 深入理解：DTR/RTS 到底是什么信号？

**1. 它们是 RS-232 时代的“握手线”。** 老式串口（DB9 接口）不只有 TX/RX，还有若干控制线：

| 信号 | 方向（相对电脑） | 原始含义 |
| --- | --- | --- |
| DTR | 电脑 → 外设 | Data Terminal Ready：终端（电脑）已就绪 |
| DSR | 外设 → 电脑 | Data Set Ready：调制解调器已就绪 |
| RTS | 电脑 → 外设 | Request To Send：请求发送（流控制） |
| CTS | 外设 → 电脑 | Clear To Send：允许发送（流控制） |

这些线与 TX/RX 上的数据字节毫无关系，是独立的开关电平，当年用于电脑与调制解调器协调链路。

**2. 到了 USB 转串口芯片，它们变成“软件遥控的开关”。** CH340 把 DTR/RTS 实现为普通输出引脚，电平完全由 PC 端软件决定（串口助手的复选框、pyserial 的 `ser.dtr` / `ser.rts`）。引脚名写作 `DTR#`、`RTS#` 表示低有效：空闲为高，“使能”输出低。注意：很多工具在“打开串口”的瞬间会自动把 DTR/RTS 置为使能状态。

**3. 正点原子把它们接到了 NRST 和 BOOT0 上。** ST 芯片自带 ROM Bootloader：`BOOT0=1` 时复位，芯片从系统存储器启动，在 USART1 上等待 ST 私有的下载协议——这就是不写任何代码就能“串口下载”的原因。传统流程需要手动跳线 BOOT0 再按复位；探索者板把 CH340 的 `DTR#`/`RTS#` 经 Q2/Q3 三极管网络连到 NRST 与 BOOT0，让上位机软件按约定顺序改变电平即可自动完成：

```text
BOOT0 拉高 → NRST 打一个复位脉冲 → 芯片进入 ROM Bootloader
  ↓
上位机经 USART1 烧写固件
  ↓
BOOT0 拉回低 → 再次复位 → 从 Flash 启动新程序
```

这就是“一键下载”，不用动跳线帽。

**4. 为什么日常调试时它是陷阱。** 电路焊死在板上，任何控制 DTR/RTS 的软件都在间接“按复位键、拨 BOOT0”：

- 打开串口时自动使能 DTR/RTS → 芯片被持续复位或直接停在 ROM Bootloader，你自己的程序（包括我们的 Bootloader）根本没在运行；
- 传输中途电平跳变 → 芯片意外复位。对升级流程尤其致命：可能停在“App 已擦除、新固件只写了一半”。

**5. 为什么拔 P10 跳帽没用。** P10 只断开 CH340 与 PA9/PA10 之间的 TX/RX 两根数据线；DTR#/RTS# 走的是另一条路径（CH340 → Q2/Q3 → NRST/BOOT0），断数据线不影响控制线。

**6. 正确做法。** 在串口工具里显式关闭 DTR 和 RTS；写上位机脚本时打开端口后立即清掉：

```python
ser = serial.Serial('COM5', 115200, timeout=1)
ser.dtr = False
ser.rts = False
```

或者使用不带这些连线的独立 USB-TTL 小板。

**7. 与本课程的呼应。** 我们自写的 Bootloader 靠 2 秒窗口内的 UPDATE 命令进入升级，不依赖 DTR/RTS——这比“一键下载”更通用，因为 CAN、蓝牙、以太网通道上根本没有这两根信号。反过来，这个电路的思路也值得借鉴：用一根普通 GPIO 做“强制留在 Bootloader”的拨杆，是产品中常见的兜底设计。

## 第 14 课：升级协议与固件头

串口升级不能直接把任意文件字节持续发送给 Bootloader。Bootloader 必须预先知道固件是否属于本协议、应接收多少数据、接收后的正确校验值是什么。

本项目先约定如下升级顺序：

```text
UPDATE 命令 → 固件头（magic、size、crc32）→ 固件数据 → 校验 → 跳转 App
```

### 14.1 魔数（magic）

魔数是协议开头的固定身份标记，例如：

```c
#define FW_MAGIC  0x55AA1234UL
```

Bootloader 收到头部后必须比较 `header.magic == FW_MAGIC`。不相等就丢弃，因为这通常是串口杂字节、残留数据或错误格式，而不是升级包。魔数由我们自己约定；电脑端和 Bootloader 端一致即可。

### 14.2 固件头的三个基本字段

```text
magic    4 字节：协议身份标记
size     4 字节：后续固件数据的总字节数
crc32    4 字节：完整固件数据的 CRC32 校验值
```

`size` 让 Bootloader 知道何时接收完成，并检查数据不会写出 App 区；`crc32` 用来确认数据在串口传输和 Flash 写入后仍完全正确。

### 14.3 用 C 结构体描述固件头

```c
typedef struct
{
    uint32_t magic;
    uint32_t size;
    uint32_t crc32;
} firmware_header_t;
```

`struct` 是把有关联的多个数据组织成一个类型。此处三个成员均为 4 字节 `uint32_t`，所以 `firmware_header_t` 正好是 12 字节；电脑端必须按 `magic → size → crc32` 的顺序发送这 12 字节。

读取后的访问形式为：`header.magic`、`header.size`、`header.crc32`。本项目把该定义放在 `boot_update.h`，以后接收代码和上位机协议都以它为准。

### 14.4 接收与验证必须分开

`boot_receive_header(firmware_header_t *header)` 只负责把 12 字节写到指定内存，`HAL_UART_Receive()` 返回 `HAL_OK` 时该函数返回 1。这里必须使用 `sizeof(*header)`，不能使用 `sizeof(header)`：前者为结构体本身的 12 字节，后者只是指针的 4 字节。

`boot_header_is_valid(const firmware_header_t *header)` 只负责检查，至少验证空指针、魔数，以及 `0 < size <= APP_MAX_SIZE`。其中 `const` 表示检查函数不修改固件头。

在 `main.c` 中的调用顺序为：收到 `UPDATE` 后创建局部变量 `firmware_header_t header`，调用 `boot_receive_header(&header)`，成功后再调用 `boot_header_is_valid(&header)`。此阶段只报告“头部合法/非法”，还不能擦除 Flash。

本项目中 Bootloader 与 App 都必须使用探索者板的 8 MHz 外部晶振，并采用：`PLLM=8`、`PLLN=336`、`PLLP=2`、`PLLQ=7`。这样最终均为 168 MHz，USART1 的 115200 波特率也一致。

---

## 第 15 课：把固件头接入主流程，完成第一次真实握手

### 15.1 当前进度盘点

已完成的代码：

```text
boot_wait_update_command()   2 秒窗口内逐字节匹配 UPDATE
boot_receive_header()        收 12 字节固件头
boot_header_is_valid()       检查 magic 与 size
boot_jump_to_app()           含 HAL_RCC_DeInit 的完整跳转
```

但 `boot_receive_header()` 和 `boot_header_is_valid()` 还没有被任何地方调用——第 14 课的函数只是"造好了零件"。本课把它们装进 `main.c`，并用电脑完成第一次完整的协议握手。

### 15.2 先修掉 boot_update.h 里的一行隐患

当前 `boot_update.h` 中有这样一行：

```c
firmware_header_t *header;    /* ← 应删除 */
```

这是**变量定义**，不是声明。头文件会被多个 `.c` 包含：每包含一次就会定义一个同名全局变量，轻则产生被意外共享的状态，重则链接时报重复定义错误。而且它从未被使用。

规则：**头文件只放类型、宏和函数声明；变量定义放在 `.c` 里。**

删除该行后，`boot_update.h` 的完整内容应为：

```c
#ifndef BOOT_UPDATE_H
#define BOOT_UPDATE_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "usart.h"

#define FW_MAGIC     0x55AA1234UL
#define APP_MAX_SIZE 0x000E0000UL

typedef struct
{
    uint32_t magic;
    uint32_t size;
    uint32_t crc32;
} firmware_header_t;

int boot_receive_header(firmware_header_t *header);
int boot_header_is_valid(const firmware_header_t *header);

#endif
```

### 15.3 接进 main.c

`main.c` 需要两处修改。

第一，包含头文件（USER CODE Includes 区域）：

```c
#include "boot_jump.h"
#include "boot_update.h"   /* 新增 */
#include <string.h>
```

第二，改造 USER CODE 2 中收到 UPDATE 后的分支：

```c
static const uint8_t wait[]       = "WAIT UPDATE (2s)\r\n";
static const uint8_t update_ok[]  = "UPDATE DETECTED\r\n";
static const uint8_t jump[]       = "TIMEOUT: JUMPING TO APP\r\n";
static const uint8_t invalid[]    = "APP INVALID: STAY IN BOOTLOADER\r\n";
static const uint8_t header_ok[]  = "HEADER OK\r\n";      /* 新增 */
static const uint8_t header_bad[] = "HEADER BAD\r\n";     /* 新增 */
const uint8_t *status;
uint16_t status_length;

HAL_UART_Transmit(&huart1, wait, sizeof(wait) - 1U, HAL_MAX_DELAY);

if (boot_wait_update_command()) {
    firmware_header_t header;    /* 局部变量，不是全局 */

    if (boot_receive_header(&header) &&
        boot_header_is_valid(&header)) {
        status = header_ok;
        status_length = sizeof(header_ok) - 1U;
        /* 此处尚未擦除 Flash，第 16 课开始加入。 */
    } else {
        status = header_bad;
        status_length = sizeof(header_bad) - 1U;
    }
} else {
    HAL_UART_Transmit(&huart1, jump, sizeof(jump) - 1U, HAL_MAX_DELAY);
    HAL_Delay(20U);
    boot_jump_to_app();

    /* 只有 App 无效时，boot_jump_to_app() 才会返回。 */
    status = invalid;
    status_length = sizeof(invalid) - 1U;
}
```

执行顺序变成：

```text
收到 UPDATE
  ↓
立即开始等待 12 字节固件头（超时 1000 ms）
  ↓
接收成功且校验通过 → 循环打印 HEADER OK
接收失败或校验不通过 → 循环打印 HEADER BAD
```

### 15.4 字节序：结构体在串口线上长什么样

`boot_receive_header()` 把串口字节直接写进结构体内存。这隐含一个约定：**双方必须使用相同的字节序和相同的字段排布**。

Cortex-M 是**小端（little-endian）**：低字节放在低地址，而串口按地址递增顺序发送。于是：

| 字段 | C 代码中的值 | 串口上先后出现的字节 |
| --- | --- | --- |
| magic | `0x55AA1234` | `34 12 AA 55` |
| size | `0x00000100` | `00 01 00 00` |
| crc32 | `0x11223344` | `44 33 22 11` |

整条固件头（12 字节）：

```text
34 12 AA 55 00 01 00 00 44 33 22 11
```

电脑端生成这 12 字节（x86 也是小端，直接按字节序打包即可）。Python 示例：

```python
import struct
struct.pack('<III', 0x55AA1234, 0x100, 0x11223344)
# 结果即 b'\x34\x12\xaa\x55\x00\x01\x00\x00\x44\x33\x22\x11'
```

`<` 表示小端，三个 `I` 表示三个 4 字节无符号整数，与 `firmware_header_t` 一一对应。

### 15.5 用串口工具做第一次握手测试

1. 编译烧录 Bootloader（App 保留不动，用于观察超时跳转仍正常）；
2. 打开串口工具：115200、8N1、**关闭 DTR/RTS**（第 13 课的 CH340 陷阱）；
3. 复位后 2 秒内，以文本方式发送 `UPDATE`；
4. 随后 1 秒内，以 **HEX 方式**发送 `34 12 AA 55 00 01 00 00 44 33 22 11`；
5. 观察输出。

预期现象：

```text
正常头：       WAIT UPDATE (2s) → UPDATE DETECTED/HEADER OK
改坏 magic：   把首字节 34 改成 35 → HEADER BAD
size = 0：     00 00 00 00 → HEADER BAD
size 超限：    00 00 0F 00（0xF0000 > 896K）→ HEADER BAD
```

**测试协议时必须同时验证拒绝路径**：只验证"好包被接受"的测试是不完整的。

手动操作小技巧：从发 `UPDATE` 切到 HEX 发送可能超过 1 秒超时。两个办法任选：

- 把 `boot_receive_header()` 里的 `1000U` 暂时改成 `5000U`，测试完再改回；
- 使用支持"多段发送 + 间隔延时"的串口工具，一次点击按顺序发出两段。

### 15.6 本课必须记住

1. 头文件只放声明，不放变量定义；`firmware_header_t *header;` 这类行不能出现在 `.h`。
2. 结构体经串口直收，等于按内存布局逐字节传输，双方必须同字节序、同字段排布。
3. STM32 与 x86/Python（`'<I'`）都是小端：`0x55AA1234` 的线上字节序是 `34 12 AA 55`。
4. 协议测试要覆盖正反两面：合法头被接受，非法头（错 magic、0 长度、超长）被拒绝。

### 15.7 下一课预告

第 16 课将开始真正动 Flash：`HAL_FLASH_Unlock()`、按扇区擦除 App 区（Sector 5～11）、`VOLTAGE_RANGE_3`、按字编程 `HAL_FLASH_Program()`，以及写入后的回读校验。

---

## 第 16 课：擦除 App 扇区

### 16.1 擦写 Flash 的三条物理规则

1. **擦除后全 1，编程只能 1→0。** 刚擦除的 Flash 每个字节都是 `0xFF`；写入只能把位从 1 改成 0。想把 0 改回 1，唯一办法是擦除整个扇区。
2. **擦除的单位是扇区，不是字节。** F407 的 App 区（Sector 5～11）每个扇区 128 KB。哪怕只改 1 个字节，也要重擦 128 KB。
3. **擦除很慢，而且擦除期间芯片“停摆”。** 一个 128 KB 扇区的擦除典型需要几百毫秒，最坏可超过 1 秒；并且 F407 是单 bank 结构，擦除/编程期间 CPU 无法从 Flash 取指令——程序实质暂停，中断也进不去。这直接决定了升级流程的顺序：**必须先擦除、后接收数据**（第 9.4 节流程图的依据），绝不能边收串口数据边擦 Flash，否则擦除期间到达的字节必然丢失。

### 16.2 HAL 的三步：解锁 → 擦除 → 上锁

复位后 Flash 控制器默认上锁，这是防止跑飞的程序误写 Flash 把自己变砖的保护。因此每次擦写都要：

```c
HAL_FLASH_Unlock();                 /* 1. 解锁 */
/* 2. 执行擦除或写入 */
HAL_FLASH_Lock();                   /* 3. 上锁 */
```

擦除通过填充一个结构体再调用 `HAL_FLASHEx_Erase()` 完成：

| 字段 | 本项目的值 | 含义 |
| --- | --- | --- |
| `TypeErase` | `FLASH_TYPEERASE_SECTORS` | 按扇区擦（另一种是整片 Mass Erase） |
| `Banks` | `FLASH_BANK_1` | F407 只有 Bank 1 |
| `Sector` | `FLASH_SECTOR_5` | 起始扇区号，对应 `0x08020000` |
| `NbSectors` | 按固件大小计算 | 连续擦几个扇区 |
| `VoltageRange` | `FLASH_VOLTAGE_RANGE_3` | 2.7～3.6 V 供电档位；板上 3.3 V 用这一档 |

> 扇区号是“真枪实弹”：把起始扇区写成 0～4，擦掉的就是 Bootloader 代码或 Metadata，板子要靠 ST-Link/SWD 才能救回来。写 `FLASH_SECTOR_5` 前多看一眼。

### 16.3 要擦几个扇区

用固件头的 `size` 向上取整：

```c
nb = (size + 128K - 1) / 128K;      /* ceil */
```

`size = 0x100` → 1 个扇区；`size = 0x20001` → 2 个；上限 7（`0xE0000 / 0x20000`）。只擦新固件需要的扇区，比无脑擦满 7 个省时间（7 × 128 KB 的擦除可能要好几秒）。

### 16.4 boot_flash 模块

新建 `boot_flash.h`：

```c
#ifndef BOOT_FLASH_H
#define BOOT_FLASH_H

#include <stdint.h>

/* App 区起始扇区与扇区数（Sector 5～11，每个 128 KB）。 */
#define APP_FIRST_SECTOR  5U
#define APP_SECTOR_COUNT  7U
#define APP_SECTOR_SIZE   0x00020000UL

int boot_flash_erase_app(uint32_t app_size);

#endif
```

新建 `boot_flash.c`（与 `boot_update.c` 同目录，加入 Keil 的 Core 组编译）：

```c
#include "boot_flash.h"
#include "stm32f4xx_hal.h"

int boot_flash_erase_app(uint32_t app_size)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0;
    uint32_t nb_sectors;

    if (app_size == 0UL) {
        return 0;
    }

    nb_sectors = (app_size + APP_SECTOR_SIZE - 1UL) / APP_SECTOR_SIZE;
    if (nb_sectors > APP_SECTOR_COUNT) {
        return 0;
    }

    HAL_FLASH_Unlock();

    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Banks        = FLASH_BANK_1;
    erase.Sector       = APP_FIRST_SECTOR;
    erase.NbSectors    = nb_sectors;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK) {
        HAL_FLASH_Lock();
        return 0;
    }

    HAL_FLASH_Lock();
    return 1;
}
```

要点：

- 失败路径也必须 `HAL_FLASH_Lock()`，不能带着锁开着继续跑；
- `sector_error` 记录出问题的扇区号，调试时可下断点查看；
- 返回值只有 1/0，与 `boot_receive_header()` 风格一致。

### 16.5 接进 main.c

在 `boot_header_is_valid()` 为真的分支里紧接着擦除（记得：`status` 与 `status_length` 成对赋值）：

```c
      } else if (boot_header_is_valid(&header)) {
          if (boot_flash_erase_app(header.size)) {
              status = erase_ok;                  /* "ERASE OK\r\n" */
              status_length = sizeof(erase_ok) - 1U;
          } else {
              status = erase_fail;                /* "ERASE FAIL\r\n" */
              status_length = sizeof(erase_fail) - 1U;
          }
      } else {
```

同时在文件头部新增两个字符串和 `#include "boot_flash.h"`。

时序现象：发完固件头后串口会安静约半秒到一秒多（擦除期间 CPU 停摆，连 SysTick 和闪灯都停），然后 `ERASE OK` 出现、闪灯恢复。这个“停顿”本身就是 16.1 规则 3 的直接证据。

### 16.6 验证方法（破坏性测试）

1. 基线：复位后不发包，超时进入 App，App 的 LED 常亮；
2. 发 `UPDATE` + 正常固件头（size 用 `0x100` 即可）；
3. 等待约 1 秒，看到 `ERASE OK`；
4. 按复位、不发包：超时后应打印 `APP INVALID: STAY IN BOOTLOADER` 并双灯闪——App 向量表已被擦成 `0xFF`，`boot_app_is_valid()` 的第一层检查就不过。这是第 7 课“空白 Flash 不是一个 App”的现场兑现；
5. 用 ST-Link/mcuisp 把 App 重新烧回去，恢复基线。

### 16.7 本课必须记住

1. 编程只能 1→0；擦除按扇区；擦后全 `0xFF`。
2. 解锁 → 操作 → 上锁，三步成组，失败路径也要上锁。
3. 3.3 V 供电选 `FLASH_VOLTAGE_RANGE_3`。
4. 擦除期间 CPU 停摆、中断进不来，所以必须先擦后收。
5. 起始扇区号错一位就是变砖，`FLASH_SECTOR_5` 要多看一眼。

### 16.8 补充：这些参数在哪里查？

两份 ST 官方文档各管一摊：

| 文档 | 回答的问题 | 本课信息的出处 |
| --- | --- | --- |
| Datasheet（数据手册） | 这颗芯片“是什么”：容量（由料号决定）、电气特性、封装 | STM32F407**ZGT6** = 1 MB Flash |
| Reference Manual **RM0090**（参考手册） | 这类芯片“怎么用”：外设原理、寄存器、Flash 组织 | 第 3 章 *Embedded Flash memory* 的 *Flash memory organization* 表 |

RM0090 中 1 MB 型号的扇区布局（本课程分区的原始依据）：

```text
Sector 0～3   0x08000000 ～ 0x0800FFFF   4 × 16 KB
Sector 4      0x08010000 ～ 0x0801FFFF   1 × 64 KB
Sector 5～11  0x08020000 ～ 0x080FFFFF   7 × 128 KB
```

注意容量由**完整料号**决定：探索者板的 STM32F407ZGT6 是 1 MB；若换成 512 KB 型号（如 F407VET6），只有 Sector 0～7，App 区仅 3 个 128 KB 扇区。买芯片、看原理图、用 STM32CubeProgrammer 读设备信息都能确认料号。

获取方式：st.com 搜索 `RM0090` 或 `STM32F407 datasheet` 可免费下载（有中文版参考手册）；正点原子资料盘通常也附带这两份。

学习习惯：以后每用一个新外设（UART、CRC、RTC……），先翻 RM0090 对应章节，再动手写代码。

### 16.9 下一课预告

第 17 课：`HAL_FLASH_Program()` 按字写入、不足 4 字节的补齐处理、分块接收循环与回读校验。

---

## 第 17 课：按字写入与分块接收——第一次真升级

### 17.1 写入与擦除的三点不同

1. **单位不同**：擦除按扇区，写入可以小到"字"（4 字节）。HAL 提供 `FLASH_TYPEPROGRAM_WORD` 按字编程。
2. **地址要求**：按字写入的地址必须 4 字节对齐。
3. **前提条件**：只能把 1 写成 0，所以写入的目标必须是刚擦除过的区域（这就是"先擦后写"的硬约束）。

HAL 函数原型值得看一眼：

```c
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data);
```

第三个参数是 `uint64_t`（HAL 为兼容双字编程的芯片统一用 64 位），按字写入时只用低 32 位，传入 `uint32_t` 会隐式扩展，无需强转。

### 17.2 尾部补齐：为什么补 0xFF

固件字节数不一定是 4 的倍数（本课的 App 是 4592 字节，恰好整除；但 4591、4593 都可能出现）。最后一字不足 4 字节时，缺的位置补 `0xFF`：

```text
最后 2 个有效字节 AB CD → 写入的字 = 0xFFFFCDAB
```

补 `0xFF` 的原因：擦除后本来就是全 1，写 0xFF 等于"什么都不改"，不会违反 1→0 规则；回读校验和后续 CRC 都只统计 `size` 内的字节，补齐部分不参与。

### 17.3 停等协议：为什么必须"一问一答"

升级数据有几百 KB，但 RAM 只有 128 KB，不可能整体收完再写，只能分块。分块后有两种策略：

```text
连续发送：PC 不停地发 → Bootloader 边收边写
          ✗ 写 Flash 期间 CPU 可能停摆（单 bank），串口无流控，必然丢字节

停等发送：PC 发一块(256B) → 等 "OK" → 再发下一块
          ✓ 每块写完并回读校验通过才回 ACK，下一块才出发，零丢失风险
```

本课程采用停等（stop-and-wait）。这是最简单、最可靠的流控方式，代价是速度受限于往返延迟（115200 下传输本身占大头，损失不大）。擦除同理：发完固件头后，PC 等 `ERASE OK` 再开始发数据。

### 17.4 任务一：boot_flash_write_chunk()（自己写）

> 教学模式从本课调整：**老师不再提供函数实现，改为"任务书 + 评审"。** 擦除函数（解锁/上锁/结构体）、HAL_UART_Receive（收命令/收固件头）、volatile 读 Flash（读向量表）这些积木都已练过，剩下的是自己搭。

任务书：

```c
int boot_flash_write_chunk(uint32_t address, const uint8_t *data, uint32_t length);
```

验收标准：

1. 把 `data` 的 `length` 字节写入 `address` 开始的 Flash；
2. 返回 1 成功，0 失败（地址未对齐 / HAL 出错 / 回读不一致都算失败）；
3. 尾部不足 4 字节的字，缺的位补 `0xFF`；
4. 全部写完后回读 `length` 字节，与 `data` 逐一比对；
5. 解锁、上锁纪律与擦除函数一致（含失败路径）。

引导问题（先自己答，再动手）：

1. `HAL_FLASH_Program` 一次只写一个字——`length` 字节要循环几次？每次地址怎么走？
2. 4 个独立字节怎么拼成一个 `uint32_t`？想想小端：低字节放在数的低位，用移位和或。
3. 最后一组不足 4 个字节时：先造一个全 `0xFFFFFFFF` 的初值，再把有效字节逐个填进去——为什么这样补齐是安全的？
4. "读 Flash 的某一个字节"你早就会——第 3 课读 App 向量表第 0 项用的就是那个写法。

脑内测试用例（编译前先在纸上跑）：

- `length = 8`：两个字，无补齐；
- `length = 5`：两个字，第二字只有 1 个有效字节，其余 3 个字节是什么值？
- `length = 0`：自己定约定（建议：直接返回成功）。

### 17.5 任务二：boot_receive_and_write()（自己写）

```c
int boot_receive_and_write(uint32_t total_size);
```

验收标准：

1. 分块接收 `total_size` 字节，每块写入 Flash，写成功后回发 `"OK\r\n"`；
2. 每块接收设超时（建议 2 秒），超时或写失败立即返回 0；
3. 全部收完且写完返回 1；
4. 块大小建议 256 字节，缓冲区用 `static` 数组——想想为什么不用局部大数组（栈有多大？）。

引导问题：

1. 循环的条件是什么？（"已收字节数"对比"总数"）
2. 每次应收多少 = min(剩余量, 块大小)，最后一块自动是余数——比如 4592 = 17×256 + 240；
3. 写入地址 = `APP_BASE + 已收字节数`（在 `boot_update.h` 定义 `APP_BASE`）；
4. `HAL_UART_Receive` 的用法已经写过两次（收 UPDATE 字节、收 12 字节固件头）；
5. ACK 用 `HAL_UART_Transmit` 发，消息必须是 `"OK\r\n"`——上位机脚本等的就是它。

### 17.6 任务三：main.c 接线（自己写）

要求：

1. 头合法 → 擦除成功后**立即打印一次 ERASE OK**（上位机等这个信号才开始发数据）→ 调用接收会话；
2. 会话成功 → 打印 `"WRITE DONE: REBOOTING\r\n"` → 延时约 500ms → `NVIC_SystemReset()`（调用后不会返回）；
3. 会话失败 → `status` 指向 `"WRITE FAIL\r\n"`；
4. `"ERASE OK"`、`"OK"`、`"WRITE DONE"` 三个标记必须与 `flash_send.py` 中等待的文本一字不差——协议两端是同一份合同；
5. 动手前先画路径走查表（第 16 课的方法），写完逐条对照。

### 17.7 让 Keil 自动生成 .bin

串口升级发送的是纯二进制 `.bin`，Keil 默认只生成 `.hex`。配置方法（App 工程）：

1. `Options for Target` → `User` 选项卡；
2. 勾选 `Run user programs after Build/Rebuild` 的 `Run #1`；
3. 填入（本机 Keil 为 AC6，注意路径带空格要加引号）：

```text
"D:\Software Download\Keil5\ARM\ARMCLANG\Bin\fromelf.exe" --bin --output=.\BOOTLoader\BOOTLoader.bin .\BOOTLoader\BOOTLoader.axf
```

之后每次编译自动产出 `MDK-ARM\BOOTLoader\BOOTLoader.bin`。

### 17.8 Python 上位机 flash_send.py

已放在 `G:\Boot\flash_send.py`，做的事与协议一一对应：

```text
打开串口(关 DTR/RTS) → 等待 WAIT UPDATE → 发 UPDATE + 12字节头(小端, 含真实CRC32)
  → 等 ERASE OK → 逐块发送, 每块等 OK → 等 WRITE DONE
```

使用前把开头的 `PORT = 'COM5'` 改成实际串口号。CRC32 用 `zlib.crc32` 计算——Bootloader 目前还没校验它（第 18 课的内容），但上位机从现在起就发真实值，将来协议不用改。

### 17.9 验收测试：第一次真升级

1. **改造 App 为 V2**：把 App 的 main.c 主循环改成双灯 300ms 闪烁（当前 V1 是常亮）：

```c
  while (1)
  {
    HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_9 | GPIO_PIN_10);
    HAL_Delay(300U);
  }
```

2. 重新编译 App 工程（确认自动生成了新的 .bin）；
3. **不要**用 ST-Link 烧 V2——V1 还在板子上跑（常亮）；
4. 运行 `python flash_send.py`，按提示按一下复位键；
5. 观察：进度条走完 → 板子自动复位 → **LED 从常亮变成 300ms 闪烁**；
6. 再次上电：2 秒超时后进入 V2 App——升级效果持久化了。

从这一刻起，ST-Link 只在改 Bootloader 自身时才需要。

### 17.10 本课必须记住

1. 按字写入：地址 4 字节对齐，只能 1→0，必须先擦后写。
2. 尾部不足 4 字节补 `0xFF`——"不改任何位"的安全补齐。
3. 停等协议：每块写完+回读通过才回 ACK，规避 CPU 停摆期间的丢字节。
4. 写完用软复位 `NVIC_SystemReset()` 让新固件从干净状态启动。
5. 上位机与 Bootloader 是同一份协议的两端，字段、字节序、消息文本必须一字不差。

### 17.11 下一课预告

第 18 课：CRC32 完整性校验——Bootloader 端计算 App 区 CRC，与固件头中的值比对；以及 Metadata（第 10 课的 Sector 4）正式投入使用，让"掉电安全"的升级状态机落地。

### 17.12 本次动手补充：写入函数与分包接收函数

本次实际完成并理解了 `boot_flash_write_chunk()` 的核心框架。它的参数保护应拒绝以下情况：

```c
data == NULL
length == 0U
address < APP_BASE
address >= APP_END
length > (APP_END - address)
```

最后一个条件采用减法而不是 `address + length > APP_END`，可避免错误输入导致加法溢出后绕过边界检查。

完整的 4 字节写入需要按小端序组合：

```c
word = ((uint32_t)data[0])
     | ((uint32_t)data[1] << 8U)
     | ((uint32_t)data[2] << 16U)
     | ((uint32_t)data[3] << 24U);
```

`data` 是 `const uint8_t *` 指针，因此 `data[0]` 等价于 `*(data + 0)`；每写完一个 word 后，`address += 4U` 和 `data += 4U`，下次便处理后面的 4 字节。`const` 限制函数只读数据，不修改接收缓冲区。

尾部有 1～3 字节时，必须额外写一个 word：先将 `word` 初始化为 `0xFFFFFFFFUL`，再逐个替换低位的有效字节。不能直接对全 `0xFFFFFFFFUL` 使用或运算，因为 `0xFF | 任意值` 仍为 `0xFF`；要先清除目标字节再填入：

```c
word &= ~(0xFFUL << (8U * i));
word |=  ((uint32_t)data[i] << (8U * i));
```

这个尾部 word 只有在 `remain = length % 4U` 不为 0 时才写入；若 `remain == 0U`，绝不能额外调用 `HAL_FLASH_Program()`，否则会误写下一个地址。

为支持分块传输，协议新增：

```c
#define FW_CHUNK_SIZE  256U
```

并定义：

```c
int boot_receive_chunk(uint8_t *buffer, uint32_t length);
```

该函数只接收一包数据：检查 `buffer != NULL`、`length != 0U`、`length <= FW_CHUNK_SIZE`，然后调用 `HAL_UART_Receive(&huart1, buffer, length, 3000U)`；成功返回 1，失败返回 0。条件连接使用逻辑或 `||`，不要使用按位或 `|`。

### 17.13 总控循环：接收一包、写一包、确认一包

`boot_receive_and_write(uint32_t total_size)` 用来完成完整固件的数据阶段。它使用：

```c
static uint8_t buffer[FW_CHUNK_SIZE];
uint32_t received = 0U;
uint32_t chunk_size;
```

循环条件是 `received < total_size`。每轮先计算 `remain = total_size - received`，并令 `chunk_size` 等于 `remain` 与 `FW_CHUNK_SIZE` 两者中较小的那个，因而最后一包会自动缩短。

每轮的固定顺序：

```text
boot_receive_chunk(buffer, chunk_size)
→ boot_flash_write_chunk(APP_BASE + received, buffer, chunk_size)
→ received += chunk_size
→ HAL_UART_Transmit(..., "OK\r\n", ...)
```

`received += chunk_size` 不能写成 `received += received + chunk_size`；后者会让已接收计数重复相加、跳过地址。ACK 必须使用 `HAL_UART_Transmit()`，因为它的方向是 STM32 → 电脑；`HAL_UART_Receive()` 的方向相反，是电脑 → STM32。

### 17.14 main.c 中的升级主流程

收到合法固件头后，`main.c` 按以下顺序组织：

```text
擦除 App 区成功
→ 立即发送 "ERASE OK\r\n"
→ boot_receive_and_write(header.size)
→ 成功：发送 "WRITE DONE: REBOOTING\r\n"
→ HAL_Delay(500U)
→ NVIC_SystemReset()
```

`ERASE OK` 必须立即发送，因为上位机收到它后才发送第一包数据；不能只设置一个稍后在 `while(1)` 中重复打印的状态字符串。

若 `boot_receive_and_write()` 返回 0，应让状态变为 `WRITE FAIL` 并停留在 Bootloader。升级前旧 App 已被擦除，此时不能跳转 App。

`NVIC_SystemReset()` 是 Cortex-M 的软件复位：效果接近按复位键。它让芯片从干净的初始状态重新启动，之后 Bootloader 再按正常逻辑等待 UPDATE 或跳转新 App。该函数调用后不返回；`HAL_UART_Transmit()` 的最后一个参数是发送超时，不是复位前的等待时间，因此需单独调用 `HAL_Delay(500U)`。

### 17.15 第一次串口升级实测成功

实测发送 App `BOOTLoader.bin`（4664 字节）时，Python 脚本逐包收到 ACK 并显示 `4664 / 4664`；开发板随后自动复位，LED 从旧版的 LED0 常亮变为新版两个 LED 每 300ms 闪烁。这证明完整升级链路已打通：

```text
串口头部 → 擦除 App 区 → 分包接收 → 按字写 Flash → ACK → 软件复位 → 新 App 启动
```

观察到 LED 闪烁可能先于终端最后一行进度文字显示。这是主机终端刷新与开发板实际执行并不同步造成的视觉顺序；只有最后一包的 ACK 返回后，脚本才会累计显示为 `4664 / 4664`，因此不会在数据未发送完成前启动新 App。

### 17.16 不要把“故障闪烁”误认为 App 主循环

为调试跳转过程，App 曾在 `Error_Handler()` 和各类 Fault Handler（HardFault、BusFault、UsageFault 等）中加入“PF9/PF10 同时翻转”的临时诊断代码。因此当新 App 的主循环明明设计为 LED0/LED1 交替、实际却看到两个 LED 同时闪烁时，应判断为程序进入了异常处理，而不是主循环。

调试时可将两类信号设计成不同图案，例如：`Error_Handler()` 只闪 LED0，Fault Handler 只闪 LED1。这样可先区分“HAL 配置函数返回错误”与“CPU 异常”；再用 Keil 连接 SWD，在相应处理函数入口下断点，检查调用位置和故障状态寄存器。

## 第 18 课：CRC32——确认写入 Flash 的固件没有损坏

每包 ACK 只能说明 Bootloader 当时收到了这一包、写 Flash 的 HAL 调用没有立即报错；它不能证明整份固件最终内容与电脑原文件完全相同。因此升级完成后，需要对 Flash 中的 App 区再做一次完整 CRC32 校验。

CRC32 是数据的 32 位“指纹”，不是加密或密码。电脑端已经对 `.bin` 文件计算 CRC32 并放入 12 字节固件头的 `header.crc32` 中；Bootloader 写完后从 `APP_BASE` 开始读取 `header.size` 个字节，独立算出 `flash_crc`，只有 `flash_crc == header.crc32` 才允许复位启动新 App。

```text
电脑 .bin → CRC32 → header.crc32
                         ↓
Flash App 区 → CRC32 → flash_crc
                         ↓
                 两者相等才升级成功
```

已定义函数接口：

```c
uint32_t boot_crc32_flash(uint32_t address, uint32_t length);
```

它的职责是从 Flash 的 `address` 起读取 `length` 个字节并返回 CRC32。后续会先使用软件实现，使算法与 Python 的 `zlib.crc32()` 保持一致；不直接使用 STM32 硬件 CRC 外设，是因为硬件 CRC 的输入字节顺序与标准 zlib CRC32 不完全相同，初学阶段容易产生“同一数据 CRC 不同”的困惑。

### 18.1 软件 CRC32 的双层循环

软件 CRC32 逐字节、逐位处理 Flash：外层循环运行 `length` 次，每轮先执行 `crc ^= data[i]`；内层循环运行 8 次，因为一个 `uint8_t` 字节有 8 个 bit。

本项目采用与 Python `zlib.crc32()` 相同的反射 CRC-32 算法。每处理一个 bit，检查 CRC 最低位：

```c
if ((crc & 1UL) != 0UL)
{
    crc = (crc >> 1U) ^ 0xEDB88320UL;
}
else
{
    crc >>= 1U;
}
```

`crc & 1UL` 的结果只可能为 0 或 1，用来判断最低 bit。最低位为 1 时，右移后还要异或常量 `0xEDB88320UL`（CRC-32 多项式的反射表示）；为 0 时只右移。全部字节处理完成后，返回值还需要执行一次 `crc ^ 0xFFFFFFFFUL`。

### 18.2 为什么只看最低位、为什么是异或

CRC 可理解为“二进制长除法的余数”。普通十进制长除法判断当前最高位是否够减除数；本项目的反射 CRC32 从低位到高位处理，所以每轮只判断当前最低位 `crc & 1UL`。右移一位后，原本的下一 bit 自动来到最低位，下一轮继续判断。

### 18.3 `boot_crc32_flash()` 的核心循环

从 Flash 地址读取数据时，可以把地址转换成只读字节指针：

```c
const uint8_t *data = (const uint8_t *)address;
```

外层循环每次把一个字节 `data[i]` 送进当前 CRC；内层循环恰好运行 8 次，模拟这个字节的 8 次二进制多项式除法。反射 CRC32 使用右移，因此多项式常量必须是 `0xEDB88320UL`：

```c
crc ^= data[i];
for (uint32_t bit = 0U; bit < 8U; bit++) {
    if ((crc & 1UL) != 0UL) {
        crc = (crc >> 1U) ^ 0xEDB88320UL;
    } else {
        crc >>= 1U;
    }
}
```

循环结束后不能直接返回 `crc`，还必须做最终异或：`return crc ^ 0xFFFFFFFFUL;`。这样计算结果才与电脑端 Python 的 `zlib.crc32()` 一致。

### 18.4 写入后校验：CRC 是升级的最后一道门

`boot_receive_and_write(header.size)` 成功只表示数据已接收、写入函数没有报错；接着必须对 App Flash 区重新计算 CRC，并和固件头中电脑端给出的 `header.crc32` 比较：

```c
uint32_t flash_crc = boot_crc32_flash(APP_BASE, header.size);

if (flash_crc == header.crc32) {
    /* 允许发送 WRITE DONE，然后软件复位并启动新 App。 */
} else {
    /* 发送或保存 CRC FAIL；绝不能复位进入这份 App。 */
}
```

因此 CRC 不匹配时，Bootloader 应留在自身的升级模式，等待重新传输完整固件；这可以拦住掉包、写入失败、固件内容不是预期版本等问题。

### 18.5 本次实测结果

重新编译并下载带 CRC32 校验的 Bootloader 后，使用 `flash_send.py` 升级 App：串口收到 `ERASE OK`，数据发送完成后收到 `WRITE DONE: REBOOTING`，开发板复位并运行新的 App。说明 STM32 软件 CRC32 与电脑端 Python `zlib.crc32()` 的算法、初值、位方向和最终异或完全匹配。

## 第 19 课：Metadata——应对升级中途掉电

当前工程实际使用的 Flash 布局：

```text
0x08000000 ~ 0x0800FFFF：Bootloader（Sector 0~3，64 KB）
0x08010000 ~ 0x0801FFFF：Metadata（Sector 4，64 KB）
0x08020000 ~ 0x080FFFFF：App（Sector 5~11，896 KB）
```

新模块应与现有 `boot_flash`、`boot_update`、`boot_jump` 一样，放在 `BOOTLOADER/MDK-ARM/Core/`：`boot_metadata.h` 和 `boot_metadata.c`；只有 `.c` 需要加入 Keil 工程的 `Core` 分组。

Flash 写入只能使 bit 从 `1` 变成 `0`，不能直接把 `0` 改回 `1`。因此状态编码必须单向变化：

```c
#define APP_STATE_EMPTY     0xFFFFFFFFUL
#define APP_STATE_UPDATING  0x7FFFFFFFUL
#define APP_STATE_VALID     0x3FFFFFFFUL
```

状态过程为 `EMPTY → UPDATING → VALID`，每一次只额外清零 bit；这让升级完成时可以直接把状态字从 `UPDATING` 写成 `VALID`，无需再次擦除 Sector 4。

### 19.1 Metadata 记录与“开始升级”函数

Metadata 记录放在 `METADATA_BASE (0x08010000)`，使用四个连续的 32 位字：

```c
typedef struct {
    uint32_t magic;
    uint32_t app_size;
    uint32_t app_crc32;
    uint32_t state;
} app_metadata_t;
```

其在 Flash 中的地址布局为：

```text
0x08010000  magic
0x08010004  app_size
0x08010008  app_crc32
0x0801000C  state
```

`boot_metadata_begin_update(app_size, app_crc32)` 的职责：先检查 `app_size` 合法，再解锁 Flash、擦除 **仅 Sector 4**，写入上述四个字（状态先写 `APP_STATE_UPDATING`），最后无论成功或失败都保证 Flash 已上锁。四次 `HAL_FLASH_Program()` 不能都写 `METADATA_BASE`，每个 `uint32_t` 都必须使用自己的 `+ 0U/+ 4U/+ 8U/+ 12U` 地址。

### 19.2 升级完成后标记有效

`boot_metadata_mark_valid(void)` 不擦除 Sector 4，只解锁 Flash，然后把 `METADATA_BASE + 12U` 的 `state` 字写为 `APP_STATE_VALID`。写入函数必须检查 `HAL_FLASH_Program()` 的返回值：失败时上锁并返回 `0`，成功时也要上锁后才返回 `1`。`UPDATING (0x7FFFFFFF) → VALID (0x3FFFFFFF)` 只发生 `1 → 0`，因此不需要擦除。

### 19.3 上电时先读 Metadata

启动流程必须先读取 Sector 4 的 Metadata：若状态为 `UPDATING`，代表上次升级可能中途掉电，绝不能跳转 App；只有记录为 `VALID`，并且后续 App 向量表和 CRC32 均验证通过，才能跳转。

`boot_metadata_read(app_metadata_t *metadata)` 的职责是把 `METADATA_BASE` 处的 16 字节 Flash 记录复制到调用者提供的 RAM 结构体。实现顺序：检查传入指针非空 → 建立指向 Flash 的只读结构体指针 → 复制整份结构体 → 返回成功。

这里有两个指针方向：`temp`（或 `flash_metadata`）应先被赋为 `METADATA_BASE`，使它**指向 Flash 中的记录**；不能对尚未指向任何位置的 `*temp` 赋值。随后复制方向应是“Flash 记录 → RAM 记录”，即把 `*temp` 赋给 `*metadata`，而不是把地址值或 RAM 数据写回 Flash。

### 19.4 判断 Metadata 记录是否可信

`boot_metadata_is_valid(const app_metadata_t *metadata)` 不校验 App 内容，只判断 Sector 4 中的这份“说明书”是否能用。检查顺序遵循由外到内：先确认指针非空，再检查 `magic == METADATA_MAGIC`，再确认 `state == APP_STATE_VALID`，最后确认 `app_size` 为非零且不超过 `APP_MAX_SIZE`。任一项失败立即返回 `0`；全部通过才返回 `1`。

### 19.5 接入 `main.c` 的时机

没有收到 `UPDATE` 命令时，不能直接跳 App。应先把 Flash 的 Metadata 读到 RAM 变量：`boot_metadata_read()` 的实参必须是 `&metadata`，因为函数需要的是 RAM 结构体的地址，不是 `METADATA_BASE` 这个 Flash 地址。读取成功后依次判断：Metadata 有效 → App 向量表有效 → 对 `metadata.app_size` 个 App 字节计算 CRC32，并与 `metadata.app_crc32` 一致；仅全部通过才打印跳转提示并调用跳转函数。

升级路径也必须配合状态：固件头通过合法性检查后、擦除 App 之前写 `UPDATING`；写入后 CRC32 通过时，先写 `VALID`，成功后才宣布完成并复位。

### 19.6 最终的安全启动链

在“未收到 UPDATE”分支中，按顺序执行四道门：`boot_metadata_read(&metadata)` 读取成功 → `boot_metadata_is_valid(&metadata)` 通过 → `boot_app_is_valid()` 通过 → `boot_crc32_flash(APP_BASE, metadata.app_size) == metadata.app_crc32`。只有四项均成立，才提示跳转并调用 `boot_jump_to_app()`；任何一项失败都继续走 `APP INVALID: STAY IN BOOTLOADER`，不执行未知或半写入的 App。

### 19.7 升级主流程中的状态转换

收到合法固件头后，必须先判断 `boot_metadata_begin_update(header.size, header.crc32)` 是否成功；失败则报告 `METADATA FAIL` 并且不能擦除 App。它成功后才擦 App、接收写入、计算 CRC。CRC 不匹配时 Metadata 保持 `UPDATING`，下次启动会拒绝跳转。CRC 匹配后，还要判断 `boot_metadata_mark_valid()` 成功，成功才可以报告 `WRITE DONE` 并复位；若标记失败也报告 `METADATA FAIL`，绝不能复位进新 App。

首次下载加入 Metadata 功能的新 Bootloader 后，Sector 4 仍是空白，因此旧 App 会被启动检查拒绝并提示 `APP INVALID`；这是预期的安全行为。必须通过新版 Bootloader 完成一次串口升级，写入完整 App 和 `VALID` Metadata 后，后续复位才会自动校验并跳转 App。

### 19.8 Metadata 的实测清单

不能只以“升级一次成功”判断实现正确，应分别验证：

1. 正常升级后，串口出现 `WRITE DONE: REBOOTING`，App 自动运行；再次按复位，App 仍直接运行，证明 `VALID` 启动路径正确。
2. 在 Keil 调试的 Memory 窗口查看 `0x08010000`：应依次看到 `magic=0x4D455441`、当前 App 大小、上位机显示的 CRC32、`state=0x3FFFFFFF`。
3. 测试掉电保护：临时在写入 `UPDATING` 成功之后增加一个足够长的等待，趁等待按复位；下次启动必须拒绝跳 App 并提示 `APP INVALID`。测试后删除临时等待并重新下载 Bootloader。
4. 测试 CRC 防线：让上位机故意发送错误的头 CRC（不要改变数据）；Bootloader 必须报告 `CRC FAIL`，不应复位到 App。恢复正确脚本后重新升级即可。

异常测试的“成功标准”不是让半成品 App 继续运行，而是：异常后 Bootloader 安全拒绝启动 App；恢复正确固件传输后，又能正常升级并恢复自动启动。

本次“只发头、不发数据”实测的状态机过程：原先 Sector 4 为 `VALID` → 收到合法头后，`boot_metadata_begin_update()` 擦除 Sector 4 并写入大小、CRC、`UPDATING` → App 擦除后 Bootloader 等待数据 → 上位机故意不发送，接收超时并报 `WRITE FAIL` → 因为未完成 CRC 校验，绝不调用 `boot_metadata_mark_valid()`，因此状态永久保持 `UPDATING`，直到下一次正常升级覆盖它。复位后 `boot_metadata_is_valid()` 只接受 `VALID`，故立即拒绝跳 App。这是软件状态机的掉电保护，不依赖“检测断电”硬件。

工程根目录已提供三份上位机脚本：`flash_send.py` 用于正常升级；`test_incomplete_update.py` 只发送 `UPDATE` 和正确固件头、故意不发送数据，用于验证 `UPDATING` 与中断恢复；`test_crc_fail.py` 发送正确固件数据但将头 CRC 的最低位翻转，用于验证 `CRC FAIL`。两个异常测试都会破坏当前 App 的有效状态；测试后必须再次运行正常升级脚本恢复。

CRC 错误脚本实测：真实 CRC 为 `F6562A5C`，脚本发送的头 CRC 为 `F6562A5D`（仅最低位不同）。4720 字节数据仍全部发送完成，Bootloader 拒绝升级成功状态，说明最终 Flash CRC 比对实际生效。

## 第 20 课：固件版本与防回滚

CRC32 只能回答“内容是否完整”，不能回答“这份固件是否比当前版本旧”。为避免误刷旧程序，升级包头与 Metadata 都需要记录同一个无符号版本号 `app_version`。

升级前应在擦除 Metadata 与 App **之前**读取当前有效 Metadata：若当前版本为 `current_version`，新包版本为 `new_version`，则 `new_version < current_version` 时拒绝升级并报告版本错误；`new_version == current_version` 可保留为允许重刷（便于修复同版本固件），`new_version > current_version` 是正常更新。

新的记录字段建议为：`magic`、`app_size`、`app_crc32`、`app_version`、`state`。`state` 仍放最后，因此会从原偏移 `+12U` 移至 `+16U`。更改结构体后，首次升级会重新擦 Sector 4 并写入新格式的 Metadata；旧格式记录自然不再有效，这是预期行为。

实际写入布局必须同步为：`+0U magic`、`+4U app_size`、`+8U app_crc32`、`+12U app_version`、`+16U state`。`boot_metadata_mark_valid()` 也必须改为写 `METADATA_BASE + 16U`；否则会覆盖版本字段而状态仍保持 `UPDATING`。

版本字段的基本规则：升级头的 `version` 必须非零；因此 `boot_header_is_valid()` 在原有 magic 与大小检查基础上增加 `header->version != 0U`。同样，启动时验证 Metadata 时也应要求 `app_version != 0U`，以拒绝旧的四字段记录或未写完的记录。

### 20.1 防回滚判断的正确位置与方向

版本比较发生在 `boot_header_is_valid()` 成功后、`boot_metadata_begin_update()` 擦除任何 Flash 之前。必须先确认旧 Metadata 有效；只有“旧记录有效 **且** `header.version < metadata.app_version`”时才拒绝，并报告 `VERSION REJECT`。不能用 `header.version > metadata.app_version` 作为允许条件：空白或无效 Metadata 的版本字段可能是 `0xFFFFFFFF`，会错误阻止首次安装；而同版本重刷也会被误拒绝。旧 Metadata 无效时跳过比较，继续允许升级恢复。

### 20.2 控制嵌套：用函数与提前返回

把所有升级步骤直接堆在 `main()` 中会产生很多层 `if`，逻辑正确但难维护。后续重构原则是：将“处理一次升级”与“验证并启动 App”拆成独立函数；函数内每项失败立刻 `return` 一个结果，不再向右嵌套。这样 `main()` 只负责两条分支：收到 UPDATE 则处理升级，否则尝试启动 App；新增错误或步骤只修改相应函数。

当前 `main.c` 的升级路径已采用连续 `else if`，相比多层嵌套已经清晰，可暂时不重构。更紧急的是协议同步：`firmware_header_t` 已由 12 字节扩为 16 字节（新增 `version`），所以 Python 必须从 `struct.pack('<III', ...)` 改为发送四个 32 位字段，并定义非零 `FW_VERSION`。否则 Bootloader 会把 App 数据的前 4 字节错当成版本，造成后续接收数据整体错位。

版本 1 实测：Sector 4 从 `0x08010000` 读取到 `41 54 45 4D`（小端 `METADATA_MAGIC`）、`70 12 00 00`（4720 字节）、`5C 2A 56 F6`（CRC `F6562A5C`）、`01 00 00 00`（版本 1）、`FF FF FF 3F`（小端 `APP_STATE_VALID`）。五个字段位置和内容均正确。

版本管理实测：版本 2 在当前版本 1 的基础上允许升级；随后尝试发送版本 1 时，Bootloader 在擦除前报告 `VERSION FAIL`，证明回滚被拦截且原有版本 2 App 未被破坏。正常发送脚本已恢复为 `FW_VERSION = 2`。

### 20.3 上位机版本号参数化

把版本号固定写在 Python 源码中容易忘记修改。更实用的方式是让命令行传版本：例如 `python .\\flash_send.py 3`。脚本应保留一个默认版本（当前为 2），未传参数时仍可直接运行；传入的版本需要转换为整数并验证大于 0，随后用于固件头的第 4 个 `uint32_t` 字段。

实测命令行版本 3 升级成功，Metadata 的版本字段为 3。正常发送脚本的默认 `FW_VERSION` 已同步更新为 3；两个异常测试脚本也同步改为发送 16 字节版本化固件头，避免与新版 Bootloader 协议不一致。

## 第 21 课：分包协议——识别漏包、重复包与错位

当前数据阶段的协议是“发送最多 256 字节 → Bootloader 写 Flash → 返回 `OK`”。它能处理普通超时，但每包本身没有身份；如果 ACK 丢失而上位机重发，或数据流错位，Bootloader 无法知道当前数据属于第几包。

改进方向是在每个数据包前增加小包头：`sequence`（包序号）、`length`（本包数据长度）、`crc32`（本包数据 CRC32），后接 `length` 字节数据。Bootloader 只接受期待的 `sequence` 和合法长度、CRC；成功写入后回复 ACK 与序号。先完成协议设计，再改收发代码。

### 21.1 分包头结构

在 `boot_update.h` 中新增 `firmware_chunk_header_t`，包含三个 `uint32_t`：`sequence`、`length`、`crc32`，所以固定大小为 12 字节。数据 `data` 不放进该结构体，因为它的长度随最后一包而变化；协议顺序始终是“先收固定 12 字节分包头，再按 `length` 收数据”。

### 21.2 Flash CRC 与 RAM CRC

`boot_crc32_flash(address, length)` 的输入是 Flash 地址；分包校验时数据刚由串口接收到 RAM 缓冲区，需要一个新函数 `boot_crc32_buffer(const uint8_t *data, uint32_t length)`。它的双层 CRC 循环与 Flash 版相同，只是从 `data[i]` 读取。后续可让 Flash 版复用它：将 Flash 地址转换为 `const uint8_t *` 后调用通用缓冲区 CRC 函数，避免维护两份算法。

`boot_crc32_buffer()` 的实现先拒绝空指针，然后以 `0xFFFFFFFFUL` 初始化 CRC，逐字节和逐 bit 执行反射 CRC32，最后异或 `0xFFFFFFFFUL` 返回。长度为 0 时循环不运行、返回 0，这也是标准 CRC32 对空数据的结果。

### 21.3 再理解一次软件 CRC32 算法

CRC 是在 GF(2) 上做的二进制多项式长除法：没有进位和借位，减法就是异或。代码使用反射 CRC32，数据从每个字节的低 bit 向高 bit 处理，因此每轮检查 `crc & 1UL` 并右移；若最低 bit 为 1，就说明本轮需要异或反射多项式 `0xEDB88320UL`，若为 0 则只右移。一个字节恰有 8 bit，所以内层循环固定执行 8 次。

外层的 `crc ^= data[i]` 并不是得出最终结果，而是把新字节送进当前“除法余数”；上一个字节得到的 `crc` 不会清零，会继续影响下一个字节，因而整段数据任何一处变化最终都可能得到完全不同的 CRC。初始值和最终 `^ 0xFFFFFFFFUL` 是标准 zlib CRC32 参数，用于与 Python `zlib.crc32()` 一致。

`^` 是 C 的按位异或（XOR），不是乘方：两个对应 bit 相同结果为 0，不同结果为 1。`a ^ b` 产生新值；`a ^= b` 是简写，等价于 `a = a ^ b`。CRC 用异或模拟 GF(2) 多项式除法中的减法，因为其没有进位或借位。

`&` 是按位与（AND），不是 XOR 的相反：只有两个对应 bit 都为 1，结果才为 1。`crc & 1UL` 利用掩码 `1UL` 保留 CRC 的最低 bit，用于判断本轮是否需要异或多项式。要区分按位与 `&` 和逻辑与 `&&`：前者计算每一位，后者判断两个条件是否同时为真。

CRC 的每一位处理步骤是：`if ((crc & 1UL) != 0UL)` 先判断最低 bit；为 1 时执行 `(crc >> 1U) ^ CRC32_POLY`，即右移并异或多项式，模拟一次需要“减除数”的多项式长除法；为 0 时只执行 `crc >>= 1U`。`crc >>= 1U` 等价于 `crc = crc >> 1U`。二者不是两个独立算法，而是同一轮除法根据当前最低 bit 选择的两条路径。

右移的目的，是让原来倒数第二低的 bit 成为新的最低 bit，供下一轮 `crc & 1UL` 判断；被判断过的原最低 bit 已完成本轮处理。当前实现采用低位优先的反射 CRC32，故使用右移和反射多项式 `0xEDB88320UL`。另一种高位优先写法会检查最高位、左移，并配合 `0x04C11DB7UL`；两种写法不能混搭。

最终 `crc` 不等于 `data + CRC32_POLY`。`data` 是被除的数据，`CRC32_POLY` 是固定除数，`crc` 是随每个 bit 更新的余数状态；`crc ^= data[i]` 只是把一个输入字节送入该状态，条件异或多项式则是长除法的步骤。最终返回的 CRC32 是整段数据按标准参数除以该多项式后的余数形式。

#### 玩具 CRC 长除法示例

为理解角色，可用 4 bit 的玩具除数 `1011`（非真实 CRC32）去处理数据 `1101000`。每次当前 4 bit 的首位为 1，就与固定除数异或：`1101 ^ 1011 = 0110`，带入下一 bit 得 `1100`；`1100 ^ 1011 = 0111`，带入下一 bit 得 `1110`；`1110 ^ 1011 = 0101`，最后余数为低 3 bit `101`。全过程中 `1011` 始终不变，是除数；输入 `1101000` 是数据；不断得到的 `0110/0111/0101` 是中间余数状态，最终 `101` 才是校验值。真实代码用 32 bit、反射方向和标准初末异或，但角色关系完全一样。

注意：`1101 - 1011` 的普通二进制减法确实等于 `0010`；但 CRC 在 GF(2) 多项式中运算，没有借位，所谓“减法”是 XOR，因此 `1101 ^ 1011 = 0110`。逐位算为 `1^1=0、1^0=1、0^1=1、1^1=0`。

玩具例子为了易读按高位优先展示“带下一个 bit”；实际项目使用反射 CRC32，方向相反。`crc ^= data[i]` 中 `data[i]` 是 8 位数，提升为 32 位时高 24 位为 0，因此它只与 `crc` 的低 8 位异或；随后内层循环右移 8 次，依次处理这个字节的最低 bit 到最高 bit。两者都是“将新数据送入余数”的同一个概念，只是处理方向不同。

### 21.4 接收与验证分包头

`boot_receive_chunk_header(firmware_chunk_header_t *header)` 只接收固定的 12 字节分包头，使用 `sizeof(*header)`，成功返回 1、串口超时或错误返回 0。实现还应先检查 `header != NULL`。接收成功不表示该包可写入；下一步独立验证 `sequence` 是否等于当前期待序号、`length` 是否在 `1..FW_CHUNK_SIZE`，之后收数据并用分包 CRC 验证内容。

`boot_chunk_header_is_valid(const firmware_chunk_header_t *header, uint32_t expected_sequence)` 已实现四项拒绝条件：空指针、序号不是期待序号、长度为 0、长度超过 `FW_CHUNK_SIZE`。`boot_update.c` 包含 `boot_flash.h` 是合理的，因为 `FW_CHUNK_SIZE` 定义在其中。

### 21.5 把分包协议接入写 Flash 循环

当前 `boot_receive_and_write()` 已有局部 `received` 和 `chunk_size`。改造时新增局部 `expected_sequence = 0U` 和 `firmware_chunk_header_t chunk_header`，并在 `boot_flash.c` 中包含 `boot_update.h`。每轮保留“根据 `total_size - received` 算 `chunk_size`”，然后依次：接收 `chunk_header` → 调用分包头验证（传 `expected_sequence`）→ 再要求 `chunk_header.length == chunk_size`（严格限制最后一包长度）→ 接收 `chunk_header.length` 字节到 buffer → 比较 `boot_crc32_buffer(buffer, chunk_header.length)` 与 `chunk_header.crc32` → 写 Flash → `received += chunk_header.length` 与 `expected_sequence++` → 发送 ACK。任意一步失败立刻返回 0，绝不能增加 received 或序号。

常见混淆：分包**头**自身固定为 12 字节（序号、长度、CRC）；`chunk_header.length` 描述的是后面数据本体的长度，最大是 `FW_CHUNK_SIZE`（当前 256），不是 12。接收分包头失败时应立即返回；只有接收成功后才读取并验证其中的字段。长度与本轮预期长度不相等时才应失败，即使用 `!= chunk_size`。

上位机每包发送必须同步为：先计算 `chunk_crc = zlib.crc32(chunk) & 0xFFFFFFFF`，发送 `struct.pack('<III', sequence, len(chunk), chunk_crc)`，再发送 `chunk` 本体；收到 `OK` 后才增加 `sequence` 和发送偏移。总固件头仍是 `'<IIII'`，不要与单个分包头混淆。

分包协议实测成功：版本 3、4720 字节固件发送完成并收到 `WRITE DONE: REBOOTING`。这说明 16 字节总头、每包 12 字节分包头、256 字节以内数据、分包 CRC32、序号递增、Flash 写入和最终整包 CRC32 已全部协同工作。

### 21.6 单包 CRC 错误测试

`test_chunk_crc_fail.py` 发送正确的 16 字节总头和第 0 包数据，但故意将第 0 包头的 `crc32` 最低 bit 翻转。预期 Bootloader 在第 0 包写 Flash 前停止并报告 `WRITE FAIL`；这验证分包 CRC 防线，而不是之前的整包 CRC 防线。测试会使 Metadata 保持 `UPDATING`，结束后需运行正常 `flash_send.py` 恢复。

### 21.7 分包序号错误测试

`test_sequence_fail.py` 的总头、数据长度、数据内容和分包 CRC 都正确，仅将第一个分包的 `sequence` 从应有的 0 故意写成 1。Bootloader 的 `expected_sequence` 初值为 0，因而分包头验证应失败并报告 `WRITE FAIL`，证明序号检查在写 Flash 前生效。测试后同样需运行正常升级脚本恢复。

分包序号错误实测通过：第一个包发送 sequence=1 时，Bootloader 在写 Flash 前拒绝并报告 `WRITE FAIL`。至此已分别验证正常分包升级、单包 CRC 错误拦截、分包序号错误拦截。

## 第 22 课：ACK/NACK 与单包重传

当前分包协议发现包错误后立即终止整次升级。下一步目标是局部恢复：Bootloader 对每个包回复“确认”或“否认”，上位机只重发出错包。语义约定为 `ACK sequence`：该序号已校验并写入 Flash；`NACK sequence`：Bootloader 当前仍期待该序号，请重新发送。每个包设置最大尝试次数（例如 3）；超出才终止整次升级。

关键原则：`expected_sequence` 和 `received` 仅能在一包通过 CRC、成功写 Flash 后前进。包头或分包 CRC 失败时，不改变两者，回复 `NACK expected_sequence`，回到循环开头等待重发。

### 22.1 固定长度 ACK/NACK 帧

为避免文本解析、换行和乱码问题，回复采用固定 5 字节二进制帧：第 0 字节为状态码，`0x06U` 是 ACK、`0x15U` 是 NACK；后 4 字节是 `uint32_t sequence` 的 STM32 小端内存表示。例如 `06 05 00 00 00` 表示第 5 包已确认。协议常量及 `boot_send_chunk_reply(status, sequence)` 的声明放入 `boot_update.h`，发送实现放入 `boot_update.c`。

实现回复帧时，非法状态判断必须是“既不是 ACK 且也不是 NACK”，即使用 `&&`，不能用 `||`。拆序号字节时，先右移再转为 `uint8_t`：`(uint8_t)(sequence >> shift)`；若先转 `uint8_t`，高 24 位会先丢失。固定回复帧是 5 个有效字节，没有 C 字符串终止符，因此发送长度必须是 `sizeof(reply)`，不能减 1；成功发送后返回 1。

`boot_send_chunk_reply()` 已实现正确：ACK/NACK 状态合法性检查、5 字节数组、序号小端拆分、完整 5 字节 UART 发送，以及成功返回 1。接下来在分包接收循环中，成功包改为发送 `ACK expected_sequence`（在递增序号前），失败包发送 `NACK expected_sequence` 并保持进度不变；还须为反复失败设置重试上限。

重传不能只在旧循环中把 `return 0` 改成继续：当前 Python 是包头和数据连续发送，若 Bootloader 在包头阶段发现序号或长度错误，数据本体仍会留在 UART 缓冲区，下一轮读包头会发生帧错位。可靠重传需要两阶段握手：先发分包头，Bootloader 验证序号/长度后回复“可以发数据”或拒绝；只有收到允许回复，上位机才发数据；数据 CRC 或 Flash 写入失败时再回复 NACK，让上位机重发同一包。

三种回复状态含义：`READY (0x16U)` 只表示分包头合法、允许上位机发送本包数据；`ACK (0x06U)` 表示数据 CRC 和 Flash 写入都成功；`NACK (0x15U)` 表示本包失败、仍期待同一序号。READY 不是写入成功确认，不能推进 `received` 或 `expected_sequence`。

两阶段分包握手实测成功：上位机发送分包头后收到 READY，再发送数据并收到 ACK，完整固件升级成功。说明总头版本协议、分包头、序号、分包 CRC、READY/ACK 回复和 Flash 写入流程已经同步。

正常发送脚本已改为两阶段握手：每包先发送 12 字节分包头并等待带序号的 READY，再发送数据并等待带序号的 ACK；未收到正确回复时同一包最多重试 3 次。脚本读取 5 字节二进制回复，并忽略文本提示残留的非状态字节。

Bootloader 端对应的重试循环：每个包单独把 `retry_count` 清零；包头接收、序号/长度验证、数据接收、分包 CRC 或 Flash 写入任意失败，都发送 `NACK expected_sequence` 并重试；包头通过后先发送 `READY`，只有数据 CRC 与写入都成功才发送 `ACK`，随后才增加 `received` 和 `expected_sequence`。达到 `FW_CHUNK_MAX_RETRIES` 仍失败才结束整次升级。

`retry_count++` 记录的是“当前包本次尝试失败”，不是修改固件进度。失败后 `continue` 只会回到内层重试循环的开头，重新等待同一个 `expected_sequence`；`received` 和 `expected_sequence` 保持不变。若不增加计数，持续损坏或断线时会无限循环；成功时不增加失败次数，并在写入后才推进进度。

学习约定：代码由学习者自行编写；教学仅说明概念、实现顺序和检查结果，不直接提供可复制的完整实现。

### 22.2 上位机与下位机

上位机通常指电脑端的软件，资源多、负责发起命令和组织数据；本项目中的 `flash_send.py` 就是上位机，它读取 `.bin`、计算 CRC、填写固件头和分包头、发送数据并解析 Bootloader 回复。下位机通常指被控制的设备，本项目的 STM32F407 板及其 Bootloader 就是下位机，负责接收、验证、擦写 Flash、回复 ACK/NACK 并启动 App。

“上/下”不是物理位置，而是通信中的控制关系：上位机发起任务，下位机执行任务并反馈结果。串口线是通信通道；`UPDATE`、固件头、分包头和数据是上位机发给下位机的协议帧；`READY`、`ACK`、`NACK` 是下位机发回上位机的反馈帧。
### 当前代码检查：READY 状态同步

检查当前代码发现一个协议阻塞点：`boot_flash.c` 会调用 `boot_send_chunk_reply(BOOT_REPLY_READY, ...)`，但 `boot_update.c` 中该函数的合法性判断目前只允许 `BOOT_REPLY_ACK` 和 `BOOT_REPLY_NACK`，因此 READY 会被函数直接拒绝发送，Python 会一直等不到 READY。新增任何回复状态后，发送函数的合法状态白名单必须同步扩展。
### 当前代码检查（2026-08-28）

当前 `boot_send_chunk_reply()` 已允许 ACK、NACK、READY 三种状态，和 `boot_receive_and_write()` 及 Python 的两阶段握手一致；`FW_CHUNK_MAX_RETRIES` 也已定义为 3。`boot_flash.c` 顶部仍有旧的 `static const uint8_t ack[] = "OK\\r\\n"`，但新版循环已不再使用（2026-08-29 核对：该变量已删除，本项关闭）。另一个可靠性注意点是：如果 `HAL_UART_Receive()` 超时后只收到部分数据，残留字节可能让下一次重试的包头错位；当前先完成正常流程测试，之后可通过“清空接收缓冲/帧同步魔数”进一步增强。
## 第 23 课：帧同步——当字节流的边界丢失时

> 状态（2026-08-29）：**已实现并全量验证通过**。过程记录：① 死锁实验——旧上位机发旧格式包头（第 0 包 12 字节里连 AA/55 都没有），Bootloader 每次扫满 5 秒窗口后回 NACK，3 次重试后有序失败，与推演完全一致；② 上位机收编 `send_chunk_header()` 为分包头唯一出口（grep 验证 `<III` 格式全项目仅此一处）；③ `test_crc_fail.py` 属于最老版协议（裸发数据等文本 OK），顺带修成两阶段握手；④ `test_frame_sync.py` 三场景（垃圾前缀 / 假锁定自愈 / 半包超时）+ `flash_send.py`、`test_retry.py` 回归全部通过，WRITE DONE 收尾。
>
> 本课踩坑：首版 `boot_receive_chunk_header()` 把出口写反了（`== HAL_OK` 时返回 0），包头明明收到却报失败，协议一包都过不去。教训：**函数的每个 return 出口都要人工走一遍**，且必须先跑 happy path 再加异常注入。

### 23.1 错位事故的字节级推演

现协议有一个隐含假设：每次 `HAL_UART_Receive()` 都恰好消费整数个包。打破它的场景：

1. 上位机发 12 字节分包头，只到 7 字节，接收超时返回——**7 字节已被消费**；
2. Bootloader 回 NACK，上位机重发完整 12 字节；
3. Bootloader 再读 12 字节 = 旧包头尾巴 5 字节 + 新包头开头 7 字节 = 一个垃圾包头；
4. sequence 不匹配 → NACK → 重发 → 偏移仍是 7 字节……

**只要字节流偏移不是 12 的倍数，错位永远无法自愈**，直到 `FW_CHUNK_MAX_RETRIES` 耗尽，整次升级失败。串口线插拔、打开串口时的杂字节、复位中途进入升级，都可能触发。

帧同步的本质：给接收方一个**主动重获边界**的手段，而不是指望字节流自己变干净。

### 23.2 设计决策三问

**Q1：同步字为什么是 AA 55？两个字节能防住假锁定吗？**
AA = 10101010、55 = 01010101，位特征鲜明，线路噪声凑出该序列的概率低。但两个字节**防不住假锁定**——数据区里完全可能出现 AA 55。真正的防线是组合拳：

> 同步字负责"边界可能在哪"，包头校验负责"边界对不对"。

假锁定后 Bootloader 读 12 字节垃圾，sequence 恰好等于期待值**且** CRC32 恰好自洽的概率约等于零 → 校验拒绝 → 继续扫描。**假锁定无害、可自愈**——这句话是本课的灵魂。

**Q2：扫描状态机怎么写？**
与 `boot_wait_update_command()` 同款手法（字符流匹配状态机 → 字节流边界恢复，同一思想第二次应用）：状态 0 = 找 AA；状态 1 = AA 已见、找 55。细节：状态 1 时又收到 AA 应**保持状态 1**（`AA AA 55` 也是合法锁定）；收到其他字节回状态 0。

**Q3：扫描窗口多长？**
NACK 后上位机立即重发，窗口覆盖重传到达时间即可，取 5 秒（兼顾人为操作）。窗口内单字节 10ms 超时轮询，总窗口用 `HAL_GetTick()` 控制——时间结构照抄 `boot_wait_update_command()`。

### 23.3 包格式变化（只动分包头）

```text
原来： sequence(4) | length(4) | crc32(4) | data...
现在： AA 55 | sequence(4) | length(4) | crc32(4) | data...
```

UPDATE 命令与 16 字节固件头**不改**——那条路有 "WAIT UPDATE" 文本锚点，不存在错位问题。

### 23.4 Bootloader 端：替换 `boot_receive_chunk_header()`

```c
#define CHUNK_SYNC0           0xAAU
#define CHUNK_SYNC1           0x55U
#define SYNC_SCAN_TIMEOUT_MS  5000U

/* 先扫描同步字 AA 55 重获边界，锁定后再收 12 字节分包头 */
int boot_receive_chunk_header(firmware_chunk_header_t *header)
{
    uint8_t  byte;
    uint32_t sync_stage = 0U;          /* 0=找AA, 1=AA已见找55 */
    uint32_t start_tick = HAL_GetTick();
    uint8_t  locked = 0U;

    if (header == NULL)
    {
        return 0;
    }

    while ((HAL_GetTick() - start_tick) < SYNC_SCAN_TIMEOUT_MS)
    {
        if (HAL_UART_Receive(&huart1, &byte, 1U, 10U) != HAL_OK)
        {
            continue;                  /* 窗口内暂时没字节，继续扫 */
        }
        if (sync_stage == 0U)
        {
            if (byte == CHUNK_SYNC0) sync_stage = 1U;
        }
        else if (byte == CHUNK_SYNC1)
        {
            locked = 1U;
            break;
        }
        else
        {
            sync_stage = (byte == CHUNK_SYNC0) ? 1U : 0U;
        }
    }

    if (!locked)
    {
        return 0;                      /* 5 秒没等到同步字，上层 NACK 重试 */
    }
    if (HAL_UART_Receive(&huart1, (uint8_t *)header, sizeof(*header), 1000U) != HAL_OK)
    {
        return 0;                      /* 锁定后包头没到齐，同样交给 NACK */
    }
    return 1;
}
```

`boot_chunk_header_is_valid()` **一行不改**：sequence/length/CRC 校验照旧，同步层只负责回答"包头从哪开始"。

### 23.5 上位机端与 DRY 重构

```python
CHUNK_SYNC = b'\xAA\x55'
ser.write(CHUNK_SYNC + struct.pack('<III', sequence, len(chunk), chunk_crc))
```

协议格式目前散落在 `flash_send.py` 与 4 个 `test_*.py` 各自手写的 `struct.pack('<III', ...)` 里——本次改动必须全改，正好暴露了这个坏味道。重构：`flash_send.py` 新增统一出口：

```python
CHUNK_SYNC = b'\xAA\x55'

def send_chunk_header(ser, sequence, length, crc32_val):
    """分包头唯一出口：同步字 + 12 字节。所有脚本统一调用。"""
    ser.write(CHUNK_SYNC + struct.pack('<III', sequence, length, crc32_val))
```

测试脚本全部改为 import 它。**协议格式永远只写一处**——这个习惯适用于以后任何协议开发。

### 23.6 验收测试（新建 test_frame_sync.py）

1. **垃圾前缀**：发 30 个随机字节 + 同步字 + 包头 → 应正常 READY → ACK；
2. **假锁定自愈**：发真实固件数据（天然可能含 AA 55），制造一次 NACK，重发完整包 → 应恢复成功；
3. **半包超时**：包头 → READY → 只发 50/256 字节 → 等 NACK → 重发 → 成功，且整次升级最终走到 WRITE DONE。

### 23.7 本课必须记住

- 同步字解决"边界在哪"，包头校验解决"边界对不对"——**缺一不可**；
- 假锁定无害的前提是包头有强校验（sequence 匹配 + CRC32 自洽）；
- 字节流协议永远不要假设"上一次读取消费干净了"——串口、网络协议的通则；
- 协议格式（同步字、包头布局）在代码里只允许出现一处。

---

## 后续学习目标：帧同步与可靠升级扩展

下一阶段首先实现**帧同步与错位恢复**（课程内容见第 23 课，代码待实现）。问题是 UART 可能只收到半个包就超时，遗留字节会使下一次重传时的分包头错位。解决方案是在每个分包前增加固定同步字（建议两个字节 `0xAA 0x55`）：

```text
AA 55 + sequence + length + chunk_crc32 + data
```

Bootloader 先从串口字节流中搜索 `AA 55`，找到后才接收并解释后续 12 字节分包头；如果中途丢字节或数据损坏，可继续扫描下一次 `AA 55`，重新获得包边界。学习顺序：定义同步字与新包格式 → 上位机发送同步字 → 下位机扫描同步字 → 修改重传流程 → 制造半包超时/错位测试。

帧同步完成后可继续扩展：

1. A/B 双 App 分区：下载新固件到备用区，验证后再切换，进一步提升掉电安全。
2. 固件签名校验：确认固件来自可信发布者，而不仅是“数据未损坏”。
3. 加密传输：保护固件内容不被直接读取。
4. CAN、USB 或其他通信接口升级：复用相同升级状态机与 Metadata 设计。
