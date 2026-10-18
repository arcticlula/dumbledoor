/*
 * Intercom Code Reader for ESP32-S2 (Polling Method with LED Feedback)
 * This version uses a polling method to watch the CALL_INPUT_PIN and
 * automatically resets after each code attempt or timeout.
 * */

// --- Pin Definitions ---
const int CALL_INPUT_PIN = 12;  
const int UNLOCK_OUTPUT_PIN = 13; 

// --- LED Pin Definitions ---
const int GREEN_LED_1_PIN = 39;
const int GREEN_LED_2_PIN = 37;
const int GREEN_LED_3_PIN = 35;
const int RED_LED_PIN = 33;

// --- Unlock Code Definition ---
const int CODE_LENGTH = 3;
int unlockCode[CODE_LENGTH] = {0, 1, 0};  // Example: Short, Long, Short
const int greenLedPins[CODE_LENGTH] = {GREEN_LED_1_PIN, GREEN_LED_2_PIN, GREEN_LED_3_PIN};

// --- Timing Thresholds (in milliseconds) ---
const unsigned long MIN_VALID_PRESS = 20;    // A press must be longer than this to be considered valid.
const unsigned long SHORT_PRESS_MAX = 400;   
const unsigned long LONG_PRESS_MIN  = 700;   
const unsigned long GAP_TIMEOUT     = 1500;  // Reset if the pause is longer than this.

// --- Global Variables for State Management ---
unsigned long pressStartTime = 0;
unsigned long lastPressEndTime = 0;
bool isPressing = false;

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
  pinMode(CALL_INPUT_PIN, INPUT_PULLUP); // Use PULLUP to ensure a stable HIGH state

  for (int i = 0; i < CODE_LENGTH; i++) {
    pinMode(greenLedPins[i], OUTPUT);
  }
  pinMode(RED_LED_PIN, OUTPUT);
  resetLeds();
  
  Serial.println("System ready (Polling Method). Waiting for code input...");
}

void loop() {
  bool pinState = (digitalRead(CALL_INPUT_PIN) == LOW);

  // --- Detect Start of a Press (Falling Edge) ---
  if (pinState && !isPressing) {
    isPressing = true;
    pressStartTime = millis();
    Serial.println(">>> Press detected (Falling edge)");
  }
  
  // --- Detect End of a Press (Rising Edge) ---
  else if (!pinState && isPressing) {
    isPressing = false;
    unsigned long pressDuration = millis() - pressStartTime;
    lastPressEndTime = millis(); // Record when the pause started
    
    Serial.printf("Press ended. Duration: %lu ms\n", pressDuration);

    if (pressDuration >= MIN_VALID_PRESS) {
      int pressType = -1;
      if (pressDuration < SHORT_PRESS_MAX) {
        Serial.println(" -> Classified as: Short");
        pressType = 0;
      } else if (pressDuration >= LONG_PRESS_MIN) {
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
    }
    
    // --- Check if the full code has been entered ---
    if (codeIndex == CODE_LENGTH) {
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
      resetLeds();
      Serial.println("\nSystem re-armed. Ready for new code input...");
    }
  }

  // --- Check for a timeout between presses ---
  if (codeIndex > 0 && !isPressing && (millis() - lastPressEndTime > GAP_TIMEOUT)) {
    Serial.println("Input timed out, resetting...");
    flashError();
    
    // Automatically reset after timeout
    codeIndex = 0;
    resetLeds();
    Serial.println("\nSystem re-armed. Ready for new code input...");
  }
}
