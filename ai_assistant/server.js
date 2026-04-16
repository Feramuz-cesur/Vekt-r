import fs from 'fs';
import path from 'path';
import http from 'http';
import { fileURLToPath } from 'url';
import express from 'express';
import { WebSocketServer, WebSocket } from 'ws';
import 'dotenv/config';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);


let GEMINI_API_KEY = process.env.GEMINI_API_KEY || '';

// ======================== KİŞİSELLEŞTİRME AYARLARI ========================
// Varsayılan ayarlar (dashboard üzerinden değiştirilebilir)
let aiConfig = {
    systemPrompt: "Sen Wector adında yardımsever, cana yakın ve fütüristik bir robotsun. Feramuz isimli yapımcın tarafından yaratıldın. İnsanlarla doğal ve esprili bir dille samimi şekilde konuşursun. Cevapların her zaman çok kısa olmalı, gereksiz cümle uzatma.",
    voiceName: "Aoede",
    model: "models/gemini-2.5-flash-native-audio-latest",
    temperature: 1.0,
    speakerName: "Kullanıcı",
    proactiveAudio: false,
    volume: 1.0,
    transcriptionEnabled: true
};

// Ayarları diske kaydet/yükle
const CONFIG_PATH = path.join(__dirname, 'ai_config.json');

function loadConfig() {
    try {
        if (fs.existsSync(CONFIG_PATH)) {
            const saved = JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8'));
            aiConfig = { ...aiConfig, ...saved };
            console.log('[CONFIG] Kaydedilmiş ayarlar yüklendi.');
        }
    } catch (e) {
        console.log('[CONFIG] Ayar dosyası okunamadı, varsayılanlar kullanılıyor.');
    }
}

function saveConfig() {
    try {
        fs.writeFileSync(CONFIG_PATH, JSON.stringify(aiConfig, null, 2));
    } catch (e) {
        console.error('[CONFIG] Ayarlar kaydedilemedi:', e.message);
    }
}

loadConfig();

process.on('SIGINT', () => {
    console.log("\n[SİSTEM] Node.js kapatılıyor...");
    process.exit();
});

// ======================== EXPRESS & WEBSOCKET SERVERS ========================
const app = express();
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// --- API: Durumu sorgula ---
app.get('/api/status', (req, res) => {
    res.json({
        hasApiKey: !!GEMINI_API_KEY && GEMINI_API_KEY !== 'GEMINI_API_KEY_BURAYA'
    });
});

// --- API: Anahtar kaydet ---
app.post('/api/save-key', (req, res) => {
    const { apiKey } = req.body;
    if (apiKey) {
        GEMINI_API_KEY = apiKey;
        const envPath = path.join(__dirname, '.env');
        fs.writeFileSync(envPath, `GEMINI_API_KEY=${apiKey}\n`);
        uiLog(`✅ Gemini API Anahtarı başarıyla güncellendi.`);
        res.json({ success: true });
    } else {
        res.json({ success: false });
    }
});

// --- API: Kişiselleştirme ayarlarını getir ---
app.get('/api/config', (req, res) => {
    res.json(aiConfig);
});

// --- API: Kişiselleştirme ayarlarını kaydet ---
app.post('/api/config', (req, res) => {
    const { systemPrompt, voiceName, model, temperature, speakerName, proactiveAudio, volume, transcriptionEnabled } = req.body;
    if (systemPrompt !== undefined) aiConfig.systemPrompt = systemPrompt;
    if (voiceName !== undefined) aiConfig.voiceName = voiceName;
    if (model !== undefined) aiConfig.model = model;
    if (temperature !== undefined) aiConfig.temperature = parseFloat(temperature);
    if (speakerName !== undefined) aiConfig.speakerName = speakerName;
    if (proactiveAudio !== undefined) aiConfig.proactiveAudio = !!proactiveAudio;
    if (volume !== undefined) aiConfig.volume = Math.max(0.1, Math.min(3.0, parseFloat(volume)));
    if (transcriptionEnabled !== undefined) aiConfig.transcriptionEnabled = !!transcriptionEnabled;
    saveConfig();
    uiLog(`⚙️ AI ayarları güncellendi ve kaydedildi.`);
    res.json({ success: true, config: aiConfig });
});

// --- API: AI'ı manuel başlat ---
app.post('/api/start-ai', (req, res) => {
    // Önce ayarları güncelle (gövdede varsa)
    const { systemPrompt, voiceName, model, temperature, speakerName, proactiveAudio } = req.body;
    if (systemPrompt !== undefined) aiConfig.systemPrompt = systemPrompt;
    if (voiceName !== undefined) aiConfig.voiceName = voiceName;
    if (model !== undefined) aiConfig.model = model;
    if (temperature !== undefined) aiConfig.temperature = parseFloat(temperature);
    if (speakerName !== undefined) aiConfig.speakerName = speakerName;
    if (proactiveAudio !== undefined) aiConfig.proactiveAudio = !!proactiveAudio;
    saveConfig();

    if (!isEsp32Connected || !currentEsp32Ws) {
        return res.json({ success: false, error: 'ESP32 bağlı değil! Önce robotu bağlayın.' });
    }
    if (!GEMINI_API_KEY || GEMINI_API_KEY === 'GEMINI_API_KEY_BURAYA') {
        return res.json({ success: false, error: 'API Anahtarı girilmemiş!' });
    }
    if (isAIRunning || isSystemReady) {
        return res.json({ success: false, error: 'AI zaten çalışıyor veya hazır durumda!' });
    }

    isSystemReady = true;
    uiLog("🟢 Sistem Hazır. Wector'dan (ESP32) tetikleme mesajı bekleniyor...");
    broadcastStatus();
    res.json({ success: true });
});

// --- API: AI'ı durdur ---
app.post('/api/stop-ai', (req, res) => {
    isSystemReady = false;
    stopGeminiSession('Dashboard üzerinden durduruldu');
    uiLog("🛑 Sistem Standby modundan çıkarıldı.");
    res.json({ success: true });
});

const UI_PORT = 3000;
const uiServer = app.listen(UI_PORT, () => {
    console.log(`💻 AI Dashboard: http://localhost:${UI_PORT}`);
});

// ======================== PORT 8080: ESP32 + TARAYICI MİKROFONU ========================
// Aynı port, farklı path:
//   ws://[ip]:8080/      → ESP32 robot bağlantısı
//   ws://[ip]:8080/mic   → Tarayıcı (telefon) mikrofonu
const AI_PORT = process.env.AI_PORT || 8080;
const server8080 = http.createServer();
const wssEsp32 = new WebSocketServer({ noServer: true });
const wssBrowserMic = new WebSocketServer({ noServer: true });

server8080.on('upgrade', (request, socket, head) => {
    const pathname = new URL(request.url, `http://localhost`).pathname;
    if (pathname === '/mic') {
        wssBrowserMic.handleUpgrade(request, socket, head, (ws) => {
            wssBrowserMic.emit('connection', ws, request);
        });
    } else {
        wssEsp32.handleUpgrade(request, socket, head, (ws) => {
            wssEsp32.emit('connection', ws, request);
        });
    }
});

server8080.listen(AI_PORT, () => {
    console.log(`📡 ESP32 Portu:      ws://localhost:${AI_PORT}`);
    console.log(`🎤 Tarayıcı Mikrofon: ws://[pc-ip]:${AI_PORT}/mic`);
});

// Dashboard WebSocket (port 3000/ui)
const wssUI = new WebSocketServer({ server: uiServer, path: '/ui' });

let uiClients = new Set();
wssUI.on('connection', (ws) => {
    uiClients.add(ws);
    broadcastStatus();

    // UI komutlarını dinle
    ws.on('message', (data) => {
        try {
            const msg = JSON.parse(data.toString());
            if (msg.type === 'start-ai') {
                if (!GEMINI_API_KEY) return uiLog('❌ API Anahtarı yok!');
                if (isAIRunning || isSystemReady) return uiLog('⚠️ Sistem zaten hazır veya çalışıyor!');

                // Ayarları güncelle
                if (msg.config) {
                    Object.assign(aiConfig, msg.config);
                    saveConfig();
                }
                isSystemReady = true;
                uiLog("🟢 Sistem Hazır. Wector'dan (ESP32) tetikleme bekleniyor...");
                broadcastStatus();
            } else if (msg.type === 'stop-ai') {
                isSystemReady = false;
                stopGeminiSession('Dashboard üzerinden durduruldu');
                uiLog("🛑 Sistem Standby modundan çıkarıldı.");
            }
        } catch (e) { /* ignore */ }
    });

    ws.on('close', () => uiClients.delete(ws));
});

// ======================== DURUM YÖNETİMİ ========================
let isEsp32Connected = false;
let isGeminiConnected = false;
let isGeminiSetupComplete = false;
let isAIRunning = false;
let isSystemReady = false; // "Başlatıldı" durumu ama Gemini henüz aktif değil
let latestLog = "Sunucu başlatıldı. Robot ile bağlantı bekleniyor...";
let transcriptHistory = []; // Sohbet geçmişi
let lastGeminiAudioMimeType = "";

// Aktif bağlantılar
let currentEsp32Ws = null;
let currentGeminiWs = null;
let currentBrowserMicWs = null;

// VAD (Voice Activity Detection) durumu — modül seviyesinde, session reset'te sıfırlanır
let vadSpeaking = false;
let vadSilenceMs = 0;
let vadNoiseFloor = 200;
let vadLastStateLog = 0;
let vadStreamActive = false;

// Robot durum takibi (göreceli servo hareketi ve hız için)
let currentArmAngle = 0;    // Kol açısı (0-180)
let currentHeadAngle = 30;  // Kafa açısı (0-60, 30 = orta)
let defaultSpeed = 150;     // Varsayılan hareket hızı (0-255)
let moveDurationTimer = null; // Zamanlı hareket timer'ı

function broadcastStatus() {
    const status = {
        esp32: isEsp32Connected,
        gemini: isGeminiConnected,
        aiRunning: isAIRunning,
        systemReady: isSystemReady,
        log: latestLog,
        transcriptHistory: transcriptHistory
    };
    uiClients.forEach(client => {
        if (client.readyState === 1) client.send(JSON.stringify(status));
    });
}

function uiLog(msg) {
    latestLog = msg;
    console.log(msg);
    broadcastStatus();
}

// Transkript biriktirme: Her kelimeyi ayrı göndermek yerine, sıra bitene kadar biriktir
let pendingUserTranscript = '';
let pendingAssistantTranscript = '';

function appendTranscript(role, text) {
    if (!text) return;
    if (role === 'user') {
        pendingUserTranscript += text;
    } else {
        pendingAssistantTranscript += text;
    }
}

function flushTranscript(role) {
    if (role === 'user' && pendingUserTranscript.trim()) {
        transcriptHistory.push({
            role: 'user',
            text: pendingUserTranscript.trim(),
            time: new Date().toLocaleTimeString('tr-TR')
        });
        pendingUserTranscript = '';
    } else if (role === 'assistant' && pendingAssistantTranscript.trim()) {
        transcriptHistory.push({
            role: 'assistant',
            text: pendingAssistantTranscript.trim(),
            time: new Date().toLocaleTimeString('tr-TR')
        });
        pendingAssistantTranscript = '';
    }
    if (transcriptHistory.length > 50) transcriptHistory.shift();
    broadcastStatus();
}

// ======================== YARDIMCI FONKSİYONLAR ========================
function parseRateFromMimeType(mimeType) {
    if (!mimeType) return null;
    const match = /(?:^|;)\s*rate\s*=\s*(\d+)/i.exec(mimeType);
    return match ? Number(match[1]) : null;
}

function parseChannelsFromMimeType(mimeType) {
    if (!mimeType) return null;
    const match = /(?:^|;)\s*channels\s*=\s*(\d+)/i.exec(mimeType);
    return match ? Number(match[1]) : null;
}

function isWavMimeType(mimeType) {
    if (!mimeType) return false;
    const mt = mimeType.toLowerCase();
    return mt.includes('audio/wav') || mt.includes('audio/x-wav') || mt.includes('audio/wave');
}

function isPcmMimeType(mimeType) {
    if (!mimeType) return false;
    const mt = mimeType.toLowerCase();
    return mt.includes('audio/pcm') || mt.includes('audio/l16');
}

function swap16EndianInPlace(buffer) {
    for (let i = 0; i + 1 < buffer.length; i += 2) {
        const a = buffer[i];
        buffer[i] = buffer[i + 1];
        buffer[i + 1] = a;
    }
}

function tryExtractPcmFromWav(wavBuffer) {
    if (wavBuffer.length < 44) return null;
    if (wavBuffer.toString('ascii', 0, 4) !== 'RIFF') return null;
    if (wavBuffer.toString('ascii', 8, 12) !== 'WAVE') return null;

    let offset = 12;
    let fmt = null;
    let dataChunk = null;

    while (offset + 8 <= wavBuffer.length) {
        const id = wavBuffer.toString('ascii', offset, offset + 4);
        const size = wavBuffer.readUInt32LE(offset + 4);
        const chunkStart = offset + 8;
        const chunkEnd = chunkStart + size;
        if (chunkEnd > wavBuffer.length) break;

        if (id === 'fmt ') {
            if (size < 16) return null;
            fmt = {
                audioFormat: wavBuffer.readUInt16LE(chunkStart),
                channels: wavBuffer.readUInt16LE(chunkStart + 2),
                sampleRate: wavBuffer.readUInt32LE(chunkStart + 4),
                bitsPerSample: wavBuffer.readUInt16LE(chunkStart + 14)
            };
        } else if (id === 'data') {
            dataChunk = wavBuffer.subarray(chunkStart, chunkEnd);
        }

        offset = chunkEnd + (size % 2);
        if (fmt && dataChunk) break;
    }

    if (!fmt || !dataChunk) return null;
    if (fmt.audioFormat !== 1 || fmt.bitsPerSample !== 16) return null;

    return { pcm: dataChunk, sampleRate: fmt.sampleRate, channels: fmt.channels };
}

// ======================== GEMİNİ OTURUM YÖNETİMİ ========================

// Ses yollama kuyruğu (ESP32'ye)
let esp32AudioQueue = [];
let isSendingAudio = false;
let playbackActive = false;
let playbackEndTimer = null; // Debounce: playback bitişini geciktir

// Ses seviyesi uygula (PCM16 buffer'ına volume çarpanı)
function applyVolume(pcmBuffer, volume) {
    if (volume === 1.0) return pcmBuffer;
    const out = Buffer.from(pcmBuffer);
    for (let i = 0; i < out.length - 1; i += 2) {
        let sample = out.readInt16LE(i);
        sample = Math.round(sample * volume);
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        out.writeInt16LE(sample, i);
    }
    return out;
}

async function processAudioQueue() {
    if (isSendingAudio) return;
    isSendingAudio = true;

    // Yeni ses geldi → bekleyen "playback bitti" zamanlayıcısını iptal et
    if (playbackEndTimer) {
        clearTimeout(playbackEndTimer);
        playbackEndTimer = null;
    }

    if (!playbackActive) {
        playbackActive = true;
        uiLog("🔊 Wector konuşuyor...");
    }

    while (esp32AudioQueue.length > 0) {
        if (!currentEsp32Ws || currentEsp32Ws.readyState !== WebSocket.OPEN) {
            esp32AudioQueue = [];
            break;
        }

        const buf = esp32AudioQueue.shift();
        // Ses seviyesini uygula
        const amplified = applyVolume(buf, aiConfig.volume);

        // ESP32'nin 16KB ringbuffer'ı taşmasın diye akış kontrolü (pacing) gerekli.
        // Veriyi 4096 byte'lık parçalar halinde gönder, her parça arasında
        // ses süresinin %95'i kadar bekle. Bu ESP32 buffer'ını beslerken taşırmaz.
        // 4096 byte = ~85ms ses → setTimeout jitter'ı (%±12) daha az etkili.
        const CHUNK_SIZE = 4096;

        for (let i = 0; i < amplified.length; i += CHUNK_SIZE) {
            if (!currentEsp32Ws || currentEsp32Ws.readyState !== WebSocket.OPEN) break;
            const chunk = amplified.subarray(i, i + CHUNK_SIZE);
            currentEsp32Ws.send(chunk);
            // 24kHz PCM16 mono = 48000 byte/s
            // 4096 byte ≈ 85ms ses süresi, %95 hızda gönder → ~81ms bekleme
            const durationMs = (chunk.length / 48000) * 1000;
            await new Promise(r => setTimeout(r, durationMs * 0.95));
        }
    }

    isSendingAudio = false;

    // Gemini sesi parça parça gönderir. Her parça sonrası hemen
    // "playback bitti" demek yerine 500ms bekle. Bu sürede yeni
    // ses parçası gelirse timer iptal olur ve playback kesintisiz devam eder.
    // Gelmezse 500ms sonra mic açılır ve dinleme moduna geçilir.
    if (playbackEndTimer) clearTimeout(playbackEndTimer);
    playbackEndTimer = setTimeout(() => {
        playbackEndTimer = null;
        if (esp32AudioQueue.length === 0 && !isSendingAudio) {
            playbackActive = false;
            uiLog("🎤 Dinleniyor...");
        }
    }, 500);
}

function enqueueAudioForEsp32(buffer) {
    esp32AudioQueue.push(buffer);
    processAudioQueue();
}

function sendGemini(obj, label) {
    if (currentGeminiWs && currentGeminiWs.readyState === WebSocket.OPEN) {
        currentGeminiWs.send(JSON.stringify(obj));
    }
}

// ======================== TARAYICI MİKROFONU SES İŞLEME ========================
// Telefondaki data/index.html'den gelen PCM16 ses verisi burada işlenir.
// VAD (ses algılama) uygular ve Gemini'ye iletir.
function handleBrowserAudioData(data) {
    if (!isAIRunning || !isGeminiSetupComplete || !currentGeminiWs || currentGeminiWs.readyState !== WebSocket.OPEN) return;

    // Playback sırasında mic susturulur (çift konuşmayı önler)
    if (playbackActive) return;

    // PCM16 hizalama
    if (data.length % 2 !== 0) {
        data = data.subarray(0, data.length - 1);
    }

    // Seviye ölçümü (VAD)
    let sum = 0;
    for (let i = 0; i < data.length; i += 2) {
        sum += Math.abs(data.readInt16LE(i));
    }
    const avg = sum / (data.length / 2);
    const chunkMs = (data.length / 2) / 16000 * 1000;

    // Noise floor güncelle (sessiz anlardan öğren)
    if (!vadSpeaking) {
        vadNoiseFloor = (vadNoiseFloor * 0.98) + (avg * 0.02);
    }

    const startThr = Math.max(150, vadNoiseFloor * 1.8);
    const stopThr  = Math.max(100, vadNoiseFloor * 1.3);

    if (!vadSpeaking) {
        if (avg >= startThr) {
            vadSpeaking = true;
            vadSilenceMs = 0;
            if (!vadStreamActive) {
                vadStreamActive = true;
                sendGemini({ realtimeInput: { activityStart: {} } }, "activityStart");
            }
            uiLog(`🗣️ Konuşma algılandı (avg=${avg.toFixed(0)}, thr=${startThr.toFixed(0)})`);
        }
    } else {
        if (avg < stopThr) {
            vadSilenceMs += chunkMs;
            if (vadSilenceMs >= 700) {
                vadSpeaking = false;
                vadSilenceMs = 0;
                if (vadStreamActive) {
                    vadStreamActive = false;
                    sendGemini({ realtimeInput: { activityEnd: {} } }, "activityEnd");
                    flushTranscript('user');
                }
                uiLog(`🤫 Konuşma bitti (avg=${avg.toFixed(0)})`);
            }
        } else {
            vadSilenceMs = 0;
        }
    }

    // Sessizse Gemini'ye gönderme
    const now = Date.now();
    if (!vadSpeaking) {
        if (now - vadLastStateLog > 8000) {
            vadLastStateLog = now;
            uiLog(`🔇 Sessiz (avg=${avg.toFixed(0)}, noise=${vadNoiseFloor.toFixed(0)})`);
        }
        return;
    }

    const audioBase64 = data.toString('base64');
    sendGemini({
        realtimeInput: {
            audio: {
                mimeType: "audio/pcm;rate=16000",
                data: audioBase64
            }
        }
    }, "realtimeInput.audio");
}

function startGeminiSession() {
    if (!currentEsp32Ws || currentEsp32Ws.readyState !== WebSocket.OPEN) {
        uiLog('❌ ESP32 bağlı değil, AI başlatılamıyor.');
        return;
    }

    uiLog('🔄 Gemini Multimodal Live API bağlantısı kuruluyor...');
    transcriptHistory = [];
    lastGeminiAudioMimeType = "";
    esp32AudioQueue = [];
    isSendingAudio = false;
    playbackActive = false;
    currentArmAngle = 0;
    currentHeadAngle = 30;
    // VAD sıfırla
    vadSpeaking = false;
    vadSilenceMs = 0;
    vadNoiseFloor = 200;
    vadStreamActive = false;

    const HOST = 'generativelanguage.googleapis.com';
    const WS_URL = `wss://${HOST}/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=${GEMINI_API_KEY}`;

    const geminiWs = new WebSocket(WS_URL);
    currentGeminiWs = geminiWs;

    geminiWs.on('open', () => {
        isGeminiConnected = true;
        isGeminiSetupComplete = false;
        uiLog('✅ Gemini bağlantısı kuruldu! Session başlatılıyor...');

        // --- SESSION SETUP (Kişiselleştirilmiş) ---
        const sessionSetup = {
            setup: {
                model: aiConfig.model,
                generationConfig: {
                    temperature: aiConfig.temperature,
                    responseModalities: ["AUDIO"],
                    speechConfig: {
                        voiceConfig: {
                            prebuiltVoiceConfig: {
                                voiceName: aiConfig.voiceName
                            }
                        }
                    }
                },
                systemInstruction: {
                    parts: [{
                        text: aiConfig.systemPrompt
                    }]
                },
                // Manuel VAD: activityStart/activityEnd gönderiyoruz
                realtimeInputConfig: {
                    automaticActivityDetection: { disabled: true }
                },
                tools: [{
                    functionDeclarations: [
                        {
                            name: "move_robot",
                            description: "Robotu belirtilen yönde ve hızda hareket ettir",
                            parameters: {
                                type: "object",
                                properties: {
                                    direction: {
                                        type: "string",
                                        enum: ["FORWARD", "BACKWARD", "LEFT", "RIGHT", "STOP"],
                                        description: "Hareket yönü"
                                    },
                                    speed: {
                                        type: "integer",
                                        description: "Hız değeri (0-255 arası, varsayılan 150)"
                                    }
                                },
                                required: ["direction"]
                            }
                        },
                        {
                            name: "set_mood",
                            description: "Robotun yüz ifadesini ve ses tepkisini ayarla",
                            parameters: {
                                type: "object",
                                properties: {
                                    mood: {
                                        type: "string",
                                        enum: ["HAPPY", "ANGRY", "TIRED", "CONFUSED", "DEFAULT"],
                                        description: "Duygu durumu"
                                    }
                                },
                                required: ["mood"]
                            }
                        },
                        {
                            name: "set_lights",
                            description: "Robottaki LED ışıklarını kontrol et",
                            parameters: {
                                type: "object",
                                properties: {
                                    mode: {
                                        type: "string",
                                        enum: ["OFF", "STATIC", "RAINBOW", "PULSE", "POLICE"],
                                        description: "Işık modu"
                                    },
                                    color: {
                                        type: "string",
                                        description: "#RRGGBB formatında HEX renk kodu (STATIC ve PULSE modları için)"
                                    }
                                },
                                required: ["mode"]
                            }
                        },
                        {
                            name: "play_dance",
                            description: "Robota dans sekansı yaptır (1-4 arası numaralar mevcuttur)",
                            parameters: {
                                type: "object",
                                properties: {
                                    dance_number: {
                                        type: "integer",
                                        description: "Dans sekansı numarası (1-4)"
                                    }
                                },
                                required: ["dance_number"]
                            }
                        },
                        {
                            name: "control_servo",
                            description: "Robotun kol (arm) veya kafa (head) servosunu kontrol et. Göreceli modda mevcut açıya delta eklenir/çıkarılır. 'biraz' için ±15, normal için ±30, 'çok' için ±45 kullan.",
                            parameters: {
                                type: "object",
                                properties: {
                                    servo: {
                                        type: "string",
                                        enum: ["arm", "head"],
                                        description: "'arm'=kol (0-180 derece, 0=aşağı, 180=yukarı), 'head'=kafa (0-60 derece, 0=aşağı, 60=yukarı)"
                                    },
                                    mode: {
                                        type: "string",
                                        enum: ["absolute", "relative", "min", "max"],
                                        description: "'absolute'=belirli açıya git, 'relative'=mevcut açıya delta ekle (+ kaldırır, - indirir), 'min'=en aşağı, 'max'=en yukarı"
                                    },
                                    value: {
                                        type: "integer",
                                        description: "'absolute' modda hedef açı (derece), 'relative' modda delta. Pozitif=yukarı/kaldır, Negatif=aşağı/indir."
                                    }
                                },
                                required: ["servo", "mode"]
                            }
                        },
                        {
                            name: "move_for_duration",
                            description: "Robotu belirli bir süre boyunca hareket ettir, süre dolunca otomatik olarak durdur.",
                            parameters: {
                                type: "object",
                                properties: {
                                    direction: {
                                        type: "string",
                                        enum: ["FORWARD", "BACKWARD", "LEFT", "RIGHT"],
                                        description: "Hareket yönü"
                                    },
                                    duration_ms: {
                                        type: "integer",
                                        description: "Hareket süresi milisaniye cinsinden. 1 saniye=1000, 2 saniye=2000. Maksimum 10000 (10sn)."
                                    },
                                    speed: {
                                        type: "integer",
                                        description: "Hız değeri (0-255). Belirtilmezse mevcut varsayılan hız kullanılır."
                                    }
                                },
                                required: ["direction", "duration_ms"]
                            }
                        },
                        {
                            name: "set_speed",
                            description: "Robotun varsayılan hareket hızını yüzde olarak ayarla. Sonraki move_robot ve move_for_duration komutlarında bu hız kullanılır.",
                            parameters: {
                                type: "object",
                                properties: {
                                    percentage: {
                                        type: "integer",
                                        description: "Hız yüzdesi (0-100). %0=dur, %50=orta, %100=maksimum"
                                    }
                                },
                                required: ["percentage"]
                            }
                        }
                    ]
                }]
            }
        };

        // Transkript sadece açıksa ekle (token tasarrufu)
        if (aiConfig.transcriptionEnabled) {
            sessionSetup.setup.inputAudioTranscription = {};
            sessionSetup.setup.outputAudioTranscription = {};
        }

        // Birikmiş transkript tamponlarını temizle
        pendingUserTranscript = '';
        pendingAssistantTranscript = '';

        sendGemini(sessionSetup, "setup");
    });

    geminiWs.on('message', (message) => {
        try {
            const raw = typeof message === 'string' ? message : message.toString('utf8');
            const data = JSON.parse(raw);

            // Setup tamamlandı
            if (data.setupComplete) {
                isGeminiSetupComplete = true;
                isAIRunning = true;
                uiLog('🟢 AI Aktif! Mikrofondan ses bekleniyor...');

                // ESP32'ye AI modunu aç komutu gönder
                if (currentEsp32Ws && currentEsp32Ws.readyState === WebSocket.OPEN) {
                    currentEsp32Ws.send(JSON.stringify({
                        type: "server_command",
                        modeCommand: "AI"
                    }));
                    console.log('[SİSTEM] ESP32\'ye AI modu komutu gönderildi.');
                }

                broadcastStatus();
            }

            // Transkript biriktirme (serverContent içinde)
            if (data.serverContent) {
                const sc = data.serverContent;
                if (sc.inputTranscription && sc.inputTranscription.text) {
                    appendTranscript('user', sc.inputTranscription.text);
                }
                if (sc.outputTranscription && sc.outputTranscription.text) {
                    appendTranscript('assistant', sc.outputTranscription.text);
                }
            }

            // Transkript biriktirme (üst seviyede)
            if (data.inputTranscription && data.inputTranscription.text) {
                appendTranscript('user', data.inputTranscription.text);
            }
            if (data.outputTranscription && data.outputTranscription.text) {
                appendTranscript('assistant', data.outputTranscription.text);
            }

            // Model ses çıktısı
            if (data.serverContent && data.serverContent.modelTurn) {
                const parts = data.serverContent.modelTurn.parts;
                if (!parts) return;

                parts.forEach(part => {
                    if (part.inlineData && typeof part.inlineData.mimeType === 'string' && part.inlineData.mimeType.toLowerCase().includes("audio")) {
                        const mimeType = part.inlineData.mimeType;
                        const rawAudioBuffer = Buffer.from(part.inlineData.data, 'base64');

                        // Format değişikliği bildir
                        if (currentEsp32Ws && currentEsp32Ws.readyState === WebSocket.OPEN && mimeType !== lastGeminiAudioMimeType) {
                            lastGeminiAudioMimeType = mimeType;
                            const rate = parseRateFromMimeType(mimeType);
                            const channels = parseChannelsFromMimeType(mimeType);
                            currentEsp32Ws.send(JSON.stringify({ type: "audio_out_format", mimeType, rate, channels }));
                            uiLog(`🎵 Audio Format: ${mimeType} (rate=${rate ?? "?"}, ch=${channels ?? "?"})`);
                        }

                        let pcmBuffer = rawAudioBuffer;

                        // WAV → PCM
                        if (isWavMimeType(mimeType)) {
                            const extracted = tryExtractPcmFromWav(rawAudioBuffer);
                            if (extracted) {
                                pcmBuffer = extracted.pcm;
                            } else {
                                uiLog(`⚠️ WAV parse hatası (${mimeType})`);
                                return;
                            }
                        }

                        // L16 big-endian → little-endian
                        if (mimeType.toLowerCase().includes('audio/l16')) {
                            const copy = Buffer.from(pcmBuffer);
                            swap16EndianInPlace(copy);
                            pcmBuffer = copy;
                        }

                        // Desteklenmeyen format kontrolü
                        if (!isPcmMimeType(mimeType) && !isWavMimeType(mimeType)) {
                            uiLog(`⚠️ Desteklenmeyen audio format: ${mimeType}`);
                            return;
                        }

                        // PCM16 hizalama
                        if (pcmBuffer.length % 2 !== 0) {
                            pcmBuffer = pcmBuffer.subarray(0, pcmBuffer.length - 1);
                        }

                        // Ses seviyesi kontrolü (Kaldırıldı)
                        if (currentEsp32Ws && currentEsp32Ws.readyState === WebSocket.OPEN) {
                            enqueueAudioForEsp32(pcmBuffer);
                        }
                    }

                    // Text çıktısı
                    if (part.text && part.text.trim() !== "") {
                        appendTranscript('assistant', part.text);
                    }
                });
            }

            // Lifecycle events
            if (data.serverContent) {
                if (data.serverContent.turnComplete) {
                    // Sıra tamamlandı → ÖNCE kullanıcıyı, SONRA asistanı flush et
                    flushTranscript('user');
                    flushTranscript('assistant');
                    uiLog("✔️ Yanıt tamamlandı.");
                }
                if (data.serverContent.generationComplete) uiLog("📝 Üretim tamamlandı.");
                if (data.serverContent.interrupted) {
                    flushTranscript('assistant');
                    uiLog('⚡ Kesinti algılandı, ses kuyruğu temizleniyor.');
                    esp32AudioQueue = [];
                }
            }

            // Hata
            if (data.error) {
                uiLog(`❌ Gemini Hatası: ${data.error.message}`);
                console.error('[Gemini Error]', data.error);
            }

            // *** TOOL CALLING (AI → Robot Fiziksel Kontrol) ***
            if (data.toolCall && data.toolCall.functionCalls) {
                const responses = [];
                for (const call of data.toolCall.functionCalls) {
                    let result = { success: false };
                    if (call.name === "move_robot") {
                        const direction = call.args?.direction ?? "STOP";
                        const speed = call.args?.speed ?? defaultSpeed;
                        sendRobotCommand({ direction, speed });
                        uiLog(`🤖 AI Komutu → Hareket: ${direction} (hız: ${speed})`);
                        result = { success: true, direction, speed };
                    } else if (call.name === "set_mood") {
                        const mood = call.args?.mood ?? "DEFAULT";
                        sendRobotCommand({ mood });
                        uiLog(`🤖 AI Komutu → Mod: ${mood}`);
                        result = { success: true, mood };
                    } else if (call.name === "set_lights") {
                        const mode = call.args?.mode ?? "OFF";
                        const color = call.args?.color ?? "#ffffff";
                        sendRobotCommand({ lightMode: mode, color });
                        uiLog(`🤖 AI Komutu → Işık: ${mode} (${color})`);
                        result = { success: true, mode, color };
                    } else if (call.name === "play_dance") {
                        const danceNum = call.args?.dance_number ?? 1;
                        sendRobotCommand({ mood: `DANCE_${danceNum}` });
                        uiLog(`🤖 AI Komutu → Dans: ${danceNum}`);
                        result = { success: true, dance_number: danceNum };
                    } else if (call.name === "control_servo") {
                        const servo = call.args?.servo;
                        const mode = call.args?.mode;
                        const value = call.args?.value ?? 0;
                        const maxVal = servo === "arm" ? 180 : 60;
                        let current = servo === "arm" ? currentArmAngle : currentHeadAngle;
                        let target;
                        if (mode === "absolute")     target = Math.max(0, Math.min(maxVal, value));
                        else if (mode === "relative") target = Math.max(0, Math.min(maxVal, current + value));
                        else if (mode === "min")      target = 0;
                        else if (mode === "max")      target = maxVal;
                        else                          target = current;
                        if (servo === "arm") {
                            currentArmAngle = target;
                            sendRobotCommand({ arm: target });
                            uiLog(`🤖 AI Komutu → Kol: ${target}° (${mode})`);
                        } else {
                            currentHeadAngle = target;
                            sendRobotCommand({ head: target });
                            uiLog(`🤖 AI Komutu → Kafa: ${target}° (${mode})`);
                        }
                        result = { success: true, servo, mode, target_angle: target };
                    } else if (call.name === "move_for_duration") {
                        const direction = call.args?.direction ?? "FORWARD";
                        const duration_ms = Math.min(call.args?.duration_ms ?? 1000, 10000);
                        const speed = call.args?.speed ?? defaultSpeed;
                        // Önceki zamanlı hareketi iptal et
                        if (moveDurationTimer) {
                            clearTimeout(moveDurationTimer);
                            moveDurationTimer = null;
                        }
                        sendRobotCommand({ direction, speed });
                        uiLog(`🤖 AI Komutu → ${direction} ${duration_ms}ms (hız: ${speed})`);
                        moveDurationTimer = setTimeout(() => {
                            moveDurationTimer = null;
                            sendRobotCommand({ direction: "STOP", speed: 0 });
                            uiLog(`🤖 Otomatik Dur (${duration_ms}ms doldu)`);
                        }, duration_ms);
                        result = { success: true, direction, duration_ms, speed };
                    } else if (call.name === "set_speed") {
                        const pct = Math.max(0, Math.min(100, call.args?.percentage ?? 50));
                        defaultSpeed = Math.round((pct / 100) * 255);
                        uiLog(`🤖 AI Komutu → Hız %${pct} (${defaultSpeed}/255)`);
                        result = { success: true, percentage: pct, raw_speed: defaultSpeed };
                    }
                    responses.push({ id: call.id, name: call.name, response: { output: result } });
                }
                sendGemini({ toolResponse: { functionResponses: responses } }, "toolResponse");
            }
        } catch (e) {
            try {
                const raw = typeof message === 'string' ? message : message.toString('utf8');
                uiLog(`⚠️ Mesaj işlenemedi: ${e?.message ?? e}`);
                if (raw && raw.length) console.log(`[Gemini Raw (ilk 200)]: ${raw.slice(0, 200)}`);
            } catch (_) {
                uiLog(`⚠️ Mesaj işlenemedi: ${e?.message ?? e}`);
            }
        }
    });

    geminiWs.on('close', (code, reason) => {
        isGeminiConnected = false;
        isGeminiSetupComplete = false;
        const wasRunning = isAIRunning;
        isAIRunning = false;
        uiLog(`🔴 Gemini bağlantısı kapandı (Code: ${code}${reason ? ', ' + reason.toString() : ''})`);

        // AI çalışırken kopuş olduysa ESP32'ye bildir
        if (wasRunning && currentEsp32Ws && currentEsp32Ws.readyState === WebSocket.OPEN) {
            currentEsp32Ws.send(JSON.stringify({
                type: "server_command",
                modeCommand: "NORMAL"
            }));
        }

        currentGeminiWs = null;
        broadcastStatus();
    });

    geminiWs.on('error', (err) => {
        uiLog(`❌ Gemini bağlantı hatası: ${err.message}`);
    });

    // ESP32 text mesajlarını dinle (artık binary ses yok, sadece komutlar)
    if (currentEsp32Ws) {
        currentEsp32Ws.removeAllListeners('message');
        currentEsp32Ws.on('message', (data, isBinary) => {
            if (!isBinary) {
                const rawMsg = data.toString();
                try {
                    const msg = JSON.parse(rawMsg);
                    if (msg.command === "STOP_AI") {
                        stopGeminiSession('ESP32 üzerinden durduruldu');
                    }
                } catch(e) { console.log('[ESP32 Text]:', rawMsg); }
            }
        });
    }
}

function attachStandbyMessageListener(ws) {
    ws.removeAllListeners('message');
    ws.on('message', (data, isBinary) => {
        if (!isBinary) {
            const rawMsg = data.toString();
            try {
                const msg = JSON.parse(rawMsg);
                if (msg.command === "START_AI") {
                    if (isSystemReady && !isAIRunning) {
                        uiLog("🚀 ESP32 Yapay Zeka Başlatma isteği gönderdi! Ses kaydı başlıyor.");
                        startGeminiSession();
                    } else if (!isSystemReady) {
                        uiLog("⚠️ ESP32 konuşmak istiyor ama Dashboard'dan 'Başlat'a basılarak sistem hazır hale getirilmemiş.");
                    }
                } else if (msg.command === "STOP_AI") {
                    if (isAIRunning) stopGeminiSession('ESP32 üzerinden durduruldu');
                }
            } catch (e) { console.log('[ESP32 Text]:', rawMsg); }
        }
    });
}

// ======================== ROBOT KOMUT GÖNDERİCİ ========================
function sendRobotCommand(cmd) {
    if (currentEsp32Ws && currentEsp32Ws.readyState === WebSocket.OPEN) {
        currentEsp32Ws.send(JSON.stringify(cmd));
        console.log('[ROBOT CMD]', JSON.stringify(cmd));
    } else {
        uiLog(`⚠️ Robot komutu gönderilemedi: ESP32 bağlı değil. (${JSON.stringify(cmd)})`);
    }
}

function stopGeminiSession(reason) {
    uiLog(`⏹️ AI durduruluyor: ${reason}`);

    // Bekleyen zamanlı hareket varsa iptal et ve robotu durdur
    if (moveDurationTimer) {
        clearTimeout(moveDurationTimer);
        moveDurationTimer = null;
        sendRobotCommand({ direction: "STOP", speed: 0 });
    }

    // ESP32'ye normal moda dönmesini söyle
    if (currentEsp32Ws && currentEsp32Ws.readyState === WebSocket.OPEN) {
        currentEsp32Ws.send(JSON.stringify({
            type: "server_command",
            modeCommand: "NORMAL"
        }));
    }

    // Gemini bağlantısını kapat
    if (currentGeminiWs && currentGeminiWs.readyState === WebSocket.OPEN) {
        currentGeminiWs.close();
    }

    isAIRunning = false;
    isGeminiConnected = false;
    isGeminiSetupComplete = false;
    esp32AudioQueue = [];
    isSendingAudio = false;
    playbackActive = false;
    currentGeminiWs = null;

    // Bekleme modu dinleyicisini geri yükle (eğer bağlantı hala açıksa)
    if (currentEsp32Ws && currentEsp32Ws.readyState === WebSocket.OPEN) {
        attachStandbyMessageListener(currentEsp32Ws);
    }

    broadcastStatus();
}

// ======================== ESP32 BAĞLANTI YÖNETİMİ ========================
wssEsp32.on('connection', (esp32Ws) => {
    currentEsp32Ws = esp32Ws;
    isEsp32Connected = true;
    lastGeminiAudioMimeType = "";
    uiLog('🤖 Robot (ESP32) bağlandı! Dashboard\'dan AI\'ı başlatabilirsiniz.');
    broadcastStatus();

    // Bağlantı sağlığı (Ping) kontrolü (Koptuysa erken algıla)
    esp32Ws.isAlive = true;
    esp32Ws.on('pong', () => { esp32Ws.isAlive = true; });
    const pingInterval = setInterval(() => {
        if (esp32Ws.readyState !== WebSocket.OPEN) return;
        if (esp32Ws.isAlive === false) return esp32Ws.terminate();
        esp32Ws.isAlive = false;
        esp32Ws.ping();
    }, 5000);

    // Standby mod dinleyicisini bağla
    attachStandbyMessageListener(esp32Ws);

    esp32Ws.on('close', () => {
        clearInterval(pingInterval);
        isEsp32Connected = false;
        currentEsp32Ws = null;
        uiLog('🔌 Robot (ESP32) bağlantısı koptu.');

        // AI çalışıyorsa durdur
        if (isAIRunning) {
            stopGeminiSession('ESP32 bağlantısı koptu');
        }

        broadcastStatus();
    });

    esp32Ws.on('error', (err) => {
        console.error('[ESP32 WS Error]:', err.message);
    });
});

// ======================== TARAYICI MİKROFONU BAĞLANTISI (/mic) ========================
wssBrowserMic.on('connection', (browserWs) => {
    // Önceki bağlantıyı kapat (aynı anda tek mikrofon)
    if (currentBrowserMicWs && currentBrowserMicWs !== browserWs) {
        currentBrowserMicWs.terminate();
    }
    currentBrowserMicWs = browserWs;
    uiLog('🎤 Tarayıcı mikrofonu bağlandı!');
    broadcastStatus();

    browserWs.on('message', (data, isBinary) => {
        if (isBinary) {
            handleBrowserAudioData(data);
        }
    });

    browserWs.on('close', () => {
        if (currentBrowserMicWs === browserWs) currentBrowserMicWs = null;
        // Konuşma stream'i açıksa kapat
        if (vadStreamActive) {
            vadStreamActive = false;
            vadSpeaking = false;
            sendGemini({ realtimeInput: { activityEnd: {} } }, "activityEnd-micDisconnect");
        }
        uiLog('🎤 Tarayıcı mikrofonu bağlantısı kapandı.');
        broadcastStatus();
    });

    browserWs.on('error', (err) => {
        console.error('[Browser Mic WS Error]:', err.message);
    });
});
