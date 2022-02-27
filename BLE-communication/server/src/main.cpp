/**
 * @file main.cpp
 * @brief Wasm binrary transmission between ESP32s using BLE.  
 * Links of references:
 + https://randomnerdtutorials.com/esp32-ble-server-client/
 */
#include <Arduino.h>
#include <esp_wifi.h>
#include "WiFi.h"
#include <SPI.h>

//BLE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

//wasm3
#include "wasm3.h"
#include "m3_env.h"
#include "wasm3_defs.h"

//Web Server
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>

#define WASM_STACK_SLOTS    4000
#define CALC_INPUT  2
#define FATAL(func, msg) { Serial.print("Fatal: " func " "); Serial.println(msg); return; }
#define CHANNEL 0
//Default MTU of BLE is 23 Byte. Max. size is not usable due to packet's structure. Furthermore, notfy allows only max. 20 Byte. (It also sends packets flag, total packet size, offset, etc.)
#define MAX_PAYLOAD_SIZE 15 
#define bleServerName "Wasm_ESP32"

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/
#define SERVICE_UUID "ed6a9e2f-2408-4b78-a3d6-3aa55f71a38a"

//Default Temperature is in Celsius
//Comment the next line for Temperature in Fahrenheit
#define temperatureCelsius

// Temperature Characteristic and Descriptor
#ifdef temperatureCelsius
  BLECharacteristic bmeTemperatureCelsiusCharacteristics("cba1d466-344c-4be3-ab3f-189f80dd7518", BLECharacteristic::PROPERTY_NOTIFY);
  BLEDescriptor bmeTemperatureCelsiusDescriptor(BLEUUID((uint16_t)0x2902));
#else
  BLECharacteristic bmeTemperatureFahrenheitCharacteristics("f78ebbff-c8b7-4107-93de-889a6a06d408", BLECharacteristic::PROPERTY_NOTIFY);
  BLEDescriptor bmeTemperatureFahrenheitDescriptor(BLEUUID((uint16_t)0x2901));
#endif

// Humidity Characteristic and Descriptor
BLECharacteristic bmeHumidityCharacteristics("ca73b3ba-39f6-4ab3-91ae-186dc9577d99", BLECharacteristic::PROPERTY_NOTIFY);
BLEDescriptor bmeHumidityDescriptor(BLEUUID((uint16_t)0x2903));

// Wasm Characteristic and Descriptor
BLECharacteristic wasmCharacteristics("f5703842-3515-4da8-ab2e-d40fbf457105", BLECharacteristic::PROPERTY_NOTIFY);
BLEDescriptor wasmDescriptor(BLEUUID((uint16_t)0x2904));

IM3Environment env;
IM3Runtime runtime;
IM3Module module;
IM3Function calcWasm;
int wasmResult = 0;


// Variable to store if sending data was successful
String success;

int wl_status = WL_IDLE_STATUS;
AsyncWebServer server(80);

//For wasm binary transmission
int currentTransmitOffset = 0;
int numberOfPackets = 0;
byte sendNextPacketFlag = 0;

//! If a client is connected to the server, the state is true. If the client disconnects, the boolean variable changes to false.
bool deviceConnected = false;

float temp;
float tempF;
float hum;

// Timer variables
unsigned long lastTime = 0;
unsigned long timerDelay = 30000;

//! This flag == true: a new wasm file is available.
bool newWasmAvailable = true;

/**
 * @fn 
 * Get WiFi channel
 * @param ssid SSID of the router (const char*)
 */
int32_t getWiFiChannel(const char *ssid) {
  if (int32_t n = WiFi.scanNetworks()) {
      for (uint8_t i=0; i<n; i++) {
          if (!strcmp(ssid, WiFi.SSID(i).c_str())) {
              return WiFi.channel(i);
          }
      }
  }
  return 0;
}


/**
 * @fn
 * WiFi setup. One must set up the same WiFi channel of the receiver board. 
 * The Wi-Fi channel of the receiver board is automatically assigned by the Wi-Fi router => check the channel from the router.
 * The Wi-Fi mode of the receiver board must be access point and station (WIFI_AP_STA).
 */
void setupWifi() {

  const char* ssid = "FRITZ!Box 7560 YQ";
  const char* password = "19604581320192568195";
  //const char* ssid = RWTH_ID;
  //const char* password = RWTH_PASS;

  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  // Set the device as a Station and Soft Access Point simultaneously
  WiFi.mode(WIFI_AP_STA);
  //esp_wifi_set_mac(WIFI_IF_STA, &newMACAddress[0]);

  int32_t channel = getWiFiChannel(ssid);

  WiFi.printDiag(Serial); // Uncomment to verify channel number before
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  WiFi.printDiag(Serial);
  
  WiFi.begin(ssid, password);

  while (wl_status != WL_CONNECTED) {
    delay(500);
    Serial.print(wl_status);
    wl_status = WiFi.status();
  }

  randomSeed(micros());

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

/**
 * @class MyServerCallbacks
 * @brief Setup callbacks onConnect and onDisconnect. (BLEServerCallbacks)
 */

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
  };
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
  }
};


/**
 * @fn
 * send and check the sending statement
 * @param dataArray uint8_t *, payload
 * @param dataArrayLength uint8_t, payload length 
*/

void sendData(uint8_t * dataArray, uint8_t dataArrayLength) {
    wasmCharacteristics.setValue(dataArray, dataArrayLength);
    wasmCharacteristics.notify();
    sendNextPacketFlag = 1;
}


/**
 * @fn
 * Starting transmission. The first message just imforms the number of packets. (without main data)
 * Message structure (uint8_t *):
 * - First message
 * | message flag | second byte of the number of packets | first byte of the number of packets |
 * - Other messages 
 * | message flag | second byte of the number of offset | first byte of the offset | payload |
 * Flag for the first message: 0x01
 * Otherwise: 0x02
*/
void startTransmit()
{
  Serial.println("Starting transmit");
  File file = SPIFFS.open("/main.wasm", "r");
  if (!file) {
    Serial.println("Failed to open file in reading mode");
    return;
  }
  Serial.println(file.size());
  double fileSize = file.size();
  file.close();
  currentTransmitOffset = 0;
  numberOfPackets = ceil(fileSize/MAX_PAYLOAD_SIZE); //split binary data into the esp-now-max-length (250 Bytes)
  Serial.println(numberOfPackets);
   //integer value must be splitted into uint_8 because of esp_now_send(). the bit shift >>8 means that it takes second byte. 
  uint8_t message[] = {0x01, numberOfPackets >> 8, (byte) numberOfPackets};
  sendData(message, sizeof(message));
}


/**
 * @fn
 * This function will be called in loop() after calling startTransimit() and start to send the payload.
 * It compares current offset of the sent packet and the total number of packets.
 * If offset < total, then continues transmission using the flag. (The flag will be set to 1 in OnDataSent, i.e., after calling startTransimit())
*/
void sendNextPacket()
{
  // claer the flag
  sendNextPacketFlag = 0;

  // if got to AFTER the last package
  if (currentTransmitOffset == numberOfPackets)
  {
    currentTransmitOffset = 0;
    numberOfPackets = 0;
    newWasmAvailable = false;
    Serial.println("Done submiting files");
    //takeNextPhotoFlag = 1;
    return;
  } //end if

  //first read the data.
  File file = SPIFFS.open("/main.wasm", "r");
  if (!file) {
    Serial.println("Failed to open file in writing mode");
    return;
  }

  // set array size.
  int fileDataSize = MAX_PAYLOAD_SIZE; // if its the last package - we adjust the size !!!
  if (currentTransmitOffset == numberOfPackets - 1)
  {
    Serial.println("*************************");
    Serial.println(file.size());
    Serial.println(numberOfPackets - 1);
    Serial.println((numberOfPackets - 1)*MAX_PAYLOAD_SIZE);
    fileDataSize = file.size() - ((numberOfPackets - 1) * MAX_PAYLOAD_SIZE);
  }

  // define message array
  uint8_t messageArray[fileDataSize + 3];
  messageArray[0] = 0x02;

  //offset
  file.seek(currentTransmitOffset * MAX_PAYLOAD_SIZE);
  currentTransmitOffset++;

  messageArray[1] = currentTransmitOffset >> 8;
  messageArray[2] = (byte) currentTransmitOffset;
  for (int i = 0; i < fileDataSize; i++)
  {
    if (file.available())
    {
      messageArray[3 + i] = file.read();
    } //end if available
    else
    {
      Serial.println("END !!!");
      break;
    }
  } //end for

  sendData(messageArray, sizeof(messageArray));
  Serial.print("Next packet transmitted! ");
  Serial.print(numberOfPackets - currentTransmitOffset);
  Serial.println(" packets remain");
  file.close();

}

//TODO: If the simultaneous run of WiFi and BLE is possible, set true to newWasm flag after uploading
/**
 * @fn 
 * Browser Editer: Handle uploaded data
 * @param request AsyncWebServerRequest *
 * @param filename String
 * @param index size_t
 * @param data uint8_t *
 * @param len size_t
 * @param final bool
 */
void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
    if (!index) {
        Serial.println((String)"UploadStart: " + filename);
        // open the file on first call and store the file handle in the request object
        request->_tempFile = SPIFFS.open("/" + filename, "w");
    }
    if (len) {
        // stream the incoming chunk to the opened file
        request->_tempFile.write(data, len);
    }
    if (final) {
        Serial.println((String)"UploadEnd: " + filename + "," + index+len);
        // close the file handle as the upload is now done
        request->_tempFile.close();
        request->send(200, "text/plain", "File Uploaded !");
        Serial.println((String)"Start broadcast via ESP-NOW");
        startTransmit();
    }
}

/**
 * @fn 
 * WASM setup using wasm3
 */

static void run_wasm(void*)
{
  // load wasm from SPIFFS
  /* If default CONFIG_ARDUINO_LOOP_STACK_SIZE 8192 < wasmFile,
  a new size must be given in  \Users\<user>\.platformio\packages\framework-arduinoespressif32\tools\sdk\include\config\sdkconfig.h
  https://community.platformio.org/t/esp32-stack-configuration-reloaded/20994/2
  */
  File wasmFile = SPIFFS.open("/main.wasm", "r");
  unsigned int build_main_wasm_len = wasmFile.size();
  Serial.println("wasm_length:");
  Serial.println(build_main_wasm_len);
  // read file
  unsigned char build_main_wasm[build_main_wasm_len];
  wasmFile.readBytes((char *) build_main_wasm, build_main_wasm_len);

  Serial.println("Loading WebAssembly was successful");

  M3Result result = m3Err_none;

  //it warks also without using variable
  //uint8_t* wasm = (uint8_t*)build_app_wasm;

  env = m3_NewEnvironment ();
  if (!env) FATAL("NewEnvironment", "failed");

  runtime = m3_NewRuntime (env, WASM_STACK_SLOTS, NULL);
  if (!runtime) FATAL("m3_NewRuntime", "failed");

  #ifdef WASM_MEMORY_LIMIT
    runtime->memoryLimit = WASM_MEMORY_LIMIT;
  #endif

   result = m3_ParseModule (env, &module, build_main_wasm, build_main_wasm_len);
   if (result) FATAL("m3_ParseModule", result);

   result = m3_LoadModule (runtime, module);
   if (result) FATAL("m3_LoadModule", result);

   // link
   //result = LinkArduino (runtime);
   //if (result) FATAL("LinkArduino", result);


   result = m3_FindFunction (&calcWasm, runtime, "calcWasm");
   if (result) FATAL("m3_FindFunction(calcWasm)", result);

   Serial.println("Running WebAssembly...");

}

  /**
 * @fn 
 * Call WASM task
 */

  void wasm_task(){
    const void *i_argptrs[CALC_INPUT];
    byte inputBytes[CALC_INPUT] = {0x01, 0x02};
    M3Result result = m3Err_none;
    
    for(int i=0; i<CALC_INPUT ;i++){
      i_argptrs[i] = &inputBytes[i];
    }

    /*
    m3_Call(function, number_of_arguments, arguments_array)
    To get return, one have to call a function with m3_Call first, then call m3_GetResultsV(function, adress).
    (Apparently) m3_Call stores the result in the liner memory, then m3_GetResultsV accesses the address.
    */
    result = m3_Call(calcWasm,CALC_INPUT,i_argptrs);                       
    if(result){
      FATAL("m3_Call(calcWasm):", result);
    }

    result = m3_GetResultsV(calcWasm, &wasmResult);
      if(result){
      FATAL("m3_GetResultsV(calcWasm):", result);
    }

  }

void setup(){

  Serial.begin(115200);
  Serial.println(WiFi.macAddress());

  //setupWifi();

  SPIFFS.begin();

  //set up for wasm
  //run_wasm(NULL);


  /*if ( !MDNS.begin("esp-net") ) {
    Serial.println( "Error setting up MDNS responder!" );
    while(1) {
        delay(1000);
    }
  }
  Serial.println( "mDNS responder started" );

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, PUT");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/ide.html");
  });

  server.on("/arduino.ts", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/arduino.ts");
  });

  server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
      request->send(200);
      }, handleUpload);

  server.serveStatic("/", SPIFFS, "/").setDefaultFile("ide.html");

  server.onNotFound([](AsyncWebServerRequest *request) {
      if (request->method() == HTTP_OPTIONS) {
          request->send(200);
      } else {
          Serial.println("Not found");
          request->send(404, "Not found");
      }
  });

  server.begin();*/

  // Create the BLE Device
  BLEDevice::init(bleServerName);

  Serial.println("ble initilized");

  // Create the BLE Server
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create the BLE Service
  BLEService *bmeService = pServer->createService(SERVICE_UUID);

  // Create BLE Characteristics and Create a BLE Descriptor
  // Temperature
  #ifdef temperatureCelsius
    bmeService->addCharacteristic(&bmeTemperatureCelsiusCharacteristics);
    bmeTemperatureCelsiusDescriptor.setValue("BME temperature Celsius");
    bmeTemperatureCelsiusCharacteristics.addDescriptor(&bmeTemperatureCelsiusDescriptor);
  #else
    bmeService->addCharacteristic(&bmeTemperatureFahrenheitCharacteristics);
    bmeTemperatureFahrenheitDescriptor.setValue("BME temperature Fahrenheit");
    bmeTemperatureFahrenheitCharacteristics.addDescriptor(&bmeTemperatureFahrenheitDescriptor);
  #endif 

  // Humidity
  bmeService->addCharacteristic(&bmeHumidityCharacteristics);
  bmeHumidityDescriptor.setValue("BME humidity");
  bmeHumidityCharacteristics.addDescriptor(new BLE2902());

  // Wasm
  bmeService->addCharacteristic(&wasmCharacteristics);
  wasmDescriptor.setValue("Upload Wasm file");
  wasmCharacteristics.addDescriptor(new BLE2902());

  // Start the service
  bmeService->start();

  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pServer->getAdvertising()->start();
  Serial.println("Waiting a client connection to notify...");

}
 
void loop(){

  //outputReadings.temp = 18;
  //outputReadings.hum = 60;

  // Send message via ESP-NOW
  /*esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &outputReadings, sizeof(outputReadings));

  if (result == ESP_OK) {
    Serial.println("Sent with success");

    wasm_task();
    Serial.println("Wasm result:");
    Serial.println(wasmResult);
  }
  else {
    Serial.println("Error sending the data");
  }

  // if the sendNextPackageFlag is set
  if (sendNextPacketFlag)
    sendNextPacket();*/

  if (deviceConnected) {
    //if ((millis() - lastTime) > timerDelay) {
      if(newWasmAvailable){
        if(currentTransmitOffset == 0 && !sendNextPacketFlag){
          startTransmit();
        }
        else if(sendNextPacketFlag){
          sendNextPacket();
        }
      
      
      //lastTime = millis();
    //}
    }
  }
  
  /*wasm_task();
    Serial.println("Wasm result:");
    Serial.println(wasmResult);*/

  delay(1000);
}

