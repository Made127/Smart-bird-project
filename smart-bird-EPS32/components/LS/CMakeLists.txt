# 注册 BH1750 光照传感器组件。
idf_component_register(
    SRCS "LS.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
)
