# PSoC 6 ZephyrOS Exploration

A collection of embedded projects on the **Infineon CY8CPROTO-062-4343W** prototyping kit,
built with **Zephyr RTOS**. Each project lives in its own folder under [`apps/`](apps) and
explores a different side of the board.

**The board:** PSoC 62 MCU (Cortex-M4 + Cortex-M0+, 2 MB flash, 1 MB SRAM), a CYW4343W
Wi-Fi + Bluetooth radio, CAPSENSE touch sensors, a thermistor, PDM microphones, QSPI flash
and an onboard debugger.

## Projects

| Project | Summary | Built with |
|---|---|---|
| [**Thermo-Blinky**](#thermo-blinky) | Temperature sensor node with an LED readout, a Bluetooth LE beacon and touch controls | Threads, zbus, state machines, BLE, CapSense, flash settings, watchdog |

---

## Thermo-Blinky

📁 [`apps/rtos_blinky`](apps/rtos_blinky) · [full details →](apps/rtos_blinky/README.md)

A blinking LED that grew, one Zephyr subsystem at a time, into a small multithreaded
sensor node.

- 🌡️ **Measures temperature** with the onboard thermistor once a second, timed by a
  hardware timer.
- 💡 **Shows it on the LED**: solid when cold, a slow blink when normal, a fast blink
  when hot.
- 📡 **Broadcasts it over Bluetooth LE**: visible in any BLE scanner, with live readings
  in nRF Connect.
- 👆 **Touch controls**: CAPSENSE buttons switch between temperature and manual modes,
  and the slider sets the blink rate.
- 💾 **Remembers** its settings across power cycles, and recovers from a stuck
  thread with a hardware watchdog.

The modules don't call each other directly. They talk through Zephyr's **zbus** message
channels, and the LED is driven by a **hierarchical state machine**:

```mermaid
flowchart LR
    TMR([Hardware timer<br/>1 s interrupt]) --> TH[Thermistor<br/>ADC]
    TH -- publish --> TC[(temp_chan)]

    TC --> LOG[Logging]
    TC --> STATS[Statistics]
    TC --> LED
    TC --> BLE

    CS([CapSense<br/>buttons + slider]) --> LED
    SW0([Push button]) --> LED
    SH([Serial shell]) --> LED

    LED[LED controller<br/>state machine] -- publish --> LC[(led_chan)]
    LED --> WDT([Watchdog])
    LED <--> SET[(Settings in flash)]
    LC --> BLE[Bluetooth LE<br/>beacon + GATT]
    LC --> CS
```

**Highlights**
- Ports Infineon's CapSense middleware to Zephyr: clock, analog routing and interrupt
  setup that the vendor tooling normally generates.
- Runs the Bluetooth host on the PSoC 6, with the CYW4343W radio as the controller;
  uses a standard Environmental Sensing service plus a custom LED service.
- Measures the temperature in a way that cancels out supply-voltage and ADC-reference
  errors.
- 13 threads, interrupts that hand work off to threads, and a work queue, all running
  in ~288 KB of flash.
