// 11.01.2026 v003 Remote VCO chatgpt version OH2BTG wifi toimii ja jonkunlainen webbimenu näkyy
// 12.01.2026 v007 CLK0 1 2 Are possible to set via Wifi and using browser 
// 17.01.2026 v010 Frequency settings and memory write / read is working
// 18.01.2026 v011 added max output drive_strength 8mA freq calibration 
// 19.01.2026 v012 TX GPIO 5 as digital input. TX_ON = LOW, Must not be floating
// 23.01.2026 v014 web serverin ping laski alle 50ms 
// 24.01.2026 v015 CLK0 = TX freq0 CLK1 RX freq1. When RX is on TX freq is changed 10kHz not doing interference with RX 
// Koodi toimii hyvin Shinwa 68MHz radiossa.
// 25.03.2026 recovery after Ubuntu SSD memory failure 
// 28.04.2026 v18 For SRP12  / 50  
// 29.04.2026 v19 For SRP12  / 50 version 17 broke
// 01.05.2026 v20 http page frequency RX and TX input formulas corrected ctcss ms field added
// 02.05.2026 v21 ctcss to ms lookup table calibrated
// 04.05.2026 v22 ctcss drop down menu created
// 05.05.2026 v23 added wifi timeout 3 min from last activity
#include <WiFi.h>
#include <WebServer.h>
#include <si5351.h>        // Etherkit Si5351 library
#include <Wire.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <math.h>

Preferences prefs;

#define PRESET_COUNT 10
uint32_t presets[PRESET_COUNT];
uint32_t freq0, freq1, freq2;

// Constants
const int dacPin = 17;          // A1 DAC1 on GPIO 17 ctcss out
const int TX_in_LOW = 5;         // TX GPIO 5 as digital input. TX_ON = LOW, Must not be floating
const int SQ_open_in = 4;      // GPIO 4 Squelch open LOW = wifi ON
const int resolution = 64;     // 128 Number of points in the sine wave (must match LUT size)
const int amplitude = 127;      // Amplitude of the sine wave (0 to 255 range)
const int offset = 128;         // Offset to center the sine wave within 0 to 255 range
const float epsilon = 0.001;  // Define a small epsilon for float comparison tolerance
float frequency_dac = 118; // Frequency of the sine wave (in Hz) 67.0 Hz to 254.1 Hz,
float frequency_dac_saved = 10;
float number = 29;
float mappedOutput = 100;
float inquiryValue = 118; // The input value you want to check
float outputValue = 122;
int ctcssIndex = 0;
unsigned long lastActivity = 0;
const unsigned long WIFI_TIMEOUT = 10000; // 3 min in ms

// CTCSS-taajuudet (Hz) 0 is first index number, 16 is 118.8
const float ctcssFrequencies[] = { 
  67.0, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4, 88.5, 91.5, 94.8, 97.4, 100.0,
  103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3, 131.8, 136.5, 141.3, 146.2,
  151.4, 156.7, 162.2, 167.9, 173.8, 179.9, 186.2, 192.8, 203.5, 210.7, 218.1,
  225.7, 233.6, 241.8, 250.3
};
// ctcss corresponding delay values in microSeconds
/*const float ctcss_to_micro_s[] = {
  229,212,205,197,191,185,178,172,166,161,156,151,147,141,137,132,128,122,118,114,
  110,106,102,99,95,92,88,86,83,80,77,73,70,67,65,63,61,58
};*/
// 02052026 New calibration for ESP32s2 china version Lolin s2 mini
const float ctcss_to_micro_s[] = {
  224,207,201,193,187,179,173,167,161,155,150,146,
  141,136,131,126,122,117,113,109,105,100,97,
  93,89,86,83,80,77,73,70,67,64,61,
  60,57,54,51
};
const int LUT_SIZE = sizeof(ctcssFrequencies) / sizeof(ctcssFrequencies[0]);
int currentFrequencyIndex = 0;  // Nykyinen taajuus
bool init_once = false;
// Ajastimen muuttujat
esp_timer_handle_t timer = NULL;
volatile bool state = false;  // PWM-tila
int timerInterval = 0;  // Ajastimen väli mikrosekunteina


float mapToLUT(float inputValue) {
  for (int i = 0; i < LUT_SIZE; i++) {
    if (fabs(inputValue - ctcssFrequencies[i]) < 0.5) { // wider tolerance
      return ctcss_to_micro_s[i];

    }
  }

  Serial.println("CTCSS not found!");
  return -1;
}

// Lookup table for sine wave values
int sineLUT[resolution];

// Function to fill the sine wave lookup table
void generateSineLUT() {
  for (int i = 0; i < resolution; i++) {
    float angle = (2.0 * PI * i) / resolution;  // Calculate the angle in radians
    sineLUT[i] = (int)(amplitude * sin(angle) + offset); // Scale and store the sine value
  }
}


// Replace these with your Wi‑Fi network credentials
const char* ssid     = "SRP12/50_OH2BTG_v20";
const char* password = "12345678";

unsigned long lastRead = 0;  // 
const unsigned long readInterval = 200;  // 0.2 second
bool wifiPrinted = false;
bool lastState = HIGH; // RX = High TX = Low
// Kiinteä IP-osoite
IPAddress local_IP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

WebServer server(80);
Si5351 si5351;


String htmlTemplate = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Salora SRP12/50 VCO</title>
</head>
<body>

<h2>Salora SRP12/50 VCO V023 OH2BTG</h2>

<label>RX Frequency (kHz) CLK1 :</label><br>
<b>Memory: %FREQ1% kHz</b><br>
<input type="number" id="freqInput1" placeholder="Enter frequency in kHz">
<button onclick="sendFreq1()">Set Frequency CLK1</button>
<p id="status1"></p>

<label>TX Frequency (kHz) CLK0 :</label><br>
<b>Memory: %FREQ0% kHz</b><br>
<input type="number" id="freqInput0" placeholder="Enter frequency in kHz">
<button onclick="sendFreq0()">Set Frequency CLK0</button>
<p id="status0"></p>

<label>CTCSS (Hz) DAC :</label><br>
<b>Memory: %FREQ2% CTCSS index</b><br>

<select id="ctcssSelect">
  %CTCSS_OPTIONS%
</select>

<button onclick="sendFreq2()">Set CTCSS</button>
<p id="status2"></p>

<script>
function sendFreq0() {
  let f = document.getElementById("freqInput0").value;
  fetch("/setfreq0?value=" + f)
    .then(r => r.text())
    .then(t => document.getElementById("status0").innerText = t);
}

function sendFreq1() {
  let f = document.getElementById("freqInput1").value;
  fetch("/setfreq1?value=" + f)
    .then(r => r.text())
    .then(t => document.getElementById("status1").innerText = t);
}

function sendFreq2() {
  let idx = document.getElementById("ctcssSelect").value;
  fetch("/setfreq2?index=" + idx)
    .then(r => r.text())
    .then(t => document.getElementById("status2").innerText = t);
}

</script>

</body>
</html>
)rawliteral";



void handleRoot() {
 //   server.send(200, "text/html", htmlTemplate);
lastActivity = millis();
 String page = generateHTML();
  server.send(200, "text/html",page);
}

// Handle frequency command CLK0 TX freq
void handleSetFreq0() {
  lastActivity = millis();
  if (server.hasArg("value")) {
    String freqStr0 = server.arg("value");
    uint64_t freqHz0 = freqStr0.toInt();

    if (freqHz0 == 0) {
      server.send(400, "text/plain", "Invalid frequency!");
      return;
    }

    // Set frequency on CLK0
    si5351.set_freq((freqHz0 )*100000ULL , SI5351_CLK0); // TX freq corrected 3kHz up
    Serial.println("TX vco freq0 ");
    Serial.println((freqHz0)*100000ULL); 
    String reply = "Frequency set to CLK0: " + freqStr0 + " kHz";
    server.send(200, "text/plain", reply);
    saveFrequency(0,freqHz0);

    Serial.println(reply);
  } else {
    server.send(400, "text/plain", "Missing value");
  }
}
// Handle frequency command CLK1 RX
void handleSetFreq1() {
  lastActivity = millis();
  if (server.hasArg("value")) {
    String freqStr1 = server.arg("value");
    uint64_t freqHz1 = freqStr1.toInt();

    if (freqHz1 == 0) {
      server.send(400, "text/plain", "Invalid frequency!");
      return;
    }

    // Set frequency on CLK1 RX freq
    si5351.set_freq((freqHz1)*100000ULL , SI5351_CLK1); // not add 10700kHz here
    Serial.println("RX vco freq1 ");
    Serial.println((freqHz1)*100000ULL); 
    String reply = "Frequency set to CLK1: " + freqStr1 + " kHz";
    server.send(200, "text/plain", reply);
    saveFrequency(1,freqHz1);

    Serial.println(reply);
  } else {
    server.send(400, "text/plain", "Missing value");
  }
}

void handleSetFreq2() { // chatgpt version
  lastActivity = millis();
  if (!server.hasArg("index")) {
    server.send(400, "text/plain", "Missing index");
    return;
  }

  int index = server.arg("index").toInt();

  int maxIndex = sizeof(ctcss_to_micro_s) / sizeof(ctcss_to_micro_s[0]);

  if (index < 0 || index >= maxIndex) {
    server.send(400, "text/plain", "Invalid index");
    return;
  }

  float delay_us = ctcss_to_micro_s[index];

  ctcssIndex = index;        // store current selection
  outputValue = delay_us;    // or whatever you use

  saveFrequency(2, index);   // store index, NOT frequency

  Serial.print("CTCSS index: ");
  Serial.println(index);
  Serial.print("Delay us: ");
  Serial.println(delay_us);

  server.send(200, "text/plain", "CTCSS set OK");
}

void saveFrequency(uint8_t index, uint32_t freq) {
  prefs.begin("radio", false);   // open namespace for writing false mean write / read

  char key[10];
  sprintf(key, "freq%d", index);   // freq0, freq1, freq2...
    Serial.print("Saving ");
  Serial.print(key);
  Serial.print(" = ");
  Serial.print(freq);
  Serial.println(" Hz");
  prefs.putUInt(key, freq);
  prefs.end();
  loadAllFrequencies();
}

uint32_t loadFrequency(uint8_t index, uint32_t defaultFreq) {  // get freqs from memory and load to si5351
  prefs.begin("radio", true);   // read-only

  char key[10];
  sprintf(key, "freq%d", index);

  uint32_t freq = prefs.getUInt(key, defaultFreq);

  prefs.end();

  // Convert kHz → Hz
 //  uint64_t freq = (uint64_t)freq_kHz * 1000ULL;
    // Apply to correct clock output
  if (index == 0) si5351.set_freq((freq /12) * 100000ULL, SI5351_CLK0); // TX
  if (index == 1) si5351.set_freq((freq - 10700) * 100000ULL, SI5351_CLK1); // RX
  if (index == 2) {
   ctcssIndex = freq;   // freq actually holds index
   outputValue = ctcss_to_micro_s[ctcssIndex];
  Serial.printf("CTCSS index = %d, delay = %.2f us\n",
                ctcssIndex, outputValue);
  
  }
  //if (index == 2) outputValue = freq; // CTCSS ms
  //if (index == 2) si5351.set_freq(freq * 100ULL, SI5351_CLK2);
 // Serial.printf("Loaded freq%d = %lu Hz\n", index, freq);
  return freq;
}

String generateHTML() {
  Serial.println("Generate html OK ");

  String page = htmlTemplate;
  page.replace("%FREQ0%", String(freq0 )); // TX Freq
  page.replace("%FREQ1%", String((freq1) ));  // RX Freq
  page.replace("%CTCSS_OPTIONS%", generateCTCSSOptions(ctcssIndex));
  page.replace("%FREQ2%", String(freq2)); // CTCSS ms

  return page;
}

String generateCTCSSOptions(int ctcssIndex) {
  String options = "";

  int count = sizeof(ctcssFrequencies) / sizeof(ctcssFrequencies[0]);

  for (int i = 0; i < count; i++) {
    options += "<option value='";
    options += String(i);          // index
    options += "'";

    if (i == ctcssIndex) options += " selected";

    options += ">";
    options += String(ctcssFrequencies[i], 1);
    options += " Hz</option>";
  }

  return options;
}


void loadAllFrequencies() {
  prefs.begin("radio", true);
  freq0 = prefs.getUInt("freq0", 433000000);
  Serial.println("load all frequencies ");
 //  Serial.print(freq0);
  freq1 = prefs.getUInt("freq1", 868000000);
  freq2 = prefs.getUInt("freq2", 915000000);
  prefs.end();
  Serial.print("freq0 ");
  Serial.println(freq0);
  Serial.print("freq1 ");
  Serial.println(freq1);
  Serial.print("CTCSS Hz ");
  Serial.println(freq2);
}
void handleSerial() { // for serial debugging
  static String input = "";
   while (Serial.available()) {
    char c = Serial.read();
    
    if (c == '\r') continue;  // ignore CR
   
    if (c == '\n') {
      input.trim();

      if (input.length() > 0) {
        Serial.println("Received: " + input);

        float number = input.toFloat();
        outputValue = number;
        saveFrequency(2,outputValue); // saving ctcss ms value

       Serial.println(outputValue); // sending to ctcss calculation
      }

      input = "";   // clear buffer
    } else {
      input += c;
    }
  }
}
void SQopen_wifi_on(){
 if (digitalRead(SQ_open_in)==LOW && digitalRead(TX_in_LOW == HIGH )){
    server.handleClient();       
    if(!init_once){
      WiFi.mode(WIFI_AP);
      WiFi.softAP(ssid, password, 1); // last number is ch 1
      esp_wifi_set_ps(WIFI_PS_NONE); // turn off ESP32 wifi pwr save
      // WiFi.begin(, password);
      lastActivity = millis();   //  reset timer here
      Serial.print("SQ mode Connecting to Wi‑Fi");
      Serial.println();
      Serial.print("Connected! IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("Kiinteä IP-osoite: ");
      Serial.println(WiFi.softAPIP());
      server.on("/", handleRoot);
      server.on("/setfreq0", handleSetFreq0);
      server.on("/setfreq1", handleSetFreq1);
      server.on("/setfreq2", handleSetFreq2);
      server.begin();
      Serial.println("Web server started.");
      if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
       Serial.println("IP-konfiguraation asettaminen epäonnistui");
      }
      init_once = true; 
    } 
  }
   
}

void setup() {
  Serial.begin(115200);
 // while (!Serial) {; }  // Blocking untial serial is connected, used for debugging
   delay(2000);
  Serial.println("Welcome to Remote VCO");
 // prefs.begin("radio", false);   // "radio" is namespace, false = read/write
  pinMode(TX_in_LOW, INPUT_PULLUP); // gpio5  TX as digital input RX ON = High
  Serial.println(digitalRead(4));
  pinMode(SQ_open_in, INPUT_PULLUP); // gpio4 SQ open LOW = WIFI ON
  generateSineLUT();
  Serial.println("ctcss Minimal setup");
 

  Wire.begin(7, 6);   // SDA, SCL             // I2C
  si5351.init(SI5351_CRYSTAL_LOAD_6PF, 0, 0);  
  si5351.set_correction( 100000, SI5351_PLL_INPUT_XO);// chrystal calibration ppm word
    // Set output drive strength
  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA);
  si5351.drive_strength(SI5351_CLK1, SI5351_DRIVE_8MA);
  si5351.drive_strength(SI5351_CLK2, SI5351_DRIVE_8MA);
  si5351.output_enable(SI5351_CLK2, 0);

  uint32_t fRead0 = loadFrequency(0, 433000000);
  uint32_t fRead1 = loadFrequency(1, 433000000);
  uint32_t fRead2 = loadFrequency(2, 433000000);
  
  loadAllFrequencies();

}

void loop() { 

      
      // tehdään siniaaltoa lookup taulun arvoista ja ulostulo DAC pin kautta  
   for (int i = 0; i < resolution; i++) {
      // Output the value from the lookup table to the DAC pin
      dacWrite(dacPin, sineLUT[i]);
      delayMicroseconds(outputValue); // testing
      //  Serial.println(outputValue);  
   }
     
     // lastRead = now;
     bool state = digitalRead(TX_in_LOW);
    
    if (state != lastState) {
     lastState = state;
    
     if (state == LOW) {
       Serial.println("Input is TX on (pressed)");
       si5351.set_freq((freq0  / 12)*100000ULL, SI5351_CLK0); //this is send to TX VCO +3kHz
       //Serial.println(freq0);
       si5351.output_enable(SI5351_CLK0, 1);
       si5351.output_enable(SI5351_CLK1, 0);
       Serial.print("TX vco freq0:  ");
       Serial.println((freq0  / 12)*100000ULL );
       WiFi.disconnect(true);
       WiFi.mode(WIFI_OFF);
       Serial.println("Wifi Off");
       init_once = false;
       setCpuFrequencyMhz(80); 
      } else {
      
       //unsigned long now = millis();
        if (WiFi.status() == WL_CONNECTED && !wifiPrinted) {
         Serial.println("RX mode Connected!");
         Serial.print("IP: ");
         Serial.println(WiFi.localIP());
         wifiPrinted = true;
        }
         if (WiFi.getMode() != WIFI_OFF) {
           if (millis() - lastActivity > WIFI_TIMEOUT) {
           Serial.println(millis()); 
           Serial.println("WiFi timeout → shutting down");
           WiFi.mode(WIFI_OFF);
           init_once = false;
          }
  } 
       Serial.println("Input is RX on (released)");
       si5351.set_freq(100*100ULL, SI5351_CLK0);
       si5351.output_enable(SI5351_CLK0, 0);
       si5351.set_freq((freq1 - 10700 ) * 100000ULL , SI5351_CLK1); // this is send to RX VCO
       Serial.print("RX vco freq1: ");
       Serial.println((freq1 - 10700 ) * 100000ULL );
       si5351.output_enable(SI5351_CLK1, 1);
       Serial.print("freq2 CTCSS Hz: ");
       Serial.println(freq2 );
       handleSerial();   
      }
    }
    SQopen_wifi_on();
  
}

