#include <WiFi.h>
#include <HTTPClient.h>

#define TRIG_PIN 5
#define ECHO_PIN 18

// --- WiFi AP credentials ---
const char* ap_ssid     = "WaterTank-ESP32";
const char* ap_password = "watertank123";
const char* serverURL   = "http://192.168.4.2:3000/api/water-level";

// --- Cylindrical tank dimensions ---
const float DIAMETER_CM = 28.0;
const float RADIUS_CM   = DIAMETER_CM / 2.0;
const float HEIGHT_CM   = 50.0;

// Volume (liters) = π × radius² × height ÷ 1000
const float MAX_LITERS = PI * RADIUS_CM * RADIUS_CM * HEIGHT_CM / 1000.0;

// --- Calibration state ---
float empty_distance_cm = 0;
float full_distance_cm  = 0;
int   calibrationStep   = 0;

// --- Averaged distance reading (10 samples) ---
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
            total += (dur * 0.0343) / 2.0;
            valid++;
        }
        delay(50);
    }
    return (valid == 0) ? -1 : total / valid;
}

// Volume (liters) = π × r² × water_height ÷ 1000
float calcLiters(float water_height_cm) {
    return PI * RADIUS_CM * RADIUS_CM * water_height_cm / 1000.0;
}

void sendToServer(float liters, float pct, float dist_cm) {
    if (WiFi.softAPgetStationNum() == 0) {
        Serial.println("[AP] No clients connected, skipping send");
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

void setup() {
    Serial.begin(115200);
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid, ap_password);

    Serial.println("\n[AP] ESP32 Access Point started");
    Serial.println("[AP] Network : " + String(ap_ssid));
    Serial.println("[AP] Password: " + String(ap_password));
    Serial.println("[AP] ESP32 IP: " + WiFi.softAPIP().toString());
    Serial.printf("[Tank] Diameter: %.1f cm | Height: %.1f cm | Max: %.2f L\n",
                  DIAMETER_CM, HEIGHT_CM, MAX_LITERS);
    Serial.println("------------------------------");
    Serial.println("STEP 1: Make sure container is EMPTY");
    Serial.println("Measuring in 5 seconds...");
    delay(5000);
}

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
        if (range < 3.0) {
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
        delay(2000);
        return;
    }

    // --- Normal operation ---
    float dist = getDistance();
    if (dist < 0) {
        Serial.println("[Sensor] Out of range");
        delay(2000);
        return;
    }

    // Water height = how far the water surface is from the sensor bottom reference
    float range          = empty_distance_cm - full_distance_cm;
    float water_height   = constrain(empty_distance_cm - dist, 0, range);

    // Volume (liters) = π × radius² × water_height ÷ 1000
    float liters = constrain(calcLiters(water_height), 0, MAX_LITERS);
    float pct    = constrain((liters / MAX_LITERS) * 100.0, 0, 100);

    Serial.println("====== WATER LEVEL ======");
    Serial.printf("Distance     : %.1f cm\n", dist);
    Serial.printf("Water Height : %.1f cm\n", water_height);
    Serial.printf("Volume       : %.2f / %.2f L\n", liters, MAX_LITERS);
    Serial.printf("Fill         : %.1f%%\n", pct);
    if      (pct < 20.0) Serial.println("*** WARNING: LOW WATER! ***");
    else if (pct > 90.0) Serial.println("*** WARNING: NEARLY FULL! ***");
    else                 Serial.println("Status: OK");
    Serial.println();

    sendToServer(liters, pct, dist);
    delay(5000);
}
