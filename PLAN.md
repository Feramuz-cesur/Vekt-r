# Vector V2 — Yazılım İnceleme & Geliştirme Planı

> Tarih: 2026-04-14  
> Hazırlayan: Claude Code (Sonnet 4.6)  
> Kapsam: Mevcut durumun analizi, tespit edilen boşluklar, AI robot kontrolü fizibilite araştırması ve geliştirme yol haritası

---

## 1. Mevcut Mimari — Genel Bakış

```
[ Tarayıcı / Telefon ]
        │ ws://ESP32_IP/ws (doğrudan)
        ▼
[ ESP32 (main.cpp) ]  ←→  Motorlar, Servolar, OLED, LED, DFPlayer
        │
        │  (şu an YOK — olması gereken bağlantı)
        ▼
[ PC — server.js (Node.js) ]
   ├─ port 3000: Electron Dashboard (UI)
   ├─ port 8080: ESP32 WebSocket (wssEsp32)
   └─ wss → Gemini Multimodal Live API
```

### Üç Bileşen

| Bileşen | Dosya | Görev |
|---------|-------|-------|
| ESP32 Firmware | `src/main.cpp` | Robot donanımını yönetir; WebSocket üzerinden JSON komutları alır |
| PC Sunucusu | `ai_assistant/server.js` | Gemini Live API köprüsü; ESP32'yi AI'ya bağlar |
| Kontrol Arayüzü | `data/index.html` | Kullanıcının robotu manuel veya AI üzerinden sürdüğü web arayüzü |

---

## 2. Mevcut Durumun Derinlemesine Analizi

### 2.1 main.cpp — ESP32 Firmware

**Ne yapıyor:**
- `AsyncWebSocket` ile `/ws` endpoint'inde JSON komutları dinliyor (port 80)
- `handleWebSocketMessage()` şu JSON anahtarlarını tanıyor:
  - `arm`, `head` → Servo kontrolü
  - `direction` + `speed` → Motor kontrolü (FORWARD / BACKWARD / LEFT / RIGHT / STOP)
  - `volume` → DFPlayer ses seviyesi
  - `mood` → Göz animasyonu ve ses (HAPPY, ANGRY, TIRED, CONFUSED, DEFAULT, RANDOM, DANCE_1..7)
  - `lightMode` + `color` → NeoPixel LED (OFF, STATIC, RAINBOW, PULSE, POLICE)
  - `command: "showClock"` → OLED'de saat gösterimi
- 3 FreeRTOS görevi: `Task_Gozler` (çekirdek 1), `Task_Network` (çekirdek 0), `Task_Mantik` (çekirdek 1)
- Her 5 saniyede pil yüzdesini tüm WebSocket istemcilerine yayınlıyor

**Eksik olan (tespit edilen boşluklar):**
- `modeCommand: "AI"` ve `serverIp` alanlarını **tanımıyor** → `index.html`'in AI butonuna bastığında ESP32'ye gönderdiği komut işlenmiyor
- `server_command` tipli mesajları **tanımıyor** → server.js'in gönderdiği `{ type: "server_command", modeCommand: "AI" }` kaybolup gidiyor
- Mikrofon desteği **yok** → server.js binary PCM ses bekliyor ama ESP32'de ses yakalama donanımı/kodu yok
- `mic_mute` ve `audio_out_format` komutlarını **tanımıyor**

---

### 2.2 server.js — PC Sunucusu

**Ne yapıyor:**
- **Durum makinesi:** `isSystemReady` → `isAIRunning` geçişleri
- **ESP32 Köprüsü:** port 8080'de ESP32 bağlantısı bekliyor; binary PCM ses alıp Gemini'ye, Gemini'nin sesini ESP32'ye gönderiyor
- **Gemini Live API:** `wss://generativelanguage.googleapis.com/...` üzerinden gerçek zamanlı çift yönlü ses iletişimi
- **VAD (Voice Activity Detection):** Yazılımsal gürültü eşikleme, `activityStart`/`activityEnd` gönderimi
- **Ses işleme:** WAV→PCM dönüşüm, L16 endian düzeltme, 4096 byte'lık chunk pacing, ses seviyesi amplifikasyonu
- **Dashboard:** Electron üzerinden port 3000'de çalışan yönetim arayüzü; API anahtarı, sistem prompt, ses, model, transkript

**Tespit edilen boşluklar:**
- **Robot fiziksel kontrolü yok:** Gemini'nin sesli yanıtından çıkarılacak robot komutları için sinyaller/tool call mekanizması **mevcut değil**
- `attachStandbyMessageListener()` fonksiyonu ESP32'den `START_AI` / `STOP_AI` bekliyor ama ESP32 bu mesajları hiç göndermiyor
- AI konuşurken robotun fiziksel olarak tepki vermesi (örn. `HAPPY` mood'u, LED yanması) **uygulanmamış**

---

### 2.3 data/index.html — Kontrol Arayüzü

**Ne yapıyor:**
- ESP32'nin kendi web sunucusundan yükleniyor (`ws://ESP32_IP/ws` ile bağlanıyor)
- 3 mod: **MANUEL** (joystick+sliderlar), **OTO** (kayıt ve oynatma), **YAPAY ZEKA** (AI overlay)
- **Kayıt/Oynatma Sistemi:** Kullanıcı hareketleri `timeOffset` damgasıyla kaydediyor, setTimeout ile senkronize geri oynatıyor
- Pil yüzdesini gerçek zamanlı gösteriyor
- AI moduna girildiğinde `{ modeCommand: "AI", serverIp: "..." }` gönderiyor

**Tespit edilen boşluklar:**
- AI modunda ESP32'ye gönderilen `serverIp` işlenmiyor (firmware boşluğu)
- AI'nın robota verdiği komutlar için herhangi bir görsel geri bildirim yok (AI ne yapıyor? ne söylüyor? görünmüyor)
- `cfgAiServerIp` input'u var ama UI'da nerede olduğu belirsiz (muhtemelen gizli bir ayar bölümünde)

---

## 3. "AI Robotumu Kontrol Edebilir mi?" Sorusunun Cevabı

**Kısa cevap: Evet, ama iki farklı düzeyde mümkün.**

### Seviye 1 — AI Sesle Konuşur, Siz Kontrol Edersiniz (MEVCUT — %80 hazır)

Bu senaryoda AI sadece konuşur; fiziksel kontrol kullanıcıda kalır.

```
Kullanıcı Konuşur → ESP32 Mikrofon → server.js → Gemini Live API
                                                        ↓
ESP32 Hoparlör ← server.js ← Gemini Sesli Yanıt
```

**Eksik tek şey:** ESP32 tarafında mikrofon ve ses çıkışı (I2S veya DFPlayer). server.js bu akışı tamamen destekliyor. ESP32 firmware'e:
1. `modeCommand: "AI"` + `serverIp` alındığında `ws://serverIp:8080`'e bağlanma
2. Mikrofon sesini binary PCM olarak server.js'e streaming
3. server.js'ten gelen binary PCM'i hoparlörden çalma kodu eklenmesi gerekiyor.

---

### Seviye 2 — AI Robotu Fiziksel Olarak Kontrol Eder (HENÜZ UYGULANMAMIŞ)

Bu senaryoda AI "konuşurken" aynı zamanda robotun motorlarını, LED'lerini, servisini yönetir.

**Bu nasıl çalışır?**

server.js'te `currentEsp32Ws` zaten mevcut ve şu anda herhangi bir JSON komutunu ESP32'ye gönderebilir:

```javascript
// server.js içinde bu fonksiyon ZATEN var (eksik değil)
function sendRobotCommand(cmd) {
    if (currentEsp32Ws && currentEsp32Ws.readyState === WebSocket.OPEN) {
        currentEsp32Ws.send(JSON.stringify(cmd));
    }
}

// Örnek: AI "ileri git" dediğinde
sendRobotCommand({ direction: "FORWARD", speed: 150 });

// Örnek: AI "mutlu ol" dediğinde
sendRobotCommand({ mood: "HAPPY" });

// Örnek: AI "kırmızı yak" dediğinde
sendRobotCommand({ lightMode: "STATIC", color: "#ff0000" });
```

**Sorun:** Gemini'ye "hangi robotu ne zaman kontrol et?" kararını nasıl verdirirsiniz?

İki yaklaşım:

#### Yaklaşım A — Gemini Function Calling (Tool Use)

Gemini Live API, `tools` (işlev tanımları) destekliyor. Session setup'ta şöyle tanımlarsınız:

```json
{
  "setup": {
    "tools": [{
      "functionDeclarations": [
        {
          "name": "move_robot",
          "description": "Robotu hareket ettir",
          "parameters": {
            "type": "object",
            "properties": {
              "direction": { "type": "string", "enum": ["FORWARD","BACKWARD","LEFT","RIGHT","STOP"] },
              "speed": { "type": "integer", "minimum": 0, "maximum": 255 }
            }
          }
        },
        {
          "name": "set_mood",
          "description": "Robotun duygusal ifadesini ayarla",
          "parameters": {
            "type": "object",
            "properties": {
              "mood": { "type": "string", "enum": ["HAPPY","ANGRY","TIRED","CONFUSED","DEFAULT"] }
            }
          }
        },
        {
          "name": "set_lights",
          "description": "LED ışıklarını kontrol et",
          "parameters": {
            "type": "object",
            "properties": {
              "mode": { "type": "string", "enum": ["OFF","STATIC","RAINBOW","PULSE","POLICE"] },
              "color": { "type": "string", "description": "#RRGGBB formatında renk" }
            }
          }
        }
      ]
    }]
  }
}
```

Gemini bu araçları kullandığında server.js `toolCall` eventini yakalar ve karşılık gelen komutu ESP32'ye gönderir.

#### Yaklaşım B — Metin Çıktısını Parse Et

Gemini'nin yanıtına `[CMD:direction=FORWARD,speed=150]` gibi özel etiketler eklemesi için system prompt'a talimat yazarsınız. server.js bu etiketi regex ile yakalar ve robota gönderir. Daha kırılgan ama daha basit.

**Tavsiye: Yaklaşım A** — Gemini Live API `toolCall`/`toolResponse` protokolünü destekliyor ve çok daha güvenilir.

---

## 4. "Manuel Arayüzden Gönderilen Verileri PC Serverden Gönderebilir miyiz?" Sorusunun Cevabı

**Cevap: Evet, teknik olarak tamamen mümkün ve altyapı zaten hazır.**

`index.html`'in ESP32'ye gönderdiği her JSON komutu:

```json
{ "direction": "FORWARD", "speed": 150 }
{ "arm": 90, "head": 30 }
{ "mood": "HAPPY" }
{ "lightMode": "RAINBOW" }
{ "command": "showClock" }
```

server.js içindeki `currentEsp32Ws.send(JSON.stringify(cmd))` ile birebir gönderilebilir. `main.cpp`'nin `handleWebSocketMessage()` fonksiyonu gönderenin kim olduğunu ayırt etmiyor — JSON içeriğine bakıyor.

**Pratik kullanım senaryoları:**

| Senaryo | Nasıl Uygulanır |
|---------|-----------------|
| AI konuşurken happy diyince HAPPY mood'u | Gemini tool call → `sendRobotCommand({ mood: "HAPPY" })` |
| "ileri git" sesli komutu | Gemini tool call → `sendRobotCommand({ direction: "FORWARD", speed: 150 })` |
| Zamanlayıcılı otomatik hareketler | server.js'te `setInterval` ile periyodik komutlar |
| Web API üzerinden uzaktan kontrol | server.js'e yeni `POST /api/robot-command` endpoint'i ekle |
| Senaryo dosyasından oynatma | JSON dosyasını okuyup timeOffset'e göre setTimeout ile replay |

---

## 5. Tespit Edilen Teknik Sorunlar

| # | Bileşen | Sorun | Önem |
|---|---------|-------|------|
| 1 | `main.cpp` | WiFi şifresi kodda açık yazılmış (`ssid`/`password` hardcode) | Orta |
| 2 | `main.cpp` | `modeCommand`/`serverIp` komutlarını tanımıyor | Yüksek |
| 3 | `main.cpp` | `server_command` tipli mesajları tanımıyor | Yüksek |
| 4 | `main.cpp` | dance_5, dance_6, dance_7 boş fonksiyon | Düşük |
| 5 | `main.cpp` | `Task_Mantik` içinde `vTaskDelay` ile dans→`displayLocked` aynı görevde (kilitlenme riski yok ama tasarım kötü) | Düşük |
| 6 | `server.js` | Gemini'nin robot komutlarını tetiklemesi için tool calling yok | Yüksek |
| 7 | `server.js` | ESP32 ses çıkışı binary PCM bekliyor ama format/bant genişliği yönetimi kırılgan | Orta |
| 8 | `index.html` | AI modunda server IP girilmeden geçiş engellenmiş ama kullanıcıya nereye yazacağı belirsiz | Düşük |
| 9 | `ai_config.json` | `model` değeri `gemini-3.1-flash-live-preview` yazıyor (server.js varsayılanı `gemini-2.5-flash-native-audio-latest`) | Orta |

---

## 6. Geliştirme Yol Haritası (Öncelik Sırasına Göre)

### Faz 1 — Mevcut Bağlantıyı Tamamla (Öncelikli)

**Hedef:** AI konuşma sistemini gerçekten çalıştırmak.

1. **`main.cpp`'ye `modeCommand` işlemcisi ekle:**
   - `modeCommand: "AI"` geldiğinde `serverIp`'yi al ve `ws://serverIp:8080`'e WebSocket bağlantısı kur
   - `modeCommand: "NORMAL"` geldiğinde bu bağlantıyı kapat
   - Bağlantı kurulunca `START_AI` gönder

2. **Ses donanımı kararı:**
   - Mevcut I2S destekli bir mikrofon modülü (INMP441, MAX4466) ESP32'ye eklenebilir
   - Alternatif: DFPlayer'dan ses çıkışı (sadece tek yön — AI konuşur ama dinleyemez)
   - I2S mikrofon seçilirse: `main.cpp`'ye PCM streaming kodu eklenmeli

3. **`server.js`'te Gemini tool calling desteği ekle:**
   - `sessionSetup.setup.tools` array'ine `move_robot`, `set_mood`, `set_lights`, `play_dance` tanımları ekle
   - `geminiWs.on('message')` içinde `toolCall` event'ini yakaladığında `currentEsp32Ws`'a karşılık gelen komutu gönder

---

### Faz 2 — AI Robot Kontrolü

**Hedef:** AI sesinizden robotu fiziksel olarak yönetsin.

1. **System prompt'u güçlendir:** "Kullanıcı 'ileri git' dediğinde `move_robot` aracını çağır" gibi talimatlar ekle
2. **Mood senkronizasyonu:** AI "Çok mutluyum!" gibi bir şey söylediğinde otomatik `HAPPY` mood tetikle
3. **LED durumları:** AI aktifken RAINBOW, konuşurken PULSE, dinlerken sabit renk

---

### Faz 3 — Arayüz ve Kalite İyileştirmeleri

1. **`index.html` — AI modu geri bildirimi:**
   - AI overlay'e transkript/ses dalga animasyonu ekle
   - AI'nın verdiği robot komutlarını overlay'de göster

2. **Kayıt/Oynatma sistemi:**
   - Kaydedilen hareketleri JSON olarak dışa aktar/içe aktar
   - Birden fazla kayıt senaryosu saklama

3. **`main.cpp` — Dance tamamlama:**
   - `dance_5`, `dance_6`, `dance_7` fonksiyonlarını gerçek hareket dizileriyle doldur

4. **WiFi güvenliği:**
   - WiFi bilgilerini `src/credentials.h` veya PlatformIO `build_flags` içine taşı

---

### Faz 4 — Gelişmiş AI Özellikleri

1. **Senaryo sistemi:** AI'ya JSON formatında "sahne scriptleri" besle; robot belirli konuşmalarda koreografik hareketler yapsın
2. **Bağlamsal hafıza:** Konuşma geçmişini ESP32 üzerinden tetiklenen olaylarla zenginleştir (dokunmatik sensör, pil durumu)
3. **Çoklu dil desteği:** Gemini Live API çok dilli; system prompt'ta dil kısıtlaması kaldırılabilir

---

## 7. Özet Tablo

| Soru | Cevap |
|------|-------|
| AI robotla konuşabilir mi? | Evet, ama ESP32'de ses donanımı gerekiyor |
| AI robotu fiziksel olarak kontrol edebilir mi? | Evet — server.js'e Gemini tool calling eklenmesi yeterli |
| Manuel UI'dan gönderilen komutlar server'dan gönderilebilir mi? | Evet — altyapı hazır, sadece server.js'te fonksiyon çağrısı yeterli |
| Şu an çalışıyor mu? | Hayır — ESP32↔server.js bağlantı katmanı firmware'de eksik |
| Ne kadar çalışma gerekiyor? | Faz 1 için: ~2-3 gün (tool calling + modeCommand) |

---

*Bu döküman sadece inceleme ve planlama amaçlıdır. Kod değişikliği içermez.*
