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

// Dashboard
const int MAX_LOG = 10;                // how many past rides to keep

// ---------- STATE ----------
WebServer server(80);
int carriageIndex = 0;            // how many steps are done
bool stepSpinning = false;        // motor running for a step
bool rideSpinning = false;        // motor running for the final ride
bool rideSlowing = false;         // slowing down after the ride, looking for a carriage
bool leftSensor = false;          // true once the previous carriage has moved away from the sensor
unsigned long spinStartTime = 0;
unsigned long slowStartTime = 0;

// Dashboard state
unsigned long timeOffset = 0;     // phone epoch seconds minus ESP32 uptime seconds
bool timeKnown = false;           // true once a phone has synced the clock
unsigned long rideStartEpoch = 0; // start time of the current ride (epoch seconds)
unsigned long rideStartMillis = 0;
unsigned long logStartEpoch[MAX_LOG];
unsigned long logDurSec[MAX_LOG];
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

void resetRide() {
  carriageIndex = 0;
  for (int i = 0; i < totalCarriages; i++) digitalWrite(carriagePins[i], LOW);
  stepSpinning = false;
  rideSpinning = false;
  rideSlowing = false;
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

// Saves the finished ride to the dashboard log (newest first), then resets
void finishRide() {
  unsigned long dur = (millis() - rideStartMillis) / 1000;
  if (logCount < MAX_LOG) logCount++;
  for (int i = logCount - 1; i > 0; i--) {
    logStartEpoch[i] = logStartEpoch[i - 1];
    logDurSec[i] = logDurSec[i - 1];
  }
  logStartEpoch[0] = rideStartEpoch;
  logDurSec[0] = dur;
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

// Dashboard card shown at the bottom of the main page
String buildDashboard() {
  bool running = rideSpinning || rideSlowing;
  unsigned long longest = 0;
  for (int i = 0; i < logCount; i++) if (logDurSec[i] > longest) longest = logDurSec[i];

  String h = "<div class='card'><h3>Ride Dashboard</h3><div class='last'>";
  if (running) {
    h += "<b>Running now</b><br>Started <span data-e='" + String(rideStartEpoch) + "'></span>";
    h += "<br>Elapsed " + fmtDur((millis() - rideStartMillis) / 1000);
  } else if (logCount > 0) {
    h += "<b>Last ride</b><br>Started <span data-e='" + String(logStartEpoch[0]) + "'></span>";
    h += "<br>Duration " + fmtDur(logDurSec[0]);
  } else {
    h += "No rides yet";
  }
  h += "</div><div class='grid'>";
  h += "<div class='stat'><b>" + String(totalRides) + "</b><span>Total rides</span></div>";
  h += "<div class='stat'><b>" + (logCount > 0 ? fmtDur(logDurSec[0]) : String("-")) + "</b><span>Last ride</span></div>";
  h += "<div class='stat'><b>" + (logCount > 0 ? fmtDur(longest) : String("-")) + "</b><span>Longest</span></div>";
  h += "</div>";
  if (logCount > 0) {
    h += "<table><tr><th>#</th><th>Started</th><th>Duration</th></tr>";
    for (int i = 0; i < logCount; i++) {
      h += "<tr><td>" + String(totalRides - i) + "</td><td data-e='" + String(logStartEpoch[i]) + "'></td><td>" + fmtDur(logDurSec[i]) + "</td></tr>";
    }
    h += "</table>";
  }
  h += "</div>";
  h += "<script>document.querySelectorAll('[data-e]').forEach(function(x){var e=+x.getAttribute('data-e');x.innerText=e?new Date(e*1000).toLocaleString():'time not synced';});</script>";
  return h;
}

String buildPage() {
  bool busy = stepSpinning || rideSpinning || rideSlowing;
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
  html += ".msg{margin:10px 0 0;color:#4a5568}";
  html += ".dots{display:flex;justify-content:center;gap:10px;margin:14px 0}";
  html += ".dot{width:24px;height:24px;border-radius:50%;background:#e2e8f0;border:2px solid #cbd5e0}";
  html += ".dot.on{background:#f6ad55;border-color:#ed8936;box-shadow:0 0 10px #f6ad55}";
  html += "button{width:100%;font-size:22px;font-weight:600;padding:16px;border-radius:14px;border:none;color:#fff;box-shadow:0 4px 10px rgba(0,0,0,.15)}";
  html += "button:disabled{opacity:.55}";
  html += "input[type=range]{width:100%;margin-top:12px;accent-color:#6b46c1}";
  html += ".sens{display:inline-block;padding:8px 18px;border-radius:20px;background:#e2e8f0;color:#4a5568;font-weight:600;font-size:14px}";
  html += ".sens.on{background:#c6f6d5;color:#22543d}";
  html += ".last{background:#f7fafc;border-radius:12px;padding:10px;font-size:14px;line-height:1.6}";
  html += ".grid{display:flex;gap:8px;margin-top:10px}";
  html += ".stat{flex:1;background:#f7fafc;border-radius:12px;padding:10px}";
  html += ".stat b{display:block;font-size:18px}.stat span{font-size:12px;color:#718096}";
  html += "table{width:100%;border-collapse:collapse;margin-top:12px;font-size:13px}";
  html += "th,td{padding:8px 4px;border-bottom:1px solid #edf2f7;text-align:left}th{color:#718096}";
  html += "</style></head><body>";

  html += "<div class='hd'><h1>&#127905; Ferris Wheel</h1><p>Phone control panel</p></div>";
  html += "<div class='wrap'><div class='card'>";

  // status pill + message
  if (rideSpinning) {
    unsigned long elapsed = millis() - spinStartTime;
    unsigned long remaining = (elapsed >= rideSpinMs) ? 0 : (rideSpinMs - elapsed) / 1000 + 1;
    html += pill("run", "Ride in progress") + "<p class='msg'>" + String(remaining) + "s left at full speed</p>";
  } else if (rideSlowing) {
    html += pill("slow", "Slowing down") + "<p class='msg'>Landing on the next carriage...</p>";
  } else if (stepSpinning) {
    html += pill("run", "Moving") + "<p class='msg'>Step " + String(carriageIndex) + " of " + String(totalCarriages) + "</p>";
  } else if (carriageIndex < totalCarriages) {
    html += pill("idle", "Ready") + "<p class='msg'>Load carriage " + String(carriageIndex + 1) + " of " + String(totalCarriages) + "</p>";
  } else {
    html += pill("done", "All loaded") + "<p class='msg'>Ready for the ride</p>";
  }

  // carriage dots
  html += "<div class='dots'>";
  for (int i = 0; i < totalCarriages; i++) {
    if (i < carriageIndex) html += "<div class='dot on'></div>";
    else html += "<div class='dot'></div>";
  }
  html += "</div>";

  // main button
  if (rideSpinning) {
    html += formButton("/status", "Spinning...", "#718096", true);
  } else if (rideSlowing) {
    html += formButton("/status", "Slowing...", "#718096", true);
  } else if (stepSpinning) {
    html += formButton("/status", "Moving...", "#718096", true);
  } else if (carriageIndex < totalCarriages) {
    html += formButton("/next", "NEXT (" + String(carriageIndex + 1) + "/" + String(totalCarriages) + ")", "#2b6cb0", false);
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

  // sensor badge
  html += "<div class='card'><span id='s' class='sens'>Sensor: ...</span></div>";
  html += "<script>function u(){fetch('/sensor').then(r=>r.text()).then(t=>{var e=document.getElementById('s');if(t=='1'){e.className='sens on';e.innerText='Carriage detected';}else{e.className='sens';e.innerText='No carriage';}});}setInterval(u,300);u();fetch('/settime?t='+Math.floor(Date.now()/1000));</script>";

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

void handleNext() {
  // ignored while the motor is running
  if (!stepSpinning && !rideSpinning && !rideSlowing && carriageIndex < totalCarriages) {
    digitalWrite(carriagePins[carriageIndex], HIGH); // light up this carriage
    carriageIndex++;
    stepSpinning = true;
    leftSensor = false;
    spinStartTime = millis();
  }
  redirectHome();
}

void handleStart() {
  // only available after all steps are done
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

  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP started. Connect phone to WiFi: ");
  Serial.println(AP_SSID);
  Serial.print("Then open: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
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