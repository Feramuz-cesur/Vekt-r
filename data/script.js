var gateway = `ws://${window.location.hostname}/ws`; // sunucunun ıp adresi alınır, gateway değişkenine atanır
var websocket;
window.addEventListener('load', onload);             // sayfa yüklendiğinde onload fonksiyonu çağrılır 
function onload(event) {
    initWebSocket();
    // getCurrentValue();    // bu fonksiyon bir bir geri beslenme olayı ekranı güncellemek için tek ekrandan kontrol 
}

function initWebSocket() {
    console.log('Trying to open a WebSocket connection…');
    websocket = new WebSocket(gateway);                 // sunucu ile bağlantı kurulur
    websocket.onopen = onOpen;                          // sunucu ile bağlantı kurulduğunda onOpen fonksiyonu çağrılır
    websocket.onclose = onClose;                        // sunucu ile bağlantı kapatıldığında onClose fonksiyonu çağrılır
    // websocket.onmessage = onMessage;                    // sunucudan mesaj geldiğinde onMessage fonksiyonu çağrılır
}

function onOpen(event) {
    console.log('Connection opened');
}

function onClose(event) {
    console.log('Connection closed');
    setTimeout(initWebSocket, 2000);                    // 2 saniye sonra tekrar bağlanmayı dener
}

function onMessage(event) {
    console.log(event.data);
}

var currentMood = "RANDOM";                    // varsayılan mood değeri  
var direction = "STOP";                      // varsayılan direction değeri    

// Mood butonuna tıklandığında mood değişkenini günceller
function updateMood(mood) {
    currentMood = mood;
    sendData()
}
function updateDirection(dir) {
    direction = dir;
    sendData()
}

function toggleCheckbox(checkbox) {
    const modeContainer = document.getElementById("mode-container");
    const mainContainer = document.getElementById("main-container");
    const switchLabel = document.getElementById("currentSwitch");

    if (checkbox.checked) {
        // Checkbox seçiliyse
        modeContainer.style.display = "flex"; // mode-container'ı göster
        switchLabel.textContent = "MANUAL MOOD"; // Switch içeriğini değiştir
        currentMood = "DEFAULT";
        sendData();
    } else {
        // Checkbox seçili değilse
        modeContainer.style.display = "none"; // mode-container'ı gizle
        switchLabel.textContent = "RANDOM MOOD"; // Switch içeriğini değiştir
        currentMood = "RANDOM";
        sendData();
    }
}

function sendData() {                    // slider değeri değiştiğinde sunucuya yeni değeri gönderir

    var data = {
        arm: document.getElementById("armSlider").value,
        head: document.getElementById("headSlider").value,
        speed: document.getElementById("speedSlider").value,
        mood: currentMood,
        direction: direction,
    };

    // JSON formatında WebSocket ile gönder
    websocket.send(JSON.stringify(data));
    console.log("Sent data:", data);

    if (currentMood == "CONFUSED") {
        {
            currentMood = "DEFAULT";
        }
    }
}



