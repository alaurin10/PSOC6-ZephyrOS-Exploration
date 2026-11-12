# CapSense configuration

Generated CapSense configuration for the CY8CPROTO-062-4343W, taken unmodified from
Infineon's board support package
[TARGET_CY8CPROTO-062-4343W `release-v3.1.0`](https://github.com/Infineon/TARGET_CY8CPROTO-062-4343W/tree/release-v3.1.0/COMPONENT_BSP_DESIGN_MODUS)
(Apache-2.0):

| File | Contents |
|---|---|
| `design.cycapsense` | CapSense Configurator project (open/tune with ModusToolbox CapSense Configurator) |
| `cycfg_capsense.c/.h`, `cycfg_capsense_defines.h` | Code generated from it: 2 CSX buttons, 5-segment CSD slider |
| `cycfg_peripherals.h` | **Hand-written** replacement for the BSP file: pins, clocks, and the clock divider index (changed to 8-bit divider 7) |

The CapSense middleware itself (`Infineon/capsense` `release-v3.0.1`, Infineon EULA) is not
in this repository. `CMakeLists.txt` clones it into `<workspace>/modules/lib/capsense` on the
first build.

If you regenerate the files with the CapSense Configurator, keep the hand-written
`cycfg_peripherals.h`.
