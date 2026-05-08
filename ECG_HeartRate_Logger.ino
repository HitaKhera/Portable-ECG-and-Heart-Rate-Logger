/*
 * ============================================================
 *  PORTABLE ECG AND HEART RATE LOGGER
 * ============================================================
 *  Hardware:
 *    - Arduino Nano
 *    - AD8232 ECG Sensor Module
 *    - ADS1115 16-bit ADC (I2C: 0x48)
 *    - SSD1306 OLED Display 128x32 (I2C: 0x3C)
 *    - DS3231 RTC Module (I2C: 0x57)
 *    - MicroSD Card Module (SPI: CS = D10)
 *    - 3 ECG Electrodes (RA, LA, RL)
 *
 *  Libraries Required (install via Library Manager):
 *    - Adafruit GFX Library
 *    - Adafruit SSD1306
 *    - Adafruit ADS1X15
 *    - TimerOne
 *    - RTClib  (by Adafruit)
 *    - SD      (built-in)
 *
 *  Wiring Summary:
 *    OLED  SDA -> A4  |  SCL -> A5
 *    ADS1115 SDA -> A4  |  SCL -> A5
 *    DS3231  SDA -> A4  |  SCL -> A5   (all share I2C bus)
 *    SD CS  -> D10
 *    AD8232 OUTPUT -> ADS1115 A0
 *    AD8232 LO+    -> D4
 *    AD8232 LO-    -> D5
 *    AD8232 SDN    -> D6 (optional shutdown pin)
 * ============================================================
 */

#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_ADS1X15.h>
#include <TimerOne.h>
#include <RTClib.h>

// ============================================================
//  CONFIG
// ============================================================
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     32
#define OLED_I2C_ADDR     0x3C

#define SD_CS_PIN         10

#define LO_PLUS_PIN       4    // AD8232 Leads-Off detect +
#define LO_MINUS_PIN      5    // AD8232 Leads-Off detect -
#define SDN_PIN           6    // AD8232 shutdown (optional)

#define ADS_I2C_ADDR      0x48
#define ADS_GAIN          GAIN_ONE   // ±4.096V range (fits 3.3V signal)

// Sampling: 500 SPS is safe for ECG (Nyquist well above 150Hz)
const long   SAMPLE_INTERVAL_US  = 2000;  // 1,000,000 / 500 = 2000 µs
const unsigned long DISPLAY_UPDATE_MS = 30;

#define DISPLAY_BUFFER_LEN  SCREEN_WIDTH   // one pixel per column
#define RR_HISTORY_LEN      8

// R-peak detection tuning
const unsigned long MIN_RR_MS = 300;   // ~200 BPM max
const unsigned long MAX_RR_MS = 2000;  // ~30  BPM min

// Info area at top of OLED
const int TOP_INFO_HEIGHT = 10;
const int WAVEFORM_TOP    = TOP_INFO_HEIGHT;
const int WAVEFORM_HEIGHT = SCREEN_HEIGHT - WAVEFORM_TOP;

// SD log file
#define LOG_FILENAME "ecglog.csv"
#define LOG_INTERVAL_MS 1000   // write to SD every second batch

// ============================================================
//  GLOBALS
// ============================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_ADS1115 ads;
RTC_DS3231       rtc;

// Timer ISR flag
volatile bool dataReadyFlag = false;

// Circular display buffer
int displayY[DISPLAY_BUFFER_LEN];
int displayIdx = 0;

// BPM tracking
unsigned long rrHistory[RR_HISTORY_LEN] = {0};
int           rrIndex           = 0;
unsigned long lastPeakTimeMicros = 0;
bool          firstPeakSeen     = false;
float         currentBPM        = 0.0F;

// Adaptive baseline / threshold
float baseline       = 1.65F;   // ~mid-supply for 3.3V systems
float baselineAlpha  = 0.9995F;
float peakEst        = 0.0F;
float thresholdFactor = 0.30F;
float prevV          = 0.0F;
int   consecAbove    = 0;

// Display timing
unsigned long lastDisplayMs  = 0;
unsigned long lastSDWriteMs  = 0;

// SD / leads-off state
bool sdAvailable     = false;
bool leadsOff        = false;

// ============================================================
//  TIMER ISR  – fires at SAMPLE_INTERVAL_US
// ============================================================
void timerISR() {
  dataReadyFlag = true;
}

// ============================================================
//  HELPERS
// ============================================================

/**
 * Convert raw ADS1115 count to voltage (V).
 * GAIN_ONE => 4.096V FSR, 32768 counts.
 */
inline float rawToVoltage(int16_t raw) {
  return raw * (4.096F / 32768.0F);
}

/**
 * Map voltage to a Y pixel coordinate on the waveform area.
 * Assumes signal swings 0–3.3V centered around ~1.65V.
 */
int voltageToY(float v) {
  // Clamp
  if (v < 0.0F) v = 0.0F;
  if (v > 3.3F) v = 3.3F;
  // Map [0, 3.3] → [WAVEFORM_TOP + WAVEFORM_HEIGHT - 1 .. WAVEFORM_TOP]
  int y = WAVEFORM_TOP + WAVEFORM_HEIGHT - 1
          - (int)((v / 3.3F) * (WAVEFORM_HEIGHT - 1));
  return y;
}

/**
 * R-peak detection using adaptive threshold + refractory period.
 * Updates currentBPM via RR interval history.
 */
void detectPeak(float v, unsigned long tMicros) {
  // Adaptive baseline (exponential moving average)
  baseline = baselineAlpha * baseline + (1.0F - baselineAlpha) * v;

  // Track running peak amplitude (decays slowly)
  if (v > peakEst) peakEst = v;
  peakEst *= 0.999F;

  float dynamicThresh = baseline + thresholdFactor * (peakEst - baseline + 0.02F);

  // Count consecutive samples above threshold (debounce)
  if (v > dynamicThresh && prevV <= dynamicThresh) {
    consecAbove = 1;
  } else if (v > dynamicThresh) {
    consecAbove++;
  } else {
    consecAbove = 0;
  }

  if (consecAbove >= 1) {
    unsigned long nowMs = tMicros / 1000UL;
    unsigned long lastMs = lastPeakTimeMicros / 1000UL;

    if ((nowMs - lastMs) > MIN_RR_MS) {
      // Detect falling edge of peak (prev was higher → we're past the apex)
      if (prevV > v && prevV > dynamicThresh) {
        unsigned long rr = 0;
        if (lastPeakTimeMicros != 0) {
          rr = (tMicros - lastPeakTimeMicros) / 1000UL;
        }
        lastPeakTimeMicros = tMicros;
        firstPeakSeen = true;

        if (rr >= MIN_RR_MS && rr <= MAX_RR_MS) {
          rrHistory[rrIndex] = rr;
          rrIndex = (rrIndex + 1) % RR_HISTORY_LEN;

          // Average RR → BPM
          unsigned long sum = 0;
          int cnt = 0;
          for (int i = 0; i < RR_HISTORY_LEN; i++) {
            if (rrHistory[i] != 0) { sum += rrHistory[i]; cnt++; }
          }
          if (cnt > 0) {
            currentBPM = 60000.0F / ((float)sum / (float)cnt);
          }
        }
      }
    }
  }

  prevV = v;
}

// ============================================================
//  SD CARD LOGGING
// ============================================================
void logToSD(unsigned long tMs, float voltage, float bpm, bool lo) {
  if (!sdAvailable) return;

  File f = SD.open(LOG_FILENAME, FILE_WRITE);
  if (!f) return;

  // Timestamp from RTC
  DateTime now = rtc.now();
  char ts[22];
  snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());

  f.print(ts);
  f.print(F(","));
  f.print(tMs);
  f.print(F(","));
  f.print(voltage, 4);
  f.print(F(","));
  f.print((int)(bpm + 0.5F));
  f.print(F(","));
  f.println(lo ? F("LEADS_OFF") : F("OK"));
  f.close();
}

// ============================================================
//  DISPLAY
// ============================================================
void updateDisplay() {
  display.clearDisplay();

  if (leadsOff) {
    // Show leads-off warning
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 0);
    display.print(F("!! LEADS OFF !!"));
    display.setCursor(20, 16);
    display.print(F("Check electrodes"));
    display.display();
    return;
  }

  // --- Top info bar: BPM ---
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("HR:"));
  int bpmShow = (int)(currentBPM + 0.5F);
  if (!firstPeakSeen || bpmShow < 30 || bpmShow > 220) {
    display.print(F("---"));
  } else {
    display.print(bpmShow);
  }
  display.print(F(" bpm"));

  // --- Horizontal grid lines ---
  for (int y = WAVEFORM_TOP; y < SCREEN_HEIGHT; y += 8) {
    for (int x = 0; x < SCREEN_WIDTH; x += 4) {
      display.drawPixel(x, y, SSD1306_WHITE);
    }
  }

  // --- ECG Waveform ---
  int start = displayIdx;  // oldest sample
  int prevX = 0;
  int prevYd = displayY[start];
  for (int i = 1; i < DISPLAY_BUFFER_LEN; i++) {
    int idx = (start + i) % DISPLAY_BUFFER_LEN;
    int x   = i;
    int y   = displayY[idx];
    display.drawLine(prevX, prevYd, x, y, SSD1306_WHITE);
    prevX  = x;
    prevYd = y;
  }

  display.display();
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000);
  Serial.println(F("=== Portable ECG Logger Booting ==="));

  // Leads-off detect pins
  pinMode(LO_PLUS_PIN,  INPUT);
  pinMode(LO_MINUS_PIN, INPUT);

  Wire.begin();

  // ----- OLED -----
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    Serial.println(F("OLED init FAILED. Check wiring."));
    while (1);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 10);
  display.print(F("ECG Logger"));
  display.display();
  Serial.println(F("OLED OK"));

  // ----- ADS1115 -----
  if (!ads.begin(ADS_I2C_ADDR)) {
    Serial.println(F("ADS1115 init FAILED. Check wiring."));
    while (1);
  }
  ads.setGain(ADS_GAIN);
  Serial.println(F("ADS1115 OK"));

  // ----- RTC -----
  if (!rtc.begin()) {
    Serial.println(F("DS3231 not found – timestamps will be 0."));
  } else {
    if (rtc.lostPower()) {
      // Set RTC to compile time when power was lost
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println(F("RTC adjusted to compile time."));
    }
    Serial.println(F("RTC OK"));
  }

  // ----- SD Card -----
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println(F("SD card init FAILED – logging disabled."));
    sdAvailable = false;
  } else {
    sdAvailable = true;
    Serial.println(F("SD card OK"));
    // Write CSV header if file doesn't exist
    if (!SD.exists(LOG_FILENAME)) {
      File f = SD.open(LOG_FILENAME, FILE_WRITE);
      if (f) {
        f.println(F("Timestamp,Millis,Voltage_V,BPM,Status"));
        f.close();
      }
    }
  }

  // ----- Initialise display buffer -----
  int midY = WAVEFORM_TOP + WAVEFORM_HEIGHT / 2;
  for (int i = 0; i < DISPLAY_BUFFER_LEN; i++) {
    displayY[i] = midY;
  }

  // ----- Timer1 interrupt for sampling -----
  Timer1.initialize(SAMPLE_INTERVAL_US);
  Timer1.attachInterrupt(timerISR);
  Serial.print(F("Sampling at "));
  Serial.print(1000000L / SAMPLE_INTERVAL_US);
  Serial.println(F(" Hz. Ready."));

  delay(1000);  // show boot message on OLED briefly
}

// ============================================================
//  MAIN LOOP
// ============================================================
void loop() {

  // ---- (1) Sample on timer flag ----
  if (dataReadyFlag) {
    noInterrupts();
    dataReadyFlag = false;
    interrupts();

    // Check leads-off status
    leadsOff = (digitalRead(LO_PLUS_PIN) == HIGH ||
                digitalRead(LO_MINUS_PIN) == HIGH);

    if (!leadsOff) {
      int16_t raw      = ads.readADC_SingleEnded(0);
      unsigned long tUs = micros();
      float v          = rawToVoltage(raw);

      // Detect R-peak → BPM
      detectPeak(v, tUs);

      // Push to display ring buffer
      int y = voltageToY(v);
      displayY[displayIdx] = y;
      displayIdx = (displayIdx + 1) % DISPLAY_BUFFER_LEN;

      // Serial plotter output (open Serial Plotter in Arduino IDE)
      Serial.print(F("ECG:"));
      Serial.print(v, 4);
      Serial.print(F(",BPM:"));
      Serial.println((int)(currentBPM + 0.5F));
    }
  }

  // ---- (2) Refresh OLED ----
  unsigned long nowMs = millis();
  if (nowMs - lastDisplayMs >= DISPLAY_UPDATE_MS) {
    lastDisplayMs = nowMs;
    updateDisplay();
  }

  // ---- (3) Log to SD every LOG_INTERVAL_MS ----
  if (nowMs - lastSDWriteMs >= LOG_INTERVAL_MS) {
    lastSDWriteMs = nowMs;
    // Read latest sample for logging
    int16_t raw = ads.readADC_SingleEnded(0);
    float v     = rawToVoltage(raw);
    logToSD(nowMs, v, currentBPM, leadsOff);
  }
}
