# 🚀 Learning Journey: Zephyr RTOS on Baremetal nRF52840 Dev Board

Welcome to my personal learning repository dedicated to mastering **Zephyr RTOS** on the **Nordic Semiconductor nRF52840** microcontroller. This repository serves as a showcase of my progression, ranging from basic hardware control (GPIO/Interrupts) to low-power optimizations and secure wireless communication (BLE).

The target hardware used for these projects is the **Pro Micro nRF52840** form-factor development board.

## Repository Structure

Each folder in this repository contains a self-contained Zephyr application with its own configuration (`prj.conf`), source code (`src/main.c`), and hardware description overrides (`.overlay`).

* **`interrupt_lowpower/`** : Managing GPIO interrupts (buttons) combined with Zephyr's power management system to optimize energy consumption.
* **`secure_ble_proximity/`** : A GeoRide-like security system. The nRF52840 acts as a BLE Central, scanning for a specific authenticated badge (smartphone). It automatically triggers a status LED based on proximity, RSSI filtering, and AES-CCM encrypted bonding.

## 🏗️ How to Build and Flash

Zephyr uses the `west` meta-tool to manage dependencies, build applications, and flash firmware. Follow the instructions below to run any project in this repository.

### 1. Prerequisites
Make sure you have initialized your Zephyr workspace and activated your virtual environment:
```bash
# Example inside a Docker container or local workspace
cd /workspaces/zephyrproject
source .venv/bin/activate