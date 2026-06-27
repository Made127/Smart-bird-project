#ifndef WIFI_H
#define WIFI_H

// 连接指定 WiFi，成功返回 0。
int wifi_connect(const char *ssid, const char *password);
// 返回当前是否已联网。
int wifi_is_connected(void);

#endif
