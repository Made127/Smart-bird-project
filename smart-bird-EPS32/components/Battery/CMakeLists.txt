# 注册电池检测组件，依赖 ADC、GPIO 驱动和状态灯/配置组件。
idf_component_register(
    SRCS "Battery.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc StatusLED Config
)
