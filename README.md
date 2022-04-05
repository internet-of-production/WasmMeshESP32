# WasmMeshESP32
Wasm file transimission over BLE-mesh/simple BLE-communication/ESP-NOW.
### Development environment
- VSCode
- Platform IO
- Targeted board: ESP32dev
- Detailed settings: See platformio.ini file of corresponding project

### BLE-mesh
In development (implementation using esp-idf)

### BLE-communication
BLE-client and BLE-server for wasm transmission (implemetned on the arduino-framework).
A BLE-client node scans advitisement from a ble-server. If it establishes a connection, wasm transmission started.

#### Limitations
- Max payload size of each packet is 17 bytes (+ 3 bytes for notify-header + 3 bytes for infomation header of the transmission). (setMTU function is not available because of supported arduino-framework version in PIO, 05.04.2022)
- Coexistence of WiFi and Bluetooth is not available (?)

### ESP-NOW
Wasm transmission over ESP-NOW (using WiFi). (implemetned on the arduino-framework)

#### Limitations
- Communication system only for ESPs due to the specific protocol ESP-NOW. 
