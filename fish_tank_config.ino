#define MY_WIFI_SSID   "P2701"
#define MY_WIFI_PASS   "12345678"
#define MY_TG_TOKEN    ""
#define MY_TG_CHAT_ID  "5540092261"
#define MY_SERVER      "https://fish.kolla.click"

// ============================================================
#include <Preferences.h>

void setup() {
  Serial.begin(115200);
  delay(1000);

  Preferences prefs;
  prefs.begin("fishtank", false);
  prefs.putString("ssid",     MY_WIFI_SSID);
  prefs.putString("pass",     MY_WIFI_PASS);
  prefs.putString("server",   MY_SERVER);
  prefs.putString("tgToken",  MY_TG_TOKEN);
  prefs.putString("tgChatId", MY_TG_CHAT_ID);
  prefs.end();

  Serial.println("Xong! Hay upload lai fish_tank_last.ino");
}

void loop() {}
