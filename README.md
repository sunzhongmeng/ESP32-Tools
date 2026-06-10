# ESP32 无线 Web 串口调试工具

这是一个基于 ESP32-WROOM-32E 开发板的无线串口调试助手。ESP32 连接被测设备的 TTL UART，PC 或手机只要和 ESP32 在同一个局域网内，就可以打开 ESP32 的 IP 地址，通过网页收发串口数据。

## 默认硬件连接

| ESP32 | 被测设备 TTL UART |
| --- | --- |
| GPIO26 / UART TX | RX |
| GPIO25 / UART RX | TX |
| GND | GND |

- 默认串口是 `UART2`，默认参数是 `115200 8N1`。
- `GPIO16` 已用于板载 WS2812B RGB LED，不再用于 UART。
- `GPIO34/GPIO35` 被板载 CH343P 占用，不再用于 Wi-Fi 模式选择、UART 或指示灯。
- 只建议直接连接 `3.3V TTL`。如果被测设备是 `5V TTL`，建议增加电平转换。
- 不能把 RS232 的正负电压信号直接接到 ESP32。后续扩展 RS232 可加 `MAX3232`，扩展 RS485/RS422 可加差分收发器。

## Wi-Fi 工作方式

固件启动后会优先尝试加入局域网 Wi-Fi：

- 局域网静态 IP：`192.168.0.201`
- 网关：`192.168.0.1`
- DNS：`192.168.0.1` / `8.8.8.8`

如果局域网 Wi-Fi 未配置，ESP32 会直接开启 AP 热点：

- SSID：`ESP32-Tools`
- 密码：`12345678`
- 默认地址：`http://192.168.4.1/`

如果已配置局域网 Wi-Fi 但连接失败，ESP32 会回退到 AP 热点，同时板载 RGB LED 红色常亮表示 Wi-Fi 连接失败。

真实局域网 Wi-Fi 密码放在本地文件 [src/wifi_config.h](C:/Users/Lenovo/Documents/ESP32_UART/src/wifi_config.h)，该文件已加入 `.gitignore`，不会提交到公开仓库。公开仓库里保留模板 [src/wifi_config.example.h](C:/Users/Lenovo/Documents/ESP32_UART/src/wifi_config.example.h)：

```cpp
#define ESP32_TOOLS_STA_SSID "YOUR_WIFI_SSID"
#define ESP32_TOOLS_STA_PASSWORD "YOUR_WIFI_PASSWORD"
```

## RGB 状态灯

板载 WS2812B RGB LED 连接在 `GPIO16`，由 ESP32 RMT 外设驱动，不依赖第三方 LED 库。

| 状态 | RGB LED |
| --- | --- |
| 正常运行 | 绿色 1 秒闪烁 |
| Wi-Fi 连接失败并回退 AP | 红色常亮 |

## 本地预览 Web 页面

板卡调试前，可以先打开 [web/index.html](C:/Users/Lenovo/Documents/ESP32_UART/web/index.html) 核对页面功能。页面在本地打开时会进入模拟模式，可预览：

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

手动编译命令：

```powershell
& 'D:\arduino-cli_1.5.1_Windows_64bit\arduino-cli.exe' compile --fqbn esp32:esp32:esp32 'C:\Users\Lenovo\Documents\ESP32_UART' --warnings default
```

上传到当前板卡 `COM22`：

```powershell
& 'D:\arduino-cli_1.5.1_Windows_64bit\arduino-cli.exe' compile --upload -p COM22 --fqbn esp32:esp32:esp32 'C:\Users\Lenovo\Documents\ESP32_UART' --warnings default
```

串口监视器会打印 ESP32 获取到的 IP。浏览器打开：

```text
http://<ESP32_IP>/
```

网页本身使用 `80` 端口，实时串口数据使用 `81` 端口 WebSocket。如果页面能打开但一直显示未连接，优先检查 PC 防火墙、浏览器代理或局域网隔离。

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
- STA 优先，失败自动 AP 热点
- GPIO16 WS2812B RGB 状态灯
- 不依赖第三方 Arduino 库

## 后续扩展接口

当前固件先验证 TTL UART 和无线 Web 串口链路。后续扩展物理接口时，Web 和 UART 参数部分基本不用变：

- RS232：ESP32 UART TX/RX 前面加 `MAX3232`。
- RS485 半双工：加 485 收发器，另加一个 GPIO 控制 DE/RE，发送前拉使能，发送完成后切回接收。
- RS422：加全双工差分驱动/接收器，逻辑上仍可复用 UART2。
