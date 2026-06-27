# 注册 RGB 状态灯组件。
idf_component_register(
    SRCS "StatusLED.c"
    INCLUDE_DIRS "include"
    REQUIRES driver Config
)
