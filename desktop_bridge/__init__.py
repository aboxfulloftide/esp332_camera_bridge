"""GardePro trail-camera bridge for a Linux desktop (replaces the ESP32 firmware).

Exposes the subset of the ESP32 board HTTP API (port 18080) that the server
uses for camera control, durable download/delete jobs and live view, and
registers itself with the server's GPRT tunnel listener so the server talks to
this machine instead of the ESP32. See DESKTOP_BRIDGE.md.
"""

VERSION = "desktop-0.1.0"
