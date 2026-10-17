/*
  ESP32 แจ้งเตือนไฟบ้านดับเข้า Telegram
  โค้ดประกอบคลิป K iTech Review / Kham iTech Lab

  หลักการ:
    - ESP32 ใช้ไฟจากแบตสำรอง (พาวเวอร์แบงก์ที่ชาร์จไปใช้ไปได้ หรือ UPS ตัวเล็ก)
    - อะแดปเตอร์ 5V อีกตัวเสียบปลั๊กบ้าน ใช้เป็น "ตัวตรวจไฟบ้าน"
    - ไฟบ้านมา = ขา SENSE เป็น HIGH / ไฟบ้านดับ = LOW

  ต่อวงจร (ตัวแบ่งแรงดัน 5V -> 3.3V):
    อะแดปเตอร์ 5V (+) --- R1 10k ---+--- GPIO 34 (SENSE_PIN)
                                     |
                                    R2 20k
                                     |
    อะแดปเตอร์ 5V (-) ---------------+--- GND ของ ESP32
    5 x 20 / (10 + 20) = 3.33V  ห้ามต่อ 5V เข้าขาตรง ๆ
    ห้ามต่อไฟบ้าน 220V เข้าบอร์ดเด็ดขาด ใช้อะแดปเตอร์เท่านั้น

  เราเตอร์อินเทอร์เน็ตต้องมีไฟสำรองด้วย ไม่งั้นส่งข้อความไม่ออก
  (ถ้าส่งไม่ออก โค้ดนี้จะลองส่งซ้ำจนกว่าจะสำเร็จ)
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"   // คัดลอกจาก secrets.example.h แล้วแก้ค่า

// ---------- ตั้งค่า ----------
const int SENSE_PIN = 34;                 // ขาตรวจไฟบ้าน (GPIO 34 เป็นขารับอย่างเดียว)
const unsigned long CONFIRM_MS = 3000;    // ต้องดับ/มาค้างนานเท่านี้ก่อนแจ้ง กันไฟกะพริบ
const unsigned long RETRY_MS   = 15000;   // ส่งไม่สำเร็จ ลองใหม่ทุก 15 วินาที

// ---------- ตัวแปร ----------
bool powerOn = true;          // สถานะไฟบ้านที่ยืนยันแล้ว
bool lastRead = true;
unsigned long lastChange = 0;
unsigned long outageStart = 0;
String pending = "";          // ข้อความที่ยังส่งไม่สำเร็จ
unsigned long lastTry = 0;

String urlEncode(const String &s) {
  String out;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

bool sendTelegram(const String &text) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();  // ง่ายสำหรับโปรเจกต์บ้าน (ไม่ตรวจใบรับรอง)
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + BOT_TOKEN + "/sendMessage";
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = String("chat_id=") + CHAT_ID + "&text=" + urlEncode(text);
  int code = http.POST(body);
  Serial.printf("Telegram HTTP %d\n", code);
  http.end();
  return code == 200;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nWiFi OK" : "\nWiFi failed, retry later");
}

// ใส่ข้อความเข้าคิว แล้วลองส่งทันที (ถ้าส่งไม่ได้ จะลองใหม่ใน loop)
void notify(const String &msg) {
  if (pending.length()) pending += "\n";
  pending += msg;
  lastTry = 0;
}

String minutesText(unsigned long ms) {
  unsigned long m = ms / 60000UL;
  if (m < 1) return "ไม่ถึง 1 นาที";
  if (m < 60) return String(m) + " นาที";
  return String(m / 60) + " ชม. " + String(m % 60) + " นาที";
}

void setup() {
  Serial.begin(115200);
  pinMode(SENSE_PIN, INPUT);   // ตัวต้านทาน R2 ทำหน้าที่ดึงลงกราวด์ตอนไฟดับ
  connectWiFi();
  powerOn = lastRead = digitalRead(SENSE_PIN) == HIGH;
  notify(String("✅ ระบบแจ้งเตือนไฟดับพร้อมแล้ว\nตอนนี้ไฟบ้าน") + (powerOn ? "ปกติ" : "ดับอยู่"));
}

void loop() {
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 10000) {
    lastWiFiCheck = millis();
    connectWiFi();
  }

  bool r = digitalRead(SENSE_PIN) == HIGH;
  if (r != lastRead) {
    lastRead = r;
    lastChange = millis();
  }
  if (r != powerOn && millis() - lastChange > CONFIRM_MS) {
    powerOn = r;
    if (!powerOn) {
      outageStart = millis();
      Serial.println("POWER OFF");
      notify("⚡ ไฟบ้านดับ!");
    } else {
      Serial.println("POWER BACK");
      notify(String("✅ ไฟกลับมาแล้ว (ดับไป ") + minutesText(millis() - outageStart) + ")");
    }
  }

  if (pending.length() && (lastTry == 0 || millis() - lastTry > RETRY_MS)) {
    lastTry = millis();
    if (sendTelegram(pending)) pending = "";
  }
  delay(20);
}
