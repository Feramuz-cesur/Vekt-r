import { app, BrowserWindow } from 'electron';
import path from 'path';
import { fileURLToPath } from 'url';

// Sunucuyu başlat (server.js'yi import ettiğimizde kendi kendine çalışacaktır)
import './server.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

let mainWindow;

function createWindow() {
    mainWindow = new BrowserWindow({
        width: 1200,
        height: 800,
        title: 'Wector AI Dashboard',
        autoHideMenuBar: true,
        webPreferences: {
            nodeIntegration: true
        }
    });

    // Sunucunun ayağa kalkması ve portları dinlemesi için kısa bir süre tanıyalım
    setTimeout(() => {
        mainWindow.loadURL('http://localhost:3000');
    }, 1000);

    mainWindow.on('closed', () => {
        mainWindow = null;
    });
}

app.whenReady().then(createWindow);

app.on('window-all-closed', () => {
    // macOS ortamında kullanıcı Command+Q diyene kadar uygulamalar genellikle açık kalır
    if (process.platform !== 'darwin') {
        app.quit();
    }
});

app.on('activate', () => {
    // macOS tıklandığında pencere yoksa yeni oluşturur
    if (mainWindow === null) {
        createWindow();
    }
});
