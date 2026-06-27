# 注册纯头文件配置组件，供其他组件包含 bird_test_config.h。
idf_component_register(
    INCLUDE_DIRS "include"
)

function(add_env_string_define name)
    if(DEFINED ENV{${name}} AND NOT "$ENV{${name}}" STREQUAL "")
        # 这行很重要：字符串配置来自环境变量，编译定义必须保留引号。
        target_compile_definitions(${COMPONENT_LIB} INTERFACE ${name}="$ENV{${name}}")
    endif()
endfunction()

function(add_env_number_define name)
    if(DEFINED ENV{${name}} AND NOT "$ENV{${name}}" STREQUAL "")
        target_compile_definitions(${COMPONENT_LIB} INTERFACE ${name}=$ENV{${name}})
    endif()
endfunction()

add_env_string_define(BIRD_WIFI_SSID)
add_env_string_define(BIRD_WIFI_PASSWORD)
add_env_string_define(BIRD_SERVER_IP)
add_env_number_define(BIRD_SERVER_PORT)
