# 注册 AI 通信组件，依赖 HTTP 客户端、音频、传感器和电池模块。
idf_component_register(
    SRCS "AI.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client Sound IR LS SoilProbe Battery
)
