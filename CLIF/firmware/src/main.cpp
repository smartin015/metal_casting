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
#define VACUUM_PIN 26
#define PURGE_PIN 33
#define INDUCTION_PIN 2

typedef enum {
  BUTTON_VAC,
  BUTTON_PURGE, 
  BUTTON_HEAT,
  NUM_BUTTONS
} button_t;
const int BUTTON_PINS[NUM_BUTTONS] = {
  15, // BUTTON_VAC
  17, // BUTTON_PURGE
  21, // BUTTON_HEAT
};
Button2 button[NUM_BUTTONS]; 

char buff[512];
int vref = 1100;

typedef enum {
  STATE_UNKNOWN,
  STATE_READY,
  STATE_VACUUM,
  STATE_PURGE,
  STATE_HEATING,
  NUM_STATES
} state_t;

state_t state = STATE_UNKNOWN;

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
  } else if (s != STATE_READY) {
    int i = (now / 50) % 20;
    if (i == 0 && !buzzing) {
      buzz(1000);
    } else if (i == 1 && buzzing) {      
      buzz(0);
    }
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
    case STATE_READY:
      text = (s1) ? "IDLE": "READY";
      color = TFT_DARKGREY;
      break;
    case STATE_VACUUM:
      text = (s1) ? "VACUUM": "OPEN";
      color = TFT_RED;
      break;
    case STATE_PURGE:
      text = (s1) ? "PURGE": "OPEN";
      color = TFT_RED;
      break;
    case STATE_HEATING:
      text = (s1) ? "ARMED" : "HEAT";
      color = TFT_ORANGE;
      break;
    default:
      text = (s1) ? "ERR": "IMPL";
      break;
  }

  if (text != lastText) {
    // State Text
    tft.setTextWrap(true);
    tft.setCursor(0, 0);
    tft.setTextColor(color);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(textsize);
    tft.fillScreen(bgcolor);
    tft.drawString(text,  tft.width()/2, tft.height()/2);
    lastText = text;
  }
}

bool prev_vac = false;
bool prev_purge = false;
bool prev_heat = false;

state_t calcState(state_t cur, bool vac, bool purge, bool heat) {
  switch (cur) {
    case STATE_READY:
      if (vac && !prev_vac) {
        return STATE_VACUUM;
      } else if (purge && !prev_purge) { 
        return STATE_PURGE;
      } else if (heat && !prev_heat) {
        return STATE_HEATING;
      }
      return cur;
    case STATE_VACUUM:
      if (vac && !prev_vac) {
        return STATE_READY;
      } else if (purge && !prev_purge) {
        return STATE_PURGE;
      } else if (heat && !prev_heat) {
        return STATE_HEATING;
      }
      return cur;
    case STATE_PURGE:
      if (vac && !prev_vac) {
        return STATE_VACUUM;
      } else if (purge && !prev_purge) {
        return STATE_READY;
      } else if (heat && !prev_heat) {
        return STATE_HEATING;
      }
      return cur;
    case STATE_HEATING:
      // Ignore vacuum & purge commands while heating
      if (heat && !prev_heat) {
        return STATE_READY;
      }
      return cur;
    default:
      return STATE_UNKNOWN;
  }
}

uint64_t stateStart = 0;
void generalButtonHandler(Button2& b) {
  bool vac = button[BUTTON_VAC].isPressed();
  bool purge = button[BUTTON_PURGE].isPressed();
  bool heat = button[BUTTON_HEAT].isPressed();
  state_t next = calcState(state, vac, purge, heat);
  prev_vac = vac;
  prev_purge = purge;
  prev_heat = heat;
  if (next != state) {
    state = next;
    stateStart = millis();
  }
  if (b.isPressed()) {
    buzzButtonPress = true;
  }
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Start");

    tft.init();
    tft.setRotation(3);
    if (TFT_BL > 0) { // TFT_BL has been set in the TFT_eSPI library in the User Setup file TTGO_T_Display.h
         pinMode(TFT_BL, OUTPUT); // Set backlight pin to output mode
         digitalWrite(TFT_BL, TFT_BACKLIGHT_ON); // Turn backlight on. TFT_BACKLIGHT_ON has been set in the TFT_eSPI library in the User Setup file TTGO_T_Display.h
    }
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(VACUUM_PIN, OUTPUT);
    pinMode(PURGE_PIN, OUTPUT);
    pinMode(INDUCTION_PIN, OUTPUT);
    digitalWrite(INDUCTION_PIN, LOW);
    digitalWrite(VACUUM_PIN, LOW);
    digitalWrite(PURGE_PIN, LOW);
    for (int i = 0; i < NUM_BUTTONS; i++) {
      button[i].begin(BUTTON_PINS[i], INPUT_PULLUP, /* isCapacitive */ false, /* activeLow */ true);
      button[i].setChangedHandler(&generalButtonHandler);
    }

    tft.setCursor(0, 0);
    tft.setTextDatum(MC_DATUM);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(4);
    tft.drawString("EVIL-C",  tft.width()/2, tft.height()*1/4-4);
    tft.drawString("v0.1", tft.width()/2, tft.height()/2-4);
    tft.setTextSize(2);
    tft.drawString("Experimental",  tft.width()/2, tft.height()*3/4 - 16);
    tft.drawString("Vacuum-Induction",  tft.width()/2, tft.height()*3/4);
    tft.drawString("Levitation Caster",  tft.width()/2, tft.height()*3/4 + 16);
    buzz(880);
    delay(500);
    buzz(0);
    delay(4000);
    state = STATE_READY;
}

void outputLoop(uint64_t now, state_t state) {
  digitalWrite(INDUCTION_PIN, (state == STATE_HEATING) ? HIGH : LOW);
  digitalWrite(VACUUM_PIN, (state == STATE_VACUUM) ? HIGH : LOW);
  digitalWrite(PURGE_PIN, (state == STATE_PURGE) ? HIGH : LOW);
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
    buzzerLoop(now, state);
    outputLoop(now, state);
}

