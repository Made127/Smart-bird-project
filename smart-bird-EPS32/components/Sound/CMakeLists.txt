# 注册喇叭播放组件。
idf_component_register(
    SRCS "Sound.c"
    INCLUDE_DIRS "include"
    REQUIRES driver Config
)

# Sound.c 使用 sinf 生成开机测试音，需要链接数学库。
target_link_libraries(${COMPONENT_LIB} PUBLIC m)
