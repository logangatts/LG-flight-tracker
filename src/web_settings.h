// On-network settings page at http://flightradar.local (mDNS) / device IP.
#pragma once

namespace websrv {

// Call once after WiFi is up.
void start();

// Call frequently from the net task loop.
void service();

}  // namespace websrv
