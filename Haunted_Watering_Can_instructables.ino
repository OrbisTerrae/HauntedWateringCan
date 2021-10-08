/*
  Orbis Terrae (c) 2021 - Haunted Watering Can

  The code controls the ultrasonic mist maker, the pump and the pulsing led strip inside the watering can.

  These elements can be controlled or watched thru different interfaces, including different level of intensity.
    Physical Buttons on the brain control box (on/off)
    internal web server to control the intensity (percentage)

  Several other outputs have been integrated for logging or debug purpose, entirely optional:
    OLED screen, hidden in the brain box, for run time debugging.
    ESP8266 serial port, to debug on the host during the development.
    internal EEPROM to save settings upon restart.

  The main loop handle these interfaces every second.
  The frequency may be increased for a better reactivity, but will have an impact on the level of power used.

  Simply update the code with the #TOBEUPDATED comment to adapt it to your needs.
*/

// Import required libraries
#include <ESP8266WiFi.h>
#include <DHTesp.h>
#include <aREST.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

#define LOCATION "BedRoom" // #TOBEUPDATED 
#define FWREV "v0.1.3"
#define FWNAME "Haunted Watering Can"
#define IOT_ID "1"

#define FOG_PIN 14    // D5
#define LED_STRIP 12  // D6
#define PUMP 13       // D7 

#define EE_LED   1    // EEPROM MAP -> LED Status
#define EE_PUMP  2    // EEPROM MAP -> Pump status
#define EE_FOG   3    // EEPROM MAP -> Fog status
#define EE_PLED  4    // EEPROM MAP -> previous LED Status
#define EE_PPUMP 5    // EEPROM MAP -> previous Pump status
#define EE_PFOG  6    // EEPROM MAP -> previous Fog status

// define if there is an OLED display connected
// OLED SLK -> ESP8266 D1/GPIO5/SCL ; OLED SDA -> ESP8266 D2/GPIO4/SDA
#define OLEDCONNECTED 1 // #TOBEUPDATED comment if no OLED SCREENCONNECTED
Adafruit_SSD1306 display = Adafruit_SSD1306(128, 64, &Wire);

#define INTERVAL_SERVER   1000      //interval to update the web server in ms -> 1s
#define INTERVAL_SENSOR   60000 //interval to read the sensor (min 2s from the specs) and send to IoTGuru - 1 minute
#define INTERVAL_RESET    60*60*24  //reset once a day...
#define LEDSTATUS 1                 //ON
//#define MAX_OUTPUT 255  // on ESP8266 V3 CH340, max output is 255     #TOBEUPDATED
#define MAX_OUTPUT 1023  // on ESP8266 V2 CP2102, max output is 1023    #TOBEUPDATED 


// WiFi parameters
const char* ssid = "xxxxxxxxx";     // #TOBEUPDATED
const char* password = "xxxxxxxxx"; // #TOBEUPDATED
// Create aREST instance
aREST rest = aREST();
// The port to listen for incoming TCP connections
#define LISTEN_PORT           80 // this is where the interval webserver display variables
// Create an instance of the server
WiFiServer server(LISTEN_PORT);

// Variables to be exposed to the API on the internal web server 127.0.0.1:80
int fogStatus       = 100; // where the current value of the fog intensity % is stored
int pumpStatus      = 100; // current value of the pump intensity %
int ledStatus       = LEDSTATUS; // current value of the LED strip %
int OLEDStatus      = OLEDCONNECTED; // do we have an OLED display conencted?
int previousLedIntensity  = 0; // value of the previous ledStatus% before it was switched off
int previousFogIntensity  = 0; // value of the previous fogStatus% before it was switched off
int previousPumpIntensity = 0; // value of the previous pumpStatus% before it was switched off
int EEPROMchanged         = 0; // eeprom commit is done once per loop at maximum to prevent crash
long runningTime          = 0;


/*  -------------------------------------------------------------------------------*/
/*  -- fogControl -----------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
void fogControl(int percent) {
  fogStatus = percent;
  if (fogStatus > 0) { // save the previous state in EEPROM
    previousFogIntensity = fogStatus;
  }
  EEPROM.write(EE_FOG, fogStatus); // only can store a value between 0 to MAX_OUTPUT, not the full scale - so we store the % only
  EEPROM.write(EE_PFOG, previousFogIntensity);
  Serial.printf("Set fog intensity: %d%%\n", fogStatus);
  EEPROMchanged = 1; // commit will be done in main loop - prevent crash under interrupts
  analogWrite(FOG_PIN, fogStatus * MAX_OUTPUT / 100); // convert the percentage of intensity into a full scale value - up to 1023
}

/*  -------------------------------------------------------------------------------*/
/*  -- fogOn ----------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int fogOn (String command) {
  // Get pump intensity from command - this is a percentage between 1 and 100%
  //  http://192.168.1.53/fogOn?param=75
  int percent = command.toInt();
  if (percent == 0 || percent > 100) { // 0 means no argument was provided to the web rest server
    percent = 100; // so we default to 100%
  }
  fogControl(percent);
  return percent;
}

/*  -------------------------------------------------------------------------------*/
/*  -- fogOff ---------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int fogOff (String command) {
  fogStatus = 0;
  fogControl(fogStatus);
  return fogStatus;
}

/*  -------------------------------------------------------------------------------*/
/*  -- pumpControl  ---------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
void pumpControl(int percent) {
  pumpStatus = percent;

  if (pumpStatus > 0) {
    previousPumpIntensity = pumpStatus;
  }
  EEPROM.write(EE_PUMP, pumpStatus);
  EEPROM.write(EE_PPUMP, previousPumpIntensity);
  Serial.printf("Set pump intensity: %d%%\n", pumpStatus);
  EEPROMchanged = 1; // commit will be done in main loop - prevent crash under interrupts
  analogWrite(PUMP, pumpStatus * MAX_OUTPUT / 100);
}

/*  -------------------------------------------------------------------------------*/
/*  -- pumpOn  --------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int pumpOn (String command) {
  // Get pump intensity from command
  //  http://192.168.1.53/pumpOn?param=75

  int percent = command.toInt();
  if (percent == 0 || percent > 100) {
    percent = 100;
  }
  pumpControl(percent);
  return percent;
}

/*  -------------------------------------------------------------------------------*/
/*  -- pumpOff  -------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int pumpOff (String command) {
  pumpStatus = 0;
  pumpControl(pumpStatus);
  return pumpStatus;
}


/*  -------------------------------------------------------------------------------*/
/*  -- ledOn ----------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int ledOn(String command) {

  // Get led strip intensity from command
  //  http://192.168.1.53/displayOn?param=75
  int percent = command.toInt();
  if (percent == 0 || percent > 100) {
    percent = 100;
  }
  int intense = percent * MAX_OUTPUT / 100; // parameter is a percentage, as we can only store up to 255 in the EEPROM

  ledStatus = percent;
  if (ledStatus > 0) {
    previousLedIntensity = ledStatus;
  }
  EEPROM.write(EE_LED, percent);
  EEPROM.write(EE_PLED, previousLedIntensity);
  EEPROMchanged = 1;
  Serial.printf("Set LED intensity: %d%%\n", ledStatus);
  for (int i = 0 ; i < intense ; i++) {
    analogWrite(LED_STRIP, i);
    delay(1);
  }
  return ledStatus;
}


/*  -------------------------------------------------------------------------------*/
/*  -- ledOff ---------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int ledOff(String command) {
  ledStatus = 0;

  EEPROM.write(EE_LED, ledStatus);
  EEPROM.write(EE_PLED, previousLedIntensity);
  EEPROMchanged = 1;
  for (int i = previousLedIntensity ; i >= 0 ; i--) {
    analogWrite(LED_STRIP, i);
    delay(1);
  }
  return ledStatus;
}

/*  -------------------------------------------------------------------------------*/
/*  -- homebridgeStatus -----------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int homebridgeStatus(String command) {
  // direct control from HomeBridge -> return status
  if (ledStatus > 0 || pumpStatus > 0 || fogStatus > 0) {
    // at least one activity on one of the peripheral -> the crystal skull is running
    return 1;
  }
  else {
    // no activity, everything is off
    return 0;
  }
}

/*  -------------------------------------------------------------------------------*/
/*  -- homebridgeOn ---------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int homebridgeOn(String command) {
  // direct control from HomeBridge -> turn On all peripherals based on previously saved intensity
  if (ledStatus >= 1) {
    ledOn(String(ledStatus));
    //    Serial.println("engaging leds");
  }
  else {
    ledOn(String(previousLedIntensity));
  }
  if (pumpStatus >= 1) {
    pumpOn(String(pumpStatus));
    //    Serial.println("engaging pump");
  }
  else {
    pumpOn(String(previousPumpIntensity));
  }
  if (fogStatus >= 1) {
    fogOn(String(fogStatus));
    //    Serial.println("engaging fog");
  }
  else {
    fogOn(String(previousFogIntensity));
  }
  return 1;
}

/*  -------------------------------------------------------------------------------*/
/*  -- homebridgeOff --------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int homebridgeOff(String command) {
  // direct control from HomeBridge -> turn Off all peripherals
  ledOff("");
  pumpOff("");
  fogOff("");
  return 1;
}

/*  -------------------------------------------------------------------------------*/
/*  -- display_header -------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int display_header() {
  if (OLEDStatus == 1) {
    //    display.dim(true);
    display.clearDisplay();
    display.display();

    // text display tests
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("Orbis Terrae - IoT");
    display.println(IOT_ID);
    display.println(FWNAME);
    display.print("IP: ");
    display.println(WiFi.localIP());
    display.print("running: ");
    display.print(runningTime);
    display.print("s\n");
    display.println();

    display.setTextSize(2);
    display.setCursor(0, 34);
    display.print("Fog:  ");
    display.print(fogStatus);
    display.print("\n");
    display.print("Pump: ");
    display.print(pumpStatus);
    display.print("\n");

    display.setCursor(0, 0);
    display.display(); // actually display all of the above
  }
  return OLEDStatus;
}

/*  -------------------------------------------------------------------------------*/
/*  -- heartBeat ------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int heartBeat() {

  if (ledStatus > 0) { // only activate if LED strip is on
    int i;
    for (i = ledStatus ; i >= ledStatus / 2 ; i--) {
      analogWrite(LED_STRIP, i * MAX_OUTPUT / 100);
      delay(25);
    }
    for (i = ledStatus / 2 ; i <= ledStatus ; i++) {
      analogWrite(LED_STRIP, i * MAX_OUTPUT / 100);
      delay(25);
    }
  }
  return 1;
}

/*  -------------------------------------------------------------------------------*/
/*  -- dumpEEPROM -----------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
int dumpEEPROM(String command) {
  int lled, llog, lpump, lfog, lpled, lppump, lpfog, lhb;
  lled     = EEPROM.read(EE_LED); // read the last led status from EEPROM
  Serial.printf("current display status stored in EEPROM: %d\n", lled);
  lpump      = EEPROM.read(EE_PUMP); // read the last cloud status from EEPROM
  Serial.printf("current pump intensity stored in EEPROM: %d\n", lpump);
  lfog      = EEPROM.read(EE_FOG); // read the last cloud status from EEPROM
  Serial.printf("current fog intensity stored in EEPROM: %d\n", lfog);
  lpled     = EEPROM.read(EE_PLED); // read the last led status from EEPROM
  Serial.printf("previous display status stored in EEPROM: %d\n", lpled);
  lppump      = EEPROM.read(EE_PPUMP); // read the last cloud status from EEPROM
  Serial.printf("previous pump intensity stored in EEPROM: %d\n", lppump);
  lpfog      = EEPROM.read(EE_PFOG); // read the last cloud status from EEPROM
  Serial.printf("previous fog intensity stored in EEPROM: %d\n", lpfog);
  return 1;
}

/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
void setup(void)
{
  // Start Serial
  Serial.begin(115200);
  EEPROM.begin(512);

  Serial.println();
  Serial.printf("\nOrbis Terrae (c) 2021 %s IoT %s\n", FWNAME, IOT_ID );

  String thisBoard = ARDUINO_BOARD;
  Serial.println(thisBoard);

  // read the last current and previous status / intensity % from EEPROM
  ledStatus                   = EEPROM.read(EE_LED);
  fogStatus                   = EEPROM.read(EE_FOG);
  pumpStatus                  = EEPROM.read(EE_PUMP);
  previousLedIntensity        = EEPROM.read(EE_PLED);
  previousFogIntensity        = EEPROM.read(EE_PFOG);
  previousPumpIntensity       = EEPROM.read(EE_PPUMP);

  // Init variables and expose them to REST API on the internal web server
  rest.variable("ledStatus", &ledStatus);
  rest.variable("fogStatus", &fogStatus);
  rest.variable("pumpStatus", &pumpStatus);
  rest.variable("OLEDStatus", &OLEDStatus);
  rest.variable("runTime", &runningTime);

  rest.function("ledOn", ledOn); // turn led strip on with specified intensity (in %)
  rest.function("ledOff", ledOff); // turn led strip off
  rest.function("pumpOn", pumpOn); // turn pump on with potential parameter
  rest.function("pumpOff", pumpOff); // turn pump off
  rest.function("fogOn", fogOn); // turn the ultrasonic humidifier with specified intensity (in %)
  rest.function("fogOff", fogOff); // turn fog off
  rest.function("dumpEEPROM", dumpEEPROM); // display the content of EEPROM to the serial port (debug)
  rest.function("homebridgeStatus", homebridgeStatus); // answer to the homebridge status query
  rest.function("homebridgeOn", homebridgeOn); // turn on all peripherals (pump, fog, led)
  rest.function("homebridgeOff", homebridgeOff); // turn off all peripherals (pump, fog, led)

  // Give name & ID to the device (ID should be 6 characters long) for the internal webserver
  rest.set_id(IOT_ID);
  rest.set_name("Orbis Terrae IoT");

  if (OLEDStatus == 1) {
    bool noerror = false;
    noerror = display.begin(SSD1306_SWITCHCAPVCC, 0x3C); // Address 0x3C for 128x32
    if (noerror == true) {
      Serial.println("OLED Connected");
      display.display();
    }
    else {
      Serial.println("no OLED display connected");
      OLEDStatus = 0;
    }
  }

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  // Start the server
  server.begin();
  Serial.println("Server started");

  // Print the IP address
  Serial.println(WiFi.localIP());

  pinMode(FOG_PIN,    OUTPUT);
  pinMode(LED_STRIP,  OUTPUT);
  pinMode(PUMP,       OUTPUT);

  display_header();
  if (ledStatus >= 1) {
    ledOn(String(ledStatus));
    //    Serial.println("engaging leds");
  }
  if (fogStatus >= 1) {
    fogOn(String(fogStatus));
    //    Serial.println("engaging fog");
  }
  if (pumpStatus >= 1) {
    pumpOn(String(pumpStatus));
    //    Serial.println("engaging fog");
  }

  Serial.println("#########################");
}

/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
/*  -------------------------------------------------------------------------------*/
void loop() {

  // handle the storage of key values into EEPROM
  if (EEPROMchanged == 1) { // means there has been a change in the key values of the pump or fog, via interrupt or rest server
    if (OLEDStatus == 1) { // so we update the OLED display on top of commiting the EEPROM
      display_header();
    }
    EEPROMchanged = 0; // clear the flag
    if (EEPROM.commit()) {
      Serial.println("Saving to EEPROM...");
    } else {
      Serial.println("ERROR! EEPROM commit failed");
    }
  } else {
    if (OLEDStatus == 1) {
      display_header();
    }
  }

  // sleep a bit...
  delay(INTERVAL_SERVER);
  runningTime += INTERVAL_SERVER / 1000;

  // add some robustness
  if (runningTime > (INTERVAL_RESET)) {   // reseting after 1 day of runntime
    Serial.printf("Orbis Terrae IoT - running time: %ds - Rebooting\n", runningTime);
    runningTime = 0;
    ESP.restart();
  }

  // Handle REST calls
  WiFiClient client = server.available();
  if (client) {
    rest.handle(client);
  }


  heartBeat();
}
