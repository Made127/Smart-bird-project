# 土壤探针接线与测试

本项目已按 Modbus RTU 探针接入，默认参数：

- 探针地址：`0x02`
- 波特率：`9600`
- 串口：`UART1`
- 板子接收脚：`GPIO18`
- 板子发送脚：`GPIO21`
- 读取寄存器：`0x0000` 开始 8 个寄存器

## 4P 磁吸头接线

如果探针是 RS485 四线：

| 磁吸头线 | 连接 |
| --- | --- |
| V+ | 探针供电，按探针要求接 5V/12V/24V |
| GND | 电源地，并与 ESP32 GND 共地 |
| A+ | RS485 A+ |
| B- | RS485 B- |

板子这边需要 RS485 转 UART 模块：

| RS485 模块 | ESP32 |
| --- | --- |
| RO/TX | `GPIO18` |
| DI/RX | `GPIO21` |
| VCC | 按模块要求接 3.3V 或 5V |
| GND | ESP32 GND |
| A/B | 接磁吸头 A+/B- |

如果你的模块没有自动收发方向，需要把 DE/RE 接到一个 GPIO，并在
`components/Config/include/bird_test_config.h` 里设置：

```c
#define BIRD_SOIL_PROBE_DE_PIN GPIO_NUM_xx
```

如果探针是 TTL 四线：

| 探针线 | ESP32 |
| --- | --- |
| V+ | 按探针要求供电 |
| GND | ESP32 GND |
| TX | `GPIO18` |
| RX | `GPIO21` |

注意：不要把 12V/24V 接到 ESP32 的 GPIO18/GPIO21。

## 测试流程

1. 确认 4P 磁吸头两端线序完全一致。
2. 启动电脑服务：`.\run_server.ps1`。
3. 烧录并打开串口监视器。
4. 等 WiFi 连接成功后，App 点“立即采集”，或等待开机 5 秒自动采集。
5. 串口应看到类似：

```text
I SoilProbe: temp=25.1 hum=43.2 ec=...
I AI: collect source=boot status=200
```

6. App 传感器页应显示土壤温度、湿度、EC、盐分、氮、磷、钾、PH。
7. 语音可以问：“土壤湿度多少”“PH 多少”“氮磷钾多少”。

如果串口显示 `SoilProbe: response timeout`，优先检查 A/B 是否接反、探针供电、地址是否为 `0x02`、波特率是否为 `9600`。
