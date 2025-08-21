# Desktop Health Assistant Robot - ESP32 Firmware

This project is a fork of [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) that has been modified to serve as a health assistant robot, focusing on medication management and health monitoring.

## Original Project

This firmware is based on the excellent xiaozhi-esp32 project by 虾哥 (Xia Ge), which provides a foundation for ESP32-based AI chatbots with voice interaction capabilities.

## Major Modifications

### Health-Focused Features
- **Medication Reminder System**: Multi-button support for medication confirmation

- **Health Data Management**: Store and track health metrics including medication adherence

### BLE Configuration Mode
- **New BLE Setup Mode**: Configure WiFi and user credentials via Bluetooth Low Energy
  - Eliminates need for WiFi AP mode during initial setup
  - Secure credential exchange with mobile app
  - Automatic device discovery and pairing
  - Real-time configuration status feedback

- **Button-Triggered BLE Mode**: Long press button to enter BLE configuration mode for easy re-configuration

## Configuration

1. Create a `.env.secret` file in the project root with your server URLs

2. Build and flash the firmware:
```bash
idf.py build
idf.py flash
```

## Hardware Requirements

- ESP32-S3 or ESP32-C3 board
- Buttons for medication confirmation
- OLED/LCD display (optional)
- Speaker for audio feedback
- Microphone for voice interaction

## Usage

### Daily Operation
- Voice interaction for health queries
- Button presses to confirm medication intake
- Automatic reminders at scheduled times
- Real-time sync with server

## Acknowledgments

Special thanks to the original xiaozhi-esp32 project creators for providing the excellent foundation for this health assistant implementation.