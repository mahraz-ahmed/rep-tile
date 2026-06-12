#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// --- AP Network Credentials ---
const char* ap_ssid = "REP-TILE";
const char* ap_password = "notareptile";

// --- Hardware Pins ---
#define PN532_SCK  27
#define PN532_MISO 14
#define PN532_MOSI 32
#define BUTTON_PIN 4
#define BUZZER_PIN 19 

// --- LCD Configuration ---
// The PCF8574T I2C backpack typically uses address 0x27.
// Both LCDs can share the same I2C bus (SDA/SCL) and address (0x27).
// They receive identical I2C data and mirror each other automatically.
#define LCD_ADDRESS 0x27
#define LCD_COLUMNS 20
#define LCD_ROWS 4
LiquidCrystal_I2C display(LCD_ADDRESS, LCD_COLUMNS, LCD_ROWS);

// --- SS (Chip Select) Pins Array ---
// Pins in use: SPI (14,27,32), I2C (21,22), BUTTON (4), BUZZER (19)
// Avoid: 0 (BOOT button), 12 (must be LOW at boot), 34-39 (input-only)
// Pin order maps SS_PINS[N-1] -> physical reader N (matches dashboard labels).
// Reader 10 sits on GPIO 5, which has boot-time PWM and may be unreliable —
// shape masks should not depend on Reader 10.
const uint8_t SS_PINS[] = {18, 23, 25, 26, 33, 17, 16, 15, 13, 5};
const int NUM_READERS = sizeof(SS_PINS) / sizeof(SS_PINS[0]);

Adafruit_PN532* nfcReaders[NUM_READERS];
String lastTagUIDs[NUM_READERS][2];

// --- Read hysteresis: tolerate brief missed reads before blanking the slot ---
const uint8_t MAX_MISSED_READS = 4;
uint8_t missedReads[NUM_READERS][2] = {{0}};

// --- Role Cache (UID -> role, avoids repeated page reads) ---
struct RoleEntry { uint8_t uid[7]; uint8_t role; };
#define ROLE_CACHE_SIZE 20
RoleEntry roleCache[ROLE_CACHE_SIZE];
int roleCacheCount = 0;

uint8_t lookupRole(const uint8_t* uid) {
  for (int i = 0; i < roleCacheCount; i++)
    if (memcmp(roleCache[i].uid, uid, 7) == 0) return roleCache[i].role;
  return 0xFF;
}

void storeRole(const uint8_t* uid, uint8_t role) {
  for (int i = 0; i < roleCacheCount; i++) {
    if (memcmp(roleCache[i].uid, uid, 7) == 0) { roleCache[i].role = role; return; }
  }
  if (roleCacheCount < ROLE_CACHE_SIZE) {
    memcpy(roleCache[roleCacheCount].uid, uid, 7);
    roleCache[roleCacheCount].role = role;
    roleCacheCount++;
  }
}

void clearRoleCache() { roleCacheCount = 0; }

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

// --- Default Shape Database ---
// Bit (n-1) of readerMask = Physical Reader n. Bumping SHAPES_VERSION wipes
// flash on next boot and reloads these defaults.
const int SHAPES_VERSION = 3;
#define READER(n) ((uint16_t)1 << ((n) - 1))
struct DefaultShape { const char* name; uint16_t mask; uint8_t difficulty; };
const DefaultShape DEFAULT_SHAPES[] = {
  // {name,                mask,                                       difficulty}
  {"Green Mountain",       READER(5),                                  3},
  {"Yellow Half Arrow",    READER(8),                                  3},
  {"Orange Triangle",      READER(2)|READER(4)|READER(8)|READER(9),    1},
  {"Orange Hexagon",       READER(1),                                  2},
  {"Green Stairs",         READER(8),                                  2},
  {"Green Triangle",       READER(3)|READER(4)|READER(9),              1},
  {"Blue Sphinx",          READER(4)|READER(9),                        3},
  {"Blue Pentagon",        READER(1),                                  2},
  {"Red Trapezium",        READER(3)|READER(4),                        2},
  {"Red Triangle",         READER(2)|READER(4)|READER(8)|READER(9),    1},
  {"Yellow Kite",          READER(5),                                  3},
  {"White L Shape",        READER(7),                                  2},
  {"Pink Slanted Kite",    READER(6),                                  2},
  {"Black Trapezium",      READER(6),                                  3},
};
const int NUM_DEFAULT_SHAPES = sizeof(DEFAULT_SHAPES) / sizeof(DEFAULT_SHAPES[0]);

int activeRoundShapes[3];
bool useManualRounds = false;
int manualRoundShapes[3] = {-1, -1, -1};

enum GameState { PREP, GAME_SETUP, ROUND_INTRO, PLAYING, SCORING, GAME_OVER };
GameState currentState = PREP;

int currentRound = 1;
int blueScore = 0;
int redScore = 0;
unsigned long roundEndTime = 0;
unsigned long roundDuration = 60000; // milliseconds, configurable via web dashboard
int playersReady = 0; // Count of players who've signalled "done" in the current round (0..2)
unsigned long scoringStartTime = 0;   // millis() when the answers screen was first shown
unsigned long gameOverStartTime = 0;  // millis() when the GAME_OVER screen was first shown
const unsigned long AUTO_ADVANCE_MS = 10000; // 10s hold for every automatic state transition

// --- Speed scoring: first player to be fully correct gets 10, second gets 5 ---
// Answers are not finalised until round end (timer or 2 button presses).
// During the round we silently record the first time each player ever reaches a
// correct placement; at round end we look at the FINAL state and only award the
// speed bonus if that player is still correct then.
const int POINTS_FIRST = 10;
const int POINTS_SECOND = 5;
int blueFinishOrder = 0;     // 0 = no bonus, 1 = fastest (+10), 2 = second (+5). Set at round end.
int redFinishOrder = 0;
int bluePointsThisRound = 0; // net points earned this round (speed bonus minus wrong-penalties)
int redPointsThisRound = 0;
unsigned long blueFirstCorrectTime = 0; // millis() the first time Blue achieved correct placement; 0 if never
unsigned long redFirstCorrectTime  = 0;

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
  preferences.putInt("ver", SHAPES_VERSION);
  preferences.putInt("total", totalShapes);
  for (int i = 0; i < totalShapes; i++) {
    preferences.putString(("n" + String(i)).c_str(), shapeDatabase[i].name);
    preferences.putUShort(("m" + String(i)).c_str(), shapeDatabase[i].readerMask);
    preferences.putUChar(("d" + String(i)).c_str(), shapeDatabase[i].difficulty);
  }
}

void loadDefaultShapes() {
  totalShapes = NUM_DEFAULT_SHAPES;
  if (totalShapes > 30) totalShapes = 30;
  for (int i = 0; i < totalShapes; i++) {
    shapeDatabase[i].name = String(DEFAULT_SHAPES[i].name);
    shapeDatabase[i].readerMask = DEFAULT_SHAPES[i].mask;
    shapeDatabase[i].difficulty = DEFAULT_SHAPES[i].difficulty;
  }
  saveShapesToFlash();
  Serial.println("Loaded " + String(totalShapes) + " default shapes.");
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
    <p style="margin-bottom: 5px; margin-top: 15px;">Select valid answer readers for this shape:</p>
    <div class="checkbox-grid">
      <script>
        for(let i=1; i<=10; i++) {
          document.write('<div class="checkbox-item"><label>Reader '+i+' <input type="checkbox" id="sr'+i+'"></label></div>');
        }
      </script>
    </div>
    <button class="btn" style="background-color: #FFC107;" onclick="addShape()">Add Shape to Game</button>
    <p id="shapeStatus" style="color: #4CAF50; display: none; margin-top:10px;">Shape Added!</p>
    
    <hr>
    <h3 style="margin-bottom: 5px; color:#aaa;">Saved Shapes</h3>
    <table>
      <thead><tr><th>Shape</th><th>Diff.</th><th>Valid Readers</th><th></th></tr></thead>
      <tbody id="shapeListTable"></tbody>
    </table>
  </div>

  <!-- ===== Round Configuration Panel ===== -->
  <div class="panel">
    <h2 style="color: #E040FB;">Round Configuration</h2>
    <div style="display:flex; justify-content:center; align-items:center; gap:12px; margin-bottom:15px;">
      <label style="color:#aaa; font-weight:bold;">Round Timer:</label>
      <input type="number" id="timerInput" min="5" max="600" value="60" style="width:70px; padding:8px; border-radius:5px; border:none; font-size:1em; background:#2a2a2a; color:#fff; text-align:center;">
      <span style="color:#aaa;">seconds</span>
      <button class="btn" onclick="setTimer()">Set</button>
    </div>
    <p id="timerStatus" style="color: #4CAF50; display: none; margin-top:0; margin-bottom:10px;">Timer Updated!</p>
    <hr style="margin-bottom:15px;">
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
        <label style="color: #2196F3;"><input type="radio" name="playerRole" id="p1" value="1" checked> Blue</label>
        <label style="color: #f44336;"><input type="radio" name="playerRole" id="p2" value="2"> Red</label>
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
      for(let i=1; i<=10; i++) {
        if(document.getElementById('sr'+i).checked) mask |= (1 << (i - 1));
      }
      fetch('/addShape?name='+encodeURIComponent(name)+'&mask='+mask+'&diff='+diff).then(() => {
        const s = document.getElementById('shapeStatus');
        s.style.display = 'block'; setTimeout(() => s.style.display = 'none', 3000);
        document.getElementById('shapeName').value = '';
        document.getElementById('shapeDiff').value = '1';
        for(let i=1; i<=10; i++) document.getElementById('sr'+i).checked = false;
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
          let readers = [];
          for(let i=0; i<10; i++) { if(s.mask & (1 << i)) readers.push(i+1); }
          let readerStr = readers.length > 0 ? readers.join(', ') : 'None';
          let diffLabel = s.diff===1?'Easy':(s.diff===2?'Medium':'Hard');
          let diffColor = s.diff===1?'#4CAF50':(s.diff===2?'#FFC107':'#f44336');
          html += '<tr><td>'+s.name+'</td><td style="color:'+diffColor+'">'+diffLabel+'</td><td>'+readerStr+'</td><td><button class="btn-del" onclick="deleteShape('+index+')">X</button></td></tr>';
        });
        if(data.length===0) html='<tr><td colspan="4" style="text-align:center;color:#555;">No shapes added yet.</td></tr>';
        document.getElementById('shapeListTable').innerHTML = html;

        // Populate manual round dropdowns filtered by required difficulty
        [['r1Shape',1],['r2Shape',2],['r3Shape',3]].forEach(([id, diff]) => {
          let sel = document.getElementById(id);
          let prev = sel.value;
          sel.innerHTML = '<option value="-1">-- Select --</option>';
          data.forEach((s, i) => {
            if(s.diff === diff) sel.innerHTML += '<option value="'+i+'">'+s.name+'</option>';
          });
          if(prev && prev !== '-1') sel.value = prev;
        });
      });
    }

    // --- Timer ---
    function setTimer() {
      let secs = parseInt(document.getElementById('timerInput').value);
      if (isNaN(secs) || secs < 5 || secs > 600) return alert('Enter a time between 5 and 600 seconds.');
      fetch('/setTimer?secs=' + secs).then(() => {
        const s = document.getElementById('timerStatus');
        s.style.display = 'block'; setTimeout(() => s.style.display = 'none', 2000);
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
        if(data.timer !== undefined) document.getElementById('timerInput').value = data.timer;
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
        html += '<div class="score-box" style="background:#2196F322;color:#2196F3;border:1px solid #2196F3;">Blue: '+data.p1+'</div>';
        html += '<div class="score-box" style="background:#f4433622;color:#f44336;border:1px solid #f44336;">Red: '+data.p2+'</div>';
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

void allFieldsOff() {
  for (int i = 0; i < NUM_READERS; i++) setRFField(nfcReaders[i], false);
}

void allFieldsOn() {
  for (int i = 0; i < NUM_READERS; i++) setRFField(nfcReaders[i], true);
}

// === Sound-effect library ===
void sfxBootup() {
  // Game-show fanfare ascending into a sustain
  int notes[] = {392, 523, 659, 784, 1047}; // G4 C5 E5 G5 C6
  int durs[]  = {80, 80, 80, 80, 280};
  for (int i = 0; i < 5; i++) {
    tone(BUZZER_PIN, notes[i], durs[i]);
    delay(durs[i] + 25);
  }
  noTone(BUZZER_PIN);
}

void sfxTagDetected() {
  // Pleasant rising double-blip
  tone(BUZZER_PIN, 1568, 45); delay(55);
  tone(BUZZER_PIN, 2093, 80);
}

void sfxTagPlaced() {
  // Same as pre tag detection sound
  tone(BUZZER_PIN, 1568, 45); delay(55);
  tone(BUZZER_PIN, 2093, 80);
}

void sfxWriteSuccess() {
  // Triumphant 3-note arpeggio (C6-E6-G6)
  int notes[] = {1047, 1319, 1568};
  int durs[]  = {80, 80, 220};
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, notes[i], durs[i]);
    delay(durs[i] + 30);
  }
  noTone(BUZZER_PIN);
}

void sfxError() {
  // Descending wah-wah-wah
  tone(BUZZER_PIN, 311, 150); delay(160); // Eb4
  tone(BUZZER_PIN, 277, 150); delay(160); // Db4
  tone(BUZZER_PIN, 220, 400);             // A3
  delay(420);
  noTone(BUZZER_PIN);
}

void sfxShapesAssigned() {
  // Quick "ready, set..." fanfare (G5-B5-D6)
  int notes[] = {784, 988, 1175};
  int durs[]  = {100, 100, 280};
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, notes[i], durs[i]);
    delay(durs[i] + 25);
  }
  noTone(BUZZER_PIN);
}

void sfxRoundStart() {
  // 3-2-1-GO countdown
  for (int i = 3; i > 0; i--) {
    display.clear();
    display.setCursor(9, 1);
    display.print(i);
    tone(BUZZER_PIN, 700, 130);
    delay(1000);
  }
  display.clear();
  display.setCursor(8, 1);
  display.print("GO!");
  tone(BUZZER_PIN, 1568, 500); // GO!
  delay(520);
  noTone(BUZZER_PIN);
}

void sfxTimesUp() {
  // Rapid klaxon (alternating high/low)
  for (int i = 0; i < 8; i++) {
    tone(BUZZER_PIN, 1000, 80); delay(90);
    tone(BUZZER_PIN, 500, 80);  delay(90);
  }
  noTone(BUZZER_PIN);
}

void sfxReset() {
  // Descending whoosh
  for (int f = 1800; f >= 200; f -= 100) {
    tone(BUZZER_PIN, f, 25);
    delay(20);
  }
  noTone(BUZZER_PIN);
}

void sfxVictory() {
  // Extended celebratory fanfare with double-tap and big finish
  int notes[] = {
    523, 659, 784, 1047,        // C5 E5 G5 C6 ascending
    784, 1047, 784, 1047,       // double-tap
    1319, 1047, 784, 1047,      // bounce
    1319, 1568, 2093,           // climb
    1568, 2093                  // big ending
  };
  int durs[] = {
    100, 100, 100, 220,
    90,  90,  90,  90,
    100, 100, 100, 100,
    120, 120, 220,
    150, 600
  };
  for (int i = 0; i < 17; i++) {
    tone(BUZZER_PIN, notes[i], durs[i]);
    delay(durs[i] + 30);
  }
  noTone(BUZZER_PIN);
}

void sfxTie() {
  // Suspenseful alternating 2-tone sting
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 523, 180); delay(200); // C5
    tone(BUZZER_PIN, 466, 180); delay(200); // Bb4
  }
  tone(BUZZER_PIN, 392, 500); // G4 settle
  delay(520);
  noTone(BUZZER_PIN);
}

void updateOLED(String line1, String line2, String line3 = "", String line4 = "") {
  display.clear();
  display.setCursor(0, 0);
  display.print(line1);
  
  display.setCursor(0, 1);
  display.print(line2);
  
  if (line3 != "") { 
    display.setCursor(0, 2);
    display.print(line3); 
  }
  
  if (line4 != "") { 
    display.setCursor(0, 3);
    display.print(line4); 
  }
}

void wrapText(const String& prefix, const String& text, String& line2, String& line3) {
  String full = prefix + text;
  if ((int)full.length() <= LCD_COLUMNS) {
    line2 = full;
    line3 = "";
    return;
  }
  int breakAt = LCD_COLUMNS;
  for (int i = min((int)full.length() - 1, LCD_COLUMNS - 1); i >= (int)prefix.length(); i--) {
    if (full[i] == ' ') { breakAt = i; break; }
  }
  line2 = full.substring(0, breakAt);
  line3 = full.substring(breakAt + (full[breakAt] == ' ' ? 1 : 0));
  if ((int)line3.length() > LCD_COLUMNS) line3 = line3.substring(0, LCD_COLUMNS);
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

// --- Reset the game to initial PREP state ---
void resetGame() {
  currentRound = 1;
  blueScore = 0;
  redScore = 0;
  playersReady = 0;
  blueFinishOrder = 0;
  redFinishOrder = 0;
  bluePointsThisRound = 0;
  redPointsThisRound = 0;
  blueFirstCorrectTime = 0;
  redFirstCorrectTime = 0;
  isWriteMode = false;
  clearRoleCache();
  for (int i = 0; i < NUM_READERS; i++) {
    lastTagUIDs[i][0] = "Waiting...";
    lastTagUIDs[i][1] = "Waiting...";
  }
  currentState = PREP;
  sfxBootup();
  updateOLED("REP-TILE READY", "Press to start");
  allFieldsOn();
}

// --- Helper: has this player placed a tag on ANY of the valid answer readers? ---
// Shapes can have several valid readers; landing one is enough to "win". Extra correct
// placements are a bonus (no additional points). Wrong placements are scored separately.
bool playerHasCorrectPlacement(uint8_t role, uint16_t requiredMask) {
  const char* tag = (role == 1) ? "(BLUE)" : "(RED)";
  for (int i = 0; i < NUM_READERS; i++) {
    if (!(requiredMask & (1 << i))) continue;
    if (lastTagUIDs[i][0].indexOf(tag) != -1 ||
        lastTagUIDs[i][1].indexOf(tag) != -1) {
      return true;
    }
  }
  return false;
}

// --- Helper: how many whole seconds remain in an N-ms auto-advance window? ---
// At elapsed=0 returns total/1000 (10). At elapsed>=total returns 0. Counts down 10→1.
int countdownSeconds(unsigned long elapsed, unsigned long total) {
  if (elapsed >= total) return 0;
  return (int)(((total - 1 - elapsed) / 1000) + 1);
}

// --- Helper: format a signed integer ("+10", "-5", "0") for compact display ---
String signedStr(int n) {
  if (n > 0) return "+" + String(n);
  return String(n);
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
  // Drive all CS pins HIGH immediately so PN532s see a clean idle state
  // (GPIO 5 in particular outputs PWM during boot — this silences it ASAP)
  for (int i = 0; i < NUM_READERS; i++) {
    pinMode(SS_PINS[i], OUTPUT);
    digitalWrite(SS_PINS[i], HIGH);
  }

  Serial.begin(115200);
  delay(1000);

  // Load Preferences
  preferences.begin("game", false);
  int storedVersion = preferences.getInt("ver", 0);
  if (storedVersion != SHAPES_VERSION) {
    Serial.println("Shape DB version mismatch (stored=" + String(storedVersion) + ", expected=" + String(SHAPES_VERSION) + "). Loading defaults.");
    loadDefaultShapes();
  } else {
    totalShapes = preferences.getInt("total", 0);
    if (totalShapes > 30) totalShapes = 30; // Safety cap
    for (int i = 0; i < totalShapes; i++) {
      shapeDatabase[i].name = preferences.getString(("n" + String(i)).c_str(), "Unknown");
      shapeDatabase[i].readerMask = preferences.getUShort(("m" + String(i)).c_str(), 0);
      shapeDatabase[i].difficulty = preferences.getUChar(("d" + String(i)).c_str(), 1);
    }
    Serial.println("Loaded " + String(totalShapes) + " shapes from flash.");
  }

  randomSeed(esp_random());

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  display.init();
  display.backlight();
  updateOLED("BOOTING...", "Initialising Game");

  for (int i = 0; i < NUM_READERS; i++) {
    lastTagUIDs[i][0] = "Waiting..."; lastTagUIDs[i][1] = "Waiting...";
    nfcReaders[i] = new Adafruit_PN532(PN532_SCK, PN532_MISO, PN532_MOSI, SS_PINS[i]);
    initReader(nfcReaders[i], i + 1);
  }

  allFieldsOn();

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
      clearRoleCache(); // tags may be re-written with new roles
      if (server.hasArg("role")) currentPlayerWriteRole = server.arg("role").toInt();
      updateOLED("WRITE MODE", "Role: " + String(currentPlayerWriteRole == 1 ? "Blue" : "Red"), "Tap tag to reader");
    } else {
      isWriteMode = false;
      currentState = PREP; 
      updateOLED("REP-TILE READY", "Press to start");
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
      uint16_t mask = (uint16_t)server.arg("mask").toInt();
      if (mask == 0) mask = READER(1); // "None" answers default to Reader 1
      shapeDatabase[totalShapes].readerMask = mask;
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
      int r1 = server.arg("r1").toInt();
      int r2 = server.arg("r2").toInt();
      int r3 = server.arg("r3").toInt();
      if (r1 < 0 || r1 >= totalShapes || shapeDatabase[r1].difficulty != 1 ||
          r2 < 0 || r2 >= totalShapes || shapeDatabase[r2].difficulty != 2 ||
          r3 < 0 || r3 >= totalShapes || shapeDatabase[r3].difficulty != 3) {
        server.send(400, "text/plain", "R1=Easy, R2=Medium, R3=Hard");
        return;
      }
      manualRoundShapes[0] = r1;
      manualRoundShapes[1] = r2;
      manualRoundShapes[2] = r3;
      useManualRounds = true;
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing r1, r2, r3");
    }
  });

  server.on("/setTimer", HTTP_GET, []() {
    if (server.hasArg("secs")) {
      int secs = server.arg("secs").toInt();
      if (secs >= 5 && secs <= 600) roundDuration = (unsigned long)secs * 1000;
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/getGameState", HTTP_GET, []() {
    String json = "{";
    json += "\"state\":\"" + gameStateName() + "\"";
    json += ",\"round\":" + String(currentRound);
    json += ",\"p1\":" + String(blueScore);
    json += ",\"p2\":" + String(redScore);
    json += ",\"timer\":" + String(roundDuration / 1000);
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
  updateOLED("REP-TILE READY", "Press to start");
  sfxBootup();
}

void checkNFC(Adafruit_PN532* nfc, int readerNum) {
  int arrayIndex = readerNum - 1;

  // --- WRITE MODE: cycle field manually so only one reader is active ---
  if (isWriteMode) {
    setRFField(nfc, true);
    delay(5);
    uint8_t uid[7]; uint8_t uidLength;
    if (nfc->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50)) {
      if (uidLength == 7) {
        updateOLED("WRITING...", "Reader " + String(readerNum), "Keep steady");
        uint8_t pageData[4] = { currentPlayerWriteRole, 0, 0, 0 };
        if (nfc->mifareultralight_WritePage(6, pageData)) {
          storeRole(uid, currentPlayerWriteRole);
          updateOLED("SUCCESS!", "Tag is " + String(currentPlayerWriteRole == 1 ? "Blue" : "Red"), "Next tag...");
          sfxWriteSuccess();
          delay(700);
          updateOLED("WRITE MODE", "Role: " + String(currentPlayerWriteRole == 1 ? "Blue" : "Red"), "Tap tag to reader");
        } else {
          sfxError();
          updateOLED("WRITE MODE", "Role: " + String(currentPlayerWriteRole == 1 ? "Blue" : "Red"), "Tap tag to reader");
        }
      }
    }
    setRFField(nfc, false);
    return;
  }

  // --- READ MODE: field is kept on at all times during PLAYING ---
  uint8_t uid1[7]; uint8_t uid1Length = 0;
  uint8_t uid2[7]; uint8_t uid2Length = 0;
  bool success = nfc->readTwoPassiveTargetIDs(PN532_MIFARE_ISO14443A, uid1, &uid1Length, uid2, &uid2Length);

  uint8_t tag1Role = 0; uint8_t tag2Role = 0;

  if (success) {
    if (uid1Length == 7) {
      uint8_t cached = lookupRole(uid1);
      if (cached != 0xFF) {
        tag1Role = cached;
      } else {
        uint8_t data[16];
        if (nfc->mifareultralight_ReadPageTarget(1, 6, data)) {
          tag1Role = data[0];
          storeRole(uid1, tag1Role);
        }
      }
    }
    if (uid2Length == 7) {
      uint8_t cached = lookupRole(uid2);
      if (cached != 0xFF) {
        tag2Role = cached;
      } else {
        uint8_t data[16];
        if (nfc->mifareultralight_ReadPageTarget(2, 6, data)) {
          tag2Role = data[0];
          storeRole(uid2, tag2Role);
        }
      }
    }
    // Put tags back to IDLE so they're findable on the next scan without a field cycle
    nfc->inRelease(0);
  }

  // --- Slot 1 ---
  if (success && uid1Length > 0) {
    String newUID1 = "";
    for (uint8_t i = 0; i < uid1Length; i++) { if (uid1[i] < 0x10) newUID1 += "0"; newUID1 += String(uid1[i], HEX); }
    newUID1.toUpperCase();
    if (tag1Role == 1) newUID1 += " (BLUE)"; else if (tag1Role == 2) newUID1 += " (RED)"; else newUID1 += " (UNASSIGNED)";
    if (newUID1 != lastTagUIDs[arrayIndex][0]) {
      if (currentState == PLAYING) sfxTagPlaced();
      lastTagUIDs[arrayIndex][0] = newUID1;
    }
    missedReads[arrayIndex][0] = 0;
  } else {
    if (lastTagUIDs[arrayIndex][0] != "Waiting...") {
      if (++missedReads[arrayIndex][0] >= MAX_MISSED_READS) {
        lastTagUIDs[arrayIndex][0] = "Waiting...";
        missedReads[arrayIndex][0] = 0;
      }
    }
  }

  // --- Slot 2 ---
  if (success && uid2Length > 0) {
    String newUID2 = "";
    for (uint8_t i = 0; i < uid2Length; i++) { if (uid2[i] < 0x10) newUID2 += "0"; newUID2 += String(uid2[i], HEX); }
    newUID2.toUpperCase();
    if (tag2Role == 1) newUID2 += " (BLUE)"; else if (tag2Role == 2) newUID2 += " (RED)"; else newUID2 += " (UNASSIGNED)";
    if (newUID2 != lastTagUIDs[arrayIndex][1]) {
      if (currentState == PLAYING) sfxTagPlaced();
      lastTagUIDs[arrayIndex][1] = newUID2;
    }
    missedReads[arrayIndex][1] = 0;
  } else {
    if (lastTagUIDs[arrayIndex][1] != "Waiting...") {
      if (++missedReads[arrayIndex][1] >= MAX_MISSED_READS) {
        lastTagUIDs[arrayIndex][1] = "Waiting...";
        missedReads[arrayIndex][1] = 0;
      }
    }
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
        tone(BUZZER_PIN, 1000, 50); // Beep on button press
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
      {
        static int prepReaderIndex = 0;
        String prevUID0 = lastTagUIDs[prepReaderIndex][0];
        String prevUID1 = lastTagUIDs[prepReaderIndex][1];
        checkNFC(nfcReaders[prepReaderIndex], prepReaderIndex + 1);
        if ((prevUID0 == "Waiting..." && lastTagUIDs[prepReaderIndex][0] != "Waiting...") ||
            (prevUID1 == "Waiting..." && lastTagUIDs[prepReaderIndex][1] != "Waiting...")) {
          sfxTagDetected();
          updateOLED("REP-TILE READY", "Press to start", "Reader " + String(prepReaderIndex + 1), "detected a tag!");
        }
        if (++prepReaderIndex >= NUM_READERS) prepReaderIndex = 0;
      }
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
            sfxError();
            delay(1500);
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
            sfxError();
            delay(1500);
            updateOLED("READY", "Add shapes online,", "then press Start");
            break;
          }
          
          activeRoundShapes[0] = easyList[random(0, numEasy)];
          activeRoundShapes[1] = medList[random(0, numMed)];
          activeRoundShapes[2] = hardList[random(0, numHard)];
        }
        
        currentState = GAME_SETUP;
        sfxShapesAssigned();
        
        // Show all 3 shapes for organiser prep. Each shape's label is the letter
        // corresponding to its index in the shape database (A = first shape, B = second, ...).
        updateOLED(
          String((char)('A' + activeRoundShapes[0])) + ": " + shapeDatabase[activeRoundShapes[0]].name,
          String((char)('A' + activeRoundShapes[1])) + ": " + shapeDatabase[activeRoundShapes[1]].name,
          String((char)('A' + activeRoundShapes[2])) + ": " + shapeDatabase[activeRoundShapes[2]].name,
          "Press to begin"
        );
        delay(500); 
      }
      break;

    case GAME_SETUP:
      // Organiser has seen all 3 shapes. Button advances to Round 1 intro.
      if (btn) {
        currentState = ROUND_INTRO;
        String shapeName = shapeDatabase[activeRoundShapes[0]].name;
        String shapeLine2, shapeLine3;
        wrapText("Shape: ", shapeName, shapeLine2, shapeLine3);
        updateOLED("ROUND 1", shapeLine2, shapeLine3, "Press to start");
        delay(500);
      }
      break;

    case ROUND_INTRO:
      if (btn) {
        currentState = PLAYING;
        allFieldsOn();
        sfxRoundStart();
        roundEndTime = millis() + roundDuration;
        playersReady = 0;
        blueFinishOrder = 0;
        redFinishOrder = 0;
        bluePointsThisRound = 0;
        redPointsThisRound = 0;
        blueFirstCorrectTime = 0;
        redFirstCorrectTime = 0;
      }
      break;

    case PLAYING:
      {
        static unsigned long lastDisplayUpdate = 0;
        static int currentReaderIndex = 0;
        static unsigned long messageHoldUntil = 0;

        uint16_t requiredMask = shapeDatabase[activeRoundShapes[currentRound - 1]].readerMask;

        // Two button presses end the round (the only way besides the timer).
        // Going straight to SCORING — no transition overlay.
        if (btn && playersReady < 2) {
          playersReady++;
          if (playersReady >= 2) {
            roundEndTime = millis();
            messageHoldUntil = 0;
          } else {
            updateOLED("PLAYER READY", "(1 of 2)");
            messageHoldUntil = millis() + 1500;
          }
        }

        long timeLeft = (millis() < roundEndTime) ? ((roundEndTime - millis()) / 1000) : 0;

        // Suppress periodic redraw once the round is ending — prevents a stale
        // "TIME: 0s ROUND X..." flash before the SCORING screen appears.
        if (millis() < roundEndTime &&
            millis() >= messageHoldUntil &&
            millis() - lastDisplayUpdate >= 1000) {
          lastDisplayUpdate = millis();
          String currentShapeName = shapeDatabase[activeRoundShapes[currentRound - 1]].name;
          String roundLine = "ROUND " + String(currentRound) + ":";
          if (currentRound < 3) {
            char nextLetter = (char)('A' + activeRoundShapes[currentRound]);
            roundLine = "(Next: " + String(nextLetter) + ") " + roundLine;
          }
          updateOLED("TIME: " + String(timeLeft) + "s", roundLine, currentShapeName, "Blue: " + String(blueScore) + " Red: " + String(redScore));
        }

        checkNFC(nfcReaders[currentReaderIndex], currentReaderIndex + 1);
        currentReaderIndex++;
        if (currentReaderIndex >= NUM_READERS) currentReaderIndex = 0;

        // --- Silent speed tracking — record the first time each player ever reaches
        // a correct placement. No overlay, no sfx. finishOrder is NOT set here;
        // it's computed at round end based on each player's FINAL state.
        if (blueFirstCorrectTime == 0 && playerHasCorrectPlacement(1, requiredMask)) {
          blueFirstCorrectTime = millis();
        }
        if (redFirstCorrectTime == 0 && playerHasCorrectPlacement(2, requiredMask)) {
          redFirstCorrectTime = millis();
        }

        // Time's Up — finalise answers, compute points, transition straight to SCORING.
        if (millis() >= roundEndTime) {
          // NB: leave RF fields ON — SCORING needs to detect when tokens are removed.

          // Finalise: only players still correct at round end qualify for the speed bonus.
          // Among qualifiers, order is decided by earliest first-correct timestamp.
          bool blueFinalCorrect = playerHasCorrectPlacement(1, requiredMask);
          bool redFinalCorrect  = playerHasCorrectPlacement(2, requiredMask);
          blueFinishOrder = 0;
          redFinishOrder  = 0;
          if (blueFinalCorrect && redFinalCorrect) {
            if (blueFirstCorrectTime > 0 && redFirstCorrectTime > 0 &&
                redFirstCorrectTime < blueFirstCorrectTime) {
              redFinishOrder = 1; blueFinishOrder = 2;
            } else {
              blueFinishOrder = 1; redFinishOrder = 2;
            }
          } else if (blueFinalCorrect) {
            blueFinishOrder = 1;
          } else if (redFinalCorrect) {
            redFinishOrder = 1;
          }

          // Count wrong placements (player's tag on a non-required reader).
          int blueWrongCount = 0, redWrongCount = 0;
          for (int i = 0; i < NUM_READERS; i++) {
            if (requiredMask & (1 << i)) continue; // required reader, not a wrong slot
            bool blueHere = (lastTagUIDs[i][0].indexOf("(BLUE)") != -1 ||
                             lastTagUIDs[i][1].indexOf("(BLUE)") != -1);
            bool redHere  = (lastTagUIDs[i][0].indexOf("(RED)")  != -1 ||
                             lastTagUIDs[i][1].indexOf("(RED)")  != -1);
            if (blueHere) blueWrongCount++;
            if (redHere)  redWrongCount++;
          }

          int blueSpeedBonus = (blueFinishOrder == 1) ? POINTS_FIRST :
                               (blueFinishOrder == 2) ? POINTS_SECOND : 0;
          int redSpeedBonus  = (redFinishOrder  == 1) ? POINTS_FIRST :
                               (redFinishOrder  == 2) ? POINTS_SECOND : 0;

          bluePointsThisRound = blueSpeedBonus - (blueWrongCount * 5);
          redPointsThisRound  = redSpeedBonus  - (redWrongCount  * 5);

          blueScore += bluePointsThisRound;
          redScore  += redPointsThisRound;

          currentState = SCORING;
          sfxTimesUp();
          scoringStartTime = millis();
          lastDisplayUpdate = 0;
          currentReaderIndex = 0;
        }
      }
      break;

    case SCORING:
      {
        static unsigned long lastShownScoringStart = 0;
        static int lastShownScoringSec = -1;
        static bool lastShownTokensPresent = false;
        static unsigned long clearStartTime = 0;     // millis() when the board first went clear; 0 while tokens remain
        static int scoringReaderIndex = 0;

        const unsigned long CLEAR_COUNTDOWN_MS = 5000; // post-clear hold before auto-advance

        bool freshEntry = (scoringStartTime != lastShownScoringStart);
        if (freshEntry) {
          lastShownScoringStart = scoringStartTime;
          lastShownScoringSec = -1;
          clearStartTime = 0;
          scoringReaderIndex = 0;
        }

        // Keep scanning so we notice tokens being lifted off the readers.
        checkNFC(nfcReaders[scoringReaderIndex], scoringReaderIndex + 1);
        scoringReaderIndex++;
        if (scoringReaderIndex >= NUM_READERS) scoringReaderIndex = 0;

        bool tokensPresent = false;
        for (int i = 0; i < NUM_READERS; i++) {
          if (lastTagUIDs[i][0] != "Waiting..." || lastTagUIDs[i][1] != "Waiting...") {
            tokensPresent = true;
            break;
          }
        }

        if (tokensPresent) {
          clearStartTime = 0;          // reset countdown if anything reappears
        } else if (clearStartTime == 0) {
          clearStartTime = millis();   // first cleared moment — start the 5s timer
        }
        int secondsLeft = (clearStartTime > 0)
            ? countdownSeconds(millis() - clearStartTime, CLEAR_COUNTDOWN_MS)
            : 0;

        bool stateChanged     = (tokensPresent != lastShownTokensPresent);
        bool countdownChanged = (!tokensPresent && secondsLeft != lastShownScoringSec);
        if (freshEntry || stateChanged || countdownChanged) {
          lastShownTokensPresent = tokensPresent;
          lastShownScoringSec = secondsLeft;

          String header;
          if (blueFinishOrder == 0 && redFinishOrder == 0)      header = "BOTH INCORRECT!";
          else if (blueFinishOrder == 1)                        header = "BLUE WAS FASTEST!";
          else if (redFinishOrder == 1)                         header = "RED WAS FASTEST!";
          else                                                  header = "ROUND " + String(currentRound) + " OVER";

          String roundLine = "Blue " + signedStr(bluePointsThisRound) + " Red " + signedStr(redPointsThisRound);
          String totalLine = "Total B: " + String(blueScore) + " R: " + String(redScore);
          String footer = tokensPresent
              ? String("Remove tokens!")
              : ("Continue in " + String(secondsLeft) + "s...");

          updateOLED(header, roundLine, totalLine, footer);
        }

        // Advance only once the board is clear — button or countdown both work then.
        bool countdownDone = (clearStartTime > 0) && (millis() - clearStartTime >= CLEAR_COUNTDOWN_MS);
        if (!tokensPresent && (btn || countdownDone)) {
          lastShownScoringSec = -1;
          lastShownTokensPresent = false;
          clearStartTime = 0;
          for (int i = 0; i < NUM_READERS; i++) {
            lastTagUIDs[i][0] = "Waiting...";
            lastTagUIDs[i][1] = "Waiting...";
          }
          currentRound++;
          if (currentRound > 3) {
            allFieldsOff();
            currentState = GAME_OVER;
            gameOverStartTime = millis();
            if (blueScore == redScore) sfxTie();
            else sfxVictory();
          } else {
            currentState = ROUND_INTRO;
            String nextShape = shapeDatabase[activeRoundShapes[currentRound - 1]].name;
            String shapeLine2, shapeLine3;
            wrapText("Shape: ", nextShape, shapeLine2, shapeLine3);
            updateOLED("ROUND " + String(currentRound), shapeLine2, shapeLine3, "Press to start");
          }
        }
      }
      break;

    case GAME_OVER:
      {
        static unsigned long lastShownGameOverStart = 0;
        static int lastShownGameOverSec = -1;

        if (gameOverStartTime != lastShownGameOverStart) {
          lastShownGameOverStart = gameOverStartTime;
          lastShownGameOverSec = -1;
        }

        unsigned long elapsed = millis() - gameOverStartTime;
        int secondsLeft = countdownSeconds(elapsed, AUTO_ADVANCE_MS);

        if (secondsLeft != lastShownGameOverSec) {
          lastShownGameOverSec = secondsLeft;
          String winner;
          if (blueScore == redScore)       winner = "IT'S A TIE!";
          else if (blueScore > redScore)   winner = "BLUE WINS!";
          else                             winner = "RED WINS!";
          updateOLED("GAME OVER",
                     winner,
                     "Blue: " + String(blueScore) + "  Red: " + String(redScore),
                     "Restart in " + String(secondsLeft) + "s");
        }

        if (btn || elapsed >= AUTO_ADVANCE_MS) {
          lastShownGameOverSec = -1;
          resetGame();
        }
      }
      break;
  }
}