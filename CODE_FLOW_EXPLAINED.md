# Smart Door Automation - Luồng Hoạt Động Code

## 📋 Tổng Quan Kiến Trúc

```
┌─────────────┐
│   SETUP()   │ → Khởi tạo tất cả components
└──────┬──────┘
       ↓
┌─────────────┐
│   LOOP()    │ → Chạy liên tục, gọi các hàm check
└──────┬──────┘
       ↓
   ┌───┴───┐
   │ STATE │ → Quản lý trạng thái hệ thống
   └───────┘
```

---

## 🔄 State Machine (Máy Trạng Thái)

### SystemState (Trạng thái chính)

```
┌──────────────┐
│   SYS_IDLE   │ ← Ban đầu: Cửa đóng, chờ người đến
└──────┬───────┘
       │ Person detected (distance < 10cm)
       ↓
┌──────────────┐
│  SYS_AUTH    │ ← Đang xác thực (keypad/RFID)
└──────┬───────┘
       │ Authentication success
       ↓
┌──────────────┐
│SYS_DOOR_OPEN │ ← Cửa mở, đợi người đi qua
└──────┬───────┘
       │ No person for 3s
       ↓
   (quay lại SYS_IDLE)
```

### AuthMode (Sub-states trong SYS_AUTH)

```
AUTH_ENTER_PASS          ← Nhập password để mở cửa hoặc vào menu
    ↓ [pass + *]
AUTH_CHANGE_PASS         ← Đổi password
    ↓ [new pass + #]  ↓ [*]
    └────────────────→ AUTH_RFID_MENU
                           ↓ [1]         ↓ [2]
                   AUTH_RFID_ADD_SCAN  AUTH_RFID_DELETE_SCAN
```

---

## 🚀 SETUP() - Khởi Tạo

```cpp
void setup() {
  // 1. Khởi tạo Serial (Bluetooth HC-05)
  Serial.begin(9600);
  
  // 2. Khởi tạo RFID module
  SPI.begin();
  rfid.PCD_Init();
  
  // 3. Setup GPIO pins
  pinMode(TRIG, OUTPUT);      // Ultrasonic trigger
  pinMode(ECHO, INPUT);       // Ultrasonic echo
  pinMode(INSIDE_BUTTON, INPUT_PULLUP);  // Nút nhấn trong nhà
  
  // 4. Khóa cửa ban đầu
  doorServo.attach(SERVO_PIN);
  doorServo.write(LOCK_POS);  // 0 độ = khóa
  delay(500);
  doorServo.detach();         // Tắt servo để tiết kiệm điện
  
  // 5. Hiển thị LCD
  lcd.init();
  lcd.backlight();
  showLine0("Door CLOSED");
  showWaiting();
}
```

**Kết quả:**
- Hệ thống ở trạng thái `SYS_IDLE`
- LCD hiển thị: `Door CLOSED / Waiting...`
- Servo đã detach (không tiêu thụ điện)

---

## 🔁 LOOP() - Vòng Lặp Chính

```cpp
void loop() {
  checkPresence();      // 1. Kiểm tra có người không
  checkDoorTimer();     // 2. Kiểm tra tự động đóng cửa
  checkKeypad();        // 3. Đọc phím bấm
  checkRFID();          // 4. Đọc thẻ RFID
  checkBluetooth();     // 5. Nhận lệnh từ app
  checkInsideButton();  // 6. Đọc nút trong nhà
}
```

**Frequency:** Chạy liên tục ~1000-5000 lần/giây (tùy hardware)

---

## 📡 1. checkPresence() - Phát Hiện Người

### Luồng hoạt động:

```
updateDistance() → Đọc ultrasonic mỗi 200ms
    ↓
distance < 10cm? (có người)
    │ YES                     │ NO
    ↓                         ↓
Update lastSeen          Nếu SYS_AUTH:
    ↓                    → timeout 3s → SYS_IDLE
Nếu SYS_IDLE:
→ Chuyển sang SYS_AUTH
→ Hiển thị "Password:"
```

### Code flow:

```cpp
void checkPresence() {
  updateDistance();  // Đọc sensor HC-SR04
  
  // CẢ KHI DOOR_OPEN cũng update lastSeen
  if (state == SYS_DOOR_OPEN) {
    if (distance < PERSON_DISTANCE) {
      lastSeen = millis();  // Ghi nhận người còn đứng đó
    }
    return;
  }
  
  if (distance < PERSON_DISTANCE) {  // < 10cm
    lastSeen = millis();
    
    if (state == SYS_IDLE) {
      state = SYS_AUTH;           // Chuyển sang mode xác thực
      authMode = AUTH_ENTER_PASS;
      showPassword();
      resetInput();
    }
  } else {
    // Nếu người đi ra ngoài quá 3s → timeout
    if (state == SYS_AUTH && 
        authMode == AUTH_ENTER_PASS && 
        millis() - lastSeen > PRESENCE_TIMEOUT) {
      state = SYS_IDLE;
      showLine0("Door CLOSED");
      showWaiting();
    }
  }
}
```

**Key points:**
- Chỉ timeout khi ở `AUTH_ENTER_PASS` (không timeout khi đang đổi pass hoặc RFID menu)
- Update `lastSeen` liên tục khi có người (kể cả khi cửa đang mở)

---

## 🚪 2. checkDoorTimer() - Tự Động Đóng Cửa

```
Cửa đang mở (SYS_DOOR_OPEN)?
    │ NO → return
    ↓ YES
Có người gần không? (distance < 10cm)
    │ NO → Đã 3s chưa? (millis() - lastSeen > 3000)
    │         │ YES → closeDoor()
    ↓ YES
Không làm gì (người vẫn đứng đó)
```

### Code:

```cpp
void checkDoorTimer() {
  if (state != SYS_DOOR_OPEN) return;
  
  // CHỈ đóng cửa khi: KHÔNG có người + đã 3s
  if (distance >= PERSON_DISTANCE && 
      millis() - lastSeen > DOOR_TIMEOUT) {
    closeDoor();
  }
}
```

**Scenarios:**

| Tình huống | Hành động |
|------------|-----------|
| Người đi qua cửa (2s) rồi đi ra xa | Đợi 3s sau khi người rời → đóng cửa ✅ |
| Người đứng ở cửa 10s | Cửa vẫn mở (không timeout) ✅ |
| Mở cửa, không có người | Đóng sau 3s ✅ |

---

## ⌨️ 3. checkKeypad() - Xử Lý Bàn Phím

### Flow chart:

```
state == SYS_AUTH? 
    │ NO → return
    ↓ YES
authMode là gì?
    │
    ├─ AUTH_ENTER_PASS
    │   ├─ [0-9] → Thêm vào inputPass[], hiện *
    │   ├─ [#]   → Check password → openDoor() hoặc "Wrong Pass"
    │   └─ [*]   → Check password → AUTH_CHANGE_PASS hoặc "Wrong Pass"
    │
    ├─ AUTH_CHANGE_PASS
    │   ├─ [0-9] → Thêm vào newPass[], hiện *
    │   ├─ [#]   → Lưu password mới → quay về AUTH_ENTER_PASS
    │   └─ [*]   → Vào AUTH_RFID_MENU
    │
    ├─ AUTH_RFID_MENU
    │   ├─ [1] → AUTH_RFID_ADD_SCAN
    │   ├─ [2] → AUTH_RFID_DELETE_SCAN
    │   └─ [#] → Thoát về AUTH_ENTER_PASS
    │
    └─ AUTH_RFID_ADD/DELETE_SCAN
        └─ [#] → Quay lại RFID menu
```

### Chi tiết từng mode:

#### AUTH_ENTER_PASS (Nhập password)

```cpp
// User nhập: 1234#
if (isDigit(key) && inputIndex < PASS_MAX) {
  inputPass[inputIndex++] = key;  // inputPass = "1234"
  lcd.print("*");                 // Hiện: ****
}

if (key == '#') {
  if (inputIndex < PASS_MIN) {    // < 4 digits → reject
    resetInput();
    showPassword();
    return;
  }
  
  inputPass[inputIndex] = '\0';   // Null-terminate string
  
  if (strcmp(inputPass, password) == 0) {
    openDoor();  // ✅ Đúng password
  } else {
    lcd.print("Wrong Pass");  // ❌ Sai
    delay(800);
  }
  resetInput();
}

if (key == '*') {
  // Tương tự như #, nhưng vào AUTH_CHANGE_PASS thay vì mở cửa
}
```

#### AUTH_CHANGE_PASS (Đổi password)

```
LCD hiển thị: "New Pass (*RFID)"
User nhập: 5678#  → Lưu password mới
User nhấn: *      → Vào RFID menu
```

#### AUTH_RFID_MENU (Menu RFID)

```
LCD hiển thị: 
  Line 0: "RFID [3/5]"
  Line 1: "1:Add  2:Delete"

User nhấn 1 → AUTH_RFID_ADD_SCAN
User nhấn 2 → AUTH_RFID_DELETE_SCAN
User nhấn # → Thoát về IDLE
```

---

## 💳 4. checkRFID() - Xử Lý Thẻ RFID

### Flow:

```
state == SYS_AUTH? (NO → return)
    ↓ YES
authMode == AUTH_CHANGE_PASS? (YES → return, không cho scan)
    ↓ NO
Có thẻ mới? (rfid.PICC_IsNewCardPresent())
    ↓ YES
Đọc được UID? (rfid.PICC_ReadCardSerial())
    ↓ YES
authMode là gì?
    │
    ├─ AUTH_RFID_ADD_SCAN
    │   → findCardIndex() → Đã có? → "Already exists!"
    │   → cardCount >= 5? → "Memory full!"
    │   → Còn chỗ → saveCurrentCardAsAllowed() → "RFID Added [X/5]"
    │
    ├─ AUTH_RFID_DELETE_SCAN
    │   → findCardIndex() → Tìm thấy? → deleteCurrentCard() → "Deleted [X/5]"
    │                      → Không? → "Not Found"
    │
    └─ AUTH_ENTER_PASS (normal auth)
        → currentCardMatchesAllowed() → Match? → openDoor()
                                      → No? → "Access Denied"
```

### Multi-card storage:

```cpp
byte allowedUIDs[5][4];  // Array 2D: 5 cards, mỗi card 4 bytes
byte cardCount = 0;      // Số card đang lưu

// Tìm card trong array
int findCardIndex(byte uid[4]) {
  for (byte i = 0; i < cardCount; i++) {
    bool match = true;
    for (byte j = 0; j < 4; j++) {
      if (allowedUIDs[i][j] != uid[j]) {
        match = false;
        break;
      }
    }
    if (match) return i;  // Tìm thấy
  }
  return -1;  // Không tìm thấy
}

// Thêm card mới
void saveCurrentCardAsAllowed() {
  if (findCardIndex(rfid.uid.uidByte) >= 0) return;  // Đã có rồi
  if (cardCount >= MAX_RFID_CARDS) return;           // Full
  
  for (byte i = 0; i < 4; i++) {
    allowedUIDs[cardCount][i] = rfid.uid.uidByte[i];
  }
  cardCount++;
}

// Xóa card
void deleteCurrentCard() {
  int index = findCardIndex(rfid.uid.uidByte);
  if (index < 0) return;  // Không tìm thấy
  
  // Shift array: [A,B,C,D,E] → xóa B → [A,C,D,E,_]
  for (byte i = index; i < cardCount - 1; i++) {
    for (byte j = 0; j < 4; j++) {
      allowedUIDs[i][j] = allowedUIDs[i + 1][j];
    }
  }
  cardCount--;
}
```

---

## 📱 5. checkBluetooth() - Nhận Lệnh Từ App

### Protocol:

```
App gửi → HC-05 → Serial → Arduino

Commands:
  SWITCH1_ON  → Mở cửa (nếu đang đóng)
  SWITCH1_OFF → Mở cửa (nếu đang đóng)
  SWITCH2_ON  → Đóng cửa (nếu đang mở)
  SWITCH2_OFF → Đóng cửa (nếu đang mở)

Arduino phản hồi:
  "DOOR OPEN"   → App nhận biết cửa đã mở
  "DOOR CLOSED" → App nhận biết cửa đã đóng
  "OK_OPEN"     → Xác nhận đã thực hiện lệnh mở
  "OK_CLOSE"    → Xác nhận đã thực hiện lệnh đóng
```

### Code:

```cpp
void checkBluetooth() {
  if (!Serial.available()) return;
  
  String command = Serial.readStringUntil('\n');
  command.trim();  // Xóa whitespace/newline
  
  if (command == "SWITCH1_ON" || command == "SWITCH1_OFF") {
    if (state != SYS_DOOR_OPEN) {
      openDoor();
      Serial.println("OK_OPEN");  // Feedback cho app
    }
  } else if (command == "SWITCH2_ON" || command == "SWITCH2_OFF") {
    if (state == SYS_DOOR_OPEN) {
      closeDoor();
      Serial.println("OK_CLOSE");
    }
  }
}
```

---

## 🔘 6. checkInsideButton() - Nút Nhấn Trong Nhà

```
Đọc digitalRead(INSIDE_BUTTON)
    ↓
Debouncing: Chỉ xử lý khi:
  1. Button state thay đổi từ HIGH → LOW
  2. Đã qua 300ms từ lần nhấn trước
    ↓
state == SYS_DOOR_OPEN? 
    │ YES → closeDoor()
    │ NO  → openDoor()
```

### Code:

```cpp
void checkInsideButton() {
  static bool lastState = HIGH;         // Trạng thái trước
  static unsigned long lastPress = 0;   // Thời điểm nhấn trước
  
  bool s = digitalRead(INSIDE_BUTTON);
  
  // Detect falling edge + debounce 300ms
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
```

---

## 🔓 openDoor() - Mở Cửa

```cpp
void openDoor() {
  if (state == SYS_DOOR_OPEN) return;  // Đã mở rồi
  
  // 1. Điều khiển servo
  doorServo.attach(SERVO_PIN);  // Cấp điện cho servo
  doorServo.write(OPEN_POS);    // Quay 90 độ = mở
  
  // 2. Cập nhật UI
  showLine0("Door OPEN");
  clearLine(1);
  
  // 3. Gửi status cho app
  Serial.print("DOOR OPEN");
  
  // 4. Cập nhật state
  doorTimer = millis();
  state = SYS_DOOR_OPEN;
  authMode = AUTH_ENTER_PASS;  // Reset về mode default
  resetInput();
  resetNewPass();
}
```

**Lưu ý:** Servo KHÔNG detach khi mở (cần giữ lực để giữ cửa mở)

---

## 🔒 closeDoor() - Đóng Cửa

```cpp
void closeDoor() {
  if (state == SYS_IDLE) return;  // Đã đóng rồi
  
  // 1. Điều khiển servo
  doorServo.write(LOCK_POS);   // Quay về 0 độ = khóa
  delay(400);                  // Đợi servo quay xong
  doorServo.detach();          // TẮT servo để tiết kiệm điện
  
  // 2. Cập nhật UI
  showLine0("Door CLOSED");
  showWaiting();
  
  // 3. Gửi status cho app
  Serial.print("DOOR CLOSED");
  
  // 4. Cập nhật state
  state = SYS_IDLE;
}
```

**Lưu ý:** Servo detach sau khi đóng → tiết kiệm điện + giảm nhiễu servo

---

## 📊 Timeline Example: User Opens Door

```
Time | Event                        | State          | LCD Display
-----|------------------------------|----------------|------------------
0ms  | System boot                  | SYS_IDLE       | Door CLOSED
     |                              |                | Waiting...
-----|------------------------------|----------------|------------------
1s   | Person approaches (8cm)      | SYS_AUTH       | Door CLOSED
     | → checkPresence() triggers   | AUTH_ENTER_PASS| Password: ____
-----|------------------------------|----------------|------------------
2s   | User presses: 1,2,3,4        | SYS_AUTH       | Door CLOSED
     |                              | AUTH_ENTER_PASS| Password: ****
-----|------------------------------|----------------|------------------
3s   | User presses: #              | SYS_DOOR_OPEN  | Door OPEN
     | → openDoor() called          |                | 
     | → Servo: 0° → 90°            |                |
-----|------------------------------|----------------|------------------
4s   | Person walks through         | SYS_DOOR_OPEN  | Door OPEN
     | distance = 5cm               |                |
     | → lastSeen updated           |                |
-----|------------------------------|----------------|------------------
5s   | Person leaves                | SYS_DOOR_OPEN  | Door OPEN
     | distance = 50cm              |                |
-----|------------------------------|----------------|------------------
8s   | 3 seconds passed             | SYS_IDLE       | Door CLOSED
     | → checkDoorTimer() triggers  |                | Waiting...
     | → closeDoor() called         |                |
     | → Servo: 90° → 0°            |                |
```

---

## 🔄 Complete State Transition Diagram

```
                    ┌──────────────────┐
                    │    SYS_IDLE      │
                    │ Door CLOSED      │
                    │ Waiting...       │
                    └────────┬─────────┘
                             │
                   Person detected (<10cm)
                             │
                    ┌────────▼─────────┐
                    │    SYS_AUTH      │
                    │ AUTH_ENTER_PASS  │◄───────┐
                    │ Password: ____   │        │
                    └────────┬─────────┘        │
                             │                  │
              ┌──────────────┼──────────────┐   │
              │              │              │   │
         [Pass + #]     [Pass + *]    [RFID]   │
              │              │              │   │
              │     ┌────────▼─────────┐    │   │
              │     │    SYS_AUTH      │    │   │
              │     │ AUTH_CHANGE_PASS │    │   │
              │     │ New Pass: ____   │    │   │
              │     └────────┬─────────┘    │   │
              │              │              │   │
              │      ┌───────┼───────┐      │   │
              │      │       │       │      │   │
              │  [# save] [* menu]  │      │   │
              │      │       │       │      │   │
              │      │  ┌────▼──────────┐   │   │
              │      │  │   SYS_AUTH    │   │   │
              │      │  │AUTH_RFID_MENU │   │   │
              │      │  │ RFID [X/5]    │   │   │
              │      │  │1:Add 2:Delete │   │   │
              │      │  └────┬──────────┘   │   │
              │      │       │              │   │
              │      │  ┌────┼────┐         │   │
              │      │  │    │    │         │   │
              │      │ [1] [2]  [#]         │   │
              │      │  │    │    │         │   │
              │      │  │    │    └─────────┼───┘
              │      │  │    │              │
              │      │ Add Delete           │
              │      │  │    │              │
              │      │  └────┴────┐         │
              │      │            │         │
              │      └────────────┼─────────┘
              │                   │
              │                   │ [Scan complete]
              │                   │
              └───────────────────┴───────────┐
                                              │
                                    ┌─────────▼─────────┐
                                    │  SYS_DOOR_OPEN    │
                                    │  Door OPEN        │
                                    │                   │
                                    └─────────┬─────────┘
                                              │
                              No person for 3s OR button/BT
                                              │
                                    ┌─────────▼─────────┐
                                    │    SYS_IDLE       │
                                    │  Door CLOSED      │
                                    │  Waiting...       │
                                    └───────────────────┘
```

---

## 🎯 Key Design Decisions

### 1. **Servo Power Management**
- **Attach** khi cần điều khiển
- **Detach** khi idle → tiết kiệm điện + giảm nhiễu

### 2. **Presence-Based Logic**
- `lastSeen` được update liên tục khi có người
- Timeout chỉ áp dụng khi người đi ra ngoài

### 3. **State Isolation**
- Mỗi `check*()` function chỉ xử lý khi ở đúng state
- Tránh xung đột giữa các input sources

### 4. **Multi-Card Storage**
- Array 2D: `allowedUIDs[5][4]`
- Linear search: O(n), acceptable vì n=5
- Shift array khi delete để tránh gaps

### 5. **LCD Buffer Management**
- Tất cả messages ≤16 chars
- `clearLine()` trước khi ghi mới
- Cursor positioning chính xác

### 6. **Debouncing**
- Button: 300ms
- Ultrasonic: 200ms between reads
- Keypad: hardware debounced by library

---

## ⚡ Performance Characteristics

| Metric | Value |
|--------|-------|
| Loop frequency | ~1000-5000 Hz |
| Ultrasonic read | Every 200ms |
| RFID scan | ~50ms per attempt |
| Keypad scan | ~10ms per loop |
| Servo movement | ~500ms (0°→90°) |
| LCD refresh | <10ms per write |

**RAM usage:**
- Global variables: ~200 bytes
- RFID cards: 20 bytes (5×4)
- Stack: ~50 bytes per function
- **Total: ~500-600 bytes** (safe for Uno's 2KB RAM)

---

## 🐛 Edge Cases Handled

✅ **Multiple button presses** → Debouncing  
✅ **Duplicate RFID add** → "Already exists!"  
✅ **Memory full** → "Memory full!" rejection  
✅ **Person stays at door** → No auto-close  
✅ **Password too short** → Validation on # and *  
✅ **Timeout during menu** → Only in AUTH_ENTER_PASS  
✅ **Door already open/closed** → Early return  
✅ **Servo power management** → Attach/detach cycle  

---

## 📝 Maintenance Tips

1. **Thay đổi số lượng cards:**
   ```cpp
   #define MAX_RFID_CARDS 10  // Từ 5 → 10
   byte allowedUIDs[10][4];   // Update array size
   ```

2. **Điều chỉnh timeouts:**
   ```cpp
   #define DOOR_TIMEOUT 5000       // 3s → 5s
   #define PRESENCE_TIMEOUT 10000  // 3s → 10s
   ```

3. **Thêm buzzer feedback:**
   ```cpp
   void openDoor() {
     // ... existing code ...
     tone(BUZZER_PIN, 1000, 200);  // Beep khi mở
   }
   ```

4. **Log to SD card:**
   ```cpp
   void openDoor() {
     // ... existing code ...
     logToSD("OPEN," + String(millis()));
   }
   ```

---

**Tài liệu này giải thích 100% luồng hoạt động của code.**  
**Bất kỳ câu hỏi nào về flow, cứ hỏi mình! 🚀**
