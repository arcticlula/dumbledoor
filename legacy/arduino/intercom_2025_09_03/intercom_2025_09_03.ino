const int CALL_INPUT_PIN = 12;  
const int UNLOCK_OUTPUT_PIN = 13; 

// --- Unlock code ---
const int CODE_LENGTH = 3;
int unlockCode[CODE_LENGTH] = {0, 0, 0};  // 0=short, 1=long

// --- Variables ---
volatile unsigned long lastPulseTime = 0;
volatile bool burstActive = false;

unsigned long burstStart = 0;
unsigned long burstEnd = 0;
int enteredCode[CODE_LENGTH];
int codeIndex = 0;

// Thresholds
const unsigned long SHORT_BURST_MAX = 400;   // <400 ms = short press
const unsigned long LONG_BURST_MIN  = 700;   // >=700 ms = long press
const unsigned long GAP_TIMEOUT     = 1500;  // reset if pause >1.5s

void IRAM_ATTR onPulse() {
  lastPulseTime = millis();
  if (!burstActive) {
    burstStart = lastPulseTime;
    burstActive = true;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(UNLOCK_OUTPUT_PIN, OUTPUT);
  digitalWrite(UNLOCK_OUTPUT_PIN, LOW);

  pinMode(CALL_INPUT_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(CALL_INPUT_PIN), onPulse, FALLING);
  delay(1000);
  Serial.println("Ready for code input...");
}

void loop() {
  unsigned long now = millis();

  // Check for end of burst
  if (burstActive && (now - lastPulseTime > 100)) {
    burstEnd = lastPulseTime;
    unsigned long burstDuration = burstEnd - burstStart;
    burstActive = false;
    
    Serial.printf("Burst detected. Duration: %lu ms\n", burstDuration);

    // Classify
    if (burstDuration < SHORT_BURST_MAX) {
      Serial.printf("Short burst (%lu ms)\n", burstDuration);
      enteredCode[codeIndex++] = 0;
    } else if (burstDuration >= LONG_BURST_MIN) {
      Serial.printf("Long burst (%lu ms)\n", burstDuration);
      enteredCode[codeIndex++] = 1;
    } else {
      Serial.printf("Invalid burst (%lu ms)\n", burstDuration);
    }

    // Compare if full code entered
    if (codeIndex == CODE_LENGTH) {
      bool match = true;
      for (int i = 0; i < CODE_LENGTH; i++) {
        if (enteredCode[i] != unlockCode[i]) {
          match = false;
          break;
        }
      }

      if (match) {
        Serial.println("Code accepted! Unlocking door...");
        digitalWrite(UNLOCK_OUTPUT_PIN, HIGH);
        delay(1000);
        digitalWrite(UNLOCK_OUTPUT_PIN, LOW);
      } else {
        Serial.println("Wrong code!");
      }

      codeIndex = 0; // reset for next attempt
    }
  }

  // Reset if too long between bursts
  if (codeIndex > 0 && (now - lastPulseTime > GAP_TIMEOUT)) {
    Serial.println("Timeout, resetting input...");
    codeIndex = 0;
  }
}
