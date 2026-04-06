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
    header.writeUInt32LE(36 + dataSize, 4);   // ChunkSize
    header.write('WAVE', 8);
    header.write('fmt ', 12);
    header.writeUInt32LE(16, 16);              // Subchunk1Size
    header.writeUInt16LE(1, 20);               // AudioFormat: PCM
    header.writeUInt16LE(1, 22);               // NumChannels: 1 (Mono)
    header.writeUInt32LE(16000, 24);           // SampleRate: 16000 Hz
    header.writeUInt32LE(32000, 28);           // ByteRate
    header.writeUInt16LE(2, 32);               // BlockAlign
    header.writeUInt16LE(16, 34);              // BitsPerSample: 16
    header.write('data', 36);
    header.writeUInt32LE(dataSize, 40);        // Subchunk2Size
    return header;
}

let GEMINI_API_KEY = process.env.GEMINI_API_KEY || '';

// Global RAM havuzu (bağlantı kopsa da, node kapatılsa da içeriği kaydetmek için)
const recordedMicChunks = [];

function saveWavToFile() {
    if (recordedMicChunks.length > 0) {
        const pcmData = Buffer.concat(recordedMicChunks);
        const wavHeader = createWavHeader(pcmData.length);
        fs.writeFileSync(path.join(__dirname, 'test_kayit.wav'), Buffer.concat([wavHeader, pcmData]));
        console.log(`\n[BİLGİ] Ses kaydı başarıyla "test_kayit.wav" dosyasına kaydedildi! Boyut: ${pcmData.length} byte.`);
        recordedMicChunks.length = 0;
    }
}

// Eğer sunucu terminalden (Ctrl+C) kapatılırsa, yine de diske yaz!
process.on('SIGINT', () => {
    console.log("\n[SİSTEM] Node.js kapatılıyor, RAM'deki mevcut sesler diske yazılıyor...");
    saveWavToFile();
    process.exit();
});

const app = express();
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// Arayüze API Anahtarının yüklü olup olmadığını bildir
app.get('/api/status', (req, res) => {
    res.json({
        hasApiKey: !!GEMINI_API_KEY && GEMINI_API_KEY !== 'GEMINI_API_KEY_BURAYA'
    });
});

app.post('/api/save-key', (req, res) => {
    const { apiKey } = req.body;
    if (apiKey) {
        GEMINI_API_KEY = apiKey;
        const envPath = path.join(__dirname, '.env');
        fs.writeFileSync(envPath, `GEMINI_API_KEY=${apiKey}\n`);
        uiLog(`Gemini API Anahtarı başarıyla güncellendi ve sisteme yüklendi.`);
        res.json({ success: true });
    } else {
        res.json({ success: false });
    }
});

const UI_PORT = 3000;
const uiServer = app.listen(UI_PORT, () => {
    console.log(`💻 AI Dashboard: http://localhost:${UI_PORT} adresinden arayüze ulaşabilirsiniz!`);
    console.log(`📡 ESP32 Portu: ws://localhost:8080`);
});

// WebSocket Sunucuları
const wssEsp32 = new WebSocketServer({ port: 8080 });
const wssUI = new WebSocketServer({ server: uiServer, path: '/ui' });

let uiClients = new Set();
wssUI.on('connection', (ws) => {
    uiClients.add(ws);
    broadcastStatus();
    ws.on('close', () => uiClients.delete(ws));
});

let isEsp32Connected = false;
let isOpenAIConnected = false; // Using same var name for UI tracking
let isGeminiSetupComplete = false;
let latestLog = "Sunucu başlatıldı. Robot ile bağlantı bekleniyor...";
let transcriptLog = "";
let lastGeminiAudioMimeType = "";

function broadcastStatus() {
    const status = {
        esp32: isEsp32Connected,
        openai: isOpenAIConnected, 
        log: latestLog,
        transcript: transcriptLog
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

function uiTranscript(msg) {
    transcriptLog = msg;
    broadcastStatus();
}

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
    // Minimal WAV (RIFF) parser for PCM/16-bit. Returns { pcm, sampleRate, channels } or null.
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
            const audioFormat = wavBuffer.readUInt16LE(chunkStart + 0);
            const channels = wavBuffer.readUInt16LE(chunkStart + 2);
            const sampleRate = wavBuffer.readUInt32LE(chunkStart + 4);
            const bitsPerSample = wavBuffer.readUInt16LE(chunkStart + 14);
            fmt = { audioFormat, channels, sampleRate, bitsPerSample };
        } else if (id === 'data') {
            dataChunk = wavBuffer.subarray(chunkStart, chunkEnd);
        }

        // Chunks are word-aligned
        offset = chunkEnd + (size % 2);
        if (fmt && dataChunk) break;
    }

    if (!fmt || !dataChunk) return null;
    if (fmt.audioFormat !== 1) return null; // PCM
    if (fmt.bitsPerSample !== 16) return null;

    return {
        pcm: dataChunk,
        sampleRate: fmt.sampleRate,
        channels: fmt.channels
    };
}

wssEsp32.on('connection', (esp32Ws) => {
    isEsp32Connected = true;
    lastGeminiAudioMimeType = "";
    uiLog('[ESP32] Robot Bağlandı! AI Modu Aktif.');
    broadcastStatus();

    // --- MİKROFON TESTİ İÇİN OTOMATİK SES KAYDI RAM HAVUZU ---
    // Her yeni bağlantıda eski RAM temizlenir.
    recordedMicChunks.length = 0; 
    console.log(`[BİLGİ] Robotun duyduğu sesler RAM'de biriktiriliyor. (Bağlantı koparsa VEYA konsoldan Ctrl+C ile çıkarsanız otomatik kaydedilecektir)...`);
    // -----------------------------------------------------------

    if (!GEMINI_API_KEY || GEMINI_API_KEY === 'GEMINI_API_KEY_BURAYA') {
        uiLog("HATA: Gemini API Anahtarı eksik! Lütfen arayüzden giriniz.");
        return;
    }

    uiLog('[Gemini] Multimodal Live API bağlantısı kuruluyor...');
    
    // Gemini Live API URL'si
    const HOST = 'generativelanguage.googleapis.com';
    // Not: Dokumanlarda v1beta endpoint oneriliyor; v1alpha preview akisi zaman zaman 1011 verebiliyor.
    const WS_URL = `wss://${HOST}/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=${GEMINI_API_KEY}`;
    
    const geminiWs = new WebSocket(WS_URL);

    let lastGeminiClientMessageLabel = "";
    let lastGeminiClientMessagePreview = "";
    function sendGemini(obj, label) {
        const payload = JSON.stringify(obj);
        lastGeminiClientMessageLabel = label;
        lastGeminiClientMessagePreview = payload.slice(0, 220);
        geminiWs.send(payload);
    }

    // Ses Yollama Kuyrugu (ESP32 RAM'i tasmamasi icin audio'yu yavasca aktarir)
    let esp32AudioQueue = [];
    let isSendingAudio = false;
    let playbackActive = false;

    function setMicMute(mute) {
        if (esp32Ws.readyState === WebSocket.OPEN) {
            esp32Ws.send(JSON.stringify({ type: "mic_mute", mute }));
        }
    }

    async function processAudioQueue() {
        if (isSendingAudio) return;
        isSendingAudio = true;
        
        if (!playbackActive) {
            playbackActive = true;
            setMicMute(true);
            uiLog("[AUDIO] Playback basladi, mic susturuldu");
        }

        while(esp32AudioQueue.length > 0) {
            if (esp32Ws.readyState !== WebSocket.OPEN) {
                esp32AudioQueue = [];
                break;
            }

            const buf = esp32AudioQueue.shift();
            // CHUNK_SIZE = 4096 byte = 2048 sample = ~85 milisaniye (24kHz'de)
            const CHUNK_SIZE = 4096; 

            for(let i = 0; i < buf.length; i += CHUNK_SIZE) {
                if (esp32Ws.readyState !== WebSocket.OPEN) break;

                const chunk = buf.subarray(i, i + CHUNK_SIZE);
                esp32Ws.send(chunk);

                // Gonderilen sesin gercek uzunlugu
                // 24000 PCM16 formatta -> saniyede 48000 byte
                const durationMs = (chunk.length / 48000) * 1000;
                
                // Cok ufak erken gonderip ESP32 tamponunun (Ringbuf) stabil dolmasini sagliyoruz
                await new Promise(r => setTimeout(r, durationMs * 0.90));
            }
        }

        isSendingAudio = false;
        playbackActive = false;
        setMicMute(false);
        uiLog("[AUDIO] Playback bitti, mic dinliyor");
    }

    function enqueueAudioForEsp32(buffer) {
        esp32AudioQueue.push(buffer);
        processAudioQueue();
    }

    geminiWs.on('open', () => {
        isOpenAIConnected = true;
        isGeminiSetupComplete = false; // Setup bitmeden veri yollanmamali
        uiLog('[Gemini] Bağlantı başarılı! Session başlatılıyor...');
        
        // Gemini Session Setup
        const sessionSetup = {
            setup: {
                model: "models/gemini-3.1-flash-live-preview", // Kullanıcının belirttiği model
                generationConfig: {
                    responseModalities: ["AUDIO"],
                    speechConfig: {
                        voiceConfig: {
                            prebuiltVoiceConfig: {
                                voiceName: "Aoede" // Diğer seçenekler: Puck, Charon, Kore, Fenrir, Puma
                            }
                        }
                    }
                },
                // Otomatik activity detection kapali: VAD ile activityStart/activityEnd gonderecegiz.
                realtimeInputConfig: {
                    automaticActivityDetection: { disabled: true }
                },
                // Debug icin input/output transcription ac (alan yok, bos obje yeterli)
                inputAudioTranscription: {},
                outputAudioTranscription: {},
                systemInstruction: {
                    parts: [{
                        text: "Sen Wector adında yardımsever, cana yakın ve fütüristik bir robotsun. Feramuz isimli yapımcın tarafından yaratıldın. İnsanlarla doğal ve esprili bir dille samimi şekilde konuşursun. Cevapların her zaman çok kısa olmalı, gereksiz cümle uzatma."
                    }]
                }
            }
        };
        sendGemini(sessionSetup, "setup");
    });

    geminiWs.on('message', (message) => {
        try {
            const raw = typeof message === 'string' ? message : message.toString('utf8');
            const data = JSON.parse(raw);
            
            if (data.setupComplete) {
                isGeminiSetupComplete = true;
                uiLog('[Gemini] Setup tamamlandı. Mikrofondan ses aktarımı başladı.');
            }

            // Eğer serverContent gelmişse
            // Debug: eger Gemini input/output transcription gonderiyorsa UI'da goster
            if (data.serverContent) {
                const sc = data.serverContent;
                if (sc.inputTranscription && sc.inputTranscription.text) {
                    uiTranscript(`[SEN]: ${sc.inputTranscription.text}`);
                }
                if (sc.outputTranscription && sc.outputTranscription.text) {
                    uiTranscript(`[WECTOR]: ${sc.outputTranscription.text}`);
                }
            }

            if (data.serverContent && data.serverContent.modelTurn) {
                const parts = data.serverContent.modelTurn.parts;
                
                parts.forEach(part => {
                    // Sesi yakala
                    if (part.inlineData && typeof part.inlineData.mimeType === 'string' && part.inlineData.mimeType.toLowerCase().includes("audio")) {
                        const mimeType = part.inlineData.mimeType;
                        const rawAudioBuffer = Buffer.from(part.inlineData.data, 'base64');

                        // Ilk paket veya format degisimi: ESP32'ye format bilgisini text olarak yolla (clock/debug icin)
                        if (esp32Ws.readyState === 1 && mimeType !== lastGeminiAudioMimeType) {
                            lastGeminiAudioMimeType = mimeType;
                            const rate = parseRateFromMimeType(mimeType);
                            const channels = parseChannelsFromMimeType(mimeType);
                            esp32Ws.send(JSON.stringify({ type: "audio_out_format", mimeType, rate, channels }));
                            uiLog(`[Gemini] Audio mimeType: ${mimeType} (rate=${rate ?? "?"}, channels=${channels ?? "?"})`);
                        }

                        let pcmBuffer = rawAudioBuffer;

                        // WAV gelirse: header'i soy ve PCM'i al
                        if (isWavMimeType(mimeType)) {
                            const extracted = tryExtractPcmFromWav(rawAudioBuffer);
                            if (extracted) {
                                pcmBuffer = extracted.pcm;
                            } else {
                                uiLog(`[Gemini] UYARI: WAV parse edilemedi, ham veri atlanacak. mimeType=${mimeType}`);
                                return;
                            }
                        }

                        // audio/L16 genelde big-endian (network order) kabul edilir; ESP32 tarafinda LE bekledigimiz icin cevir.
                        if (mimeType.toLowerCase().includes('audio/l16')) {
                            const copy = Buffer.from(pcmBuffer);
                            swap16EndianInPlace(copy);
                            pcmBuffer = copy;
                        }

                        // PCM degilse: ESP32 tarafinda decode edemeyiz
                        if (!isPcmMimeType(mimeType) && !isWavMimeType(mimeType)) {
                            uiLog(`[Gemini] UYARI: Beklenmeyen audio format, ham aktarim atlandi. mimeType=${mimeType}`);
                            return;
                        }

                        // PCM16 hizalama
                        if (pcmBuffer.length % 2 !== 0) {
                            pcmBuffer = pcmBuffer.subarray(0, pcmBuffer.length - 1);
                        }

                        // Debug: bazen ses seviyesi kontrolu
                        if (pcmBuffer.length >= 2000) {
                            let sum = 0;
                            const samples = Math.min(1000, pcmBuffer.length / 2);
                            for (let i = 0; i < samples * 2; i += 2) {
                                sum += Math.abs(pcmBuffer.readInt16LE(i));
                            }
                            const avg = sum / samples;
                            if (avg < 50) {
                                uiLog(`[Gemini] UYARI: Cok dusuk ses seviyesi (avg=${avg.toFixed(0)}) mimeType=${mimeType}`);
                            }
                        }

                        if (esp32Ws.readyState === 1) { // OPEN
                            // Hemen gondermek yerine, Node.js RAM'inde siralayip ESP32'ye zamani gelince yollatiyoruz.
                            enqueueAudioForEsp32(pcmBuffer);
                        }
                    }
                    // Yazıyı yakala (Eğer model aynı zamanda text de dönüyorsa)
                    if (part.text && part.text.trim() !== "") {
                        uiTranscript(part.text);
                    }
                });
            }
            
            // Gemini bazen content gelmediğinde boş bir serverContent objesi atabilir
            // Debug: lifecycle + transcription (transcription bazi mesajlarda ust seviyede gelebilir)
            if (data.serverContent) {
                if (data.serverContent.turnComplete) uiLog("[Gemini] turnComplete");
                if (data.serverContent.generationComplete) uiLog("[Gemini] generationComplete");
                if (data.serverContent.interrupted) uiLog("[Gemini] interrupted");
            }
            if (data.inputTranscription && data.inputTranscription.text) {
                uiTranscript(`[SEN]: ${data.inputTranscription.text}`);
            }
            if (data.outputTranscription && data.outputTranscription.text) {
                uiTranscript(`[WECTOR]: ${data.outputTranscription.text}`);
            }

            if (data.serverContent && data.serverContent.interrupted) {
                uiLog('[Gemini] Ses kesildi (Interrupted).');
                // Sesi hemen kes, kuyrugu bosalt!
                esp32AudioQueue = []; 
            }
            
            // Hata mesajı varsa
            if (data.error) {
                uiLog(`[Gemini Hatası]: ${data.error.message}`);
                console.error(data.error);
            }
        } catch (e) {
            // Sessizce geçme: parse edilemeyen/hatalı mesajları UI'a bas
            try {
                const raw = typeof message === 'string' ? message : message.toString('utf8');
                uiLog(`[Gemini] UYARI: Mesaj işlenemedi: ${e?.message ?? e}`);
                if (raw && raw.length) uiLog(`[Gemini] UYARI: Ham (ilk 200): ${raw.slice(0, 200)}`);
            } catch (_) {
                uiLog(`[Gemini] UYARI: Mesaj işlenemedi: ${e?.message ?? e}`);
            }
        }
    });

    geminiWs.on('close', (code, reason) => {
        isOpenAIConnected = false;
        isGeminiSetupComplete = false;
        uiLog(`[Gemini] Bağlantı koptu. (Code: ${code}, Reason: ${reason.toString()})`);
        if (lastGeminiClientMessageLabel) {
            uiLog(`[Gemini] Son istek: ${lastGeminiClientMessageLabel} | ${lastGeminiClientMessagePreview}`);
        }
        broadcastStatus();
    });

    geminiWs.on('error', (err) => {
        uiLog(`[Gemini Bağlantı Hatası]: ${err.message}`);
    });

    // ESP32'den gelen PCM 16kHz verilerini Gemini'ye aktar
    let packageCount = 0;

    // Bar hesaplaması için gerekli periyodik değişkenler
    let barSampleSum = 0;
    let barSampleCount = 0;
    let lastBarPrint = Date.now();
    const BAR_INTERVAL_MS = 500;

    // Basit VAD (server-side): ESP32'dan gelen PCM'i turn'lere ayiriyoruz.
    // Setup'ta automaticActivityDetection.disabled=true oldugu icin activityStart/activityEnd gonderiyoruz.
    let vadSpeaking = false;
    let vadSilenceMs = 0;
    let vadNoiseFloor = 200; // avg abs
    let vadLastStateLog = 0;
    let vadStreamActive = false;
    
    esp32Ws.on('message', (data, isBinary) => {
        if (isBinary || Buffer.isBuffer(data)) {
            // [TEST] Gelen ses paketini RAM havuzuna alıyoruz:
            recordedMicChunks.push(Buffer.from(data));

            if (isOpenAIConnected && isGeminiSetupComplete && geminiWs.readyState === 1) {
                // Model konuşurken mic kapalı: kendi sesini interrupt etmesin
                if (playbackActive) return;

                if (data.length % 2 !== 0) {
                    // PCM16 olmasi icin 2 byte hizalamaya zorla
                    data = data.subarray(0, data.length - 1);
                }

                // Seviye olcumu (VAD + debug)
                let sum = 0;
                for (let i = 0; i < data.length; i += 2) {
                    sum += Math.abs(data.readInt16LE(i));
                }
                const avg = sum / (data.length / 2);

                // Chunk suresi (ms)
                const chunkMs = (data.length / 2) / 16000 * 1000;

                // Noise floor'u sadece "konusmuyor" durumunda takip et
                if (!vadSpeaking) {
                    vadNoiseFloor = (vadNoiseFloor * 0.98) + (avg * 0.02);
                }

                // Daha hassas VAD (Sesi kolay algilasin diye esikleri epey dusurduk)
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
                        uiLog(`[VAD] Konusma basladi (avg=${avg.toFixed(0)}, thr=${startThr.toFixed(0)})`);
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
                            }
                            uiLog(`[VAD] Konusma bitti (avg=${avg.toFixed(0)}, thr=${stopThr.toFixed(0)})`);
                        }
                    } else {
                        vadSilenceMs = 0;
                    }
                }

                // Ses çubuğunu (bar) periyodik 500ms biriktirerek göster
                barSampleSum += sum;
                barSampleCount += (data.length / 2);

                const now = Date.now();
                if (now - lastBarPrint >= BAR_INTERVAL_MS) {
                    const barAvg = barSampleCount > 0 ? barSampleSum / barSampleCount : 0;
                    const barLength = Math.min(Math.floor(barAvg / 20), 50);
                    const bar = '█'.repeat(barLength) || '░';
                    console.log(`🎤 [${new Date().toLocaleTimeString()}] Ses: ${Math.floor(barAvg).toString().padStart(4,'0')} | ${bar}`);
                    barSampleSum = 0;
                    barSampleCount = 0;
                    lastBarPrint = now;
                }

                // Gemini'ye sadece "konusma" aninda yolla (sessizligi kes)
                if (!vadSpeaking) {
                    const now = Date.now();
                    if (now - vadLastStateLog > 8000) {
                        vadLastStateLog = now;
                        uiLog(`[VAD] Sessiz (avg=${avg.toFixed(0)}, noise=${vadNoiseFloor.toFixed(0)})`);
                    }
                    return;
                }

                const audioBase64 = data.toString('base64');
                const realtimeInput = {
                    realtimeInput: {
                        audio: {
                            mimeType: "audio/pcm;rate=16000",
                            data: audioBase64
                        }
                    }
                };
                sendGemini(realtimeInput, "realtimeInput");
            }
        } else {
            const msg = data.toString();
            console.log('[ESP32 Text]:', msg);
        }
    });

    esp32Ws.on('close', () => {
        isEsp32Connected = false;
        uiLog('[ESP32] Bağlantı koptu. AI modundan çıkıldı.');
        
        saveWavToFile();

        broadcastStatus();
        if (geminiWs.readyState === WebSocket.OPEN) {
            geminiWs.close();
        }
    });
});
