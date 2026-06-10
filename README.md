# ESP32 无线 Web 串口调试工具

这是一个基于 ESP32-WROOM-32E 开发板的无线串口调试助手。ESP32 连接被测设备的 TTL UART，PC 或手机只要和 ESP32 在同一个局域网内，就可以打开 ESP32 的 IP 地址，通过网页收发串口数据。

## 默认硬件连接

| ESP32 | 被测设备 TTL UART |
| --- | --- |
| GPIO17 / TX2 | RX |
| GPIO16 / RX2 | TX |
| GND | GND |

- 默认串口是 `UART2`，默认参数是 `115200 8N1`。
- 只建议直接连接 `3.3V TTL`。如果被测设备是 `5V TTL`，建议增加电平转换。
- 不能把 RS232 的正负电压信号直接接到 ESP32。后续扩展 RS232 可加 `MAX3232`，扩展 RS485/RS422 可加差分收发器。
- 淘宝常见 YD-ESP32 / ESP32-WROOM-32E 核心板一般可用 `GPIO16/GPIO17` 做 UART2。如果板卡引脚不同，可在网页里修改 RX/TX GPIO。

## Wi-Fi 模式选择

固件启动时读取 `IO34`：

- `IO34 = 1`：加入局域网 Wi-Fi，静态 IP 为 `192.168.0.201`，网关为 `192.168.0.1`。
- `IO34 = 0`：开启 ESP32 热点，热点名称为 `ESP32-Tools`，密码为 `12345678`，默认地址为 `http://192.168.4.1/`。

`IO34` 是输入专用脚，建议外部电阻明确上拉或下拉，避免悬空导致启动模式不稳定。

真实局域网 Wi-Fi 密码放在本地文件 [src/wifi_config.h](C:/Users/Lenovo/Documents/ESP32_UART/src/wifi_config.h)，该文件已加入 `.gitignore`，不会提交到公开仓库。公开仓库里保留模板 [src/wifi_config.example.h](C:/Users/Lenovo/Documents/ESP32_UART/src/wifi_config.example.h)：

```cpp
#define ESP32_TOOLS_STA_SSID "YOUR_WIFI_SSID"
#define ESP32_TOOLS_STA_PASSWORD "YOUR_WIFI_PASSWORD"
```

如果选择局域网模式但 Wi-Fi 连接失败，ESP32 会回退到 AP 热点，同时系统运行灯长亮表示异常。

## 指示灯

| GPIO | 功能 | 电平 |
| --- | --- | --- |
| IO32 | UART RX 指示 | 高有效，收到串口数据短亮 |
| IO33 | 系统运行状态 | 正常 1Hz 闪烁，异常长亮 |
| IO35 | 用户期望 UART TX 指示 | ESP32-WROOM-32 上是输入专用脚，不能输出点灯 |

注意：ESP32-WROOM-32 的 `IO35` 不能作为输出脚。当前固件会检测到这个限制并关闭 TX LED 输出，只在串口日志里打印提示。如果需要 TX 指示灯，建议把 [src/main.cpp](C:/Users/Lenovo/Documents/ESP32_UART/src/main.cpp) 里的 `UART_TX_LED_PIN` 改到 `IO25/IO26/IO27` 等可输出 GPIO。

## 本地预览 Web 页面

板卡还没到时，可以先打开 [web/index.html](C:/Users/Lenovo/Documents/ESP32_UART/web/index.html) 核对页面功能。页面在本地打开时会进入模拟模式，可预览：

- ASCII / HEX 接收显示
- ASCII / HEX 发送、CR/LF、定时发送
- 波特率、数据位、校验位、停止位、RX/TX GPIO 设置
- 多字符串发送条目：新增、勾选发送、循环发送、单条发送
- 多字符串条目 ASCII / HEX 格式、独立发送间隔、本地保存、JSON 导入/导出
- UART BREAK，时间范围 `1..5000 ms`

## Arduino CLI 编译上传

本机已验证可用的 Arduino CLI 路径：

```powershell
D:\arduino-cli_1.5.1_Windows_64bit\arduino-cli.exe
```

当前工程已用 `esp32:esp32@2.0.17` 和通用 `ESP32 Dev Module` 编译通过。手动编译命令：

```powershell
& 'D:\arduino-cli_1.5.1_Windows_64bit\arduino-cli.exe' compile --fqbn esp32:esp32:esp32 'C:\Users\Lenovo\Documents\ESP32_UART' --warnings default
```

板子到货后，假设端口是 `COM5`，可编译并上传：

```powershell
& 'D:\arduino-cli_1.5.1_Windows_64bit\arduino-cli.exe' compile --upload -p COM5 --fqbn esp32:esp32:esp32 'C:\Users\Lenovo\Documents\ESP32_UART' --warnings default
```

VS Code 已配置任务：[.vscode/tasks.json](C:/Users/Lenovo/Documents/ESP32_UART/.vscode/tasks.json)。可在 VS Code 里运行 `Terminal -> Run Task... -> Arduino: compile ESP32`。

串口监视器会打印 ESP32 获取到的 IP。浏览器打开：

```text
http://<ESP32_IP>/
```

网页本身使用 `80` 端口，实时串口数据使用 `81` 端口 WebSocket。如果页面能打开但一直显示未连接，优先检查 PC 防火墙、浏览器代理或局域网隔离。

也可以用 Arduino IDE 打开 [ESP32_UART.ino](C:/Users/Lenovo/Documents/ESP32_UART/ESP32_UART.ino)，选择 `ESP32 Dev Module` 后编译上传。

## 已实现功能

- 浏览器 Web 页面实时收发串口数据
- WebSocket 二进制传输，适合原始字节流
- 自定义波特率、数据位、校验位、停止位、RX/TX 引脚
- 接收显示支持 ASCII / HEX
- 发送输入支持 ASCII / HEX
- 可选发送追加 CR / LF
- 可选定时发送，间隔可调
- 多字符串发送条目，可单条发送、勾选顺序发送、循环发送
- 多字符串条目支持 ASCII / HEX、独立发送间隔、浏览器本地保存
- 多字符串条目支持 JSON 导入/导出
- 支持 UART BREAK，BREAK 时间 `1..5000 ms`
- IO34 选择 STA/AP 模式；STA 使用静态 IP，失败自动 AP 热点
- IO32 UART RX 指示灯，IO33 系统运行/异常指示灯
- 不依赖第三方 Arduino 库

## 多字符串条目文件格式

导出的 JSON 可以直接修改后再导入：

```json
{
  "version": 1,
  "rows": [
    {
      "enabled": true,
      "name": "AT",
      "mode": "ascii",
      "value": "AT\\r\\n",
      "interval": 300
    },
    {
      "enabled": true,
      "name": "Modbus",
      "mode": "hex",
      "value": "01 03 00 00 00 02 C4 0B",
      "interval": 1000
    }
  ]
}
```

## 后续扩展接口

当前固件先验证 TTL UART 和无线 Web 串口链路。后续扩展物理接口时，Web 和 UART 参数部分基本不用变：

- RS232: ESP32 UART TX/RX 前面加 `MAX3232`。
- RS485 半双工: 加 485 收发器，另加一个 GPIO 控制 DE/RE，发送前拉使能，发送完成后切回接收。
- RS422: 加全双工差分驱动/接收器，逻辑上仍可复用 UART2。
