/*
References:
https://randomnerdtutorials.com/esp-now-esp32-arduino-ide/
https://randomnerdtutorials.com/esp-now-two-way-communication-esp32/
https://github.com/talofer99/ESP32CAM-Capture-and-send-image-over-esp-now
https://randomnerdtutorials.com/esp32-esp-now-wi-fi-web-server/#:~:text=Using%20ESP%2DNOW%20and%20Wi%2DFi%20Simultaneously&text=The%20Wi%2DFi%20channel%20of,by%20your%20Wi%2DFi%20router.&text=You%20can%20set%20up%20the,same%20of%20the%20receiver%20board

Default_MAC_adress_esp32_1: AC:67:B2:20:40:2C
Default_MAC_adress_esp32_2: 10:52:1C:5D:84:18
*/
#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "WiFi.h"
#include <SPI.h>

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
#define MAX_PAYLOAD_SIZE 240 //ESP_NOW_MAX_DATA_LEN (250byte) is not usable due to packet's structure. (it also sends packets flag, total packet size, offset, etc.) 

IM3Environment env;
IM3Runtime runtime;
IM3Module module;
IM3Function calcWasm;
int wasmResult = 0;

//Set MAC addresses of ESP32 receivers
uint8_t broadcastAddress[] = {0x10, 0x52, 0x1C, 0x5D, 0x84, 0x18};
//uint8_t broadcastAddress[] = {0xAC, 0x67, 0xB2, 0x20, 0x40, 0x2C};

typedef struct struct_message {
    float temp;
    float hum;
} struct_message;

// Create a struct_message for the output
struct_message outputReadings;

// Create a struct_message to hold incoming data
struct_message incomingReadings;

// Variable to store if sending data was successful
String success;

int wl_status = WL_IDLE_STATUS;
AsyncWebServer server(80);

//For wasm binary transmission
int currentTransmitOffset = 0;
int numberOfPackets = 0;
byte sendNextPacketFlag = 0;

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
 * ESP-Now: Callback when data is sent
 * @param mac_addr const uint8_t*
 * @param status esp_now_send_status_t
 */
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  
if (numberOfPackets)
  {
    sendNextPacketFlag = 1;
    // if nto suecess 0 resent the last one
    if (status != ESP_NOW_SEND_SUCCESS)
      currentTransmitOffset--;
  } //end if

  Serial.println("sendFlag:");
  Serial.println(sendNextPacketFlag);
  Serial.println("numberOfPackets:");
  Serial.println(numberOfPackets);
  Serial.print("\r\nLast Packet Send Status:\t");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

/**
 * @fn 
 * ESP-Now: Callback when data is alived
 * @param mac const uint8_t *
 * @param data const uint8_t *
 * @param len int
 */
void OnDataRecv(const uint8_t * mac, const uint8_t *data, int len) {
  /*memcpy(&incomingReadings, data, sizeof(incomingReadings));
  Serial.print("Bytes received: ");
  Serial.println(len);
  Serial.println("Temp:");
  Serial.println(incomingReadings.temp);*/

  switch (*data++)
  {
    case 0x01:
      Serial.println("Start of new file transmit");
      currentTransmitOffset = 0;
      currentTransmitOffset = (*data++) << 8 | *data;
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
 * send and check the sending statement
 * @param dataArray uint8_t *, payload
 * @param dataArrayLength uint8_t, payload length 
*/

void sendData(uint8_t * dataArray, uint8_t dataArrayLength) {
  //Serial.print("Sending: "); Serial.println(data);
  //Serial.print("length: "); Serial.println(dataArrayLength);

  esp_err_t result = esp_now_send(broadcastAddress, dataArray, dataArrayLength);
  //Serial.print("Send Status: ");
  if (result == ESP_OK) {
    //Serial.println("Success");
  } else if (result == ESP_ERR_ESPNOW_NOT_INIT) {
    // How did we get so far!!
    Serial.println("ESPNOW not Init.");
  } else if (result == ESP_ERR_ESPNOW_ARG) {
    Serial.println("Invalid Argument");
  } else if (result == ESP_ERR_ESPNOW_INTERNAL) {
    Serial.println("Internal Error");
  } else if (result == ESP_ERR_ESPNOW_NO_MEM) {
    Serial.println("ESP_ERR_ESPNOW_NO_MEM");
  } else if (result == ESP_ERR_ESPNOW_NOT_FOUND) {
    Serial.println("Peer not found.");
  } else {
    Serial.println("Not sure what happened");
  }
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

  //WiFi.mode(WIFI_STA);
  setupWifi();

  SPIFFS.begin();

  //set up for wasm
  run_wasm(NULL);

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Once ESPNow is successfully Init, we will register for Send CB to
  // get the status of Trasnmitted packet
  esp_now_register_send_cb(OnDataSent);
  
  // Register peer
  esp_now_peer_info_t peerInfo;
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;  
  peerInfo.encrypt = false;
  
  // Add peer        
  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("Failed to add peer");
    return;
  }
  // Register for a callback function that will be called when data is received
  esp_now_register_recv_cb(OnDataRecv);


  if ( !MDNS.begin("esp-net") ) {
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

  server.begin();

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
  }*/

  // if the sendNextPackageFlag is set
  if (sendNextPacketFlag)
    sendNextPacket();
  
  wasm_task();
    Serial.println("Wasm result:");
    Serial.println(wasmResult);

  delay(5000);
}

