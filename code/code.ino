#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Servo.h>

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
#define PRESENCE_TIMEOUT 3000

#define PASS_MIN 4
#define PASS_MAX 6
#define PASS_POS 9

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
  AUTH_RFID_DELETE_SCAN
};

SystemState state = SYS_IDLE;
AuthMode authMode = AUTH_ENTER_PASS;

char password[7] = "0000";
char inputPass[7];
char newPass[7];

byte inputIndex = 0;
byte newIndex = 0;

unsigned long doorTimer = 0;
unsigned long lastSeen = 0;
unsigned long lastSensorRead = 0;

long distance = 999;

byte allowedUID[4] = { 0x0E, 0xD1, 0x44, 0x02 };
bool hasAllowedUID = true;

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

void resetInput() {
  inputIndex = 0;
  memset(inputPass, 0, sizeof(inputPass));
}

void resetNewPass() {
  newIndex = 0;
  memset(newPass, 0, sizeof(newPass));
}

void showRFIDMenu() {
  showLine0("RFID MENU");
  clearLine(1);
  lcd.setCursor(0, 1);
  lcd.print("1:Add 2:Delete");
}

bool currentCardMatchesAllowed() {
  if (!hasAllowedUID || rfid.uid.size < 4) return false;
  for (byte i = 0; i < 4; i++) {
    if (rfid.uid.uidByte[i] != allowedUID[i]) return false;
  }
  return true;
}

void saveCurrentCardAsAllowed() {
  if (rfid.uid.size < 4) return;
  for (byte i = 0; i < 4; i++) {
    allowedUID[i] = rfid.uid.uidByte[i];
  }
  hasAllowedUID = true;
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

  doorTimer = millis();
  state = SYS_DOOR_OPEN;
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

  if (state == SYS_DOOR_OPEN) return;

  updateDistance();

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

    if (state == SYS_AUTH && millis() - lastSeen > PRESENCE_TIMEOUT) {
      state = SYS_IDLE;
      authMode = AUTH_ENTER_PASS;
      showWaiting();
      resetInput();
      resetNewPass();
    }
  }
}

void checkDoorTimer() {
  if (state != SYS_DOOR_OPEN) return;
  if (millis() - doorTimer > DOOR_TIMEOUT) closeDoor();
}

void checkKeypad() {

  if (state != SYS_AUTH) return;

  char key = keypad.getKey();
  if (!key) return;

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

  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  if (authMode == AUTH_RFID_ADD_SCAN) {

    saveCurrentCardAsAllowed();

    clearLine(1);
    lcd.setCursor(0, 1);
    lcd.print("RFID Added");

    delay(1000);

    authMode = AUTH_RFID_MENU;
    showRFIDMenu();

  } else if (authMode == AUTH_RFID_DELETE_SCAN) {

    clearLine(1);
    lcd.setCursor(0, 1);

    if (currentCardMatchesAllowed()) {
      hasAllowedUID = false;
      lcd.print("RFID Deleted");
    } else {
      lcd.print("RFID Not Found");
    }

    delay(1000);

    authMode = AUTH_RFID_MENU;
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

  String command = Serial.readStringUntil('\n');
  command.trim();

  if ((command == "SWITCH1_ON" || command == "SWITCH1_OFF") && state != SYS_DOOR_OPEN) openDoor();
  if ((command == "SWITCH2_ON" || command == "SWITCH2_OFF") && state == SYS_DOOR_OPEN) closeDoor();
}

void checkInsideButton() {

  static bool lastState = HIGH;
  static unsigned long lastPress = 0;

  bool s = digitalRead(INSIDE_BUTTON);

  if (s == LOW && lastState == HIGH && millis() - lastPress > 300) {

    lastPress = millis();

    if (state == SYS_DOOR_OPEN) closeDoor();
    else openDoor();
  }

  lastState = s;
}

void setup() {

  Serial.begin(9600);

  SPI.begin();
  rfid.PCD_Init();

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(INSIDE_BUTTON, INPUT_PULLUP);

  doorServo.attach(SERVO_PIN);
  doorServo.write(LOCK_POS);

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
