# Phân Tích Vấn Đề Bảo Mật - Ultrasonic Detection

## 🔍 Cơ Chế Kiểm Tra Có Người (Hiện Tại)

### Hardware: HC-SR04 Ultrasonic Sensor

```
TRIG pin → Phát xung siêu âm (40kHz)
   ↓
Sóng siêu âm bay ra → chạm vật thể → phản xạ
   ↓
ECHO pin → Nhận sóng phản xạ → tính thời gian
   ↓
distance (cm) = (duration × 0.034) / 2
```

**Góc phát sóng:** ~15-30 độ (cone shape)  
**Tầm phát hiện:** 2cm - 400cm  
**Độ chính xác:** ±3mm (ở khoảng cách ngắn)

### Code Logic:

```cpp
void updateDistance() {
  if (millis() - lastSensorRead < 200) return;  // Đọc mỗi 200ms
  lastSensorRead = millis();
  distance = readUltrasonic();
}

void checkPresence() {
  updateDistance();
  
  if (state == SYS_DOOR_OPEN) {
    if (distance < PERSON_DISTANCE) {  // < 10cm
      lastSeen = millis();  // ⚠️ UPDATE LIÊN TỤC
    }
    return;
  }
  
  // ... auth logic
}

void checkDoorTimer() {
  if (state != SYS_DOOR_OPEN) return;
  
  // CHỈ đóng cửa khi: KHÔNG có người + đã 3s
  if (distance >= PERSON_DISTANCE && 
      millis() - lastSeen > DOOR_TIMEOUT) {
    closeDoor();
  }
}
```

**Ý tưởng:**
- Người đứng gần (<10cm) → `lastSeen` liên tục reset
- Người đi xa (≥10cm) → sau 3s đóng cửa

---

## 🚨 LỖ HỔNG BẢO MẬT NGHIÊM TRỌNG

### Scenario: Tailgating Attack (Đi theo sau)

```
Timeline:

0s   User đứng trước cửa (distance = 8cm)
     → Nhập password → Cửa mở
     
1s   User đi qua cửa, ra ngoài (distance = 50cm)
     lastSeen = 1000ms
     
2s   Kẻ xấu đứng ngay cửa (distance = 5cm) ⚠️
     lastSeen = 2000ms ← RESET!
     
3s   Kẻ xấu vẫn đứng đó (distance = 5cm)
     lastSeen = 3000ms ← RESET!
     
4s   Kẻ xấu đi vào nhà (distance = 5cm → 15cm → 30cm)
     lastSeen = 4000ms
     
7s   Kẻ xấu đã vào, ra khỏi tầm sensor (distance = 200cm)
     
10s  Sau 3s không phát hiện người → Cửa đóng
     ❌ KẺ XẤU ĐÃ VÀO NHÀ!
```

### Vấn Đề:

**Ultrasonic sensor KHÔNG PHÂN BIỆT:**
- ❌ Ai là người vừa mở cửa
- ❌ Người đi vào hay đi ra
- ❌ Bao nhiêu người (1 hay nhiều người)

**Chỉ biết:** Có vật thể <10cm hay không

---

## 🛡️ Giải Pháp

### Option 1: Fixed Maximum Timeout ⭐ ĐANG ÁP DỤNG (CẦN THÊM)

**Ý tưởng:** Đóng cửa sau X giây bất kể có người hay không

```cpp
#define DOOR_TIMEOUT 3000        // Timeout khi không có người
#define DOOR_MAX_TIMEOUT 10000   // Timeout cứng tối đa

void checkDoorTimer() {
  if (state != SYS_DOOR_OPEN) return;
  
  // Force close sau 10s dù có người hay không
  if (millis() - doorTimer > DOOR_MAX_TIMEOUT) {
    closeDoor();
    return;
  }
  
  // Normal close: không có người 3s
  if (distance >= PERSON_DISTANCE && 
      millis() - lastSeen > DOOR_TIMEOUT) {
    closeDoor();
  }
}
```

**Ưu điểm:**
✅ Đơn giản, không cần hardware thêm  
✅ Chắc chắn đóng cửa sau 10s  

**Nhược điểm:**
⚠️ Nếu người chủ chậm chạp → cửa đóng vào mặt  
⚠️ Vẫn có 10s để kẻ xấu chui vào  

---

### Option 2: Dual Sensor (Direction Detection) ⭐⭐ TỐT NHẤT

**Hardware cần thêm:** 1 ultrasonic sensor nữa

```
     [Outside]        [Inside]
         │                │
    Sensor OUT       Sensor IN
         │                │
    ─────┼────[ DOOR ]────┼─────
```

**Logic:**
```cpp
bool personPassedOutward = false;
bool personPassedInward = false;

void checkDirection() {
  if (distanceOut < 10 && distanceIn >= 10) {
    // Người ở phía ngoài
    personOutside = true;
  }
  
  if (distanceOut >= 10 && distanceIn < 10) {
    // Người đang đi vào
    if (personOutside) {
      personPassedInward = true;  // ⚠️ CẢNH BÁO: Có người vào!
    } else {
      personPassedOutward = true; // ✅ Chủ nhà đi ra
    }
  }
}

void checkDoorTimer() {
  if (personPassedOutward && distanceOut >= 10 && distanceIn >= 10) {
    // Chủ nhà đi ra rồi + không có ai ở cửa → đóng
    closeDoor();
  }
  
  if (personPassedInward) {
    // CẢNH BÁO: Có người vào sau chủ nhà!
    // Option: Khóa ngay, báo động, gửi alert
    triggerAlarm();
  }
}
```

**Ưu điểm:**
✅ Phân biệt được chiều di chuyển  
✅ Phát hiện tailgating  
✅ Có thể count số người  

**Nhược điểm:**
❌ Cần hardware thêm (~$2)  
❌ Code phức tạp hơn  

---

### Option 3: Single Pass Mode ⭐⭐

**Ý tưởng:** Chỉ cho 1 người đi qua, sau đó đóng ngay

```cpp
bool doorUsed = false;  // Cờ đánh dấu đã có người qua

void openDoor() {
  // ... existing code ...
  doorUsed = false;  // Reset cờ
}

void checkPresence() {
  updateDistance();
  
  if (state == SYS_DOOR_OPEN) {
    if (distance < PERSON_DISTANCE) {
      lastSeen = millis();
      doorUsed = true;  // Đánh dấu đã có người
    }
    return;
  }
}

void checkDoorTimer() {
  if (state != SYS_DOOR_OPEN) return;
  
  // Đóng ngay sau khi người đi qua (3s sau khi rời khỏi sensor)
  if (doorUsed && 
      distance >= PERSON_DISTANCE && 
      millis() - lastSeen > DOOR_TIMEOUT) {
    closeDoor();
  }
  
  // Force close nếu chưa dùng sau 10s (lỗi/cancel)
  if (!doorUsed && millis() - doorTimer > 10000) {
    closeDoor();
  }
}
```

**Ưu điểm:**
✅ Không cần hardware thêm  
✅ Đơn giản  
✅ Đóng nhanh sau khi dùng  

**Nhược điểm:**
⚠️ Nếu user quên đồ, quay lại lấy → phải mở cửa lại  
⚠️ Vẫn không phát hiện tailgating trong lúc người đi qua  

---

### Option 4: PIR Motion Sensor Inside ⭐⭐⭐ AN TOÀN NHẤT

**Hardware:** Thêm PIR motion sensor bên trong nhà

```
[Outside]              [Inside]
    │                     │
Ultrasonic            PIR Motion
    │                     │
────┼────[ DOOR ]────────┼────
```

**Logic:**
```cpp
void checkSecurity() {
  if (state == SYS_DOOR_OPEN) {
    
    // User đã đi ra ngoài (ultrasonic không phát hiện)
    if (distance >= PERSON_DISTANCE) {
      
      // Nếu PIR phát hiện chuyển động trong nhà
      if (digitalRead(PIR_PIN) == HIGH) {
        // ⚠️ CÓ NGƯỜI TRONG NHÀ MÀ KHÔNG PHẢI CHỦ!
        triggerAlarm();
        closeDoorImmediately();
        lockSystem();
      } else {
        // Không có ai trong nhà → đóng cửa bình thường
        if (millis() - lastSeen > DOOR_TIMEOUT) {
          closeDoor();
        }
      }
    }
  }
}
```

**Ưu điểm:**
✅ Phát hiện người TRONG NHÀ  
✅ Phân biệt chủ đi ra vs kẻ xấu vào  
✅ Có thể trigger alarm  

**Nhược điểm:**
❌ Cần hardware PIR (~$1)  
⚠️ PIR delay ~2s (có thể miss người chạy nhanh)  

---

## 📊 So Sánh Giải Pháp

| Giải pháp | Chi phí | Độ phức tạp | Bảo mật | Khuyến nghị |
|-----------|---------|-------------|---------|-------------|
| Fixed Max Timeout | $0 | ⭐ | ⭐⭐ | Tạm thời OK |
| Dual Ultrasonic | $2 | ⭐⭐⭐ | ⭐⭐⭐⭐ | Tốt |
| Single Pass | $0 | ⭐⭐ | ⭐⭐⭐ | OK cho gia đình |
| PIR Inside | $1 | ⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | **TỐT NHẤT** |

---

## 🎯 KHUYẾN NGHỊ

### Ngắn hạn (Không thêm hardware):
✅ **Áp dụng Option 1 + Option 3:** Fixed timeout + Single pass

```cpp
#define DOOR_MAX_TIMEOUT 10000  // 10s tối đa

bool doorUsed = false;

void checkDoorTimer() {
  if (state != SYS_DOOR_OPEN) return;
  
  // Force close sau 10s
  if (millis() - doorTimer > DOOR_MAX_TIMEOUT) {
    closeDoor();
    return;
  }
  
  // Single pass: đóng ngay sau khi người qua
  if (doorUsed && 
      distance >= PERSON_DISTANCE && 
      millis() - lastSeen > DOOR_TIMEOUT) {
    closeDoor();
  }
}
```

### Dài hạn (Thêm hardware):
✅ **Áp dụng Option 4:** PIR motion sensor inside

- Giá rẻ ($1)
- Dễ setup
- Bảo mật cao nhất

---

## 🔐 Tổng Kết

**Vấn đề hiện tại:**
❌ Kẻ xấu có thể tailgate (đi theo sau) vào nhà trong vòng 3-10s

**Giải pháp tốt nhất:**
✅ **Ngay:** Thêm fixed max timeout (10s)  
✅ **Lâu dài:** Thêm PIR sensor bên trong để phát hiện intrusion

**Bạn muốn mình implement giải pháp nào?**
1. Fixed max timeout (10s) - Nhanh, không cần hardware
2. PIR sensor inside - Cần thêm 1 sensor, bảo mật cao
