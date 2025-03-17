#include <WiFi.h>
#include <PubSubClient.h>

//Loctek Motion controller pins
#define displayPin20 4           // RJ45 connector of Loctek Motion controller: Pin20 for power on the desk controller
#define rxPin 16                 // RJ45 connector of Loctek Motion controller: RX-Pin
#define txPin 17                 // RJ45 connector of Loctek Motion controller: TX-Pin

// Other pins
#define togglePresetButtonPin 2  // single toggle button that switches between preset1/preset2
#define ledPinRed 21             // button led ring: red color
#define ledPinGreen 22           // button led ring:  green color
#define ledPinBlue 23            // button led ring:  color
#define ledStripPin 19           // PWM-Signal for LED strip that illuminates the garbage bins (goes into a MOSFET that drives the LED strip)

// WiFi- and MQTT Configuration
const char* wifi_ssid = "******************************";
const char* wifi_password = "******************************";
const char* hostname = "Muelltonne";
const char* mqtt_server = "192.168.x.x";
const int   mqtt_port = 1883;
const char* mqtt_user = "*********************";
const char* mqtt_password = "*********************";
const char* mqtt_topic_command = "muelltonne/command";
const char* mqtt_topic_position = "muelltonne/position";
const char* mqtt_topic_lwt = "muelltonne/lwt"; // last will and testament
const char* mqtt_topic_log = "muelltonne/log";


enum LedColor {
  off,
  red,
  green,
  blue,
  purple,
  yellow,
  cyan,
  white
};

enum DimmingAction 
{
  Idle, 
  FadeOn, 
  FadeOff,
  On,
  Off
};

enum DeskAction
{
  action_none,
  action_preset1,
  action_preset2,
  action_preset3,
  action_preset4,
  action_moveup,
  action_movedown
};


HardwareSerial deskSerial(1);               // use HardwareSerial-Port 1
unsigned long lastCommunicationMillis = 0;  // Store the time of the last communication to enable sleep after idle time
unsigned long lastMovementMillis = 0;       // Store the time of the last position changed update
bool deskConnected = false;
bool togglePresetButtonPressed = false;
bool preset1ButtonPressed = false;
bool preset2ButtonPressed = false;
int packet_pos = 0;                   //position in packet
int packet_len = 0;                   //packet lenght
int packet_type = 0;                  //type of packet
byte message_height[3];               //message containing height code
float height = 0.0;                   // for Flexispot EC5 the valid range is between 62.0 and 127.0 cm
float previousHeight = 0.0; 
int activePreset = 0;                 // current active preset 1-4, used for toggling between them
DeskAction nextDeskAction = action_none;
DeskAction lastDeskAction = action_none;
unsigned long nextDeskActionExecutionTime = 0; // time when nextDeskAction is executed
unsigned long previousLedRingMillis = 0;  // used for flashing the led

// LED Strip definitions
const int freq = 250;      // PWM frequency in Hz
const int resolution = 10;   // PWM resolution in bits (10 bits = 0 to 1024)
const int maxBrightness = 1023; // Maximum brightness (100% at 10-bit resolution)
unsigned long previousLedStripMillis = 0;  // Timestamp for the last step
int stepInterval = 6000 / maxBrightness;  // Duration for one step in milliseconds
float brightness = 0;  // Current brightness (as float for the exponential calculation of the LED brightness)
DimmingAction ledStripAction = Idle;
bool ledStripEnabled = true;

// Reconnect vars
unsigned long wifiReconnectPreviousMillis = 0;
unsigned long mqttReconnectPreviousMillis = 0;

// MQTT-Client
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// supported commands
const byte wakeup[] = { 0x9b, 0x06, 0x02, 0x00, 0x00, 0x6c, 0xa1, 0x9d };
const byte command_up[] = { 0x9b, 0x06, 0x02, 0x01, 0x00, 0xfc, 0xa0, 0x9d };
const byte command_down[] = { 0x9b, 0x06, 0x02, 0x02, 0x00, 0x0c, 0xa0, 0x9d };
const byte command_m[] = { 0x9b, 0x06, 0x02, 0x20, 0x00, 0xac, 0xb8, 0x9d };
const byte command_preset_1[] = { 0x9b, 0x06, 0x02, 0x04, 0x00, 0xac, 0xa3, 0x9d };
const byte command_preset_2[] = { 0x9b, 0x06, 0x02, 0x08, 0x00, 0xac, 0xa6, 0x9d };
const byte command_preset_3[] = { 0x9b, 0x06, 0x02, 0x10, 0x00, 0xac, 0xac, 0x9d };
const byte command_preset_4[] = { 0x9b, 0x06, 0x02, 0x00, 0x01, 0xac, 0x60, 0x9d };

void logMessage(const String& message) {
  Serial.println(message);
  mqttClient.publish(mqtt_topic_log, message.c_str(), true);
}


bool init_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(wifi_ssid);

  WiFi.setHostname(hostname);
  WiFi.begin(wifi_ssid, wifi_password);

  int counter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
    counter++;
    if (counter > 30)
      return false;
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP-Address: ");
  Serial.println(WiFi.localIP());
  return true;
}

void callback(char* topic, byte* payload, unsigned int length) {

  String command;
  for (int i = 0; i < length; i++) {
    command += (char)payload[i];
  }
  logMessage(String("Message received [") + String(topic) + "]: " + command);

  if (command == "preset1") {
    preset1();
  } else if (command == "preset2") {
    preset2();
  } else if (command == "preset3") {
    preset3();
  } else if (command == "preset4") {
    preset4();
  } else if (command == "wake") {
    wake();
  } else if (command == "moveup") {
    moveUp();
  } else if (command == "movedown") {
    moveDown();
  } else if (command == "enableLedStrip") {
    ledStripEnabled = true;
  } else if (command == "disableLedStrip") {
    ledStripEnabled = false;
  }
}

void connectMqtt() {
  if (!mqttClient.connected()) {
    Serial.print("(re)connect to MQTT-Broker...");
    bool connectResult;
    connectResult = mqttClient.connect(hostname, mqtt_user, mqtt_password, mqtt_topic_lwt, 1, false, "Offline");

    if (connectResult) {
      Serial.println("connected");
      mqttClient.subscribe(mqtt_topic_command, 1); // QoS = 1 (at least once)
      mqttClient.publish(mqtt_topic_lwt, "Online", true);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retry in 30 seconds");
    }
  }
}

void setup() {
  Serial.begin(115200);                              // Debug serial
  deskSerial.begin(9600, SERIAL_8N1, rxPin, txPin);  // Flexispot E5

  pinMode(displayPin20, OUTPUT);
  pinMode(togglePresetButtonPin, INPUT_PULLUP);
  pinMode(ledPinRed, OUTPUT);
  pinMode(ledPinGreen, OUTPUT);
  pinMode(ledPinBlue, OUTPUT);
  ledcAttach(ledStripPin, freq, resolution);
  
  digitalWrite(displayPin20, LOW);
  digitalWrite(ledPinBlue, HIGH);

  init_wifi();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(callback);
  connectMqtt();

  activate();
}

void loop() {
  // Reconnect handling
  unsigned long currentMillis = millis();
  // reconnect WiFi if down for 30s
  if ((WiFi.status() != WL_CONNECTED) && (currentMillis - wifiReconnectPreviousMillis >= 30000ul)) {
    Serial.println("Reconnecting to WiFi...");
    WiFi.disconnect();
    WiFi.reconnect();
    wifiReconnectPreviousMillis = currentMillis;
  }

  // reconnect mqtt if down
  if (!mqttClient.connected() && (currentMillis - mqttReconnectPreviousMillis >= 30000ul)) {
    connectMqtt();
    mqttReconnectPreviousMillis = currentMillis;
  }


  mqttClient.loop();

  handleScheduledAction();  
  handleButton();
  handleLedRing();
  handleLedStrip();
  

  while (deskSerial.available()) {
    unsigned long in = deskSerial.read();

    // Start of packet
    if (in == 0x9B)  //start of packet
    {
      packet_pos = 0;  //first position
      continue;
    } else if (in == 0x9D)  //end of packet
    {
      //Serial.println();
      if ((packet_type == 18) && (deskConnected == true))  //packettype 18 is height information but ignore all packets incoming after desk was disconnected (I received some weired 111cm messages 10s after last valid height)
      {
        DecodeHeight();
      }
    } else
      packet_pos++;

    if (packet_pos == 1)  //position 1 = lenght of packet
      packet_len = (int)in;
    else if (packet_pos == 2)  //position 2 = type of packet
      packet_type = (int)in;
    else if (packet_type == 18 && packet_pos >= 3 && packet_pos <= 5)  //if packet type is height (0x12) and position is 3-5
      message_height[packet_pos - 3] = in;

    // Print Hex for Debug
    //Serial.print(in, HEX);
    //Serial.print(" ");
  }

  currentMillis = millis();  // Get the current time
  if (currentMillis - lastCommunicationMillis > 8000) {
    if (deskConnected) {
      digitalWrite(displayPin20, LOW);  // end communication with control box so the controller can go to sleep now
      deskConnected = false;
      logMessage("No action last x millis -> Sleep");
    }
  }



}

void activate() {
  digitalWrite(displayPin20, HIGH);  // Pin 20 must always be high on the Flexispot EC5 as long as we are communicating with the control box
  if (deskConnected == false)
    delay(1000);  // after setting pin20 to high controller need some time to boot up
  deskConnected = true;
  lastCommunicationMillis = millis();
}

void wake() {
  activate();
  logMessage("Sende Wake-Up-Befehl um Position zu erhalten");
  deskSerial.flush();
  deskSerial.write(wakeup, sizeof(wakeup));
}

void preset1() {
  activate();
  logMessage("Sende Preset_1");
  if ((activePreset != 1) || !isDeskMoving()) {
    deskSerial.flush();
    deskSerial.write(command_preset_1, sizeof(command_preset_1));
    activePreset = 1;
  }
}

void preset2() {
  activate();
  logMessage("Sende Preset_2");
  if ((activePreset != 2) || !isDeskMoving()) {
    deskSerial.flush();
    deskSerial.write(command_preset_2, sizeof(command_preset_2));
    activePreset = 2;
  }
}

void preset3() {
  activate();
  logMessage("Sende Preset_3");
  deskSerial.flush();
  deskSerial.write(command_preset_3, sizeof(command_preset_3));
}

void preset4() {
  activate();
  logMessage("Sende Preset_4");
  deskSerial.flush();
  deskSerial.write(command_preset_4, sizeof(command_preset_4));
}

void moveUp() {
  activate();
  logMessage("Sende Up-Befehl");
  deskSerial.flush();
  deskSerial.write(command_up, sizeof(command_up));
}

void moveDown() {
  activate();
  logMessage("Sende Down-Befehl");
  deskSerial.flush();
  deskSerial.write(command_down, sizeof(command_down));
}


void DecodeHeight() {
  float h = DecodeNumber(message_height[0]) * 100.0 + DecodeNumber(message_height[1]) * 10.0 + DecodeNumber(message_height[2]);
  if (h > 200.0)  //if value is > 200 then it is with decimal point so it should be divided by 10
    h = h / 10;

  if (height != h) {
    activate();  // while getting height messages keep connection (Pin20) active
    lastMovementMillis = millis();

    height = h;

    logMessage("Height: " + String(height));

    // Publish height
    mqttClient.publish(mqtt_topic_position, String(height).c_str());
  }
}
int DecodeNumber(byte in) {
  int number;

  switch (in) {
    case 0x3f:
      number = 0;
      break;
    case 0xbf:  //0 with decimal
      number = 0;
      break;
    case 0x06:
      number = 1;
      break;
    case 0x86:  //1 with decimal
      number = 1;
      break;
    case 0x5b:
      number = 2;
      break;
    case 0xdb:  //2 with decimal
      number = 2;
      break;
    case 0x4f:
      number = 3;
      break;
    case 0xcf:  //3 with decimal
      number = 3;
      break;
    case 0x66:
      number = 4;
      break;
    case 0xe6:  //4 with decimal
      number = 4;
      break;
    case 0x6d:
      number = 5;
      break;
    case 0xed:  //5 with decimal
      number = 5;
      break;
    case 0x7d:
      number = 6;
      break;
    case 0xfd:  //6 with decimal
      number = 6;
      break;
    case 0x07:
      number = 7;
      break;
    case 0x87:  //7 with decimal
      number = 7;
      break;
    case 0x7f:
      number = 8;
      break;
    case 0xff:  //8 with decimal
      number = 8;
      break;
    case 0x6f:
      number = 9;
      break;
    case 0xef:  //9 with decimal
      number = 9;
      break;
  }

  return number;
}

// calculate the non-linear (exponential) value for brightness
int getAdjustedBrightness(float brightness) {
  if (brightness == 0.0)
    return 0;
  // use exponential curve, to adapt brightness to human eye perception
  float gamma = 2.2;
  int newBrightness = pow(brightness / maxBrightness, gamma) * maxBrightness + 1;
  if (newBrightness > maxBrightness)
    newBrightness = maxBrightness;
  return newBrightness;
}


void handleScheduledAction() {
  if (nextDeskAction != action_none) {
    unsigned long currentMillis = millis();
    if (currentMillis >= nextDeskActionExecutionTime) {

      switch (nextDeskAction)
      {
        case action_preset1:
          preset1();
          break;
        case action_preset2: 
          preset2();
          break;
        case action_preset3: 
          preset3();
          break;
        case action_preset4: 
          preset4();
          break;                
        case action_moveup:
          moveUp();
          break;
        case action_movedown:
          moveDown();
          break;
      }

      lastDeskAction = nextDeskAction;
      nextDeskAction = action_none;
    }
  }
}

void handleButton() {
  // toggle preset button
  int buttonValue = digitalRead(togglePresetButtonPin);
  if (buttonValue == LOW) {
    if (togglePresetButtonPressed == false) {
      // button rising edge -> do stuff
      togglePresetButtonPressed = true;
      logMessage("Button gedrückt");
      //setLedColor(purple);
      // move desk up or down (preset 1 or 2)
      if (isDeskMoving() && (lastDeskAction != action_movedown)) {
        nextDeskAction = action_movedown;  // if we are moving this will stop the movement
        lastMovementMillis = 0;
        logMessage("Stop");
      } else {
        if ((height > 126.0) || (activePreset == 2)) {
          nextDeskAction = action_preset1;  //close
          logMessage("Preset1");
        } else if ((height < 63.0) || (activePreset == 1)) {
          nextDeskAction = action_preset2;  // open
          logMessage("Preset2");
        } else{
          nextDeskAction = action_preset1;  // move to lowest position
          logMessage("Preset1");
        }
        lastMovementMillis = millis();
      }
      // delay execution of command, if the last command was sent only 1000ms ago
      unsigned long currentMillis = millis();
      if ((nextDeskActionExecutionTime + 2000) > currentMillis) {
        nextDeskActionExecutionTime = nextDeskActionExecutionTime + 2000; // last command was sent less than 1000ms ago --> delay sending so that commands have a send interval of at least 1000ms
        logMessage(" execute delayed");
      } else {
        nextDeskActionExecutionTime = currentMillis;
        logMessage(" execute immediately");        
      }
    }
  } else {
    togglePresetButtonPressed = false;
  }
}

void handleLedRing() {
  unsigned long currentMillis = millis(); 

  if (isDeskMoving() || (digitalRead(togglePresetButtonPin) == LOW)) {
    // flash red with 250ms
    if (currentMillis - previousLedRingMillis >= 250) {
      previousLedRingMillis = currentMillis; 

      // Switch the LED on or off?
      if (digitalRead(ledPinRed) == LOW) {
        setLedColor(red);
      } else {
        setLedColor(off);
      }
    }
  } else {
    if ((height > 126.0) || (activePreset == 2))
      setLedColor(red);
    else if ((height < 63.0) || (activePreset == 1))
      setLedColor(blue);
    else
      setLedColor(yellow);
  }
}

void handleLedStrip() {
  // turn strip on/off when height is above/below 95cm (=30cm open)
  if ((height >= 95.0) && (height <=97.0) && ledStripEnabled) {
    if (height > previousHeight) // moving up
      ledStripAction = DimmingAction::FadeOn; 
    else if (height < previousHeight)  // moving down
      ledStripAction = DimmingAction::FadeOff; 
  } else if (height < 70.0) {
    ledStripAction = DimmingAction::Off; 
  }
  previousHeight = height;

  updateLedStripBrightness();
}

void updateLedStripBrightness() {
  unsigned long currentMillis = millis(); 

  // check whether enough time has passed to take the next step
  if (currentMillis - previousLedStripMillis >= stepInterval) {
    previousLedStripMillis = currentMillis;  // update timestamp

    switch (ledStripAction)
    {
      case FadeOn:
        brightness++;  // increase brightness
        if (brightness >= maxBrightness) {
          ledStripAction = Idle; // value reached
        }
        break;
      case FadeOff: 
        brightness--;  // decrease brightness
        if (brightness <= 0) {
          ledStripAction = Idle; // value reached
        }
        break;
      case On:
        brightness = maxBrightness;
        break;
      case Off:
        brightness = 0;
        break;
      case Idle: 
        break;    
    }

    // set PWM-duty-cycle based on adjusted brightness
    ledcWrite(ledStripPin, getAdjustedBrightness(brightness));
  }
}

bool isDeskMoving() {
  unsigned long currentMillis = millis();          // Get the current time
  if ((currentMillis - lastMovementMillis) < 500)  // height update should be sent at least twice per second, so no updates means no movement
    return true;
  else
    return false;
}

void setLedColor(LedColor color) {
  switch (color) {
    case off:
      digitalWrite(ledPinRed, LOW);
      digitalWrite(ledPinGreen, LOW);
      digitalWrite(ledPinBlue, LOW);
      break;
    case red:
      digitalWrite(ledPinRed, HIGH);
      digitalWrite(ledPinGreen, LOW);
      digitalWrite(ledPinBlue, LOW);
      break;
    case green:
      digitalWrite(ledPinRed, LOW);
      digitalWrite(ledPinGreen, HIGH);
      digitalWrite(ledPinBlue, LOW);
      break;
    case blue:
      digitalWrite(ledPinRed, LOW);
      digitalWrite(ledPinGreen, LOW);
      digitalWrite(ledPinBlue, HIGH);
      break;
    case purple:
      digitalWrite(ledPinRed, HIGH);
      digitalWrite(ledPinGreen, LOW);
      digitalWrite(ledPinBlue, HIGH);  // Rot + Blau = Lila
      break;
    case yellow:
      digitalWrite(ledPinRed, HIGH);
      digitalWrite(ledPinGreen, HIGH);
      digitalWrite(ledPinBlue, LOW);  // Rot + Grün = Gelb
      break;
    case cyan:
      digitalWrite(ledPinRed, LOW);
      digitalWrite(ledPinGreen, HIGH);
      digitalWrite(ledPinBlue, HIGH);  // Grün + Blau = Cyan
      break;
    case white:
      digitalWrite(ledPinRed, HIGH);
      digitalWrite(ledPinGreen, HIGH);
      digitalWrite(ledPinBlue, HIGH);  // Rot + Grün + Blau = Weiß
      break;
  }
}
