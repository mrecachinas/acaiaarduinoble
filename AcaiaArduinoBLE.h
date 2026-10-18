/*
  AcaiaArduinoBLE.h - Library for connecting to 
  an Acaia Scale using the ArduinoBLE library.
  Created by Tate Mazer, December 13, 2023.
  Released into the public domain.

  Pio Baettig: Adding Felicita Arc support 
  A-TWJ: Fixing Felicita Arc bug (issue #15)
  
  Known Bugs:
    * Only supports Grams
*/
#ifndef AcaiaArduinoBLE_h
#define AcaiaArduinoBLE_h

#define LIBRARY_VERSION        "3.3.0"
#define WRITE_CHAR_OLD_VERSION "2a80"
#define READ_CHAR_OLD_VERSION  "2a80"
#define WRITE_CHAR_NEW_VERSION "49535343-8841-43f4-a8d4-ecbe34729bb3"
#define READ_CHAR_NEW_VERSION  "49535343-1e4d-4bd9-ba61-23c647249616"
#define WRITE_CHAR_GENERIC     "ff12"
#define READ_CHAR_GENERIC      "ff11"
#define WRITE_CHAR_FELICITA    "ffe1"   // Felicita Arc: single ffe1 char (service ffe0) for both
#define READ_CHAR_FELICITA     "ffe1"   //   notify (weight) and write (commands)
#define HEARTBEAT_PERIOD_MS     2750
#define MAX_PACKET_PERIOD_MS    5000
#define CMD_SPACING_MS          100     // Protective minimum gap between Acaia commands.
#define RX_BUFFER_SIZE          128     // Reassembly buffer for the Acaia (NEW) notification stream
#define MAX_FRAME_LENGTH        40      // Larger "EF DD" frames are treated as corrupt

#include "Arduino.h"
#include <ArduinoBLE.h>

enum scale_type{
    OLD,     // Lunar (pre-2021)
    NEW,     // Lunar (2021), Pyxis
    GENERIC, // BooKoo Themis, Decent, etc (ff11/ff12, binary protocol)
    FELICITA // Felicita Arc (ffe1, single-byte ASCII protocol)
};

class AcaiaArduinoBLE{
    public:
        AcaiaArduinoBLE(bool debug);
        bool init(String = "");
        bool tare();
        bool startTimer();
        bool stopTimer();
        bool resetTimer();
        bool tareStartTimer();
        bool beep();
        bool heartbeat();
        float getWeight();
        bool heartbeatRequired();
        bool isConnected();
        bool newWeightAvailable();
    private:
        bool isScaleName(String);
        bool writeCommand(const byte* data, int length, bool withResponse = false);

        // Stream reassembly for NEW (Lunar 2021 / Pyxis) scales
        static void onNotification(BLEDevice device, BLECharacteristic characteristic);
        void appendRx(const uint8_t* data, int length);
        bool parseRxBuffer();
        bool decodeWeight(const uint8_t* payload);
        static AcaiaArduinoBLE* _instance;

        //debug functions
        void exploreService(BLEService service);
        void exploreCharacteristic(BLECharacteristic characteristic);
        void exploreDescriptor(BLEDescriptor descriptor);
        void printData(const unsigned char data[], int length);

        float               _currentWeight;
        BLECharacteristic   _write;
        BLECharacteristic   _read;
        long                _lastHeartBeat;
        bool                _connected;
        scale_type          _type;
        bool                _debug; 
        long                _packetPeriod;
        unsigned long       _lastPacket;
        unsigned long       _connectedAt;
        bool                _hasWeight;
        unsigned long       _lastWrite;
        uint8_t             _rxBuf[RX_BUFFER_SIZE];
        int                 _rxLen;
};

#endif
