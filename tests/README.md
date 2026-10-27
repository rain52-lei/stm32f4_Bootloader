# 本地验证与真机验收

所有产物写入 `G:\Boot\.build`，不需要下载或安装依赖，也不会自动烧录开发板。

## Python 协议回归

在项目根目录执行：

```powershell
python -m unittest discover -s tests -p test_host_protocol.py -v
```

检查正常包、NACK 重传、中间/末包 ACK 丢失、旧回复过滤、重试耗尽、回复短读、完整 GOTOBOOT 横幅解析、镜像槽位和范围、TCP 缓存及断连。

## 运行真实 C 实现的本机测试

```powershell
.\tests\run_native.ps1
```

使用现有的 `G:\Lvgl\lvgl\mingw64\bin\gcc.exe`。直接编译项目的 `boot_flash.c`、`boot_update.c` 和 `boot_metadata.c`，以模拟 HAL 和内存映射 Flash 执行。覆盖 CRC 标准向量、槽边界、补齐写入、重复包不重复编程、回复/编程失败、连续会话、陈旧 RAM 拒绝、单副本恢复和 generation 回绕。

这不能代替真机掉电、Flash 磨损、串口电气和 Cortex-M 跳转测试。

## 完整 ARM 构建

```powershell
python .\tools\build_firmware.py
```

使用现有 Keil Arm Compiler 6.23，按工程文件的源文件、头路径和定义编译。输出：

- `.build\BOOTLoader\BOOTLoader.axf`
- `.build\BOOTLoader\BOOTLoader.bin`
- `.build\BOOTLoader\BOOTLoader.map`

编译按警告视为错误处理。map 中 Bootloader 最大链接区域必须为 `0xC000`，避免进入 Metadata 副本0的 Sector3。

## 真机新增测试

先通过 ST-LINK/Keil 下载新的 Bootloader，再使用当前发送脚本；Metadata v3 和20字节固件头布局不变。下面示例的槽位要与实际横幅 DOWNLOAD 匹配，版本不得低于当前固件版本：

```powershell
python .\test_ack_loss.py --slot B --version 12 --port COM19
python .\test_session_retry.py --slot A --version 13 --port COM19
```

两条命令都会真实升级固件，第一条完成后槽位通常已经改变。分别检查：

1. 软件丢弃第0包和末包的第一次 ACK，升级仍能完成；设备重复包只回 ACK，不重写数据。
2. 第一轮发送总头后故意不发分包；出现 WRITE FAIL 后不按复位，第二轮完整升级成功。

正常升级的最后一包后增加约4秒补确认窗口，再做整片 CRC、标记和复位，这是设计行为。

本次不包含 App 试运行确认、IWDG 自动回滚或数字签名。
