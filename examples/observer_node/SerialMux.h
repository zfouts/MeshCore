#pragma once

#include <helpers/BaseSerialInterface.h>

// Fans the single companion-protocol endpoint out over two physical links: the
// primary (BLE or USB, always begin()'d at boot) and a secondary WiFi TCP
// console that only comes up once `set wifi_ssid` provisions credentials.
// Replies and push frames go to the link the app most recently sent a frame
// on, falling back to whichever link has a live connection. The companion
// session is stateful, so two apps talking at once will fight over it -- same
// as two TCP clients on the stock wifi build (last one in wins).
class SerialMux : public BaseSerialInterface {
  BaseSerialInterface* _pri;
  BaseSerialInterface* _sec;
  BaseSerialInterface* _active;

  // a disabled secondary counts as disconnected: its deviceConnected state
  // goes stale once we stop polling it (e.g. after `set wifi_ssid -`)
  bool secConnected() const { return _sec->isEnabled() && _sec->isConnected(); }

  BaseSerialInterface* route() const {
    BaseSerialInterface* act = (_active == _sec && !_sec->isEnabled()) ? _pri : _active;
    if (act->isConnected()) return act;
    return secConnected() ? _sec : _pri;
  }

public:
  SerialMux(BaseSerialInterface& pri, BaseSerialInterface& sec)
      : _pri(&pri), _sec(&sec), _active(&pri) {}

  // enable/disable is the `!ble` / ble_enabled toggle -- it must only gate the
  // primary link, so a node with BLE advertising off keeps its WiFi console
  void enable() override { _pri->enable(); }
  void disable() override { _pri->disable(); }
  bool isEnabled() const override { return _pri->isEnabled(); }

  bool isConnected() const override { return _pri->isConnected() || secConnected(); }

  bool isWriteBusy() const override { return route()->isWriteBusy(); }
  size_t writeFrame(const uint8_t src[], size_t len) override {
    return route()->writeFrame(src, len);
  }
  size_t checkRecvFrame(uint8_t dest[]) override {
    size_t len = _pri->checkRecvFrame(dest);
    if (len > 0) { _active = _pri; return len; }
    // the WiFi interface is only safe to poll once begin() has started its TCP
    // server; it stays disabled until credentials are provisioned
    if (_sec->isEnabled()) {
      len = _sec->checkRecvFrame(dest);
      if (len > 0) { _active = _sec; return len; }
    }
    return 0;
  }
};
