// Program to connect to soyosource grid tie inverter RS485 limiter connection and provide it with a demand signal based on
// grid import readings from emonpi published on MQTT. This replaces the current sensing limiter device provided with the inverter and duplicates its RS485 output signal.

// Features:
// - Will send a demand signal to Inverter that attempts to keep electricity imported from Grid at 0 + importbuffer
// - Integration with emonpi via the MQTT communication protocol. Code could be modified to support other sources of grid import power measurement, either via MQTT or direct connection of current sensor to microcontroller.
// - Performs connectivity check with MQTT broker and handles lost connectivity with MQTT, will attempt to reconnect and will default the Inverter to zero output.
// - Performs connectivity check of WiFi and will initiate reconnect if disconnected.
// - Uses ESP32 watchdog timer to reset microcontroller if no MQTT packet recieved for 20 seconds. (this provides some tollerance of bad power supply issues to ESP32)
// - Supports output of status messages via an OLED display screen

// Excluded functionality:
// - Does not implement any "safety" features, relies on inverter to implement low input voltage shutdown and have an appropriate power limit set (this is the normal operating mode for the Inverter and how it is designed to behave).
// - Does not have input bounds checking, will respond to any integer value passed on the subscribed MQTT topic.
// - Does not implement any ramp function for large changes in demand signal, this is not required as the inverter implements its own smoothing algorithm (this is the normal operating mode for the Inverter and how it is designed to behave).
// - Does not have a setting for multiple inverters on same RS485 bus, this can be implemented by modifying code to divide the demand signal by the number of inverters. Without this modification I think this code would follow grid demand but with more oscillation. I do not have multiple inverters so am unable to develop and test this functionality.
// - Older firmware versions of this Inverter output a periodic status message, this program ignores that message. The inverter I have does not output the message so I am unable to test this functionality.

// Future planned functionality:
// - Publishing of status messages to MQTT (including heartbeat message for this program)
// - Temperature monitoring via Dallas 1 wire sensor
// - Inverter Current and voltage monitoring using analog inputs of microcontroller.
// - Logic to interpret these monitoring inputs and publish a request message on MQTT (which will be acted upon by other system components also subscribed to that topic):
//    + Request battery charger switch on if export to grid detected (for use with co-located grid tie solar installation)
//    + Request battery charger switch off/inhibit if battery temperature too low (below 2c).
//    + Request battery charger switch on if ambient temperature is low to heat the batteries by charging them and maintain their temperature above 5c
//    + Request Charger or inverter early switch off to keep battery SOC in a particular range to prolong battery cycle life (e.g. 20-80%) - not to be used as a safety feature, inverter and charger hardware must self terminate before 0% or 100% is reached.
//    + Computation of power into or out of batteries (based on current and voltage sensor).
//    + Estimation of battery state of charge (either by requesting inverter or charger shutdown to take an off-load voltage reading, or including estimate of voltage sag based on load to take an on-load reading).


// Hardware:
// Runs on Heltec WiFi Kit 32 ESP32 based microcontroller board (and clone boards of same spec) to also use built in OLED screen
// Should work (but not tested) on other ESP32 and ESP8266 microcontrollers or other Arduino IDE compatible boards with network connectivity
// Requires an RS485 to Serial TTL converter board. Any chipset should work as the serial baud rate is very low, make sure voltages match
// your microcontroller (e.g. 3.3v input to ESP32). Do not cross over any of the serial connections.

// Connections:
// ESP32 RX --> RS485 converter RX
// ESP32 TX --> RS485 converter TX
// Inverter RS485 A+  --> RS485 converter A+
// Inverter RS485 B-  --> RS485 converter B-

// sources used:
//  https://randomnerdtutorials.com/esp32-mqtt-publish-subscribe-arduino-ide/  -Tutorial for using MQTT on ESP microcontrolers, parts of tutorial code modified and used here
//  https://pubsubclient.knolleary.net/api  -Docs for the MQTT library used here
//  https://github.com/drcross/virtualmeter   - Python script to communicate with inverter, basic structure and parts of code converted to Arduino C and used here
//  https://secondlifestorage.com/index.php?threads/limiter-inverter-with-rs485-load-setting.7631/   - Details of RS485 packet structure and a lot of information on using the Inverter via RS485


#include <U8g2lib.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>


//--Configuration variables---
#define WDT_TIMEOUT 20 //if no MQTT message is received within this many seconds trigger a hardware reboot via watchdog timer. This needs to be long enough to enable the ESP to connect to wifi and MQTT and receive first message when first booting
const char* ssid = "SSID";
const char* password = "PASSWORD";
String hostname = "HouseBattery Controller"; //hostname (will appear in wifi router connected devices list etc)
const char* mqtt_server = "192.168.1.103";
//const char* mqtt_username = "emonpi";  //note: emonpi will allow connections with this default account but tends to drop connection regularly, for best performance create a new user account on mosquitto just for this device.
//const char* mqtt_password = "emonpimqtt2016";
const char* mqtt_username = "housebattery";
const char* mqtt_password = "mqttsecret";
int maxOutput = 500; //edit this to limit TOTAL power output in watts (not individual unit output)
int importbuffer = 120; // Current clamp sensors have poor accuracy at low load, a buffer ensures some current flow in the import direction to ensure no exporting. Adjust according to accuracy of meter.
int maxchargerpower = 580; //how much excess export to allow before engaging charger (e.g. if capturing solar export to charge a battery)

//--OLED variables
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/ 15, /* data=*/ 4, /* reset=*/ 16);
U8G2LOG u8g2log;
// define width 16 and height 4 - dependant on font size
#define U8LOG_WIDTH 16
#define U8LOG_HEIGHT 4
// allocate memory
uint8_t u8log_buffer[U8LOG_WIDTH*U8LOG_HEIGHT];

// -- Serial data --
byte byte0 = 36;
byte byte1 = 86;
byte byte2 = 0;
byte byte3 = 33;
byte byte4 = 0; //(2 byte watts as short integer xaxb)
byte byte5 = 0; //(2 byte watts as short integer xaxb)
byte byte6 = 128;
byte byte7 = 8; // checksum
byte serialpacket[8];
int importingnow = 0; //amount of electricity being imported from grid
int demand = 0; //current power inverter should deliver (default to zero)

// -- Connectivity setup
WiFiClient espClient;
PubSubClient client(espClient);
long lastMsg = 0;
char msg[50];
int value = 0;

void setup() {
    //-- Start watchdog timer, note first MQTT message must be received within WDT_TIMEOUT seconds from this point
    esp_task_wdt_init(WDT_TIMEOUT, true); //enable panic so ESP32 restarts
    esp_task_wdt_add(NULL); //add current thread to WDT watch
    
    // -- Setup OLED screen
    u8g2.begin();    
    u8g2.setFont(u8g2_font_profont17_mf);  // set the font for the terminal window
    u8g2log.begin(u8g2, U8LOG_WIDTH, U8LOG_HEIGHT, u8log_buffer); // connect to u8g2, assign buffer
    u8g2log.setLineHeightOffset(0); // set extra space between lines in pixel, this can be negative
    u8g2log.setRedrawMode(0);   // 0: Update screen with newline, 1: Update screen for every char  

    // -- Setup serial communication with inverter (uses Serial 1 which is also used by USB port and is the marked RX/TX connections on the board
    Serial.begin(4800);
    u8g2log.print("Serial started");
    u8g2log.print("\n");

    serialpacket[0]=byte0;
    serialpacket[1]=byte1;
    serialpacket[2]=byte2;
    serialpacket[3]=byte3;
    serialpacket[4]=byte4;
    serialpacket[5]=byte5;
    serialpacket[6]=byte6;
    serialpacket[7]=byte7;

    // -- Setup connectivity
    setup_wifi();
    client.setServer(mqtt_server, 1883);
    client.setCallback(callback);

}

void setup_wifi() {
  delay(10);
  // We start by connecting to a WiFi network
  u8g2log.print("Connecting to ");
  u8g2log.print(ssid);
  u8g2log.print("\n");
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.setHostname(hostname.c_str()); //define hostname
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    u8g2log.print("Wifi wait \\");
    u8g2log.print("\n");
    delay(100);
    u8g2log.print("Wifi wait |");
    u8g2log.print("\n");
    delay(100);
    u8g2log.print("Wifi wait /");
    u8g2log.print("\n");
    delay(100);
    u8g2log.print("Wifi wait -");
    u8g2log.print("\n");
  }
  u8g2log.println("WiFi connected");
  u8g2log.print("\n");
  u8g2log.print("IP address: ");
  u8g2log.print("\n");
  u8g2log.print(WiFi.localIP());
  u8g2log.print("\n");
}

void reconnect() {
  // Loop until we're reconnected to MQTT
  while (!client.connected()) {
    u8g2log.print("MQTT connect...");
    u8g2log.print("\n");
    //no MQTT connection so set inverter output to zero.
    serialpacket[4]=0;
    serialpacket[5]=0;
    serialpacket[6]=128;
    demand = 0;
    importingnow = 0;
    // Attempt to connect
    if (client.connect("BatteryInverter",mqtt_username, mqtt_password)) {
      u8g2log.println("connected");
      u8g2log.print("\n");
      // Subscribe
      client.subscribe("emon/emonpi/power1");
    } else {
      u8g2log.print("failed, rc=");
      u8g2log.print("\n");
      u8g2log.print(client.state());
      u8g2log.print("\n");
      delay(5000);
    }
  }
}

void callback(char* topic, byte* message, unsigned int length) {
  // -- mqtt callback function
  //  u8g2log.print("MQTT MSG: ");
  //  u8g2log.print(topic);
  //  u8g2log.print(". Message: ");
  String messageTemp;
  
  for (int i = 0; i < length; i++) {
    messageTemp += (char)message[i];
  }

   if (String(topic) == "emon/emonpi/power1") {
    importingnow = messageTemp.toInt();
    
    // -- Compute demand signal --
    importingnow = importingnow-importbuffer; //target grid demand minus buffer
    demand = demand + importingnow; //add grid import to current demand, expects that grid import will be negative if exporting
    if (demand >= maxOutput){
        demand = maxOutput;} //cap at maxOutput - if the inverter is off or not keeping up with demand then the demand value would get very large if not capped
    else if (demand <= 0){ // if demand is negative reset to zero (control of a charger can be added here)
        demand = 0;}
    importingnow = importingnow+importbuffer; //remove change to actual import value
    
    // -- Compute serial packet to inverter (just the 3 bytes that change) --
    byte4 = int(demand/256); // (2 byte watts as short integer xaxb)
    if (byte4 < 0 or byte4 > 256){
        byte4 = 0;}
    byte5 = int(demand)-(byte4 * 256); // (2 byte watts as short integer xaxb)
    if (byte5 < 0 or byte5 > 256) {
        byte5 = 0;}
    byte7 = (264 - byte4 - byte5); //checksum calculation
    if (byte7 > 256){
        byte7 = 8;}

    serialpacket[4]=byte4;
    serialpacket[5]=byte5;
    serialpacket[7]=byte7;
    Serial.write(serialpacket,8);

    // -- Reset Hardware Watchdog Timer
    esp_task_wdt_reset();

    // -- update OLED with inverter request --
    u8g2log.print(" GridImp: ");
    u8g2log.print(importingnow);
    u8g2log.print("\n");
    u8g2log.print(" Bat Exp: "); 
    u8g2log.print(demand);   
    u8g2log.print("\n");
    u8g2log.print("WIFI RSSI: ");     
    u8g2log.print(WiFi.RSSI());
    u8g2log.print("\n");
    u8g2log.print("\n");    
   }

  // -- Process other command messages (must be subscribed to the topic in MQTT setup function)
  // If a message is received on the topic esp32/output, you check if the message is either "on" or "off". 
  // Changes the output state according to the message
  if (String(topic) == "esp32/output") {
    if(messageTemp == "on"){

    }
    else if(messageTemp == "off"){

    }
  }
}

void loop() {
  // -- check to see if wifi still connected reconnect if not
  if (WiFi.status() != WL_CONNECTED) {
    u8g2log.print("Wifi ERROR");
    u8g2log.print("\n");
    WiFi.disconnect();
    WiFi.reconnect();
    
    while (WiFi.status() != WL_CONNECTED) {
      delay(200);
      u8g2log.print("Wifi reconnect...");
      u8g2log.print("\n");
    }
  }
  
  // -- check to see if MQTT still connected reconnect if not
  if (!client.connected()) {
    reconnect();
  }
  // -- check to see if MQTT message received (or queued to send)
  // -- Callback function will run at this point
  client.loop();

  // inverter requires a regular message over serial or it will default to zero output (lost link)
  // MQTT updates (and therefore callback) may have an interval longer than this lost link interval,
  // so regularly repeat the last computed packet to the inverter even though it hasn't changed
  // on startup this will output a request for zero power (serialpacket initialised in void setup()), serialpacket is then updated by the MQTT callback
  // If MQTT link lost after startup the reconnect function also sets the serialpacket to zero power to ensure zero power until next MQTT message received.
  // whilst MQTT is reconnecting program execution will stay in the reconnect function so no serial data will be transmitted and inverter will set itself to zero (lost link)
    Serial.write(serialpacket,8);
    delay(600); //wait half a second so as not to overload the inverter with updates - change this to use an if millis statement so it is not blocking execution for half a second
    u8g2log.print("\n"); //clear off the OLED display after half a second to reduce screen burn in
    u8g2log.print("\n");
    u8g2log.print("\n");
    u8g2log.print(".");
}
