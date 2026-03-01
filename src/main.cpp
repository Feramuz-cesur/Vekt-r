#include "DFRobotDFPlayerMini.h"
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
#include <FluxGarage_RoboEyes.h>
#include <WiFi.h>
#include <Wire.h>

// RGB Kütüphanesi
#include <Adafruit_NeoPixel.h>

// --- PIN TANIMLAMALARI ---
#define IN1 32
#define IN2 33
#define IN3 25
#define IN4 26
#define THC_PIN 13
const int servoPin1 = 27;
const int servoPin2 = 14;
#define RX_PIN 4
#define TX_PIN 5

// *** YENİ RGB PIN TANIMI ***
#define RGB_PIN 23  // NeoPixel Data Pini
#define NUMPIXELS 8 // Kaç adet LED var? (Burayı sayına göre değiştir)

// --- NESNELER ---
DFRobotDFPlayerMini myDFPlayer;
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define i2c_Address 0x3c
Adafruit_SH1106G display =
    Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
RoboEyes<Adafruit_SH1106G> eyes(display);

Servo servo1, servo2;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// RGB Nesnesi
Adafruit_NeoPixel pixels(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

// --- GLOBAL DEĞİŞKENLER ---
const char *ssid = "FiberHGW_TPB9C0";
const char *password = "NVArVrUNL3Ap";

volatile bool displayLocked = false; // Ekran kilitli mi?

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

// --- SERVO FONKSİYONU ---

/**
 * @brief Servo motor kontrol fonksiyonu.
 *
 * @param s          Servo referansı
 * @param hedefAci   [0..180] derece
 * @param hiz        [0..10]
 *                   1 = minimum hız ,
 *                   10 = maksimum hız
 */
void servoGit(Servo &s, int hedefAci, int hiz) {
  hedefAci = constrain(hedefAci, 0, 180);
  hiz = constrain(hiz, 1, 10);

  int baslangicAci = s.read();

  int step = map(hiz, 1, 10, 1, 5); // hız arttıkça adım büyür
  int bekleme = 10;                 // sabit gecikme

  if (baslangicAci < hedefAci) {
    for (int i = baslangicAci; i <= hedefAci; i += step) {
      s.write(i);
      vTaskDelay(bekleme / portTICK_PERIOD_MS);
    }
  } else {
    for (int i = baslangicAci; i >= hedefAci; i -= step) {
      s.write(i);
      vTaskDelay(bekleme / portTICK_PERIOD_MS);
    }
  }
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
  servoGit(servo1, 50, 2);
  servoGit(servo1, 10, 7);
  geri(60);
  vTaskDelay(1500 / portTICK_PERIOD_MS);
  dur();
  servoGit(servo2, 40, 9);
  servoGit(servo2, 0, 9);
  servoGit(servo2, 40, 9);
  servoGit(servo2, 0, 9);
  servoGit(servo2, 40, 9);
  servoGit(servo2, 20, 9);

  Serial.println("Dans Bitti.");
  danceTrigger = 0;
  lightMode = oldMode; // Eski moda dön
}

void dance_2() {
  servoGit(servo2, 40, 10);
  servoGit(servo2, 0, 10);
  servoGit(servo2, 40, 10);
  servoGit(servo2, 0, 10);
  servoGit(servo2, 40, 10);
  servoGit(servo2, 0, 10);
  servoGit(servo2, 20, 10);

  danceTrigger = 0;
}

void dance_3() {

  servoGit(servo2, 40, 7);
  servoGit(servo2, 0, 7);
  servoGit(servo2, 40, 7);
  servoGit(servo2, 0, 7);
  servoGit(servo2, 40, 7);
  servoGit(servo2, 0, 7);
  servoGit(servo2, 20, 7);

  danceTrigger = 0;
}

void dance_4() {

  myDFPlayer.play(7);

  eyes.setMood(ANGRY);

  // vTaskDelay(1000 / portTICK_PERIOD_MS);

  servoGit(servo1, 50, 8);
  servoGit(servo1, 10, 8);

  servoGit(servo2, 40, 7);
  servoGit(servo2, 0, 7);
  servoGit(servo2, 40, 7);
  servoGit(servo2, 0, 7);
  servoGit(servo2, 20, 7);

  vTaskDelay(500 / portTICK_PERIOD_MS);

  servoGit(servo1, 50, 8);
  servoGit(servo1, 10, 8);

  servoGit(servo2, 40, 7);
  servoGit(servo2, 0, 7);
  servoGit(servo2, 40, 7);
  servoGit(servo2, 0, 7);
  servoGit(servo2, 20, 7);

  vTaskDelay(500 / portTICK_PERIOD_MS);

  servoGit(servo1, 50, 8);
  servoGit(servo1, 10, 8);

  servoGit(servo2, 40, 7);
  servoGit(servo2, 0, 7);
  servoGit(servo2, 40, 7);
  servoGit(servo2, 0, 7);
  servoGit(servo2, 20, 7);

  danceTrigger = 0;
}
void dance_5() { danceTrigger = 0; }
void dance_6() { danceTrigger = 0; }
void dance_7() { danceTrigger = 0; }

// --- WEBSOCKET HANDLERS ---
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len &&
      info->opcode == WS_TEXT) {
    data[len] = 0;
    JSONVar myObj = JSON.parse((char *)data);
    if (JSON.typeof(myObj) == "undefined")
      return;

    // Gelen veri kontrolü (Veri eksikse çökmesin diye kontrol ekliyoruz)
    if (myObj.hasOwnProperty("arm")) {
      disableClockMode(); // Saat modunu boz
      int arm = atoi((const char *)myObj["arm"]);
      servo1.write(arm);
    }
    if (myObj.hasOwnProperty("head")) {
      disableClockMode(); // Saat modunu boz
      int head = atoi((const char *)myObj["head"]);
      servo2.write(map(head, 0, 60, 60, 0));
    }

    // Motor Kontrolü
    if (myObj.hasOwnProperty("direction") && myObj.hasOwnProperty("speed")) {
      String direction = (const char *)myObj["direction"];
      int speed = atoi((const char *)myObj["speed"]);
      if (direction != "STOP")
        disableClockMode(); // Sadece yön değiştiğinde boz

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

    // Ses Kontrolü
    if (myObj.hasOwnProperty("volume")) {
      disableClockMode(); // Saat modunu boz
      int vol = atoi((const char *)myObj["volume"]);
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
      }
    }
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
             AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DATA)
    handleWebSocketMessage(arg, data, len);
}

// =========================================================================
// GÖREVLER
// =========================================================================
void Task_Gozler(void *parameter) {
  for (;;) {
    // EĞER EKRAN KİLİTLİYSE (IPveya Saat YAZIYORSA) BURADA BEKLE VE ÇİZİM YAPMA
    if (displayLocked) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    // Normal çizim döngüsü
    eyes.update();
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

void Task_Network(void *parameter) {
  for (;;) {
    ws.cleanupClients();
    ArduinoOTA.handle();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void Task_Mantik(void *parameter) {
  for (;;) {
    unsigned long currentMillis = millis();

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
    updateLEDs();

    vTaskDelay(20 / portTICK_PERIOD_MS); // Döngü hızı (50 fps)
  }
}

// =========================================================================
// SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);

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

  servo1.attach(servoPin1, 500, 2400);
  servo2.attach(servoPin2, 500, 2400);

  display.begin(i2c_Address, true);
  eyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
  eyes.setAutoblinker(ON, 3, 2);
  eyes.setIdleMode(ON, 2, 2);
  eyes.setCuriosity(ON);

  ekranaYaz("WiFi Agina", "Baglaniyor...");

  if (!SPIFFS.begin(true))
    Serial.println("SPIFFS Error");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // Açılış efekti (Kırmızı Yükleniyor)
  pixels.fill(pixels.Color(255, 0, 0));
  pixels.show();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  // Bağlanınca Yeşil
  pixels.fill(pixels.Color(0, 255, 0));
  pixels.show();
  delay(500);
  pixels.clear();
  pixels.show();

  Serial.println("");
  Serial.println(WiFi.localIP());

  // --- NTP SAAT AYARI ---
  // GMT_OFFSET = 10800 (Türkiye için 3 saat x 3600 sn)
  // DAYLIGHT_OFFSET = 0 (Türkiye'de yaz saati ukygulaması sabit)
  configTime(10800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP Saati ayarlandi.");

  showIpAddressFor3Seconds();
  delay(IP_SHOW_DURATION_MS);

  ws.onEvent(onEvent);
  server.addHandler(&ws);
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  server.begin();
  ArduinoOTA.begin();

  xTaskCreatePinnedToCore(Task_Gozler, "TaskGozler", 10000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(Task_Network, "TaskNetwork", 10000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(Task_Mantik, "TaskMantik", 10000, NULL, 1, NULL, 1);

  myDFPlayer.play(5);
}

void loop() { vTaskDelay(1000 / portTICK_PERIOD_MS); }
