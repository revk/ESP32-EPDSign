# ESP32-Doorbell

<img src=Manual/Door.jpg width=50% align=right border=10>

This project is designed to provide an interactive doorbell sign. It can connect to a bell push directly.

The concept is simple, the sign shows a suitable idle message (e.g. house number) but on bell push changes to a different message. The system has automatic seasonal variations on the idle message.

The simple case is a "please wait, we are on the way" messaged to acknowledge the bell push, but it can be customised via web hooks or MQTT or tracking the state of tasmota switches. This means a message could be different when "out" (e.g. leave behind gates, or in secure bin, etc), but without that message on the door all the time.

The bell push itself typically works a bell via MQTT, such as a tasmota switch.

The code runs on an ESP32-S3 and uses the PCB designs from [ESP32-GFX EPD75](https://github.com/revk/ESP32-GFX/tree/main/PCB/EPD75). This includes an LED in each corner and fits the Waveshare 7.5" e-paper. The Waveshare is available with laminated glass front as per this image, and the PCB needs a waterproof coating. Be wary of encasing in resin as the FPC connector can stop working.

[Manual](Manual/Manual.md)