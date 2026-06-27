# 注册土壤 Modbus 探头组件。
idf_component_register(
    SRCS "SoilProbe.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer Config
)
