#ifndef SOIL_PROBE_H
#define SOIL_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 土壤探头数据，带 x10 后缀的字段表示实际值扩大 10 倍保存。
typedef struct {
    bool valid;
    int16_t temperature_x10;
    uint16_t humidity_x10;
    uint16_t ec_us_cm;
    uint16_t salt_ppm;
    uint16_t nitrogen_mg_kg;
    uint16_t phosphorus_mg_kg;
    uint16_t potassium_mg_kg;
    uint16_t ph_x10;
    int64_t updated_ms;
} soil_probe_data_t;

// 初始化土壤探头串口。
esp_err_t SoilProbe_init(void);
// 读取一次土壤探头数据。
esp_err_t SoilProbe_read_once(void);
// 获取最近一次有效数据。
bool SoilProbe_get_latest(soil_probe_data_t *out);

#endif
