/*  ==============================================================================================================
    MrDIY is an locally-controlled over MQTT, internet-independent media notifier that can play MP3s, stream
    icecast radios, read text and play RTTTL ringtones without any external components.

    Connect your audio jack to Rx and GND - an external amplifier if required. You can build a simple amp
    with a single 1kΩ resistor and an NPN 2N3904 transistor, like this:


                                        2N3904 (NPN)
                                       +---------+
                                       |         |     +--|
                                       | E  B  C |    / S |
                                       +-|--|--|-+    | P |
                                         |  |  +------+ E |
                                         |  |         | A |
           ESP8266-GND ------------------+  |  +------+ K |
                                            |  |      | E |
           ESP8266-I2SOUT (Rx) -----/\/\/\--+  |      \ R |
                                     1kΩ       |       +--|
           USB 5V -----------------------------+


    Commands, to:

     - Play MP3              MQTT topic: "mqttFullTopic()/play"
                             MQTT load: http://url-to-the-mp3-file/file.mp3

     - Play Icecast Stream   MQTT topic: "mqttFullTopic()/stream"
                             MQTT load: http://url-to-the-icecast-stream/file.mp3, example: http://22203.live.streamtheworld.com/WHTAFM.mp3

     - Play Ringtone         MQTT topic: "mqttFullTopic()/tone"
                             MQTT load: RTTTL formated text, example: Soap:d=8,o=5,b=125:g,a,c6,p,a,4c6,4p,a,g,e,c,4p,4g,a

     - Say Text              MQTT topic: "mqttFullTopic()/say"
                             MQTT load: Text to be read, example: Hello There. How. Are. You?

     - Change Volume         MQTT topic: "mqttFullTopic()/volume"
                             MQTT load: a double between 0.00 and 1.00, example: 0.7

     - Stop Playing         MQTT topic: "mqttFullTopic()/stop"

     To get status:

     - The notifier sends status update on this MQTT topic: "mqttFullTopic()/status"

                  "playing"       either paying an mp3, streaming, playing a ringtone or saying a text
                  "idle"          waiting for a command
                  "error"         error when receiving a command: example: MP3 file URL can't be loaded
                  "connected"     device just connected to MQTT server

     - The notifier plays a 2 second audio clip when it is first booted and connected to Wifi & MQTT


    To Upload to Wesmo D1 Mini:

     - Set CPU Frequency to 160MHz
     - Set IwIP to V2 Higher Bandwidth
     - Update the SSID info
     - Connect Rx to an AMP

    Dependencies:

     - ESP8266          https://github.com/esp8266/Arduino
     - ESP8266Audio     https://github.com/earlephilhower/ESP8266Audio
     - ESP8266SAM       https://github.com/earlephilhower/ESP8266SAM
     - IotWebConf       https://github.com/prampec/IotWebConf
     - PubSubClient     https://github.com/knolleary/pubsubclient


    Many thanks to all the authors and contributors to the above libraries - you have done an amazing job!

    For more info visit me at MrDIY.ca and find the Notifier Video on my YouTube channel:
    https://youtu.be/SPa9SMyPU58

  ============================================================================================================== */

//#define DEBUG_FLAG

#define USE_I2S //uncomment to use I2S DAC instead of Serial Rx pin.

#include "Arduino.h"
#include "boot_sound.h"
#include "ESP8266WiFi.h"
#include "PubSubClient.h"
#include "AudioFileSourceHTTPStream.h"
#include "AudioFileSourceICYStream.h"
#include "AudioFileSourcePROGMEM.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorWAV.h"
#include "AudioGeneratorRTTTL.h"

#ifdef USE_I2S
  #include "AudioOutputI2S.h"
#else
  #include "AudioOutputI2SNoDAC.h"
#endif

#include "ESP8266SAM.h"
#include "IotWebConf.h"

AudioGeneratorMP3         *mp3 = NULL;
AudioGeneratorWAV         *wav = NULL;
AudioGeneratorRTTTL       *rtttl = NULL;
AudioFileSourceHTTPStream *file_http = NULL;
AudioFileSourcePROGMEM    *file_progmem = NULL;
AudioFileSourceICYStream *file_icy = NULL;
AudioFileSourceBuffer     *buff = NULL;

#ifdef USE_I2S
  AudioOutputI2S       *out = NULL;
#else
  AudioOutputI2SNoDAC       *out = NULL;
#endif

WiFiClient                wifiClient;
PubSubClient              mqttClient(wifiClient);
#define  port             1883
#define  MQTT_MSG_SIZE    256

#define LED_Pin 0 // for the external LED pin.

// AudioRelated ---------------------------
float volume_level              = 0.8;
String playing_status;
const int preallocateBufferSize = 2048;
void *preallocateBuffer         = NULL;
byte i;

//Get chip ID to append to ThingName to make unique
#ifdef ESP8266
String ChipId = String(ESP.getChipId(), HEX);
#elif ESP32
String ChipId = String((uint32_t)ESP.getEfuseMac(), HEX);
#endif

// WifiManager -----------------------------
String thingName = String("MrDIY Notifier - ") + ChipId; 
#define wifiInitialApPassword "mrdiy.ca"
char mqttServer[16];
char mqttUserName[32];
char mqttUserPassword[32];
char mqttTopicPrefix[32];
char mqttTopic[MQTT_MSG_SIZE];
DNSServer             dnsServer;
WebServer             server(80);
IotWebConf            iotWebConf(thingName.c_str(), &dnsServer, &server, wifiInitialApPassword, "JamesOGorman");
IotWebConfParameter   mqttServerParam = IotWebConfParameter("MQTT server", "mqttServer", mqttServer, sizeof(mqttServer) );
IotWebConfParameter   mqttUserNameParam = IotWebConfParameter("MQTT username", "mqttUser", mqttUserName, sizeof(mqttUserName));
IotWebConfParameter   mqttUserPasswordParam = IotWebConfParameter("MQTT password", "mqttPass", mqttUserPassword, sizeof(mqttUserPassword), "password");
IotWebConfParameter   mqttTopicParam = IotWebConfParameter("MQTT Topic", "mqttTopic", mqttTopicPrefix, sizeof(mqttTopicPrefix));

//Last Will and Testament (LWT)
byte willQoS = 0;
char* willTopic = "LWT";
const char* willMessage ="Offline";
boolean willRetain = false;

/* ################################## Setup ############################################# */

void setup() {
  pinMode(LED_Pin, OUTPUT); // LED pin
  analogWrite(LED_Pin, 100);

#ifdef DEBUG_FLAG
  Serial.begin(115200);
#endif
  iotWebConf.addParameter(&mqttServerParam);
  iotWebConf.addParameter(&mqttUserNameParam);
  iotWebConf.addParameter(&mqttUserPasswordParam);
  iotWebConf.addParameter(&mqttTopicParam);
  iotWebConf.setWifiConnectionCallback(&wifiConnected);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setStatusPin(LED_BUILTIN);
  iotWebConf.skipApStartup();

  boolean validConfig = iotWebConf.init();
  if (!validConfig)
  {
    mqttServer[0] = '\0';
    mqttUserName[0] = '\0';
    mqttUserPassword[0] = '\0';
  }

  server.on("/", [] { iotWebConf.handleConfig(); });
  server.onNotFound([] {  iotWebConf.handleNotFound();  });
#ifdef USE_I2S
  out = new AudioOutputI2S();
  Serial.println("Using I2S output");
#else
  out = new AudioOutputI2SNoDAC(); 
  Serial.println("Using No DAC - using Serial port Rx pin");
#endif
  out->SetGain(volume_level);
}

/* ##################################### Loop ############################################# */

void loop() {

  mqttReconnect();
  mqttClient.loop();
  // give processor priority to audio
  if (!mp3) iotWebConf.doLoop();
  if (mp3   && !mp3->loop())    stopPlaying();
  if (wav   && !wav->loop())    stopPlaying();
  if (rtttl && !rtttl->loop())  stopPlaying();

#ifdef DEBUG_FLAG
  if (mp3 && mp3->isRunning()) {
    static int unsigned long lastms = 0;
    if (millis() - lastms > 1000) {
      lastms = millis();
      Serial.print(F("Free: "));
      Serial.print(ESP.getFreeHeap(), DEC);
      Serial.print(F("  ("));
      Serial.print( ESP.getFreeHeap() - ESP.getMaxFreeBlockSize(), DEC);
      Serial.print(F(" lost)"));
      Serial.print(F("  Fragmentation: "));
      Serial.print(ESP.getHeapFragmentation(), DEC);
      Serial.print(F("%"));
      if ( ESP.getHeapFragmentation() > 40) Serial.print(F("  ----------- "));
      Serial.println();
    }
  }
#endif
}

/* ############################### Audio ############################################ */

void playBootSound() {

  file_progmem = new AudioFileSourcePROGMEM(boot_sound, sizeof(boot_sound));
  wav = new AudioGeneratorWAV();
  wav->begin(file_progmem, out);
}

void stopPlaying() {
  analogWrite(LED_Pin, 255); //Turn LED back to full brightness

  if (mp3) {
#ifdef DEBUG_FLAG
    Serial.print(F("...#"));
    Serial.println(F("Interrupted!"));
    Serial.println();
#endif
    mp3->stop();
    delete mp3;
    mp3 = NULL;
  }
  if (wav) {
    wav->stop();
    delete wav;
    wav = NULL;
  }
  if (rtttl) {
    rtttl->stop();
    delete rtttl;
    rtttl = NULL;
  }
  if (buff) {
    buff->close();
    delete buff;
    buff = NULL;
  }
  if (file_http) {
    file_http->close();
    delete file_http;
    file_http = NULL;
  }
  if (file_progmem) {
    file_progmem->close();
    delete file_progmem;
    file_progmem = NULL;
  }
  if (file_icy) {
    file_icy->close();
    delete file_icy;
    file_icy = NULL;
  }
  broadcastStatus("status", "idle");
}

/* ################################## MQTT ############################################### */

void onMqttMessage(char* topic, byte* payload, unsigned int length)  {

  char newMsg[MQTT_MSG_SIZE];

  if (length > 0) {
    memset(newMsg, '\0' , sizeof(newMsg));
    memcpy(newMsg, payload, length);
#ifdef DEBUG_FLAG
    Serial.println();
    Serial.print(F("Requested ["));
    Serial.print(topic);
    Serial.print(F("] "));
    Serial.println(newMsg);
#endif
    // got a new URL to play ------------------------------------------------
    if ( !strcmp(topic, mqttFullTopic("play") ) ) {
      stopPlaying();
      file_http = new AudioFileSourceHTTPStream();
      if ( file_http->open(newMsg)) {
        broadcastStatus("status", "playing");
        analogWrite(LED_Pin, 100); // Dim LED while playing
        buff = new AudioFileSourceBuffer(file_http, preallocateBuffer, preallocateBufferSize);
        mp3 = new AudioGeneratorMP3();
        mp3->begin(buff, out);
      } else {
        stopPlaying();
        broadcastStatus("status", "error");
        broadcastStatus("status", "idle");
      }
    }

    // got a new URL to play ------------------------------------------------
    if ( !strcmp(topic, mqttFullTopic("stream"))) {
      stopPlaying();
      file_icy = new AudioFileSourceICYStream();
      if ( file_icy->open(newMsg)) {
        broadcastStatus("status", "playing");
        analogWrite(LED_Pin, 100); // Dim LED while playing
        buff = new AudioFileSourceBuffer(file_icy, preallocateBuffer, preallocateBufferSize);
        mp3 = new AudioGeneratorMP3();
        mp3->begin(buff, out);
      } else {
        stopPlaying();
        broadcastStatus("status", "error");
        broadcastStatus("status", "idle");
      }
    }

    // got a tone request --------------------------------------------------
    if ( !strcmp(topic, mqttFullTopic("tone") ) ) {
      stopPlaying();
      broadcastStatus("status", "playing");
      analogWrite(LED_Pin, 100); // Dim LED while playing
      file_progmem = new AudioFileSourcePROGMEM( newMsg, sizeof(newMsg) );
      rtttl = new AudioGeneratorRTTTL();
      rtttl->begin(file_progmem, out);
      broadcastStatus("status", "idle");
    }

    //got a TTS request ----------------------------------------------------
    if ( !strcmp(topic, mqttFullTopic("say"))) {
      stopPlaying();
      broadcastStatus("status", "playing");
      analogWrite(LED_Pin, 100); // Dim LED while playing
      ESP8266SAM *sam = new ESP8266SAM;
      sam->Say(out, newMsg);
      delete sam;
      stopPlaying();
      broadcastStatus("status", "idle");
    }

    // got a volume request, expecting double [0.0,1.0] ---------------------
    if ( !strcmp(topic, mqttFullTopic("volume"))) {
      volume_level = atof(newMsg);
      if ( volume_level < 0.0 ) volume_level = 0;
      if ( volume_level > 1.0 ) volume_level = 0.7;
      out->SetGain(volume_level);
    }

    // got a stop request  --------------------------------------------------
    if ( !strcmp(topic, mqttFullTopic("stop"))) {
      stopPlaying();
    }
  }
}

void broadcastStatus(char topic[], String msg) {

  if ( playing_status != msg) {
    char charBuf[msg.length() + 1];
    msg.toCharArray(charBuf, msg.length() + 1);
    mqttClient.publish(mqttFullTopic(topic), charBuf);
    playing_status = msg;
#ifdef DEBUG_FLAG
    Serial.println();
    Serial.print("status: ");
    Serial.println(msg);
#endif
  }
}


void mqttReconnect() {

  if (!mqttClient.connected()) {
    analogWrite(LED_Pin, 100); //turn LED low if not connected 
    
    if (mqttClient.connect(thingName.c_str(), mqttUserName, mqttUserPassword, mqttFullTopic(willTopic), willQoS, willRetain, willMessage)) {
      broadcastStatus("status", "connected");
      mqttClient.subscribe(mqttFullTopic("play"));
      mqttClient.subscribe(mqttFullTopic("stream"));
      mqttClient.subscribe(mqttFullTopic("tone"));
      mqttClient.subscribe(mqttFullTopic("say"));
      mqttClient.subscribe(mqttFullTopic("stop"));
      mqttClient.subscribe(mqttFullTopic("volume"));
#ifdef DEBUG_FLAG
      Serial.println();
      Serial.print(F("Connected to MQTT: "));
      Serial.println(F("mrdiynotifier"));
#endif
      broadcastStatus(willTopic, "Online");
      broadcastStatus("ThingName", thingName.c_str());
      broadcastStatus("IPAddress", WiFi.localIP().toString()); 
      broadcastStatus("status", "idle");
      analogWrite(LED_Pin, 255); // Turn LED HIGH once connected to MQTT
    }
  }
}
/* ############################ WifiManager ############################################# */

void wifiConnected() {

#ifdef DEBUG_FLAG
  Serial.println();
  Serial.println(F("=================================================================="));
  Serial.println(F("  MrDIY Notifier"));
  Serial.println(F("=================================================================="));
  Serial.println();
  Serial.print(F("Connected to Wifi ["));
  Serial.print(WiFi.localIP());
  Serial.println(F("]"));
#endif
  playBootSound();
  mqttClient.setServer(mqttServer, port);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(MQTT_MSG_SIZE);
  mqttReconnect();
}

boolean formValidator() {

  boolean valid = true;
  int l = server.arg(mqttServerParam.getId()).length();
  if (l == 0) {
    mqttServerParam.errorMessage = "Please provide the MQTT server address";
    valid = false;
  }
  return valid;
}

char* mqttFullTopic(char action[]) {
  strcpy (mqttTopic, mqttTopicPrefix);
  strcat (mqttTopic, "/");
  strcat (mqttTopic, action);
  return mqttTopic;
}
