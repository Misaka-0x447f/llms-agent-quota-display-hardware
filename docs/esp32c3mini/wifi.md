# Wi‑Fi 使用

## 能力概览

ESP32-C3 SuperMini 支持 2.4 GHz IEEE 802.11 b/g/n，可工作在：

- Station（STA，连接现有路由器）
- SoftAP（自身作为热点）
- SoftAP + Station
- 混杂模式

## 通用准备

- Arduino 板型：`ESP32C3 Dev Module`
- 串口监视器：`115200 baud`
- 连接方式：USB Type-C 数据线
- 代码中的 SSID、密码使用占位符；不要把真实凭据提交到仓库

## 1. 扫描附近 Wi‑Fi（Station 模式）

```cpp
#include "WiFi.h"

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  Serial.println("WiFi scan ready");
}

void loop() {
  Serial.println("scan start");
  int count = WiFi.scanNetworks();
  Serial.println("scan done");

  if (count <= 0) {
    Serial.println("no networks found");
  } else {
    Serial.printf("%d networks found\n", count);
    for (int i = 0; i < count; ++i) {
      Serial.printf("%d: %s (%d dBm)%s\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    WiFi.RSSI(i),
                    WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " [open]" : " [secured]");
    }
  }

  WiFi.scanDelete();
  delay(5000);
}
```

## 2. 连接已有 Wi‑Fi

```cpp
#include <WiFi.h>

const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASSWORD = "your-password";

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting");
  const unsigned long timeoutMs = 15000;
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection timeout");
  }
}

void loop() {}
```

## 3. 创建 Wi‑Fi 热点（SoftAP）

```cpp
#include "WiFi.h"

const char* AP_SSID = "ESP32C3-SuperMini";
const char* AP_PASSWORD = "replace-with-a-strong-password";

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_AP);

  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("SoftAP start failed");
    return;
  }

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  Serial.printf("connected stations: %d\n", WiFi.softAPgetStationNum());
  delay(1000);
}
```

## 4. Wi‑Fi 控制板载 LED

下面的示例启动一个 HTTP 服务：访问 `/H` 将 GPIO8 置高，访问 `/L` 将 GPIO8 置低。

```cpp
#include <WiFi.h>

const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASSWORD = "your-password";
const int LED_PIN = 8;
WiFiServer server(80);

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  Serial.println(WiFi.localIP());
  server.begin();
}

void loop() {
  WiFiClient client = server.available();
  if (!client) return;

  String request = client.readStringUntil('\r');
  client.flush();
  if (request.indexOf("GET /H") >= 0) digitalWrite(LED_PIN, HIGH);
  if (request.indexOf("GET /L") >= 0) digitalWrite(LED_PIN, LOW);

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.println("<a href='/H'>LED HIGH</a><br>");
  client.println("<a href='/L'>LED LOW</a>");
  client.stop();
}
```

## Agent 生成代码时的工程建议

- 不要使用无限等待连接的循环；生产代码应设置超时和重试退避。
- 不要把真实 Wi‑Fi 密码写入源码、日志或回答中。
- 对网络服务增加请求长度限制、路由白名单和连接超时。
- Wi‑Fi 功耗与信号强度、天线、连接状态和发送频率有关，不能仅凭标称深度睡眠电流推断整机功耗。

来源：<https://wiki.nologo.tech/product/esp32/esp32c3/esp32c3supermini/esp32C3SuperMiniWifi.html>
