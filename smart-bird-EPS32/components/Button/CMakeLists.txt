# 注册按键组件，依赖 GPIO、系统休眠、喇叭和状态灯组件。
idf_component_register(
    SRCS "Button.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_hw_support esp_system Sound StatusLED Config
)
