# 注册红外人体感应组件。
idf_component_register(
    SRCS "IR.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
)
