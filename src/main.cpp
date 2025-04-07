// Import required libraries
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include <Arduino_JSON.h>
#include <ESP32Servo.h> 
//display
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "DFRobotDFPlayerMini.h"

//              SOL MOT      SAĞ MOT5
//           
//            IN1    IN2    IN3    IN4
// ielri       0     PWM     0     PWM
// geri        1    rPWM     1    rPWM
// sağ         0     PWM     1    rPWM
// sol         1    rPWM     0     PWM

unsigned long touchMillis = 0; // Zamanlayıcı için değişken
unsigned long lastMillis = 0;
int long interval = 3000;
bool isMoodActive = false;
bool randORdefault = false;
bool touchActive = false;

#define IN1 32
#define IN2 33
#define IN3 25
#define IN4 26

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
// Declaration for an SSD1306 display connected to I2C (SDA, SCL pins)
#define i2c_Address 0x3c
#define OLED_RESET -1
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#include <FluxGarage_RoboEyes.h>

roboEyes RoboEyes; // create RoboEyes instance

#define THC_PIN 13

// Replace with your network credentials
const char *ssid = "DUNYA2A";
const char *password = "10203040";

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);
// Create a WebSocket object
AsyncWebSocket ws("/ws");

// Servo motor nesnesi
Servo servo1, servo2;

const int servoPin1 = 27;
const int servoPin2 = 14;

// Initialize SPIFFS
void initSPIFFS()
{
  if (!SPIFFS.begin(true))
  {
    Serial.println("An error has occurred while mounting SPIFFS");
  }
  Serial.println("SPIFFS mounted successfully");
}

// Initialize WiFi
void initWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print('.');
    delay(1000);
  }
  Serial.println(WiFi.localIP());
}


// Yön tayini

void ileri(int pwm) {
  digitalWrite(IN1, LOW);  // Sol motor ileri
  digitalWrite(IN3, LOW);  // Sağ motor ileri
  analogWrite(IN2, pwm);   // Sol motor PWM
  analogWrite(IN4, pwm);   // Sağ motor PWM
}

void geri(int pwm) {
  digitalWrite(IN1, HIGH);      // Sol motor geri
  digitalWrite(IN3, HIGH);      // Sağ motor geri
  analogWrite(IN2, 254 - pwm);  // Sol motor PWM
  analogWrite(IN4, 254 - pwm);  // Sağ motor PWM
}

void sag(int pwm) {
  digitalWrite(IN1, LOW);       // Sol motor ileri
  digitalWrite(IN3, HIGH);      // Sağ motor geri
  analogWrite(IN2, pwm);        // Sol motor PWM
  analogWrite(IN4, 254 - pwm);  // Sağ motor PWM
}

void sol(int pwm) {
  digitalWrite(IN1, HIGH);      // Sol motor geri
  digitalWrite(IN3, LOW);       // Sağ motor ileri
  analogWrite(IN2, 254 - pwm);  // Sol motor PWM
  analogWrite(IN4, pwm);        // Sağ motor PWM
}

void dur() {
  digitalWrite(IN1, LOW);  // Sol motor ileri
  digitalWrite(IN3, LOW);  // Sağ motor ileri
  analogWrite(IN2, 0);     // Sol motor PWM
  analogWrite(IN4, 0);     // Sağ motor PWM
}


void handleWebSocketMessage(void *arg, uint8_t *data, size_t len)
{
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
  {
    data[len] = 0; // Null-terminate string

    // Serial.printf("Raw Data: %s\n", (char *)data);

    JSONVar myObj = JSON.parse((char *)data);

    // Verileri işleme
    int arm = atoi((const char *)myObj["arm"]);
    int head = atoi((const char *)myObj["head"]);
    int speed = atoi((const char *)myObj["speed"]);
    String mood = (const char *)myObj["mood"];
    String direction = (const char *)myObj["direction"];

    Serial.printf("Arm: %d, Head: %d, Speed: %d, Mood: %s\n",
                  arm, head, speed, mood.c_str());


    // Servo kontrolü ******************

    servo1.write(50 - arm);
    servo2.write(head);

    // Hareket kontrolü ****************

    if (direction == "FORWARD")
    {
      ileri(speed);
    }
    else if (direction == "BACKWARD")
    {
      geri(speed);
    }
    else if (direction == "RIGHT")
    {
      sag(speed);
    }
    else if (direction == "LEFT")
    {
      sol(speed);
    }
    else if (direction == "STOP")
    {
      dur();
    }

    // Mood kontrolü *******************

    if (mood == "RANDOM")
    {
      randORdefault = true;
    }
    else
    {
      randORdefault = false;

      if (mood == "HAPPY")
      {
        RoboEyes.setMood(HAPPY);
      }
      else if (mood == "ANGRY")
      {
        RoboEyes.setMood(ANGRY);
      }
      else if (mood == "TIRED")
      {
        RoboEyes.setMood(TIRED);
      }
      else if (mood == "CONFUSED")
      {
        RoboEyes.setMood(DEFAULT);
        RoboEyes.anim_confused();
      }
      else if (mood == "DEFAULT")
      {
        RoboEyes.setMood(DEFAULT);
      }
    }
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
    Serial.printf("WebSocketttt client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
    break;
  case WS_EVT_DISCONNECT:
    Serial.printf("WebSocket client #%u disconnected\n", client->id());
    break;
  case WS_EVT_DATA:
    handleWebSocketMessage(arg, data, len);
    break;
  case WS_EVT_PONG:
  case WS_EVT_ERROR:
    break;
  }
}

void initWebSocket()
{
  ws.onEvent(onEvent);
  server.addHandler(&ws);
}

void setup()
{
  pinMode(IN4, OUTPUT);  // IN4
  pinMode(IN3, OUTPUT);  // IN3
  pinMode(IN2, OUTPUT);  // IN2
  pinMode(IN1, OUTPUT);  // IN1

  pinMode(THC_PIN, INPUT);
  display.begin(i2c_Address, true);
  RoboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
  RoboEyes.setAutoblinker(ON, 3, 2);
  RoboEyes.setIdleMode(ON, 2, 2);
  RoboEyes.setCuriosity(ON);
  RoboEyes.anim_confused();

  Serial.begin(115200);

  // Servo motoru başlatıyoruz
  servo1.attach(servoPin1, 500, 2400);
  servo2.attach(servoPin2, 500, 2400);

  initSPIFFS();
  initWiFi();
  initWebSocket();

  // Web sunucusunu ayarla
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/index.html", "text/html"); });
  server.serveStatic("/", SPIFFS, "/");
  // Başlat
  server.begin();
}

void rand_mood_control()
{
  unsigned long currentMillis = millis();

  if (currentMillis - lastMillis >= interval)
  {
    lastMillis = currentMillis;

    if (isMoodActive)
    {
      // Reset mood
      RoboEyes.setMood(DEFAULT); // Return to default expression
      isMoodActive = false;
    }
    else
    {
      // Randomly select a mood
      int randomMood = random(0, 4); // 0, 1, 2, 3
      switch (randomMood)
      {
      case 0:
        RoboEyes.setMood(TIRED);
        break;
      case 1:
        RoboEyes.setMood(ANGRY);
        break;
      case 2:
        RoboEyes.setMood(HAPPY);
        RoboEyes.anim_laugh();
        break;
      case 3:
        RoboEyes.anim_confused();
        break;
      }
      isMoodActive = true;
    }

    interval = random(2000, 4000); // Random interval between 2 to 4 seconds
  }
}
void touch()
{
  if (digitalRead(THC_PIN) == HIGH)
  {
    RoboEyes.setMood(HAPPY);
    RoboEyes.anim_laugh();
    touchActive = true;
    touchMillis = millis(); // Zamanlayıcıyı başla
  }
}

void checkTouchTimeout()
{
  if (touchActive && (millis() - touchMillis >= 2000)) // 2 saniye sonra
  {
    RoboEyes.setMood(DEFAULT); // Varsayılan moda geç
    touchActive = false;
  }
}

void mood()
{
  if (randORdefault == true)
  {
    rand_mood_control();
  }
  touch();
  checkTouchTimeout(); // Zamanlayıcıyı kontrol et
}
void loop()
{
  ws.cleanupClients();
  RoboEyes.update(); // update eyes drawings
  mood();
}

