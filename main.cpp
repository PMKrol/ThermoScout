#include <max6675.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <LITTLEFS.h>
#define SPIFFS LITTLEFS //aka migration
#include <FS.h>
#include <dhtnew.h>
#include <miniz.h> 

#include "esp_task_wdt.h"

String getCurrentTime();

#define DHTPIN 15      // D15 (GPIO 15) -  DHT22
#define DHTTYPE DHT22  // DHT type

DHTNEW dht(DHTPIN);

unsigned long lastLoopEnd = 0;
unsigned long lastLoopStart = 0;
#define LOOP_TIME 2000

#define SPIFFS_MAX (1024*1024)

float dhtValues[2];  // Tablica do przechowywania temperatury i wilgotności

int processNo = 0;

#define WiFi_Name "Termo8.1 by PMK"
#define WiFi_Pass "12345678"

// Global variable for the filename
String currentFilename = "/data.csv";

// Placeholder
float variable1 = 1.23;
float variable2 = 4.56;
float variable3 = 7.89;

// Ustawienia NTP
const long utcOffsetInSeconds = 2*3600;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.google.com", utcOffsetInSeconds);

// Globals for time
unsigned long ntpTimeAtFetch;
unsigned long processorTimeAtFetch;

// For saving wifi cred. and results
Preferences prefs;

// Data place
const int maxEntries = 14400;  // Max datum no.
int currentEntryIndex = 0;

IPAddress IP;

// MAX6675 (CLK, CS)
const int csPins[] = {5, 18, 19, 23, 26, 25, 33, 32};
const int soPin = 27;  // Common SO (Serial Out)
const int sckPin = 4;  // Common SCK (Clock)

MAX6675* sensors[8];

// Global readings
float temperatureReadings[8];

// Adress of I2C PCF8574 for LCD
// SDA - 21, SCL - 22
LiquidCrystal_I2C lcd(0x27, 20, 4);

AsyncWebServer server(80);

int tvoc1;
int tvoc2;

// Backslash "\" definition
byte backslashChar[8] = {
  0b10000,  // X....
  0b01000,  // .X...
  0b00100,  // ..X..
  0b00010,  // ...X.
  0b00001,  // ....X
  0b00000,  // .....
  0b00000,  // .....
  0b00000   // .....
};

int mq_analog;
float mq_ppm;

//########################### html

// HTML with autorefresh
const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="2">
  <title>Temperatury</title>
  <style>
    body { font-family: Arial, sans-serif; }
    table { width: 100%; border-collapse: collapse; }
    th, td { border: 1px solid #ddd; padding: 8px; }
    th { background-color: #f2f2f2; }
  </style>
</head>
<body>
  <h1>Aktualne Temperatury</h1>
  <div>Logi: <a href=/spiffs> tutaj</a>.</br></div>
  <table>
    <tr><th>Czujnik</th><th>Wartość</th><th>Jednostka</th></tr>
    %TEMPERATURES%
  </table>
</body>
</html>
)rawliteral";


// Wi-Fi settings html
String generateWiFiForm() {
  return "<html><body>"
         "<h1>Ustawienia Wi-Fi</h1>"
         "<form action=\"/save\" method=\"POST\">"
         "SSID: <input type=\"text\" name=\"ssid\"><br>"
         "Hasło: <input type=\"password\" name=\"password\"><br>"
         "<input type=\"submit\" value=\"Zapisz\">"
         "</form></body></html>";
}

// HTML with temp.
String generateHTML() {
  String tempHtml = "";

  //getCurrentTime()
  tempHtml += "<tr><td>Log file</td><td><a href=/get" + currentFilename + ">" + currentFilename + "</a></td><td></td></tr>\n";
  tempHtml += "<tr><td>Time</td><td>" + getCurrentTime() + "</td><td></td></tr>\n";
  tempHtml += "<tr><td>System time</td><td>" + String(millis()) + "</td><td>ms</td></tr>\n";
  
  for (int i = 0; i < 8; i++) {
    tempHtml += "<tr><td>T" + String(i + 1) + "</td><td>" + String(temperatureReadings[i], 1) + "</td><td>°C</td></tr>\n";
  }

  char buf[16];
  String tmpstr;
  
  tempHtml += "<tr><td>DHT0 (temp.)</td><td>" + String(dhtValues[0], 1) + "</td><td>°C</td></tr>\n";
  tempHtml += "<tr><td>DHT1 (hum.)</td><td>" + String(dhtValues[1], 1) + "</td><td>%</td></tr>\n";

  String page = html;
  page.replace("%TEMPERATURES%", tempHtml);
  return page;
}

// Save wifi and reboot
void saveWiFiCredentials(AsyncWebServerRequest *request) {
  if (request->hasParam("ssid", true) && request->hasParam("password", true)) {
    String ssid = request->getParam("ssid", true)->value();
    String password = request->getParam("password", true)->value();

    if (ssid.length() > 0 && password.length() > 0) {
      prefs.putString("ssid", ssid);
      prefs.putString("password", password);

      request->send(200, "text/html", "<html><body><h1>Saved, reboot ESP32...</h1></body></html>");
      delay(3000);  // Poczekaj, aż odpowiedź zostanie wysłana
      ESP.restart();
    } else {
      request->send(400, "text/html", "<html><body><h1>Inproper data</h1></body></html>");
    }
  } else {
    request->send(400, "text/html", "<html><body><h1>No SSID or password</h1></body></html>");
  }
}

// Download results file
void serveFileContent(AsyncWebServerRequest *request) {
  String path = request->url();
  path.replace("/get", "");
  path.replace("%20", " ");

  Serial.println("Requested path: " + path);

  if (!LITTLEFS.exists(path)) {
      request->send(404, "text/plain", "File not found");
      return;
  }

  String contentType = "text/plain";
  if (path.endsWith(".csv")) contentType = "text/csv";
  else if (path.endsWith(".html")) contentType = "text/html";
  else if (path.endsWith(".json")) contentType = "application/json";

  request->send(LITTLEFS, path, contentType);
}

void serveSPIFFSFileList(AsyncWebServerRequest *request) {
    String html = "<html><body>";
    
    html += "<h1>SPIFFS File List</h1>";

    // Generate file list
    html += "<ul>";
    File root = SPIFFS.open("/");
    if (!root || !root.isDirectory()) {
        html += "<li>Error opening directory</li>";
        Serial.println("Error opening directory");
    } else {
        File file = root.openNextFile();
        std::vector<String> fileNames;

        // collect filenames
        while (file) {
            String fileName = "/" + String(file.name());
            fileNames.push_back(fileName);
            file.close();
            file = root.openNextFile();
        }

        // Sort and revert
        std::sort(fileNames.begin(), fileNames.end(), std::greater<String>());

        // Generate HTML
        for (const auto& name : fileNames) {
            String fileUrl = "/get" + name;
            html += "<li><a href=\"" + fileUrl + "\">" + name + "</a></li>";
            Serial.println("File listed: " + name);
        }
    }
    html += "</ul>";
    
    // Remove file.
    html += "<form action=\"/delete_csv\" method=\"post\"><button type=\"submit\">Delete All CSV Files</button></form>";
    
    html += "</body></html>";
    request->send(200, "text/html", html);
}


void deleteAllCSVFiles(AsyncWebServerRequest *request) {
    Serial.println("Delete CSV files requested");

    File root = SPIFFS.open("/");
    if (!root || !root.isDirectory()) {
        Serial.println("Error opening directory for deletion");
        request->send(500, "text/plain", "Error opening directory");
        return;
    }

    File file = root.openNextFile();
    while (file) {
        String fileName = "/" + String(file.name());
        file.close();
        
        if (fileName.endsWith(".csv")) {
            SPIFFS.remove(fileName);
            Serial.println("Deleted file: " + fileName);
        }
        
        file = root.openNextFile();
    }

    request->redirect("/spiffs");

    ESP.restart();
}

void setupWebServer(){
  // Setup web
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String htmlContent = generateHTML();
    request->send(200, "text/html", htmlContent);
  });

  // Wifi credentials
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    String htmlContent = generateWiFiForm();
    request->send(200, "text/html", htmlContent);
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    saveWiFiCredentials(request);
  });

  // Filelist
  server.on("/spiffs", HTTP_GET, [](AsyncWebServerRequest *request) {
      serveSPIFFSFileList(request);
  });

  // Download
  server.on("/get/*", HTTP_GET, [](AsyncWebServerRequest *request) {
      serveFileContent(request);
  });

  // Remove all CSV
  server.on("/delete_csv", HTTP_POST, [](AsyncWebServerRequest *request) {
      deleteAllCSVFiles(request);
  });

  server.begin();
}

//################## HTML


// Get current time
void fetchCurrentTime() {
  timeClient.begin();
  timeClient.update();

  ntpTimeAtFetch = timeClient.getEpochTime();
  processorTimeAtFetch = millis();

  Serial.print("Current time (NTP): ");
  Serial.println(timeClient.getFormattedTime());
  
  Serial.print("CPU time (ms): ");
  Serial.println(processorTimeAtFetch);
}

// Calculate current time
String getCurrentTime() {
  unsigned long currentProcessorTime = millis();
  unsigned long elapsedProcessorTime = currentProcessorTime - processorTimeAtFetch;
  unsigned long currentNtpTime = ntpTimeAtFetch + (elapsedProcessorTime / 1000);

  time_t currentTime = currentNtpTime;
  struct tm* timeInfo = localtime(&currentTime);

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H.%M.%S", timeInfo);

  return String(buffer);
}

// Function to generate a new filename based on the current time
void generateNewFilename() {
    String filename = "/" + getCurrentTime() + ".csv";
    currentFilename = filename;
}

void deleteOldestFile() {
    // Open the root directory
    File root = SPIFFS.open("/");
    if (!root || !root.isDirectory()) {
        Serial.println("Failed to open directory");
        return;
    }

    std::vector<String> files; // Vector to hold filenames

    // Iterate through the files
    File file = root.openNextFile();
    while (file) {
        files.push_back(file.name()); // Add the filename to the vector
        file.close();
        file = root.openNextFile();
    }

    // Sort the files based on their names (lexicographically)
    std::sort(files.begin(), files.end());

    // If we have more than 3 files, proceed to delete the oldest one
    if (files.size() > 3) {
        // Get the name of the oldest file (first after sorting)
        String oldestFileName = files[0];

        // Check if the oldest file is not one of the three youngest files
        if (oldestFileName != files[files.size() - 1] &&
            oldestFileName != files[files.size() - 2] &&
            oldestFileName != files[files.size() - 3]) {
            // Delete the oldest file
            if (SPIFFS.remove(String("/") + oldestFileName)) {
                Serial.println("Deleted file: " + oldestFileName);
            } else {
                Serial.println("Failed to delete file: " + oldestFileName);
            }
        } else {
            Serial.println("Oldest file is one of the three youngest files; not deleting.");
        }
    } else {
        Serial.println("Not enough files to delete.");
    }
}

void printSPIFFSSpace(int longText){
    // Get filesystem info
    uint32_t totalBytes = SPIFFS.totalBytes();
    uint32_t usedBytes = SPIFFS.usedBytes();

    if(longText){
      Serial.print("[SPIFFS] Used bytes: ");
      Serial.print(usedBytes);
      Serial.print(" of max: ");
      Serial.print(SPIFFS_MAX);
      Serial.print(" -> ");
      Serial.print(usedBytes*100/SPIFFS_MAX);
      Serial.println("%");
    }else{
      Serial.print(usedBytes*100/SPIFFS_MAX);
      Serial.print("%");
    }
}

void checkSpaceAndDeleteOldestFile() {
    // Get filesystem info
    uint32_t totalBytes = SPIFFS.totalBytes();
    uint32_t usedBytes = SPIFFS.usedBytes();
    //size_t freeBytes = totalBytes - usedBytes;
    size_t threshold = SPIFFS_MAX;

    if (usedBytes > threshold) {
        printSPIFFSSpace(1);
        // List files and delete the one with the lowest name
        deleteOldestFile();
    }
}
// Function to save measurement data
void saveMeasurementData(float* data, float param1, float param2, float param3, float param4, float mqPPM) {
    // Open file in append mode
    File file = SPIFFS.open(currentFilename, FILE_APPEND);
    
    if (file) {
        // Write data to file
        file.print(getCurrentTime());
        file.print(",");
        file.print(millis());
        file.print(",");
    
        for(int i=0; i < 8; i++){
              file.printf("%.2f,", data[i]);
        }
        
        //file.printf("%.2f,%.2f,%.2f,%.2f,%.2f\n", param1, param2, param3, param4, mqPPM);
        file.print(param3);
        file.print(",");
        file.print(param4);  
        
        file.print("\n");      
        
        file.close();
    }else{
        Serial.println("Failed to open file for writing");
        printSPIFFSSpace(1);
    }

    // Check available space and delete oldest file if necessary
    checkSpaceAndDeleteOldestFile();
}

// Setup ESP32 a AP
void setupAccessPoint() {
  WiFi.mode(WIFI_AP);

  // // Start Access Point
  if(!WiFi.softAP(WiFi_Name, WiFi_Pass)){
    Serial.println("Error setting AP.");
    return;
  }

  // Wyświetlenie informacji na porcie szeregowym
  Serial.println("Access Point set:");
  Serial.print("SSID: ");
  Serial.println(WiFi.softAPSSID());
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  lcd.clear(); 
  lcd.setCursor(0, 0);
  lcd.print("AP: ");
  lcd.print(WiFi.softAPSSID());
  lcd.setCursor(0, 1);
  lcd.print("Pass: ");
  lcd.print(WiFi_Pass);
  lcd.setCursor(0, 2);
  lcd.print("Connect and go to: ");
  lcd.setCursor(0, 3);
  lcd.print(WiFi.softAPIP());
  lcd.print("/wifi");
}

// Initiate Wifi or AP
void connectToWiFi() {
  // Read saved data
  String ssid = prefs.getString("ssid", "");
  String password = prefs.getString("password", "");

  // If no data create Access Point
  if (ssid.isEmpty() || password.isEmpty()) {
    Serial.println("No wifi data!");
    lcd.setCursor(0, 1);
    lcd.print("No wifi data...");
    setupAccessPoint();
    return;
  }

  // Próba połączenia z zapisanym Wi-Fi
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.print("Connecting to ");
  Serial.print(ssid);
  Serial.print("...");
  lcd.setCursor(0, 1);
  lcd.print("Conn to ");
  lcd.print(ssid);
  //lcd.print("...");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  // Jeśli połączenie udane, zakończ
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("Connected to Wi-Fi!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    lcd.setCursor(0, 2);
    lcd.print("Ok!");
    IP = WiFi.localIP();
    return;
  }

  // Jeśli połączenie nieudane, uruchom Access Point
  Serial.println();
  Serial.println("Unable to connect to Wi-Fi.");

  lcd.setCursor(0, 1);
  lcd.print("Wifi error, creating AP.");
  setupAccessPoint();
}

void processSymbol(){
  lcd.setCursor(19, 0); //col, row
  if(processNo == 0){
    lcd.print("-");
  }else if(processNo == 1){
    lcd.write(0);
  }else if(processNo == 2){
    lcd.print("|");
  }else if(processNo == 3){
    lcd.print("/");
  }

  processNo++;
  if(processNo > 3){
    processNo = 0;
  }
}

// Init sensors
void initializeSensors() {
  for (int i = 0; i < 8; i++) {
    sensors[i] = new MAX6675(sckPin, csPins[i], soPin);
  }
}

// Print temp on LCD
void displayTemperaturesOnLCD() {
  lcd.clear();

  for (int i = 0; i < 4; i++) {
    int row = i;  // row no

    //First col
    lcd.setCursor(0, row);
    lcd.print("T");
    lcd.print(i + 1);  // sensor no (T1, T2, T3, ...)
    lcd.print(": ");

    if(temperatureReadings[i]<100){
      lcd.print(" ");
    }

    lcd.print(temperatureReadings[i], 0);

    //second col
    lcd.setCursor(10, row);
    lcd.print("T");
    lcd.print(i + 5);
    lcd.print(": ");

    if(temperatureReadings[i+4]<100){
      lcd.print(" ");
    }

    lcd.print(temperatureReadings[i+4], 0);

  }
}

// read all temp.
void readTemperatures() {
  for (int i = 0; i < 8; i++) {
    temperatureReadings[i] = sensors[i]->readCelsius();
    delay(10);
  }
}

// send results to rs
void sendTemperaturesToSerial() {
  for (int i = 0; i < 8; i++) {
    Serial.print("T");
    Serial.print(i + 1);
    Serial.print(":");
    Serial.print((int)temperatureReadings[i]);
    Serial.print("°C ");
  }
}

// read temp and hum to dht[]
void readDHTData(float dhtData[2]) {
    esp_task_wdt_init(10, false); // max timeout 10s, false = dont auto reset
    esp_task_wdt_add(NULL);       // add to WDT

    int chk = dht.read();

    // read temp and hum.
    float temperature = dht.getTemperature();

    esp_task_wdt_reset();         // reset wdt

    float humidity = dht.getHumidity();        // read hum %

    esp_task_wdt_reset();
    esp_task_wdt_delete(NULL);

    switch (chk)
    {
      case DHTLIB_OK:
        //Serial.print("OK,\t");
        break;
      case DHTLIB_ERROR_CHECKSUM:
        Serial.print("Checksum error,\t");
        return;
        break;
      case DHTLIB_ERROR_TIMEOUT_A:
        Serial.print("Time out A error,\t");
        return;
        break;
      case DHTLIB_ERROR_TIMEOUT_B:
        Serial.print("Time out B error,\t");
        return;
        break;
      case DHTLIB_ERROR_TIMEOUT_C:
        Serial.print("Time out C error,\t");
        return;
        break;
      case DHTLIB_ERROR_TIMEOUT_D:
        Serial.print("Time out D error,\t");
        return;
        break;
      case DHTLIB_ERROR_SENSOR_NOT_READY:
        Serial.print("Sensor not ready,\t");
        return;
        break;
      case DHTLIB_ERROR_BIT_SHIFT:
        Serial.print("Bit shift error,\t");
        return;
        break;
      case DHTLIB_WAITING_FOR_READ:
        //Serial.print("Waiting for read,\t");
        return;
        break;
      default:
        Serial.print("Unknown: ");
        Serial.print(chk);
        Serial.print(",\t");
        return;
        break;
    }

    // save
    dhtData[0] = temperature;  // Index 0: temp
    dhtData[1] = humidity;     // Index 1: hum
}

void setup() {
  Serial.begin(115200);

  // Init LCD
  lcd.begin(20, 4);
  lcd.backlight();
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Hello Prof.!");

  // Init sensors
  initializeSensors();

  // Init Preferences
  prefs.begin("data_storage", false);

  if (!SPIFFS.begin(true)) {
      Serial.println("Failed to mount file system");
      return;
  } else {
      Serial.println("SPIFFS mounted successfully");
  }

  printSPIFFSSpace(1);

  // Read currentEntryIndex from prefs
  currentEntryIndex = prefs.getUInt("currentEntryIndex", 0);

  // Init Wi-Fi
  lcd.setCursor(0, 1);
  lcd.print("Laczenie z WiFi...");
  connectToWiFi();

  // Get NTP time.
  fetchCurrentTime();
  generateNewFilename();

  setupWebServer();


  String currentTime = getCurrentTime();
  Serial.print("Aktualny czas: ");
  Serial.println(currentTime);

  if (WiFi.getMode() & WIFI_AP) {
    Serial.println("ESP32 is in AP mode.");
    
    Serial.print("AP IP Address: ");
    Serial.println(WiFi.softAPIP());

    // Get SSID
    String ssid = WiFi.softAPSSID();
    Serial.print("SSID: ");
    Serial.println(ssid);

    while(true){
      ;
    }
  }
  
  // create special character "\"
  lcd.createChar(0, backslashChar);

  lcd.setCursor(0, 1);
  lcd.print("");
  lcd.setCursor(0, 2);
  lcd.print(currentTime);
  lcd.setCursor(0, 3);
  lcd.print(IP);

  delay(5000);
}

void loop() {

  //wait so measurements are about each 2s.
  //while(millis() - lastLoopEnd < LOOP_TIME){}
  lastLoopStart = millis();

  readTemperatures();
  readDHTData(dhtValues);

  saveMeasurementData(temperatureReadings, tvoc1, tvoc2, dhtValues[0], dhtValues[1], mq_ppm);

  Serial.print("MEM:");
  printSPIFFSSpace(0);
  Serial.print(" ");

  sendTemperaturesToSerial();

  Serial.print("DHT_temp:");
  Serial.print(dhtValues[0]);
  Serial.print("C ");

  Serial.print("DHT_hum:");
  Serial.print(dhtValues[1]);
  Serial.print("% ");

  Serial.println();

  displayTemperaturesOnLCD();

  processSymbol();
  
  while(millis() - lastLoopStart < 2000){
    delay(1);
  }  

}
