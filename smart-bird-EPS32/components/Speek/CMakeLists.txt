# 注册麦克风录音组件，依赖 I2S 驱动、配置和 AI 上传接口。
idf_component_register(
    SRCS "Speek.c"
    INCLUDE_DIRS "include"
    REQUIRES driver Config AI
)
