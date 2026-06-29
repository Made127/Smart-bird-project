# 电池模块接线

本项目按单节 3.7V、2500mAh 锂电池检测配置。当前代码使用两个
1K 电阻做 1:1 分压，GPIO3 读取到的电压会在软件里乘以 2 还原为电池电压。

## 供电接线

| 电池/模块引脚 | 接到 |
| --- | --- |
| B+ / 电池红线 | 电池正极 |
| B- / 电池黑线 | 电池负极 / GND |
| OUT+ | 如果开发板支持该模块输出电压，可接开发板供电输入 |
| GND | ESP32 GND 和所有模块 GND |
| IN+ | 如果充电板引出了该脚，可接 Type-C/5V 充电输入正极 |

给 ESP32 供电前先用万用表确认模块输出电压。有些充电板 OUT+ 输出的是电池原始电压，
有些升压板 OUT+ 可能输出 5V。

## 电池电压检测

不要把电池 B+ 或 OUT+ 直接接到 ESP32 ADC 引脚。

当前代码配置：

| 信号 | ESP32-S3 |
| --- | --- |
| 电池分压中点 | GPIO3 / ADC1_CH2 |
| 电池/模块 GND | GND |

使用两个 1K 电阻分压：

```text
电池 B+ ---- 1K ---- GPIO3 ---- 1K ---- GND
```

这个 1:1 分压下，满电 4.2V 到 GPIO3 约为 2.1V，处在 ESP32 ADC 可测范围内。
注意：两个 1K 电阻会持续消耗约 2.1mA，长期待机时会比 100K 分压更耗电；后续如果有
100K 电阻，建议换回 100K + 100K 并同步修改代码常量。

## 软件行为

- 串口日志会打印 `Battery: voltage=...mV percent=...%`。
- App 状态页显示电池百分比和电压。
- 低于 `BIRD_BATTERY_LOW_PERCENT` 时，状态灯显示低电量。
- 如果后续充电模块有 `CHG` / `FULL` 状态脚，再到
  `components/Config/include/bird_test_config.h` 配置
  `BIRD_BATTERY_CHG_PIN` 和 `BIRD_BATTERY_FULL_PIN`。
