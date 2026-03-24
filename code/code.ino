#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Servo.h>
#include <EEPROM.h>
#include <string.h>

#define SERVO_PIN 8
#define TRIG 2
#define ECHO 3
#define SS_PIN 10
#define RST_PIN 9
#define INSIDE_BUTTON 7

#define LOCK_POS 0
#define OPEN_POS 90

#define PERSON_DISTANCE 10
#define DOOR_TIMEOUT 3000
#define INTERFACE_IDLE_TIMEOUT 45000

#define PASS_MIN 4
#define PASS_MAX 6
#define PASS_POS 9
#define EEPROM_PASS_LEN_ADDR 0
#define EEPROM_PASS_DATA_ADDR 1
#define EEPROM_RFID_COUNT_ADDR (EEPROM_PASS_DATA_ADDR + PASS_MAX)
#define EEPROM_RFID_DATA_ADDR (EEPROM_RFID_COUNT_ADDR + 1)
#define RFID_UID_SIZE 4
#define RECOVERY_TRIGGER "*#*#"
#define RECOVERY_WINDOW 60000UL
#define RECOVERY_MAX_ATTEMPTS 3
#define RECOVERY_LOCKOUT 300000UL

#define MAX_RFID_CARDS 5

LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo doorServo;
MFRC522 rfid(SS_PIN, RST_PIN);

enum SystemState {
  SYS_IDLE,
  SYS_AUTH,
  SYS_DOOR_OPEN
};

enum AuthMode {
  AUTH_ENTER_PASS,
  AUTH_CHANGE_PASS,
  AUTH_RFID_MENU,
  AUTH_RFID_ADD_SCAN,
  AUTH_RFID_DELETE_SCAN,
  AUTH_RECOVERY_OTP,
  AUTH_RECOVERY_SET_PASS
};

SystemState state = SYS_IDLE;
AuthMode authMode = AUTH_ENTER_PASS;

char password[7] = "0000";
char inputPass[7];
char newPass[7];
char recoveryOtpInput[7];

byte inputIndex = 0;
byte newIndex = 0;
byte recoveryOtpIndex = 0;

unsigned long lastSeen = 0;
unsigned long lastSensorRead = 0;

long distance = 999;

byte allowedUIDs[MAX_RFID_CARDS][4];
byte cardCount = 0;
unsigned long recoveryExpiresAt = 0;
unsigned long recoveryLockoutUntil = 0;
byte recoveryAttempts = 0;
bool recoveryPendingOtp = false;
bool recoveryCanSetPass = false;
char recoveryOtp[7] = "";

const byte ROWS = 4;
const byte COLS = 3;

char keys[ROWS][COLS] = {
  { '1', '2', '3' },
  { '4', '5', '6' },
  { '7', '8', '9' },
  { '*', '0', '#' }
};

byte rowPins[ROWS] = { A0, A1, A2, A3 };
byte colPins[COLS] = { 4, 5, 6 };

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

void clearLine(byte l) {
  lcd.setCursor(0, l);
  lcd.print("                ");
}

void showLine0(const char* m) {
  clearLine(0);
  lcd.setCursor(0, 0);
  lcd.print(m);
}

void showWaiting() {
  clearLine(1);
  lcd.setCursor(0, 1);
  lcd.print("Waiting...");
}

void showPassword() {
  clearLine(1);
  lcd.setCursor(0, 1);
  lcd.print("Password: ");
}

void restoreDoorStatusUI() {
  if (state == SYS_DOOR_OPEN) {
    showLine0("Door OPEN");
    clearLine(1);
    return;
  }

  showLine0("Door CLOSED");
  if (state == SYS_AUTH) showPassword();
  else showWaiting();
}

void showRecoveryNoticeThenRestore(const char* title, const char* detail, unsigned int ms) {
  showLine0(title);
  clearLine(1);
  lcd.setCursor(0, 1);
  lcd.print(detail);
  delay(ms);
  restoreDoorStatusUI();
}

void resetInput() {
  inputIndex = 0;
  memset(inputPass, 0, sizeof(inputPass));
}

void resetNewPass() {
  newIndex = 0;
  memset(newPass, 0, sizeof(newPass));
}

void resetRecoveryOtpInput() {
  recoveryOtpIndex = 0;
  memset(recoveryOtpInput, 0, sizeof(recoveryOtpInput));
}

void showRecoveryOtpInput() {
  clearLine(1);
  lcd.setCursor(0, 1);
  lcd.print("OTP: ");
  for (byte i = 0; i < recoveryOtpIndex; i++) {
    lcd.print("*");
  }
}

void showRecoveryNewPassInput() {
  clearLine(1);
  lcd.setCursor(0, 1);
  lcd.print("NEW: ");
  for (byte i = 0; i < newIndex; i++) {
    lcd.print("*");
  }
}

void clearRecoveryState() {
  recoveryPendingOtp = false;
  recoveryCanSetPass = false;
  recoveryExpiresAt = 0;
  memset(recoveryOtp, 0, sizeof(recoveryOtp));
  resetRecoveryOtpInput();
}

bool isDigitsOnlyCStr(const char* value) {
  if (!value || value[0] == '\0') return false;
  for (byte i = 0; value[i] != '\0'; i++) {
    if (!isDigit(value[i])) return false;
  }
  return true;
}

void generateRecoveryOtp() {
  long value = random(100000, 1000000);
  snprintf(recoveryOtp, sizeof(recoveryOtp), "%06ld", value);
}

void tryStartRecoveryByKeypad() {
  if (millis() < recoveryLockoutUntil) {
    showLine0("Recovery Locked");
    clearLine(1);
    lcd.setCursor(0, 1);
    lcd.print("Try later...");
    return;
  }

  recoveryPendingOtp = true;
  recoveryCanSetPass = false;
  authMode = AUTH_RECOVERY_OTP;
  recoveryExpiresAt = millis() + RECOVERY_WINDOW;
  recoveryAttempts = 0;
  generateRecoveryOtp();
  resetRecoveryOtpInput();

  showLine0("Enter OTP");
  showRecoveryOtpInput();
  Serial.println(recoveryOtp);
}

bool isValidPassLength(byte len) {
  return len >= PASS_MIN && len <= PASS_MAX;
}

void loadPasswordFromEEPROM() {
  byte storedLen = EEPROM.read(EEPROM_PASS_LEN_ADDR);
  if (!isValidPassLength(storedLen)) {
    strcpy(password, "0000");
    return;
  }

  for (byte i = 0; i < storedLen; i++) {
    char c = (char)EEPROM.read(EEPROM_PASS_DATA_ADDR + i);
    if (!isDigit(c)) {
      strcpy(password, "0000");
      return;
    }
    password[i] = c;
  }

  password[storedLen] = '\0';
}

void savePasswordToEEPROM() {
  byte len = strlen(password);
  if (!isValidPassLength(len)) return;

  EEPROM.update(EEPROM_PASS_LEN_ADDR, len);
  for (byte i = 0; i < PASS_MAX; i++) {
    byte value = (i < len) ? (byte)password[i] : 0;
    EEPROM.update(EEPROM_PASS_DATA_ADDR + i, value);
  }
}

void loadRFIDFromEEPROM() {
  byte storedCount = EEPROM.read(EEPROM_RFID_COUNT_ADDR);
  if (storedCount > MAX_RFID_CARDS) {
    cardCount = 0;
    memset(allowedUIDs, 0, sizeof(allowedUIDs));
    return;
  }

  cardCount = storedCount;
  for (byte i = 0; i < cardCount; i++) {
    for (byte j = 0; j < RFID_UID_SIZE; j++) {
      allowedUIDs[i][j] = EEPROM.read(EEPROM_RFID_DATA_ADDR + (i * RFID_UID_SIZE) + j);
    }
  }

  for (byte i = cardCount; i < MAX_RFID_CARDS; i++) {
    for (byte j = 0; j < RFID_UID_SIZE; j++) {
      allowedUIDs[i][j] = 0;
    }
  }
}

void saveRFIDToEEPROM() {
  EEPROM.update(EEPROM_RFID_COUNT_ADDR, cardCount);
  for (byte i = 0; i < MAX_RFID_CARDS; i++) {
    for (byte j = 0; j < RFID_UID_SIZE; j++) {
      byte value = (i < cardCount) ? allowedUIDs[i][j] : 0;
      EEPROM.update(EEPROM_RFID_DATA_ADDR + (i * RFID_UID_SIZE) + j, value);
    }
  }
}

void showRFIDMenu() {
  clearLine(0);
  lcd.setCursor(0, 0);
  lcd.print("RFID [");
  lcd.print(cardCount);
  lcd.print("/5]");
  clearLine(1);
  lcd.setCursor(0, 1);
  lcd.print("1:Add  2:Delete");
}

int findCardIndex(byte uid[4]) {
  for (byte i = 0; i < cardCount; i++) {
    bool match = true;
    for (byte j = 0; j < 4; j++) {
      if (allowedUIDs[i][j] != uid[j]) {
        match = false;
        break;
      }
    }
    if (match) return i;
  }
  return -1;
}

bool currentCardMatchesAllowed() {
  if (cardCount == 0 || rfid.uid.size < 4) return false;
  return findCardIndex(rfid.uid.uidByte) >= 0;
}

void saveCurrentCardAsAllowed() {
  if (rfid.uid.size < 4) return;
  
  int existingIndex = findCardIndex(rfid.uid.uidByte);
  if (existingIndex >= 0) return;
  
  if (cardCount >= MAX_RFID_CARDS) return;
  
  for (byte i = 0; i < 4; i++) {
    allowedUIDs[cardCount][i] = rfid.uid.uidByte[i];
  }
  cardCount++;
}

void deleteCurrentCard() {
  if (rfid.uid.size < 4 || cardCount == 0) return;
  
  int index = findCardIndex(rfid.uid.uidByte);
  if (index < 0) return;
  
  for (byte i = index; i < cardCount - 1; i++) {
    for (byte j = 0; j < 4; j++) {
      allowedUIDs[i][j] = allowedUIDs[i + 1][j];
    }
  }
  cardCount--;
}

long readUltrasonic() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);

  long duration = pulseIn(ECHO, HIGH, 30000);
  if (duration == 0) return 400;

  return duration * 0.034 / 2;
}

void updateDistance() {
  if (millis() - lastSensorRead < 200) return;
  lastSensorRead = millis();
  distance = readUltrasonic();
}

void openDoor() {

  if (state == SYS_DOOR_OPEN) return;

  doorServo.attach(SERVO_PIN);
  doorServo.write(OPEN_POS);

  showLine0("Door OPEN");
  clearLine(1);

  Serial.print("DOOR OPEN");

  lastSeen = millis();
  state = SYS_DOOR_OPEN;
  authMode = AUTH_ENTER_PASS;
  resetInput();
  resetNewPass();
}

void closeDoor() {

  if (state == SYS_IDLE) return;

  doorServo.write(LOCK_POS);
  delay(400);
  doorServo.detach();

  showLine0("Door CLOSED");
  showWaiting();

  Serial.print("DOOR CLOSED");

  state = SYS_IDLE;
}

void checkPresence() {

  updateDistance();

  if (state == SYS_DOOR_OPEN) {
    if (distance < PERSON_DISTANCE) {
      lastSeen = millis();
    }
    return;
  }

  if (distance < PERSON_DISTANCE) {

    lastSeen = millis();

    if (state == SYS_IDLE) {
      state = SYS_AUTH;
      authMode = AUTH_ENTER_PASS;
      showPassword();
      resetInput();
      resetNewPass();
    }

  } else {

    if (state == SYS_AUTH && millis() - lastSeen > INTERFACE_IDLE_TIMEOUT) {
      state = SYS_IDLE;
      authMode = AUTH_ENTER_PASS;
      showLine0("Door CLOSED");
      showWaiting();
      resetInput();
      resetNewPass();
    }
  }
}

void checkDoorTimer() {
  if (state != SYS_DOOR_OPEN) return;
  
  if (distance >= PERSON_DISTANCE && millis() - lastSeen > DOOR_TIMEOUT) {
    closeDoor();
  }
}

void checkKeypad() {

  if (state != SYS_AUTH) return;

  char key = keypad.getKey();
  if (!key) return;

  if ((recoveryPendingOtp || recoveryCanSetPass) && millis() > recoveryExpiresAt) {
    clearRecoveryState();
    authMode = AUTH_ENTER_PASS;
    showRecoveryNoticeThenRestore("Recovery Timeout", "Try again", 700);
    return;
  }

  if (authMode == AUTH_ENTER_PASS) {
    static const char trigger[] = RECOVERY_TRIGGER;
    static byte triggerIndex = 0;

    if (key == trigger[triggerIndex]) {
      triggerIndex++;
      if (trigger[triggerIndex] == '\0') {
        triggerIndex = 0;
        resetInput();
        tryStartRecoveryByKeypad();
        return;
      }
    } else {
      triggerIndex = (key == trigger[0]) ? 1 : 0;
    }
  }

  if (authMode == AUTH_RECOVERY_OTP) {
    static bool starCancelArmedOtp = false;

    if (key == '*') {
      if (starCancelArmedOtp) {
        starCancelArmedOtp = false;
        clearRecoveryState();
        authMode = AUTH_ENTER_PASS;
        showRecoveryNoticeThenRestore("Recovery Canceled", "Back to normal", 700);
        return;
      }
      starCancelArmedOtp = true;
      resetRecoveryOtpInput();
      showRecoveryOtpInput();
      return;
    }

    starCancelArmedOtp = false;

    if (isDigit(key) && recoveryOtpIndex < 6) {
      recoveryOtpInput[recoveryOtpIndex++] = key;
      showRecoveryOtpInput();
    }

    if (key == '#') {
      if (recoveryOtpIndex != 6) {
        clearLine(1);
        lcd.setCursor(0, 1);
        lcd.print("Need 6 digits");
        delay(700);
        resetRecoveryOtpInput();
        showRecoveryOtpInput();
        return;
      }

      recoveryOtpInput[recoveryOtpIndex] = '\0';
      if (strcmp(recoveryOtpInput, recoveryOtp) == 0) {
        recoveryPendingOtp = false;
        recoveryCanSetPass = true;
        recoveryExpiresAt = millis() + RECOVERY_WINDOW;
        recoveryAttempts = 0;
        authMode = AUTH_RECOVERY_SET_PASS;
        resetRecoveryOtpInput();
        resetNewPass();
        showLine0("New Pass");
        showRecoveryNewPassInput();
        Serial.println("OK_RECOVER");
      } else {
        recoveryAttempts++;
        Serial.println("ERR_OTP");
        if (recoveryAttempts >= RECOVERY_MAX_ATTEMPTS) {
          recoveryLockoutUntil = millis() + RECOVERY_LOCKOUT;
          clearRecoveryState();
          authMode = AUTH_ENTER_PASS;
          showRecoveryNoticeThenRestore("Recovery Locked", "Too many tries", 900);
        } else {
          clearLine(1);
          lcd.setCursor(0, 1);
          lcd.print("Wrong OTP");
          delay(700);
          resetRecoveryOtpInput();
          showRecoveryOtpInput();
        }
      }
    }
    return;
  }

  if (authMode == AUTH_RECOVERY_SET_PASS) {
    static bool starCancelArmedPass = false;

    if (key == '*') {
      if (starCancelArmedPass) {
        starCancelArmedPass = false;
        clearRecoveryState();
        authMode = AUTH_ENTER_PASS;
        showRecoveryNoticeThenRestore("Recovery Canceled", "Back to normal", 700);
        return;
      }
      starCancelArmedPass = true;
      resetNewPass();
      showRecoveryNewPassInput();
      return;
    }

    starCancelArmedPass = false;

    if (isDigit(key) && newIndex < PASS_MAX) {
      newPass[newIndex++] = key;
      showRecoveryNewPassInput();
    }

    if (key == '#') {
      if (newIndex < PASS_MIN) {
        clearLine(1);
        lcd.setCursor(0, 1);
        lcd.print("Too Short");
        delay(700);
        resetNewPass();
        showRecoveryNewPassInput();
        return;
      }

      newPass[newIndex] = '\0';
      strcpy(password, newPass);
      savePasswordToEEPROM();
      clearRecoveryState();
      authMode = AUTH_ENTER_PASS;
      resetInput();
      resetNewPass();
      showRecoveryNoticeThenRestore("Pass Recovered", "Saved", 700);
      Serial.println("OK_PASS_CHANGED");
    }
    return;
  }

  if (authMode == AUTH_ENTER_PASS) {

    if (isDigit(key) && inputIndex < PASS_MAX) {
      inputPass[inputIndex++] = key;
      lcd.setCursor(PASS_POS + inputIndex - 1, 1);
      lcd.print("*");
    }

    if (key == '*') {

      if (inputIndex < PASS_MIN) {
        resetInput();
        showPassword();
        return;
      }

      inputPass[inputIndex] = '\0';

      if (strcmp(inputPass, password) == 0) {

        authMode = AUTH_CHANGE_PASS;
        resetNewPass();

        clearLine(1);
        lcd.setCursor(0, 1);
        lcd.print("New Pass (*RFID)");
      } else {

        clearLine(1);
        lcd.setCursor(0, 1);
        lcd.print("Wrong Pass");
        delay(800);

        showPassword();
        resetInput();
      }
    }

    if (key == '#') {

      if (inputIndex < PASS_MIN) {
        resetInput();
        showPassword();
        return;
      }

      inputPass[inputIndex] = '\0';

      if (strcmp(inputPass, password) == 0) openDoor();
      else {

        clearLine(1);
        lcd.setCursor(0, 1);
        lcd.print("Wrong Pass");
        delay(800);

        showPassword();
      }

      resetInput();
    }

  } else if (authMode == AUTH_CHANGE_PASS) {

    if (isDigit(key) && newIndex < PASS_MAX) {
      newPass[newIndex++] = key;
      lcd.setCursor(PASS_POS + newIndex - 1, 1);
      lcd.print("*");
    }

    if (key == '#') {

      if (newIndex < PASS_MIN) {

        clearLine(1);
        lcd.setCursor(0, 1);
        lcd.print("Too Short");
        delay(800);

        showPassword();
        authMode = AUTH_ENTER_PASS;
        resetInput();
        resetNewPass();
        return;
      }

      newPass[newIndex] = '\0';
      strcpy(password, newPass);
      savePasswordToEEPROM();

      clearLine(1);
      lcd.setCursor(0, 1);
      lcd.print("Pass Changed");

      delay(1000);

      authMode = AUTH_ENTER_PASS;
      resetInput();
      resetNewPass();
      showPassword();
    }

    if (key == '*') {
      authMode = AUTH_RFID_MENU;
      showRFIDMenu();
      resetNewPass();
    }

  } else if (authMode == AUTH_RFID_MENU) {

    if (key == '1') {
      authMode = AUTH_RFID_ADD_SCAN;
      showLine0("Add RFID");
      clearLine(1);
      lcd.setCursor(0, 1);
      lcd.print("Scan card...");
    }

    if (key == '2') {
      authMode = AUTH_RFID_DELETE_SCAN;
      showLine0("Delete RFID");
      clearLine(1);
      lcd.setCursor(0, 1);
      lcd.print("Scan card...");
    }

    if (key == '#') {
      authMode = AUTH_ENTER_PASS;
      showLine0("Door CLOSED");
      showPassword();
      resetInput();
      resetNewPass();
    }

  } else if (authMode == AUTH_RFID_ADD_SCAN || authMode == AUTH_RFID_DELETE_SCAN) {

    if (key == '#') {
      authMode = AUTH_RFID_MENU;
      showRFIDMenu();
    }
  }
}

void checkRFID() {

  if (state != SYS_AUTH) return;
  if (authMode == AUTH_CHANGE_PASS || authMode == AUTH_RECOVERY_OTP || authMode == AUTH_RECOVERY_SET_PASS) return;

  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  if (authMode == AUTH_RFID_ADD_SCAN) {

    int existingIdx = findCardIndex(rfid.uid.uidByte);
    
    if (existingIdx >= 0) {
      clearLine(1);
      lcd.setCursor(0, 1);
      lcd.print("Already exists!");
      delay(1000);
    } else if (cardCount >= MAX_RFID_CARDS) {
      clearLine(1);
      lcd.setCursor(0, 1);
      lcd.print("Memory full!");
      delay(1000);
    } else {
      saveCurrentCardAsAllowed();
      saveRFIDToEEPROM();
      clearLine(1);
      lcd.setCursor(0, 1);
      lcd.print("RFID Added [");
      lcd.print(cardCount);
      lcd.print("/5]");
      delay(1000);
    }

    authMode = AUTH_ENTER_PASS;
    state = SYS_IDLE;
    showLine0("Door CLOSED");
    showWaiting();
    resetInput();
    resetNewPass();

  } else if (authMode == AUTH_RFID_DELETE_SCAN) {

    clearLine(1);
    lcd.setCursor(0, 1);

    int idx = findCardIndex(rfid.uid.uidByte);
    if (idx >= 0) {
      deleteCurrentCard();
      saveRFIDToEEPROM();
      lcd.print("Deleted [");
      lcd.print(cardCount);
      lcd.print("/5]");
    } else {
      lcd.print("Not Found");
    }

    delay(1000);

    authMode = AUTH_ENTER_PASS;
    state = SYS_IDLE;
    showLine0("Door CLOSED");
    showWaiting();
    resetInput();
    resetNewPass();

  } else if (authMode == AUTH_RFID_MENU) {
    clearLine(1);
    lcd.setCursor(0, 1);
    lcd.print("Pick 1:Add 2:Del");
    delay(800);
    showRFIDMenu();

  } else {

    if (currentCardMatchesAllowed()) {
      openDoor();
    } else {

      clearLine(1);
      lcd.setCursor(0,1);
      lcd.print("Access Denied");

      delay(1000);

      showPassword();
      resetInput();
      lastSeen = millis();
    }
  }

  rfid.PICC_HaltA();
}

void checkBluetooth() {

  if (!Serial.available()) return;

  static char command[32];
  byte idx = 0;
  while (Serial.available() && idx < sizeof(command) - 1) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (idx == 0) continue;
      break;
    }
    command[idx++] = c;
  }
  command[idx] = '\0';
  if (idx == 0) return;

  if ((recoveryPendingOtp || recoveryCanSetPass) && millis() > recoveryExpiresAt) {
    clearRecoveryState();
    Serial.println("ERR_RECOVER_TIMEOUT");
    showRecoveryNoticeThenRestore("Recovery Timeout", "Try again", 700);
  }

  if (strcmp(command, "OTP_REQ") == 0 || strcmp(command, "OTP_REQUEST") == 0 || strcmp(command, "SEND_OTP") == 0) {
    if (!recoveryPendingOtp) {
      Serial.println("ERR_OTP_REQ_STATE");
      return;
    }
    if (millis() < recoveryLockoutUntil) {
      clearRecoveryState();
      Serial.println("ERR_RECOVER_LOCKED");
      return;
    }
    Serial.println(recoveryOtp);
    return;
  }

  if (strncmp(command, "RECOVER_OTP:", 12) == 0) {
    if (!recoveryPendingOtp) {
      Serial.println("ERR_RECOVER_STATE");
      return;
    }

    if (millis() < recoveryLockoutUntil) {
      clearRecoveryState();
      Serial.println("ERR_RECOVER_LOCKED");
      return;
    }

    const char* otp = command + 12;
    if (strcmp(otp, recoveryOtp) == 0) {
      recoveryPendingOtp = false;
      recoveryCanSetPass = true;
      recoveryExpiresAt = millis() + RECOVERY_WINDOW;
      recoveryAttempts = 0;
      Serial.println("OK_RECOVER");
      showLine0("OTP Verified");
      clearLine(1);
      lcd.setCursor(0, 1);
      lcd.print("Send SET_PASS");
    } else {
      recoveryAttempts++;
      Serial.println("ERR_OTP");
      if (recoveryAttempts >= RECOVERY_MAX_ATTEMPTS) {
        recoveryLockoutUntil = millis() + RECOVERY_LOCKOUT;
        clearRecoveryState();
        showRecoveryNoticeThenRestore("Recovery Locked", "Too many tries", 900);
      }
    }
    return;
  }

  if (strncmp(command, "SET_PASS:", 9) == 0) {
    if (!recoveryCanSetPass || millis() > recoveryExpiresAt) {
      clearRecoveryState();
      Serial.println("ERR_SET_PASS_STATE");
      return;
    }

    const char* nextPass = command + 9;
    byte nextPassLen = (byte)strlen(nextPass);
    if (!isDigitsOnlyCStr(nextPass) || !isValidPassLength(nextPassLen)) {
      Serial.println("ERR_SET_PASS_FORMAT");
      return;
    }

    strncpy(password, nextPass, sizeof(password) - 1);
    password[sizeof(password) - 1] = '\0';
    savePasswordToEEPROM();
    clearRecoveryState();
    resetInput();
    resetNewPass();

    showRecoveryNoticeThenRestore("Pass Recovered", "Saved", 700);
    Serial.println("OK_PASS_CHANGED");
    return;
  }

  if (strcmp(command, "SWITCH1_ON") == 0 || strcmp(command, "SWITCH1_OFF") == 0) {
    if (state == SYS_AUTH) {
      openDoor();
      Serial.println("OK_OPEN");
    } else {
      Serial.println("ERR_SLEEP");
    }
  } else if (strcmp(command, "SWITCH2_ON") == 0 || strcmp(command, "SWITCH2_OFF") == 0) {
    if (state == SYS_DOOR_OPEN) {
      closeDoor();
      Serial.println("OK_CLOSE");
    } else {
      Serial.println("ERR_NOT_OPEN");
    }
  }
}

void checkInsideButton() {

  static bool lastState = HIGH;
  static unsigned long lastPress = 0;

  bool s = digitalRead(INSIDE_BUTTON);

  if (s == LOW && lastState == HIGH && millis() - lastPress > 300) {

    lastPress = millis();

    if (state == SYS_DOOR_OPEN) {
      closeDoor();
    } else {
      openDoor();
    }
  }

  lastState = s;
}

void setup() {

  Serial.begin(9600);
  randomSeed(analogRead(A5) ^ millis());
  loadPasswordFromEEPROM();
  loadRFIDFromEEPROM();

  SPI.begin();
  rfid.PCD_Init();

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(INSIDE_BUTTON, INPUT_PULLUP);

  doorServo.attach(SERVO_PIN);
  doorServo.write(LOCK_POS);
  delay(500);
  doorServo.detach();

  lcd.init();
  lcd.backlight();

  showLine0("Door CLOSED");
  showWaiting();
}

void loop() {

  checkPresence();
  checkDoorTimer();
  checkKeypad();
  checkRFID();
  checkBluetooth();
  checkInsideButton();
}
