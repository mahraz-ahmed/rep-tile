#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// --- AP Network Credentials ---
const char* ap_ssid = "ESP32_NFC_Reader";
const char* ap_password = "password123";

// --- Hardware Pins ---
#define PN532_SCK  27
#define PN532_MISO 14
#define PN532_MOSI 32
#define BUTTON_PIN 4        
#define BUZZER_PIN 19       
#define RESET_MOTOR_PIN 23  

// --- OLED Configuration ---
// Both OLEDs share the same I2C bus (SDA/SCL) and address (0x3C).
// They receive identical I2C data and mirror each other automatically.
// No extra code is needed for the second display.
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- SS (Chip Select) Pins Array ---
// Currently 3 readers for testing. To expand to 9, add SS pin numbers here.
// Example: const uint8_t SS_PINS[] = {26, 25, 13, XX, XX, XX, XX, XX, XX};
const uint8_t SS_PINS[] = {26}; 
const int NUM_READERS = sizeof(SS_PINS) / sizeof(SS_PINS[0]);

// --- LED Pins Array (one white LED above each reader) ---
// Must match NUM_READERS in count. Expand alongside SS_PINS.
const uint8_t LED_PINS[] = {18, 16, 17};
const int NUM_LEDS = sizeof(LED_PINS) / sizeof(LED_PINS[0]);

Adafruit_PN532* nfcReaders[NUM_READERS];
String lastTagUIDs[NUM_READERS][2];

// --- Global States & Variables ---
bool isWriteMode = false;
uint8_t currentPlayerWriteRole = 1; 

// Shape Database Structure
struct Shape {
  String name;
  uint16_t readerMask; 
  uint8_t difficulty; // 1 = Easy, 2 = Medium, 3 = Hard
};
Shape shapeDatabase[30]; 
int totalShapes = 0;

int activeRoundShapes[3]; 
bool useManualRounds = false;
int manualRoundShapes[3] = {-1, -1, -1};

enum GameState { PREP, GAME_SETUP, ROUND_INTRO, PLAYING, SCORING, GAME_OVER };
GameState currentState = PREP;

int currentRound = 1;
int player1Score = 0;
int player2Score = 0;
unsigned long roundEndTime = 0;
const unsigned long ROUND_DURATION = 5000; // 5s for testing. Change to 90000 for production.

// --- Button Debounce & Long-Press State ---
bool lastButtonState = HIGH;       // Previous raw reading (INPUT_PULLUP: HIGH = released)
bool buttonState = HIGH;           // Debounced state
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 50; // 50ms debounce window
unsigned long buttonPressStart = 0;      // millis() when button was first pressed
bool longPressHandled = false;           // Prevents repeat-firing during a single long press
const unsigned long LONG_PRESS_MS = 3000; // 3 second hold to reset

WebServer server(80);
Preferences preferences;

// --- Flash Storage ---
void saveShapesToFlash() {
  preferences.clear();
  preferences.putInt("total", totalShapes);
  for (int i = 0; i < totalShapes; i++) {
    preferences.putString(("n" + String(i)).c_str(), shapeDatabase[i].name);
    preferences.putUShort(("m" + String(i)).c_str(), shapeDatabase[i].readerMask);
    preferences.putUChar(("d" + String(i)).c_str(), shapeDatabase[i].difficulty);
  }
}

// --- HTML Dashboard ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Rep-Tile Game Manager</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; background-color: #121212; color: #ffffff; margin: 0; padding: 20px; }
    h1 { color: #00ADB5; }
    .panel { background: #1e1e1e; border: 1px solid #00ADB5; border-radius: 12px; padding: 20px; max-width: 620px; margin: 0 auto 30px auto; box-shadow: 0 4px 10px rgba(0,255,255,0.1); }
    .toggle-container { display: flex; justify-content: center; align-items: center; gap: 15px; margin-bottom: 15px; font-size: 1.2em; font-weight: bold; }
    .switch { position: relative; display: inline-block; width: 60px; height: 34px; }
    .switch input { opacity: 0; width: 0; height: 0; }
    .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #4CAF50; transition: .4s; border-radius: 34px; }
    .slider:before { position: absolute; content: ""; height: 26px; width: 26px; left: 4px; bottom: 4px; background-color: white; transition: .4s; border-radius: 50%; }
    input:checked + .slider { background-color: #f44336; }
    input:checked + .slider:before { transform: translateX(26px); }
    .role-selection { display: flex; justify-content: center; gap: 30px; margin-top: 20px; }
    .role-selection label { font-size: 1.3em; font-weight: bold; cursor: pointer; }
    .btn { background-color: #00ADB5; color: #121212; border: none; padding: 10px 20px; margin-top: 15px; border-radius: 6px; font-weight: bold; cursor: pointer; font-size: 1em; transition: 0.2s; }
    .btn:hover { background-color: #008f96; }
    .btn-del { background-color: #f44336; color: white; border: none; padding: 5px 10px; border-radius: 4px; cursor: pointer; }
    .checkbox-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; margin-top: 10px; }
    .checkbox-item { background: #2a2a2a; padding: 10px; border-radius: 6px; }
    input[type=text] { width: 80%; padding: 10px; border-radius: 5px; border: none; font-size: 1em; }
    select { padding: 8px; border-radius: 5px; border: none; font-size: 1em; background: #2a2a2a; color: #fff; }
    .container { display: flex; justify-content: center; gap: 20px; flex-wrap: wrap; }
    .card { background: #1e1e1e; padding: 20px; border-radius: 12px; width: 280px; box-shadow: 0 8px 16px rgba(0,0,0,0.6); border: 1px solid #333; }
    h2 { color: #00ADB5; margin-top: 0; }
    hr { border-color: #333; margin: 20px 0; }
    .uid-box { font-size: 1.2em; font-weight: bold; margin: 10px 0; color: #EEEEEE; padding: 10px; background: #2a2a2a; border-radius: 8px; min-height: 25px; }
    table { width: 100%; margin-top: 15px; border-collapse: collapse; }
    th, td { padding: 10px; border-bottom: 1px solid #333; text-align: left; }
    th { color: #00ADB5; }
    .round-row { display: flex; justify-content: space-between; align-items: center; padding: 8px 0; }
    .round-row label { font-weight: bold; min-width: 70px; }
    .round-row select { flex: 1; margin-left: 10px; }
    .state-badge { display: inline-block; padding: 4px 12px; border-radius: 12px; font-weight: bold; font-size: 0.9em; }
    .score-display { display: flex; justify-content: center; gap: 40px; margin-top: 10px; }
    .score-box { padding: 10px 20px; border-radius: 8px; font-size: 1.3em; font-weight: bold; }
  </style>
</head>
<body>
  <h1>Rep-Tile Game Manager</h1>

  <!-- ===== Shape Database Panel ===== -->
  <div class="panel">
    <h2 style="color: #FFC107;">Shape Database</h2>
    <input type="text" id="shapeName" placeholder="Enter Shape Name (e.g., Red Triangle)">
    <div style="margin-top: 12px;">
      <label style="color:#aaa;">Difficulty: </label>
      <select id="shapeDiff">
        <option value="1">Easy</option>
        <option value="2">Medium</option>
        <option value="3">Hard</option>
      </select>
    </div>
    <p style="margin-bottom: 5px; margin-top: 15px;">Select valid answer slots for this shape:</p>
    <div class="checkbox-grid">
      <script>
        for(let i=2; i<=10; i++) {
          document.write('<div class="checkbox-item"><label>Slot '+i+' <input type="checkbox" id="sr'+i+'"></label></div>');
        }
      </script>
    </div>
    <button class="btn" style="background-color: #FFC107;" onclick="addShape()">Add Shape to Game</button>
    <p id="shapeStatus" style="color: #4CAF50; display: none; margin-top:10px;">Shape Added!</p>
    
    <hr>
    <h3 style="margin-bottom: 5px; color:#aaa;">Saved Shapes</h3>
    <table>
      <thead><tr><th>Shape</th><th>Diff.</th><th>Valid Slots</th><th></th></tr></thead>
      <tbody id="shapeListTable"></tbody>
    </table>
  </div>

  <!-- ===== Round Configuration Panel ===== -->
  <div class="panel">
    <h2 style="color: #E040FB;">Round Configuration</h2>
    <div class="toggle-container">
      <span id="roundModeLabel" style="color: #4CAF50;">RANDOM</span>
      <label class="switch">
        <input type="checkbox" id="roundModeSwitch" onchange="toggleRoundMode()">
        <span class="slider" style="background-color:#4CAF50;"></span>
      </label>
      <span style="color: #FFC107;">MANUAL</span>
    </div>
    <p id="roundModeDesc" style="color:#aaa; font-size:0.9em;">Auto-selects by difficulty: R1=Easy, R2=Medium, R3=Hard</p>
    <div id="manualRoundConfig" style="display:none;">
      <hr>
      <div style="text-align:left; max-width:400px; margin:0 auto;">
        <div class="round-row"><label style="color:#4CAF50;">Round 1:</label><select id="r1Shape"></select></div>
        <div class="round-row"><label style="color:#FFC107;">Round 2:</label><select id="r2Shape"></select></div>
        <div class="round-row"><label style="color:#f44336;">Round 3:</label><select id="r3Shape"></select></div>
      </div>
      <button class="btn" style="background-color:#E040FB;" onclick="setManualRounds()">Set Rounds</button>
      <p id="roundSetStatus" style="color: #4CAF50; display: none; margin-top:10px;">Rounds Set!</p>
    </div>
  </div>

  <!-- ===== Upcoming Game Panel ===== -->
  <div class="panel">
    <h2 style="color: #00E676;">Upcoming Game</h2>
    <div id="gameStateInfo">
      <p style="color:#555;">Press the hardware button to initialise a game...</p>
    </div>
  </div>

  <!-- ===== Read/Write Mode Panel ===== -->
  <div class="panel">
    <div class="toggle-container">
      <span id="modeLabel" style="color: #4CAF50;">READ MODE</span>
      <label class="switch">
        <input type="checkbox" id="modeSwitch" onchange="toggleModeUI()">
        <span class="slider"></span>
      </label>
      <span style="color: #f44336;">WRITE MODE</span>
    </div>
    <div id="writerConfig" style="display: none;">
      <hr>
      <p style="color:#aaa; font-size:1em;">Assign this tag as:</p>
      <div class="role-selection">
        <label style="color: #4CAF50;"><input type="radio" name="playerRole" id="p1" value="1" checked> Player 1</label>
        <label style="color: #2196F3;"><input type="radio" name="playerRole" id="p2" value="2"> Player 2</label>
      </div>
      <button class="btn" onclick="applySettings()">Apply Role &amp; Start Writing</button>
      <p id="saveStatus" style="color: #f44336; font-weight: bold; display: none; margin-top:15px;">Ready! Tap tag to any reader.</p>
    </div>
  </div>

  <!-- ===== Reader Cards ===== -->
  <div class="container" id="cards-container"></div>

  <script>
    const stateNames = {
      'PREP':'Waiting to Start','GAME_SETUP':'Shapes Assigned',
      'ROUND_INTRO':'Round Ready','PLAYING':'Round In Progress',
      'SCORING':'Round Complete','GAME_OVER':'Game Over'
    };
    const stateColors = {
      'PREP':'#555','GAME_SETUP':'#FFC107','ROUND_INTRO':'#E040FB',
      'PLAYING':'#00E676','SCORING':'#2196F3','GAME_OVER':'#f44336'
    };

    // --- Mode Toggle ---
    function toggleModeUI() {
      const isWrite = document.getElementById('modeSwitch').checked;
      document.getElementById('writerConfig').style.display = isWrite ? 'block' : 'none';
      document.getElementById('modeLabel').style.color = isWrite ? '#555' : '#4CAF50';
      if (!isWrite) fetch('/config?mode=read');
    }

    function applySettings() {
      let role = document.getElementById('p1').checked ? 1 : 2;
      fetch('/config?mode=write&role='+role);
      const status = document.getElementById('saveStatus');
      status.style.display = 'block'; setTimeout(() => status.style.display = 'none', 3000);
    }

    // --- Shape Database ---
    function addShape() {
      let name = document.getElementById('shapeName').value;
      if(!name) return alert('Enter a shape name!');
      let diff = document.getElementById('shapeDiff').value;
      let mask = 0;
      for(let i=2; i<=10; i++) {
        if(document.getElementById('sr'+i).checked) mask |= (1 << (i - 2));
      }
      fetch('/addShape?name='+encodeURIComponent(name)+'&mask='+mask+'&diff='+diff).then(() => {
        const s = document.getElementById('shapeStatus');
        s.style.display = 'block'; setTimeout(() => s.style.display = 'none', 3000);
        document.getElementById('shapeName').value = '';
        document.getElementById('shapeDiff').value = '1';
        for(let i=2; i<=10; i++) document.getElementById('sr'+i).checked = false;
        loadShapes();
      });
    }

    function deleteShape(index) {
      if(confirm("Delete this shape?")) {
        fetch('/deleteShape?id='+index).then(() => loadShapes());
      }
    }

    function loadShapes() {
      fetch('/getShapes').then(r => r.json()).then(data => {
        let html = '';
        data.forEach((s, index) => {
          let slots = [];
          for(let i=0; i<9; i++) { if(s.mask & (1 << i)) slots.push(i+2); }
          let slotStr = slots.length > 0 ? slots.join(', ') : 'None';
          let diffLabel = s.diff===1?'Easy':(s.diff===2?'Medium':'Hard');
          let diffColor = s.diff===1?'#4CAF50':(s.diff===2?'#FFC107':'#f44336');
          html += '<tr><td>'+s.name+'</td><td style="color:'+diffColor+'">'+diffLabel+'</td><td>'+slotStr+'</td><td><button class="btn-del" onclick="deleteShape('+index+')">X</button></td></tr>';
        });
        if(data.length===0) html='<tr><td colspan="4" style="text-align:center;color:#555;">No shapes added yet.</td></tr>';
        document.getElementById('shapeListTable').innerHTML = html;

        // Populate manual round dropdowns
        ['r1Shape','r2Shape','r3Shape'].forEach(id => {
          let sel = document.getElementById(id);
          let prev = sel.value;
          sel.innerHTML = '<option value="-1">-- Select --</option>';
          data.forEach((s, i) => {
            sel.innerHTML += '<option value="'+i+'">'+s.name+'</option>';
          });
          if(prev && prev !== '-1') sel.value = prev;
        });
      });
    }

    // --- Round Configuration ---
    function toggleRoundMode() {
      const isManual = document.getElementById('roundModeSwitch').checked;
      document.getElementById('manualRoundConfig').style.display = isManual ? 'block' : 'none';
      document.getElementById('roundModeLabel').style.color = isManual ? '#555' : '#4CAF50';
      document.getElementById('roundModeDesc').textContent = isManual
        ? 'Manually choose shapes for each round below.'
        : 'Auto-selects by difficulty: R1=Easy, R2=Medium, R3=Hard';
      fetch('/setRoundMode?type='+(isManual?'manual':'random'));
    }

    function setManualRounds() {
      let r1 = document.getElementById('r1Shape').value;
      let r2 = document.getElementById('r2Shape').value;
      let r3 = document.getElementById('r3Shape').value;
      if(r1==='-1'||r2==='-1'||r3==='-1') return alert('Select a shape for all 3 rounds!');
      fetch('/setRounds?r1='+r1+'&r2='+r2+'&r3='+r3).then(() => {
        const s = document.getElementById('roundSetStatus');
        s.style.display = 'block'; setTimeout(() => s.style.display = 'none', 3000);
      });
    }

    // --- Game State Polling ---
    function loadGameState() {
      fetch('/getGameState').then(r => r.json()).then(data => {
        let el = document.getElementById('gameStateInfo');
        if(data.state === 'PREP') {
          el.innerHTML = '<p style="color:#555;">Press the hardware button to initialise a game...</p>';
          return;
        }
        let html = '<div style="margin-bottom:12px;"><span class="state-badge" style="background:'+
          (stateColors[data.state]||'#555')+'20;color:'+(stateColors[data.state]||'#555')+
          ';border:1px solid '+(stateColors[data.state]||'#555')+';">'+
          (stateNames[data.state]||data.state)+'</span> &mdash; Round '+data.round+'/3</div>';
        html += '<table><thead><tr><th>Round</th><th>Shape</th><th>Difficulty</th></tr></thead><tbody>';
        if(data.shapes && data.shapes.length > 0) {
          data.shapes.forEach((s,i) => {
            let active = (data.round===i+1 && data.state!=='GAME_OVER' && data.state!=='PREP');
            let style = active ? 'color:#00ADB5;font-weight:bold;' : '';
            let dl = s.diff===1?'Easy':(s.diff===2?'Medium':'Hard');
            let dc = s.diff===1?'#4CAF50':(s.diff===2?'#FFC107':'#f44336');
            html += '<tr style="'+style+'"><td>Round '+(i+1)+'</td><td>'+s.name+'</td><td style="color:'+dc+'">'+dl+'</td></tr>';
          });
        }
        html += '</tbody></table>';
        html += '<div class="score-display">';
        html += '<div class="score-box" style="background:#4CAF5022;color:#4CAF50;border:1px solid #4CAF50;">P1: '+data.p1+'</div>';
        html += '<div class="score-box" style="background:#2196F322;color:#2196F3;border:1px solid #2196F3;">P2: '+data.p2+'</div>';
        html += '</div>';
        el.innerHTML = html;
      });
    }

    // --- Init & Polling ---
    window.onload = function() { loadShapes(); loadGameState(); };

    setInterval(function() {
      fetch('/data').then(r => r.json()).then(data => {
        const container = document.getElementById('cards-container');
        if (container.children.length === 0) {
          data.forEach((tags, index) => {
            const card = document.createElement('div');
            card.className = 'card';
            card.innerHTML = '<h2>Reader '+(index+1)+'</h2><p style="color:#aaa;font-size:0.9em;">Slot 1</p><div id="uid'+index+'_1" class="uid-box">'+tags[0]+'</div><p style="color:#aaa;font-size:0.9em;">Slot 2</p><div id="uid'+index+'_2" class="uid-box" style="background:#222;">'+tags[1]+'</div>';
            container.appendChild(card);
          });
        } else {
          data.forEach((tags, index) => {
            document.getElementById('uid'+index+'_1').innerText = tags[0];
            document.getElementById('uid'+index+'_2').innerText = tags[1];
          });
        }
      });
    }, 800);

    setInterval(loadGameState, 2000);
  </script>
</body>
</html>
)rawliteral";

// --- Helper Functions ---
void setRFField(Adafruit_PN532* nfc, bool enable) {
  uint8_t cmd[] = { 0x32, 0x01, (uint8_t)(enable ? 0x01 : 0x00) };
  nfc->sendCommandCheckAck(cmd, sizeof(cmd));
}

void playTone(int frequency, int duration) {
  tone(BUZZER_PIN, frequency, duration);
}

void playVictoryTune() {
  // Ascending major arpeggio: C4 E4 G4 C5 E5 G5 C6
  int notes[]    = {262, 330, 392, 523, 659, 784, 1047};
  int durations[] = {120, 120, 120, 120, 120, 120, 400};
  for (int i = 0; i < 7; i++) {
    tone(BUZZER_PIN, notes[i], durations[i]);
    delay(durations[i] + 40);
  }
  noTone(BUZZER_PIN);
}

void updateOLED(String line1, String line2, String line3 = "", String line4 = "") {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(2);
  display.println(line1);
  display.setTextSize(1);
  display.println(line2);
  if (line3 != "") { display.println(); display.println(line3); }
  if (line4 != "") { display.println(line4); }
  display.display();
}

void initReader(Adafruit_PN532* nfc, int readerNum) {
  nfc->begin();
  if (!nfc->getFirmwareVersion()) {
    Serial.print("Didn't find board for Reader "); Serial.println(readerNum);
  } else {
    nfc->SAMConfig(); 
    nfc->setPassiveActivationRetries(0x01); 
    setRFField(nfc, false);
  }
}

void allLEDsOff() {
  for (int i = 0; i < NUM_LEDS; i++) {
    digitalWrite(LED_PINS[i], LOW);
  }
}

// --- Reset the game to initial PREP state ---
void resetGame() {
  currentRound = 1;
  player1Score = 0;
  player2Score = 0;
  allLEDsOff();
  isWriteMode = false;
  for (int i = 0; i < NUM_READERS; i++) {
    lastTagUIDs[i][0] = "Waiting...";
    lastTagUIDs[i][1] = "Waiting...";
  }
  currentState = PREP;
  // Feedback: descending tone to indicate reset
  playTone(1000, 150); delay(160);
  playTone(600, 150);  delay(160);
  playTone(300, 300);
  delay(400);
  updateOLED("RESET", "Game cleared.", String(totalShapes) + " shapes loaded.", "Press Start");
}

// --- Helper: get game state name as string ---
String gameStateName() {
  switch(currentState) {
    case PREP: return "PREP";
    case GAME_SETUP: return "GAME_SETUP";
    case ROUND_INTRO: return "ROUND_INTRO";
    case PLAYING: return "PLAYING";
    case SCORING: return "SCORING";
    case GAME_OVER: return "GAME_OVER";
    default: return "UNKNOWN";
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Load Preferences
  preferences.begin("game", false);
  totalShapes = preferences.getInt("total", 0);
  if (totalShapes > 30) totalShapes = 30; // Safety cap
  for (int i = 0; i < totalShapes; i++) {
    shapeDatabase[i].name = preferences.getString(("n" + String(i)).c_str(), "Unknown");
    shapeDatabase[i].readerMask = preferences.getUShort(("m" + String(i)).c_str(), 0);
    shapeDatabase[i].difficulty = preferences.getUChar(("d" + String(i)).c_str(), 1);
  }
  Serial.println("Loaded " + String(totalShapes) + " shapes from flash.");

  randomSeed(esp_random());

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RESET_MOTOR_PIN, OUTPUT);
  digitalWrite(RESET_MOTOR_PIN, LOW);

  // Initialise LEDs
  for (int i = 0; i < NUM_LEDS; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
  }

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { Serial.println(F("SSD1306 alloc failed")); }
  display.setTextColor(SSD1306_WHITE);
  updateOLED("BOOTING...", "Initializing Game");

  for (int i = 0; i < NUM_READERS; i++) {
    lastTagUIDs[i][0] = "Waiting..."; lastTagUIDs[i][1] = "Waiting...";
    nfcReaders[i] = new Adafruit_PN532(PN532_SCK, PN532_MISO, PN532_MOSI, SS_PINS[i]);
    initReader(nfcReaders[i], i + 1);
  }

  WiFi.softAP(ap_ssid, ap_password); 
  
  // --- Web Endpoints ---
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", index_html); });
  
  server.on("/data", HTTP_GET, []() {
    String jsonResponse = "[";
    for (int i = 0; i < NUM_READERS; i++) {
      jsonResponse += "[\"" + lastTagUIDs[i][0] + "\", \"" + lastTagUIDs[i][1] + "\"]";
      if (i < NUM_READERS - 1) jsonResponse += ", ";
    }
    jsonResponse += "]";
    server.send(200, "application/json", jsonResponse);
  });
  
  server.on("/config", HTTP_GET, []() {
    String mode = server.arg("mode");
    if (mode == "write") {
      isWriteMode = true;
      if (server.hasArg("role")) currentPlayerWriteRole = server.arg("role").toInt();
      updateOLED("WRITE MODE", "Role: Player " + String(currentPlayerWriteRole), "Tap tag to reader");
    } else {
      isWriteMode = false;
      currentState = PREP; 
      updateOLED("READY", "Read Mode Active", "Press to Start");
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/getShapes", HTTP_GET, []() {
    String json = "[";
    for(int i = 0; i < totalShapes; i++) {
      json += "{\"name\":\"" + shapeDatabase[i].name + "\",\"mask\":" + String(shapeDatabase[i].readerMask) + ",\"diff\":" + String(shapeDatabase[i].difficulty) + "}";
      if(i < totalShapes - 1) json += ",";
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  server.on("/addShape", HTTP_GET, []() {
    if (totalShapes < 30) {
      shapeDatabase[totalShapes].name = server.arg("name");
      shapeDatabase[totalShapes].readerMask = server.arg("mask").toInt();
      int diff = server.arg("diff").toInt();
      shapeDatabase[totalShapes].difficulty = (diff >= 1 && diff <= 3) ? diff : 1;
      totalShapes++;
      saveShapesToFlash();
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Full");
    }
  });

  server.on("/deleteShape", HTTP_GET, []() {
    if (server.hasArg("id")) {
      int id = server.arg("id").toInt();
      if (id >= 0 && id < totalShapes) {
        // Shift array left to remove the item
        for(int i = id; i < totalShapes - 1; i++) {
          shapeDatabase[i] = shapeDatabase[i+1];
        }
        totalShapes--;
        saveShapesToFlash();
      }
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/setRoundMode", HTTP_GET, []() {
    if (server.arg("type") == "manual") {
      useManualRounds = true;
    } else {
      useManualRounds = false;
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/setRounds", HTTP_GET, []() {
    if (server.hasArg("r1") && server.hasArg("r2") && server.hasArg("r3")) {
      manualRoundShapes[0] = server.arg("r1").toInt();
      manualRoundShapes[1] = server.arg("r2").toInt();
      manualRoundShapes[2] = server.arg("r3").toInt();
      useManualRounds = true;
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing r1, r2, r3");
    }
  });

  server.on("/getGameState", HTTP_GET, []() {
    String json = "{";
    json += "\"state\":\"" + gameStateName() + "\"";
    json += ",\"round\":" + String(currentRound);
    json += ",\"p1\":" + String(player1Score);
    json += ",\"p2\":" + String(player2Score);
    json += ",\"roundMode\":\"" + String(useManualRounds ? "manual" : "random") + "\"";
    json += ",\"shapes\":[";
    if (currentState != PREP) {
      for (int i = 0; i < 3; i++) {
        int idx = activeRoundShapes[i];
        if (idx >= 0 && idx < totalShapes) {
          json += "{\"name\":\"" + shapeDatabase[idx].name + "\",\"diff\":" + String(shapeDatabase[idx].difficulty) + "}";
        } else {
          json += "{\"name\":\"?\",\"diff\":0}";
        }
        if (i < 2) json += ",";
      }
    }
    json += "]}";
    server.send(200, "application/json", json);
  });

  server.begin();
  updateOLED("READY", "ESP32 Running", String(totalShapes) + " shapes loaded.", "Press Start");
  playTone(1000, 200);
}

void checkNFC(Adafruit_PN532* nfc, int readerNum) {
  int arrayIndex = readerNum - 1;
  setRFField(nfc, true);
  delay(10); 

  // --- WRITE MODE ---
  if (isWriteMode) {
    uint8_t uid[7]; uint8_t uidLength;
    if (nfc->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50)) {
      if (uidLength == 7) { 
        updateOLED("WRITING...", "Reader " + String(readerNum), "Keep steady");
        uint8_t pageData[4] = { currentPlayerWriteRole, 0, 0, 0 };
        if (nfc->mifareultralight_WritePage(6, pageData)) {
          updateOLED("SUCCESS!", "Tag is Player " + String(currentPlayerWriteRole), "Next tag...");
          playTone(2000, 100); delay(100); playTone(2000, 100); 
          delay(1000); 
          updateOLED("WRITE MODE", "Role: Player " + String(currentPlayerWriteRole), "Tap tag to reader");
        } else {
          playTone(200, 500); 
          updateOLED("WRITE MODE", "Role: Player " + String(currentPlayerWriteRole), "Tap tag to reader");
        }
      }
    }
    setRFField(nfc, false);
    return;
  }

  // --- READ MODE ---
  uint8_t uid1[7]; uint8_t uid1Length = 0;
  uint8_t uid2[7]; uint8_t uid2Length = 0;
  bool success = nfc->readTwoPassiveTargetIDs(PN532_MIFARE_ISO14443A, uid1, &uid1Length, uid2, &uid2Length);
  
  uint8_t tag1Role = 0; uint8_t tag2Role = 0;

  if (success) {
    uint8_t data[16];
    if (uid1Length == 7 && nfc->mifareultralight_ReadPageTarget(1, 6, data)) tag1Role = data[0]; 
    if (uid2Length == 7 && nfc->mifareultralight_ReadPageTarget(2, 6, data)) tag2Role = data[0];
  }
  setRFField(nfc, false);

  String newUID1 = "Waiting..."; String newUID2 = "Waiting...";

  if (success) {
    if (uid1Length > 0) {
      newUID1 = "";
      for (uint8_t i = 0; i < uid1Length; i++) { if (uid1[i] < 0x10) newUID1 += "0"; newUID1 += String(uid1[i], HEX); }
      newUID1.toUpperCase();
      if (tag1Role == 1) newUID1 += " (P1)"; else if (tag1Role == 2) newUID1 += " (P2)"; else newUID1 += " (UNASSIGNED)";
    }
    if (uid2Length > 0) {
      newUID2 = "";
      for (uint8_t i = 0; i < uid2Length; i++) { if (uid2[i] < 0x10) newUID2 += "0"; newUID2 += String(uid2[i], HEX); }
      newUID2.toUpperCase();
      if (tag2Role == 1) newUID2 += " (P1)"; else if (tag2Role == 2) newUID2 += " (P2)"; else newUID2 += " (UNASSIGNED)";
    }
  }

  if (newUID1 != lastTagUIDs[arrayIndex][0] || newUID2 != lastTagUIDs[arrayIndex][1]) {
    if (currentState == PLAYING && (newUID1 != "Waiting..." || newUID2 != "Waiting...")) {
      playTone(1500, 100);
      // Flash LED above this reader for immediate feedback
      if (arrayIndex < NUM_LEDS) {
        digitalWrite(LED_PINS[arrayIndex], HIGH);
        delay(150);
        digitalWrite(LED_PINS[arrayIndex], LOW);
      }
    }
    lastTagUIDs[arrayIndex][0] = newUID1;
    lastTagUIDs[arrayIndex][1] = newUID2;
  }
}

// --- Main Game Loop ---
void loop() {
  server.handleClient();

  // --- Debounced Button Read with Long-Press Detection ---
  bool btn = false;        // True for ONE loop cycle on short-press release
  bool longPress = false;  // True for ONE loop cycle when 3s hold is detected

  bool reading = digitalRead(BUTTON_PIN);

  // Reset debounce timer whenever the raw reading changes
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  lastButtonState = reading;

  // Only accept the reading after it's been stable for DEBOUNCE_DELAY
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    // Detect edges on the debounced state
    if (reading != buttonState) {
      bool prevState = buttonState;
      buttonState = reading;

      if (buttonState == LOW && prevState == HIGH) {
        // --- Button just pressed (falling edge) ---
        buttonPressStart = millis();
        longPressHandled = false;
      }
      else if (buttonState == HIGH && prevState == LOW) {
        // --- Button just released (rising edge) ---
        if (!longPressHandled) {
          // Short press — only if we didn't already fire a long press
          btn = true;
        }
      }
    }

    // --- Long-press detection while held ---
    if (buttonState == LOW && !longPressHandled) {
      if ((millis() - buttonPressStart) >= LONG_PRESS_MS) {
        longPressHandled = true;
        longPress = true;
      }
    }
  }

  // --- Handle long press: reset game from any state ---
  if (longPress) {
    resetGame();
    return; // Skip the rest of this loop iteration
  }

  if (isWriteMode) {
    static int writeReaderIndex = 0;
    checkNFC(nfcReaders[writeReaderIndex], writeReaderIndex + 1);
    writeReaderIndex++;
    if (writeReaderIndex >= NUM_READERS) writeReaderIndex = 0;
    return; 
  }

  switch (currentState) {
    
    case PREP:
      if (btn) {
        if (useManualRounds) {
          // --- Manual Mode: use organiser's selections ---
          bool valid = true;
          for (int i = 0; i < 3; i++) {
            if (manualRoundShapes[i] < 0 || manualRoundShapes[i] >= totalShapes) {
              valid = false;
              break;
            }
            activeRoundShapes[i] = manualRoundShapes[i];
          }
          if (!valid) {
            updateOLED("ERROR", "Set all 3 rounds", "in the dashboard", "before starting!");
            playTone(200, 1000);
            delay(2000);
            updateOLED("READY", "Set rounds online,", "then press Start");
            break;
          }
        } else {
          // --- Random Mode: pick by difficulty ---
          int easyList[30], medList[30], hardList[30];
          int numEasy = 0, numMed = 0, numHard = 0;
          for (int i = 0; i < totalShapes; i++) {
            if (shapeDatabase[i].difficulty == 1) easyList[numEasy++] = i;
            else if (shapeDatabase[i].difficulty == 2) medList[numMed++] = i;
            else if (shapeDatabase[i].difficulty == 3) hardList[numHard++] = i;
          }
          
          if (numEasy < 1 || numMed < 1 || numHard < 1) {
            updateOLED("ERROR", "Need at least:", "1 Easy, 1 Medium,", "1 Hard shape!");
            playTone(200, 1000);
            delay(2000);
            updateOLED("READY", "Add shapes online,", "then press Start");
            break;
          }
          
          activeRoundShapes[0] = easyList[random(0, numEasy)];
          activeRoundShapes[1] = medList[random(0, numMed)];
          activeRoundShapes[2] = hardList[random(0, numHard)];
        }
        
        currentState = GAME_SETUP;
        playTone(1200, 300);
        
        // Show all 3 shapes for organiser prep
        display.clearDisplay();
        display.setCursor(0, 0);
        display.setTextSize(1);
        display.println("--- SHAPES DRAWN ---");
        display.println("R1: " + shapeDatabase[activeRoundShapes[0]].name);
        display.println("R2: " + shapeDatabase[activeRoundShapes[1]].name);
        display.println("R3: " + shapeDatabase[activeRoundShapes[2]].name);
        display.println("--------------------");
        display.println("Press to begin");
        display.display();
        delay(500); 
      }
      break;

    case GAME_SETUP:
      // Organiser has seen all 3 shapes. Button advances to Round 1 intro.
      if (btn) {
        currentState = ROUND_INTRO;
        String shapeName = shapeDatabase[activeRoundShapes[0]].name;
        display.clearDisplay();
        display.setCursor(0, 0);
        display.setTextSize(2);
        display.println("ROUND 1");
        display.setTextSize(1);
        display.println("Shape: " + shapeName);
        display.println("");
        display.println("Press to Start");
        display.display();
        delay(500);
      }
      break;

    case ROUND_INTRO:
      if (btn) {
        currentState = PLAYING;
        roundEndTime = millis() + ROUND_DURATION;
        playTone(800, 200); delay(200); playTone(1200, 400); 
      }
      break;

    case PLAYING:
      {
        static unsigned long lastDisplayUpdate = 0; 
        static int currentReaderIndex = 0; 
        
        long timeLeft = (roundEndTime - millis()) / 1000;
        if (timeLeft < 0) timeLeft = 0; 
        
        if (millis() - lastDisplayUpdate >= 1000) {
          lastDisplayUpdate = millis();
          String currentShapeName = shapeDatabase[activeRoundShapes[currentRound - 1]].name;
          updateOLED("TIME: " + String(timeLeft) + "s", "Round " + String(currentRound) + ":", currentShapeName, "P1:" + String(player1Score) + " P2:" + String(player2Score));
        }

        checkNFC(nfcReaders[currentReaderIndex], currentReaderIndex + 1);
        currentReaderIndex++;
        if (currentReaderIndex >= NUM_READERS) currentReaderIndex = 0; 

        // Time's Up - Scoring Phase
        if (millis() >= roundEndTime) {
          currentState = SCORING;
          playTone(500, 1000); 
          updateOLED("TIME'S UP!", "Calculating...");
          delay(2000);
          
          int requiredMask = shapeDatabase[activeRoundShapes[currentRound - 1]].readerMask;
          
          // Score: +10 for correct slots, -5 for wrong slots
          for (int i = 0; i < NUM_READERS; i++) {
            bool isRequired = (requiredMask & (1 << i)) != 0;
            bool p1Here = (lastTagUIDs[i][0].indexOf("(P1)") != -1 || lastTagUIDs[i][1].indexOf("(P1)") != -1);
            bool p2Here = (lastTagUIDs[i][0].indexOf("(P2)") != -1 || lastTagUIDs[i][1].indexOf("(P2)") != -1);

            if (isRequired) {
              if (p1Here) player1Score += 10;
              if (p2Here) player2Score += 10;
            } else {
              if (p1Here) player1Score -= 5;
              if (p2Here) player2Score -= 5;
            }
          }

          // Light up LEDs above correct readers to reveal answers
          for (int i = 0; i < NUM_LEDS; i++) {
            digitalWrite(LED_PINS[i], (requiredMask & (1 << i)) ? HIGH : LOW);
          }

          // Show scores
          updateOLED("SCORES", "P1:" + String(player1Score) + " P2:" + String(player2Score));
          delay(3000);

          // Auto-reset mechanism: drop tokens
          updateOLED("RESETTING", "Clearing tokens...");
          delay(500);
          digitalWrite(RESET_MOTOR_PIN, HIGH);
          delay(1500); 
          digitalWrite(RESET_MOTOR_PIN, LOW);

          // Clear tag data
          for (int i = 0; i < NUM_READERS; i++) {
            lastTagUIDs[i][0] = "Waiting...";
            lastTagUIDs[i][1] = "Waiting...";
          }

          // Show prompt (LEDs still on showing answers)
          updateOLED("SCORES", "P1:" + String(player1Score) + " P2:" + String(player2Score), "Press to Continue");
        }
      }
      break;

    case SCORING:
      if (btn) {
        // Turn off answer LEDs
        allLEDsOff();

        currentRound++;
        if (currentRound > 3) {
          currentState = GAME_OVER;
          playVictoryTune();
          String winner = (player1Score > player2Score) ? "PLAYER 1 WINS!" : ((player2Score > player1Score) ? "PLAYER 2 WINS!" : "TIE GAME!");
          updateOLED("GAME OVER", winner, "P1:" + String(player1Score) + " P2:" + String(player2Score), "Press to restart");
        } else {
          currentState = ROUND_INTRO;
          String nextShape = shapeDatabase[activeRoundShapes[currentRound - 1]].name;
          
          display.clearDisplay();
          display.setCursor(0, 0);
          display.setTextSize(2);
          display.println("ROUND " + String(currentRound));
          display.setTextSize(1);
          display.println("Shape: " + nextShape);
          display.println("");
          display.println("Press to Start");
          display.display();
        }
        delay(500);
      }
      break;

    case GAME_OVER:
      if (btn) {
        currentRound = 1;
        player1Score = 0;
        player2Score = 0;
        allLEDsOff();
        currentState = PREP;
        updateOLED("READY", "ESP32 Running", String(totalShapes) + " shapes loaded.", "Press Start");
        delay(500);
      }
      break;
  }
}