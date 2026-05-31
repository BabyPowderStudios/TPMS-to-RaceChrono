# TPMS-to-RaceChrono
An ESP32 bridge between 433 MHz TPMS sensors and RaceChrono.

The firmware receives TPMS packets with a CC1101 module and keeps the RaceChrono BLE output unchanged. Sensors are discovered automatically, shown on a small web UI hosted by the ESP32 access point, and the selected sensors are stored in flash.


Thanks to these:  
https://github.com/MagnusThome/TPMS-to-RaceChrono  
https://github.com/NicoEFI/Racechrono-ESP32-S3  
https://github.com/andi38/TPMS  
https://github.com/upiir/arduino_tpms_tire_pressure  

## Hardware

Tested target is an ESP32 DevKit style board with this CC1101 wiring:

- `CS` -> `GPIO15`
- `SCK` -> `GPIO18`
- `MISO` -> `GPIO19`
- `MOSI` -> `GPIO23`
- `GDO0` -> `GPIO5`
- `GDO2` -> `GPIO3`

The receiver is configured for the TPMS truck protocol used by the reference project from andi38: 433.92 MHz, 19.2 kbps, Manchester encoding, sync word `0x001A`.

## Code layout

- `main/main.cpp`: firmware entry point, BLE service for RaceChrono, setup/loop
- `main/tpms.cpp`: CC1101 receiver, sensor tracking, AP web UI, persisted sensor selection
- `main/PacketIdInfo.*`: packet notification throttling used by the RaceChrono BLE bridge

The project no longer depends on an `.ino` sketch file and is intended to be built with PlatformIO only.

## PlatformIO

The project now builds with PlatformIO and pulls external dependencies from the PlatformIO registry.

Build:

```powershell
C:\.platformio\penv\Scripts\platformio.exe run -d .
```

Upload:

```powershell
C:\.platformio\penv\Scripts\platformio.exe run -d . -t upload
```

Serial monitor:

```powershell
C:\.platformio\penv\Scripts\platformio.exe device monitor -d . -b 115200
```

The build uses the `huge_app` ESP32 partition layout because BLE, WiFi, WebServer and RadioLib together exceed the default app partition.

## Sensor selection UI

After boot the ESP32 opens the access point `TPMS-Bridge`.

- Connect to the AP and open `http://192.168.4.1/` in a normal browser
- The page is served as a regular local website, not as a captive portal
- The page shows all recently received 433 MHz sensors
- Choose the wanted sensor for each wheel position `FL`, `FR`, `RL`, `RR`
- `http://192.168.4.1/ping` is available as a minimal reachability check
- The button `Auto-Vorschlaege uebernehmen` copies the current wheel-position suggestions into the persisted selection

Only the selected sensors are forwarded to RaceChrono over the original BLE interface. If no packet has been received from a selected sensor for about 70 minutes, the last pressure is kept and the temperature is forced to `0` as a missing-signal indicator.
  

<img width="400" src="https://github.com/user-attachments/assets/d188f58d-d76c-4e19-bc0c-c9e1b884d5b4" />  

<img width="400" src="https://github.com/user-attachments/assets/fcfa28c5-8430-4c20-b292-80c3b20232b8" />  
  
![PXL_20250808_121657496](https://github.com/user-attachments/assets/058de4e5-72ef-4998-9203-bde412110fdf)

## Setting up in Racechrono  
  
In Racechrono add the DIY BLE under "Add other device".   
Then add these eight channels in your car's settings:  
  
<img width="400" src="https://github.com/user-attachments/assets/73a66d05-20b3-4a0e-a827-9919c5fbdc06" />
  
## Pressure  
  
The four tyre *pressure* channels have the follwing PIDs  
0x01 = FL Front Left  
0x02 = FR Front Right  
0x03 = RL Rear Left  
0x04 = RR Rear Right  
Note the "Equation" also in the picture   
  
<img width="400" src="https://github.com/user-attachments/assets/825ac94f-d89e-4fb6-9f20-22d99df5e1cb" />
  
## Temperature  
  
The four tyre *temperature* channels have the follwing PIDs  
0x08 (08) = FL Front Left  
0x09 (09) = FR Front Right  
0x0A (10) = RL Rear Left  
0x0B (11) = RR Rear Right  
Note the "Equation" also in the picture    

When a sensor hasn't reported anything in five minutes an "alarm" is raised by reporting the temperature as 0 degrees but the previous older read of pressure is kept.
  
<img width="400" src="https://github.com/user-attachments/assets/04dd2139-6c74-41ba-9762-cf8261e29d31" />
