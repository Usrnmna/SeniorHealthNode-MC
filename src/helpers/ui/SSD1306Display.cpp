#include "SSD1306Display.h"
#if ENV_INCLUDE_MPU6050
#include <HealthNodeConfig.h>
#endif

bool SSD1306Display::i2c_probe(TwoWire& wire, uint8_t addr) {
  wire.beginTransmission(addr);
  uint8_t error = wire.endTransmission();
  return (error == 0);
}

// Color scheme
ColorVal UIColor::window_bkg = SSD1306_BLACK;
ColorVal UIColor::title_bkg = SSD1306_BLACK;
ColorVal UIColor::title_txt = SSD1306_WHITE;
ColorVal UIColor::primary_txt = SSD1306_WHITE;
ColorVal UIColor::secondary_txt = SSD1306_WHITE;
ColorVal UIColor::warning_txt = SSD1306_WHITE;
ColorVal UIColor::popup_bkg = SSD1306_BLACK;
ColorVal UIColor::popup_txt = SSD1306_WHITE;
ColorVal UIColor::corp_blue = SSD1306_WHITE;

bool SSD1306Display::begin() {
  if (!_isOn) {
    if (_peripher_power) _peripher_power->claim();
    _isOn = true;
  }
  #ifdef DISPLAY_ROTATION
  display.setRotation(DISPLAY_ROTATION);
  #endif
  return display.begin(SSD1306_SWITCHCAPVCC, DISPLAY_ADDRESS, true, false) && i2c_probe(Wire, DISPLAY_ADDRESS);
}

void SSD1306Display::turnOn() {
  if (!_isOn) {
    if (_peripher_power) _peripher_power->claim();
    _isOn = true;  // set before begin() to prevent double claim
    if (_peripher_power) begin();  // re-init display after power was cut
  }
  display.ssd1306_command(SSD1306_DISPLAYON);
}

void SSD1306Display::turnOff() {
#if ENV_INCLUDE_MPU6050
  frame_transfer.cancel();
#endif
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  if (_isOn) {
    if (_peripher_power) {
#if PIN_OLED_RESET >= 0
      digitalWrite(PIN_OLED_RESET, LOW);
#endif
      _peripher_power->release();
    }
    _isOn = false;
  }
}

void SSD1306Display::clear() {
  display.clearDisplay();
  endFrame();
}

void SSD1306Display::startFrame(ColorVal bkg) {
  display.clearDisplay();  // TODO: apply 'bkg'
  _color = SSD1306_WHITE;
  display.setTextColor(_color);
  display.setTextSize(1);
  display.cp437(true);         // Use full 256 char 'Code Page 437' font
}

void SSD1306Display::setTextSize(int sz) {
  display.setTextSize(sz);
}

void SSD1306Display::setColor(ColorVal c) {
  _color = c;
  display.setTextColor(_color);
}

void SSD1306Display::setCursor(int x, int y) {
  display.setCursor(x, y);
}

void SSD1306Display::print(const char* str) {
  display.print(str);
}

void SSD1306Display::fillRect(int x, int y, int w, int h) {
  display.fillRect(x, y, w, h, _color);
}

void SSD1306Display::drawRect(int x, int y, int w, int h) {
  display.drawRect(x, y, w, h, _color);
}

void SSD1306Display::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  display.drawBitmap(x, y, bits, w, h, _color);
}

uint16_t SSD1306Display::getTextWidth(const char* str) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  return w;
}

void SSD1306Display::endFrame() {
#if ENV_INCLUDE_MPU6050
  frame_transfer.start(); // Framebuffer stays immutable until service() finishes.
#else
  display.display();
#endif
}

bool SSD1306Display::framePending() const {
#if ENV_INCLUDE_MPU6050
  return frame_transfer.pending();
#else
  return false;
#endif
}

void SSD1306Display::service() {
#if ENV_INCLUDE_MPU6050
  if (!framePending() || !_isOn || !display.getBuffer()) return;
  constexpr uint8_t count = OledFrameTransfer::chunk_bytes;
  const uint16_t frame_offset = frame_transfer.position();
  const uint8_t col = frame_offset % 128, page = frame_offset / 128;
  const uint32_t old_clock = Wire.getClock();
  const uint16_t old_timeout = Wire.getTimeOut();
  Wire.setClock(400000);
  Wire.setTimeOut(HealthNodeConfig::sensor_i2c_timeout_ms);
  // Explicit addressing makes each chunk independent of the preceding transfer.
  const uint8_t commands[] = {0x00, SSD1306_COLUMNADDR, col, uint8_t(col + count - 1),
                            SSD1306_PAGEADDR, page, page};
  Wire.beginTransmission(DISPLAY_ADDRESS);
  Wire.write(commands, sizeof(commands));
  bool ok = Wire.endTransmission() == 0;
  if (ok) {
    Wire.beginTransmission(DISPLAY_ADDRESS);
    Wire.write(0x40);
    Wire.write(display.getBuffer() + frame_offset, count);
    ok = Wire.endTransmission() == 0;
  }
  Wire.setClock(old_clock);
  Wire.setTimeOut(old_timeout);
  frame_transfer.completed(ok); // Retry a new frame next refresh, never spin.
#endif
}
