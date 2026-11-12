/*
 * Web dashboard server (HTTP, WebSocket, mDNS).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WEB_SERVER_H_
#define WEB_SERVER_H_

/** Start the HTTP server and the thread that streams to WebSocket clients. */
int web_server_start(void);

#endif /* WEB_SERVER_H_ */
