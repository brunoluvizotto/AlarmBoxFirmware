/*
   1MB flash sizee

   sonoff header
   1 - vcc 3v3
   2 - rx
   3 - tx
   4 - gnd
   5 - gpio 14

   esp8266 connections
   gpio  0 - button
   gpio 12 - relay
   gpio 13 - green led - active low
   gpio 14 - pin 5 on header

*/

#include <vector>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <EEPROM.h>
//#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include "box.hpp"
#include "fw_updater.hpp"

#define SONOFF_BUTTON    0
#define SONOFF_RELAY    12
#define SONOFF_LED      13
#define BOX_BUTTON       1
#define BOX_LED          3
#define BOX_BUZZER      14

#define EST_ZONE (-5)

#define EEPROM_SALT 12663
typedef struct {
  int   salt = EEPROM_SALT;
} WMSettings;

WMSettings settings;

WiFiServer server(80);
WiFiServer serverTcp(2211);
WiFiClient client;
WiFiClient clientTcp;

WiFiUDP udp;
unsigned int localUdpPort = 2115;
unsigned int remoteUdpPort = 123;
IPAddress timeServerIP;
const char* ntpServerName = "pool.ntp.org";//"time.nist.gov";
#define NTP_PACKET_SIZE 48
byte packetBuffer[NTP_PACKET_SIZE];

unsigned long epoch = 0;
unsigned long lastEpoch = 0;
unsigned long lastMillis = 0;
int hours = -1;
int minutes = -1;
int seconds = -1;
int lastMinutes = -1;

Luvitronics::FWUpdater* fwUpdater = new FWUpdater();
//for LED status
#include <Ticker.h>
Ticker ticker;
Ticker tickerOTA;
Ticker tickerTCP;
Ticker tickerPB;
Ticker tickerPWM;
Ticker tickerGetTime;
Ticker tickerCheckUDP;
Ticker tickerCheckAlarm;

bool ota = 0;
bool pb = 0;
bool tcp = 0;
bool pwmState = 0;
bool getTime = 0;
bool checkUDP = 0;
bool checkAlarm = 0;

char box1hour = -1;
char box1min = -1;
std::vector<std::unique_ptr<Box>> boxes;

const int CMD_WAIT = 0;
const int CMD_BUTTON_CHANGE = 1;

int cmd = CMD_WAIT;
int relayState = HIGH;

//inverted button state
int buttonState = HIGH;

static long startPress = 0;

String inputString = "";         // a string to hold incoming data
boolean stringComplete = false;  // whether the string is complete

void tick()
{
  //toggle state
  int state = digitalRead(SONOFF_LED);  // get the current state of GPIO1 pin
  digitalWrite(SONOFF_LED, !state);     // set pin to the opposite state
}

void tickOTA()
{
  ota = 1;
}

void tickTCP()
{
  tcp = 1;
}

void tickPB()
{
  pb = 1;
}

void tickPWM()
{
  pwmState = !pwmState;
  analogWrite(BOX_BUZZER, 512 * pwmState);
}

void tickGetTime()
{
  getTime = 1;
}

void tickCheckUDP()
{
  checkUDP = 1;
}

void tickCheckAlarm()
{
  checkAlarm = 1;
}

//gets called when WiFiManager enters configuration mode
void configModeCallback (WiFiManager *myWiFiManager) {
  //Serial.println("Entered config mode");
  //Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  //Serial.println(myWiFiManager->getConfigPortalSSID());
  //entered config mode, make led toggle faster
  ticker.attach(0.2, tick);
}


void setState(int s) {
  digitalWrite(SONOFF_RELAY, s);
  digitalWrite(SONOFF_LED, (s + 1) % 2); // led is active low
}

void turnOn() {
  relayState = HIGH;
  setState(relayState);
}

void turnOff() {
  relayState = LOW;
  setState(relayState);
}

void toggleState() {
  cmd = CMD_BUTTON_CHANGE;
}

void resetBox1(){
  tickerPWM.detach();
  pwmState = 0;
  analogWrite(BOX_BUZZER, 512 * pwmState);
  digitalWrite(BOX_LED, 0);
}

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  //Serial.println("Should save config");
  shouldSaveConfig = true;
}


void toggle() {
  //Serial.println("toggle state");
  //Serial.println(relayState);
  relayState = relayState == HIGH ? LOW : HIGH;
  setState(relayState);
}

void restart() {
  ESP.reset();
  delay(1000);
}

void reset() {
  //reset settings to defaults
  /*
    WMSettings defaults;
    settings = defaults;
    EEPROM.begin(512);
    EEPROM.put(0, settings);
    EEPROM.end();
  */
  //reset wifi credentials
  WiFi.disconnect();
  delay(1000);
  ESP.reset();
  delay(1000);
}

// send an NTP request to the time server at the given address
unsigned long sendNTPpacket(IPAddress& address)
{
  //Serial.println("sending NTP packet...");
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  udp.beginPacket(address, 123); //NTP requests are to port 123
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
}

void setup()
{
    Serial.begin(115200);

    inputString.reserve(200);

    //set led pin as output
    pinMode(SONOFF_LED, OUTPUT);
    // start ticker with 0.5 because we start in AP mode and try to connect
    ticker.attach(0.6, tick);


    const char *hostname = "ESP8266";

    WiFiManager wifiManager;
    //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wifiManager.setAPCallback(configModeCallback);

    //timeout - this will quit WiFiManager if it's not configured in 3 minutes, causing a restart
    wifiManager.setConfigPortalTimeout(180);

    //custom params
    EEPROM.begin(512);
    EEPROM.get(0, settings);
    EEPROM.end();

    if (settings.salt != EEPROM_SALT) {
        Serial.println("Invalid settings in EEPROM, trying with defaults");
        WMSettings defaults;
        settings = defaults;
    }

    //set config save notify callback
    wifiManager.setSaveConfigCallback(saveConfigCallback);

    if (!wifiManager.autoConnect(hostname)) {
        Serial.println("failed to connect and hit timeout");
        //reset and try again, or maybe put it to deep sleep
        ESP.reset();
        delay(1000);
    }

    //save the custom parameters to FS
    if (shouldSaveConfig) {
        Serial.println("Saving config");

        EEPROM.begin(512);
        EEPROM.put(0, settings);
        EEPROM.end();
    }

    //if you get here you have connected to the WiFi
    Serial.println("connected...yeey :)");
    ticker.detach();

    // Start the server
    server.begin();
    serverTcp.begin();
    Serial.println("Server started");

    // Print the IP address
    Serial.print("Use this URL to connect: ");
    Serial.print("http://");
    Serial.print(WiFi.localIP());
    Serial.println("/");

    //setup button
    pinMode(SONOFF_BUTTON, INPUT);
    attachInterrupt(SONOFF_BUTTON, toggleState, CHANGE);

    //setup relay
    pinMode(SONOFF_RELAY, OUTPUT);

    /*EEPROM.begin(1024);
    EEPROM.put(513, 1);
    EEPROM.put(514, 8);
    EEPROM.put(515, 45);
    EEPROM.put(516, 255);
    EEPROM.put(517, 255);
    EEPROM.put(534, 1);
    EEPROM.put(535, 7);
    EEPROM.put(536, 30);
    //for (int i = 513; i < 1024; ++i)
    //    EEPROM.put(i, 255);
    EEPROM.end();*/

    /*EEPROM.begin(1024);
    EEPROM.get(513, box1hour);
    EEPROM.get(514, box1min);
    EEPROM.end();*/

    //setup box IOs
    Box* bxs[] = {
        new Box(1, (uint8_t)BOX_LED, (uint8_t)BOX_BUTTON)
    };
    for (auto box : bxs) boxes.emplace_back(box);

    pinMode(BOX_BUZZER, OUTPUT);
    analogWriteFreq(1000);
    analogWrite(BOX_BUZZER, 0);
    attachInterrupt(BOX_BUTTON, resetBox1, FALLING);

    udp.begin(localUdpPort);
    WiFi.hostByName(ntpServerName, timeServerIP); 
    sendNTPpacket(timeServerIP);

    turnOff();
    //Serial.println("done setup");

    tickerOTA.attach(0.1, tickOTA);
    tickerTCP.attach(0.05, tickTCP);
    tickerPB.attach(0.1, tickPB);
    //tickerPWM.attach(0.5, tickPWM);
    tickerGetTime.attach(300, tickGetTime); // every 5 minutes get ntp time
    tickerCheckUDP.attach(1, tickCheckUDP);
    tickerCheckAlarm.attach(10, tickCheckAlarm);
  
}

void loop()
{
  delay(20);
  
  if(ota)
  {
    ota = 0;
    fwUpdater->process();
  }

  if(tcp)
  {
    tcp = 0;
    client = server.available();
    
    if(!clientTcp.connected())
      clientTcp = serverTcp.available();

    delay(10);
    
    if(client.available())
    {
      // Read the first line of the request
      String request = client.readStringUntil('\r');
      //Serial.println(request);
      client.flush();

      int value = LOW;
      bool valid = false;
      if (request.indexOf("/DO0=0") != -1)  {
        turnOff();
        value = LOW;
        valid = true;
      }   
      else if (request.indexOf("/DO0=1") != -1)  {
        turnOn();
        value = HIGH;
        valid = true;
      }

      // Return the response
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: text/html");
      client.println(""); //  do not forget this one
      client.println("<!DOCTYPE HTML>");
      client.println("<html>");

      client.print("DO0=");
 
      if(relayState == HIGH) {
        client.print("1");
      } else {
        client.print("0");
      }
      
      for(auto& box : boxes)
      {
        //task->process();
        client.println("<br><br>");
        client.print("Box");
        client.print(box->getBoxNumber());
        client.println(":");
        
        std::vector<std::pair<uint8_t,uint8_t>> currentWETimes = box->getWEAlarmTimes();
        uint8_t i = 0;
        //for(auto i = 0; i < box.getLenWETimes(); ++i)
        for(auto& time : currentWETimes)
        {
            ++i;
            //Serial.print (buffer); std::get<0>(time)
            client.print("<p>");
            client.print("Weekend Time ");
            client.print(i);
            client.print(": ");
            client.print((uint8_t)std::get<0>(time));
            //client.print((int)box1hour);
            client.print(":");
            client.print((uint8_t)std::get<1>(time));
            //client.print((int)box1min);
            client.println("</p>");
        }
        
        std::vector<std::pair<uint8_t,uint8_t>> currentWDTimes = box->getWDAlarmTimes();
        i = 0;
        for(auto& time : currentWDTimes)
        {
            ++i;
            //Serial.print (buffer); std::get<0>(time)
            client.print("<p>");
            client.print("Weekday Time ");
            client.print(i);
            client.print(": ");
            client.print((uint8_t)std::get<0>(time));
            //client.print((int)box1hour);
            client.print(":");
            client.print((uint8_t)std::get<1>(time));
            //client.print((int)box1min);
            client.println("</p>");
        }
        client.print("<p>Alarm Box1 = ");
        client.print(digitalRead(BOX_LED));
        client.println("</p>");
      }
      
      client.println("<br><br>");
      client.println("<a href=\"/DO0=1\"\"><button>Turn On </button></a>");
      client.println("<a href=\"/DO0=0\"\"><button>Turn Off </button></a><br />");  
      client.println("</html>");
    }
    
    if(clientTcp.available())
    {
      String request = clientTcp.readStringUntil('\n');
      //Serial.println(request);
      clientTcp.flush();
        
      if (request.indexOf("DO0=0") != -1)  {
        turnOff();
        clientTcp.println("OK");
      }   
      else if (request.indexOf("DO0=1") != -1)  {
        turnOn();
        clientTcp.println("OK");
      }
      else if (request.indexOf("B1T=") != -1)  {
        int indexAux = request.indexOf("B1T=");
	int indexAux2 = request.indexOf(":");
	int newHour = request.substring(indexAux + 4, indexAux2).toInt();
        indexAux = request.indexOf("\n");
	int newMinute = request.substring(indexAux2 + 1, indexAux).toInt();
	
	if(newHour >= 0 && newHour < 24 && newMinute >= 0 && newMinute < 60)
	{
	  box1hour = newHour;
	  box1min = newMinute;
	  EEPROM.begin(1024);
	  EEPROM.put(513, newHour);
	  EEPROM.put(514, newMinute);
	  EEPROM.end();
	  clientTcp.println("OK");
	}
	else
	  clientTcp.println("Fail");
      }
      else if (request.indexOf("A1=OFF") != -1)  
      {
	resetBox1();
	clientTcp.println("OK");
      }

      
      else if (request.indexOf("DO0?") != -1)  {
        clientTcp.print("DO0=");
        if(relayState == HIGH) {
          clientTcp.println("1");
        } else {
          clientTcp.println("0");
        }
      }   
    }
  }

  if(pb)
  {
    pb = 0;
    switch (cmd) {
      case CMD_WAIT:
        break;
      case CMD_BUTTON_CHANGE:
        int currentState = digitalRead(SONOFF_BUTTON);
        if (currentState != buttonState) {
          if (buttonState == LOW && currentState == HIGH) {
            long duration = millis() - startPress;
            if (duration < 1000) {
              //Serial.println("short press - toggle relay");
              toggle();
            } else if (duration < 5000) {
              //Serial.println("medium press - reset");
              restart();
            } else if (duration < 60000) {
              //Serial.println("long press - reset settings");
              reset();
            }
          } else if (buttonState == HIGH && currentState == LOW) {
            startPress = millis();
          }
          buttonState = currentState;
        }
        break;
    }
  }
  
  if(getTime)
  {
    getTime = 0;
    
    WiFi.hostByName(ntpServerName, timeServerIP); 
    sendNTPpacket(timeServerIP);
    
  }
  
  if(checkUDP)
  {
    checkUDP = 0;
    
    int cb = udp.parsePacket();
    if (cb)
    {
      // We've received a packet, read the data from it
      udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

      //the timestamp starts at byte 40 of the received packet and is four bytes,
      // or two words, long. First, esxtract the two words:

      unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
      unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
      // combine the four bytes (two words) into a long integer
      // this is NTP time (seconds since Jan 1 1900):
      unsigned long secsSince1900 = highWord << 16 | lowWord;

      const unsigned long seventyYears = 2208988800UL;
      // subtract seventy years:
      lastEpoch = epoch;
      epoch = secsSince1900 - seventyYears;
      
      //Serial.print("The difference in the clocks is: ");
      //Serial.print(epoch - lastEpoch);
      //Serial.println(" seconds");
      
      // print the hour, minute and second:
      //Serial.print("The EST time is ");       // UTC is the time at Greenwich Meridian (GMT)
      //Serial.print(((epoch + 3600*EST_ZONE) % 86400L) / 3600); // print the hour (86400 equals secs per day)
      //Serial.print(':');
      if ( ((epoch % 3600) / 60) < 10 ) {
	// In the first 10 minutes of each hour, we'll want a leading '0'
	//Serial.print('0');
      }
      //Serial.print((epoch  % 3600) / 60); // print the minute (3600 equals secs per minute)
      //Serial.print(':');
      if ( (epoch % 60) < 10 ) {
	// In the first 10 seconds of each minute, we'll want a leading '0'
	//Serial.print('0');
      }
      //Serial.println(epoch % 60); // print the second
      
      hours = ((epoch + 3600*EST_ZONE) % 86400L) / 3600;
      minutes = (epoch  % 3600) / 60;
      seconds = epoch % 60;
      lastMillis = millis();
    }
  }
  
  if(checkAlarm)
  {
    checkAlarm = 0;
      
    unsigned long secsDiff;
    secsDiff = (millis() - lastMillis)/1000;
    lastMillis = millis();
    if(seconds + secsDiff >= 60)
    {
      seconds = (seconds + secsDiff) % 60;
      minutes++;
    }
    else
      seconds = seconds + secsDiff;
    
    if(minutes >= 60)
    {
      minutes = minutes % 60;
      hours++;
    }
    
    if(hours >= 24)
    {
      hours = 0;
    }
    
    //Serial.print("Current time: ");
    //Serial.print(hours);
    //Serial.print(":");
    //Serial.print(minutes);
    //Serial.print(":");
    //Serial.println(seconds);
    
    if(hours == box1hour && minutes == box1min && lastMinutes != minutes)
    {
      lastMinutes = minutes;
      tickerPWM.attach(0.5, tickPWM);
      digitalWrite(BOX_LED, 1);
      //Serial.println("ALARM!");
    }
  }

}




