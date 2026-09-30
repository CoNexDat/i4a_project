# Gateway

This folder contains scripts and configurations for a **generic LAN-to-WAN gateway device**.
The gateway connects the local network of node devices to an external network using **NAT**, while providing a local HTTP server to receive JSON data from ESP32 devices on the LAN.

## Features

For firmware updates through the central ESP32 USB-UART, use
[`flash_node.py`](flash_node.py) and the [node update guide](../docs/actualizacion_firmware.md).
The uploader runs on a PC or Raspberry Pi; distribution inside the node uses SPI.

- **LAN-to-WAN Routing**: Forwards traffic from the LAN interface to the WAN interface using NAT.
- **Automatic Network Setup**: Bash dispatcher script configures IP forwarding, routing, and NAT setup when the LAN interface connects.
- **JSON Receiver**: Flask server listens on LAN for ESP32 devices to send data.
- **Flexible Device**: Can run on any device with two network interfaces.
