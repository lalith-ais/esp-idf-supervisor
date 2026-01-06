
# ESP-IDF  Supervisor
A minimal task supervisor for ESP32 industrial control systems. Built with the philosophy that "simplicity is reliability".

## Overview
This supervisor provides a clean, reliable alternative to ESP-IDF's event loop/callback architecture. It creates isolated service tasks (actors) and monitors them, restarting failed services with exponential backoff. Perfect for 24/7 industrial control systems on ESP32-P4.

### Why Use This Supervisor?
- **No hidden state:** Unlike ESP-IDF callbacks, service state is explicit
- **Isolated failures:** One crashed service doesn't bring down the system
- **Automatic recovery:** Services restart with exponential backoff
- **Essential service protection:** System reboots if critical services die
- **Simple & auditable:** ~250 lines of clean C code
- **Industrial reliability:** Battle-tested on ESP32-P4 RISC-V
