/*
 * Wi-Fi station manager.
 *
 * Joins a network from the stored credentials ("wifi cred add ..." in the
 * shell; kept in flash by the Wi-Fi credentials library on top of Settings),
 * reconnects with a growing back-off when the link drops, and once DHCP has
 * given an address, sets the clock from SNTP. Every change is published on
 * net_chan.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WIFI_MGR_H_
#define WIFI_MGR_H_

/** Start managing the Wi-Fi interface (connects if credentials are stored). */
int wifi_mgr_start(void);

#endif /* WIFI_MGR_H_ */
