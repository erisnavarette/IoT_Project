#include <WiFi.h>
#include <WebServer.h>

// ---------- CONFIG ----------
const char* AP_SSID = "FerrisWheel";
const char* AP_PASS = "12345678";      // min 8 chars required by ESP32 AP

const int totalCarriages = 4;
int carriagePins[totalCarriages] = {4, 5, 18, 19};

const int relayPin = 32;               // relay IN2 (K2) -> controls DC motor power
const int sensorPin = 34;              // IR sensor OUT pin
const int sensorLedPin = 27;           // indicator LED, lights when sensor detects something
const bool SENSOR_ACTIVE_LOW = true;   // IR obstacle modules output LOW when they detect something
const unsigned long stepTimeoutMs = 5000;  // safety: stop if no carriage is detected in this time
const unsigned long rideSpinMs = 10000;    // full-speed spin time after START NOW

// Motor speed (percent of full speed, 1-100). 100 = full speed, lower = slower
const int stepSpeedPercent = 20;       // speed during NEXT steps
int rideSpeedPercent = 50;             // speed during START NOW ride (changed from the web app)
const unsigned long pulsePeriodMs = 200; // one on/off cycle

// Landing (slow-down after the ride)
const unsigned long rampDownMs = 3000;       // time to slow from ride speed to landing speed
const int landingSpeedPercent = 20;          // creeping speed while looking for a carriage
const unsigned long landingTimeoutMs = 20000; // safety: stop if no carriage is found in this time

// Relay polarity: true = active-low (yours)
const bool RELAY_ACTIVE_LOW = true;

// Dashboard and registration
const int MAX_LOG = 10;                // how many past rides to keep
const int MAX_RIDERS = 12;             // how many names can be registered per ride

// ---------- STATE ----------
WebServer server(80);
int carriageIndex = 0;            // how many carriages have been boarded
bool stepSpinning = false;        // motor running for a step
bool rideSpinning = false;        // motor running for the final ride
bool rideSlowing = false;         // slowing down after the ride, looking for a carriage
bool leftSensor = false;          // true once the previous carriage has moved away from the sensor
unsigned long spinStartTime = 0;
unsigned long slowStartTime = 0;

// Registration state
String riders[MAX_RIDERS];
bool riderUsed[MAX_RIDERS];       // true once the rider is seated in a carriage
int riderCount = 0;
int carriageRider[totalCarriages]; // rider index in each carriage, -1 = empty

// Dashboard state
unsigned long timeOffset = 0;     // phone epoch seconds minus ESP32 uptime seconds
bool timeKnown = false;           // true once a phone has synced the clock
unsigned long rideStartEpoch = 0; // start time of the current ride (epoch seconds)
unsigned long rideStartMillis = 0;
unsigned long logStartEpoch[MAX_LOG];
unsigned long logDurSec[MAX_LOG];
String logRiders[MAX_LOG];
int logCount = 0;
int totalRides = 0;

// ---------- HELPERS ----------
void motorOn()  { digitalWrite(relayPin, RELAY_ACTIVE_LOW ? LOW : HIGH); }
void motorOff() { digitalWrite(relayPin, RELAY_ACTIVE_LOW ? HIGH : LOW); }

// Debounced sensor reading (must hold for 20 ms)
bool carriageAtSensor() {
  static bool stable = false;
  static bool last = false;
  static unsigned long changedAt = 0;
  bool raw = digitalRead(sensorPin);
  bool now = SENSOR_ACTIVE_LOW ? !raw : raw;
  if (now != last) { last = now; changedAt = millis(); }
  if (millis() - changedAt >= 20) stable = last;
  return stable;
}

// Current motor speed in percent
int currentSpeed() {
  if (rideSpinning) return rideSpeedPercent;
  if (rideSlowing) {
    int from = rideSpeedPercent;
    int to = min(rideSpeedPercent, landingSpeedPercent);
    unsigned long e = millis() - slowStartTime;
    if (e >= rampDownMs) return to;
    return from - (int)(((long)(from - to) * (long)e) / (long)rampDownMs);
  }
  return stepSpeedPercent;
}

// Pulses the relay to reduce average motor speed
void updateMotor() {
  if (!stepSpinning && !rideSpinning && !rideSlowing) {
    motorOff();
    return;
  }

  int speed = currentSpeed();
  if (speed >= 100) {
    motorOn();
    return;
  }

  unsigned long t = (millis() - spinStartTime) % pulsePeriodMs;
  if (t < (pulsePeriodMs * speed) / 100) motorOn();
  else motorOff();
}

// Clears all registered riders and carriage seats (called after every ride)
void clearRiders() {
  riderCount = 0;
  for (int i = 0; i < MAX_RIDERS; i++) { riders[i] = ""; riderUsed[i] = false; }
  for (int c = 0; c < totalCarriages; c++) carriageRider[c] = -1;
}

void resetRide() {
  carriageIndex = 0;
  for (int i = 0; i < totalCarriages; i++) digitalWrite(carriagePins[i], LOW);
  stepSpinning = false;
  rideSpinning = false;
  rideSlowing = false;
  clearRiders();
  motorOff();
}

// Current time in epoch seconds (0 if no phone has synced yet)
unsigned long nowEpoch() {
  return timeKnown ? timeOffset + millis() / 1000 : 0;
}

String fmtDur(unsigned long s) {
  if (s < 60) return String(s) + "s";
  return String(s / 60) + "m " + String(s % 60) + "s";
}

// Makes typed names safe to show on the page
String htmlEsc(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("'", "&#39;");
  s.replace("\"", "&quot;");
  return s;
}

// e.g. "C1 Ana, C3 Ben" or "no riders"
String ridersSummary() {
  String s = "";
  for (int c = 0; c < totalCarriages; c++) {
    if (carriageRider[c] >= 0) {
      if (s.length() > 0) s += ", ";
      s += "C" + String(c + 1) + " " + riders[carriageRider[c]];
    }
  }
  if (s.length() == 0) s = "no riders";
  return s;
}

// Saves the finished ride to the dashboard log (newest first), then resets
void finishRide() {
  unsigned long dur = (millis() - rideStartMillis) / 1000;
  String who = ridersSummary();
  if (logCount < MAX_LOG) logCount++;
  for (int i = logCount - 1; i > 0; i--) {
    logStartEpoch[i] = logStartEpoch[i - 1];
    logDurSec[i] = logDurSec[i - 1];
    logRiders[i] = logRiders[i - 1];
  }
  logStartEpoch[0] = rideStartEpoch;
  logDurSec[0] = dur;
  logRiders[0] = who;
  totalRides++;
  resetRide();
}

String formButton(String action, String label, String color, bool disabled) {
  String s = "<form action='" + action + "' method='GET' style='margin:0'>";
  s += "<button type='submit' style='background:" + color + "'";
  if (disabled) s += " disabled";
  s += ">" + label + "</button></form>";
  return s;
}

String pill(String cls, String text) {
  return "<span class='pill " + cls + "'>" + text + "</span>";
}

// Registration card: add names, see who is registered
String buildRegistration() {
  String s = "<div class='card'><h3>Register riders</h3>";
  s += "<form action='/addrider' method='GET'>";
  s += "<input type='text' name='n' maxlength='16' placeholder='Enter rider name' autocomplete='off'>";
  s += "<button type='submit' style='background:#6b46c1;font-size:18px;padding:12px'>ADD RIDER</button></form>";
  s += "<div class='chips'>";
  if (riderCount == 0) s += "<span class='msg'>No riders registered yet</span>";
  for (int i = 0; i < riderCount; i++) {
    if (riderUsed[i]) {
      int c = 0;
      for (int k = 0; k < totalCarriages; k++) if (carriageRider[k] == i) c = k + 1;
      s += "<span class='chip in'>" + htmlEsc(riders[i]) + " &middot; C" + String(c) + "</span>";
    } else {
      s += "<span class='chip'>" + htmlEsc(riders[i]) + "<a href='/delrider?i=" + String(i) + "'>&times;</a></span>";
    }
  }
  s += "</div></div>";
  return s;
}

// Boarding form: pick a rider (or empty) for the carriage at the sensor, then NEXT
String nextForm(bool atS) {
  String label = "NEXT (" + String(carriageIndex + 1) + "/" + String(totalCarriages) + ")";
  String s = "<div class='board'><b>Carriage " + String(carriageIndex + 1) + " boarding</b>";
  s += "<form action='/next' method='GET'><select name='r'>";
  for (int i = 0; i < riderCount; i++) {
    if (!riderUsed[i]) s += "<option value='" + String(i) + "'>" + htmlEsc(riders[i]) + "</option>";
  }
  s += "<option value='-1'>Empty (no one)</option></select>";
  s += "<button type='submit' id='nb' data-l='" + label + "' style='background:#2b6cb0'";
  if (!atS) s += " disabled";
  s += ">" + (atS ? label : String("Waiting for carriage...")) + "</button></form></div>";
  return s;
}

// Turns "C1 Ana, C3 Ben" into small name chips
String ridersChips(String s) {
  if (s == "no riders") return "<span class='dchip none'>No riders</span>";
  String out = "";
  int start = 0;
  while (start < (int)s.length()) {
    int comma = s.indexOf(", ", start);
    String part = (comma < 0) ? s.substring(start) : s.substring(start, comma);
    int sp = part.indexOf(' ');
    if (sp > 0) out += "<span class='dchip'><i>" + htmlEsc(part.substring(0, sp)) + "</i>" + htmlEsc(part.substring(sp + 1)) + "</span>";
    else out += "<span class='dchip'>" + htmlEsc(part) + "</span>";
    if (comma < 0) break;
    start = comma + 2;
  }
  return out;
}

String dtile(String v, String label) {
  return "<div class='dt'><b>" + v + "</b><span>" + label + "</span></div>";
}

// Dashboard card shown at the bottom of the main page
String buildDashboard() {
  bool running = rideSpinning || rideSlowing;
  unsigned long longest = 0, sum = 0;
  for (int i = 0; i < logCount; i++) {
    if (logDurSec[i] > longest) longest = logDurSec[i];
    sum += logDurSec[i];
  }
  unsigned long avg = logCount > 0 ? sum / logCount : 0;

  String h = "<style>";
  h += ".dh{border-radius:16px;padding:16px;color:#fff;background:linear-gradient(135deg,#6b46c1,#2b6cb0);margin-bottom:12px}";
  h += ".dh.run{background:linear-gradient(135deg,#2f855a,#38a169)}";
  h += ".dl{font-size:12px;letter-spacing:1.5px;opacity:.85;font-weight:700}";
  h += ".db{font-size:40px;font-weight:800;margin:4px 0}";
  h += ".dm{font-size:14px}";
  h += ".dr{margin-top:10px;display:flex;flex-wrap:wrap;gap:6px;justify-content:center}";
  h += ".dtiles{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px}";
  h += ".dt{background:#f7fafc;border-radius:14px;padding:12px}";
  h += ".dt b{display:block;font-size:22px}.dt span{font-size:12px;color:#718096}";
  h += ".dsec{text-align:left;font-weight:700;margin:6px 0 8px}";
  h += ".dride{display:flex;align-items:center;gap:10px;background:#f7fafc;border-radius:14px;padding:10px;margin-bottom:8px;text-align:left}";
  h += ".dn{background:#6b46c1;color:#fff;border-radius:10px;min-width:40px;text-align:center;padding:8px 4px;font-weight:700;font-size:13px}";
  h += ".dbody{flex:1;min-width:0}";
  h += ".dwhen{font-size:14px;margin-bottom:4px}";
  h += ".ddur{font-weight:800;font-size:16px;color:#2b6cb0;white-space:nowrap}";
  h += ".dchip{display:inline-block;background:#e9d8fd;color:#44337a;border-radius:12px;padding:3px 9px;font-size:12px;font-weight:600;margin:2px 2px 0 0}";
  h += ".dchip i{font-style:normal;opacity:.7;margin-right:4px}";
  h += ".dchip.none{background:#edf2f7;color:#718096}";
  h += ".dh .dchip{background:rgba(255,255,255,.22);color:#fff}";
  h += "</style>";

  h += "<div class='card'><h3>Ride Dashboard</h3>";

  // big summary box
  if (running) {
    h += "<div class='dh run'><div class='dl'>RUNNING NOW</div>";
    h += "<div class='db'>" + fmtDur((millis() - rideStartMillis) / 1000) + "</div>";
    h += "<div class='dm'><span data-d='" + String(rideStartEpoch) + "'></span> &middot; <span data-t='" + String(rideStartEpoch) + "'></span></div>";
    h += "<div class='dr'>" + ridersChips(ridersSummary()) + "</div></div>";
  } else if (logCount > 0) {
    h += "<div class='dh'><div class='dl'>LAST RIDE</div>";
    h += "<div class='db'>" + fmtDur(logDurSec[0]) + "</div>";
    h += "<div class='dm'><span data-d='" + String(logStartEpoch[0]) + "'></span> &middot; <span data-t='" + String(logStartEpoch[0]) + "'></span></div>";
    h += "<div class='dr'>" + ridersChips(logRiders[0]) + "</div></div>";
  } else {
    h += "<div class='dh'><div class='dl'>NO RIDES YET</div><div class='dm'>Your first ride will show up here</div></div>";
  }

  // stat tiles
  h += "<div class='dtiles'>";
  h += dtile(String(totalRides), "Total rides");
  h += dtile(logCount > 0 ? fmtDur(logDurSec[0]) : String("-"), "Last ride");
  h += dtile(logCount > 0 ? fmtDur(longest) : String("-"), "Longest");
  h += dtile(logCount > 0 ? fmtDur(avg) : String("-"), "Average");
  h += "</div>";

  // ride history
  if (logCount > 0) {
    h += "<div class='dsec'>Ride history</div>";
    for (int i = 0; i < logCount; i++) {
      String e = String(logStartEpoch[i]);
      h += "<div class='dride'><div class='dn'>#" + String(totalRides - i) + "</div><div class='dbody'>";
      h += "<div class='dwhen'><b data-d='" + e + "'></b> <span data-t='" + e + "'></span></div>";
      h += "<div>" + ridersChips(logRiders[i]) + "</div></div>";
      h += "<div class='ddur'>" + fmtDur(logDurSec[i]) + "</div></div>";
    }
  }
  h += "</div>";

  h += "<script>document.querySelectorAll('[data-d],[data-t]').forEach(function(x){var e=+(x.getAttribute('data-d')||x.getAttribute('data-t'));if(!e){x.innerText=x.hasAttribute('data-d')?'Time not synced':'';return;}var d=new Date(e*1000);x.innerText=x.hasAttribute('data-d')?d.toLocaleDateString([],{weekday:'short',month:'short',day:'numeric'}):d.toLocaleTimeString([],{hour:'numeric',minute:'2-digit',second:'2-digit'});});</script>";
  return h;
}

String buildPage() {
  bool busy = stepSpinning || rideSpinning || rideSlowing;
  bool loading = !busy && carriageIndex < totalCarriages;
  bool atS = carriageAtSensor();

  String html = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  if (busy) html += "<meta http-equiv='refresh' content='1'>"; // re-check until done
  html += "<style>";
  html += "*{box-sizing:border-box}";
  html += "body{margin:0;font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#eef2f7;color:#1a202c;text-align:center}";
  html += ".hd{background:linear-gradient(135deg,#2b6cb0,#6b46c1);color:#fff;padding:28px 16px 44px;border-radius:0 0 28px 28px}";
  html += ".hd h1{margin:0;font-size:26px}.hd p{margin:6px 0 0;opacity:.85;font-size:14px}";
  html += ".wrap{max-width:440px;margin:-24px auto 0;padding:0 14px 30px}";
  html += ".card{background:#fff;border-radius:18px;padding:18px;margin-bottom:14px;box-shadow:0 4px 14px rgba(0,0,0,.08)}";
  html += ".card h3{margin:0 0 10px}";
  html += ".pill{display:inline-block;padding:6px 14px;border-radius:20px;font-size:14px;font-weight:600}";
  html += ".idle{background:#bee3f8;color:#2a4365}.run{background:#c6f6d5;color:#22543d}.slow{background:#feebc8;color:#744210}.done{background:#e9d8fd;color:#44337a}";
  html += ".msg{margin:10px 0 0;color:#4a5568;font-size:14px}";
  html += "input[type=text],select{width:100%;padding:12px;font-size:16px;border-radius:12px;border:1px solid #cbd5e0;margin-bottom:10px;background:#fff;color:#1a202c}";
  html += ".chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:12px;justify-content:center}";
  html += ".chip{background:#edf2f7;border-radius:16px;padding:5px 10px;font-size:13px}";
  html += ".chip.in{background:#feebc8;color:#744210}";
  html += ".chip a{color:#e53e3e;text-decoration:none;font-weight:700;margin-left:6px}";
  html += ".seats{display:flex;flex-wrap:wrap;gap:8px;margin:14px 0}";
  html += ".seat{flex:1;min-width:70px;background:#f7fafc;border:2px dashed #cbd5e0;border-radius:12px;padding:8px 4px}";
  html += ".seat b{display:block;font-size:12px;color:#718096}";
  html += ".seat span{display:block;font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}";
  html += ".seat.on{background:#fffaf0;border:2px solid #ed8936;box-shadow:0 0 10px rgba(246,173,85,.6)}";
  html += ".seat.empty{background:#edf2f7;color:#a0aec0}";
  html += ".board{background:#f7fafc;border-radius:12px;padding:12px;margin-top:6px}";
  html += ".board b{display:block;margin-bottom:10px}";
  html += "button{width:100%;font-size:22px;font-weight:600;padding:16px;border-radius:14px;border:none;color:#fff;box-shadow:0 4px 10px rgba(0,0,0,.15)}";
  html += "button:disabled{opacity:.55}";
  html += "input[type=range]{width:100%;margin-top:12px;accent-color:#6b46c1}";
  html += ".sens{display:inline-block;padding:8px 18px;border-radius:20px;background:#e2e8f0;color:#4a5568;font-weight:600;font-size:14px}";
  html += ".sens.on{background:#c6f6d5;color:#22543d}";
  html += "</style></head><body>";

  html += "<div class='hd'><h1>&#127905; Ferris Wheel</h1><p>Phone control panel</p></div>";
  html += "<div class='wrap'>";

  // registration comes first, before boarding
  if (loading) html += buildRegistration();

  html += "<div class='card'>";

  // status pill + message
  if (rideSpinning) {
    unsigned long elapsed = millis() - spinStartTime;
    unsigned long remaining = (elapsed >= rideSpinMs) ? 0 : (rideSpinMs - elapsed) / 1000 + 1;
    html += pill("run", "Ride in progress") + "<p class='msg'>" + String(remaining) + "s left at full speed</p>";
  } else if (rideSlowing) {
    html += pill("slow", "Slowing down") + "<p class='msg'>Landing on the next carriage...</p>";
  } else if (stepSpinning) {
    html += pill("run", "Moving") + "<p class='msg'>Turning to the next carriage...</p>";
  } else if (carriageIndex < totalCarriages) {
    html += pill("idle", "Ready") + "<p class='msg'>Choose who sits in carriage " + String(carriageIndex + 1) + ", then tap NEXT</p>";
  } else {
    html += pill("done", "All loaded") + "<p class='msg'>Ready for the ride</p>";
  }

  // carriage seats
  html += "<div class='seats'>";
  for (int c = 0; c < totalCarriages; c++) {
    String cls = "seat";
    String label = "-";
    if (c < carriageIndex) {
      if (carriageRider[c] >= 0) { cls += " on"; label = htmlEsc(riders[carriageRider[c]]); }
      else { cls += " empty"; label = "Empty"; }
    }
    html += "<div class='" + cls + "'><b>" + String(c + 1) + "</b><span>" + label + "</span></div>";
  }
  html += "</div>";

  // main action
  if (rideSpinning) {
    html += formButton("/status", "Spinning...", "#718096", true);
  } else if (rideSlowing) {
    html += formButton("/status", "Slowing...", "#718096", true);
  } else if (stepSpinning) {
    html += formButton("/status", "Moving...", "#718096", true);
  } else if (carriageIndex < totalCarriages) {
    html += nextForm(atS);
  } else {
    html += formButton("/start", "START NOW", "#2f855a", false);
  }
  html += "</div>";

  // ride speed slider
  if (!busy) {
    html += "<div class='card'><b>Ride speed: <span id='sv'>" + String(rideSpeedPercent) + "</span>%</b>";
    html += "<input type='range' min='20' max='100' value='" + String(rideSpeedPercent) + "' ";
    html += "oninput=\"document.getElementById('sv').innerText=this.value\" ";
    html += "onchange=\"fetch('/setspeed?v='+this.value)\"></div>";
  }

  // sensor badge (also enables/disables the NEXT button)
  html += "<div class='card'><span id='s' class='sens'>Sensor: ...</span></div>";
  html += "<script>function u(){fetch('/sensor').then(r=>r.text()).then(t=>{var e=document.getElementById('s');if(t=='1'){e.className='sens on';e.innerText='Carriage detected';}else{e.className='sens';e.innerText='No carriage';}var b=document.getElementById('nb');if(b){b.disabled=(t!='1');b.innerText=(t=='1')?b.getAttribute('data-l'):'Waiting for carriage...';}});}setInterval(u,300);u();fetch('/settime?t='+Math.floor(Date.now()/1000));</script>";

  html += buildDashboard();
  html += "</div></body></html>";
  return html;
}

void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303);
}

// ---------- ROUTES ----------
void handleRoot() {
  server.send(200, "text/html", buildPage());
}

void handleAddRider() {
  bool idle = !stepSpinning && !rideSpinning && !rideSlowing && carriageIndex < totalCarriages;
  if (idle && server.hasArg("n")) {
    String n = server.arg("n");
    n.trim();
    if (n.length() > 16) n = n.substring(0, 16);
    if (n.length() > 0 && riderCount < MAX_RIDERS) {
      riders[riderCount] = n;
      riderUsed[riderCount] = false;
      riderCount++;
    }
  }
  redirectHome();
}

void handleDelRider() {
  bool idle = !stepSpinning && !rideSpinning && !rideSlowing && carriageIndex < totalCarriages;
  if (idle && server.hasArg("i")) {
    int i = server.arg("i").toInt();
    if (i >= 0 && i < riderCount && !riderUsed[i]) {
      for (int c = 0; c < totalCarriages; c++) if (carriageRider[c] > i) carriageRider[c]--;
      for (int j = i; j < riderCount - 1; j++) {
        riders[j] = riders[j + 1];
        riderUsed[j] = riderUsed[j + 1];
      }
      riderCount--;
      riders[riderCount] = "";
      riderUsed[riderCount] = false;
    }
  }
  redirectHome();
}

void handleNext() {
  // only when idle AND a carriage is at the sensor
  if (!stepSpinning && !rideSpinning && !rideSlowing && carriageIndex < totalCarriages && carriageAtSensor()) {
    int r = server.hasArg("r") ? server.arg("r").toInt() : -1;
    if (r >= 0 && r < riderCount && !riderUsed[r]) {
      carriageRider[carriageIndex] = r;          // seat the rider
      riderUsed[r] = true;
      digitalWrite(carriagePins[carriageIndex], HIGH); // LED on = occupied
    } else {
      carriageRider[carriageIndex] = -1;         // empty carriage, LED stays off
    }
    carriageIndex++;
    stepSpinning = true;
    leftSensor = false;
    spinStartTime = millis();
  }
  redirectHome();
}

void handleStart() {
  // only available after all carriages are done
  if (!stepSpinning && !rideSpinning && !rideSlowing && carriageIndex >= totalCarriages) {
    rideSpinning = true;
    rideStartMillis = millis();
    rideStartEpoch = nowEpoch();
    spinStartTime = millis();
  }
  redirectHome();
}

void handleStatus() {
  redirectHome();
}

void handleSensor() {
  server.send(200, "text/plain", carriageAtSensor() ? "1" : "0");
}

void handleSetSpeed() {
  if (server.hasArg("v")) {
    rideSpeedPercent = constrain(server.arg("v").toInt(), 20, 100);
  }
  server.send(200, "text/plain", "ok");
}

// The phone sends its clock here so the ESP32 knows the real date and time
void handleSetTime() {
  if (server.hasArg("t")) {
    unsigned long epoch = strtoul(server.arg("t").c_str(), NULL, 10);
    if (epoch > 1600000000UL) {
      timeOffset = epoch - millis() / 1000;
      timeKnown = true;
    }
  }
  server.send(200, "text/plain", "ok");
}

// ---------- SETUP / LOOP ----------
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < totalCarriages; i++) {
    pinMode(carriagePins[i], OUTPUT);
    digitalWrite(carriagePins[i], LOW);
  }
  pinMode(relayPin, OUTPUT);
  pinMode(sensorPin, INPUT);
  pinMode(sensorLedPin, OUTPUT);
  motorOff();
  clearRiders();

  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP started. Connect phone to WiFi: ");
  Serial.println(AP_SSID);
  Serial.print("Then open: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/addrider", handleAddRider);
  server.on("/delrider", handleDelRider);
  server.on("/next", handleNext);
  server.on("/start", handleStart);
  server.on("/status", handleStatus);
  server.on("/sensor", handleSensor);
  server.on("/setspeed", handleSetSpeed);
  server.on("/settime", handleSetTime);
  server.begin();
}

void loop() {
  server.handleClient();

  digitalWrite(sensorLedPin, carriageAtSensor() ? HIGH : LOW);   // LED on = sensor detects something

  // step: leave the current carriage, then stop when the next one reaches the sensor
  if (stepSpinning) {
    bool atSensor = carriageAtSensor();
    if (!leftSensor) {
      if (!atSensor) leftSensor = true;     // previous carriage has moved away
    } else if (atSensor) {
      stepSpinning = false;                 // next carriage arrived, stop
    }
    if (millis() - spinStartTime >= stepTimeoutMs) stepSpinning = false;  // safety timeout
  }

  // ride time is up: start slowing down instead of stopping
  if (rideSpinning && millis() - spinStartTime >= rideSpinMs) {
    rideSpinning = false;
    rideSlowing = true;
    leftSensor = false;
    slowStartTime = millis();
  }

  // landing: after the slow-down, stop at the next carriage the sensor sees
  if (rideSlowing) {
    unsigned long e = millis() - slowStartTime;
    if (e >= rampDownMs) {
      bool atSensor = carriageAtSensor();
      if (!leftSensor) {
        if (!atSensor) leftSensor = true;   // wait until any tab in front has passed
      } else if (atSensor) {
        finishRide();                       // carriage found, log the ride, stop and reset
      }
    }
    if (e >= landingTimeoutMs) finishRide(); // safety timeout
  }

  updateMotor();
}