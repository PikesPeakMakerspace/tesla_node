/*****

  TESLA Tool Node

*****/
#include <Arduino.h>

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include <Wire.h>
#include <SerLCD.h>
#include <Wiegand.h>

//Firmware version. This is sent when requested. Helpful for tech support.
const byte firmwareVersionMajor = 0;
const byte firmwareVersionMinor = 7;

const int myNumber = 3;

#define Is_a_Door 0
#define DEBUG 1

// Header stuff

void setup();
void setup_lcd();
void setup_cardreader();
void setup_wifi();
void connectToMQTT();
void loop();
void requestAccess(int long card_no);
#if Is_a_Door == 0
void sendLogout(int logoutType);
void sendStatus();
#endif
void teslaReset();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void stateChanged(bool plugged, const char* message);
void pinStateChanged();
void receivedData(uint8_t* data, uint8_t bits, const char* message);
void receivedDataError(Wiegand::DataError error, uint8_t* rawData, uint8_t rawBits, const char* message);


#if DEBUG == 1
#define debug(x) Serial.print(x)
#define debugln(x) Serial.println(x)
#else
#define debug(x)
#define debugln(x)
#endif


#define DISPLAY_ADDRESS1 0x72 //This is the default address of the OpenLCD

// Pins connected to the Wiegand D0 and D1 signals.
#define CARDREADER_D0 36
#define CARDREADER_D1 39

// Relay and switch pin def's
#define RELAY 2
#define DONEBUTTON 13

// Wifi credentials and network IP addresses
// Currently static, may need a way to use preferences and an initialization procedure

//const char* ssid = "DangerNet";
//const char* password = "8324836205";
//const char* mqttServer = "192.168.1.132";

const char* ssid = "pikespeakmakerspace";
const char* password = "weliketomake2016";
// const char* mqttServer = "192.168.23.164";
const char* mqttServer = "10.8.0.1";

const int mqttPort = 1883;
//const char* mqttUser = "yourInstanceUsername";
//const char* mqttPassword = "yourInstancePassword";



// subscribed MQTT topics
String permissionTopic;
#if Is_a_Door == 0
String lockdownTopic;
String queryTopic;
#endif

// published MQTT topics
String accessTopic;
#if Is_a_Door == 0
String logoutTopic;
String statusTopic;
#endif

// define objects

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// The object that handles the wiegand protocol for the HID card reader
Wiegand wiegand;
SerLCD lcd;


// Timers & auxiliary variables
long now = millis();
long lastMeasure = 0;
boolean Enabled = false;
boolean lockedOut = false;
int long currentUser = 0;
int long cardLastSent = 0;
int currentStatus;
int intervalSeconds; // heartbeat interval

void setup() {
  pinMode(RELAY, OUTPUT);
  pinMode(DONEBUTTON, INPUT_PULLUP);

  // embed tool number in MQTT topics subscribed to
  permissionTopic = "tesla/tool/tool";
  permissionTopic += myNumber;
  permissionTopic += "/permission";

  #if Is_a_Door == 0
  lockdownTopic = "tesla/tool/command/lockdown";

  queryTopic = "tesla/tool/command/query";
  #endif


  // embed tool number in MQTT topics to be published
  accessTopic = "tesla/tool/tool";
  accessTopic += myNumber;
  accessTopic += "/access";
  
  #if Is_a_Door == 0
  logoutTopic = "tesla/tool/tool";
  logoutTopic += myNumber;
  logoutTopic += "/logout";
  
  statusTopic = "tesla/tool/tool";
  statusTopic += myNumber;
  statusTopic += "/status";
  #endif

  setup_lcd();
  delay(1000);
  
  Serial.begin(115200);
  setup_wifi();
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  setup_cardreader();
  teslaReset();

}

void setup_lcd() {
  // setup display
  Wire.begin(); //Join the bus as master

  //By default .begin() will set I2C SCL to Standard Speed mode of 100kHz
  Wire.setClock(400000); //Optional - set I2C SCL to High Speed Mode of 400kHz
  lcd.begin(Wire);

  lcd.setFastBacklight(255, 255, 255); //Set backlight to bright white
  lcd.setContrast(5); //Set contrast. Lower to 0 for higher contrast.

}

void setup_cardreader() {
  //Install listeners and initialize Wiegand reader
  wiegand.onReceive(receivedData, "Card read: ");
  wiegand.onReceiveError(receivedDataError, "Card read error: ");
  wiegand.onStateChange(stateChanged, "State changed: ");
  wiegand.begin(Wiegand::LENGTH_ANY, true);

  //initialize pins as INPUT and attaches interruptions
  pinMode(CARDREADER_D0, INPUT);
  pinMode(CARDREADER_D1, INPUT);
  attachInterrupt(digitalPinToInterrupt(CARDREADER_D0), pinStateChanged, CHANGE);
  attachInterrupt(digitalPinToInterrupt(CARDREADER_D1), pinStateChanged, CHANGE);

  //Sends the initial pin state to the Wiegand library
  pinStateChanged();

}


// Don't change the function below. This functions connects your ESP32 to your router
void setup_wifi() {

  lcd.setFastBacklight(255, 255, 0); //Set backlight to yellow
  lcd.clear(); //Clear the display - this moves the cursor to home position as well
  lcd.print("Connecting");

  delay(10);
  // We start by connecting to a WiFi network
  debugln();
  debug("Connecting to ");
  debugln(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // if WiFi isn't working, we have a problem here
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    debug(".");
  }
  debugln("");

  debug("WiFi connected - ESP IP address: ");
  debugln(WiFi.localIP());
}


// This functions reconnects the ESP32 to the MQTT broker
// Change the function below to subscribe to more topics
void connectToMQTT() {
  // Loop until reconnected
  while (!mqttClient.connected()) {
    debug("Attempting MQTT connection...");
    // Each client (tool) needs a unique clientId
    String clientId = "TESLA-TOOL-";
    clientId += String(myNumber);
    debugln(clientId.c_str());

    // Attempt to connect

    if (mqttClient.connect(clientId.c_str())) {
      debugln("connected");
      
      // Subscribe or resubscribe to topics here
      // You can subscribe to more topics (to control more LEDs in this example)
      // String permissionTopic = "tesla/tool/tool<mynumber>/permission" and is
      // the expected response from a published "access" request

      mqttClient.subscribe(permissionTopic.c_str());
      #if Is_a_Door == 0
      mqttClient.subscribe(lockdownTopic.c_str());
      mqttClient.subscribe(queryTopic.c_str());
      #endif

    } else {
      // will hang here waiting for MQTT server to connect
      debug("failed, rc=");
      debug(mqttClient.state());
      debugln(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

// mqttCallback is executed when a message is published to a topic that we are subscribed to
// Program logic in response to received message goes here
// Respond to various topics as needed

void mqttCallback(char* topic, byte* payload, unsigned int length) {

  char str[length+1];
  debug("Message arrived [");
  debug(topic);
  debug("] ");
  int i=0;
  for (i=0;i<length;i++) {
    debug((char)payload[i]);
    str[i]=(char)payload[i];
  }
  str[i] = 0; // Null termination
  debugln();

  // if using JSON, this would be the place to decode it
  // or maybe jump to a routine to handle.  probably belongs here . . .
  StaticJsonDocument <256> doc;

  DeserializationError error = deserializeJson(doc, payload);

  // Test if parsing succeeds.
  if (error) {
    debug(F("deserializeJson() failed: "));
    debugln(error.f_str());
    return;
  }


  if (String(topic) == permissionTopic) {
    debugln("Response to access request");
  
    const char* user = doc["user"];
    const char* tool = doc["tool"];
    bool enable = doc["enable"];
  
    debugln(str);
    debug("User: ");
    debugln(user);
    debug("Tool: ");
    debugln(tool);
    debug("Enable: ");
    debugln(enable);
  
    debugln();

    lcd.clear();
    lcd.setCursor(8-(strlen(user)>>1),0);
    lcd.print(user);
  
    lcd.setCursor(4,1);

    if (enable) {
      #if Is_a_Door == 0
      if (Enabled && (cardLastSent != currentUser)) {
        sendLogout(2);  // 2 => logout on user change but don't drop relay
      }
      #endif

      lcd.setFastBacklight(0, 255, 0); //bright green
      digitalWrite(RELAY, HIGH);
      #if Is_a_Door == 1
      lastMeasure = millis(); // start door open counter
      #endif
      Enabled = true;
      lcd.print("Enabled");
      currentStatus = 1;
      currentUser = cardLastSent;
    } else {
      lcd.setFastBacklight(255, 0, 0); //bright red
      digitalWrite(RELAY, LOW);
      lastMeasure=millis();
      Enabled = false;
      lcd.print("Rejected");
      currentUser = 0;
    }
  #if Is_a_Door == 0
  } else if (String(topic) == queryTopic) {
  // query - admin request for status message, and optionally sets an expected status reporting interval in the node
  //
  // topic: "tesla/tool/command/query"
  // payload: {"tool_id":nodeNumber,"interval":newInterval}
  //   if nodeNumber == myNumber or node == -1 (global query)
  //     send status message
  //     if intervalSeconds = -1, no change to periodic status
  //                    = 0, turn off periodic status
  //                    > 0, set periodic status to setInterval seconds and turn on
    debugln("Query from admin");
 
    int tool = doc["tool_id"];
    int newInterval = doc["interval"];

    if ((tool == myNumber) || (tool == -1)) {
      sendStatus();
      if (newInterval == 0)  {
        intervalSeconds = 0;
        // stop timer
      } else if (newInterval > 0) {
        intervalSeconds = newInterval;
        // clear timer stuff
      }
    }

  } else if (String(topic) == lockdownTopic) {  
      debugln("Lockdown request"); 
  
      int tool = doc["tool_id"];
      int lockdownType = doc["lockdownType"];

      if ((tool == myNumber) || (tool == -1)) {
        if ((lockdownType == 0) && lockedOut) {
          teslaReset();
        }
        else if (lockdownType == 1) {
          lockedOut = true;
          Enabled = false;
          currentStatus = -1;
          lcd.setFastBacklight(255, 165, 0); //orange
          digitalWrite(RELAY, LOW);
          lcd.clear();
          lcd.setCursor(5,0);
          lcd.print("Locked");
          lcd.setCursor(7,1);
          lcd.print("Out");
        }
      }
        
  #endif
  } else { 
    debugln("unrecognized message");
  }
}


// 
void loop() {
  // TODO: The DONE button should send a message to the mqtt broker to indicate done condition
  #if Is_a_Door == 0
  if ((digitalRead(DONEBUTTON) == 0) && (Enabled == true)) {
    
    sendLogout(1); // 1 => logout is from done button
    teslaReset();
  }
  #endif

  // This is the non-blocking timer for rejected/unauthorized access attempt
  if (lastMeasure != 0) {
    now=millis();
    #if Is_a_Door == 0
    if (now - lastMeasure > 3000) {
      lastMeasure=0;
      teslaReset();
    }
    #else
    if (now - lastMeasure > 2000) {
      lastMeasure=0;
      teslaReset();
    }
    #endif
  }

  // Support for the card reader
  noInterrupts();
  wiegand.flush();
  interrupts();

  // MQTT requires a constant connection
  if (!mqttClient.connected()) {
    connectToMQTT();
  }
  mqttClient.loop();
}

// When either of the card reader data pins have changed, update the state of the wiegand library
void pinStateChanged() {
  wiegand.setPin0State(digitalRead(CARDREADER_D0));
  wiegand.setPin1State(digitalRead(CARDREADER_D1));
}

// Notifies when a reader has been connected or disconnected.
// This is mostly diagnostic for pluggable card readers
void stateChanged(bool plugged, const char* message) {
  debug(message);
  debugln(plugged ? "CONNECTED" : "DISCONNECTED");
}

// Notifies when a card was read.
void receivedData(uint8_t* data, uint8_t bits, const char* message) {
  // Diagnostic information
  debug(message);
  debug(bits);
  debug(" bits / ");
  debug(data[0]); // facility number
  debug(" ");
  debug ((256*data[1])+data[2]);  // card number
  debugln();

  // Check card number in database (note that the facility number is NOT currently used)
  requestAccess((256*data[1])+data[2]);
    
}

// Notifies when an invalid transmission is detected (there may be false calls to this, need to verify)
void receivedDataError(Wiegand::DataError error, uint8_t* rawData, uint8_t rawBits, const char* message) {
    
  debug(message);
  debug(Wiegand::DataErrorStr(error));
  debug(" - Raw data: ");
  debug(rawBits);
  debug("bits / ");

  //Print value in HEX
 #if DEBUG == 1 
  uint8_t bytes = (rawBits+7)/8;
  for (uint8_t i=0; i<bytes; i++) {
      Serial.print(rawData[i] >> 4, 16);
      Serial.print(rawData[i] & 0xF, 16);
  }
  Serial.println();
 #endif 
}


void requestAccess(int long card_no) {
  StaticJsonDocument<256> JSONDoc;
  // if node is lockedOut, don't send card info to access server
  if (lockedOut == false) {

    JSONDoc["tool_id"] = myNumber;
    JSONDoc["card_id"] = card_no;

    cardLastSent = card_no;

    debugln("Sending message to MQTT topic..");
    serializeJson(JSONDoc, Serial);
    char out[128];
    serializeJson(JSONDoc, out);
  
    // String accessTopic = "tesla/tool/tool<myNumber>/access"

    if (mqttClient.publish(accessTopic.c_str(), out) == true) {
      debugln("Success sending message");
    } else {
      debugln("Error sending message");
    }
  }
}

#if Is_a_Door == 0
  // logout - send user logout information to admin. 3 types of logout (node reset, done button, user change)
  //
  // topic: tesla/tool/tool<myNumber>/logout  
  // payload: {"tool_id":myNumber,"card_id":currentUserCardID,"logout_type",logoutType}
  //   where logoutType = 0 (node reset), = 1 (done button), = 2 (user change/new card scan)
  //   node reset happens at power-on, card_id will be 0
 

void sendLogout(int logoutType) {
  StaticJsonDocument<256> JSONDoc;

  JSONDoc["tool_id"] = myNumber;
  JSONDoc["card_id"] = currentUser;
  JSONDoc["logout_type"] = logoutType;

  debugln("Sending message to MQTT topic..");
  serializeJson(JSONDoc, Serial);
  char out[128];
  serializeJson(JSONDoc, out);
  
  // String accessTopic = "tesla/tool/tool<myNumber>/logout"

  if (mqttClient.publish(logoutTopic.c_str(), out) == true) {
    debugln("Success sending message");
  } else {
    debugln("Error sending message");
  }
}

// status - current state of the TESLA node, send in response to "query" or as a periodic message (heartbeat)
//
// topic: "tesla/tool/tool<myNumber>/status" 
// payload: {"tool_id":myNumber,"card_id":currentUserCardID,"status":currentStatus}
//   where currentStatus  = -1 (lockdown), = 0 (idle), = 1 (enabled), = 2 (in use, i.e. power on), = 3 (error), = 4 (end of run)
//         currentUserCardID = card_id for current user, else 0 if no user


void sendStatus() {
  StaticJsonDocument<256> JSONDoc;
  
  JSONDoc["tool_id"] = myNumber;
  JSONDoc["card_id"] = currentUser;
  JSONDoc["status"] = currentStatus;
  
  debugln("Sending message to MQTT topic..");
  serializeJson(JSONDoc, Serial);
  char out[128];
  serializeJson(JSONDoc, out);
  
  // String accessTopic = "tesla/tool/tool<myNumber>/status"

  if (mqttClient.publish(statusTopic.c_str(), out) == true) {
    debugln("Success sending message");
  } else {
    debugln("Error sending message");
  }

}
#endif

void teslaReset() {
  lcd.setFastBacklight(255, 255, 255); //Set backlight to bright white
  lcd.clear(); //Clear the display - this moves the cursor to home position as well
  lcd.print(F(" TESLA Ver "));
  lcd.print(firmwareVersionMajor);
  lcd.print(F("."));
  lcd.print(firmwareVersionMinor);
  currentUser = 0;
  lastMeasure = 0;
  digitalWrite(RELAY, LOW);
  Enabled = false;
  lockedOut = false;
  // sendLogout(0);
  currentStatus = 0; // idle
}