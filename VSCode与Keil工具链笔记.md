# VS Code + Keil 工具链笔记（配置与踩坑）

> 针对：Windows + VS Code + Keil MDK 5.42（STM32F407 探索者板）
> 配套项目：`G:\Boot`（stm32f4_Bootloader）
> 更新日期：2026-08-29
>
> **一句话工作流**：VS Code 负责写代码、Git、一键编译烧录、看串口；Keil 负责真正
> 的编译器后端（armclang）和断点调试。Keil 不退休，只是退居二线。

---

## 一、本机配置备忘（重装系统/换电脑后照着重配）

### 扩展清单

| 扩展 | 作用 |
|---|---|
| `cl.keil-assistant`（Keil Assistant） | 导入 .uvprojx，一键编译/烧录，自动生成 IntelliSense 配置 |
| `ms-vscode.cpptools`（C/C++） | 智能提示、红线检查 |
| `mhutchie.git-graph`（Git Graph） | 左下角状态栏 → Git 图谱，可视化提交历史 |
| `ms-vscode.vscode-serial-monitor` | VS Code 内看串口输出（替代串口助手） |
| `ms-ceintl.vscode-language-pack-zh-hans` | 中文界面 |

### 关键路径（本机有**两个 Keil**，认准有授权的那个）

```text
正确（注册表登记、有许可证）：D:\Software Download\Keil5\UV4\UV4.exe
错误（无许可证，会报 R207）：  D:\STM\KEIL STM\UV4\UV4.exe
```

- Keil Assistant 设置里的 UV4 路径 → 填上面"正确"的那个
- 项目导入：KEIL UVISION PROJECT 面板 → ＋ → 选 `.uvprojx` 文件（BOOTLOADER 和 APP 两个工程分别加）

### `.vscode` 目录里三个文件的分工

| 文件 | 作用 | 进仓库？ |
|---|---|---|
| `c_cpp_properties.json` | IntelliSense 的头文件路径 + 宏定义 | ✅ 提交 |
| `settings.json` | `"C_Cpp.intelliSenseEngine": "Tag Parser"`（踩坑 3 的解药） | ✅ 提交 |
| `uv4*.log`、`*.lock`、`keil-assistant.log` | 编译/运行日志 | ❌ 已被 .gitignore 排除 |

---

## 二、踩坑记录

### 坑 1：命令行编译报 R207(3) REGISTRY READ ERROR（24 个错误全是它）

**现象**：Keil IDE 里编译正常，Keil Assistant/命令行调 `UV4.exe -b` 编译报
`armclang: error: Failed to check out a license. LICENSE ERROR (R207(3))`。

**根因**：本机装了两个 Keil，配错了 UV4 路径——指向了没有许可证的那个安装。

**排查口诀**：别看目录里有什么文件，**查注册表登记的是哪个**：

```bash
reg query "HKLM\SOFTWARE\Wow6432Node\Keil\Products\MDK" /v Path
```

输出的 Path 指向哪个安装，Keil Assistant 就配哪个 UV4.exe。
（教训：我当时以"F4 器件包在哪个目录"当判据，被带偏了——**环境问题别猜，用实测说话**：
拿两个 UV4 各跑一次 `-b`，看哪个退出码是 0。）

### 坑 2：Keil Assistant 生成的 IntelliSense 配置不生效（头文件全标红）

**现象**：`#include` 全部红线，连 `<string.h>` 都红；但真实编译 0 错误。

**根因**：插件把 `c_cpp_properties.json` 生成在工程子目录（`MDK-ARM\.vscode\`），
而 **VS Code 只读工作区根目录** `G:\Boot\.vscode\` 下的同名文件。

**解法**：在根目录 `.vscode\c_cpp_properties.json` 手写配置，要点：

- `compilerPath` 指向**有授权那个** Keil 的 armclang（标准库路径随之解决）；
- `includePath` 覆盖两个工程的全部头文件目录；
- `defines` 与 Keil 工程一致：`USE_HAL_DRIVER`、`STM32F407xx`。

**概念记住**：编译器的路径账本在 `.uvprojx`，编辑器的账本在 `c_cpp_properties.json`，
**两本账独立维护**。编译报错查前者，红线查后者。

### 坑 3：`uint32_t` 不是类型名 / flash_crc 未定义（连锁 6 个假错误）

**现象**：代码 100% 合法（armclang 0 错误），但 IntelliSense 报
`应输入";"`、`未定义标识符 "flash_crc"`、`变量 "uint32_t" 不是类型名`——
且换任何 IntelliSenseMode 都一样。

**根因**：Keil 的 `stdint.h` 用 Arm 特有的条件守卫（`#ifndef __STDINT_DECLS`
嵌套 `#if defined(__clang__)...`）包裹 typedef；cpptools 的严格解析器处理这条
编译器特定的条件链时把 `uint32_t` 的 typedef 弄丢了，后续所有用它声明的变量
连锁报错。

**解法**：工作区 `settings.json` 写 `"C_Cpp.intelliSenseEngine": "Tag Parser"`
（宽容引擎，靠符号数据库解析），重启窗口后 6 条全消。
**代价**：代码补全/跳转精度略降。红线零误报对当前阶段更重要，真遇到再调。

**通用教训**：`uint32_t` 这类系统类型被标红时，错误会**记在变量名头上**而不是
类型名上——看到变量名标红，先怀疑上游某个头文件没解析对。

### 坑 4：Keil 与 VS Code 双开同一工程 → 缓冲区覆盖

**现象**：两个编辑器同时打开 main.c；Keil **不会自动刷新**外部修改。

**风险**：在 VS Code 改完代码，切回 Keil（它缓冲区还是旧内容）随手 Ctrl+S，
**VS Code 里的修改被旧版静默覆盖**。

**规则**：改代码只在 VS Code；Keil 只负责调试和烧录。
切回 Keil 弹"文件已被外部修改，是否重新加载"→ **永远点是**。

### 坑 5（附）：串口监视器的 DTR/RTS

探索者板的 CH340 一键下载电路：DTR/RTS 被拉高会把 MCU 按在复位里。
VS Code 串口监视器连上后，**把 DTR/RTS 开关都关掉**（和 Python 脚本里
`ser.dtr = False` 同一个坑）。Python 脚本已内置，手动工具要记得关。

---

## 三、日常操作速查

| 操作 | 入口 |
|---|---|
| 编译 | Keil Assistant 面板 → 工程条目 → 锤子图标 |
| 烧录 | 同上 → 下载图标（板子 + ST-Link 先接好） |
| 看编译输出/错误跳转 | 底部终端面板，报错行号可点击 |
| 看串口日志 | `Ctrl+Shift+P` → Serial Monitor: Create → COM18 / 115200（关 DTR/RTS） |
| 看提交历史 | 左下角 "Git Graph" |
| 暂存/提交/推送 | 源代码管理面板（第三个图标） |

**标准顺序**：写代码 → 编译（0 Error 才继续）→ 烧录 → 串口验证 → git 提交推送。

---

## 四、调试（进阶，未配置）

目前断点调试仍在 Keil 里做（Load + Debug 按钮）。若想在 VS Code 里断点调试，
路线是 `cortex-debug` 插件 + OpenOCD/J-Link GDB Server，Keil 的 .axf 可以直接
被 GDB 调试器加载。暂不折腾，留作后续课题。
