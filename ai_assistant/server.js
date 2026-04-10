import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import express from 'express';
import { WebSocketServer, WebSocket } from 'ws';
import 'dotenv/config';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

function createWavHeader(dataSize) {
    const header = Buffer.alloc(44);
    header.write('RIFF', 0);
    header.writeUInt32LE(36 + dataSize, 4);
    header.write('WAVE', 8);
    header.write('fmt ', 12);
    header.writeUInt32LE(16, 16);
    header.writeUInt16LE(1, 20);               // PCM
    header.writeUInt16LE(1, 22);               // Mono
    header.writeUInt32LE(16000, 24);           // 16kHz
    header.writeUInt32LE(32000, 28);           // ByteRate
    header.writeUInt16LE(2, 32);               // BlockAlign
    header.writeUInt16LE(16, 34);              // 16-bit
    header.write('data', 36);
    header.writeUInt32LE(dataSize, 40);
    return header;
}

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

// ======================== SES KAYDI HAVUZU ========================
const recordedMicChunks = [];

function saveWavToFile() {
    if (recordedMicChunks.length > 0) {
        const pcmData = Buffer.concat(recordedMicChunks);
        const wavHeader = createWavHeader(pcmData.length);
        fs.writeFileSync(path.join(__dirname, 'test_kayit.wav'), Buffer.concat([wavHeader, pcmData]));
        console.log(`\n[BİLGİ] Ses kaydı "test_kayit.wav" dosyasına kaydedildi! Boyut: ${pcmData.length} byte.`);
        recordedMicChunks.length = 0;
    }
}

process.on('SIGINT', () => {
    console.log("\n[SİSTEM] Node.js kapatılıyor, ses kaydı diske yazılıyor...");
    saveWavToFile();
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
    if (isAIRunning) {
        return res.json({ success: false, error: 'AI zaten çalışıyor!' });
    }

    startGeminiSession();
    res.json({ success: true });
});

// --- API: AI'ı durdur ---
app.post('/api/stop-ai', (req, res) => {
    stopGeminiSession('Manuel durdurma');
    res.json({ success: true });
});

const UI_PORT = 3000;
const uiServer = app.listen(UI_PORT, () => {
    console.log(`💻 AI Dashboard: http://localhost:${UI_PORT}`);
    console.log(`📡 ESP32 Portu: ws://localhost:8080`);
});

// WebSocket Sunucuları
const wssEsp32 = new WebSocketServer({ port: 8080 });
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
                if (!isEsp32Connected) return uiLog('❌ ESP32 bağlı değil!');
                if (!GEMINI_API_KEY) return uiLog('❌ API Anahtarı yok!');
                if (isAIRunning) return uiLog('⚠️ AI zaten çalışıyor!');

                // Ayarları güncelle
                if (msg.config) {
                    Object.assign(aiConfig, msg.config);
                    saveConfig();
                }
                startGeminiSession();
            } else if (msg.type === 'stop-ai') {
                stopGeminiSession('Dashboard üzerinden durduruldu');
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
let latestLog = "Sunucu başlatıldı. Robot ile bağlantı bekleniyor...";
let transcriptHistory = []; // Sohbet geçmişi
let lastGeminiAudioMimeType = "";

// Aktif bağlantılar
let currentEsp32Ws = null;
let currentGeminiWs = null;

function broadcastStatus() {
    const status = {
        esp32: isEsp32Connected,
        gemini: isGeminiConnected,
        aiRunning: isAIRunning,
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

function setMicMute(mute) {
    if (currentEsp32Ws && currentEsp32Ws.readyState === WebSocket.OPEN) {
        currentEsp32Ws.send(JSON.stringify({ type: "mic_mute", mute }));
    }
}

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
        setMicMute(true);
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
            setMicMute(false);
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

    // Kayıt havuzunu temizle
    recordedMicChunks.length = 0;
    console.log(`[BİLGİ] Ses kayıt havuzu temizlendi.`);

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
                }
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

    // --- VAD (Voice Activity Detection) değişkenleri ---
    let vadSpeaking = false;
    let vadSilenceMs = 0;
    let vadNoiseFloor = 200;
    let vadLastStateLog = 0;
    let vadStreamActive = false;

    // Bar çizimi değişkenleri
    let barSampleSum = 0;
    let barSampleCount = 0;
    let lastBarPrint = Date.now();
    const BAR_INTERVAL_MS = 500;

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
                        uiTranscript('assistant', part.text);
                    }
                });
            }

            // Lifecycle events
            if (data.serverContent) {
                if (data.serverContent.turnComplete) {
                    // Sıra tamamlandı → birikmiş transkriptleri flush et
                    flushTranscript('assistant');
                    flushTranscript('user');
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

    // ======================== ESP32'DEN GELEN SES VERİSİ ========================
    // Bu handler ESP32 ws için ayarlanır (mevcut bağlantı üzerinden)
    // Not: handler zaten ESP32 connection event'inde ayarlanıyor

    // ESP32 mesaj işleme (Gemini oturumu başladığında)
    function handleEsp32AudioData(data) {
        if (!isAIRunning || !isGeminiSetupComplete || !currentGeminiWs || currentGeminiWs.readyState !== WebSocket.OPEN) return;

        // Playback sırasında mic kapalı (echo cancellation)
        if (playbackActive) return;

        // PCM16 hizalama
        if (data.length % 2 !== 0) {
            data = data.subarray(0, data.length - 1);
        }

        // Seviye ölçümü (VAD + debug)
        let sum = 0;
        for (let i = 0; i < data.length; i += 2) {
            sum += Math.abs(data.readInt16LE(i));
        }
        const avg = sum / (data.length / 2);
        const chunkMs = (data.length / 2) / 16000 * 1000;

        // Noise floor güncelle (sessiz anlardan)
        if (!vadSpeaking) {
            vadNoiseFloor = (vadNoiseFloor * 0.98) + (avg * 0.02);
        }

        // VAD eşikleri
        const startThr = Math.max(150, vadNoiseFloor * 1.8);
        const stopThr = Math.max(100, vadNoiseFloor * 1.3);

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
                        // Konuşma bitince kullanıcının transkriptini UI'da tek parça olarak göster
                        flushTranscript('user');
                    }
                    uiLog(`🤫 Konuşma bitti (avg=${avg.toFixed(0)})`);
                }
            } else {
                vadSilenceMs = 0;
            }
        }

        // Ses çubuğu (500ms aralıklı)
        barSampleSum += sum;
        barSampleCount += (data.length / 2);
        const now = Date.now();
        if (now - lastBarPrint >= BAR_INTERVAL_MS) {
            const barAvg = barSampleCount > 0 ? barSampleSum / barSampleCount : 0;
            const barLength = Math.min(Math.floor(barAvg / 20), 50);
            const bar = '█'.repeat(barLength) || '░';
            console.log(`🎤 [${new Date().toLocaleTimeString()}] Ses: ${Math.floor(barAvg).toString().padStart(4, '0')} | ${bar}`);
            barSampleSum = 0;
            barSampleCount = 0;
            lastBarPrint = now;
        }

        // Sessiz anları Gemini'ye gönderme
        if (!vadSpeaking) {
            if (now - vadLastStateLog > 8000) {
                vadLastStateLog = now;
                uiLog(`🔇 Sessiz (avg=${avg.toFixed(0)}, noise=${vadNoiseFloor.toFixed(0)})`);
            }
            return;
        }

        // ✅ Gemini'ye yeni API formatında ses gönder (mediaChunks DEPRECATED!)
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

    // handleEsp32AudioData fonksiyonunu mevcut ESP32 bağlantısına bağla
    if (currentEsp32Ws) {
        // Önceki handler'ı temizle
        currentEsp32Ws.removeAllListeners('message');

        currentEsp32Ws.on('message', (data, isBinary) => {
            if (isBinary || Buffer.isBuffer(data)) {
                // Ses kaydı
                recordedMicChunks.push(Buffer.from(data));
                // Audio işleme
                handleEsp32AudioData(data);
            } else {
                console.log('[ESP32 Text]:', data.toString());
            }
        });
    }
}

function stopGeminiSession(reason) {
    uiLog(`⏹️ AI durduruluyor: ${reason}`);

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

    // Ses kaydını kaydet
    saveWavToFile();

    broadcastStatus();
}

// ======================== ESP32 BAĞLANTI YÖNETİMİ ========================
wssEsp32.on('connection', (esp32Ws) => {
    currentEsp32Ws = esp32Ws;
    isEsp32Connected = true;
    lastGeminiAudioMimeType = "";
    uiLog('🤖 Robot (ESP32) bağlandı! Dashboard\'dan AI\'ı başlatabilirsiniz.');
    broadcastStatus();

    // Kayıt havuzunu temizle
    recordedMicChunks.length = 0;

    // ESP32'den gelen mesajları dinle (AI başlatılmadan önce de)
    esp32Ws.on('message', (data, isBinary) => {
        if (isBinary || Buffer.isBuffer(data)) {
            // AI çalışmıyorken de kayıt havuzuna al
            recordedMicChunks.push(Buffer.from(data));
        } else {
            console.log('[ESP32 Text]:', data.toString());
        }
    });

    esp32Ws.on('close', () => {
        isEsp32Connected = false;
        currentEsp32Ws = null;
        uiLog('🔌 Robot (ESP32) bağlantısı koptu.');

        // AI çalışıyorsa durdur
        if (isAIRunning) {
            stopGeminiSession('ESP32 bağlantısı koptu');
        }

        saveWavToFile();
        broadcastStatus();
    });

    esp32Ws.on('error', (err) => {
        console.error('[ESP32 WS Error]:', err.message);
    });
});
