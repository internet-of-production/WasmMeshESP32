/**
 * @file main.cpp
 * @brief Client for Wasm binrary transmission between ESP32s using BLE. 
 * Links of references:
 + https://randomnerdtutorials.com/esp32-ble-server-client/
 */
#include <Arduino.h>
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
//#define MAX_PAYLOAD_SIZE 240
#define bleServerName "Wasm_ESP32"

// BLE Service. Set your service's UUID
static BLEUUID serviceUUID("ed6a9e2f-2408-4b78-a3d6-3aa55f71a38a");

// Wasm Characteristic
static BLEUUID wasmCharacteristicUUID("f5703842-3515-4da8-ab2e-d40fbf457105");

//Flags stating if should begin connecting and if the connection is up
static boolean doConnect = false;
static boolean connected = false;

//Address of the peripheral device. Address will be found during scanning...
static BLEAddress *pServerAddress;
 
//Characteristicd that we want to read
static BLERemoteCharacteristic* wasmCharacteristic;

//Activate notify
const uint8_t notificationOn[] = {0x1, 0x0};
const uint8_t notificationOff[] = {0x0, 0x0};


//Flags to check whether a new wasm packet is available
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



/**
 * @fn 
 * Called when the BLE Server sends a new Wasm file with the notify property
 * @param pBLERemoteCharacteristic BLERemoteCharacteristic*
 * @param data uint8_t*
 * @param len size_t
 * @param isNotify bool
 */
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


/**
 * @fn 
 * Connect to the BLE Server that has the name, Service, and Characteristics
 * @param pAddress BLEAddress
 */
bool connectToServer(BLEAddress pAddress) {
   BLEClient* pClient = BLEDevice::createClient();
 
  // Connect to the remove BLE Server.
  pClient->connect(pAddress);
  Serial.println("Connected to server");
 
  // Obtain a reference to the service we are after in the remote BLE server.
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.print("Failed to find our service UUID: ");
    Serial.println(serviceUUID.toString().c_str());
    return (false);
  }
 
  // Obtain a reference to the characteristics in the service of the remote BLE server.
  wasmCharacteristic = pRemoteService->getCharacteristic(wasmCharacteristicUUID);

  if (wasmCharacteristic == nullptr) {
    Serial.print("Failed to find our characteristic UUID");
    return false;
  }
  Serial.println("Found our characteristics");
 
  //Assign callback functions for the Characteristics
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

 /**
 * @fn 
 * setup function
 */
void setup(){

  Serial.begin(115200);

  SPIFFS.begin();

  //set up for wasm
  run_wasm(NULL);

  //Init BLE device
  BLEDevice::init("");

  Serial.println("ble initilized");

  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->start(30);

}

/**
 * @fn 
 * loop function
 */ 
void loop(){

  // If the flag "doConnect" is true then we have scanned for and found the desired
  // BLE Server with which we wish to connect.  Now we connect to it.  Once we are
  // connected we set the connected flag to be true.
  if (doConnect == true) {
    if (connectToServer(*pServerAddress)) {
      Serial.println("We are now connected to the BLE Server.");
      //Activate the Notify property of Characteristic
      wasmCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t*)notificationOn, 2, true);
      connected = true;
    } else {
      Serial.println("We have failed to connect to the server; Restart your device to scan for nearby BLE server again.");
    }
    doConnect = false;
  }

  
  wasm_task();
    Serial.println("Wasm result:");
    Serial.println(wasmResult);

  delay(5000);
}

