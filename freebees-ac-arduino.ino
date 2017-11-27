#include "WiFi.h"
#include "PubSubClient.h"
#include "Wire.h"
#include "RTClib.h"
#include "ArduinoJson.h"
#include "AsyncTCP.h"
#include "ESPAsyncWebServer.h"
#include "esp32-hal-ledc.h"
#include "SPIFFS.h"
#include "PN532_HSU.h"
#include "PN532.h"

#define EVT_ENTER          1
#define EVT_EXIT           2
#define EVT_PASS           3
#define EVT_KEY_NOT_FOUND  4
#define EVT_KEY_EXP_OR_NA  5 
#define READER1_TX_PIN     4
#define READER1_RX_PIN     5
#define READER2_TX_PIN     26
#define READER2_RX_PIN     27
#define BEEP1_PIN          16
#define BEEP2_PIN          17
#define RELAY1_PIN         18
#define RELAY2_PIN         19
#define DOOR_SENSOR1_PIN   32
#define DOOR_SENSOR2_PIN   33
#define EXIT1_BTN_PIN      12
#define EXIT2_BTN_PIN      14
#define RTC_SDA_PIN        21
#define RTC_SCL_PIN        22
#define STATUS_LED_PIN     2 
#define SETUP_BTN_PIN      0 

HardwareSerial Serial1(1);
HardwareSerial Serial2(2);
PN532_HSU      pn532hsu(Serial1);
PN532_HSU      pn532hsu2(Serial2);
PN532          nfc(pn532hsu);
PN532          nfc2(pn532hsu2);
WiFiClient     espClient;
PubSubClient   client_MQTT (espClient);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
RTC_DS3231     rtc;

const char* ssid;
const char* ssidPass;
const char* mqttServer     = "freebees.ru";
//const char* mqttServer    = "192.168.0.101";
String adminPass;
String mqttUser;
String mqttPass;

char        devId[9];

int         mqttPort       = 1884;
int         relayTime      = 100;
int         relayType;
int         accessTotal    = 0;
int         accessCount    = 0;

bool        inAPMode       = false;
bool        reader1found   = true;
bool        reader2found   = true;
bool        activateRelay1 = false;
bool        activateRelay2 = false;
bool        needReboot     = false;
bool        dirEmpty       = false;

String      topic_sub;
String      topic_part;

long        previousMillis = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("[ INFO ] Freebees Access Control");  
  if (!SPIFFS.begin()) {
    Serial.println("[ WARN ] SPIFFS Mount Failed");
    return;
  }
  if (!loadConfig()) {
    Serial.println("[ WARN ] Failed to load config");
    return;
  }  
  if (!setupRtc()) {
    Serial.println("[ WARN ] RTC init fail");
    return;
  }
  if (!setupReader1()) {
    Serial.println("[ WARN ] Reader 1 not connected");
    return;
  }
  if (!setupReader2()) {
    Serial.println("[ WARN ] Reader 2 not connected");
    return;
  }
}

void listDir(fs::FS &fs, const char * dirname, uint8_t levels) {
  Serial.printf("Listing directory: %s\n", dirname);

  File root = fs.open(dirname);
  if (!root) {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  DIR : ");
      Serial.println(file.name());
      if (levels) {
        listDir(fs, file.name(), levels - 1);
      }
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("  SIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

bool setupRtc() {
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  if (!rtc.begin()) {
    return false;
  }
  return true;
}

void connectSTA() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, ssidPass);
  Serial.println("[ INFO ] Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  } 
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  Serial.println("[ INFO ] Connected");
}

bool setupReader1() {
  Serial1.begin(115200, SERIAL_8N1, READER1_TX_PIN, READER1_RX_PIN);
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    reader1found = false;
    return false;
  }
  nfc.setPassiveActivationRetries(0x32);
  nfc.SAMConfig();
  return true;
}

bool setupReader2() {
  Serial2.begin(115200, SERIAL_8N1, READER2_TX_PIN, READER2_RX_PIN);
  nfc2.begin();
  uint32_t versiondata2 = nfc2.getFirmwareVersion();
  if (!versiondata2) {
    reader2found = false;
    return false;
  } else {
    nfc2.setPassiveActivationRetries(0x32);
    nfc2.SAMConfig();
  }
  return true;
}

String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;
  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

void DeviceTimeToSerial() {
  DateTime now = rtc.now();
  Serial.print("[ INFO ] Device time ");
  Serial.print(now.day(), DEC);
  Serial.print('.');
  Serial.print(now.month(), DEC);
  Serial.print('.');
  Serial.print(now.year(), DEC);
  Serial.print(" ");
  Serial.print(now.hour(), DEC);
  Serial.print(':');
  Serial.print(now.minute(), DEC);
  Serial.print(':');
  Serial.print(now.second(), DEC);
  Serial.println();
  Serial.print("[ INFO ] Device UNIX time ");
  Serial.println(now.unixtime());
  Serial.println();
}

void callback(char* topic, byte* payload, unsigned int length) {
  String t_cmd = getValue(topic, '/', 4);

  if (t_cmd.equals("time")) {
    handleTime(payload, length);
    return;
  }
  if (t_cmd.equals("sync")) {
    handleSync(payload);
    return;
  }
  if (t_cmd.equals("config")) {
    handleConfig(payload, length);
    return;
  }
  if (t_cmd.equals("status")) {
    handleStatus(payload);
    return;
  }
}

bool reconnectMqtt() {
  String clientId = "FRBSACDEVICE-";
  clientId += String(random(0xffff), HEX);
  Serial.println("[ INFO ] Connecting to MQTT Server...");
  digitalWrite(STATUS_LED_PIN, LOW);
  
  if (client_MQTT.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str())) {
    client_MQTT.subscribe(topic_sub.c_str());
    client_MQTT.publish((topic_part + "/ac/time").c_str(), "{\"action\":\"set\"}");
    digitalWrite(STATUS_LED_PIN, HIGH);
    Serial.println("[ INFO ] Connected");
  } else {
    Serial.println("[ WARN ] MQTT connection failed");
    
    }
  return client_MQTT.connected();
}

void reader1Loop() {
  boolean success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength;
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, &uid[0], &uidLength);
  // If card read success
  if (success) {
    String cardUid;
    for (uint8_t i = 0; i < uidLength; i++) {
      cardUid += String(uid[i], HEX);
    }
    cardUid.toUpperCase();
    String filename = "/P/" + cardUid;
    File f = SPIFFS.open(filename, "r");
    // If key file found
    if (f && !f.isDirectory()) {
      size_t size = f.size();
      std::unique_ptr<char[]> buf(new char[size]);
      f.readBytes(buf.get(), size);
      DynamicJsonBuffer jsonBuffer;
      JsonObject& json = jsonBuffer.parseObject(buf.get());
      // If JSON parse success
      if (json.success()) {
        String k_uid = json["k_uid"];
        // If read card equals to card UID from key file
        if (cardUid == k_uid) {
          DateTime now = rtc.now();
          long k_af = json["k_af"];
          long k_at = json["k_at"];
          long currentTime = now.unixtime();
          //If key already active and not expired
          if(currentTime > k_af && currentTime < k_at){
            activateRelay1 = true;
            digitalWrite(RELAY1_PIN, !relayType);
            ledcWrite(0, 255);
            delay(relayTime);
            ledcWrite(0, 0);
            digitalWrite(RELAY1_PIN, relayType);
            DynamicJsonBuffer jsonBuffer2;
            JsonObject& json2 = jsonBuffer2.createObject();
            json2["evt"] = EVT_ENTER;
            json2["k_uid"] = cardUid;
            char buffer[jsonBuffer2.size()];
            json2.printTo(buffer, sizeof(buffer));
            client_MQTT.publish((topic_part + "/ac/event").c_str(), buffer);
            jsonBuffer2.clear();
            return;
          } else {
              DynamicJsonBuffer jsonBuffer4;
              JsonObject& json4 = jsonBuffer4.createObject();
              json4["evt"] = EVT_KEY_EXP_OR_NA;
              json4["k_uid"] = cardUid;
              char buffer[jsonBuffer4.size()];
              json4.printTo(buffer, sizeof(buffer));
              client_MQTT.publish((topic_part + "/ac/event").c_str(), buffer);
              ledcWrite(1, 255);
              delay(400);
              ledcWrite(1, 0);
              jsonBuffer4.clear();
          }
          return;
        }
      }
      jsonBuffer.clear();
    } else {
      DynamicJsonBuffer jsonBuffer3;
      JsonObject& json3 = jsonBuffer3.createObject();
      json3["evt"] = EVT_KEY_NOT_FOUND;
      json3["k_uid"] = cardUid;
      char buffer[jsonBuffer3.size()];
      json3.printTo(buffer, sizeof(buffer));
      client_MQTT.publish((topic_part + "/ac/event").c_str(), buffer);
      ledcWrite(1, 255);
      delay(400);
      ledcWrite(1, 0);
      jsonBuffer3.clear();
    }
  }
}

void reader2Loop() {
  boolean success2;
  uint8_t uid2[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength2;
  success2 = nfc2.readPassiveTargetID(PN532_MIFARE_ISO14443A, &uid2[0], &uidLength2);
  if (success2) {
    String cardUid;
    for (uint8_t i = 0; i < uidLength2; i++) {
      cardUid += String(uid2[i], HEX);
    }
    cardUid.toUpperCase();
    String filename = "/P/" + cardUid;
    File f = SPIFFS.open(filename, "r");
    if (f && !f.isDirectory()) {
      size_t size = f.size();
      std::unique_ptr<char[]> buf(new char[size]);
      f.readBytes(buf.get(), size);
      DynamicJsonBuffer jsonBuffer;
      JsonObject& json = jsonBuffer.parseObject(buf.get());
      if (json.success()) {
        String k_uid = json["k_uid"];
        if (cardUid == k_uid) {
          DateTime now = rtc.now();
          long k_af = json["k_af"];
          long k_at = json["k_at"];
          long currentTime = now.unixtime();
          if(currentTime > k_af && currentTime < k_at){
            activateRelay2 = true;
            digitalWrite(RELAY2_PIN, !relayType);
            ledcWrite(1, 255);
            delay(relayTime);
            ledcWrite(1, 0);
            digitalWrite(RELAY2_PIN, relayType);
            DynamicJsonBuffer jsonBuffer2;
            JsonObject& json2 = jsonBuffer2.createObject();
            json2["evt"] = EVT_EXIT;
            json2["k_uid"] = cardUid;
            size_t size = jsonBuffer2.size();
            char buffer[size];
            json2.printTo(buffer, sizeof(buffer));
            client_MQTT.publish((topic_part + "/ac/event").c_str(), buffer);
            jsonBuffer2.clear();
            return;
          } else {
              DynamicJsonBuffer jsonBuffer4;
              JsonObject& json4 = jsonBuffer4.createObject();
              json4["evt"] = EVT_KEY_EXP_OR_NA;
              json4["k_uid"] = cardUid;
              char buffer[jsonBuffer4.size()];
              json4.printTo(buffer, sizeof(buffer));
              client_MQTT.publish((topic_part + "/ac/event").c_str(), buffer);
              ledcWrite(1, 255);
              delay(400);
              ledcWrite(1, 0);
              jsonBuffer4.clear();
          }
          return;
        }
      }
      jsonBuffer.clear();
    } else {
      DynamicJsonBuffer jsonBuffer3;
      JsonObject& json3 = jsonBuffer3.createObject();
      json3["evt"] = EVT_KEY_NOT_FOUND;
      json3["k_uid"] = cardUid;
      char buffer[jsonBuffer3.size()];
      json3.printTo(buffer, sizeof(buffer));
      client_MQTT.publish((topic_part + "/ac/event").c_str(), buffer);
      ledcWrite(1, 255);
      delay(400);
      ledcWrite(1, 0);
      jsonBuffer3.clear();
    }
  }
}

void loop() {
  unsigned long currentMillis = millis();
  if (needReboot) {
    delay(100);
    ESP.restart();
  }
  if (digitalRead(SETUP_BTN_PIN) == LOW) {
    fallbacktoAPMode();
  }
  if (reader1found) {
    reader1Loop();
  }
  if (reader2found) {
    reader2Loop();
  }
  if (!client_MQTT.connected() && WiFi.status() == WL_CONNECTED && !inAPMode) {
    if (currentMillis - previousMillis > 1000) {
      previousMillis = currentMillis; 
      if (reconnectMqtt()) {
        previousMillis = 0;
      }
    }
  } else {
    client_MQTT.loop();
  }
}

void handleTime(byte* payload, unsigned int length) {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(payload);
  if (json["action"] == "set") {
    Serial.println("[ INFO ] Time sync attempt...");
    rtc.adjust(DateTime(json["year"].as<int>(), json["month"].as<int>(), json["day"].as<int>(), json["hours"].as<int>(), json["minutes"].as<int>(), json["seconds"].as<int>()));
    jsonBuffer.clear();
    client_MQTT.publish((topic_part + "/ac/time").c_str(), "{\"success\":true}");
    DeviceTimeToSerial();
    return;
  } else {
    client_MQTT.publish((topic_part + "/ac/time").c_str(), "{\"success\":false}");
  }
}

void handleConfig(byte* payload, unsigned int length) {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(payload);
  if (json["action"] == "set") {
    File configFile = SPIFFS.open("/auth/config.json", "w");
    if (json.success()) {
      json.remove("action");
      json["cmd"] = "config";
      json.printTo(configFile);
      client_MQTT.publish((topic_part + "/ac/config").c_str(), "{\"success\":true}");
      configFile.close();
      jsonBuffer.clear();
      needReboot = true;
      return;
    } else {
      client_MQTT.publish((topic_part + "/ac/config").c_str(), "{\"success\":false}");
    }
  }
  if (json["action"] == "get") {
    File configFile = SPIFFS.open("/auth/config.json", "r");
    DynamicJsonBuffer jsonBuffer2;
    JsonObject& json2 = jsonBuffer2.parseObject(configFile);
    if (json2.success()) {
      char buffer[configFile.size()];
      json2.printTo(buffer, sizeof(buffer) + 1);
      client_MQTT.publish((topic_part + "/ac/config").c_str(), buffer);
      jsonBuffer2.clear();
      return;
    } else {
      client_MQTT.publish((topic_part + "/ac/config").c_str(), "{\"success\":false}");
    }
  }
}

void handleStatus(byte* payload) {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(payload);
  if (json["action"] == "get") {
    DynamicJsonBuffer jsonBuffer2;
    JsonObject& json2 = jsonBuffer2.createObject();
    DateTime now = rtc.now();
    String devtime =    String(now.year(), DEC)   + "-" +
                        String(now.month(), DEC)  + "-" +
                        String(now.day(), DEC)    + " " +
                        String(now.hour(), DEC)   + ":" +
                        String(now.minute(), DEC) + ":" +
                        String(now.second(), DEC);
    json2["online"]     = true;
    json2["heap"]       = ESP.getFreeHeap();
    json2["devid"]      = devId;
    json2["cpu"]        = ESP.getCpuFreqMHz();
    json2["flashsize"]  = ESP.getFlashChipSize();
    json2["devtime"]    = devtime;
    json2["keyscount"]  = getKeysCount();
    char buffer[json2.measureLength()];
    json2.printTo(buffer, sizeof(buffer) + 1);
    client_MQTT.publish((topic_part + "/ac/status").c_str(), buffer);
    jsonBuffer.clear();
    jsonBuffer2.clear();
    return;
  } else {
    client_MQTT.publish((topic_part + "/ac/status").c_str(), "{\"success\":false}");
  }
}

int getKeysCount(){
  File root = SPIFFS.open("/P");
  File file = root.openNextFile(); 
  int count = -1;
  while(file){
    count++;  
    file = root.openNextFile();
  }
  file.close();
  root.close();
  return count;
}

void handleSync(byte* payload) {

  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(payload);
  if (json.success()) {
    String filename;
    int accessLimit = 16;
    if (json["total"]) {
      //json.prettyPrintTo(Serial);
      String jsonStr = "{\"action\":\"set\",\"fetch\":0,\"limit\":" + String(accessLimit) + "}";
      accessTotal = json["total"];
      client_MQTT.publish((topic_part + "/ac/sync").c_str(), jsonStr.c_str());
    } else if (json["list"] && accessTotal >= accessCount) {
      JsonArray& list = json["list"].asArray();
      for (int i = 0; i < list.size(); i++)
      {
        String uid = list[i]["k_uid"];
        filename = "/P/" + uid;
        SPIFFS.remove(filename);
        File dataFile = SPIFFS.open(filename, "w");
        list[i].printTo(dataFile);
        uid = "";
        dataFile.close();
      }
      accessCount += accessLimit;
      String jsonStr = "{\"action\":\"set\",\"fetch\":" + String(accessCount) + ", \"limit\":" + String(accessLimit) + "}";
      client_MQTT.publish((topic_part + "/ac/sync").c_str(), jsonStr.c_str());
      
    } else {
      DynamicJsonBuffer jsonBuffer2;
      JsonObject& json2 = jsonBuffer2.createObject();
      json2["success"] = true;
      json2["cmd"] = "sync";
      json2["devid"] = devId;
      char buffer[json2.measureLength()];
      json2.printTo(buffer, sizeof(buffer) + 1);
      client_MQTT.publish((topic_part + "/ac/sync").c_str(), buffer);
      jsonBuffer2.clear();
      listDir(SPIFFS, "/", 0);
    }
  } else {
      DynamicJsonBuffer jsonBuffer2;
      JsonObject& json2 = jsonBuffer2.createObject();
      json2["success"] = false;
      json2["cmd"] = "sync";
      json2["devid"] = devId;
      char buffer[json2.measureLength()];
      json2.printTo(buffer, sizeof(buffer) + 1);
      client_MQTT.publish((topic_part + "/ac/sync").c_str(), buffer);
      jsonBuffer2.clear();
  }
  jsonBuffer.clear();

}

bool loadConfig() {
 // listDir(SPIFFS, "/", 0);
  File configFile = SPIFFS.open("/auth/config.json", "r");
  size_t size = configFile.size();
  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());
  snprintf(devId, 9, "%08X", (uint32_t)ESP.getEfuseMac());
  if (!json.success()) {
    Serial.println(F("[ WARN ] Failed to parse config file"));
    fallbacktoAPMode();
  }
  //json.prettyPrintTo(Serial);
  ssid = json["ssid"].as<const char*>();
  ssidPass = json["ssidpass"].as<const char*>();
  mqttUser = json["mqttuser"].as<String>();
  mqttPass = json["mqttpass"].as<String>();
  adminPass = json["adminpass"].as<String>();
  relayType = json["relaytype"];
  topic_part = String(mqttUser) + "/" + String(devId);
  topic_sub = String(mqttUser) + "/" + String(devId) + "/ac/dev/#";
  client_MQTT.setServer(mqttServer, mqttPort);    
  client_MQTT.setCallback(callback);
  //json.prettyPrintTo(Serial);
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(BEEP1_PIN, OUTPUT);
  pinMode(BEEP2_PIN, OUTPUT);
  pinMode(SETUP_BTN_PIN, INPUT);
  digitalWrite(RELAY1_PIN, relayType);
  digitalWrite(RELAY2_PIN, relayType);
  ledcSetup(0, 300, 8);
  ledcSetup(1, 300, 8);
  ledcAttachPin(BEEP1_PIN, 0);
  ledcAttachPin(BEEP2_PIN, 1);
  if (inAPMode) {
    //fallbacktoAPMode();
  } else {
    connectSTA();
  }

  return true;
}

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DATA) {
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    String msg = "";
    if (info->final && info->index == 0 && info->len == len) {
      for (size_t i = 0; i < info->len; i++) {
        msg += (char) data[i];
      }
      DynamicJsonBuffer jsonBuffer;
      JsonObject& root = jsonBuffer.parseObject(msg);
      if (!root.success()) {
        Serial.println(F("[ WARN ] Couldn't parse WebSocket message"));
        return;
      }
      const char * cmd = root["cmd"];
      if (strcmp(cmd, "getconfig") == 0) {
        File configFile = SPIFFS.open("/auth/config.json", "r");
        if (configFile) {
          size_t len = configFile.size();
          AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
          if (buffer) {
            configFile.readBytes((char *)buffer->get(), len + 1);
            ws.textAll(buffer);
          }
          configFile.close();
        }
        return;
      }
      if (strcmp(cmd, "config")  == 0) {
        File f = SPIFFS.open("/auth/config.json", "w+");
        if (f) {
          root.prettyPrintTo(f);
          f.close();
          needReboot = true;
        }
      }
    }
  } else if (type == WS_EVT_ERROR) {
    Serial.printf("[ WARN ] WebSocket[%s][%u] error(%u): %s\r\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
  }
}

void fallbacktoAPMode() {
  inAPMode = true;
  WiFi.mode(WIFI_AP);
  Serial.print(F("[ INFO ] Configuring access point... "));
  Serial.println(WiFi.softAP(devId) ? "Ready" : "Failed!");
  IPAddress myIP = WiFi.softAPIP();
  Serial.print(F("[ INFO ] AP IP address: "));
  Serial.println(myIP);  
  server.addHandler(&ws);
  ws.onEvent(onWsEvent);
  server.onNotFound([](AsyncWebServerRequest * request) {
    AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
    response->addHeader("Location", "http://192.168.4.1");
    request->send(response);
  });
  server.serveStatic("/", SPIFFS, "/");
  server.serveStatic("/auth/", SPIFFS, "/auth/").setDefaultFile("settings.htm").setAuthentication("admin", adminPass.c_str());
  ws.setAuthentication("admin", adminPass.c_str());
  server.begin();
}
