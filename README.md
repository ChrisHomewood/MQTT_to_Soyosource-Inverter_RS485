# MQTT_to_Soyosource-Inverter_RS485
Microcontroller program to connect to soyosource grid tie inverter RS485 limiter connection and provide it with a demand signal based on grid import readings from emonpi published on MQTT. This replaces the current sensing limiter device provided with the inverter and duplicates its RS485 output signal.

Use Arduino programming environment and relevant libraries from the library manager.


# Features:
 - Will send a demand signal to Inverter that attempts to keep electricity imported from Grid at 0 + importbuffer
 - Integration with emonpi via the MQTT communication protocol. Code could be modified to support other sources of grid import power measurement, either via MQTT or direct connection of current sensor to microcontroller.
 - Performs connectivity check with MQTT broker and handles lost connectivity with MQTT, will attempt to reconnect and will default the Inverter to zero output.
 - Performs connectivity check of WiFi and will initiate reconnect if disconnected.
 - Uses ESP32 watchdog timer to reset microcontroller if no MQTT packet recieved for 20 seconds. (this provides some tollerance of bad power supply issues to ESP32)
 - Supports output of status messages via an OLED display screen

# Excluded functionality:
 - Does not implement any "safety" features, relies on inverter to implement low input voltage shutdown and have an appropriate power limit set (this is the normal operating mode for the Inverter and how it is designed to behave).
 - Does not have input bounds checking, will respond to any integer value passed on the subscribed MQTT topic.
 - Does not implement any ramp function for large changes in demand signal, this is not required as the inverter implements its own smoothing algorithm (this is the normal operating mode for the Inverter and how it is designed to behave).
 - Does not have a setting for multiple inverters on same RS485 bus, this can be implemented by modifying code to divide the demand signal by the number of inverters. Without this modification I think this code would follow grid demand but with more oscillation. I do not have multiple inverters so am unable to develop and test this functionality.
 - Older firmware versions of this Inverter output a periodic status message, this program ignores that message. The inverter I have does not output the message so I am unable to test this functionality.

# Future planned functionality:
 - Publishing of status messages to MQTT (including heartbeat message for this program)
 - Temperature monitoring via Dallas 1 wire sensor
 - Inverter Current and voltage monitoring using analog inputs of microcontroller.
 - Logic to interpret these monitoring inputs and publish a request message on MQTT (which will be acted upon by other system components also subscribed to that topic):
    + Request battery charger switch on if export to grid detected (for use with co-located grid tie solar installation)
    + Request battery charger switch off/inhibit if battery temperature too low (below 2c).
    + Request battery charger switch on if ambient temperature is low to heat the batteries by charging them and maintain their temperature above 5c
    + Request Charger or inverter early switch off to keep battery SOC in a particular range to prolong battery cycle life (e.g. 20-80%) - not to be used as a safety feature, inverter and charger hardware must self terminate before 0% or 100% is reached.
    + Computation of power into or out of batteries (based on current and voltage sensor).
    + Estimation of battery state of charge (either by requesting inverter or charger shutdown to take an off-load voltage reading, or including estimate of voltage sag based on load to take an on-load reading).


 Hardware:
 Runs on Heltec WiFi Kit 32 ESP32 based microcontroller board (and clone boards of same spec) to also use built in OLED screen
 Should work (but not tested) on other ESP32 and ESP8266 microcontrollers or other Arduino IDE compatible boards with network connectivity
 Requires an RS485 to Serial TTL converter board. Any chipset should work as the serial baud rate is very low, make sure voltages match
 your microcontroller (e.g. 3.3v input to ESP32). Do not cross over any of the serial connections.
