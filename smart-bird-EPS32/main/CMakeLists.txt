# 注册主程序组件，并声明依赖的各硬件/业务组件。
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES Config WIFI AI Sound Speek IR LS SoilProbe StatusLED Button Battery
)
