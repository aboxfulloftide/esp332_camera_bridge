# ESP32 Camera Bridge

This repository contains the current GardePro ESP32 bridge work:

- `gardepro_dual_radio_bridge/`
  - ESP32 dual-radio bridge sketch
- `gardepro_server_api.py`
  - Python client for the board HTTP bridge
- `gardepro_server_control.py`
  - CLI wrapper for live control and validation
- `gardepro_tunnel_server.py`
  - receiver for the framed tunnel stream
- `BRIDGE_USAGE.md`
  - current operator notes and validation workflow

## Local Config

The published tree does not include local network credentials.

Create these local files from the provided examples:

- `gardepro_dual_radio_bridge/local_config.h`
- `stream_test/local_config.h`

## Notes

- The trail-camera hotspot password remains in the repo because it is the vendor default for the device under test.
- Large local reverse-engineering artifacts and machine-specific files are ignored by git.
