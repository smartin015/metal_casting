#include <Arduino.h>

#include <Tone32.h>
#include <TFT_eSPI.h> 
#include <SPI.h>
#include "WiFi.h"
#include <Wire.h>
#include <Button2.h>
#include <analogWrite.h>

#ifndef TFT_DISPOFF
#define TFT_DISPOFF 0x28
#endif

#ifndef TFT_SLPIN
#define TFT_SLPIN   0x10
#endif

TFT_eSPI tft = TFT_eSPI(135, 240); // Invoke custom library

#define BUZZER_PIN 25
#define BUZZER_CHANNEL 0
#define MOTOR_PIN 26
#define MOTOR_CHANNEL 1
#define INDUCTION_PIN 33

#define O2_RX 37
#define O2_TX 38

typedef enum {
  BUTTON_1,
  BUTTON_2,
  BUTTON_NUDGE,
  BUTTON_ARM, 
  BUTTON_LID,
  BUTTON_SPIN,
  BUTTON_INDUCTION_OFF,
  BUTTON_INDUCTION_ON,
  BUTTON_INDUCTION_PULSE,
  NUM_BUTTONS
} button_t;
const int BUTTON_PINS[NUM_BUTTONS] = {
  35, // BUTTON_1
  0, // BUTTON_2
  21, // BUTTON_NUDGE
  17, // BUTTON_ARM
  15, // BUTTON_LID
  12, // BUTTON_SPIN
  32, // BUTTON_INDUCTION_OFF
  27, // BUTTON_INDUCTION_ON
  22, // BUTTON_INDUCTION_PULSE
};
Button2 button[NUM_BUTTONS]; 

char buff[512];
int vref = 1100;


typedef enum {
  STATE_UNKNOWN,
  STATE_INIT,
  STATE_DISARMED,
  STATE_NUDGE,
  STATE_LID_OPEN,
  STATE_SPIN_LATCHED,
  STATE_RESET_ARM, // Don't just jump to armed state when spin latch pressed
  STATE_ARMED,
  STATE_HEATING,
  STATE_PULSING,
  STATE_SPINNING,
  STATE_SPINDOWN,
  NUM_STATES
} state_t;

state_t state = STATE_UNKNOWN;


struct o2_state_t {
  uint16_t conc_raw;
  float concentration;
  bool valid = false;
  uint16_t errors = 0;
  uint64_t last_data = 0;
} o2_state;

bool buzzing = false;
void buzz(int freq = 0) {
  if (freq > 0) {
    if (buzzing) {
      noTone(BUZZER_PIN, BUZZER_CHANNEL);
    }
    tone(BUZZER_PIN, freq, 0, BUZZER_CHANNEL);
    buzzing = true;
  } else {
    if (buzzing) {
      noTone(BUZZER_PIN, BUZZER_CHANNEL);
    }
    buzzing = false;
  }
}

uint64_t buzztime = 0;
bool buzzSpinStart = false;
bool buzzButtonPress = false;
void buzzerLoop(uint64_t now, state_t s) {
  if (buzzButtonPress && !buzztime) {
    buzz(500);
    buzztime = now + 100;
    buzzButtonPress = false;
  } else if (s == STATE_ARMED) {
    int i = (now / 50) % 20;
    if (i == 0 && !buzzing) {
      buzz(1000);
    } else if (i == 1 && buzzing) {      
      buzz(0);
    }
  } else if (buzzSpinStart && !buzztime) {
    buzz(2000);
    buzztime = now + 1000;
    buzzSpinStart = false;
  } else if (buzztime && now > buzztime) {
    buzz(0);
    buzztime = 0;
  }
}

String lastText = "";
float lastConc = 0;
bool lastValid = false;
int lastErrs = 0;
void showState(state_t s) {
  int16_t now2s = millis() % 2000;
  bool s1 = now2s < 1000;
  String text;
  auto bgcolor = TFT_BLACK;
  auto color = TFT_WHITE;
  int textsize = 8;
  switch (s) {
    case STATE_UNKNOWN:
      text = (s1) ? "ERROR" : "STATE";
      break;
    case STATE_INIT:
      text = (s1) ? "PLEASE" : "DISARM";
      textsize = 5;
      color = TFT_ORANGE;
      break;
    case STATE_DISARMED:
      text = "DISARMED";
      textsize = 5;
      color = TFT_DARKGREY;
      break;
    case STATE_NUDGE:
      text = "NUDGING"; // TODO "nudge nudge"
      textsize = 5;
      color = TFT_SKYBLUE;
      break;
    case STATE_LID_OPEN:
      text = (s1) ? "CLOSE": "LID";
      color = TFT_ORANGE;
      break;
    case STATE_SPIN_LATCHED:
      text = (s1) ? "DROP &": "REARM";
      color = TFT_ORANGE;
      break;
    case STATE_ARMED:
      text = (s1) ? "ARMED": "READY";
      color = TFT_RED;
      break;
    case STATE_HEATING:
      text = (s1) ? "ARMED": "HEAT";
      color = TFT_RED;
      break;
    case STATE_PULSING:
      text = (s1) ? "ARMED": "PULSE";
      color = TFT_RED;
      break;
    case STATE_SPINNING:
      text = (now2s/250 % 2) ? "SPIN!" : "";
      color = TFT_SKYBLUE;
      break;
    case STATE_SPINDOWN:
      text = (s1) ? "SPIN" : "DOWN";
      color = TFT_ORANGE;
      break;
    default:
      text = (s1) ? "ERR": "IMPL";
      break;
  }

  bool o2_late = (o2_state.last_data < millis() - 2000);

  if (text != lastText 
      || o2_state.concentration != lastConc 
      || o2_state.valid != lastValid 
      || o2_state.errors != lastErrs
      || o2_late
    ) {
    // State Text
    tft.setTextWrap(true);
    tft.setCursor(0, 0);
    tft.setTextColor(color);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(textsize);
    tft.fillScreen(bgcolor);
    tft.drawString(text,  tft.width()/2, tft.height()/2);
    lastText = text;

    // O2 State
    char buf[64];
    sprintf(buf, "O2 %.1f", o2_state.concentration);
    text = buf;
    text += "%";
    if (!o2_state.valid) {
     sprintf(buf, " CKERR %d", o2_state.errors);
     text += buf;
     tft.setTextColor(TFT_RED);
    } else if (o2_late) {
     sprintf(buf, " ENORX %d", (millis() - o2_state.last_data)/1000);
     text += buf;
     tft.setTextColor(TFT_RED);
    }
    tft.setTextSize(2);
    tft.drawString(text, tft.width()/2, tft.height()*7/8);
    lastConc = o2_state.concentration;
    lastValid = o2_state.valid;
    lastErrs = o2_state.errors;
  }
}

state_t calcState(state_t cur) {
  switch (cur) {
    case STATE_INIT:
      // We need to start in a good button state; don't just arm & spin on init
      if (!button[BUTTON_ARM].isPressed()) {
        return STATE_DISARMED; 
      }
      return cur;
    case STATE_DISARMED:
      if (button[BUTTON_ARM].isPressed()) {
        if (!button[BUTTON_LID].isPressed()) {
          return STATE_LID_OPEN;
        } else if (button[BUTTON_SPIN].isPressed()) {
          return STATE_SPIN_LATCHED;
        } else {
          return STATE_ARMED;
        }
      } else if (button[BUTTON_NUDGE].isPressed()) { 
        return STATE_NUDGE;
      }
      return cur;
    case STATE_NUDGE:
      if (!button[BUTTON_NUDGE].isPressed()) {
        return STATE_DISARMED;
      }
      return cur;
    case STATE_LID_OPEN:
      if (!button[BUTTON_ARM].isPressed()) {
        return STATE_DISARMED;
      } else if (button[BUTTON_LID].isPressed()) {
        if (button[BUTTON_SPIN].isPressed()) {
          return STATE_SPIN_LATCHED;
        } else {
          return STATE_ARMED;
        }
      }
      return cur;
    case STATE_SPIN_LATCHED:
      if (!button[BUTTON_ARM].isPressed()) {
        return STATE_DISARMED;
      }
      return cur;
    case STATE_ARMED: // case overflow intended
    case STATE_HEATING: // case overflow intended
    case STATE_PULSING:
      if (!button[BUTTON_ARM].isPressed()) {
        return STATE_DISARMED;
      } else if (!button[BUTTON_LID].isPressed()) {
        return STATE_LID_OPEN;
      } else if (button[BUTTON_SPIN].isPressed()) {
        buzzSpinStart = true;
        return STATE_SPINNING;
      } else if (button[BUTTON_INDUCTION_OFF].isPressed()) {
        return STATE_ARMED;
      } else if (button[BUTTON_INDUCTION_ON].isPressed()) {
        return STATE_HEATING;
      } else if (button[BUTTON_INDUCTION_PULSE].isPressed()) {
        return STATE_PULSING;
      }
      return cur; // Maintain current state
    case STATE_SPINNING:
      return cur; // Spindown is handled in motorLoop()
    case STATE_SPINDOWN:
      if (!button[BUTTON_LID].isPressed() && !button[BUTTON_ARM].isPressed()) {
        return STATE_DISARMED;
      }
      return cur;
    default:
      return STATE_UNKNOWN;
  }
}

uint64_t stateStart = 0;
void generalButtonHandler(Button2& b) {
  state_t next = calcState(state);
  if (next != state) {
    state = next;
    stateStart = millis();
  }
  if (b.isPressed() && b.getAttachPin() != BUTTON_PINS[BUTTON_LID] && b.getAttachPin() != BUTTON_PINS[BUTTON_SPIN]) {
    buzzButtonPress = true;
  }
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Start");

    o2_state.concentration = 0;
    o2_state.errors = 0;
    o2_state.valid = false;

    // Grove O2 sensor serial
    Serial2.begin(9600, SERIAL_8N1, O2_RX, O2_TX);


    tft.init();
    tft.setRotation(3);
    if (TFT_BL > 0) { // TFT_BL has been set in the TFT_eSPI library in the User Setup file TTGO_T_Display.h
         pinMode(TFT_BL, OUTPUT); // Set backlight pin to output mode
         digitalWrite(TFT_BL, TFT_BACKLIGHT_ON); // Turn backlight on. TFT_BACKLIGHT_ON has been set in the TFT_eSPI library in the User Setup file TTGO_T_Display.h
    }
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(MOTOR_PIN, OUTPUT);
    pinMode(INDUCTION_PIN, OUTPUT);
    digitalWrite(INDUCTION_PIN, LOW);
    ledcSetup(MOTOR_CHANNEL, 5000, 8);
    ledcWrite(MOTOR_CHANNEL, 0);
    for (int i = 0; i < NUM_BUTTONS; i++) {
      button[i].begin(BUTTON_PINS[i], INPUT_PULLUP, /* isCapacitive */ false, /* activeLow */ (i != BUTTON_LID && i != BUTTON_SPIN));

      if (i != BUTTON_1 && i != BUTTON_2) {
        button[i].setChangedHandler(&generalButtonHandler);
      }
    }

    button[BUTTON_1].setPressedHandler([](Button2 & b) {
        Serial.println("TODO btn1");
    });

    button[BUTTON_2].setPressedHandler([](Button2 & b) {
        Serial.println("TODO btn2");
    });



    tft.setCursor(0, 0);
    tft.setTextDatum(MC_DATUM);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(4);
    tft.drawString("Spincaster",  tft.width()/2, tft.height()*1/4);
    tft.drawString("v1.1", tft.width()/2, tft.height()/2);
    tft.setTextSize(2);
    tft.drawString("\"Literally ",  tft.width()/2, tft.height()*3/4);
    tft.drawString(" Revolutionary\"",  tft.width()/2, tft.height()*7/8);
    buzz(880);
    delay(500);
    buzz(0);
    delay(4000);

    state = STATE_INIT;
}

auto curInd = LOW;
void inductionLoop(uint64_t now, state_t state) {
  auto next = LOW;
  switch (state) {
    case STATE_HEATING:
      next = HIGH;
      break;
    case STATE_PULSING:
      next = (((now-stateStart) / 1000) % 2) ? HIGH : LOW;
      break;
    default:
      next = LOW;
      break;
  }
  if (next != curInd) {
    digitalWrite(INDUCTION_PIN, next);
    curInd = next;
  }
}

int curMot = 0;
void motorLoop(uint64_t now) {
  auto next = 0;
  if (state == STATE_NUDGE) {
    next = ((((now-stateStart) / 10) % 256) == 0 ? 255 : 0);
    //next = 1;
  }
  if (state == STATE_SPINNING) {
    if (now < stateStart + 2000) {
      next = 255;
    } else {
      state = STATE_SPINDOWN;
    }
  }
  if (next != curMot) {
    if (next) {
      //ledcAttachPin(MOTOR_PIN, MOTOR_CHANNEL);
      //ledcWrite(MOTOR_CHANNEL, next);
      digitalWrite(MOTOR_PIN, HIGH);
    } else {
      //ledcDetachPin(MOTOR_PIN);
      digitalWrite(MOTOR_PIN, LOW);
    }
    curMot = next;
  }
}

#define O2_BEGIN 255
#define O2_PACKET_LEN 9
#define O2_CMD_READ 0x86
uint8_t o2idx = O2_PACKET_LEN;
uint8_t o2buf[16];
void sensorLoop() {
  while (Serial2.available()) {
    uint8_t c = Serial2.read();
    if (o2idx >= O2_PACKET_LEN) {
      o2idx = (c == O2_BEGIN) ? 0 : O2_PACKET_LEN;
    }
    char hbuf[3];
    //sprintf(hbuf, "%d %02x", o2idx, c);
    //Serial.println(hbuf);
    o2buf[o2idx++] = c;
    if (o2idx == O2_PACKET_LEN) {
      // https://files.seeedstudio.com/wiki/Grove_Oxygen_Sensor_Pro/res/GGC2330-O2-1.0.pdf
      // The default communication type is active upload and it sends gas concentration every other one
      // second (the concentration is 16 hexadecimal)
      // ex. FF [86 00 00 00 00 00 00 7A] （concentration value is 0）
      // Gas concentration value = (gas concentration high byte *256 + gas concentration low byte) * resolution
      if (o2buf[1] != O2_CMD_READ) {
        Serial.print("Unsupported O2 command: ");
        Serial.println(o2buf[1]);
        continue;
      }
      o2_state.conc_raw = (o2buf[2] << 8) + o2buf[3];
      
      uint8_t checksum = 0;
      for (uint8_t i = 1; i < 8; i++) {
        checksum += o2buf[i];
      }
      checksum = (checksum ^ 0xFF)+1;
      
      char buf[64];
      if (checksum != o2buf[8]) {
        o2_state.errors++;
        o2_state.valid = false;
        sprintf(buf, "O2 CKFAIL want %02x got %02x (errs=%d)", o2buf[8], checksum, o2_state.errors);
      } else {
        o2_state.concentration = float(o2_state.conc_raw) * 0.1;
        o2_state.valid = true;
        sprintf(buf, "O2 %.1f %s E%d", o2_state.concentration, o2_state.valid ? "valid" : "invalid", o2_state.errors);
        o2_state.last_data = millis();
      }
      Serial.println(buf);
    }
  }
}

uint64_t last_update = 0;
void loop()
{
    for (int i = 0; i < NUM_BUTTONS; i++) {
      button[i].loop();
    }
    uint64_t now = millis();
    if (now > last_update + 250) {
      last_update = now;
      showState(state);
    }
    sensorLoop();
    buzzerLoop(now, state);
    inductionLoop(now, state);
    motorLoop(now); // mutates state when spindown time
}

