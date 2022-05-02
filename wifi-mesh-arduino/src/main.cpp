/*
  Rui Santos
  Complete project details at https://RandomNerdTutorials.com/esp-mesh-esp32-esp8266-painlessmesh/
  
  This is a simple example that uses the painlessMesh library: https://github.com/gmag11/painlessMesh/blob/master/examples/basic/basic.ino
*/

#include "painlessMesh.h"
#include "IPAddress.h"
#include <Arduino.h>
#include <esp_wifi.h>
#include "WiFi.h"
#include <SPI.h>

//Web Server
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>

#define   MESH_PREFIX     "WasmWifiMesh"
#define   MESH_PASSWORD   "WasmWasi"
#define   MESH_PORT       5555

IPAddress getlocalIP();
IPAddress espIP(0,0,0,0);

Scheduler userScheduler; // to control your personal task
painlessMesh  mesh;

//Webserver
int wl_status = WL_IDLE_STATUS;
AsyncWebServer server(80);

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
        //startTransmit();
    }
}

// User stub
void sendMessage() ; // Prototype so PlatformIO doesn't complain

Task taskSendMessage( TASK_SECOND * 1 , TASK_FOREVER, &sendMessage );

void sendMessage() {
  //This message is for node's ID check. Change for every node.
  String msg = "Hi from node3";
  msg += mesh.getNodeId();
  mesh.sendBroadcast( msg );
  taskSendMessage.setInterval( random( TASK_SECOND * 1, TASK_SECOND * 5 ));
}

// Needed for painless library
void receivedCallback( uint32_t from, String &msg ) {
  Serial.printf("startHere: Received from %u msg=%s\n", from, msg.c_str());
}

void newConnectionCallback(uint32_t nodeId) {
    Serial.printf("--> startHere: New Connection, nodeId = %u\n", nodeId);
}

void changedConnectionCallback() {
  Serial.printf("Changed connections\n");
}

void nodeTimeAdjustedCallback(int32_t offset) {
    Serial.printf("Adjusted time %u. Offset = %d\n", mesh.getNodeTime(),offset);
}

void setup() {
  Serial.begin(115200);

  //setupWifi();

  SPIFFS.begin();

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


//mesh.setDebugMsgTypes( ERROR | MESH_STATUS | CONNECTION | SYNC | COMMUNICATION | GENERAL | MSG_TYPES | REMOTE ); // all types on
  mesh.setDebugMsgTypes( ERROR | STARTUP );  // set before init() so that you can see startup messages

  mesh.init( MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP_STA, 6);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);
  mesh.stationManual("FRITZ!Box 7560 YQ", "19604581320192568195");
  mesh.setHostname("esp-net");

  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  userScheduler.addTask( taskSendMessage );
  taskSendMessage.enable();

  /*if ( !MDNS.begin("esp-net") ) {
    Serial.println( "Error setting up MDNS responder!" );
    while(1) {
        delay(1000);
    }
  }
  Serial.println( "mDNS responder started" );*/

  server.begin();
}

void loop() {
  // it will run the user scheduler as well
  mesh.update();
  if(espIP != getlocalIP()){
    espIP = getlocalIP();
    Serial.println("IP Address: " + espIP.toString());
  }
}

IPAddress getlocalIP() {
  return IPAddress(mesh.getStationIP());
}