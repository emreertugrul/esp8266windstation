#include <FS.h> //this needs to be first, or it all crashes and burns...

#include "DHT.h" //https://github.com/adafruit/DHT-sensor-library
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Ticker.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP8266httpUpdate.h>
#include "WiFiManager.h" //https://github.com/tzapu/WiFiManager
#include <DNSServer.h> 
#include <ArduinoJson.h> //https://github.com/bblanchon/ArduinoJson
#include "TimeLib.h"

char mqtt_server[60];
char mqtt_port[6];
char mqtt_user[40];
char mqtt_pass[40];
char kc_wind[4] = "0";
char windguru_uid[30];
char windguru_pass[20];
char windy_key[128];

String st;
String content;
int statusCode;

int debouncing_time = 10000;                                  // time in microseconds!
unsigned long last_micros = 0;
volatile int windimpulse = 0;

#define VERSION         "v1.93 OTA"
#define VERSIONINFO     "\n\n----------------- GAYIK Wind Station v1.93 OTA -----------------"
#define NameAP      "WindStationAP"
#define PasswordAP  "87654321"
#define FirmwareURL "http://gayikweatherstation.blob.core.windows.net/firmware/esp8266-WindStation.ino.generic.bin"   //URL of firmware file for http OTA update by secret MQTT command "flash" 

#define USE_Windguru

#define USE_Windy_com

//#define USE_Windy_app
static String WindyAppSecret = "YOUR_SECRET";
static String WindyAppID = "YOUR_ID"; 

//#define DeepSleepMODE                                        // !!! To enable Deep-sleep, you need to connect GPIO16 (D0 for NodeMcu) to the EXT_RSTB/REST (RST for NodeMcu) pin
//#define NightSleepMODE                                       // Enable deep-sleep only in night time
#if defined(DeepSleepMODE) || defined(NightSleepMODE)        // Deep-sleep mode power consumption ~6mAh (3*5=15sec work/5 min sleep), instead ~80mAh in default "Always On" mode
  #define SLEEPDAY      5                                    // deep sleep time in minutes, minimum 5min for use narodmon.com and reasonable power saving
  #define SLEEPNIGHT    10                                   
  #define TIMEZONE      2                                    // UTC offset
  #define TIMEMORNING   5                                    // night end at...
  #define TIMEEVENING   20                                   // night start at...
  #include "NtpClientLib.h" //https://github.com/gmag11/NtpClient 
#endif  

//#define MOSFETPIN       15                                   // Experemental!!! GPIO15 (D8 for NodeMcu). MosFET's gate pin for power supply sensors, off for current drain minimize. Not connect this GPIO directly to sensors you burning it! The maximum source current of GPIO is about 12mA

#define BUTTON          D3                                    // optional, GPIO4 for Witty Cloud. GPIO0/D3 for NodeMcu (Flash) - not work with deepsleep, set 4!
#define LED             D4                                    // GPIO2 for Witty Cloud. GPIO16/D0 for NodeMcu - not work with deepsleep, set 2!
#define DHTPIN          14                                   // GPIO14 (D5 for NodeMcu)
#define WINDPIN         5                                    // GPIO5 (D1 for NodeMcu)

#define MQTT_TOPIC      "windpoint"                          // mqtt topic (Must be unique for each device)
#define MQTT_TOPICm     "windpoint/m"                         
#define MQTT_TOPICo     "windpoint/o"                         

#ifdef DHTPIN
  #define DHTTYPE         DHT22                                // DHT11, DHT22, DHT21, AM2301
  DHT dht(DHTPIN, DHTTYPE);                                    //
#endif  

extern "C" { 
  #include "user_interface.h" 
}

bool sendStatus = true;
bool sensorReport = false;
bool firstRun = true;

int errors_count = 0;
int kUpdFreq = 1;  //minutes
int kRetries = 10;
int meterWind = 1;

const float kKnots = 1.94;                                  // m/s to knots conversion   
const int windPeriodSec = 10;                               // wind measurement period in seconds 1-10sec

float dhtH, dhtT, windMS = 0;
float WindMax = 0, WindAvr = 0, WindMin = 100;

String str;

unsigned long TTasks;
unsigned long secTTasks;
unsigned long count_btn = 0;

float windSpeed = 0;     // Wind speed (mph)

volatile unsigned long Rotations;     // cup rotation counter used in interrupt routine
volatile unsigned long ContactBounceTime;  // Timer to avoid contact bounce in interrupt routine
int vane_value;// raw analog value from wind vane
int Direction;// translated 0 - 360 direction
int CalDirection;// converted value with offset applied
char vaneOffset[4] = "0";  
char vaneMaxADC[5] = "1023";                                // ADC range for input voltage 0..1V

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
#define MSG_BUFFER_SIZE  (50)
char msg_buff[MSG_BUFFER_SIZE];

Ticker btn_timer;

//--------------------------------START OF CODE SECTION--------------------------------------------------------


//-------------------------------------------------------------------------------------------------------------
////////////////////////////////////Get wind speed  /////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------------------
void getWindSpeed(void)
  { 
     Rotations = 0; // Set Rotations count to 0 ready for calculations
     
     sei(); // Enables interrupts
     delay (3000); // Wait 3 seconds to average wind speed
     cli(); // Disable interrupts
     
     /* convert to mp/h using the formula V=P(2.25/T)
      V = P(2.25/30) = P * 0.075       V - speed in mph,  P - pulses per sample period, T - sample period in seconds */
     windSpeed = Rotations * 0.75; // 3 seconds
     Rotations = 0;   // Reset count for next sample

  if (windSpeed > WindMax) {
     WindMax = windSpeed;
  }
  if (WindMin > windSpeed ) {
      WindMin = windSpeed;
  }

  WindAvr = (WindMax + WindMin) * 0.5;   // average wind speed mph per 10 minutes

  meterWind = 1;
}

// This is the function that the interrupt calls to increment the rotation count
//-------------------------------------------------------------------------------------------------------------
////////////////////////////////////ISR rotation//////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------------------
void IRAM_ATTR isr_rotation()   
{
  if ((millis() - ContactBounceTime) > 15 ) {  // debounce the switch contact.
    Rotations++;
    ContactBounceTime = millis();
  }
}
// Get Wind Direction
//-------------------------------------------------------------------------------------------------------------
/////////////////////////////////// Wind direction ////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------------------
void getWindDirection(void) 
{
  vane_value = analogRead(A0); // read sensor data
  Direction = map(vane_value, 0, atoi(vaneMaxADC), 0, 359); // map it to 360 degrees
  CalDirection = Direction + atoi(vaneOffset); // factor in the offset degrees
  
  // Fix degrees:
  if(CalDirection > 360)
  CalDirection = CalDirection - 360;
  
  if(CalDirection < 0)
  CalDirection = CalDirection + 360;
}

// Convert MPH to Knots
float getKnots(float speed) {
   return speed * 0.868976;          //knots 0.868976;
}
// Convert MPH to m/s
float getms(float speed) {
   return speed * 0.44704;           //metric m/s 0.44704;;
}

//-------------------------------------------------------------------------------------------------------------
/////////////////////////////////// MQTT Received message callback ////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------------------
void callback(char* topic, byte* payload, unsigned int length) {
  String payload_string = String((char*)payload);
  payload_string = payload_string.substring(0, length);
  Serial.println("MQTT Topic: " + String(topic) + " MQTT Payload: " + payload_string);
  if (payload_string == "stat") {
  }
  else if (payload_string == "reset") {
    errors_count = 100;
  }
  else if (payload_string == "sensor") {
    if (minute()<10)
      mqttClient.publish("debug", (String(hour()) + ":0" + String(minute())).c_str());
    else
      mqttClient.publish("debug", (String(hour()) + ":" + String(minute())).c_str());  
    sensorReport = true;
  }
  else if (payload_string == "adc") {
     mqttClient.publish("debug",("ADC:" + String(analogRead(A0)) + " error:" + String(errors_count)).c_str());   
  }
  else if (payload_string == "flash") {
    flashOTA();
  }
  else { 
    // We check the topic in order to see what kind of payload we got:
    str = payload_string;
    int i = atoi(str.c_str());
    if ((i >= 0) && (i < 9999)) {
      if (String(topic) == MQTT_TOPIC) //we got kc_wind?
        strcpy(kc_wind, String(i).c_str());
      else
      if (String(topic) == MQTT_TOPICm) //we got vaneMaxADC?
        strcpy(vaneMaxADC, String(i).c_str());
      else  
      if (String(topic) == MQTT_TOPICo) //we got vaneOffset?       
        strcpy(vaneOffset, String(i).c_str());

      mqttClient.publish("debug","saving config");
      Serial.println("saving config");
      DynamicJsonDocument json(1024);
      json["mqtt_server"] = mqtt_server;
      json["mqtt_port"] = mqtt_port;
      json["mqtt_user"] = mqtt_user;
      json["mqtt_pass"] = mqtt_pass;
      json["kc_wind"] = kc_wind;
      json["windguru_uid"] = windguru_uid;
      json["windguru_pass"] = windguru_pass;
      json["windy_key"] = windy_key;
      json["vaneMaxADC"] = vaneMaxADC;
      json["vaneOffset"] = vaneOffset;
	  
      File configFile = SPIFFS.open("/config.json", "w");
      if (!configFile) {
        Serial.println("failed to open config file for writing");
      }

      serializeJson(json, Serial);
      serializeJson(json, configFile);
      configFile.close();
      
      sendStatus = true;
    }
  }
}

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void SaveParamsCallback () {
  Serial.println("Should save params");
  shouldSaveConfig = true;
}

void flashOTA() {
   Serial.println("HTTP_UPDATE FILE: " + String(FirmwareURL));
     //noInterrupts();
     detachInterrupt(digitalPinToInterrupt(WINDPIN));
     WiFiClient client;

     ESPhttpUpdate.setLedPin(LED, LOW);
     t_httpUpdate_return ret = ESPhttpUpdate.update(client, FirmwareURL);
     attachInterrupt(WINDPIN, isr_rotation, FALLING);
    
    switch (ret) {
      case HTTP_UPDATE_FAILED:
        Serial.printf("HTTP_UPDATE_FAILD Error (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
        ESP.reset();
        break;
      case HTTP_UPDATE_NO_UPDATES:
        Serial.println("HTTP_UPDATE_NO_UPDATES");
        break;
      case HTTP_UPDATE_OK:
        Serial.println("HTTP_UPDATE_OK");
        break;
     }
}

void setupSpiffs() {
  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");

        DynamicJsonDocument json(1024);
        DeserializationError error = deserializeJson(json, configFile);
        if (error) {
           Serial.print(F("deserializeJson() failed with code "));
           Serial.println(error.c_str());
        } else {
          serializeJson(json, Serial);
          Serial.println("\nparsed json");
          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(mqtt_port, json["mqtt_port"]);
          strcpy(mqtt_user, json["mqtt_user"]);
          strcpy(mqtt_pass, json["mqtt_pass"]);
          strcpy(kc_wind, json["kc_wind"]);
          strcpy(windguru_uid, json["windguru_uid"]);
          strcpy(windguru_pass, json["windguru_pass"]);
          strcpy(windy_key, json["windy_key"]);
          strcpy(vaneMaxADC, json["vaneMaxADC"]);
          strcpy(vaneOffset, json["vaneOffset"]);
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read configuration
}

void setup() {
  Serial.begin(115200);
  Serial.println(VERSIONINFO);
  Serial.print("\nESP ChipID: ");
  Serial.println(ESP.getChipId(), HEX);

  //clean FS, erase config.json in case of damaged file
  //SPIFFS.format();

  
  Rotations = 0;   // Set Rotations to 0 ready for calculations
  
  setupSpiffs();

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 60);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_user("user", "mqtt user", mqtt_user, 40);
  WiFiManagerParameter custom_mqtt_pass("pass", "mqtt password", mqtt_pass, 40);
  WiFiManagerParameter custom_kc_wind("kc_wind", "wind correction 1-999%", kc_wind, 4);
  WiFiManagerParameter custom_windguru_uid("windguru_uid", "windguru station UID", windguru_uid, 30);
  WiFiManagerParameter custom_windguru_pass("windguru_pass", "windguru pass", windguru_pass, 20);
  WiFiManagerParameter custom_windy_key("windy_key", "windy api key", windy_key, 128);
  WiFiManagerParameter custom_vaneMaxADC("vaneMaxADC", "Max ADC value 1-1024", vaneMaxADC, 5);
  WiFiManagerParameter custom_vaneOffset("vaneOffset", "Wind vane offset 0-359", vaneOffset, 4);

#ifdef MOSFETPIN
  pinMode(MOSFETPIN, OUTPUT);
  digitalWrite(MOSFETPIN, HIGH);
#endif   

  pinMode(LED, OUTPUT);
  pinMode(BUTTON, INPUT);
  pinMode(WINDPIN, INPUT_PULLUP);
  
  firstRun = true;
  btn_timer.attach(0.05, button);
  
  mqttClient.setCallback(callback);

  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);   //set config save notify callback

  std::vector<const char *> menu = {"wifi","info","param","update","close","sep","erase","restart","exit"};
  wifiManager.setMenu(menu); // custom menu, pass vector
  
#ifdef DeepSleepMODE
  wifiManager.setTimeout(60); //sets timeout until configuration portal gets turned off
#else 
  wifiManager.setTimeout(180); //sets timeout until configuration portal gets turned off
#endif   

  //add all your parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);
  wifiManager.addParameter(&custom_kc_wind);
  wifiManager.addParameter(&custom_windguru_uid);
  wifiManager.addParameter(&custom_windguru_pass);
  wifiManager.addParameter(&custom_windy_key);
  wifiManager.addParameter(&custom_vaneMaxADC);
  wifiManager.addParameter(&custom_vaneOffset);

  if(!wifiManager.autoConnect(NameAP, PasswordAP)) {
    Serial.println("failed to connect and hit timeout");
    delay(1000);
    //reset and try again, or maybe put it to deep sleep

#if defined(DeepSleepMODE) || defined(NightSleepMODE) 
    ESP.deepSleep(SLEEPNIGHT*60000000);  // Sleep for x* minute(s)
#else    
    ESP.reset();
#endif    
    delay(5000);
  } 

  //read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_user, custom_mqtt_user.getValue());
  strcpy(mqtt_pass, custom_mqtt_pass.getValue());
  strcpy(kc_wind, custom_kc_wind.getValue());
  strcpy(windguru_uid, custom_windguru_uid.getValue());
  strcpy(windguru_pass, custom_windguru_pass.getValue());
  strcpy(windy_key, custom_windy_key.getValue());
  strcpy(vaneMaxADC, custom_vaneMaxADC.getValue());
  strcpy(vaneOffset, custom_vaneOffset.getValue());
  
  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonDocument json(1024);
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["mqtt_user"] = mqtt_user;
    json["mqtt_pass"] = mqtt_pass;
    json["kc_wind"] = kc_wind;
    json["windguru_uid"] = windguru_uid;
    json["windguru_pass"] = windguru_pass;
    json["windy_key"] = windy_key;
	  json["vaneMaxADC"] = vaneMaxADC;
	  json["vaneOffset"] = vaneOffset;
    
    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    serializeJson(json, Serial);
    serializeJson(json, configFile);
    configFile.close();
    //end save parameters
  
  }  
  
  Serial.print("\nConnecting to WiFi"); 
  while ((WiFi.status() != WL_CONNECTED) && kRetries --) {
    delay(500);
    Serial.print(" .");
  }
  if (WiFi.status() == WL_CONNECTED) {
#if defined(DeepSleepMODE) || defined(NightSleepMODE) 
    NTP.begin ("pool.ntp.org", TIMEZONE, true); // ntpServerName, NTP_TIMEZONE, daylight = false
    NTP.setInterval(10, 600);  // ShortInterval, LongInterval
#endif       
    Serial.println(" DONE");
    Serial.print("IP Address is: "); Serial.println(WiFi.localIP());
    Serial.print("macAddress is: "); Serial.println(WiFi.macAddress());
    Serial.print("Connecting to ");Serial.print(mqtt_server);Serial.print(" Broker . .");
    delay(500);
    blinkLED(LED, 200, 2);
    mqttClient.setServer(mqtt_server, atoi(mqtt_port));
    mqttClient.setSocketTimeout(70);
    mqttClient.setKeepAlive(70);
    while (!mqttClient.connected()&& kRetries --) {
      String str = String(ESP.getChipId(), HEX);
      mqttClient.connect(str.c_str(), mqtt_user, mqtt_pass);
      Serial.print(" .");
      delay(1000);
    }
    if(mqttClient.connected()) {
      blinkLED(LED, 200, 3);
      Serial.println(" DONE");
      Serial.println("\n----------------------------  Logs  ----------------------------");
      Serial.println();
      mqttClient.subscribe(MQTT_TOPIC);
      mqttClient.subscribe(MQTT_TOPICm);
      mqttClient.subscribe(MQTT_TOPICo);
      mqttClient.publish("start", ("Version:" + String(VERSION)).c_str());
      blinkLED(LED,1000,5);
    }
    else {
      Serial.println(" FAILED!");
      Serial.println("\n----------------------------------------------------------------");
      Serial.println();
    }
  }
  else {
    Serial.println(" WiFi FAILED!");
    Serial.println("\n----------------------------------------------------------------");
    Serial.println();
  }
  attachInterrupt(WINDPIN, isr_rotation, FALLING);
}

void loop() {  
  mqttClient.loop();
  timedTasks();
  checkStatus();
  if (sensorReport) {
    getSensors();
  }
}

void blinkLED(int pin, int duration, int n) {             
  for(int i=0; i<n; i++)  {  
    digitalWrite(pin, HIGH);        
    delay(duration);
    digitalWrite(pin, LOW);
    delay(duration);
  }
}

void button() {
  if (!digitalRead(BUTTON)) {
    count_btn++;
  } 
  else {
    if (count_btn > 400){
      Serial.println("\n\nESP8266 Rebooting . . . . . . . . Please Wait"); 
      errors_count = 100;
    } 
    count_btn=0;
  }
}

void checkConnection() {
  if (WiFi.status() == WL_CONNECTED)  {
    if (mqttClient.connected()) {
      Serial.println("mqtt broker connection . . . . . . . . . . OK");
    } 
    else {
      errors_count = errors_count + 5;
      Serial.println("mqtt broker connection . . . . . . . . . . LOST errors_count = " + String(errors_count));
    }
  }
  else { 
    errors_count = errors_count + 10;
    Serial.println("WiFi connection . . . . . . . . . . LOST errors_count = " + String(errors_count));
  }
}

void checkStatus() {
  if (sendStatus) {
    mqttClient.publish("kc_wind", String(kc_wind).c_str());
    mqttClient.publish("adc", ("{\"ADC\":" + String(analogRead(A0)) +", "+"\"MaxADC\":" + String(vaneMaxADC) + ", " + "\"Offset\":"+String(vaneOffset)+ "}").c_str()); 
    sendStatus = false;
  }
  if (errors_count >= 100) {
    blinkLED(LED, 200, 5);
    ESP.restart();
    delay(500);
  }
}

void getSensors() {
  String pubString;
#ifdef DHTPIN
  Serial.print("DHT read . . . . . . . . . . . . . . . . . ");  
  dhtH = dht.readHumidity();
  dhtT = dht.readTemperature();
  if (isnan(dhtH) || isnan(dhtT)) {
    if (mqttClient.connected())
      mqttClient.publish("debug", "DHT READ ERROR");
    Serial.println("ERROR");
  } else {
    pubString = "{\"Temp\": "+String(dhtT)+", "+"\"Humidity\": "+String(dhtH) + "}";
    pubString.toCharArray(msg_buff, pubString.length()+1);
    if (mqttClient.connected())
      mqttClient.publish("temp", msg_buff);
    Serial.println("OK");
  }
#endif 

#ifdef DeepSleepMODE
  if (mqttClient.connected())
    mqttClient.publish("debug", "ADC:" + String(a0) + " " + NTP.getTimeDateString ());
#endif     
  
  if (meterWind > 0) { //already made measurement wind power
    pubString = "{\"Min\": "+String(getKnots(WindMin), 2)+", "+"\"Avr\": "+String(getKnots(WindAvr), 2)+", "+"\"Max\": "+String(getKnots(WindMax), 2)+", "+"\"Dir\": "+String(CalDirection) + "}";
    pubString.toCharArray(msg_buff, pubString.length()+1);
    if (mqttClient.connected())
      mqttClient.publish("wind", msg_buff);
      Serial.print(" Wind Min: " + String(getKnots(WindMin), 2) + " Avr: " + String(getKnots(WindAvr), 2) + " Max: " + String(getKnots(WindMax), 2) + " Dir: " + String(CalDirection)+ " (offset:" + String(vaneOffset) + ") ");
  }
  sensorReport = false;
  blinkLED(LED,200,1);
}

void timedTasks() {
  getWindSpeed();  
  getWindDirection();

#ifdef DeepSleepMODE
  if (meterWind == 3) { //after 3 measurements send data and go to sleep
    getSensors();
    SendData();
    //-------------------------------------------
    time_t time_sync = NTP.getLastNTPSync ();
    //int hours = (time_sync / 3600) % 24;
    int hours =  hour();
    Serial.print (NTP.getTimeDateString ()); Serial.print (". ");
    Serial.print (NTP.isSummerTime () ? "Summer Time. " : "Winter Time. ");
    //-------------------------------------------
    if ((time_sync!=0) && ((hours < TIMEMORNING) || (hours >= TIMEEVENING)) ){
      Serial.println("Good night sleep");
      ESP.deepSleep(SLEEPNIGHT*60000000);  // Sleep for night SLEEPTIME*1 minute(s)
    }  else {
      Serial.println("Good day sleep");
      ESP.deepSleep(SLEEPDAY*60000000);  // Sleep for day SLEEPTIME*1 minute(s)
    }  
    delay(500);
  }
#endif

#ifdef NightSleepMODE
  if (meterWind == 3) { //after 3 measurements send data and go to sleep, but only in night time!
    time_t time_sync = NTP.getLastNTPSync ();
    if (time_sync!=0) {
      //int hours = (time_sync / 3600) % 24;
      int hours =  hour();
      Serial.print ("Hours: " + String(hours));
      if ((hours < TIMEMORNING) || (hours >= TIMEEVENING)){
        getSensors();        
        SendData();       
        Serial.println(" Good night sleep");
        ESP.deepSleep(SLEEPNIGHT*60000000);  // Sleep for night SLEEPTIME*1 minute(s);
        delay(500);
      }
    } else {
      if (millis() > 120000) {
        NTP.getTime(); //if after 2 min of working not any time sync try force update
      }
    }
  }
#endif

  //kUpdFreq minutes timer
  if ((millis() > TTasks + (kUpdFreq*60000)) || (millis() < TTasks)) { 
    if ((WindMax > 2) && (WindMin == WindMax)) errors_count = errors_count + 25; // check for freeze CPU
    TTasks = millis();
    checkConnection();
    //sensorReport = true;
    getSensors();
    SendData();  
  }
  
}

void SendData() {

#ifdef USE_Windguru
     if (!SendToWindguru()) errors_count++;
#endif
#ifdef USE_Windy_com
     if (!SendToWindyCom()) errors_count++;
#endif
#ifdef USE_Windy_app
     if (!SendToWindyApp()) errors_count++;
#endif

  ResetCounters(); 
}

void ResetCounters() {
    WindMax = 0;
    WindMin= 100;
}


//-------------------------------------------------------------------------------------------------------------
/////////////////////////////////// SEND TO WINDGURU //////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------------------
bool SendToWindguru() { // send info to windguru.cz
  WiFiClient client;
  HTTPClient http; //must be declared after WiFiClient for correct destruction order, because used by http.begin(client,...)
  String getData = "", Link;
  unsigned long time;
  
  if (WiFi.status() == WL_CONNECTED) { //Check WiFi connection status
     Link = "http://www.windguru.cz/upload/api.php?";
     time = millis();
     
     //--------------------------md5------------------------------------------------
     MD5Builder md5;
     md5.begin();
     md5.add(String(time) + String(windguru_uid) + String(windguru_pass));
     md5.calculate();
     if (meterWind > 0)
       getData = "uid=" + String(windguru_uid) + "&salt=" + String(time) + "&hash=" + md5.toString() + "&interval=" + String(meterWind * windPeriodSec) + "&wind_min=" + String(getKnots(WindMin),2) + "&wind_avg=" + String(getKnots(WindAvr),2) + "&wind_max=" + String(getKnots(WindMax),2);
     //wind_direction     wind direction as degrees (0 = north, 90 east etc...) 
     getData = getData + "&wind_direction=" + String(CalDirection);
#ifdef DHTPIN   
     if (!isnan(dhtT)) getData = getData + "&temperature=" + String(dhtT);
     if (!isnan(dhtH)) getData = getData + "&rh=" + String(dhtH);
#endif     
     Serial.println(Link + getData);
     http.begin(client, Link + getData);            //Specify request destination
     int httpCode = http.GET();             //Send the request
     if (httpCode > 0) {                    //Check the returning code
       String payload = http.getString();   //Get the request response payload
       Serial.println(payload);             //Print the response payload
       if (mqttClient.connected() && (payload != "OK"))
         mqttClient.publish("debug", payload.c_str());
     }
     http.end();   //Close connection
   } else  {
      Serial.println("wi-fi connection failed");
      return false; // fail;
   }
  
  return true; //done
}


//-------------------------------------------------------------------------------------------------------------
//////////////////////////////////// SEND TO WINDY ////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------------------
//https://stations.windy.com/pws/update/XXX-API-KEY-XXX?winddir=230&windspeedmph=12&windgustmph=12&tempf=70&rainin=0&baromin=29.1&dewptf=68.2&humidity=90
//We will be displaying data in 5 minutes steps. So, it's not nessary send us data every minute, 5 minutes will be fine.
//https://community.windy.com/topic/8168/report-your-weather-station-data-to-windy  
bool SendToWindyCom() { // send info to http://stations.windy.com/stations
  WiFiClient client;
  HTTPClient http; //must be declared after WiFiClient for correct destruction order, because used by http.begin(client,...)
  String getData, Link;
  unsigned long time;
  
  if (String(windy_key).isEmpty()) {
    Serial.println("Windy API key is empty, skipping data send.");
    return true;
  }
  
  if (WiFi.status() == WL_CONNECTED) { //Check WiFi connection status
     Link = "http://stations.windy.com/pws/update/" + String(windy_key) + "?";
    
     //wind speed during interval (knots)
     if (meterWind > 0)
       getData = "winddir=" + String(CalDirection) + "&wind=" + String(WindAvr/meterWind, 2) + "&gust=" + String(WindMax, 2);
     
#ifdef DHTPIN   
     if (!isnan(dhtT)) getData = getData + "&temp=" + String(dhtT);
     if (!isnan(dhtH)) getData = getData + "&rh=" + String(dhtH);
#endif     
     Serial.println(Link + getData);
     http.begin(client, Link + getData);     //Specify request destination
     int httpCode = http.GET();             //Send the request
     if (httpCode > 0) {                    //Check the returning code
       String payload = http.getString();   //Get the request response payload
       Serial.println(payload);             //Print the response payload
       if (mqttClient.connected() && (payload.indexOf("SUCCESS") == -1))
         mqttClient.publish("debug", payload.c_str());
     }
     http.end();   //Close connection
   } else  {
      Serial.println("wi-fi connection failed");
      return false; // fail;
   }
    
  return true; //done
}


//-------------------------------------------------------------------------------------------------------------
//////////////////////////////////// SEND TO WINDYAPP /////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------------------
//https://windyapp.co/apiV9.php?method=addCustomMeteostation&secret=WindyAPPSecret&d5=123&a=11&m=10&g=15&i=test1
//d5* - direction from 0 to 1024. direction in degrees is equal = (d5/1024)*360
//a* - average wind per sending interval. for m/c - divide by 10
//m* - minimal wind per sending interval. for m/c - divide by 10
//g* - maximum wind per sending interval. for m/c - divide by 10
//i* - device number
 
bool SendToWindyApp() { // send info to http://windy.app/
  const char* host = "windyapp.co"; // only google.com not https://google.com
  String getData= "", Link;
  
  if (WiFi.status() == WL_CONNECTED) { //Check WiFi connection status
     Link = "/apiV9.php?method=addCustomMeteostation&secret=" + WindyAppSecret + "&i=" + WindyAppID +"&";
     if (meterWind > 0)
       getData = "d5=" + String(map(CalDirection, 0, 359, 1, 1024)) + "&m=" + String(WindMin *10, 0) + "&a=" + String(WindAvr/meterWind *10, 0) + "&g=" + String(WindMax *10, 0);
     
#ifdef DHTPIN   
     if (!isnan(dhtT)) getData = getData + "&t2=" + String(dhtT);
     if (!isnan(dhtH)) getData = getData + "&h=" + String(dhtH);
#endif     
     getData.replace(" ", "");  
     
     // Use WiFiClient class to create TCP connections
     WiFiClientSecure httpsClient;
     const int httpPort = 443; // 80 is for HTTP / 443 is for HTTPS!
     httpsClient.setInsecure(); // this is the magical line that makes everything work
     if (!httpsClient.connect(host, httpPort)) { //works!
       Serial.println("https connection failed");
       if (mqttClient.connected())
         mqttClient.publish("debug", "windyapp.co https connection failed");
       return false;
     }

     // We now create a URI for the request
     String url = Link + getData;

     // This will send the request to the server
     Serial.println("request sent: " + url);
     httpsClient.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" + 
               "Connection: close\r\n\r\n");
                  
     while (httpsClient.connected()) {
       String line = httpsClient.readStringUntil('\n');
       if (line == "\r") {
         Serial.println("headers received");
         break;
       }
     }

     String payload;
     while(httpsClient.available()){        
       payload = httpsClient.readString();  //Read Line by Line
     }
     payload.replace("\n", "/");
     Serial.println("reply was: " + payload); //Print response
     if (mqttClient.connected() && (payload.indexOf("success") == -1)) {
       payload = getData + "> " +payload;
       mqttClient.publish("debug", payload.c_str());
     }

   } else  {
      Serial.println("wi-fi connection failed");
      return false; // fail;
   }
    
  return true; //done
}
