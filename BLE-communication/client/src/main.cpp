/**
 * @file main.cpp
 * @brief Client for Wasm binrary transmission between ESP32s using BLE. 
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

#include <SPIFFS.h>

#define WASM_STACK_SLOTS    4000
#define CALC_INPUT  2
#define FATAL(func, msg) { Serial.print("Fatal: " func " "); Serial.println(msg); return; }
#define MAX_PAYLOAD_SIZE 240 //ESP_NOW_MAX_DATA_LEN (250byte) is not usable due to packet's structure. (it also sends packets flag, total packet size, offset, etc.) 
#define bleServerName "Wasm_ESP32"

//Default Temperature is in Celsius
//Comment the next line for Temperature in Fahrenheit
#define temperatureCelsius

// BLE Service. Set your service's UUID
static BLEUUID bmeServiceUUID("ed6a9e2f-2408-4b78-a3d6-3aa55f71a38a");

// BLE Characteristics
#ifdef temperatureCelsius
  //Temperature Celsius Characteristic
  static BLEUUID temperatureCharacteristicUUID("cba1d466-344c-4be3-ab3f-189f80dd7518");
#else
  //Temperature Fahrenheit Characteristic
  static BLEUUID temperatureCharacteristicUUID("f78ebbff-c8b7-4107-93de-889a6a06d408");
#endif

// Humidity Characteristic
static BLEUUID humidityCharacteristicUUID("ca73b3ba-39f6-4ab3-91ae-186dc9577d99");

// Wasm Characteristic
static BLEUUID wasmCharacteristicUUID("f5703842-3515-4da8-ab2e-d40fbf457105");

//Flags stating if should begin connecting and if the connection is up
static boolean doConnect = false;
static boolean connected = false;

//Address of the peripheral device. Address will be found during scanning...
static BLEAddress *pServerAddress;
 
//Characteristicd that we want to read
static BLERemoteCharacteristic* temperatureCharacteristic;
static BLERemoteCharacteristic* humidityCharacteristic;
static BLERemoteCharacteristic* wasmCharacteristic;

//Activate notify
const uint8_t notificationOn[] = {0x1, 0x0};
const uint8_t notificationOff[] = {0x0, 0x0};

//Variables to store temperature and humidity
char* temperatureChar;
char* humidityChar;

//Flags to check whether new temperature and humidity readings are available
boolean newTemperature = false;
boolean newHumidity = false;
boolean newWasm = false;

IM3Environment env;
IM3Runtime runtime;
IM3Module module;
IM3Function calcWasm;
int wasmResult = 0;

// Variable to store if sending data was successful
String success;

//For wasm binary transmission
int currentTransmitOffset = 0;
int numberOfPackets = 0;
byte sendNextPacketFlag = 0;



//When the BLE Server sends a new temperature reading with the notify property
static void temperatureNotifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, 
                                        uint8_t* pData, size_t length, bool isNotify) {
  //store temperature value
  temperatureChar = (char*)pData;
  newTemperature = true;
}

//When the BLE Server sends a new humidity reading with the notify property
static void humidityNotifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, 
                                    uint8_t* pData, size_t length, bool isNotify) {
  //store humidity value
  humidityChar = (char*)pData;
  newHumidity = true;
}

//When the BLE Server sends a new Wasm file with the notify property
static void wasmNotifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* data, size_t len, bool isNotify){

  Serial.println("EntryNotifyCallback");
  Serial.print("Payload size: ");
  Serial.println(len);

  switch (*data++)
  {
    case 0x01:
      Serial.println("Start of new file transmit");
      currentTransmitOffset = 0;
      numberOfPackets = (*data++) << 8 | *data;
      Serial.println("currentNumberOfPackets = " + String(numberOfPackets));
      SPIFFS.remove("/main.wasm");
      break;
    case 0x02:
      currentTransmitOffset = (*data++) << 8 | *data++; 
      File file = SPIFFS.open("/main.wasm",FILE_APPEND);
      if (!file)
        Serial.println("Error opening file ...");
        
      for (int i=0; i < (len-3); i++)
      {
        //byte dat = *data++;
        //Serial.println(dat);
        file.write(*data++);
      }
      file.close();

      if (currentTransmitOffset == numberOfPackets)
      {
        Serial.println("done wasm file transfer");
        File file = SPIFFS.open("/main.wasm");
        Serial.println(file.size());
        file.close();
      }
      
      break;
  } 
}

//Connect to the BLE Server that has the name, Service, and Characteristics
bool connectToServer(BLEAddress pAddress) {
   BLEClient* pClient = BLEDevice::createClient();
 
  // Connect to the remove BLE Server.
  pClient->connect(pAddress);
  Serial.println("Connected to server");
 
  // Obtain a reference to the service we are after in the remote BLE server.
  BLERemoteService* pRemoteService = pClient->getService(bmeServiceUUID);
  if (pRemoteService == nullptr) {
    Serial.print("Failed to find our service UUID: ");
    Serial.println(bmeServiceUUID.toString().c_str());
    return (false);
  }
 
  // Obtain a reference to the characteristics in the service of the remote BLE server.
  //temperatureCharacteristic = pRemoteService->getCharacteristic(temperatureCharacteristicUUID);
  //humidityCharacteristic = pRemoteService->getCharacteristic(humidityCharacteristicUUID);
  wasmCharacteristic = pRemoteService->getCharacteristic(wasmCharacteristicUUID);

  if (wasmCharacteristic == nullptr) {
    Serial.print("Failed to find our characteristic UUID");
    return false;
  }
  Serial.println("Found our characteristics");
 
  //Assign callback functions for the Characteristics
  //temperatureCharacteristic->registerForNotify(temperatureNotifyCallback);
  //humidityCharacteristic->registerForNotify(humidityNotifyCallback);
  wasmCharacteristic->registerForNotify(wasmNotifyCallback);
  return true;
}

//Callback function that gets called, when another device's advertisement has been received
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getName() == bleServerName) { //Check if the name of the advertiser matches
      advertisedDevice.getScan()->stop(); //Scan can be stopped, we found what we are looking for
      pServerAddress = new BLEAddress(advertisedDevice.getAddress()); //Address of advertiser is the one we need
      doConnect = true; //Set indicator, stating that we are ready to connect
      Serial.println("Device found. Connecting!");
    }
  }
};





/**
 * @fn
 * send and check the sending statement
 * @param dataArray uint8_t *, payload
 * @param dataArrayLength uint8_t, payload length 
*/

void sendData(uint8_t * dataArray, uint8_t dataArrayLength) {
  
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
  file.close();

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


  SPIFFS.begin();

  //set up for wasm
  run_wasm(NULL);

  //Init BLE device
  BLEDevice::init("");

  Serial.println("ble initilized");

  // Retrieve a Scanner and set the callback we want to use to be informed when we
  // have detected a new device.  Specify that we want active scanning and start the
  // scan to run for 30 seconds.
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->start(30);

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

  // If the flag "doConnect" is true then we have scanned for and found the desired
  // BLE Server with which we wish to connect.  Now we connect to it.  Once we are
  // connected we set the connected flag to be true.
  if (doConnect == true) {
    if (connectToServer(*pServerAddress)) {
      Serial.println("We are now connected to the BLE Server.");
      //Activate the Notify property of each Characteristic
      //temperatureCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t*)notificationOn, 2, true);
      //humidityCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t*)notificationOn, 2, true);
      wasmCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t*)notificationOn, 2, true);
      connected = true;
    } else {
      Serial.println("We have failed to connect to the server; Restart your device to scan for nearby BLE server again.");
    }
    doConnect = false;
  }

  //if new temperature readings are available, print vales and reset flags
  /*if (newTemperature && newHumidity){
    newTemperature = false;
    newHumidity = false;
    Serial.println(temperatureChar);
    Serial.println(humidityChar);
  }*/
  
  wasm_task();
    Serial.println("Wasm result:");
    Serial.println(wasmResult);

  delay(5000);
}

