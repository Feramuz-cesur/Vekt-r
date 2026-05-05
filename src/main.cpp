#include "DFRobotDFPlayerMini.h"
#include <math.h>
#include "SPIFFS.h"
#include "time.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Arduino_JSON.h>
#include <AsyncTCP.h>
#include <ESP32Servo.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <freertos/ringbuf.h>
#include <FluxGarage_RoboEyes.h>

// RGB Kütüphanesi
#include <Adafruit_NeoPixel.h>

// --- PIN TANIMLAMALARI ---
#define IN1 32
#define IN2 33
#define IN3 25
#define IN4 26
#define THC_PIN 13
const int servoArmPin = 27;
const int servoHeadPin = 14;
#define RX_PIN 4
#define TX_PIN 5

// *** YENİ RGB PIN TANIMI ***
#define RGB_PIN 23  // NeoPixel Data Pini
#define NUMPIXELS 8 // Kaç adet LED var? (Burayı sayına göre değiştir)

// *** PİL ÖLÇÜM (ADC) PİNİ ***
#define BATTERY_PIN 34 // 45K + 45K Gerilim Bölücü Ortası

// *** YAPAY ZEKA DONANIM PİNLERİ (I2S ÇİFT KANAL) ***
#define I2S_SPEAKER_PORT I2S_NUM_1
#define I2S_BCLK 16
#define I2S_LRC 15
#define I2S_DOUT 17

#define SPEAKER_SAMPLE_RATE 24000

// --- NESNELER ---
DFRobotDFPlayerMini myDFPlayer;
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define i2c_Address 0x3c
Adafruit_SH1106G display =
    Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
RoboEyes<Adafruit_SH1106G> eyes(display);

Servo servoArm, servoHead;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// RGB Nesnesi
Adafruit_NeoPixel pixels(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

// --- GLOBAL DEĞİŞKENLER ---
#define AP_SSID "Pixel_Setup"
#define AP_PASS "12345678"
volatile bool resetWifiPending = false; // WS'den "forgetWifi" gelince true
volatile bool rebootPending = false;    // WiFi şifresi kaydedildikten sonra yeniden başlatmak için

bool isAPMode = false;
DNSServer dnsServer;

volatile bool displayLocked = false; // Ekran kilitli mi?

volatile bool isAIModeActive = false;
RingbufHandle_t audio_ringbuf = NULL;
volatile uint32_t speaker_sample_rate = SPEAKER_SAMPLE_RATE;
volatile bool speakerTestPending = false;
// 8Ω 1W hoparlör için güvenli dijital tepe sınırı.
// MAX98357A varsayılan 9dB kazançta 5V'tan 8Ω'a tam ölçek -> ~1.8W (hoparlörün 1.8 katı!).
// 22000'e (~-3.5dBFS, ~0.8W) bağlayarak hoparlörü 1W çalışma sınırının altında tutuyoruz.
// Tarayıcı tarafında da aynı sınır uygulanıyor; bu burada ikinci savunma katmanı.
static const int16_t SPEAKER_SAFE_PEAK = 22000;
volatile float aiSpeakerGain = 1.0f; // I2S kazancı: 1.0 = pass-through; ses kontrolü tarayıcıda

// Görevler arasında veri paylaşımı
volatile int danceTrigger = 0;
volatile bool touchTrigger = false;
volatile unsigned long ipDisplayUntil = 0;
volatile bool isClockModeActive = false; // Saat modu sürekli açık mı?
volatile bool pendingClockDraw = false;  // Websocket'ten gelen çizim isteği
unsigned long lastMillis = 0;
unsigned long touchPressStartMillis = 0;
int long interval = 18000;
bool isMoodActive = false;
bool randORdefault = false;
bool ipShownForCurrentPress = false;

const unsigned long IP_SHOW_DURATION_MS = 3000;
const unsigned long TOUCH_HOLD_DURATION_MS = 4000;

// RGB Durum Değişkenleri
String lightMode = "OFF"; // OFF, STATIC, RAINBOW, PULSE, POLICE
uint32_t selectedColor = pixels.Color(0, 0, 0); // Varsayılan Kapalı
unsigned long pixelPreviousMillis = 0;
int pixelInterval = 20;
int pixelCycle = 0;
int pixelBrightness = 0;
bool breathingUp = true;

// =================================================================
// AI HAREKET KALİBRASYONU
// AI fonksiyon çağrılarından gelen 1-5 hız seviyesi ve birim (cm, mm,
// derece) değerlerinin fiziksel karşılıklarını burada ayarla.
// Tarayıcı sadece seviye + birim gönderir; tüm dönüşümler burada yapılır.
// =================================================================

// Motor PWM tablosu: hız seviyesi 1-5 -> 0-254 PWM (doğrusal)
// Hız 1 = ~%20 (51), Hız 5 = max (254)
// Düşük seviyelerde motor statik sürtünmeyi yenemeyebilir; gerekirse
// 1 ve 2'yi yukarı çek (örn. {0, 90, 130, 170, 210, 254}).
static const int MOTOR_PWM_LEVEL[6] = {0, 51, 101, 152, 203, 254};

// Servo hareket hızı: hız seviyesi 1-5 -> moveServoTo() iç ölçeği 1-10
static const int SERVO_SPEED_LEVEL[6] = {0, 2, 4, 6, 8, 10};

// Kol mm-açı dönüşümü (lineer):
//   ARM_FULL_MM mm fiziksel kalkış = ARM_FULL_ANGLE derece servo açısı
// Örn. 110mm -> 70°. Mekanik kalibrasyon için bu iki sayıyı değiştir.
static const float ARM_FULL_MM = 110.0f;
static const float ARM_FULL_ANGLE = 70.0f;

// Düz hareket: her hız seviyesi için cm/saniye.
// Süre = mesafe_cm / cm_per_sec.
// Örn. hız 4'te 10cm -> 10/22 = 454ms
// Kalibrasyon: gerçek robotu sabit zeminde ölç, değerleri güncelle.
static const float MOVE_CM_PER_SEC[6] = {0.0f, 4.0f, 9.0f, 15.0f, 22.0f, 30.0f};

// Dönüş: her hız seviyesi için derece/saniye.
// Süre = derece / deg_per_sec.
// Örn. hız 4'te 90° -> 1000ms; hız 2'de 90° -> 2000ms
static const float TURN_DEG_PER_SEC[6] = {0.0f, 22.5f, 45.0f, 67.5f, 90.0f, 112.5f};

// Varsayılan hız seviyesi (AI komutu speed_level vermezse)
static const int DEFAULT_SPEED_LEVEL = 3;

// Kafa için logik açı aralığı (mevcut UI ile aynı: 0..60)
static const int HEAD_LOGIC_MAX = 60;

// AI zamanlanmış hareket bitiş zamanı (move_distance, turn_degrees,
// timed_move için). Task_Mantik bu zaman dolduğunda dur() çağırır.
volatile unsigned long timedMoveStopAt = 0;
volatile bool timedMoveActive = false;

// Hız seviyesini 1-5'e kıs (0 veya geçersiz -> varsayılan)
static inline int clampSpeedLevel(int lvl) {
  if (lvl < 1 || lvl > 5) return DEFAULT_SPEED_LEVEL;
  return lvl;
}

// --- MOTOR FONKSİYONLARI ---
void ileri(int pwm) {
  digitalWrite(IN1, LOW);
  digitalWrite(IN3, LOW);
  analogWrite(IN2, pwm);
  analogWrite(IN4, pwm);
}
void geri(int pwm) {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN3, HIGH);
  analogWrite(IN2, 254 - pwm);
  analogWrite(IN4, 254 - pwm);
}
void sag(int pwm) {
  digitalWrite(IN1, LOW);
  digitalWrite(IN3, HIGH);
  analogWrite(IN2, pwm);
  analogWrite(IN4, 254 - pwm);
}
void sol(int pwm) {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN3, LOW);
  analogWrite(IN2, 254 - pwm);
  analogWrite(IN4, pwm);
}
void dur() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN3, LOW);
  analogWrite(IN2, 0);
  analogWrite(IN4, 0);
}

// --- TANK DRIVE (Bireysel Motor Kontrolü) ---
// v: -255..255  |  pozitif = ileri, negatif = geri
void setMotorSol(int v) {
  v = constrain(v, -255, 255);
  if (v >= 0) {
    digitalWrite(IN1, LOW);
    analogWrite(IN2, v);
  } else {
    digitalWrite(IN1, HIGH);
    analogWrite(IN2, 254 - abs(v));
  }
}

void setMotorSag(int v) {
  v = constrain(v, -255, 255);
  if (v >= 0) {
    digitalWrite(IN3, LOW);
    analogWrite(IN4, v);
  } else {
    digitalWrite(IN3, HIGH);
    analogWrite(IN4, 254 - abs(v));
  }
}

// --- SERVO FONKSİYONU ---

/**
 * @brief Smoothly drives a servo from its current position to a target angle.
 *
 * @param s            Servo reference (servoArm or servoHead).
 * @param targetAngle  Target angle in degrees. Hard-clamped to [0..180],
 *                     but the mechanical limits per servo are:
 *                       - servoArm  : 0..70°  (arm raise range)
 *                       - servoHead : 0..60°  (head tilt range)
 *                     Callers should respect those ranges.
 * @param speed        Movement speed [1..10].
 *                       1  = slowest (smoothest),
 *                       10 = fastest (snappiest).
 */
void moveServoTo(Servo &s, int targetAngle, int speed) {
  targetAngle = constrain(targetAngle, 0, 180);
  speed = constrain(speed, 1, 10);

  int startAngle = s.read();
  if (startAngle == targetAngle) return;

  int step = map(speed, 1, 10, 1, 4);
  int delayMs = map(speed, 1, 10, 22, 2);

  if (startAngle < targetAngle) {
    for (int i = startAngle; i < targetAngle; i += step) {
      s.write(i);
      vTaskDelay(delayMs / portTICK_PERIOD_MS);
    }
  } else {
    for (int i = startAngle; i > targetAngle; i -= step) {
      s.write(i);
      vTaskDelay(delayMs / portTICK_PERIOD_MS);
    }
  }
  s.write(targetAngle);
  vTaskDelay(delayMs / portTICK_PERIOD_MS);
}

// --- RGB LED YÖNETİMİ ---
void updateLEDs() {
  unsigned long currentMillis = millis();

  if (lightMode == "OFF") {
    pixels.clear();
    pixels.show();
  } else if (lightMode == "STATIC") {
    // Sadece statik renk
    for (int i = 0; i < NUMPIXELS; i++) {
      pixels.setPixelColor(i, selectedColor);
    }
    pixels.show();
  } else if (lightMode == "RAINBOW") {
    // 20ms yerine 50ms veya 100ms yaparak geçişi daha da yavaşlatabilirsin
    if (currentMillis - pixelPreviousMillis >= 40) {
      pixelPreviousMillis = currentMillis;

      // pixelCycle'ı daha büyük adımlarla döndürmek için 65536 üzerinden
      // hesaplayalım pixelCycle burada 0-65535 arası bir değer tutacak şekilde
      // modifiye edilebilir veya mevcut pixelCycle'ı (0-255) kullanıp map
      // edebiliriz.

      pixelCycle++;
      if (pixelCycle > 255)
        pixelCycle = 0;

      // Tüm LED'ler için TEK BİR renk hesaplıyoruz (i kullanılmıyor)
      uint32_t unitColor = pixels.gamma32(pixels.ColorHSV(pixelCycle * 256));

      for (int i = 0; i < pixels.numPixels(); i++) {
        pixels.setPixelColor(i, unitColor);
      }
      pixels.show();
    }
  } else if (lightMode == "PULSE") {
    // Nefes alma efekti (Seçili renkte)
    if (currentMillis - pixelPreviousMillis >= 10) {
      pixelPreviousMillis = currentMillis;

      if (breathingUp) {
        pixelBrightness++;
        if (pixelBrightness >= 255)
          breathingUp = false;
      } else {
        pixelBrightness--;
        if (pixelBrightness <= 5)
          breathingUp = true;
      }

      // Rengi parlaklığa göre ayarla
      // selectedColor'ı R, G, B bileşenlerine ayır
      uint8_t r = (uint8_t)(selectedColor >> 16);
      uint8_t g = (uint8_t)(selectedColor >> 8);
      uint8_t b = (uint8_t)selectedColor;

      // Parlaklık oranını uygula
      r = (r * pixelBrightness) / 255;
      g = (g * pixelBrightness) / 255;
      b = (b * pixelBrightness) / 255;

      for (int i = 0; i < NUMPIXELS; i++) {
        pixels.setPixelColor(i, pixels.Color(r, g, b));
      }
      pixels.show();
    }
  } else if (lightMode == "POLICE") {
    // Polis çakarı (Kırmızı / Mavi)
    if (currentMillis - pixelPreviousMillis >= 150) {
      pixelPreviousMillis = currentMillis;
      pixelCycle = !pixelCycle; // 0 veya 1

      for (int i = 0; i < NUMPIXELS; i++) {
        if (i % 2 == 0) {
          pixels.setPixelColor(i, pixelCycle ? pixels.Color(255, 0, 0)
                                             : pixels.Color(0, 0, 0));
        } else {
          pixels.setPixelColor(i, pixelCycle ? pixels.Color(0, 0, 0)
                                             : pixels.Color(0, 0, 255));
        }
      }
      pixels.show();
    }
  }
}

// --- PİL OKUMA YARDIMCISI ---
// Önceki okumayı saklayacağımız global/statik değişken (Filtre için)
static float smoothedVoltage = 0.0;

int readBatteryPercentage() {
  // 1. Çoklu okuma ile anlık çapakları al (Donanımsal gürültü için)
  long sum = 0;
  for (int i = 0; i < 20; i++) {
    sum += analogRead(BATTERY_PIN);
    delay(2);
  }
  float avgAdc = sum / 20.0;

  // 2. Ham Voltaj Hesabı
  // ADC çözünürlüğü: 4095, ESP32 Çalışma Voltajı: 3.3V
  // Gerilim Bölücü: 45K + 45K = Çarpan ~2.0
  // NOT: ESP32 ADC'si referans noktasında genelde düşük okur (~%5).
  // Kalibrasyon Katsayısı (Eğer 3.8V pili az okuyorsa bu çarpanı 2.0 yerine
  // mesela 2.15 yaptık)
  float calibrationFactor =
      2.15; // KALİBRASYON: Gerektiğinde bu sayıyı değiştir
  float pinVoltage = (avgAdc / 4095.0) * 3.3;
  float currentVoltage = pinVoltage * calibrationFactor;

  // İlk okuma ise smoothedVoltage'a direkt ata
  if (smoothedVoltage == 0.0) {
    smoothedVoltage = currentVoltage;
  } else {
    // 3. LOGARİTMİK (EMA) FİLTRE
    // Yeni değerin sadece %5'ini, eski değerin %95'ini alır. Bu sayede voltaj
    // asla "zıplamaz", yumuşakça hareket eder.
    smoothedVoltage = (0.05 * currentVoltage) + (0.95 * smoothedVoltage);
  }

  // 4. Lİ-İON PİL EĞRİSİ YÜZDE HESABI
  // Li-Ion piller doğrusal deşarj olmaz. 3.8V aslında pilin yarısından (%60
  // civarı) fazladır. 4.20V = 100% 3.80V = ~60% 3.60V = ~20% 3.30V = ~0%

  float percentage = 0;
  if (smoothedVoltage >= 4.20)
    percentage = 100.0;
  else if (smoothedVoltage >= 3.80) {
    // 3.8V - 4.2V arası (%60 - %100)
    percentage = 60.0 + ((smoothedVoltage - 3.80) / (4.20 - 3.80)) * 40.0;
  } else if (smoothedVoltage >= 3.60) {
    // 3.6V - 3.8V arası (%20 - %60)
    percentage = 20.0 + ((smoothedVoltage - 3.60) / (3.80 - 3.60)) * 40.0;
  } else if (smoothedVoltage >= 3.30) {
    // 3.3V - 3.6V arası (%0 - %20)
    percentage = ((smoothedVoltage - 3.30) / (3.60 - 3.30)) * 20.0;
  } else {
    percentage = 0.0;
  }

  // Sınırlandırma
  if (percentage > 100)
    percentage = 100;
  if (percentage < 0)
    percentage = 0;

  return (int)percentage;
}

// --- EKRANA YAZI YAZDIRMA YARDIMCISI ---
void ekranaYaz(String satir1, String satir2) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 20);
  display.println(satir1);
  display.setTextSize(1);
  display.setCursor(0, 40);
  display.println(satir2);
  display.display();
}

void showIpAddressFor3Seconds() {
  ekranaYaz("IP Adresi:", WiFi.localIP().toString());
  ipDisplayUntil = millis() + IP_SHOW_DURATION_MS;
}

// Saat çizim isteği WebSocket üzerinden tetiklenir
void enableClockMode() {
  isClockModeActive = true;
  pendingClockDraw = true;
}

// Saat modundan çıkıp gözlere dön
void disableClockMode() {
  isClockModeActive = false;
  pendingClockDraw = false;
  displayLocked = false;
}

// --- DANS FONKSİYONLARI ---
void dance_1() {
  Serial.println("Dans Basladi...");
  eyes.setMood(HAPPY);
  eyes.anim_laugh();

  // Dans sırasında ışıklar coşsun
  String oldMode = lightMode;
  lightMode = "RAINBOW";

  myDFPlayer.play(1);

  ileri(80);
  vTaskDelay(1500 / portTICK_PERIOD_MS);
  dur();
  vTaskDelay(200 / portTICK_PERIOD_MS);
  moveServoTo(servoArm, 50, 2);
  moveServoTo(servoArm, 10, 7);
  geri(60);
  vTaskDelay(1500 / portTICK_PERIOD_MS);
  dur();
  moveServoTo(servoHead, 40, 9);
  moveServoTo(servoHead, 0, 9);
  moveServoTo(servoHead, 40, 9);
  moveServoTo(servoHead, 0, 9);
  moveServoTo(servoHead, 40, 9);
  moveServoTo(servoHead, 20, 9);

  Serial.println("Dans Bitti.");
  danceTrigger = 0;
  lightMode = oldMode; // Eski moda dön
}

void dance_2() {
  moveServoTo(servoHead, 40, 10);
  moveServoTo(servoHead, 0, 10);
  moveServoTo(servoHead, 40, 10);
  moveServoTo(servoHead, 0, 10);
  moveServoTo(servoHead, 40, 10);
  moveServoTo(servoHead, 0, 10);
  moveServoTo(servoHead, 20, 10);

  danceTrigger = 0;
}

void dance_3() {

  moveServoTo(servoHead, 40, 7);
  moveServoTo(servoHead, 0, 7);
  moveServoTo(servoHead, 40, 7);
  moveServoTo(servoHead, 0, 7);
  moveServoTo(servoHead, 40, 7);
  moveServoTo(servoHead, 0, 7);
  moveServoTo(servoHead, 20, 7);

  danceTrigger = 0;
}

void dance_4() {

  myDFPlayer.play(7);

  eyes.setMood(ANGRY);

  // vTaskDelay(1000 / portTICK_PERIOD_MS);

  moveServoTo(servoArm, 50, 8);
  moveServoTo(servoArm, 10, 8);

  moveServoTo(servoHead, 40, 7);
  moveServoTo(servoHead, 0, 7);
  moveServoTo(servoHead, 40, 7);
  moveServoTo(servoHead, 0, 7);
  moveServoTo(servoHead, 20, 7);

  vTaskDelay(500 / portTICK_PERIOD_MS);

  moveServoTo(servoArm, 50, 8);
  moveServoTo(servoArm, 10, 8);

  moveServoTo(servoHead, 40, 7);
  moveServoTo(servoHead, 0, 7);
  moveServoTo(servoHead, 40, 7);
  moveServoTo(servoHead, 0, 7);
  moveServoTo(servoHead, 20, 7);

  vTaskDelay(500 / portTICK_PERIOD_MS);

  moveServoTo(servoArm, 50, 8);
  moveServoTo(servoArm, 10, 8);

  moveServoTo(servoHead, 40, 7);
  moveServoTo(servoHead, 0, 7);
  moveServoTo(servoHead, 40, 7);
  moveServoTo(servoHead, 0, 7);
  moveServoTo(servoHead, 20, 7);

  danceTrigger = 0;
}
void dance_5() { 
  
  



  
  danceTrigger = 0; }
void dance_6() { danceTrigger = 0; }
void dance_7() { danceTrigger = 0; }

// --- WEBSOCKET HANDLERS ---
// Yardımcı: JSON değerini int'e güvenli çevir (string veya number olabilir)
int jsonToInt(JSONVar &obj, const char* key) {
  if (JSON.typeof(obj[key]) == "number") {
    return (int)obj[key];
  }
  // String olarak gelmiş olabilir (web arayüzü slider'lardan)
  return atoi((const char *)obj[key]);
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len &&
      info->opcode == WS_TEXT) {

    // Güvenli null-termination: Veriyi yerel buffer'a kopyala
    // (data[len]=0 yazmak buffer overflow riski taşır)
    char* jsonStr = (char*)malloc(len + 1);
    if (!jsonStr) return; // Bellek yetersiz
    memcpy(jsonStr, data, len);
    jsonStr[len] = 0;

    JSONVar myObj = JSON.parse(jsonStr);
    free(jsonStr); // Artık ihtiyaç yok

    if (JSON.typeof(myObj) == "undefined")
      return;

    // Gelen veri kontrolü (Veri eksikse çökmesin diye kontrol ekliyoruz)
    if (myObj.hasOwnProperty("arm")) {
      disableClockMode(); // Saat modunu boz
      int arm = jsonToInt(myObj, "arm");
      servoArm.write(arm);
    }
    if (myObj.hasOwnProperty("head")) {
      disableClockMode(); // Saat modunu boz
      int head = jsonToInt(myObj, "head");
      servoHead.write(map(head, 0, 60, 60, 0));
    }

    // Tank Drive — Joystick'ten gelen bireysel sol/sağ motor hızları
    // leftSpeed / rightSpeed: -255..255 (pozitif=ileri, negatif=geri)
    if (myObj.hasOwnProperty("leftSpeed") || myObj.hasOwnProperty("rightSpeed")) {
      disableClockMode();
      int leftSpeed  = myObj.hasOwnProperty("leftSpeed")  ? jsonToInt(myObj, "leftSpeed")  : 0;
      int rightSpeed = myObj.hasOwnProperty("rightSpeed") ? jsonToInt(myObj, "rightSpeed") : 0;
      setMotorSol(leftSpeed);
      setMotorSag(rightSpeed);
    }

    // Klasik Yön Kontrolü — Manuel UI ve playback uyumu için korundu
    if (myObj.hasOwnProperty("direction")) {
      String direction = (const char *)myObj["direction"];
      int speed = myObj.hasOwnProperty("speed") ? jsonToInt(myObj, "speed") : 150;
      if (direction != "STOP")
        disableClockMode();

      if (direction == "FORWARD")
        ileri(speed);
      else if (direction == "BACKWARD")
        geri(speed);
      else if (direction == "RIGHT")
        sag(speed);
      else if (direction == "LEFT")
        sol(speed);
      else if (direction == "STOP")
        dur();
    }

    // ===============================================================
    // AI SEMANTİK KOMUTLARI (1-5 hız seviyesi + cm/mm/derece birimleri)
    // Tarayıcı sadece niyeti gönderir, dönüşümler burada yapılır.
    // ===============================================================

    // AI servo (yumuşak hareket): kol veya kafa, açı veya mm cinsinden
    //   {"aiServo":"arm","target_angle":40,"speed_level":3}
    //   {"aiServo":"arm","target_mm":80,"speed_level":3}
    //   {"aiServo":"head","target_angle":30,"speed_level":3}
    if (myObj.hasOwnProperty("aiServo")) {
      disableClockMode();
      String s = (const char *)myObj["aiServo"];
      int level = clampSpeedLevel(
          myObj.hasOwnProperty("speed_level") ? jsonToInt(myObj, "speed_level")
                                              : DEFAULT_SPEED_LEVEL);
      int hiz = SERVO_SPEED_LEVEL[level];
      bool hasTarget = false;
      int target = 0;
      if (myObj.hasOwnProperty("target_mm")) {
        float mm = (float)jsonToInt(myObj, "target_mm");
        target = (int)roundf(mm / ARM_FULL_MM * ARM_FULL_ANGLE);
        hasTarget = true;
      } else if (myObj.hasOwnProperty("target_angle")) {
        target = jsonToInt(myObj, "target_angle");
        hasTarget = true;
      }
      if (hasTarget) {
        if (s == "arm") {
          target = constrain(target, 0, (int)ARM_FULL_ANGLE);
          moveServoTo(servoArm, target, hiz);
        } else if (s == "head") {
          target = constrain(target, 0, HEAD_LOGIC_MAX);
          moveServoTo(servoHead, map(target, 0, HEAD_LOGIC_MAX, HEAD_LOGIC_MAX, 0),
                   hiz);
        }
      }
    }

    // AI anlık motor: {"aiMove":"FORWARD","speed_level":3}
    // STOP da burada işlenir; bekleyen zamanlanmış hareketi iptal eder.
    if (myObj.hasOwnProperty("aiMove")) {
      String d = (const char *)myObj["aiMove"];
      if (d != "STOP")
        disableClockMode();
      int level = clampSpeedLevel(
          myObj.hasOwnProperty("speed_level") ? jsonToInt(myObj, "speed_level")
                                              : DEFAULT_SPEED_LEVEL);
      int pwm = MOTOR_PWM_LEVEL[level];
      timedMoveActive = false; // varsa zamanlayıcı iptal
      if (d == "FORWARD")
        ileri(pwm);
      else if (d == "BACKWARD")
        geri(pwm);
      else if (d == "RIGHT")
        sag(pwm);
      else if (d == "LEFT")
        sol(pwm);
      else if (d == "STOP")
        dur();
    }

    // AI mesafeli düz hareket: {"aiMoveDistance":"FORWARD","distance_cm":10,"speed_level":4}
    // CPP süreyi cm/cm_per_sec ile hesaplar, otomatik durur.
    if (myObj.hasOwnProperty("aiMoveDistance")) {
      disableClockMode();
      String d = (const char *)myObj["aiMoveDistance"];
      float cm = (float)jsonToInt(myObj, "distance_cm");
      int level = clampSpeedLevel(
          myObj.hasOwnProperty("speed_level") ? jsonToInt(myObj, "speed_level")
                                              : DEFAULT_SPEED_LEVEL);
      int pwm = MOTOR_PWM_LEVEL[level];
      float cmps = MOVE_CM_PER_SEC[level];
      if (cmps > 0.01f && cm > 0.0f && (d == "FORWARD" || d == "BACKWARD")) {
        unsigned long duration = (unsigned long)(cm / cmps * 1000.0f);
        if (duration < 50UL) duration = 50UL;
        if (duration > 10000UL) duration = 10000UL;
        if (d == "FORWARD") ileri(pwm);
        else                geri(pwm);
        timedMoveStopAt = millis() + duration;
        timedMoveActive = true;
      }
    }

    // AI dereceli dönüş: {"aiTurn":"LEFT","degrees":90,"speed_level":4}
    // CPP süreyi deg/deg_per_sec ile hesaplar, otomatik durur.
    if (myObj.hasOwnProperty("aiTurn")) {
      disableClockMode();
      String d = (const char *)myObj["aiTurn"];
      float deg = (float)jsonToInt(myObj, "degrees");
      int level = clampSpeedLevel(
          myObj.hasOwnProperty("speed_level") ? jsonToInt(myObj, "speed_level")
                                              : DEFAULT_SPEED_LEVEL);
      int pwm = MOTOR_PWM_LEVEL[level];
      float dps = TURN_DEG_PER_SEC[level];
      if (dps > 0.01f && deg > 0.0f && (d == "LEFT" || d == "RIGHT")) {
        unsigned long duration = (unsigned long)(deg / dps * 1000.0f);
        if (duration < 50UL) duration = 50UL;
        if (duration > 10000UL) duration = 10000UL;
        if (d == "LEFT") sol(pwm);
        else             sag(pwm);
        timedMoveStopAt = millis() + duration;
        timedMoveActive = true;
      }
    }

    // AI süreli hareket: {"aiTimedMove":"FORWARD","duration_ms":1500,"speed_level":3}
    // Belirtilen süre boyunca hareket et, sonra dur.
    if (myObj.hasOwnProperty("aiTimedMove")) {
      disableClockMode();
      String d = (const char *)myObj["aiTimedMove"];
      unsigned long duration =
          (unsigned long)jsonToInt(myObj, "duration_ms");
      int level = clampSpeedLevel(
          myObj.hasOwnProperty("speed_level") ? jsonToInt(myObj, "speed_level")
                                              : DEFAULT_SPEED_LEVEL);
      int pwm = MOTOR_PWM_LEVEL[level];
      if (duration < 50UL) duration = 50UL;
      if (duration > 10000UL) duration = 10000UL;
      if (d == "FORWARD")       ileri(pwm);
      else if (d == "BACKWARD") geri(pwm);
      else if (d == "LEFT")     sol(pwm);
      else if (d == "RIGHT")    sag(pwm);
      else                      duration = 0;
      if (duration > 0) {
        timedMoveStopAt = millis() + duration;
        timedMoveActive = true;
      }
    }

    // Ses Kontrolü
    if (myObj.hasOwnProperty("volume")) {
      disableClockMode(); // Saat modunu boz
      int vol = jsonToInt(myObj, "volume");
      if (vol >= 0 && vol <= 30) {
        myDFPlayer.volume(vol);
        Serial.print("Ses seviyesi ayarlandı: ");
        Serial.println(vol);
      }
    }

    // Mood Kontrolü
    if (myObj.hasOwnProperty("mood")) {
      String mood = (const char *)myObj["mood"];
      if (mood != "DEFAULT")
        disableClockMode(); // Normal poza dönünce değil, animasyona geçince
                            // saati boz

      if (mood == "RANDOM")
        randORdefault = true;
      else {
        randORdefault = false;
        if (mood == "HAPPY") {
          eyes.setMood(HAPPY);
          eyes.anim_laugh();
          myDFPlayer.play(1);
        } else if (mood == "ANGRY") {
          eyes.setMood(ANGRY);
          myDFPlayer.play(2);
        } else if (mood == "TIRED") {
          eyes.setMood(TIRED);
          myDFPlayer.play(3);
        } else if (mood == "CONFUSED") {
          eyes.setMood(DEFAULT);
          eyes.anim_confused();
          myDFPlayer.play(4);
        } else if (mood == "DEFAULT")
          eyes.setMood(DEFAULT);
        else if (mood.startsWith("DANCE_")) {
          // "DANCE_1" -> char at 6 is '1' -> '1'-'0' = 1 (int)
          int dNum = mood.charAt(6) - '0';
          danceTrigger = dNum;
        }
      }
    }

    // *** RGB KONTROLÜ ***
    if (myObj.hasOwnProperty("lightMode")) {
      lightMode = (const char *)myObj["lightMode"];

      // Eğer statik renk veya pulse ise rengi al
      if (myObj.hasOwnProperty("color")) {
        String hexColor = (const char *)myObj["color"]; // "#RRGGBB"
        if (hexColor.length() == 7) {
          long number = strtol(&hexColor[1], NULL, 16);
          int r = number >> 16;
          int g = number >> 8 & 0xFF;
          int b = number & 0xFF;
          selectedColor = pixels.Color(r, g, b);
        }
      }

      Serial.print("LED Mode: ");
      Serial.println(lightMode);
    }

    // *** WEB ARAYÜZÜ KOMUTLARI ***
    if (myObj.hasOwnProperty("command")) {
      String cmd = (const char *)myObj["command"];
      if (cmd == "showClock") {
        enableClockMode();
      } else if (cmd == "forgetWifi") {
        // Kullanıcı UI'dan "Ağı Unut"a bastı: kreden temizleyip resetle.
        // Asenkron yapıyoruz ki WS yanıtı bitebilsin, sonra Task_Mantik
        // güvenli bir noktada gerçekleştirsin.
        resetWifiPending = true;
        Serial.println("[WiFi] forgetWifi istendi.");
      }
    }

      if (myObj.hasOwnProperty("modeCommand")) {
        String modeCmd = (const char *)myObj["modeCommand"];
        if (modeCmd == "AI") {
          isAIModeActive = true;
          speakerTestPending = true;
          lightMode = "PULSE";
          selectedColor = pixels.Color(188, 19, 254);
          Serial.println("AI Moduna Gecildi!");
        } else if (modeCmd == "NORMAL") {
          isAIModeActive = false;
          lightMode = "OFF";
          Serial.println("Normal Moda Donuldu!");
        }
      }
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
             AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->opcode == WS_BINARY) {
      // Tarayıcıdan gelen AI ses verisi doğrudan ring buffer'a
      if (isAIModeActive && audio_ringbuf != NULL) {
        xRingbufferSend(audio_ringbuf, data, len, pdMS_TO_TICKS(10));
      }
    } else {
      handleWebSocketMessage(arg, data, len);
    }
  }
}

// =========================================================================
// GÖREVLER
// =========================================================================
void Task_Gozler(void *parameter) {
  for (;;) {
    // EĞER EKRAN KİLİTLİYSE (IPveya Saat YAZIYORSA) VEYA AP MODUNDAYSAN BURADA BEKLE VE ÇİZİM YAPMA
    if (displayLocked || isAPMode) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    // Normal çizim döngüsü
    eyes.update();
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}


static void playSpeakerTestBeep() {
  // Kablolama/pin dogrulama icin kisa bip (PCM16, stereo)
  const uint32_t rate = speaker_sample_rate ? speaker_sample_rate : SPEAKER_SAMPLE_RATE;
  const int freq_hz = 1000;
  const int duration_ms = 150;
  const int16_t amp = 6000;

  int total_samples = (int)((rate * (uint32_t)duration_ms) / 1000U);
  int period = (int)(rate / (uint32_t)freq_hz);
  if (period < 2) period = 2;
  int half = period / 2;

  int16_t stereo_buffer[512]; // 256 sample * 2ch
  int sample_index = 0;
  while (sample_index < total_samples) {
    int chunk = min(256, total_samples - sample_index);
    for (int i = 0; i < chunk; i++) {
      int pos = (sample_index + i) % period;
      int16_t v = (pos < half) ? amp : (int16_t)-amp;
      stereo_buffer[i * 2] = v;
      stereo_buffer[i * 2 + 1] = v;
    }

    size_t bytes_written = 0;
    i2s_write(I2S_SPEAKER_PORT, stereo_buffer, chunk * 4, &bytes_written,
              portMAX_DELAY);
    sample_index += chunk;
  }
}

// AI Audio Cikttisi (Hoparlor) GOREVI
void Task_AI_Speaker(void *parameter) {
  for(;;) {
    if (audio_ringbuf != NULL) {
      size_t item_size;

      if (speakerTestPending && isAIModeActive) {
        speakerTestPending = false;
        playSpeakerTestBeep();
      }

      // 2048 byte'a kadar veri al (daha buyuk parcalar = daha az kesinti)
      void *item = xRingbufferReceiveUpTo(audio_ringbuf, &item_size, pdMS_TO_TICKS(100), 2048);
      
      if (item != NULL) {
        if (isAIModeActive) {
          size_t bytes_written;

          // PCM16 mono gelirse stereo frame'e kopyala (up-mix)
          int16_t* pcm_mono = (int16_t*)item;
          int num_samples = item_size / 2;
          int16_t stereo_buffer[1024]; // 512 sample * 2ch (2048 byte'a uygun)

          int offset = 0;
          while(offset < num_samples) {
              int chunk = min(512, num_samples - offset);
              for(int i = 0; i < chunk; i++) {
                  // Kazanç + tanh yumuşak doyum + SPEAKER_SAFE_PEAK ile zirve sınırı.
                  // sat fonksiyonu -1..1 aralığında doyduğundan çıkış ±SPEAKER_SAFE_PEAK'i
                  // hiçbir koşulda geçemez. Hoparlör 1W güvenli sınırının altında kalır.
                  float norm = pcm_mono[offset + i] / 32767.0f;
                  float amplified = norm * aiSpeakerGain;
                  float sat = amplified / sqrtf(1.0f + amplified * amplified); // hızlı tanh
                  int16_t s = (int16_t)(sat * (float)SPEAKER_SAFE_PEAK);
                  stereo_buffer[i * 2]     = s; // Left
                  stereo_buffer[i * 2 + 1] = s; // Right
              }
              i2s_write(I2S_SPEAKER_PORT, stereo_buffer, chunk * 4, &bytes_written, portMAX_DELAY);
              offset += chunk;
          }
        }

        vRingbufferReturnItem(audio_ringbuf, item); // AI modu kapaliyken de iade et (ringbuffer kilitlenmesin)
      }
    } else {
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }
  }
}

void Task_Network(void *parameter) {
  for (;;) {
    if (isAPMode) {
      dnsServer.processNextRequest();
    }
    ws.cleanupClients();
    ArduinoOTA.handle();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void Task_Mantik(void *parameter) {
  for (;;) {
    unsigned long currentMillis = millis();

    // 0. AI Zamanlanmış Hareket Sonu (move_distance / turn_degrees /
    //    timed_move). Süre dolduğunda motorları durdur.
    if (timedMoveActive && (long)(currentMillis - timedMoveStopAt) >= 0) {
      timedMoveActive = false;
      dur();
    }

    // 1. Dokunmatik
    if (digitalRead(THC_PIN)) {
      if (touchPressStartMillis == 0) {
        touchPressStartMillis = currentMillis;
        ipShownForCurrentPress = false;
      }

      if (!touchTrigger) {
        Serial.println("Touch Triggered!");
        eyes.setMood(HAPPY);
        eyes.anim_laugh();
        touchTrigger = true;

        // Dokununca yeşil yansın
        String oldMode = lightMode;
        lightMode = "STATIC";
        selectedColor = pixels.Color(0, 255, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        lightMode = oldMode;
      }

      // 3 Saniye (3000ms) basılı tutulursa
      if (!ipShownForCurrentPress &&
          (currentMillis - touchPressStartMillis >= 3000)) {
        Serial.println("Touch hold 5s -> IP gosteriliyor.");

        displayLocked = true; // 1. Göz Görevini Kilitle (Durulsun)
        vTaskDelay(100 / portTICK_PERIOD_MS); // 2. Gözlerin son karesinin
                                              // bitmesi için minik bekleme

        // 3. Ekranı temizle ve IP'yi yaz
        ekranaYaz("IP Adresi:", WiFi.localIP().toString());

        // 4. IP'nin okunabilmesi için 4 saniye burada beklet (Task_Mantik durur
        // ama sorun olmaz)
        vTaskDelay(4000 / portTICK_PERIOD_MS);

        displayLocked = false;         // 5. Kilidi aç, gözler geri gelsin
        ipShownForCurrentPress = true; // 6. Elini çekene kadar tekrar gösterme
      }
      // --------------------------

    } else {
      touchTrigger = false;
      touchPressStartMillis = 0;
      ipShownForCurrentPress = false;
    }

    // 2. Dans İsteği
    if (danceTrigger > 0) {
      if (danceTrigger == 1)
        dance_1();
      else if (danceTrigger == 2)
        dance_2();
      else if (danceTrigger == 3)
        dance_3();
      else if (danceTrigger == 4)
        dance_4();
      else if (danceTrigger == 5)
        dance_5();
      else if (danceTrigger == 6)
        dance_6();
      else if (danceTrigger == 7)
        dance_7();
    }

    // 3. Random Mood
    if (randORdefault && (currentMillis - lastMillis >= interval)) {
      lastMillis = currentMillis;
      if (isMoodActive) {
        eyes.setMood(DEFAULT);
        isMoodActive = false;
      } else {
        int randomMood = random(0, 4);
        switch (randomMood) {
        case 0:
          eyes.setMood(TIRED);
          myDFPlayer.play(3);
          break;
        case 1:
          eyes.setMood(ANGRY);
          myDFPlayer.play(2);
          break;
        case 2:
          eyes.setMood(HAPPY);
          eyes.anim_laugh();
          myDFPlayer.play(1);
          break;
        case 3:
          eyes.anim_confused();
          myDFPlayer.play(4);
          break;
        }
        isMoodActive = true;
      }
      interval = random(4000, 20000);
    }

    // *** SAAT ÇİZİMİ (SÜREKLİ GÜNCELLEME + I2C GÜVENLİĞİ) ***
    static unsigned long lastClockUpdate = 0;
    if (isClockModeActive) {
      if (pendingClockDraw || (currentMillis - lastClockUpdate >= 1000)) {
        pendingClockDraw = false;
        lastClockUpdate = currentMillis;

        displayLocked = true; // Gözleri kilitle
        vTaskDelay(
            100 /
            portTICK_PERIOD_MS); // Mevcut göz kare çiziminin bitmesini bekle

        struct tm timeinfo;
        if (!getLocalTime(&timeinfo)) {
          Serial.println("Saati alirken hata olustu");
          ekranaYaz("Saat Hatasi", "Baglanti Yok");
        } else {
          char timeStringBuff[10]; // 14:30
          strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M", &timeinfo);

          display.clearDisplay();
          display.setTextSize(4);
          display.setTextColor(SH110X_WHITE);
          display.setCursor(4, 16);
          display.println(timeStringBuff);
          display.display();
        }
      }
    }

    // *** 4. LED GÜNCELLEME ***
    // AI modunda LED guncellemeyi seyrekleştir (her 100ms yerine her 200ms)
    // CPU/WiFi kaynaklarini ses akisina oncelik ver
    static unsigned long lastLedUpdate = 0;
    if (!isAIModeActive || (currentMillis - lastLedUpdate >= 200)) {
      lastLedUpdate = currentMillis;
      updateLEDs();
    }

    // *** 5. PİL YÜZDESİNİ WEBSOCKET İLE GÖNDERME ***
    // AI modunda pil okuma atlanir (40ms delay + ADC kullanimi gereksiz yuk)
    static unsigned long lastBatteryUpdate = 0;
    if (!isAIModeActive && currentMillis - lastBatteryUpdate >= 5000) {
      lastBatteryUpdate = currentMillis;
      if (ws.count() > 0) {
        int batPercent = readBatteryPercentage();
        char msg[32];
        snprintf(msg, sizeof(msg), "{\"battery\": %d}", batPercent);
        ws.textAll(msg);
      }
    }

    // *** 6. WIFI'Yİ UNUT İSTEĞİ ***
    if (resetWifiPending) {
      resetWifiPending = false;
      ekranaYaz("WiFi Sifirlandi", "Yeniden basliyor");
      WiFi.disconnect(true, true);
      vTaskDelay(800 / portTICK_PERIOD_MS);
      ESP.restart();
    }

    if (rebootPending) {
      rebootPending = false;
      ekranaYaz("Aga Baglaniliyor", "Yeniden basliyor");
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      ESP.restart();
    }

    vTaskDelay(20 / portTICK_PERIOD_MS); // Döngü hızı (50 fps)
  }
}

// =========================================================================
// SETUP VE ALT FONKSIYONLAR
// =========================================================================

void initI2S() {
  // --- HOPARLÖR İÇİN (I2S_NUM_1) ---
  i2s_config_t speaker_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SPEAKER_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, 
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };
  
  i2s_pin_config_t speaker_pins = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_SPEAKER_PORT, &speaker_config, 0, NULL);
  i2s_set_pin(I2S_SPEAKER_PORT, &speaker_pins);
  i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
}

void setup() {
  Serial.begin(115200);

  // Ses icin 20KB gecici kopru bellek (RingBuffer) - ~416ms ses tamponu
  // 16KB (340ms) kesilmelere yol aciyordu, 32KB ise WiFi heap'ini kirdi
  audio_ringbuf = xRingbufferCreate(20480, RINGBUF_TYPE_BYTEBUF);

  // I2S Donanımlarını (Amfi ve I2S Mikrofon) Başlat
  initI2S();

  // RGB Init
  pixels.begin();
  pixels.setBrightness(150); // Genel parlaklık (0-255)
  pixels.show();             // Başlangıçta hepsi kapalı

  pinMode(IN4, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN1, OUTPUT);
  pinMode(THC_PIN, INPUT);

  Serial2.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  if (myDFPlayer.begin(Serial2)) {
    myDFPlayer.volume(30);
  } else {
    Serial.println("DFPlayer Error");
  }

  servoArm.attach(servoArmPin, 500, 2400);
  servoHead.attach(servoHeadPin, 500, 2400);

  display.begin(i2c_Address, true);
  eyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
  eyes.setAutoblinker(ON, 3, 2);
  eyes.setIdleMode(ON, 2, 2);
  eyes.setCuriosity(ON);

  ekranaYaz("WiFi Agina", "Baglaniyor...");

  if (!SPIFFS.begin(true))
    Serial.println("SPIFFS Error");

  // Açılış efekti (Kırmızı Yükleniyor)
  pixels.fill(pixels.Color(255, 0, 0));
  pixels.show();

  // --- KENDİ AP-STA YÖNETİMİMİZ ---
  WiFi.mode(WIFI_STA);
  WiFi.begin();

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    retries++;
    ekranaYaz("Baglaniliyor...", String(retries) + ". Deneme");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    isAPMode = false;
    pixels.fill(pixels.Color(0, 255, 0));
    pixels.show();
    delay(500);
    pixels.clear();
    pixels.show();

    Serial.println("");
    Serial.println(WiFi.localIP());
    showIpAddressFor3Seconds();
    delay(IP_SHOW_DURATION_MS);
  } else {
    ekranaYaz("Baglanilamadi!", "AP'ye Geciliyor");
    delay(2000);

    isAPMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());

    pixels.fill(pixels.Color(0, 0, 255));
    pixels.show();
    ekranaYaz(String("Ag:") + AP_SSID, String("Sifre:") + AP_PASS);
    Serial.println("[WiFi] AP modu acildi: " + String(AP_SSID));
  }

  if (!isAPMode) {
    if (MDNS.begin("robot")) {
      Serial.println("mDNS: http://robot.local");
    }
    // NTP sadece internet varken çalışır
    configTime(10800, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("NTP Saati ayarlandi.");
  }

  // --- API ROUTELARI (WIFI SETUP İÇİN) ---
  server.on("/api/wifi-scan", HTTP_GET, [](AsyncWebServerRequest *request){
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) {
      WiFi.scanNetworks(true); // Asenkron tarama baslat
      request->send(202, "application/json", "[]");
    } else if (n == WIFI_SCAN_RUNNING) {
      request->send(202, "application/json", "[]");
    } else {
      String json = "[";
      for (int i = 0; i < n; ++i) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"secure\":" + String((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "false" : "true");
        json += "}";
      }
      json += "]";
      request->send(200, "application/json", json);
      WiFi.scanDelete(); // Bir sonraki istek icin resetle
    }
  });

  server.on("/api/wifi-connect", HTTP_POST, [](AsyncWebServerRequest *request){
    if(request->hasParam("ssid", true) && request->hasParam("pass", true)){
      String ssid = request->getParam("ssid", true)->value();
      String pass = request->getParam("pass", true)->value();
      
      WiFi.begin(ssid.c_str(), pass.c_str());
      request->send(200, "text/plain", "OK");
      rebootPending = true; // Reboot edilecek ama bilgiler SİLİNMEYECEK
    } else {
      request->send(400, "text/plain", "Bad Request");
    }
  });

  // Captive Portal yonlendirmesi
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (isAPMode) {
      request->redirect("http://192.168.4.1/wifi.html");
    } else {
      request->send(404, "text/plain", "Not found");
    }
  });

  // Root URL: AP modunda wifi.html, STA modunda index.html goster
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (isAPMode) {
      request->send(SPIFFS, "/wifi.html", "text/html");
    } else {
      request->send(SPIFFS, "/index.html", "text/html");
    }
  });

  ws.onEvent(onEvent);
  server.addHandler(&ws);
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  server.begin();
  ArduinoOTA.begin();

  xTaskCreatePinnedToCore(Task_Gozler, "TaskGozler", 10000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(Task_Network, "TaskNetwork", 10000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(Task_Mantik, "TaskMantik", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Task_AI_Speaker, "TaskAISpeaker", 10000, NULL, 2, NULL, 1);

  myDFPlayer.play(5);
}

void loop() { vTaskDelay(1000 / portTICK_PERIOD_MS); }
