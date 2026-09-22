**A Microcontroller-Based Range Extender Module Supporting Legacy Device Compatibility**

Modern devices support 5GHz connectivity to a large extent. However, legacy devices and older hardware lack the required compatibility for the same. 
On the other hand, institutional Wi-Fi coverage often suffers from physical attenuation and signal dead zones. Commercial repeaters are often expensive and consume loads of power. 

The main goal is to build a compact, energy-efficient 2.4 GHz Wi-Fi repeater capable of bridging network coverage using a single ESP32 development board. 

At its core, this project turns the ESP32 into two things at once: a Wi-Fi range extender and a cloud-managed device.

The ESP32 'catches' the signal and re-broadcasts it as a new network: ESP32_REPEATER

Microsoft Azure gives the ESP32 an encrypted internet hotline to Microsoft's cloud servers.
Azure stores this data and it can be viewed through a real-time dashboard. 

In essence, this is a custom Wi-Fi booster that bridges dead zones while simulataneously allowing health monitoring and changing setting from anywhere in the world through a cloud dashboard.

Included: Photo of hardware implementation (HardwareImplementation.jpg)
