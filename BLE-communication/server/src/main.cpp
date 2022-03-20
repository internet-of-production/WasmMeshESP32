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

//Web Server
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>

#define CALC_INPUT  2
#define FATAL(func, msg) { Serial.print("Fatal: " func " "); Serial.println(msg); return; }
#define CHANNEL 0
//Default MTU of BLE is 23 Byte. Max. size is not usable due to packet's structure. Furthermore, notfy allows only max. 20 Byte. (It also sends packets flag, total packet size, offset, etc.)
#define MAX_PAYLOAD_SIZE 17 
#define bleServerName "Wasm_ESP32"

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/
#define SERVICE_UUID "ed6a9e2f-2408-4b78-a3d6-3aa55f71a38a"


// Wasm Characteristic and Descriptor
BLECharacteristic wasmCharacteristics("f5703842-3515-4da8-ab2e-d40fbf457105", BLECharacteristic::PROPERTY_NOTIFY);
BLEDescriptor wasmDescriptor(BLEUUID((uint16_t)0x2901));

//TODO: Update if overwritten by WebIDE. The current ID in flash. 
uint8_t wasmVersionID = 101;

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
 * @fn
 * reset transmission status (after disconnecting)
 */
void resetTransmissionStatus(){
  currentTransmitOffset = 0;
  sendNextPacketFlag = 0;
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
    resetTransmissionStatus();
    pServer->getAdvertising()->start();
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
  numberOfPackets = ceil(fileSize/MAX_PAYLOAD_SIZE); //split binary data into the MTU size
  Serial.println(numberOfPackets);
  //TODO: Read the current wasmVersionID from flash !!
  //integer value must be splitted into uint_8 because of esp_now_send(). the bit shift >>8 means that it takes second byte. 
  uint8_t message[] = {0x01, numberOfPackets >> 8, (byte) numberOfPackets, wasmVersionID};
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
        Serial.println((String)"Start transmission");
        startTransmit();
    }
}

void ble_start(){
  // Create the BLE Device
  BLEDevice::init(bleServerName);

  Serial.println("ble initilized");

  // Create the BLE Server
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create the BLE Service
  BLEService *wasmService = pServer->createService(SERVICE_UUID);

  // Create BLE Characteristics and Create a BLE Descriptor
  wasmService->addCharacteristic(&wasmCharacteristics);
  wasmDescriptor.setValue("Upload Wasm file");
  wasmCharacteristics.addDescriptor(new BLE2902());

  // Start the service
  wasmService->start();

  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pServer->getAdvertising()->start();
  Serial.println("Waiting a client connection to notify...");
}


/**
 * @fn
 * setup function
*/ 
void setup(){

  Serial.begin(115200);
  Serial.println(WiFi.macAddress());

  //setupWifi();

  SPIFFS.begin();


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

  ble_start();

}

/**
 * @fn
 * loop function
*/ 
void loop(){


  if (deviceConnected) {
      if(newWasmAvailable){
        if(currentTransmitOffset == 0 && !sendNextPacketFlag){
          delay(1000); //Client node cannot get a first packet if the packet is sent just after connecting.
          startTransmit();
        }
        else if(sendNextPacketFlag){
          sendNextPacket();
        }
    }
  }

  delay(50); //Packet loss causes very often if no delay time given
}

