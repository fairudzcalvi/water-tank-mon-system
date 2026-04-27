#include <WiFi.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>

// ============================================================
//  PIN DEFINITIONS
// ============================================================
#define TRIG_PIN   5
#define ECHO_PIN   18

// SIM800L — use ESP32 UART2 (TX2=17, RX2=16)
#define SIM_RX     16   // ESP32 RX2  ← SIM800L TX
#define SIM_TX     17   // ESP32 TX2  → SIM800L RX
#define SIM_BAUD   115200

HardwareSerial sim800(2);   // UART2

// ============================================================
//  WiFi CREDENTIALS  (your home/router WiFi)
// ============================================================
const char* wifi_ssid     = "YOUR_WIFI_SSID";
const char* wifi_password = "YOUR_WIFI_PASSWORD";
const char* serverURL     = "https://your-app.onrender.com/api/water-level";

// ============================================================
//  CYLINDRICAL TANK DIMENSIONS  (edit to match your container)
// ============================================================
const float DIAMETER_CM = 25.0;
const float RADIUS_CM   = DIAMETER_CM / 2.0;
const float HEIGHT_CM   = 42.0;

// Max volume  (liters) = π × r² × h ÷ 1000
const float MAX_LITERS  = PI * RADIUS_CM * RADIUS_CM * HEIGHT_CM / 1000.0;

// ============================================================
//  ALERT THRESHOLDS  (percentage — overridden by server values)
// ============================================================
float LOW_THRESHOLD_PCT  = 20.0;
float HIGH_THRESHOLD_PCT = 90.0;

// Phone number for SMS alerts (overridden by server value)
String alertPhone = "+639638476287";

// Cooldown: don't re-send the same alert for this many seconds
const unsigned long ALERT_COOLDOWN_MS = 30000UL;  // 5 minutes
unsigned long lastLowAlertMs  = 0;
unsigned long lastHighAlertMs = 0;

// ============================================================
//  CALIBRATION STATE
// ============================================================
float empty_distance_cm = 0;
float full_distance_cm  = 0;
int   calibrationStep   = 0;

// ============================================================
//  SIM800L HELPERS
// ============================================================

// Send a raw AT command and wait up to timeoutMs for a response.
String sim800SendAT(const char* cmd, unsigned long timeoutMs = 3000) {
    while (sim800.available()) sim800.read();   // flush
    sim800.println(cmd);
    String resp = "";
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (sim800.available()) {
            char c = sim800.read();
            resp += c;
        }
        if (resp.indexOf("OK") != -1  ||
            resp.indexOf("ERROR") != -1 ||
            resp.indexOf(">") != -1) break;
    }
    resp.trim();
    Serial.println("[SIM] << " + resp);
    return resp;
}

bool sim800Init() {
    Serial.println("[SIM] Initialising SIM800L...");
    sim800.begin(SIM_BAUD, SERIAL_8N1, SIM_RX, SIM_TX);
    delay(3000);   // let module boot

    // Probe up to 5 times
    for (int i = 0; i < 5; i++) {
        String r = sim800SendAT("AT");
        if (r.indexOf("OK") != -1) {
            Serial.println("[SIM] Module ready");
            sim800SendAT("ATE0");           // echo off
            sim800SendAT("AT+CMGF=1");      // SMS text mode
            sim800SendAT("AT+CSCS=\"GSM\""); // GSM charset
            return true;
        }
        delay(1000);
    }
    Serial.println("[SIM] Module not responding!");
    return false;
}

bool sendSMS(const String& number, const String& message) {
    Serial.println("[SIM] Sending SMS to " + number);

    // Set recipient
    String cmd = "AT+CMGS=\"" + number + "\"";
    String resp = sim800SendAT(cmd.c_str(), 5000);
    if (resp.indexOf(">") == -1) {
        Serial.println("[SIM] No prompt received");
        return false;
    }

    // Send message body followed by Ctrl-Z (0x1A)
    sim800.print(message);
    sim800.write(0x1A);
    delay(200);

    // Wait for +CMGS confirmation (up to 10 s)
    String result = "";
    unsigned long start = millis();
    while (millis() - start < 10000) {
        while (sim800.available()) result += (char)sim800.read();
        if (result.indexOf("+CMGS") != -1 || result.indexOf("ERROR") != -1) break;
    }
    result.trim();
    Serial.println("[SIM] << " + result);

    bool ok = result.indexOf("+CMGS") != -1;
    Serial.println(ok ? "[SIM] SMS sent OK" : "[SIM] SMS FAILED");
    return ok;
}

// Read signal quality — returns RSSI integer or -1 on error
int sim800SignalQuality() {
    String r = sim800SendAT("AT+CSQ");
    // Response: +CSQ: 18,0
    int idx = r.indexOf("+CSQ: ");
    if (idx == -1) return -1;
    return r.substring(idx + 6).toInt();
}

// ============================================================
//  ULTRASONIC HELPERS
// ============================================================
float getDistance() {
    float total = 0;
    int   valid = 0;
    for (int i = 0; i < 10; i++) {
        digitalWrite(TRIG_PIN, LOW);
        delayMicroseconds(2);
        digitalWrite(TRIG_PIN, HIGH);
        delayMicroseconds(10);
        digitalWrite(TRIG_PIN, LOW);
        long dur = pulseIn(ECHO_PIN, HIGH, 30000);
        if (dur > 0) {
            total += (dur * 0.0343f) / 2.0f;
            valid++;
        }
        delay(50);
    }
    return (valid == 0) ? -1.0f : total / valid;
}

float calcLiters(float water_height_cm) {
    return PI * RADIUS_CM * RADIUS_CM * water_height_cm / 1000.0f;
}

// ============================================================
//  FETCH THRESHOLDS FROM SERVER
// ============================================================
void fetchThresholds() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin("https://your-app.onrender.com/api/thresholds");
    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        // Minimal JSON parse — no library needed for flat JSON
        auto extractFloat = [&](const String& key) -> float {
            int idx = body.indexOf("\"" + key + "\":");
            if (idx == -1) return -1;
            idx += key.length() + 3;
            return body.substring(idx).toFloat();
        };
        auto extractStr = [&](const String& key) -> String {
            int idx = body.indexOf("\"" + key + "\":\"");
            if (idx == -1) return "";
            idx += key.length() + 4;
            int end = body.indexOf("\"", idx);
            return body.substring(idx, end);
        };

        float lp = extractFloat("low_pct");
        float hp = extractFloat("high_pct");
        String ph = extractStr("phone");
        if (lp  > 0)  LOW_THRESHOLD_PCT  = lp;
        if (hp  > 0)  HIGH_THRESHOLD_PCT = hp;
        if (ph.length() > 0) alertPhone  = ph;

        Serial.printf("[Thresholds] low=%.1f%% high=%.1f%% phone=%s\n",
                      LOW_THRESHOLD_PCT, HIGH_THRESHOLD_PCT, alertPhone.c_str());
    }
    http.end();
}

// ============================================================
//  SEND DATA TO SERVER
// ============================================================
void sendToServer(float liters, float pct, float dist_cm) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Not connected, skipping send");
        return;
    }
    HTTPClient http;
    http.begin(serverURL);
    http.addHeader("Content-Type", "application/json");

    String payload = "{\"liters\":"      + String(liters, 2) +
                     ",\"percentage\":"  + String(pct, 1)    +
                     ",\"distance_cm\":" + String(dist_cm, 1) +
                     ",\"max_liters\":"  + String(MAX_LITERS, 2) + "}";

    int code = http.POST(payload);
    Serial.println(code == 200 ? "[HTTP] Sent OK" : "[HTTP] Failed, code: " + String(code));
    http.end();
}

// ============================================================
//  ALERT LOGIC WITH SMS COOLDOWN
// ============================================================
void checkAndAlert(float pct, float liters) {
    unsigned long now = millis();

    if (pct <= LOW_THRESHOLD_PCT) {
        if (now - lastLowAlertMs >= ALERT_COOLDOWN_MS) {
            String msg = "ALERT: Water tank LOW!\n"
                         "Level: " + String(pct, 1) + "%\n"
                         "Volume: " + String(liters, 2) + " / " + String(MAX_LITERS, 2) + " L\n"
                         "Please refill the tank.";
            if (sendSMS(alertPhone, msg)) lastLowAlertMs = now;
        } else {
            Serial.println("[Alert] LOW — cooldown active, SMS skipped");
        }
    } else if (pct >= HIGH_THRESHOLD_PCT) {
        if (now - lastHighAlertMs >= ALERT_COOLDOWN_MS) {
            String msg = "ALERT: Water tank FULL!\n"
                         "Level: " + String(pct, 1) + "%\n"
                         "Volume: " + String(liters, 2) + " / " + String(MAX_LITERS, 2) + " L\n"
                         "Please stop filling.";
            if (sendSMS(alertPhone, msg)) lastHighAlertMs = now;
        } else {
            Serial.println("[Alert] FULL — cooldown active, SMS skipped");
        }
    }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    // Start SIM800L
    sim800Init();

    // Connect to WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid, wifi_password);
    Serial.print("\n[WiFi] Connecting to " + String(wifi_ssid));
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());

    Serial.printf("[Tank] Diameter: %.1f cm | Height: %.1f cm | Max: %.2f L\n",
                  DIAMETER_CM, HEIGHT_CM, MAX_LITERS);
    Serial.println("------------------------------");
    Serial.println("STEP 1: Make sure container is EMPTY");
    Serial.println("Measuring in 5 seconds...");
    delay(5000);
}

// ============================================================
//  LOOP
// ============================================================
static unsigned long lastThresholdFetch = 0;
const  unsigned long THRESHOLD_FETCH_MS = 60000UL;  // refresh every 60 s

void loop() {
    // --- STEP 1: Calibrate empty ---
    if (calibrationStep == 0) {
        empty_distance_cm = getDistance();
        if (empty_distance_cm < 0) {
            Serial.println("[ERROR] No reading! Check sensor.");
            delay(2000);
            return;
        }
        Serial.printf("[Cal] Empty distance: %.1f cm\n", empty_distance_cm);
        Serial.println("STEP 2: Fill container COMPLETELY, measuring in 10 seconds...");
        calibrationStep = 1;
        delay(10000);
        return;
    }

    // --- STEP 2: Calibrate full ---
    if (calibrationStep == 1) {
        full_distance_cm = getDistance();
        if (full_distance_cm < 0) {
            Serial.println("[ERROR] No reading! Check sensor.");
            delay(2000);
            return;
        }
        Serial.printf("[Cal] Full distance: %.1f cm\n", full_distance_cm);

        if (empty_distance_cm <= full_distance_cm) {
            Serial.println("[ERROR] Empty must be > full distance. Restarting...");
            calibrationStep = 0;
            delay(3000);
            return;
        }
        float range = empty_distance_cm - full_distance_cm;
        if (range < 3.0f) {
            Serial.println("[ERROR] Range too small (<3 cm). Restarting...");
            calibrationStep = 0;
            delay(3000);
            return;
        }

        calibrationStep = 2;
        Serial.println("------------------------------");
        Serial.println("[Cal] SUCCESS!");
        Serial.printf("  Empty : %.1f cm\n", empty_distance_cm);
        Serial.printf("  Full  : %.1f cm\n", full_distance_cm);
        Serial.printf("  Range : %.1f cm\n", range);
        Serial.printf("  Max   : %.2f liters\n", MAX_LITERS);
        Serial.println("====== Monitoring started ======");

        // Fetch thresholds from server right after calibration
        fetchThresholds();
        delay(2000);
        return;
    }

    // --- Normal operation ---

    // Periodically refresh thresholds from the server
    if (millis() - lastThresholdFetch >= THRESHOLD_FETCH_MS) {
        fetchThresholds();
        lastThresholdFetch = millis();
    }

    float dist = getDistance();
    if (dist < 0) {
        Serial.println("[Sensor] Out of range");
        delay(2000);
        return;
    }

    float range        = empty_distance_cm - full_distance_cm;
    float water_height = constrain(empty_distance_cm - dist, 0.0f, range);

    // Volume (liters) = π × radius² × water_height ÷ 1000
    float liters = constrain(calcLiters(water_height), 0.0f, MAX_LITERS);
    float pct    = constrain((liters / MAX_LITERS) * 100.0f, 0.0f, 100.0f);

    // Signal quality
    int rssi = sim800SignalQuality();

    Serial.println("====== WATER LEVEL ======");
    Serial.printf("Distance     : %.1f cm\n", dist);
    Serial.printf("Water Height : %.1f cm\n", water_height);
    Serial.printf("Volume       : %.2f / %.2f L\n", liters, MAX_LITERS);
    Serial.printf("Fill         : %.1f%%\n", pct);
    Serial.printf("GSM Signal   : %d\n", rssi);
    if      (pct < LOW_THRESHOLD_PCT)  Serial.println("*** WARNING: LOW WATER! ***");
    else if (pct > HIGH_THRESHOLD_PCT) Serial.println("*** WARNING: NEARLY FULL! ***");
    else                               Serial.println("Status: OK");
    Serial.println();

    // Send to Node.js server
    sendToServer(liters, pct, dist);

    // Send SMS if threshold crossed (with cooldown)
    checkAndAlert(pct, liters);

    delay(5000);
}
