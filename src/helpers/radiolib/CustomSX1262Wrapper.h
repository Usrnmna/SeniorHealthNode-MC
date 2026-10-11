#pragma once

#include "CustomSX1262.h"
#include "RadioLibWrappers.h"
#include "SX126xReset.h"

#ifndef USE_SX1262
#define USE_SX1262
#endif

class CustomSX1262Wrapper : public RadioLibWrapper {
public:
  CustomSX1262Wrapper(CustomSX1262& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    setParamsChecked(freq, bw, sf, cr);
  }

  // Each hardware error is returned to the configuration transaction for rollback.
  bool setParamsChecked(float freq, float bw, uint8_t sf, uint8_t cr) {
    if (((CustomSX1262 *)_radio)->setFrequency(freq) != RADIOLIB_ERR_NONE ||
        ((CustomSX1262 *)_radio)->setSpreadingFactor(sf) != RADIOLIB_ERR_NONE ||
        ((CustomSX1262 *)_radio)->setBandwidth(bw) != RADIOLIB_ERR_NONE ||
        ((CustomSX1262 *)_radio)->setCodingRate(cr) != RADIOLIB_ERR_NONE) return false;
    if (_radio->setPreambleLength(preambleLengthForSF(sf)) != RADIOLIB_ERR_NONE) return false;
    _preamble_sf = sf;
    PacketMillis pm = calcMaxPacketMillis(sf, bw, cr, preambleLengthForSF(sf));
    ((CustomSX1262 *)_radio)->setPreambleMillis(pm.preambleMillis);
    ((CustomSX1262 *)_radio)->setMaxPayloadMillis(pm.payloadMillis);
    return true;
  }

  bool isReceivingPacket() override { 
    return ((CustomSX1262 *)_radio)->isReceiving();
  }
  float getCurrentRSSI() override {
    return ((CustomSX1262 *)_radio)->getRSSI(false);
  }
  float getLastRSSI() const override { return ((CustomSX1262 *)_radio)->getRSSI(); }
  float getLastSNR() const override { return ((CustomSX1262 *)_radio)->getSNR(); }

  float packetScore(float snr, int packet_len) override {
    int sf = ((CustomSX1262 *)_radio)->spreadingFactor;
    return packetScoreInt(snr, sf, packet_len);
  }
  uint8_t getSpreadingFactor() const override { return ((CustomSX1262 *)_radio)->spreadingFactor; }
  virtual void powerOff() override {
    ((CustomSX1262 *)_radio)->sleep(false);
  }

  void doResetAGC() override { sx126xResetAGC((SX126x *)_radio); }

  bool setRxBoostedGainMode(bool en) override {
    return ((CustomSX1262 *)_radio)->setRxBoostedGainMode(en) == RADIOLIB_ERR_NONE;
  }
  bool getRxBoostedGainMode() const override {
    return ((CustomSX1262 *)_radio)->getRxBoostedGainMode();
  }
};
