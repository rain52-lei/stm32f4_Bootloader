/* ESP32-S3 串口<->WiFi 透传桥（配合 G:\Boot\wifi_upgrade_server.py 使用）
 *
 * 作用：把探索者 STM32F407 的 USART1 字节流原样搬进 TCP，
 *       对 bootloader 来说"对面是 WiFi 还是 USB 串口"完全无感。
 *
 * 接线（3 根杜邦线，两块板各自供电、必须共地）：
 *   本板 GPIO17 (TX) -> 探索者 PA10 (MCU RX)
 *   本板 GPIO18 (RX) <- 探索者 PA9  (MCU TX)
 *   本板 GND <-> 探索者 GND
 *   本板用自带 USB 口供电（WiFi 发射电流大，别从探索者 3.3V 取电）
 *
 * 探索者侧注意：P6 上 CH340 与 PA9/PA10 间的跳线帽要摘掉，
 * 否则插 USB 时 CH340 会和本板抢同一条串口线。
 */

#include <Arduino.h>
#include <WiFi.h>

/* ==== 按实际情况修改这 4 项 ==== */
const char *WIFI_SSID  = "你的WiFi名";
const char *WIFI_PASS  = "你的WiFi密码";
const char *SERVER_IP  = "192.168.1.100";  // wifi_upgrade_server.py 启动时打印的本机 IP
const uint16_t SERVER_PORT = 9000;         // 与服务器脚本 --port 一致

/* ESP32-S3 引脚经 GPIO 矩阵可任意映射；17/18 是安全脚。
 * 换脚时避开：0/3/45/46（启动 straps）、19/20（USB D+/D-）、26~37（flash/PSRAM） */
const int BRIDGE_TX = 17;   // -> 接 PA10
const int BRIDGE_RX = 18;   // <- 接 PA9

WiFiClient client;
uint32_t s_last_try = 0;

/* WiFi 与 TCP 任一断开就自动补链（这就是 ATK 固件 AT+SAVETRANSMISSIONLINK
 * 的活，现在是我们自己的代码，行为完全可控） */
void maintain_links(void)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        return;    /* setAutoReconnect 在后台重连 WiFi */
    }

    if (!client.connected() && millis() - s_last_try > 1000UL)
    {
        s_last_try = millis();
        Serial.print("[桥] 连接服务器 ");
        Serial.print(SERVER_IP);
        Serial.print(':');
        Serial.println(SERVER_PORT);
        if (client.connect(SERVER_IP, SERVER_PORT))
        {
            client.setNoDelay(true);    /* 关 Nagle，停等协议的回复不积压 */
            Serial.println("[桥] 已连接");
        }
        else
        {
            client.stop();
            Serial.println("[桥] 失败，1 秒后重试");
        }
    }
}

void setup()
{
    Serial.begin(115200);                          /* USB 调试口，只打印状态 */
    Serial1.begin(115200, SERIAL_8N1, BRIDGE_RX, BRIDGE_TX);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.println("[桥] 启动，连接 WiFi...");
}

void loop()
{
    static uint8_t buf[512];

    maintain_links();
    if (!client.connected())
    {
        delay(50);
        return;
    }

    /* STM32 -> 服务器 */
    int n = Serial1.available();
    if (n > 0)
    {
        if (n > (int)sizeof(buf))
        {
            n = (int)sizeof(buf);
        }
        n = Serial1.read(buf, n);
        if (n > 0)
        {
            client.write(buf, n);
        }
    }

    /* 服务器 -> STM32 */
    int m = client.available();
    if (m > 0)
    {
        if (m > (int)sizeof(buf))
        {
            m = (int)sizeof(buf);
        }
        m = client.read(buf, m);
        if (m > 0)
        {
            Serial1.write(buf, m);
        }
    }

    delay(1);    /* 喂狗/让出 CPU，1ms 轮询对 115200 串口绰绰有余 */
}
