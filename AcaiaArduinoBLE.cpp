/*
  AcaiaArduinoBLE.cpp - Library for connecting to 
  an Acaia Scale using the ArduinoBLE library.
  Created by Tate Mazer, December 13, 2023.
  Released into the public domain.

  Adding Generic Scale Support, Pio Baettig
  Fixing Felicita Arc Bug, A-TWJ

*/
#include "Arduino.h"
#include "AcaiaArduinoBLE.h"
#include <ArduinoBLE.h>

byte IDENTIFY[20]                   = { 0xef, 0xdd, 0x0b, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x9a, 0x6d };
byte HEARTBEAT[7]                   = { 0xef, 0xdd, 0x00, 0x02, 0x00, 0x02, 0x00 };
byte NOTIFICATION_REQUEST[14]       = { 0xef, 0xdd, 0x0c, 0x09, 0x00, 0x01, 0x01, 0x02, 0x02, 0x05, 0x03, 0x04, 0x15, 0x06 };
byte START_TIMER[7]                 = { 0xef, 0xdd, 0x0d, 0x00, 0x00, 0x00, 0x00 };
byte STOP_TIMER[7]                  = { 0xef, 0xdd, 0x0d, 0x00, 0x02, 0x00, 0x02 };
byte RESET_TIMER[7]                 = { 0xef, 0xdd, 0x0d, 0x00, 0x01, 0x00, 0x01 };
byte TARE_ACAIA[6]                  = { 0xef, 0xdd, 0x04, 0x00, 0x00, 0x00 };
byte TARE_GENERIC[6]                = { 0x03, 0x0a, 0x01, 0x00, 0x00, 0x08 };
byte START_TIMER_GENERIC[6]         = { 0x03, 0x0a, 0x04, 0x00, 0x00, 0x0a };
byte STOP_TIMER_GENERIC[6]          = { 0x03, 0x0a, 0x05, 0x00, 0x00, 0x0d };
byte RESET_TIMER_GENERIC[6]         = { 0x03, 0x0a, 0x06, 0x00, 0x00, 0x0c };
byte TARE_START_TIMER_BOOKOO[6]     = { 0x03, 0x0a, 0x07, 0x00, 0x00, 0x00 };
byte BEEP_LEVEL_1_BOOKOO[6]         = { 0x03, 0x0a, 0x02, 0x00, 0x01, 0x0a };
byte TARE_FELICITA[1]               = { 0x54 }; // 'Tare'
byte START_TIMER_FELICITA[1]        = { 0x52 }; // 'Start_Timer'
byte STOP_TIMER_FELICITA[1]         = { 0x53 }; // 'Stop_Timer'
byte RESET_TIMER_FELICITA[1]        = { 0x43 }; // 'Reset_Timer'
byte WEIGHT_TIMER_MODE_FELICITA[1]  = { 0x32 }; // 'Weight+timer mode (known-good wake state)

// Helper struct layout to enable BLEWriteWithoutResponse (0x04) in stock ArduinoBLE
struct RemoteCharAccess {
    void* vtable;
    String uuid;
    int refCount;
    uint16_t connectionHandle;
    uint16_t startHandle;
    uint8_t properties;
};
struct CharAccess {
    void* vtable;
    void* local;
    RemoteCharAccess* remote;
};

/* Generic commands from
   https://github.com/graphefruit/Beanconqueror/blob/master/src/classes/devices/felicita/constants.ts
*/

AcaiaArduinoBLE* AcaiaArduinoBLE::_instance = nullptr;

AcaiaArduinoBLE::AcaiaArduinoBLE(bool debug){
    _debug = debug;
    _currentWeight = 0;
    _connected = false;
    _packetPeriod = 0;
    _lastWrite = 0;
    _rxLen = 0;
}

bool AcaiaArduinoBLE::init(String mac){

    Serial.print("AcaiaArduinoBLE Library v");
    Serial.print(LIBRARY_VERSION);
    Serial.println(" reinitializing...");

    unsigned long start = millis();
    _lastPacket = 0;

    if (mac == ""){
        BLE.scan();
    }else if (!BLE.scanForAddress(mac)){
        Serial.print("Failed to find ");
        Serial.println(mac);
        return false;
    }
    
    do{
        BLEDevice peripheral = BLE.available();

        if(_debug && peripheral){
            // discovered a peripheral, print out address, local name, and advertised service
            Serial.print("Found ");
            Serial.print(peripheral.address());
            Serial.print(" '");
            Serial.print(peripheral.localName());
            Serial.print("' ");
            Serial.print(peripheral.advertisedServiceUuid());
            Serial.println();
        }

        if (peripheral && isScaleName(peripheral.localName())) {
            BLE.stopScan();

            Serial.println("Connecting ...");
            if (peripheral.connect()) {
                Serial.println("Connected");
            } else {
                Serial.println("Failed to connect!");
                return false;
            }

            Serial.println("Discovering attributes ...");
            if (peripheral.discoverAttributes()) {
                Serial.println("Attributes discovered");;
            } else {
                Serial.println("Attribute discovery failed!");
                peripheral.disconnect();
                return false;
            }

            if(_debug){
                // read and print device name of peripheral
                Serial.println();
                Serial.print("Device name: ");
                Serial.println(peripheral.deviceName());
                Serial.print("Appearance: 0x");
                Serial.println(peripheral.appearance(), HEX);
                Serial.println();

                // loop the services of the peripheral and explore each
                for (int i = 0; i < peripheral.serviceCount(); i++) {
                    BLEService service = peripheral.service(i);
                    exploreService(service);
                }
            }

            // Determine type of scale
            if(peripheral.characteristic(READ_CHAR_OLD_VERSION).canSubscribe()){
                Serial.println("Old version Acaia Detected");
                _type   = OLD;
                _write  = peripheral.characteristic(WRITE_CHAR_OLD_VERSION);
                _read   = peripheral.characteristic(READ_CHAR_OLD_VERSION);
            }else if(peripheral.characteristic(READ_CHAR_NEW_VERSION).canSubscribe()){
                Serial.println("New version Acaia Detected");
                _type   = NEW;
                _write  = peripheral.characteristic(WRITE_CHAR_NEW_VERSION);
                _read   = peripheral.characteristic(READ_CHAR_NEW_VERSION);
            } else if(peripheral.characteristic(READ_CHAR_GENERIC).canSubscribe()){
                Serial.println("Generic Scale Detected");
                _type   = GENERIC;
	            _write  = peripheral.characteristic(WRITE_CHAR_GENERIC);
	            _read   = peripheral.characteristic(READ_CHAR_GENERIC);
            } else if(peripheral.characteristic(READ_CHAR_FELICITA).canSubscribe()){
                Serial.println("Felicita Arc Detected");
                _type   = FELICITA;
                _write  = peripheral.characteristic(WRITE_CHAR_FELICITA);
                _read   = peripheral.characteristic(READ_CHAR_FELICITA);
            }
            else{
                Serial.println("unable to determine scale type");
                return false;
            }
            // Enable non-blocking writeCmd in stock ArduinoBLE by setting BLEWriteWithoutResponse flag (0x04)
            CharAccess* ca = (CharAccess*)(void*)&_write;
            if (ca && ca->remote) {
                ca->remote->properties |= 0x04;
            }

            // NEW-style scales stream messages that can be split across (or packed into)
            // notifications, so capture every notification instead of only the latest one.
            _rxLen = 0;
            if(_type == NEW){
                _instance = this;
                _read.setEventHandler(BLEUpdated, onNotification);
            }

            if(!_read.canSubscribe()){
                Serial.println("unable to subscribe to READ");
                return false;
            }else if(!_read.subscribe()){
                Serial.println("subscription failed");
                return false;
            }else {
                Serial.println("subscribed!");
            }
        
            if(_type == OLD || _type == NEW){
                if(writeCommand(IDENTIFY, sizeof(IDENTIFY), true)){
                    Serial.println("identify write successful");
                }else{
                    Serial.println("identify write failed");
                    return false; 
                }
                if(writeCommand(NOTIFICATION_REQUEST, sizeof(NOTIFICATION_REQUEST), true)){
                    Serial.println("notification request write successful");
                }else{
                    Serial.println("notification request write failed");
                return false; 
                                }
            }else if(_type == FELICITA){
                // Wake the Felicita into a deterministic, known-good mode
                // (weight + timer) instead of whatever IDENTIFY used to leave it in.
                if(_write.writeValue(WEIGHT_TIMER_MODE_FELICITA, sizeof(WEIGHT_TIMER_MODE_FELICITA))){
                    Serial.println("felicita weight+timer mode set");
                }else{
                    Serial.println("felicita mode set failed");
                    return false;
                }
            }
            _connected = true;
            _packetPeriod = 0;
            return true;
        }
    }while(millis() - start < 100);

    Serial.println("failed to find scale");
    BLE.stopScan(); // Clean up and stop the BLE hardware scan before exiting!
    return false;    
}

// All command writes now use sizeof() so the byte count always matches the
// array. Felicita commands are single ASCII bytes; sending the right length
// (1) is essential -- the old code wrote 20/7 bytes from these 1-byte arrays,
// leaking adjacent memory to the scale as extra commands.
bool AcaiaArduinoBLE::tare(){
    bool ok;
    switch(_type){
        case GENERIC:  ok = writeCommand(TARE_GENERIC,  sizeof(TARE_GENERIC)); break;
        case FELICITA: ok = writeCommand(TARE_FELICITA, sizeof(TARE_FELICITA)); break;
        default:       ok = writeCommand(TARE_ACAIA,    sizeof(TARE_ACAIA)); break;
    }
    if(ok){
          Serial.println("tare write successful");
          return true;
    }else{
        _connected = false;
        Serial.println("tare write failed");
        return false;
    }
}

bool AcaiaArduinoBLE::startTimer(){
    bool ok;
    switch(_type){
        case GENERIC:  ok = writeCommand(START_TIMER_GENERIC,  sizeof(START_TIMER_GENERIC)); break;
        case FELICITA: ok = writeCommand(START_TIMER_FELICITA, sizeof(START_TIMER_FELICITA)); break;
        default:       ok = writeCommand(START_TIMER,          sizeof(START_TIMER)); break;
    }
    if(ok){
	    Serial.println("start timer write successful");
        return true;
    }else{
        _connected = false;
        Serial.println("start timer write failed");
        return false;
    }
}

bool AcaiaArduinoBLE::stopTimer(){
    bool ok;
    switch(_type){
        case GENERIC:  ok = writeCommand(STOP_TIMER_GENERIC,  sizeof(STOP_TIMER_GENERIC)); break;
        case FELICITA: ok = writeCommand(STOP_TIMER_FELICITA, sizeof(STOP_TIMER_FELICITA)); break;
        default:       ok = writeCommand(STOP_TIMER,          sizeof(STOP_TIMER)); break;
    }
    if(ok){
        Serial.println("stop timer write successful");
        return true;
    }else{
        _connected = false;
        Serial.println("stop timer write failed");
        return false;
    }
}

bool AcaiaArduinoBLE::resetTimer(){
    bool ok;
    switch(_type){
        case GENERIC:  ok = writeCommand(RESET_TIMER_GENERIC,  sizeof(RESET_TIMER_GENERIC)); break;
        case FELICITA: ok = writeCommand(RESET_TIMER_FELICITA, sizeof(RESET_TIMER_FELICITA)); break;
        default:       ok = writeCommand(RESET_TIMER,          sizeof(RESET_TIMER)); break;
    }
    if(ok){
        Serial.println("reset timer write successful");
        return true;
    }else{
        _connected = false;
        Serial.println("reset timer write failed");
        return false;
    }
}

bool AcaiaArduinoBLE::tareStartTimer(){
    if(writeCommand(TARE_START_TIMER_BOOKOO, sizeof(TARE_START_TIMER_BOOKOO))){
          Serial.println("tare and start timer write successful");
          return true;
    }else{
        _connected = false;
        Serial.println("tare and start timer write failed");
        return false;
    }
}

bool AcaiaArduinoBLE::beep(){

    // for acaia's, use the tare command to generate the beep
    if(writeCommand((_type == GENERIC ? BEEP_LEVEL_1_BOOKOO : TARE_ACAIA), 6)){
          Serial.println("beep write successful");
          return true;
    }else{
        _connected = false;
        Serial.println("beep level write failed");
        return false;
    }
}

bool AcaiaArduinoBLE::heartbeat(){
    if(writeCommand(HEARTBEAT, sizeof(HEARTBEAT))){
        _lastHeartBeat = millis();
        return true;
    }else{
        _connected = false;
        return false;
    }
}
float AcaiaArduinoBLE::getWeight(){
    return _currentWeight;
}

bool AcaiaArduinoBLE::heartbeatRequired(){
    if(_type == OLD || _type == NEW){
        return (millis() - _lastHeartBeat) > HEARTBEAT_PERIOD_MS;
    }else{
        return 0;
    }
}
bool AcaiaArduinoBLE::isConnected(){
    return _connected;
}
bool AcaiaArduinoBLE::newWeightAvailable(){
    bool newWeightPacket = false;

    //check how long its been since we last got a response
    if(_lastPacket && millis()-_lastPacket > MAX_PACKET_PERIOD_MS){
        Serial.println("timeout!");
        //reset connection
        _connected = false;
        BLE.disconnect();
        return false;
    }else if(NEW == _type){
        BLE.poll(); // deliver any pending notifications to onNotification()
        newWeightPacket = parseRxBuffer();
    }else if(_read.valueUpdated()){
        byte input[] = {0,0,0,0,0,0,0,0,0,0,0,0,0};
        int l = _read.valueLength();

        // Get packet
        if(10 >= l ||                       //10 byte packets for pre-2021 lunar
          (13 >= l && OLD != _type) ||      //13 byte packets
          (14 == l && OLD == _type) ||      //14 byte packets for lunar 2021 AL008
          (20 == l && GENERIC == _type) ||  //20 byte packets for generic scales
          (18 == l && FELICITA == _type)    //18 byte packets for Felicita Arc
        ){
            _read.readValue(input, (l > 13) ? 13 : l); // readValue() seems to crash whenever l > weight packet (10, 13 or 18)

            if(_debug){
                Serial.print(l);
                Serial.print(": 0x");

                printData(input, (l > 13) ? 13 : l);
                Serial.println();
            }
        }

        // Parse old style data packet
        if( OLD == _type && (l == 10 || l == 14)){
            //Grab weight bytes (2 and 3),
            // apply scaling based on the unit byte (6)
            // get sign byte (7)
            _currentWeight = (((input[3] & 0xff) << 8) + (input[2] & 0xff)) 
                            / pow(10, input[6]) 
                            * ((input[7] & 0x02) ? -1 : 1);
            newWeightPacket = true;

        }else if( GENERIC == _type && l == 20){
            //Grab weight bytes (3-8),
            // get sign byte (2)
            _currentWeight = (( input[7] << 16) | (input[8] << 8) | input[9]);
	        
	          if (input[6] == 45) { // Check if the value is negative
            _currentWeight = -_currentWeight;
                }
            _currentWeight = _currentWeight / 100;
            newWeightPacket = true;

        }else if( FELICITA == _type && l == 18){
            // Felicita Arc 18-byte ASCII packet (matches pyfelicita lib):
            //   byte 2     = sign ('-'/0x2D = negative)
            //   bytes 3..8 = six ASCII weight digits, value / 100 = grams
            _currentWeight = (input[2] == 0x2D ? -1 : 1)
                * ( (input[3] - 0x30) * 1000
                  + (input[4] - 0x30) * 100
                  + (input[5] - 0x30) * 10
                  + (input[6] - 0x30) * 1
                  + (input[7] - 0x30) * 0.1
                  + (input[8] - 0x30) * 0.01 );
            newWeightPacket = true;
        }
    }

    if(newWeightPacket){
        if(_lastPacket){
            _packetPeriod = millis() - _lastPacket;
        }
        _lastPacket = millis();
    }
    return newWeightPacket;
}

// All writes go through here. Acaia scales get a minimum gap between commands:
// the Lunar 2021 intermittently ignores commands that arrive back-to-back
// (e.g. reset timer + start timer + tare at the start of a shot).
bool AcaiaArduinoBLE::writeCommand(const byte* data, int length, bool withResponse){
    if(_type == OLD || _type == NEW){
        while(millis() - _lastWrite < CMD_SPACING_MS){
            BLE.poll(); // keep receiving notifications while waiting
        }
    }
    bool ok = _write.writeValue(data, length, withResponse);
    _lastWrite = millis();
    return ok;
}

void AcaiaArduinoBLE::onNotification(BLEDevice device, BLECharacteristic characteristic){
    if(_instance){
        _instance->appendRx(characteristic.value(), characteristic.valueLength());
    }
}

void AcaiaArduinoBLE::appendRx(const uint8_t* data, int length){
    if(!data || length <= 0){
        return;
    }
    if(_debug){
        Serial.print("rx ");
        Serial.print(length);
        Serial.print(": 0x");
        printData(data, length);
        Serial.println();
    }
    if(length >= RX_BUFFER_SIZE){
        data += length - RX_BUFFER_SIZE;
        length = RX_BUFFER_SIZE;
        _rxLen = 0;
    }else if(_rxLen + length > RX_BUFFER_SIZE){
        // Drop the oldest bytes to make room
        int drop = _rxLen + length - RX_BUFFER_SIZE;
        memmove(_rxBuf, _rxBuf + drop, _rxLen - drop);
        _rxLen -= drop;
    }
    memcpy(_rxBuf + _rxLen, data, length);
    _rxLen += length;
}

// The Lunar 2021 uses a Microchip BM70 "transparent UART" BLE module, which
// forwards the scale's serial stream in arbitrarily sized notifications: one
// message may be split over several notifications, or several messages may
// share one. Reassemble the stream and extract every complete frame:
//   EF DD <cmd> <len> <len-1 bytes: type + payload> <ck1> <ck2>   (len + 5 bytes)
// Returns true if at least one weight message was decoded.
bool AcaiaArduinoBLE::parseRxBuffer(){
    bool newWeight = false;
    int i = 0;

    while(true){
        // Find the next header
        while(i + 1 < _rxLen && !(_rxBuf[i] == 0xEF && _rxBuf[i + 1] == 0xDD)){
            i++;
        }
        if(_rxLen - i < 4){
            break; // need header, cmd and len to know the frame size
        }
        int len = _rxBuf[i + 3];
        int frameLength = len + 5;
        if(len < 1 || frameLength > MAX_FRAME_LENGTH){
            i += 2; // implausible frame, resync on the next header
            continue;
        }
        if(_rxLen - i < frameLength){
            break; // wait for the rest of the frame
        }

        // Event message (0x0C) of type weight (0x05): payload starts at byte 5
        if(_rxBuf[i + 2] == 0x0C && _rxBuf[i + 4] == 0x05 && frameLength >= 13
           && decodeWeight(&_rxBuf[i + 5])){
            newWeight = true;
        }
        i += frameLength;
    }

    if(i > 0){
        memmove(_rxBuf, _rxBuf + i, _rxLen - i);
        _rxLen -= i;
    }
    return newWeight;
}

// payload: weight bytes 0-1 (little endian), unit/decimals byte 4, sign byte 5
bool AcaiaArduinoBLE::decodeWeight(const uint8_t* payload){
    uint8_t unit = payload[4];
    if(unit < 1 || unit > 4){
        return false; // not a valid weight, likely a corrupt frame
    }
    _currentWeight = (((payload[1] & 0xff) << 8) + (payload[0] & 0xff))
                    / pow(10, unit)
                    * ((payload[5] & 0x02) ? -1 : 1);
    return true;
}

bool AcaiaArduinoBLE::isScaleName(String name){
    String nameShort = name.substring(0,5);

    return nameShort == "CINCO"
        || nameShort == "ACAIA"
        || nameShort == "PYXIS"
        || nameShort == "LUNAR"
        || nameShort == "PEARL"
        || nameShort == "PROCH"
        || nameShort == "BOOKO"
        || nameShort == "FELIC";
}

void AcaiaArduinoBLE::exploreService(BLEService service) {
  // print the UUID of the service
  Serial.print("Service ");
  Serial.println(service.uuid());

  // loop the characteristics of the service and explore each
  for (int i = 0; i < service.characteristicCount(); i++) {
    BLECharacteristic characteristic = service.characteristic(i);

    exploreCharacteristic(characteristic);
  }
}

void AcaiaArduinoBLE::exploreCharacteristic(BLECharacteristic characteristic) {
  // print the UUID and properties of the characteristic
  Serial.print("\tCharacteristic ");
  Serial.print(characteristic.uuid());
  Serial.print(", properties 0x");
  Serial.print(characteristic.properties(), HEX);

  // check if the characteristic is readable
  if (characteristic.canRead()) {
    // read the characteristic value
    characteristic.read();

    if (characteristic.valueLength() > 0) {
      // print out the value of the characteristic
      Serial.print(", value 0x");
      printData(characteristic.value(), characteristic.valueLength());
    }
  }
  Serial.println();

  // loop the descriptors of the characteristic and explore each
  for (int i = 0; i < characteristic.descriptorCount(); i++) {
    BLEDescriptor descriptor = characteristic.descriptor(i);

    exploreDescriptor(descriptor);
  }
}

void AcaiaArduinoBLE::exploreDescriptor(BLEDescriptor descriptor) {
  // print the UUID of the descriptor
  Serial.print("\t\tDescriptor ");
  Serial.print(descriptor.uuid());

  // read the descriptor value
  descriptor.read();

  // print out the value of the descriptor
  Serial.print(", value 0x");
  printData(descriptor.value(), descriptor.valueLength());

  Serial.println();
}

void AcaiaArduinoBLE::printData(const unsigned char data[], int length) {
  for (int i = 0; i < length; i++) {
    unsigned char b = data[i];

    if (b < 16) {
      Serial.print("0");
    }

    Serial.print(b, HEX);
  }
}
