/*
 * Intercom Code Reader for ESP32-S2 (ADC Pulse Counting Method)
 * This version uses the ADC to poll the input pin and counts the pulses
 * of the incoming square wave. It detects "bursts" of pulses to time
 * the duration of a button press, providing a robust way to handle
 * a high-frequency call signal.
 * */

// --- Pin Definitions ---
const int CALL_INPUT_PIN = 1;  // IMPORTANT: Must be an ADC pin (e.g., GPIO1 / A0)
const int UNLOCK_OUTPUT_PIN = 13; 

// --- LED Pin Definitions ---
const int GREEN_LED_1_PIN = 39;
const int GREEN_LED_2_PIN = 37;
const int GREEN_LED_3_PIN = 35;
const int RED_LED_PIN = 33;

// --- ADC Thresholds (for 12-bit ADC, 0-4095) ---
const int ADC_LOW_THRESHOLD = 1000;  // Consider a pulse LOW when ADC is below this
const int ADC_HIGH_THRESHOLD = 3000; // Consider a pulse HIGH when ADC is above this

// --- Unlock Code Definition ---
const int CODE_LENGTH = 3;
int unlockCode[CODE_LENGTH] = {0, 0, 0};  // New code: Short, Short, Short
const int greenLedPins[CODE_LENGTH] = {GREEN_LED_1_PIN, GREEN_LED_2_PIN, GREEN_LED_3_PIN};

// --- Timing Thresholds (in milliseconds) ---
const unsigned long SHORT_PRESS_MAX = 400;   
const unsigned long LONG_PRESS_MIN  = 700;   
const unsigned long GAP_TIMEOUT     = 1500;  // Reset if the pause is longer than this.
const unsigned long BURST_END_TIMEOUT = 100; // If no pulses for 100ms, the burst is considered over.

// --- Global Variables for State Management ---
unsigned long burstStartTime = 0;
unsigned long lastPulseTime = 0;
bool burstActive = false;
bool lastStateIsHigh = true; // For detecting falling edges with ADC

// --- Variables for Code Entry ---
int enteredCode[CODE_LENGTH];
int codeIndex = 0;

// --- Helper Functions for LEDs ---
void resetLeds() {
  for (int i = 0; i < CODE_LENGTH; i++) {
    digitalWrite(greenLedPins[i], LOW);
  }
  digitalWrite(RED_LED_PIN, LOW);
}

void flashSuccess() {
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < CODE_LENGTH; j++) { digitalWrite(greenLedPins[j], HIGH); }
    delay(150);
    for (int j = 0; j < CODE_LENGTH; j++) { digitalWrite(greenLedPins[j], LOW); }
    delay(150);
  }
}

void flashError() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(RED_LED_PIN, HIGH);
    delay(150);
    digitalWrite(RED_LED_PIN, LOW);
    delay(150);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(UNLOCK_OUTPUT_PIN, OUTPUT);
  digitalWrite(UNLOCK_OUTPUT_PIN, LOW);
  
  // No pinMode needed for ADC input pins

  for (int i = 0; i < CODE_LENGTH; i++) {
    pinMode(greenLedPins[i], OUTPUT);
  }
  pinMode(RED_LED_PIN, OUTPUT);
  resetLeds();
  
  Serial.println("System ready (ADC Pulse Counting Method). Waiting for code input...");
}

void loop() {
  int adcValue = analogRead(CALL_INPUT_PIN);
  unsigned long now = millis();

  // --- Pulse Detection using ADC (Software Schmitt Trigger) ---
  bool currentStateIsHigh = (adcValue > ADC_HIGH_THRESHOLD);
  bool currentStateIsLow = (adcValue < ADC_LOW_THRESHOLD);

  if (lastStateIsHigh && currentStateIsLow) {
    // This is a falling edge, which we count as one pulse.
    lastPulseTime = now;
    lastStateIsHigh = false; // We are now in a LOW state

    if (!burstActive) {
      // This is the first pulse of a new press, so start the timer.
      burstActive = true;
      burstStartTime = now;
      Serial.println(">>> New burst detected (press started)");
    }
  } else if (!lastStateIsHigh && currentStateIsHigh) {
    // This is a rising edge, which means the pulse has ended.
    // We reset the state to be ready for the next falling edge.
    lastStateIsHigh = true;
  }

  // --- Detect the end of a burst (a pause in pulses) ---
  if (burstActive && (now - lastPulseTime > BURST_END_TIMEOUT)) {
    burstActive = false; // The burst is over.
    unsigned long burstDuration = lastPulseTime - burstStartTime;
    
    Serial.printf("Burst ended. Duration: %lu ms\n", burstDuration);

    int pressType = -1;
    if (burstDuration < SHORT_PRESS_MAX) {
      Serial.println(" -> Classified as: Short");
      pressType = 0;
    } else if (burstDuration >= LONG_PRESS_MIN) {
      Serial.println(" -> Classified as: Long");
      pressType = 1;
    } else {
      Serial.printf(" -> Classified as: Invalid\n");
    }
    
    if (pressType != -1 && codeIndex < CODE_LENGTH) {
        enteredCode[codeIndex] = pressType;
        digitalWrite(greenLedPins[codeIndex], HIGH);
        codeIndex++;
    }
    
    // --- Check if the full code has been entered ---
    if (codeIndex == CODE_LENGTH) {
      resetLeds();
      bool match = true;
      for (int i = 0; i < CODE_LENGTH; i++) {
        if (enteredCode[i] != unlockCode[i]) {
          match = false;
          break;
        }
      }

      if (match) {
        Serial.println(">>> Code accepted! Unlocking door...");
        flashSuccess();
        digitalWrite(UNLOCK_OUTPUT_PIN, HIGH);
        delay(1000);
        digitalWrite(UNLOCK_OUTPUT_PIN, LOW);
      } else {
        Serial.println(">>> Wrong code!");
        flashError();
      }

      // Automatically reset after attempt
      codeIndex = 0; 
      Serial.println("\nSystem re-armed. Ready for new code input...");
    }
  }

  // --- Check for a timeout between presses ---
  if (codeIndex > 0 && !burstActive && (now - lastPulseTime > GAP_TIMEOUT)) {
    Serial.println("Input timed out, resetting...");
    flashError();
    
    // Automatically reset after timeout
    codeIndex = 0;
    resetLeds();
    Serial.println("\nSystem re-armed. Ready for new code input...");
  }
}
