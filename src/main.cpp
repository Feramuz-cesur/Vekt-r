#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include <Arduino_JSON.h>
#include <ESP32Servo.h>
#include <ArduinoOTA.h> 
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "DFRobotDFPlayerMini.h"
#include <FluxGarage_RoboEyes.h>

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

// --- NESNELER ---
DFRobotDFPlayerMini myDFPlayer;
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define i2c_Address 0x3c
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
RoboEyes<Adafruit_SH1106G> eyes(display);

Servo servo1, servo2;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// --- GLOBAL DEĞİŞKENLER ---
const char *ssid = "FiberHGW_TPB9C0";
const char *password = "NVArVrUNL3Ap";

// Görevler arasında veri paylaşımı için "volatile" veya thread-safe yapılar kullanılır
volatile int danceTrigger = 0; 
volatile bool touchTrigger = false;
unsigned long lastMillis = 0;
int long interval = 18000;
bool isMoodActive = false;
bool randORdefault = false;


// --- MOTOR FONKSİYONLARI ---
void ileri(int pwm) {
  digitalWrite(IN1, LOW); digitalWrite(IN3, LOW);
  analogWrite(IN2, pwm); analogWrite(IN4, pwm);
}
void geri(int pwm) {
  digitalWrite(IN1, HIGH); digitalWrite(IN3, HIGH);
  analogWrite(IN2, 254 - pwm); analogWrite(IN4, 254 - pwm);
}
void sag(int pwm) {
  digitalWrite(IN1, LOW); digitalWrite(IN3, HIGH);
  analogWrite(IN2, pwm); analogWrite(IN4, 254 - pwm);
}
void sol(int pwm) {
  digitalWrite(IN1, HIGH); digitalWrite(IN3, LOW);
  analogWrite(IN2, 254 - pwm); analogWrite(IN4, pwm);
}
void dur() {
  digitalWrite(IN1, LOW); digitalWrite(IN3, LOW);
  analogWrite(IN2, 0); analogWrite(IN4, 0);
}


void servoGit(Servo &s, int hedefAci, int hiz) {
  if (hiz <= 0) {
    s.write(hedefAci);
    return;
  }

  // Hız güvenliği: Çok yavaşlatıp sistemi kilitlemeyelim
  // Max bekleme süresini 40ms ile sınırlayalım
  int bekleme = map(hiz, 1, 10, 40, 5); 
  
  int baslangicAci = s.read();
  
  // Eğer okunan açı saçma bir değerse (örn: servo takılı değilse) varsayılan ata
  if (baslangicAci > 180 || baslangicAci < 0) baslangicAci = 90; 

  if (baslangicAci < hedefAci) {
    for (int i = baslangicAci; i <= hedefAci; i++) {
      s.write(i);
       vTaskDelay(bekleme / portTICK_PERIOD_MS); 
      // Ekstra güvenlik için buraya da yield koyabilirsin ama smartDelay içindeki yeterli olur.
    }
  } 
  else {
    for (int i = baslangicAci; i >= hedefAci; i--) {
      s.write(i);
       vTaskDelay(bekleme / portTICK_PERIOD_MS); 
    }
  }
}

// --- EKRANA YAZI YAZDIRMA YARDIMCISI ---
void ekranaYaz(String satir1, String satir2) {
  display.clearDisplay(); // Ekranı temizle
  display.setTextSize(1); // Yazı boyutu (1: Küçük, 2: Orta)
  display.setTextColor(SH110X_WHITE); // Yazı rengi
  
  // 1. Satırı yaz
  display.setCursor(0, 20); // Konum (x, y)
  display.println(satir1);
  
  // 2. Satırı yaz
  display.setTextSize(1); // İstersen IP adresi sığsın diye boyutu 1 tutabilirsin
  display.setCursor(0, 40);
  display.println(satir2);
  
  display.display(); // Değişiklikleri ekrana bas
}

// --- DANS FONKSİYONU (Blocking olmayan versiyon) ---
// Bu fonksiyon artık Task_Mantik içinde çağrılacak
void dance_1() {
  Serial.println("Dans Basladi...");
  eyes.setMood(HAPPY);
  
  ileri(80);
  vTaskDelay(1500 / portTICK_PERIOD_MS); // vTaskDelay işlemciyi kilitlemez!
  
  dur();
  vTaskDelay(200 / portTICK_PERIOD_MS);
  
  servoGit(servo1, 50, 7);
  servoGit(servo1, 10, 7);
  geri(60);
  vTaskDelay(1500 / portTICK_PERIOD_MS);
  


  dur();
  Serial.println("Dans Bitti.");
  danceTrigger = 0; // Bayrağı indir
}

void dance_2() {
  danceTrigger = 0; // Bayrağı indir
}

void dance_3() {
  danceTrigger = 0; // Bayrağı indir
}

void dance_4() {
  danceTrigger = 0; // Bayrağı indir
}

void dance_5() {
  danceTrigger = 0; // Bayrağı indir
}

void dance_6() {
  danceTrigger = 0; // Bayrağı indir
}

void dance_7() {
  danceTrigger = 0; // Bayrağı indir
}

// --- WEBSOCKET HANDLERS ---
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0; 
    JSONVar myObj = JSON.parse((char *)data);
    if (JSON.typeof(myObj) == "undefined") return;

    int arm = atoi((const char *)myObj["arm"]);
    int head = atoi((const char *)myObj["head"]);
    int speed = atoi((const char *)myObj["speed"]);
    String mood = (const char *)myObj["mood"];
    String direction = (const char *)myObj["direction"];

    // Servo ve Motor anlık tepki vermelidir, direkt burada çağırabiliriz
    servo1.write(arm);
    servo2.write(map(head, 0, 60, 60, 0)); 

    if (direction == "FORWARD") ileri(speed);
    else if (direction == "BACKWARD") geri(speed);
    else if (direction == "RIGHT") sag(speed);
    else if (direction == "LEFT") sol(speed);
    else if (direction == "STOP") dur();

    // Mood Kontrolü
    if (mood == "RANDOM") randORdefault = true;
    else {
      randORdefault = false;
      if (mood == "HAPPY") { eyes.setMood(HAPPY); eyes.anim_laugh(); myDFPlayer.play(1); }
      else if (mood == "ANGRY") { eyes.setMood(ANGRY); myDFPlayer.play(2); }
      else if (mood == "TIRED") { eyes.setMood(TIRED); myDFPlayer.play(3); }
      else if (mood == "CONFUSED") { eyes.setMood(DEFAULT); eyes.anim_confused(); myDFPlayer.play(4); }
      else if (mood == "DEFAULT") eyes.setMood(DEFAULT);
      else if (mood == "DANCE_1") danceTrigger = 1; // Dansı Task_Mantik'a devret
      else if (mood == "DANCE_2") danceTrigger = 2; // Dansı Task_Mantik'a devret
      else if (mood == "DANCE_3") danceTrigger = 3; // Dansı Task_Mantik'a devret
      else if (mood == "DANCE_4") danceTrigger = 4; // Dansı Task_Mantik'a devret
      else if (mood == "DANCE_5") danceTrigger = 5; // Dansı Task_Mantik'a devret
      else if (mood == "DANCE_6") danceTrigger = 6; // Dansı Task_Mantik'a devret
      else if (mood == "DANCE_7") danceTrigger = 7; // Dansı Task_Mantik'a devret
    }
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if(type == WS_EVT_DATA) handleWebSocketMessage(arg, data, len);
}

// =========================================================================
// GÖREV 1: GÖRSEL (Task_Gozler) - Yüksek Öncelik, Core 1
// =========================================================================
void Task_Gozler(void * parameter) {
  // Gözlerin akıcı olması için çok hızlı bir döngü
  for(;;) {
    eyes.update(); 
    // Watchdog'u beslemek için çok kısa bekleme (1ms yeterli)
    vTaskDelay(1 / portTICK_PERIOD_MS); 
  }
}

// =========================================================================
// GÖREV 2: AĞ İŞLEMLERİ (Task_Network) - Core 0 (WiFi Stack ile aynı yer)
// =========================================================================
void Task_Network(void * parameter) {
  for(;;) {
    ws.cleanupClients();
    ArduinoOTA.handle();
    // Ağ işlemleri için 50-100ms arası kontrol yeterlidir
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// =========================================================================
// GÖREV 3: MANTIK & SENSÖR (Task_Mantik) - Core 1
// =========================================================================
void Task_Mantik(void * parameter) {
  for(;;) {
    unsigned long currentMillis = millis();

    // 1. Dokunmatik Kontrolü
    if (digitalRead(THC_PIN)) {
      if(!touchTrigger) { // Debounce mantığı (sürekli tetiklenmesin)
        Serial.println("Touch Triggered!");
        eyes.setMood(HAPPY);
        eyes.anim_laugh();
        touchTrigger = true;
      }
    } else {
      touchTrigger = false;
    }

    // 2. Dans İsteği Var mı?
    switch (danceTrigger)
    {
    case 1: dance_1(); break;
    case 2: dance_2(); break;
    case 3: dance_3(); break;
    case 4: dance_4(); break;
    case 5: dance_5(); break;
    case 6: dance_6(); break;
    case 7: dance_7(); break;
      
    
    default:
      break;
    }
    

    // 3. Random Mood Mantığı
    if (randORdefault && (currentMillis - lastMillis >= interval)) {
      lastMillis = currentMillis;
      if (isMoodActive) {
        eyes.setMood(DEFAULT);
        isMoodActive = false;
      } else {
        int randomMood = random(0, 4);
        switch (randomMood) {
          case 0: eyes.setMood(TIRED); myDFPlayer.play(3); break;
          case 1: eyes.setMood(ANGRY); myDFPlayer.play(2); break;
          case 2: eyes.setMood(HAPPY); eyes.anim_laugh(); myDFPlayer.play(1); break;
          case 3: eyes.anim_confused(); myDFPlayer.play(4); break;
        }
        isMoodActive = true;
      }
      interval = random(4000, 20000);
    }

    // Mantık döngüsü çok hızlı dönmek zorunda değil
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// =========================================================================
// SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);

  // Pin Ayarları
  pinMode(IN4, OUTPUT); pinMode(IN3, OUTPUT);
  pinMode(IN2, OUTPUT); pinMode(IN1, OUTPUT);
  pinMode(THC_PIN, INPUT);

  // DFPlayer Init
  Serial2.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  if (myDFPlayer.begin(Serial2)) {
    myDFPlayer.volume(20);
  } else {
    Serial.println("DFPlayer Error");
  }

  // Servo Init
  servo1.attach(servoPin1, 500, 2400);
  servo2.attach(servoPin2, 500, 2400);

  // --- EKRAN INIT ---
  display.begin(i2c_Address, true);
  
  // RoboEyes başlatılıyor ama henüz loop'a girmediğimiz için kontrol bizde
  eyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
  eyes.setAutoblinker(ON, 3, 2);
  eyes.setIdleMode(ON, 2, 2);
  eyes.setCuriosity(ON);

  // --- WIFI BAĞLANTISI VE EKRAN MESAJI ---
  
  // 1. Ekrana bağlanıyor yazısı gönder
  ekranaYaz("WiFi Agina", "Baglaniyor...");

  if(!SPIFFS.begin(true)) Serial.println("SPIFFS Error");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  // Bağlanana kadar bekle
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); 
    Serial.print(".");
  }
  Serial.println("");
  Serial.println(WiFi.localIP());

  // 2. Bağlandı mesajı ve IP Adresi
  ekranaYaz("BAGLANDI!", WiFi.localIP().toString());
  
  // Kullanıcının IP'yi okuması için 3 saniye bekle
  delay(3000); 

  // --- ARTIK NORMAL ROBOT MODUNA GEÇEBİLİRİZ ---

  // WebSocket & Server
  ws.onEvent(onEvent);
  server.addHandler(&ws);
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  server.begin();

  // OTA
  ArduinoOTA.begin();

  // --- FREE RTOS GÖREVLERİNİ OLUŞTURMA ---
  // Göz görevi başladığı anda ekran kontrolü RoboEyes kütüphanesine geçer
  
  // 1. Gözler (En yüksek öncelik)
  xTaskCreatePinnedToCore(Task_Gozler, "TaskGozler", 10000, NULL, 2, NULL, 1); 

  // 2. Network (Core 0)
  xTaskCreatePinnedToCore(Task_Network, "TaskNetwork", 10000, NULL, 1, NULL, 0);

  // 3. Mantık (Dans ve Sensörler)
  xTaskCreatePinnedToCore(Task_Mantik, "TaskMantik", 10000, NULL, 1, NULL, 1);

  myDFPlayer.play(5); 
}

void loop() {
  // Loop artık boş! Her şey Task'larda dönüyor.
  // İstersen burayı da vTaskDelete(NULL); ile silebilirsin ama boş kalması zararsızdır.
  vTaskDelay(1000 / portTICK_PERIOD_MS); // Boş döngüyü yavaşlat
} 