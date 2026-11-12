/*
 * CapSense hardware assignments for the CY8CPROTO-062-4343W.
 *
 * Replaces the ModusToolbox BSP's generated cycfg_peripherals.h (BSP
 * release-v3.1.0), keeping only what the generated cycfg_capsense.c needs.
 * Pins/clock values are copied from that file; the clock divider index was
 * changed (see below).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CYCFG_PERIPHERALS_H
#define CYCFG_PERIPHERALS_H

#include "cy_sysclk.h"
#include "cy_csd.h"

#define CYBSP_CSD_ENABLED     1U
#define CY_CAPSENSE_CORE      4u          /* runs on the Cortex-M4 */
#define CY_CAPSENSE_CPU_CLK   100000000u  /* must match the Zephyr board clocks */
#define CY_CAPSENSE_PERI_CLK  100000000u
#define CY_CAPSENSE_VDDA_MV   3300u

/*
 * Peripheral clock divider for the CSD block. The BSP used 8-bit divider 0,
 * but under Zephyr the HAL-based drivers (UART, timer, ADC) grab dividers
 * dynamically starting from the lowest free one. We use the last 8-bit
 * divider and reserve it at boot (src/capsense_input.c) so nobody else gets it.
 */
#define CY_CAPSENSE_PERI_DIV_TYPE   CY_SYSCLK_DIV_8_BIT
#define CY_CAPSENSE_PERI_DIV_INDEX  7u

/* Integration capacitors */
#define Cmod_PORT      GPIO_PRT7
#define Cmod_PIN       7u
#define Cmod_PORT_NUM  7u
#define CintA_PORT     GPIO_PRT7
#define CintA_PIN      1u
#define CintA_PORT_NUM 7u
#define CintB_PORT     GPIO_PRT7
#define CintB_PIN      2u
#define CintB_PORT_NUM 7u

/* CSX buttons: separate receive pins, shared transmit pin P1.0 */
#define Button0_Rx0_PORT GPIO_PRT8
#define Button0_Rx0_PIN  1u
#define Button0_Tx_PORT  GPIO_PRT1
#define Button0_Tx_PIN   0u
#define Button1_Rx0_PORT GPIO_PRT8
#define Button1_Rx0_PIN  2u
#define Button1_Tx_PORT  GPIO_PRT1
#define Button1_Tx_PIN   0u

/* CSD linear slider, segments P8.3 .. P8.7 */
#define LinearSlider0_Sns0_PORT GPIO_PRT8
#define LinearSlider0_Sns0_PIN  3u
#define LinearSlider0_Sns1_PORT GPIO_PRT8
#define LinearSlider0_Sns1_PIN  4u
#define LinearSlider0_Sns2_PORT GPIO_PRT8
#define LinearSlider0_Sns2_PIN  5u
#define LinearSlider0_Sns3_PORT GPIO_PRT8
#define LinearSlider0_Sns3_PIN  6u
#define LinearSlider0_Sns4_PORT GPIO_PRT8
#define LinearSlider0_Sns4_PIN  7u

#define CYBSP_CSD_HW  CSD0
#define CYBSP_CSD_IRQ csd_interrupt_IRQn

/* CSD driver context, shared by CapSense and any other CSD user. */
extern cy_stc_csd_context_t cy_csd_0_context;

#endif /* CYCFG_PERIPHERALS_H */
