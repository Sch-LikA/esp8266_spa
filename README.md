Based on the great work over at \
https://github.com/ccutrer/balboa_worldwide_app/wiki
https://github.com/ccutrer/balboa_worldwide_app/blob/master/doc/protocol.md


# esp8266_spa
Control for a Balboa spa controller using the esp8266 (tested on BP2100 and BP601 series)

The sketch connects to the tub, gets an ID and spits out the state to the MQTT broker on topics "Spa/#".
You can control the tub using the subscribed topics, f.e. "Spa/light" with message "ON" for maximum compatability with openhab.

Maybe you need to adjust the sketch to your tub configuration (number of pumps, connection of blower, nr of lights...). You may find the DEBUG comments useful for this task.

Bonus: you may add several relays or such like I did ;-)

# Getting started
- Make sure your Spa is using a Balboa controller
- This is known to work on and developed on a BP2100G0 and BP601 series controllers. Yours may be compatible.
- Get the parts
- Copy `include/secrets.h.example` to `include/secrets.h` and configure Wi-Fi,
  MQTT, and a unique OTA password
- Flash the ESP8266 with PlatformIO
- Connect everything together
- Hook up on the Spa
- Enjoy and get tubbin' ;-)

# Parts
- Get a esp8266, perferable a Wemos D1 Pro in case you need to attach a seperate antenna
- RS485 bus transceiver, (for example the "ARCELI TTL To RS485 Adapter 485 Serial Port UART Level Converter Module 3.3V 5V")
- A DC-DC converter for powering from the Tub (LM2596 for example)
- breadboard, wire etc...


# Hardware connections
![Example](https://github.com/EmmanuelLM/esp8266_spa/blob/master/esp8266_spa_bb.png)
- Look up finding the right wires on https://github.com/ccutrer/balboa_worldwide_app/wiki#physical-layer
- Connect the DC-DC converter to the supply wires (+ and Ground) from the Tub
- Set the DC-DC converter to output 3.3V. This output voltage (+ Ground) should then connect to the Wemos D1 Mini Pro and the RS485 transceiver
- Connect the RS485 transceiver to the A and B wires
- Connect the esp8266-TX to the RS485 TX
- Connect the esp8266-RX to the RS485 RX

![Example](https://github.com/EmmanuelLM/esp8266_spa/blob/master//PXL_20210101_104120166.jpg)

# Debug
- First, check your voltages - the system is running at 3.3V
- The RX (and to some extent the TX) LEDs of the RS485 transceiver (if using the one above) should light up as data goes through. If that is the case you know data is being converted from RS485 to TTL
- The Wemos D1 Mini Pro should spit out data to the MQTT broker (MQTT Spy can be useful here to see: 1. that it is connected to the wifi; 2. that it is connected to the broker). If that is the case, you know the device can communicate over MQTT
- Swap A and B - in my personal experience, if A & B are the wrong way round, the hot tub display (if you have one) will display (NO COMM) as the RS485 traffic get garbled by the esp8266_spa

The main serial port carries Balboa RS485 traffic and must not be used for debug
printing. Diagnostic text is available from the ESP8266 UART1 TX pin (`GPIO2` /
`D4`) at 115200 baud. Connect a 3.3V USB-to-serial adapter's RX line and ground
there; do not use the main RS485 RX/TX lines. Logs include startup and reset
reason, Wi-Fi address and RSSI, MQTT state, OTA events, RS485 client registration,
temperature requests/results, and a 30-second health summary. Passwords are not
logged.

# Updating over Wi-Fi

The serial upload remains necessary once to install an OTA-capable firmware. After
that, connect the development computer to the same local network and run:

```sh
pio run -e ota -t upload
```

The uploader reads `SPA_OTA_PASSWORD` from the ignored `include/secrets.h` file.
It advertises the device as `spa.local`; if multicast DNS is blocked on the
network, override the target with:

```sh
pio run -e ota -t upload --upload-port 192.168.1.50
```

OTA is password-protected and exposed only on the local network. Use a unique
password and do not commit `include/secrets.h`. An update is normally completed
in under a minute on a healthy local Wi-Fi connection. The RS485 timeout reset is
suspended while OTA is active, and the board reboots only after the updater
signals success.

The active target temperature can be changed through MQTT by publishing a
temperature in the spa's current unit to `Spa/target_temp/set`. The controller
accepts only the range for its current high/low setting. Results are published to
`Spa/target_temp/result` as `queued`, `ok`, `rejected: ...`, or `not_confirmed`.

# Appetiser using OpenHab...
![Example](https://github.com/cribskip/esp8266_spa/blob/master/spa_openhab.png)

# HomeAssistant integration
The system uses HomeAssistant autodiscover and should just appear in the MQTT Integration under "Esp Spa"
![Example](https://github.com/EmmanuelLM/esp8266_spa/blob/master//Hassio.png)

# TODO
- Add more documentation
- Add fault reporting
- Add more setting possibilities (filter cycles, preferences maybe)
