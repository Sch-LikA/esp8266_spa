// https://github.com/ccutrer/balboa_worldwide_app/blob/master/doc/protocol.md
// Reference:https://github.com/ccutrer/balboa_worldwide_app/wiki

// Please install the needed dependencies:
// CircularBuffer
// PubSubClient

// TODO:
// HomeAssistant autodiscover - DONE
// Wi-Fi, MQTT, and OTA configuration through ignored secrets.h -> DONE
// Password-protected Wi-Fi firmware updates -> DONE
// Temperature setting with unit/range validation and confirmation -> DONE
// Proper states (rather than just ON/OFF) -> NOT SURE HOW TO SOLVE THIS
// Pumps 3-6, light 2, mister, and auxiliary devices -> TO DO
// Clock, preferences, lock state, reminders, and filter-cycle writes -> TO DO
// STA Mode to configure wifi -> WIP

// +12V RED
// GND  BLACK
// A    YELLOW
// B    WHITE
#if __has_include(<secrets.h>)
#include <secrets.h>
#else
#error "Copy include/secrets.h.example to include/secrets.h and configure your secrets."
#endif
#include <ESP8266WiFi.h>
#include <CircularBuffer.hpp>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>

#define VERSION "0.38.2"
#define OTA_HOSTNAME "spa"
#define DEBUG_BAUD 115200
#define SPA_RESTART_REASON_MAGIC 0x53504121
#define RESTART_REASON_NONE 0
#define RESTART_REASON_RS485_TIMEOUT 1
String WIFI_SSID = SPA_WIFI_SSID;
String WIFI_PASSWORD = SPA_WIFI_PASSWORD;
String BROKER = SPA_BROKER;
String BROKER_LOGIN = SPA_BROKER_LOGIN;
String BROKER_PASS = SPA_BROKER_PASS;
#define AUTO_TX true //if your chip needs to pull D1 high/low set this to false

#define STRON String("ON").c_str()
#define STROFF String("OFF").c_str()

//HomeAssistant autodiscover
#define HASSIO true
#define PRODUCTION true

#define TX485 D1  //find a way to skip this
#define RLY1  D7
#define RLY2  D8


CircularBuffer<uint8_t, 35> Q_in;
CircularBuffer<uint8_t, 35> Q_out;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
bool otaInProgress = false;
bool haveTemperatureScale = false;

extern uint8_t crc8();
extern void ID_request();
extern void ID_ack();
extern void rs485_send();
uint8_t x, i, j;
uint8_t last_state_crc = 0x00;
uint8_t send = 0x00;
uint8_t settemp = 0x00;
uint8_t requestedSetTemperature = 0xFF;
unsigned long temperatureRequestAt = 0;
uint8_t id = 0x00;
unsigned long lastrx = 0;
bool receivedSpaPacket = false;
unsigned long lastHealthLog = 0;
char have_config = 0; //stages: 0-> want it; 1-> requested it; 2-> got it; 3-> further processed it
char have_faultlog = 0; //stages: 0-> want it; 1-> requested it; 2-> got it; 3-> further processed it
char have_filtersettings = 0; //stages: 0-> want it; 1-> requested it; 2-> got it; 3-> further processed it
char faultlog_minutes = 0; //temp logic so we only get the fault log once per 5 minutes
char filtersettings_minutes = 0; //temp logic so we only get the filter settings once per 5 minutes
String restartReason;

struct {
  uint32_t magic;
  uint32_t reason;
} ResetState;

struct {
  uint8_t jet1 :2;
  uint8_t jet2 :2;
  uint8_t blower :1;
  uint8_t light :1;
  uint8_t restmode:1;
  uint8_t highrange:1;
  uint8_t padding :2;
  uint8_t hour :5;
  uint8_t minutes :6;
} SpaState;

struct {
  uint8_t pump1 :2; //this could be 1=1 speed; 2=2 speeds
  uint8_t pump2 :2;
  uint8_t pump3 :2;
  uint8_t pump4 :2;
  uint8_t pump5 :2;
  uint8_t pump6 :2;
  uint8_t light1 :1;
  uint8_t light2 :1;
  uint8_t circ :1;
  uint8_t blower :1;
  uint8_t mister :1;
  uint8_t aux1 :1;
  uint8_t aux2 :1;
  uint8_t temp_scale :1; //0 -> Farenheit, 1-> Celcius
} SpaConfig;

struct {
  uint8_t totEntry :5;
  uint8_t currEntry :5;
  uint8_t faultCode :6;
  String faultMessage;
  uint8_t daysAgo :8;
  uint8_t hour :5;
  uint8_t minutes :6;
} SpaFaultLog;

struct {
  uint8_t filt1Hour :5;
  uint8_t filt1Minute :6;
  uint8_t filt1DurationHour :5;
  uint8_t filt1DurationMinute :6;
  uint8_t filt2Enable :1;
  uint8_t filt2Hour :5;
  uint8_t filt2Minute :6;
  uint8_t filt2DurationHour :5;
  uint8_t filt2DurationMinute :6;

} SpaFilterSettings;

void _yield() {
  yield();
  ArduinoOTA.handle();
  MDNS.update();
  if (!otaInProgress) mqtt.loop();
}

void print_msg(CircularBuffer<uint8_t, 35> &data) {
  String s;
  //for (i = 0; i < (Q_in[1] + 2); i++) {
  for (i = 0; i < data.size(); i++) {
    x = Q_in[i];
    if (x < 0x0A) s += "0";
    s += String(x, HEX);
    s += " ";
  }
  mqtt.publish("Spa/node/msg", s.c_str());
  _yield();
}

void decodeFault() {
  SpaFaultLog.totEntry = Q_in[5];
  SpaFaultLog.currEntry = Q_in[6];
  SpaFaultLog.faultCode = Q_in[7];
  switch (SpaFaultLog.faultCode) { // this is a inelegant way to do it, a lookup table would be better
    case 15:
      SpaFaultLog.faultMessage = "Sensors are out of sync";
      break;
    case 16:
      SpaFaultLog.faultMessage = "The water flow is low";
      break;
    case 17:
      SpaFaultLog.faultMessage = "The water flow has failed";
      break;
    case 18:
      SpaFaultLog.faultMessage = "The settings have been reset";
      break;
    case 19:
      SpaFaultLog.faultMessage = "Priming Mode";
      break;
    case 20:
      SpaFaultLog.faultMessage = "The clock has failed";
      break;
    case 21:
      SpaFaultLog.faultMessage = "The settings have been reset";
      break;
    case 22:
      SpaFaultLog.faultMessage = "Program memory failure";
      break;
    case 26:
      SpaFaultLog.faultMessage = "Sensors are out of sync -- Call for service";
      break;
    case 27:
      SpaFaultLog.faultMessage = "The heater is dry";
      break;
    case 28:
      SpaFaultLog.faultMessage = "The heater may be dry";
      break;
    case 29:
      SpaFaultLog.faultMessage = "The water is too hot";
      break;
    case 30:
      SpaFaultLog.faultMessage = "The heater is too hot";
      break;
    case 31:
      SpaFaultLog.faultMessage = "Sensor A Fault";
      break;
    case 32:
      SpaFaultLog.faultMessage = "Sensor B Fault";
      break;
    case 34:
      SpaFaultLog.faultMessage = "A pump may be stuck on";
      break;
    case 35:
      SpaFaultLog.faultMessage = "Hot fault";
      break;
    case 36:
      SpaFaultLog.faultMessage = "The GFCI test failed";
      break;
    case 37:
      SpaFaultLog.faultMessage = "Standby Mode (Hold Mode)";
      break;
    default:
      SpaFaultLog.faultMessage = "Unknown error";
      break;
  }
  SpaFaultLog.daysAgo = Q_in[8];
  SpaFaultLog.hour = Q_in[9];
  SpaFaultLog.minutes = Q_in[10];
  mqtt.publish("Spa/fault/Entries", String(SpaFaultLog.totEntry).c_str());
  mqtt.publish("Spa/fault/Entry", String(SpaFaultLog.currEntry).c_str());
  mqtt.publish("Spa/fault/Code", String(SpaFaultLog.faultCode).c_str());
  mqtt.publish("Spa/fault/Message", SpaFaultLog.faultMessage.c_str());
  mqtt.publish("Spa/fault/DaysAgo", String(SpaFaultLog.daysAgo).c_str());
  mqtt.publish("Spa/fault/Hours", String(SpaFaultLog.hour).c_str());
  mqtt.publish("Spa/fault/Minutes", String(SpaFaultLog.minutes).c_str());
  have_faultlog = 2;
  //mqtt.publish("Spa/debug/have_faultlog", "have the faultlog, #2");
}

void decodeFilterSettings() {
  String s;
  String d;
  String payld;

  SpaFilterSettings.filt1Hour = Q_in[5];
  SpaFilterSettings.filt1Minute = Q_in[6];
  SpaFilterSettings.filt1DurationHour = Q_in[7];
  SpaFilterSettings.filt1DurationMinute = Q_in[8];
  SpaFilterSettings.filt2Enable = bitRead(Q_in[9],7); // check
  SpaFilterSettings.filt2Hour = Q_in[9] ^ (SpaFilterSettings.filt2Enable << 7); // check
  SpaFilterSettings.filt2Minute = Q_in[10];
  SpaFilterSettings.filt2DurationHour = Q_in[11];
  SpaFilterSettings.filt2DurationMinute = Q_in[12];
  //MQTT stuff
  /*mqtt.publish("Spa/config/filt1Hour", String(SpaFilterSettings.filt1Hour).c_str());
  mqtt.publish("Spa/config/filt1Minute", String(SpaFilterSettings.filt1Minute).c_str());
  mqtt.publish("Spa/config/filt1DurationHour", String(SpaFilterSettings.filt1DurationHour).c_str());
  mqtt.publish("Spa/config/filt1DurationMinute", String(SpaFilterSettings.filt1DurationMinute).c_str());
  mqtt.publish("Spa/config/filt2Hour", String(SpaFilterSettings.filt2Hour).c_str());
  mqtt.publish("Spa/config/filt2Minute", String(SpaFilterSettings.filt2Minute).c_str());
  mqtt.publish("Spa/config/filt2DurationHour", String(SpaFilterSettings.filt2DurationHour).c_str());
  mqtt.publish("Spa/config/filt2DurationMinute", String(SpaFilterSettings.filt2DurationMinute).c_str());
  mqtt.publish("Spa/config/filt2Enable", String(SpaFilterSettings.filt2Enable).c_str());*/

  //Filter 1 time conversion
  if (SpaFilterSettings.filt1Hour < 10) s = "0"; else s = "";
  s = String(SpaFilterSettings.filt1Hour) + ":";
  if (SpaFilterSettings.filt1Minute < 10) s += "0";
  s += String(SpaFilterSettings.filt1Minute);

  if (SpaFilterSettings.filt1DurationHour < 10) d = "0"; else d = "";
  d = String(SpaFilterSettings.filt1DurationHour) + ":";
  if (SpaFilterSettings.filt1DurationMinute < 10) d += "0";
  d += String(SpaFilterSettings.filt1DurationMinute);

  payld = "{\"start\":\""+s+"\",\"duration\":\""+d+"\"}";
  mqtt.publish("Spa/filter1/state", payld.c_str());

  //Filter 2 time conversion
  if (SpaFilterSettings.filt2Hour < 10) s = "0"; else s = "";
  s += String(SpaFilterSettings.filt2Hour) + ":";
  if (SpaFilterSettings.filt2Minute < 10) s += "0";
  s += String(SpaFilterSettings.filt2Minute);

  if (SpaFilterSettings.filt2DurationHour < 10) d = "0"; else d = "";
  d += String(SpaFilterSettings.filt2DurationHour) + ":";
  if (SpaFilterSettings.filt2DurationMinute < 10) d += "0";
  d += String(SpaFilterSettings.filt2DurationMinute);
  if ((int)(SpaFilterSettings.filt2Enable) == 1) mqtt.publish("Spa/filter2_enabled/state", STRON); else mqtt.publish("Spa/filter2_enabled/state", STROFF);


  payld = "{\"start\":\""+s+"\",\"duration\":\""+d+"\"}";
  mqtt.publish("Spa/filter2/state", payld.c_str());

  have_filtersettings = 2;
}

void decodeSettings() {
  //mqtt.publish("Spa/config/status", "Got config");
  SpaConfig.pump1 = Q_in[5] & 0x03;
  SpaConfig.pump2 = (Q_in[5] & 0x0C) >> 2;
  SpaConfig.pump3 = (Q_in[5] & 0x30) >> 4;
  SpaConfig.pump4 = (Q_in[5] & 0xC0) >> 6;
  SpaConfig.pump5 = (Q_in[6] & 0x03);
  SpaConfig.pump6 = (Q_in[6] & 0xC0) >> 6;
  SpaConfig.light1 = (Q_in[7] & 0x03);
  SpaConfig.light2 = (Q_in[7] >> 2) & 0x03;
  SpaConfig.circ = ((Q_in[8] & 0x80) != 0);
  SpaConfig.blower = ((Q_in[8] & 0x03) != 0);
  SpaConfig.mister = ((Q_in[9] & 0x30) != 0);
  SpaConfig.aux1 = ((Q_in[9] & 0x01) != 0);
  SpaConfig.aux2 = ((Q_in[9] & 0x02) != 0);
  mqtt.publish("Spa/config/pumps1", String(SpaConfig.pump1).c_str());
  mqtt.publish("Spa/config/pumps2", String(SpaConfig.pump2).c_str());
  mqtt.publish("Spa/config/pumps3", String(SpaConfig.pump3).c_str());
  mqtt.publish("Spa/config/pumps4", String(SpaConfig.pump4).c_str());
  mqtt.publish("Spa/config/pumps5", String(SpaConfig.pump5).c_str());
  mqtt.publish("Spa/config/pumps6", String(SpaConfig.pump6).c_str());
  mqtt.publish("Spa/config/light1", String(SpaConfig.light1).c_str());
  mqtt.publish("Spa/config/light2", String(SpaConfig.light2).c_str());
  mqtt.publish("Spa/config/circ", String(SpaConfig.circ).c_str());
  mqtt.publish("Spa/config/blower", String(SpaConfig.blower).c_str());
  mqtt.publish("Spa/config/mister", String(SpaConfig.mister).c_str());
  mqtt.publish("Spa/config/aux1", String(SpaConfig.aux1).c_str());
  mqtt.publish("Spa/config/aux2", String(SpaConfig.aux2).c_str());
  have_config = 2;
}

void decodeState() {
  String s;
  double d = 0.0;
  double c = 0.0;

  // DEBUG for finding meaning:
  //print_msg(Q_in);

  // Status flags byte 9 contains the active temperature scale:
  // 0 = whole Fahrenheit degrees, 1 = half Celsius degrees.
  SpaConfig.temp_scale = bitRead(Q_in[14], 0);
  haveTemperatureScale = true;
  mqtt.publish("Spa/config/temp_scale", String(SpaConfig.temp_scale).c_str());

  // 25:Flag Byte 20 - Set Temperature
  if (SpaConfig.temp_scale == 0) {
    d = Q_in[25];
  } else {
    d = Q_in[25] / 2.0;
  }

  mqtt.publish("Spa/target_temp/state", String(d, 2).c_str());
  if (requestedSetTemperature != 0xFF && Q_in[25] == requestedSetTemperature) {
    Serial1.printf(
      "[temp] Confirmed target %.1f %s\n",
      d,
      SpaConfig.temp_scale ? "C" : "F"
    );
    mqtt.publish("Spa/target_temp/result", "ok");
    requestedSetTemperature = 0xFF;
  }

  // 7:Flag Byte 2 - Actual temperature
  if (Q_in[7] != 0xFF) {
    if (SpaConfig.temp_scale == 0) {
      d = Q_in[7];
    } else {
      d = Q_in[7] / 2.0;
    }

    if (c > 0) {
      if ((d > c * 1.2) || (d < c * 0.8)) d = c; //remove spurious readings greater or less than 20% away from previous read
    }

    mqtt.publish("Spa/temperature/state", String(d, 2).c_str());
    c = d;
  } else {
    d = 0;
  }
  // REMARK Move upper publish to HERE to get 0 for unknown temperature

  // 8:Flag Byte 3 Hour & 9:Flag Byte 4 Minute => Time
  if (Q_in[8] < 10) s = "0"; else s = "";
  SpaState.hour = Q_in[8];
  s += String(Q_in[8]) + ":";
  if (Q_in[9] < 10) s += "0";
  s += String(Q_in[9]);
  SpaState.minutes = Q_in[9];
  mqtt.publish("Spa/time/state", s.c_str());

  // 10:Flag Byte 5 - Heating Mode
  switch (Q_in[10]) {
    case 0:mqtt.publish("Spa/heatingmode/state", STRON); //Ready
      mqtt.publish("Spa/heat_mode/state", "heat"); //Ready
      SpaState.restmode = 0;
      break;
    case 3:// Ready-in-Rest
      SpaState.restmode = 0;
      break;
    case 1:mqtt.publish("Spa/heatingmode/state", STROFF); //Rest
      mqtt.publish("Spa/heat_mode/state", "off"); //Rest
      SpaState.restmode = 1;
      break;
  }

  // 15:Flags Byte 10 / Heat status, Temp Range
  d = bitRead(Q_in[15], 4);
  if (d == 0) mqtt.publish("Spa/heatstate/state", STROFF);
  else if (d == 1 || d == 2) mqtt.publish("Spa/heatstate/state", STRON);

  d = bitRead(Q_in[15], 2);
  if (d == 0) {
    mqtt.publish("Spa/highrange/state", STROFF); //LOW
    SpaState.highrange = 0;
  } else if (d == 1) {
    mqtt.publish("Spa/highrange/state", STRON); //HIGH
    SpaState.highrange = 1;
  }

  // 16:Flags Byte 11
  if (bitRead(Q_in[16], 1) == 1) {
    mqtt.publish("Spa/jet_1/state", STRON);
    SpaState.jet1 = 1;
  } else {
    mqtt.publish("Spa/jet_1/state", STROFF);
    SpaState.jet1 = 0;
  }

  if (bitRead(Q_in[16], 3) == 1) {
    mqtt.publish("Spa/jet_2/state", STRON);
    SpaState.jet2 = 1;
  } else {
    mqtt.publish("Spa/jet_2/state", STROFF);
    SpaState.jet2 = 0;
  }

  // 18:Flags Byte 13
  if (bitRead(Q_in[18], 1) == 1)
    mqtt.publish("Spa/circ/state", STRON);
  else
    mqtt.publish("Spa/circ/state", STROFF);

  if (bitRead(Q_in[18], 2) == 1) {
    mqtt.publish("Spa/blower/state", STRON);
    SpaState.blower = 1;
  } else {
    mqtt.publish("Spa/blower/state", STROFF);
    SpaState.blower = 0;
  }
  // 19:Flags Byte 14
  if (Q_in[19] == 0x03) {
    mqtt.publish("Spa/light/state", STRON);
    SpaState.light = 1;
  } else {
    mqtt.publish("Spa/light/state", STROFF);
    SpaState.light = 0;
  }

  last_state_crc = Q_in[Q_in[1]];

  // Publish own relay states
  s = "OFF";
  if (digitalRead(RLY1) == LOW) s = "ON";
  mqtt.publish("Spa/relay_1/state", s.c_str());

  s = "OFF";
  if (digitalRead(RLY2) == LOW) s = "ON";
  mqtt.publish("Spa/relay_2/state", s.c_str());
}

///////////////////////////////////////////////////////////////////////////////

void hardreset() {
  if (otaInProgress) return;
  Serial1.println("[spa] Restarting after fatal condition");
  Serial1.flush();
  delay(100);
  ESP.restart();
}

void prepareRestart(uint32_t reason) {
  ResetState.magic = SPA_RESTART_REASON_MAGIC;
  ResetState.reason = reason;
  ESP.rtcUserMemoryWrite(0, reinterpret_cast<uint32_t *>(&ResetState), sizeof(ResetState));
  hardreset();
}

String loadRestartReason() {
  if (ESP.rtcUserMemoryRead(0, reinterpret_cast<uint32_t *>(&ResetState), sizeof(ResetState)) &&
      ResetState.magic == SPA_RESTART_REASON_MAGIC) {
    const uint32_t reason = ResetState.reason;
    ResetState.reason = RESTART_REASON_NONE;
    ESP.rtcUserMemoryWrite(0, reinterpret_cast<uint32_t *>(&ResetState), sizeof(ResetState));
    if (reason == RESTART_REASON_RS485_TIMEOUT) return "rs485_timeout";
    if (reason == RESTART_REASON_NONE) return "software_restart";
  }
  return ESP.getResetReason();
}

void mqttpubsub() {
  // ONLY DO THE FOLLOWING IF have_config == true otherwise it will not work
  String Payload;

  // ... Hassio autodiscover
  if (HASSIO) {

      //clear topics:
      mqtt.publish("homeassistant/binary_sensor/Spa", "");
      mqtt.publish("homeassistant/sensor/Spa", "");
      mqtt.publish("homeassistant/switch/Spa", "");
      mqtt.publish("/Spa", "");

      //temperature -> can we try and remove the Payload below, it's messy
      Payload = "{\"name\":\"Hot tub status\",\"uniq_id\":\"ESP82Spa_1\",\"stat_t\":\"Spa/node/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"],\"name\":\"Esp Spa\",\"sw\":\""+String(VERSION)+"\"}}";
      mqtt.publish("homeassistant/binary_sensor/Spa/state/config", Payload.c_str(), true);
      //climate temperature
      if (SpaConfig.temp_scale == 0) {
        mqtt.publish("homeassistant/climate/Spa/temperature/config", "{\"name\":\"Hot tub thermostat\",\"uniq_id\":\"ESP82Spa_0\",\"temp_cmd_t\":\"Spa/target_temp/set\",\"mode_cmd_t\":\"Spa/heat_mode/set\",\"mode_stat_t\":\"Spa/heat_mode/state\",\"temp_unit\":\"F\",\"curr_temp_t\":\"Spa/temperature/state\",\"temp_stat_t\":\"Spa/target_temp/state\",\"min_temp\":\"50\",\"max_temp\":\"104\",\"modes\":[\"off\", \"heat\"], \"temp_step\":\"1\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      } else if (SpaConfig.temp_scale == 1) {
        mqtt.publish("homeassistant/climate/Spa/temperature/config", "{\"name\":\"Hot tub thermostat\",\"uniq_id\":\"ESP82Spa_0\",\"temp_cmd_t\":\"Spa/target_temp/set\",\"mode_cmd_t\":\"Spa/heat_mode/set\",\"mode_stat_t\":\"Spa/heat_mode/state\",\"temp_unit\":\"C\",\"curr_temp_t\":\"Spa/temperature/state\",\"temp_stat_t\":\"Spa/target_temp/state\",\"min_temp\":\"10\",\"max_temp\":\"40\",\"modes\":[\"off\", \"heat\"], \"temp_step\":\"0.5\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      }
      //heat mode
      mqtt.publish("homeassistant/switch/Spa/heatingmode/config", "{\"name\":\"Hot tub heating mode\",\"uniq_id\":\"ESP82Spa_3\",\"cmd_t\":\"Spa/heatingmode/set\",\"stat_t\":\"Spa/heatingmode/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      //heating state
      mqtt.publish("homeassistant/binary_sensor/Spa/heatstate/config", "{\"name\":\"Hot tub heating state\",\"uniq_id\":\"ESP82Spa_6\",\"stat_t\":\"Spa/heatstate/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      //high range
      mqtt.publish("homeassistant/switch/Spa/highrange/config", "{\"name\":\"Hot tub high range\",\"uniq_id\":\"ESP82Spa_4\",\"cmd_t\":\"Spa/highrange/set\",\"stat_t\":\"Spa/highrange/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);

      //OPTIONAL ELEMENTS
      if (SpaConfig.circ){
        //circulation pump
        mqtt.publish("homeassistant/binary_sensor/Spa/circ/config", "{\"name\":\"Hot tub circulation pump\",\"uniq_id\":\"ESP82Spa_5\",\"device_class\":\"power\",\"stat_t\":\"Spa/circ/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      }
      if (SpaConfig.light1) {
        //light 1
        mqtt.publish("homeassistant/switch/Spa/light/config", "{\"name\":\"Hot tub light\",\"uniq_id\":\"ESP82Spa_7\",\"cmd_t\":\"Spa/light/set\",\"stat_t\":\"Spa/light/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      }
      if (SpaConfig.pump1 != 0) {
        //jets 1
        mqtt.publish("homeassistant/switch/Spa/jet_1/config", "{\"name\":\"Hot tub jet1\",\"uniq_id\":\"ESP82Spa_8\",\"cmd_t\":\"Spa/jet_1/set\",\"stat_t\":\"Spa/jet_1/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      }
      if (SpaConfig.pump2 != 0) {
        //jets 2
        mqtt.publish("homeassistant/switch/Spa/jet_2/config", "{\"name\":\"Hot tub jet2\",\"uniq_id\":\"ESP82Spa_9\",\"cmd_t\":\"Spa/jet_2/set\",\"stat_t\":\"Spa/jet_2/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      }
      if (SpaConfig.blower)
      {
        //blower
        mqtt.publish("homeassistant/switch/Spa/blower/config", "{\"name\":\"Hot tub blower\",\"uniq_id\":\"ESP82Spa_10\",\"cmd_t\":\"Spa/blower/set\",\"stat_t\":\"Spa/blower/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      }

      mqtt.publish("homeassistant/sensor/Spa/filter1_start/config", "{\"name\":\"Filter 1 start\",\"val_tpl\": \"{{value_json.start}}\",\"uniq_id\":\"ESP82Spa_11\",\"stat_t\":\"Spa/filter1/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      mqtt.publish("homeassistant/sensor/Spa/filter2_start/config", "{\"name\":\"Filter 2 start\",\"val_tpl\": \"{{value_json.start}}\",\"uniq_id\":\"ESP82Spa_12\",\"stat_t\":\"Spa/filter2/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      mqtt.publish("homeassistant/sensor/Spa/filter1_duration/config", "{\"name\":\"Filter 1 duration\",\"val_tpl\": \"{{value_json.duration}}\",\"uniq_id\":\"ESP82Spa_13\",\"stat_t\":\"Spa/filter1/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      mqtt.publish("homeassistant/sensor/Spa/filter2_duration/config", "{\"name\":\"Filter 2 duration\",\"val_tpl\": \"{{value_json.duration}}\",\"uniq_id\":\"ESP82Spa_14\",\"stat_t\":\"Spa/filter2/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
      mqtt.publish("homeassistant/binary_sensor/Spa/filter2_enabled/config", "{\"name\":\"Filter 2 enabled\",\"uniq_id\":\"ESP82Spa_15\",\"stat_t\":\"Spa/filter2_enabled/state\",\"platform\":\"mqtt\",\"dev\":{\"ids\":[\"ESP82Spa\"]}}", true);
  }

  mqtt.publish("Spa/node/state", "ON");
  mqtt.publish("Spa/node/debug", "RECONNECT");
  //mqtt.publish("Spa/node/debug", String(millis()).c_str());
  //mqtt.publish("Spa/node/debug", String(oldstate).c_str());
  mqtt.publish("Spa/node/version", VERSION);
  mqtt.publish("Spa/node/flashsize", String(ESP.getFlashChipRealSize()).c_str());
  mqtt.publish("Spa/node/chipid", String(ESP.getChipId()).c_str());
	mqtt.publish("Spa/node/speed", String(ESP.getCpuFreqMHz()).c_str());
	mqtt.publish("Spa/node/reset_reason", restartReason.c_str(), true);

  // ... and resubscribe
  mqtt.subscribe("Spa/command");
  mqtt.subscribe("Spa/target_temp/set");
  mqtt.subscribe("Spa/heatingmode/set");
  mqtt.subscribe("Spa/heat_mode/set");
  mqtt.subscribe("Spa/highrange/set");

  //OPTIONAL ELEMENTS
  if (SpaConfig.pump1 != 0) {
    mqtt.subscribe("Spa/jet_1/set");
  }
  if (SpaConfig.pump2 != 0) {
    mqtt.subscribe("Spa/jet_2/set");
  }
  if (SpaConfig.blower) {
    mqtt.subscribe("Spa/blower/set");
  }
  if (SpaConfig.light1) {
    mqtt.subscribe("Spa/light/set");
  }

  mqtt.subscribe("Spa/relay_1/set");
  mqtt.subscribe("Spa/relay_2/set");

  //not sure what this is
  last_state_crc = 0x00;

  //done with config
  have_config = 3;
}

void reconnect() {
  //int oldstate = mqtt.state();
  //boolean connection = false;
  // Loop until we're reconnected
  if (!mqtt.connected()) {
    // Attempt to connect
    Serial1.printf("[mqtt] Connecting to %s:1883...\n", BROKER.c_str());
    if (BROKER_PASS == "") {
      //connection =
      mqtt.connect(String(String("Spa") + String(millis())).c_str());
    }
    else {
      //connection =
      mqtt.connect(String(String("Spa") + String(millis())).c_str(), BROKER_LOGIN.c_str(), BROKER_PASS.c_str());
    }
    if (mqtt.connected()) {
      Serial1.println("[mqtt] Connected");
      mqtt.publish("Spa/node/reset_reason", restartReason.c_str(), true);
      mqtt.publish("Spa/node/version", VERSION, true);
    } else {
      Serial1.printf("[mqtt] Connection failed, state %d\n", mqtt.state());
    }
    //time to connect
    delay(1000);

    //have_config = 2;
    if (have_config == 3) {
      //have_config = 2; // we have disconnected, let's republish our configuration
      mqttpubsub();
    }

  }
  mqtt.setBufferSize(512); //increase pubsubclient buffer size
}

// function called when a MQTT message arrived
void callback(char* p_topic, byte * p_payload, unsigned int p_length) {
  // concat the payload into a string
  String payload;
  for (uint8_t i = 0; i < p_length; i++) {
    payload.concat((char)p_payload[i]);
  }
  String topic = String(p_topic);

  mqtt.publish("Spa/node/debug", topic.c_str());
  _yield();

  // handle message topic
  if (topic.startsWith("Spa/relay_")) {
    bool newstate = 0;

    if (payload.equals("ON")) newstate = LOW;
    else if (payload.equals("OFF")) newstate = HIGH;

    if (topic.charAt(10) == '1') {
      pinMode(RLY1, INPUT);
      delay(25);
      pinMode(RLY1, OUTPUT);
      digitalWrite(RLY1, newstate);
    }
    else if (topic.charAt(10) == '2') {
      pinMode(RLY2, INPUT);
      delay(25);
      pinMode(RLY2, OUTPUT);
      digitalWrite(RLY2, newstate);
    }
  } else if (topic.equals("Spa/command")) {
    if (payload.equals("reset")) hardreset();
  } else if (topic.equals("Spa/heatingmode/set")) {
    if (payload.equals("ON") && SpaState.restmode == 1) send = 0x51; // ON = Ready; OFF = Rest
    else if (payload.equals("OFF") && SpaState.restmode == 0) send = 0x51;
  } else if (topic.equals("Spa/heat_mode/set")) {
    if (payload.equals("heat") && SpaState.restmode == 1) send = 0x51; // ON = Ready; OFF = Rest
    else if (payload.equals("off") && SpaState.restmode == 0) send = 0x51;
  } else if (topic.equals("Spa/light/set")) {
    if (payload.equals("ON") && SpaState.light == 0) send = 0x11;
    else if (payload.equals("OFF") && SpaState.light == 1) send = 0x11;
  } else if (topic.equals("Spa/jet_1/set")) {
    if (payload.equals("ON") && SpaState.jet1 == 0) send = 0x04;
    else if (payload.equals("OFF") && SpaState.jet1 == 1) send = 0x04;
  } else if (topic.equals("Spa/jet_2/set")) {
    if (payload.equals("ON") && SpaState.jet2 == 0) send = 0x05;
    else if (payload.equals("OFF") && SpaState.jet2 == 1) send = 0x05;
  } else if (topic.equals("Spa/blower/set")) {
    if (payload.equals("ON") && SpaState.blower == 0) send = 0x0C;
    else if (payload.equals("OFF") && SpaState.blower == 1) send = 0x0C;
  } else if (topic.equals("Spa/highrange/set")) {
    if (payload.equals("ON") && SpaState.highrange == 0) send = 0x50; //ON = High, OFF = Low
    else if (payload.equals("OFF") && SpaState.highrange == 1) send = 0x50;
  } else if (topic.equals("Spa/target_temp/set")) {
    Serial1.printf("[temp] Target request: %s\n", payload.c_str());
    double requestedTemperature = payload.toDouble();
    double minimum = SpaState.highrange ? (SpaConfig.temp_scale ? 26.0 : 80.0)
                                        : (SpaConfig.temp_scale ? 10.0 : 50.0);
    double maximum = SpaState.highrange ? (SpaConfig.temp_scale ? 40.0 : 104.0)
                                        : (SpaConfig.temp_scale ? 26.0 : 80.0);

    if (requestedTemperature < minimum || requestedTemperature > maximum) {
      Serial1.printf(
        "[temp] Rejected %.1f; active range %.1f-%.1f\n",
        requestedTemperature,
        minimum,
        maximum
      );
      mqtt.publish(
        "Spa/target_temp/result",
        String(String("rejected: expected ") + String(minimum, 1) + "-" +
               String(maximum, 1)).c_str()
      );
      return;
    }

    settemp = SpaConfig.temp_scale
      ? lround(requestedTemperature * 2.0)
      : lround(requestedTemperature);
    requestedSetTemperature = settemp;
    temperatureRequestAt = millis();
    send = 0xff;
    mqtt.publish("Spa/target_temp/result", "queued");
    Serial1.printf("[temp] Queued protocol value %u\n", settemp);
  }
}

///////////////////////////////////////////////////////////////////////////////

void setup() {
  // Begin RS485 in listening mode -> no longer required with new RS485 chip
  if (AUTO_TX){

  } else {
    pinMode(TX485, OUTPUT);
    digitalWrite(TX485, LOW);
  }

  pinMode(RLY1, OUTPUT);
  digitalWrite(RLY1, HIGH);
  pinMode(RLY2, OUTPUT);
  digitalWrite(RLY2, HIGH);

  // Spa communication, 115.200 baud 8N1
  Serial.begin(115200);
  Serial1.begin(DEBUG_BAUD);
  Serial1.println();
  Serial1.printf("[boot] esp8266_spa %s starting\n", VERSION);
  Serial1.printf(
    "[boot] Chip %08X, reset reason: %s\n",
    ESP.getChipId(),
    ESP.getResetReason().c_str()
  );
  restartReason = loadRestartReason();
  Serial1.printf("[boot] Published reset reason: %s\n", restartReason.c_str());

  // give Spa time to wake up after POST
  for (uint8_t i = 0; i < 5; i++) {
    delay(1000);
    yield();
  }

  Q_in.clear();
  Q_out.clear();

  WiFi.mode(WIFI_STA);
  WiFi.hostname(OTA_HOSTNAME);
  WiFi.setOutputPower(20.5); // this sets wifi to highest power
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());
  unsigned long timeout = millis() + 10000;

  while (WiFi.status() != WL_CONNECTED && millis() < timeout) {
    Serial1.print('.');
    yield();
    delay(100);
  }
  Serial1.println();

  // Reset because of no connection
  if (WiFi.status() != WL_CONNECTED) {
    Serial1.printf("[wifi] Connection failed, status %d\n", WiFi.status());
    hardreset();
  }
  Serial1.printf(
    "[wifi] Connected: IP %s, RSSI %d dBm, hostname %s.local\n",
    WiFi.localIP().toString().c_str(),
    WiFi.RSSI(),
    OTA_HOSTNAME
  );

  mqtt.setServer(BROKER.c_str(), 1883);
  mqtt.setCallback(callback);
  mqtt.setKeepAlive(10);
  mqtt.setSocketTimeout(20);

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(SPA_OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    Serial1.println("[ota] Update started");
    mqtt.publish("Spa/node/debug", "OTA update started");
  });
  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    Serial1.println("[ota] Update finished");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial1.printf("[ota] Update error %u\n", error);
    mqtt.publish("Spa/node/debug", String(String("OTA error: ") + error).c_str());
  });
  ArduinoOTA.begin();
  Serial1.printf("[ota] Ready at %s.local:%u (password protected)\n", OTA_HOSTNAME, 8266);
  lastrx = millis();
}

void loop() {
  if (otaInProgress) {
    _yield();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) ESP.restart();
  if (!mqtt.connected()) reconnect();
  if (have_config == 2 && haveTemperatureScale) mqttpubsub(); // publish MQTT discovery only after config and unit are known
  _yield();

  if (requestedSetTemperature != 0xFF &&
      millis() - temperatureRequestAt > 5000) {
    Serial1.println("[temp] Target update was not confirmed");
    mqtt.publish("Spa/target_temp/result", "not_confirmed");
    requestedSetTemperature = 0xFF;
  }

  //Every x minutes, read the fault log and filter settings using SpaState.minutes
  if ((int)(SpaState.minutes % 5) == 0)
  {
    //logic to only get the error message once -> this is dirty
    //have_faultlog = 0;
    if (have_faultlog == 2) { // we got the fault log before and treated it
      if (faultlog_minutes == SpaState.minutes) { // we got the fault log this interval so do nothing
      }
      else {
        faultlog_minutes = SpaState.minutes;
        have_faultlog = 0;
      }
    }
    if (have_filtersettings == 2) { // we got the filter cycles before and treated it
      if (filtersettings_minutes == SpaState.minutes) { // we got the filter cycles this interval so do nothing
      }
      else {
        filtersettings_minutes = SpaState.minutes;
        have_filtersettings = 0;
      }
    }

  }

  // DEBUG:mqtt.publish("Spa/rcv", String(x).c_str()); _yield();
  // Read from Spa RS485
  if (Serial.available()) {
    x = Serial.read();
    Q_in.push(x);

    // Drop until SOF is seen
    if (Q_in.first() != 0x7E) Q_in.clear();
    lastrx = millis();
    if (!receivedSpaPacket) {
      receivedSpaPacket = true;
      Serial1.println("[rs485] Spa traffic received");
    }
  }

  // Double SOF-marker, drop last one
  if (Q_in[1] == 0x7E && Q_in.size() > 1) Q_in.pop();

  // Complete package
  //if (x == 0x7E && Q_in[0] == 0x7E && Q_in[1] != 0x7E) {
  if (x == 0x7E && Q_in.size() > 2) {
    //print_msg();

    // Unregistered or yet in progress
    if (id == 0) {
      if (Q_in[2] == 0xFE) print_msg(Q_in);

      // FE BF 02:got new client ID
      if (Q_in[2] == 0xFE && Q_in[4] == 0x02) {
        id = Q_in[5];
        if (id > 0x2F) id = 0x2F;
        Serial1.printf("[rs485] Assigned client ID 0x%02X\n", id);

        ID_ack();
        mqtt.publish("Spa/node/id", String(id).c_str());
      }

      // FE BF 00:Any new clients?
      if (Q_in[2] == 0xFE && Q_in[4] == 0x00) {
        ID_request();
      }
    } else if (Q_in[2] == id && Q_in[4] == 0x06) { // we have an ID, do clever stuff
        // id BF 06:Ready to Send
        if (send == 0xff) {
          // 0xff marks dirty temperature for now
          Q_out.push(id);
          Q_out.push(0xBF);
          Q_out.push(0x20);
          Q_out.push(settemp);
        } else if (send == 0x00) {
          if (have_config == 0) { // Get configuration of the hot tub
            Q_out.push(id);
            Q_out.push(0xBF);
            Q_out.push(0x22);
            Q_out.push(0x00);
            Q_out.push(0x00);
            Q_out.push(0x01);
            //mqtt.publish("Spa/config/status", "Getting config");
            have_config = 1;
          } else if (have_faultlog == 0) { // Get the fault log
            Q_out.push(id);
            Q_out.push(0xBF);
            Q_out.push(0x22);
            Q_out.push(0x20);
            Q_out.push(0xFF);
            Q_out.push(0x00);
            have_faultlog = 1;
            //mqtt.publish("Spa/debug/have_faultlog", "requesting fault log, #1");
          } else if ((have_filtersettings == 0) && (have_faultlog == 2)) { // Get the filter cycles log once we have the faultlog
            Q_out.push(id);
            Q_out.push(0xBF);
            Q_out.push(0x22);
            Q_out.push(0x01);
            Q_out.push(0x00);
            Q_out.push(0x00);
            //mqtt.publish("Spa/debug/have_faultlog", "requesting filter settings, #1");
            have_filtersettings = 1;
          } else {
            // A Nothing to Send message is sent by a client immediately after a Clear to Send message if the client has no messages to send.
            Q_out.push(id);
            Q_out.push(0xBF);
            Q_out.push(0x07);
          }
        } else {
          // Send toggle commands
          Q_out.push(id);
          Q_out.push(0xBF);
          Q_out.push(0x11);
          Q_out.push(send);
          Q_out.push(0x00);
        }

        rs485_send();
        send = 0x00;
    } else if (Q_in[2] == id && Q_in[4] == 0x2E) {
      if (last_state_crc != Q_in[Q_in[1]]) {
        decodeSettings();
      }
    } else if (Q_in[2] == id && Q_in[4] == 0x28) {
      if (last_state_crc != Q_in[Q_in[1]]) {
        decodeFault();
      }
    } else if (Q_in[2] == 0xFF && Q_in[4] == 0x13) { // FF AF 13:Status Update - Packet index offset 5
      if (last_state_crc != Q_in[Q_in[1]]) {
        decodeState();
      }
    } else if (Q_in[2] == id && Q_in[4] == 0x23) { // FF AF 23:Filter Cycle Message - Packet index offset 5
      if (last_state_crc != Q_in[Q_in[1]]) {
        //mqtt.publish("Spa/debug/have_faultlog", "decoding filter settings");
        decodeFilterSettings();
      }
    } else {
      // DEBUG for finding meaning
      //if (Q_in[2] & 0xFE || Q_in[2] == id)
      //print_msg(Q_in);
    }

    // Clean up queue
    _yield();
    Q_in.clear();
  }

  // Long time no receive
  if (!otaInProgress && millis() - lastrx > 30000) {
    if (PRODUCTION) {
      Serial1.println("[rs485] No spa traffic for 30 seconds");
      prepareRestart(RESTART_REASON_RS485_TIMEOUT);
    }
  }

  if (millis() - lastHealthLog > 30000) {
    lastHealthLog = millis();
    Serial1.printf(
      "[health] WiFi RSSI %d dBm, MQTT %s, RS485 %s, client 0x%02X, free heap %u\n",
      WiFi.RSSI(),
      mqtt.connected() ? "connected" : "disconnected",
      millis() - lastrx < 30000 ? "active" : "stale",
      id,
      ESP.getFreeHeap()
    );
  }
}
