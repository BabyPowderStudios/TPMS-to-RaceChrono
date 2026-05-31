#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "PacketIdInfo.h"
#include "tpms.h"

#define SERVICE_UUID "00001ff8-0000-1000-8000-00805f9b34fb"
#define CANBUS_MAIN_UUID "0001"
#define CANBUS_FILTER_UUID "0002"

BLEServer* pServer = nullptr;
BLEAdvertising* pAdvertising = nullptr;

BLECharacteristic* canBusMainCharacteristic = nullptr;
BLECharacteristic* canBusFilterCharacteristic = nullptr;

PacketIdInfo canBusPacketIdInfo;
bool canBusAllowUnknownPackets = false;
bool isCanBusConnected = false;

uint8_t tempData[20] = {};
unsigned long lastSendTime = 0;
unsigned long lastNotifyTime = 0;
const long sendInterval = 1000;

void startRC();
void sendTyreData();

class FilterCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    String value = pCharacteristic->getValue();
    if (value.length() < 1) {
      return;
    }

    const uint8_t command = value[0];
    switch (command) {
      case 0x00:
        if (value.length() == 1) {
          canBusPacketIdInfo.reset();
          canBusAllowUnknownPackets = false;
        }
        break;

      case 0x01:
        if (value.length() == 3) {
          canBusPacketIdInfo.reset();
          canBusPacketIdInfo.setDefaultNotifyInterval(sendInterval);
          canBusAllowUnknownPackets = true;
        }
        break;

      case 0x02:
        if (value.length() == 7) {
          const uint32_t pid = value[3] << 24 | value[4] << 16 | value[5] << 8 | value[6];
          canBusPacketIdInfo.setNotifyInterval(pid, sendInterval);
        }
        break;
    }
  }
};

void startAdvertising() {
  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinInterval(0x20);
  pAdvertising->setMaxInterval(0x40);
  pAdvertising->start();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("TPMS bridge for RaceChrono");
  startRC();
  startTpms();
}

void startRC() {
  BLEDevice::init("RC DIY SIM");
  pServer = BLEDevice::createServer();

  BLEService* pService = pServer->createService(SERVICE_UUID);
  canBusMainCharacteristic = pService->createCharacteristic(
    CANBUS_MAIN_UUID,
    BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ
  );
  canBusMainCharacteristic->addDescriptor(new BLE2902());

  canBusFilterCharacteristic = pService->createCharacteristic(
    CANBUS_FILTER_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  canBusFilterCharacteristic->setCallbacks(new FilterCallback());

  pService->start();
  startAdvertising();
}

void sendTyreData() {
  const unsigned long currentTime = millis();
  if (currentTime - lastNotifyTime < sendInterval) {
    return;
  }
  lastNotifyTime = currentTime;

  for (int sensorIndex = 0; sensorIndex < NUMSENSORS; ++sensorIndex) {
    const uint32_t packetId = sensorIndex + 1;
    const uint16_t pressureInt = static_cast<uint16_t>(pressureBAR[sensorIndex] * 100.0f);
    tempData[4] = static_cast<uint8_t>((pressureInt >> 8) & 0xFF);
    tempData[5] = static_cast<uint8_t>(pressureInt & 0xFF);
    reinterpret_cast<uint32_t*>(tempData)[0] = packetId;

    PacketIdInfoItem* infoItem = canBusPacketIdInfo.findItem(packetId, canBusAllowUnknownPackets);
    if (infoItem && infoItem->shouldNotify()) {
      canBusMainCharacteristic->setValue(tempData, 6);
      canBusMainCharacteristic->notify();
      infoItem->markNotified();
    }
  }

  for (int sensorIndex = 0; sensorIndex < NUMSENSORS; ++sensorIndex) {
    const uint32_t packetId = sensorIndex + 8;
    tempData[4] = static_cast<uint8_t>(temperature[sensorIndex]);
    reinterpret_cast<uint32_t*>(tempData)[0] = packetId;

    PacketIdInfoItem* infoItem = canBusPacketIdInfo.findItem(packetId, canBusAllowUnknownPackets);
    if (infoItem && infoItem->shouldNotify()) {
      canBusMainCharacteristic->setValue(tempData, 5);
      canBusMainCharacteristic->notify();
      infoItem->markNotified();
    }
  }
}

void loop() {
  checkTpms();

  const unsigned long currentTime = millis();
  if (currentTime - lastSendTime >= sendInterval) {
    lastSendTime = currentTime;
    if (isCanBusConnected) {
      sendTyreData();
    }
  }

  if (!isCanBusConnected && pServer->getConnectedCount() > 0) {
    isCanBusConnected = true;
    Serial.println("BLE connected");
    canBusPacketIdInfo.reset();
  } else if (isCanBusConnected && pServer->getConnectedCount() == 0) {
    isCanBusConnected = false;
    Serial.println("BLE disconnected");
    pAdvertising->stop();
    delay(100);
    startAdvertising();
  }
}