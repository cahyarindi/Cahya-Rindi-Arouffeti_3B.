#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ===================== KONFIGURASI (WAJIB DIGANTI) =====================
const char* ssid     = "keyr1nn";
const char* password = "rindigemas";

const char* mqttServer = "broker.hivemq.com";
const int   mqttPort   = 1883;
const char* clientId   = "ESP8266_LabIoT_25782035_Tugas";   // harus unik

// Topik (ganti nim_anda dengan NIM Anda)
const char* topicTelemetri = "kampus/labiot/25782035/telemetri";
const char* topicKendali   = "kampus/labiot/25782035/kendali";
const char* topicAlert     = "kampus/labiot/25782035/alert";
const char* topicStatus    = "kampus/labiot/25782035/status";

// Pesan status (LWT & ONLINE)
const char* msgOffline = "{\"status\":\"OFFLINE\",\"keterangan\":\"Perangkat Terputus Tak Terduga\"}";
const char* msgOnline  = "{\"status\":\"ONLINE\",\"keterangan\":\"Perangkat Beroperasi Normal\"}";

// ===================== PIN & PARAMETER =====================
const byte dhtPin = 2;   // D4
const byte ledPin = 5;   // D1
#define DHTTYPE DHT22    // ganti DHT22 jika memakai DHT22

const float SUHU_BATAS = 31.0;                       // ambang bahaya (derajat C)
const unsigned long telemetryInterval = 4000;        // kirim tiap 4 detik
const unsigned long reconnectInterval = 5000;        // jeda percobaan reconnect

DHT dht(dhtPin, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

// ===================== VARIABEL STATUS =====================
unsigned long lastTelemetryTime = 0;
unsigned long lastReconnectAttempt = 0;
bool bahayaAktif = false;   // state latch: true = sedang kondisi bahaya

// ===================== CALLBACK KENDALI =====================
void callback(char* topic, byte* payload, unsigned int length) {
  char buf[length + 1];
  memcpy(buf, payload, length);
  buf[length] = '\0';

  Serial.println("----------------------------------------");
  Serial.printf("[KONTROL DITERIMA] Topik: %s | Payload: %s\n", topic, buf);

  // Saat bahaya, LED dikunci ON oleh sistem -> perintah manual ditolak
  if (bahayaAktif) {
    Serial.println("--> [DITOLAK] Kondisi BAHAYA aktif, LED dikunci ON oleh sistem.");
    return;
  }

  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, buf);
  if (err) {
    Serial.println("Gagal membedah payload JSON!");
    return;
  }

  if (doc["led"].is<int>()) {
    int val = doc["led"];
    digitalWrite(ledPin, val == 1 ? HIGH : LOW);
    Serial.printf("--> [INDIKATOR] LED: %s\n", val == 1 ? "MENYALA" : "PADAM");
  }
}

// ===================== KONEKSI MQTT (NON-BLOCKING) =====================
bool connectMQTT() {
  Serial.print("Menghubungkan ke MQTT Broker...");
  // connect(id, user, pass, willTopic, willQoS, willRetain, willMessage)
  if (client.connect(clientId, NULL, NULL, topicStatus, 1, true, msgOffline)) {
    Serial.println(" Sukses!");
    client.subscribe(topicKendali);
    Serial.printf("Subskripsi: %s\n", topicKendali);
    // Umumkan ONLINE (retain) menimpa pesan OFFLINE lama
    client.publish(topicStatus, msgOnline, true);
    Serial.println("Status ONLINE dipublikasikan.");
    return true;
  }
  Serial.printf(" Gagal (rc=%d)\n", client.state());
  return false;
}

void maintainConnection() {
  // Wi-Fi: biarkan library auto-reconnect, cukup tunggu
  if (WiFi.status() != WL_CONNECTED) return;

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt >= reconnectInterval) {
      lastReconnectAttempt = now;
      connectMQTT();
    }
  }
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  Serial.println("Menghubungkan Wi-Fi (non-blocking)...");

  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
  client.setKeepAlive(15);   // LWT terpicu lebih cepat (~15-22 detik)
}

// ===================== LOOP =====================
void loop() {
  maintainConnection();
  if (client.connected()) client.loop();

  unsigned long now = millis();
  if (now - lastTelemetryTime < telemetryInterval) return;
  lastTelemetryTime = now;

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    Serial.println("Peringatan: Gagal membaca data DHT!");
    return;
  }

  // ---------- Edge Safety Engine ----------
  if (t > SUHU_BATAS) {
    digitalWrite(ledPin, HIGH);            // LED bahaya otomatis ON
    if (!bahayaAktif) {                    // latch: kirim sekali saat pertama terdeteksi
      bahayaAktif = true;
      StaticJsonDocument<128> a;
      a["status"]     = "BAHAYA";
      a["parameter"]  = "OVERHEAT";
      a["nilai_suhu"] = serialized(String(t, 1));
      char ab[128];
      serializeJson(a, ab);
      if (client.connected() && client.publish(topicAlert, ab)) {
        Serial.printf("[ALERT TERKIRIM] %s\n", ab);
      } else {
        bahayaAktif = false;               // gagal kirim -> coba lagi siklus berikutnya
        Serial.println("[ERROR] Alert gagal dikirim, akan dicoba lagi.");
      }
    }
  } else if (bahayaAktif) {                // suhu kembali aman -> lepas latch
    bahayaAktif = false;
    digitalWrite(ledPin, LOW);
    Serial.println("[INFO] Suhu kembali normal, latch dilepas, LED OFF.");
  }

  // ---------- Telemetri Rutin ----------
  StaticJsonDocument<256> doc;
  doc["node_id"]      = clientId;
  doc["suhu"]         = serialized(String(t, 2));
  doc["kelembapan"]   = serialized(String(h, 2));
  doc["status_led"]   = digitalRead(ledPin);
  doc["kondisi_suhu"] = (t > SUHU_BATAS) ? "PANAS" : "NORMAL";

  char buffer[256];
  serializeJson(doc, buffer);

  if (client.connected() && client.publish(topicTelemetri, buffer)) {
    Serial.printf("[TELEMETRI TERKIRIM] %s\n", buffer);
  } else {
    Serial.println("[ERROR] Gagal memublikasikan telemetri!");
  }
}
