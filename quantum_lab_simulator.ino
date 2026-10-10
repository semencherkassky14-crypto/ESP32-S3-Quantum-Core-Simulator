/*
 * DESKTOP QUANTUM LABORATORY SIMULATOR  v2
 * Target : ESP32-S3 (Arduino-ESP32), LovyanGFX, ST7789 240x320 used as 320x240
 *
 * Pins : MOSI=13 SCLK=12 CS=10 DC=11 RST=9 | BTN=GPIO1 (INPUT_PULLUP)
 *        ENTROPY = GPIO4 (ADC1_CH3), leave physically unconnected
 *
 * Controls
 *   MENU       short = cursor down            long (>=600 ms) = enter experiment
 *   EXPERIMENT short = measurement / collapse long (>=600 ms) = back to menu
 *
 * Experiments
 *   1. 1-Qubit Hadamard : Bloch sphere + wave function, violent collapse
 *   2. EPR Pair         : 2-qubit Bell state, simultaneous collapse (|00> or |11>)
 *   3. Q-Tunneling      : real time-dependent Schroedinger solver (leapfrog)
 *   4. Quantum Eraser   : delayed-choice double slit, which-way marker toggle
 *   5. Q-Chaos CPU      : 4-qubit state-vector processor with thermal noise
 */

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <esp_system.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ───────────────────────── Enums (classic, top of file) ─────────────────────────
enum AppState { STATE_MENU, STATE_RUN, STATE_COLLAPSE };
enum ModeId   { MODE_HADAMARD = 0, MODE_EPR, MODE_TUNNEL, MODE_ERASER, MODE_CHAOS, MODE_COUNT };
enum BtnEvent { BTN_NONE, BTN_SHORT, BTN_LONG };
enum GateType { G_H = 0, G_X, G_Z, G_T, G_PH, G_CNOT };
enum PanelKind { PK_BINARY, PK_HEX };

// ───────────────────────── Constants ─────────────────────────
static const int PIN_MOSI    = 13;
static const int PIN_SCLK    = 12;
static const int PIN_CS      = 10;
static const int PIN_DC      = 11;
static const int PIN_RST     = 9;
static const int PIN_BTN     = 1;
static const int PIN_ENTROPY = 4;     // ADC1_CH3 (GPIO0 has no ADC on ESP32-S3)

static const int SCR_W   = 320;
static const int SCR_H   = 240;
static const int LEFT_W  = 210;
static const int RIGHT_W = 110;

static const uint16_t BLACK = TFT_BLACK;
static const uint16_t WHITE = TFT_WHITE;

static const uint32_t DEBOUNCE_MS    = 25;
static const uint32_t LONG_PRESS_MS  = 600;
static const uint32_t FRAME_MS       = 33;
static const uint32_t PANEL_LIVE_MS  = 250;

static const int HIST_LEN = 10;

// ───────────────────────── Plain data types ─────────────────────────
struct Cx    { float re, im; };
struct Qubit { Cx a, b; };
struct Vec3  { float x, y, z; };

struct ModeStats {
  uint32_t total;
  uint32_t c0;
  uint32_t c1;
  uint8_t  hist[HIST_LEN];
  uint8_t  last;
  bool     have;
};

struct Photon {
  float x, y, vx, vy, slitY, targetY;
  uint8_t stage;
  bool alive;
};

struct GateRec { uint8_t type, a, b; };

// ───────────────────────── Display driver ─────────────────────────
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
public:
  LGFX() {
    {
      auto c = _bus.config();
      c.spi_host    = SPI2_HOST;
      c.spi_mode    = 0;
      c.freq_write  = 40000000;
      c.freq_read   = 16000000;
      c.spi_3wire   = true;
      c.use_lock    = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk    = PIN_SCLK;
      c.pin_mosi    = PIN_MOSI;
      c.pin_miso    = -1;
      c.pin_dc      = PIN_DC;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs           = PIN_CS;
      c.pin_rst          = PIN_RST;
      c.pin_busy         = -1;
      c.panel_width      = 240;
      c.panel_height     = 320;
      c.offset_x         = 0;
      c.offset_y         = 0;
      c.offset_rotation  = 0;
      c.dummy_read_pixel = 8;
      c.dummy_read_bits  = 1;
      c.readable         = false;
      c.invert           = true;
      c.rgb_order        = false;
      c.dlen_16bit       = false;
      c.bus_shared       = false;
      _panel.config(c);
    }
    setPanel(&_panel);
  }
};

static LGFX lcd;
static lgfx::LGFX_Sprite sprLeft(&lcd);    // 210 x 240 animation zone
static lgfx::LGFX_Sprite sprRight(&lcd);   // 110 x 240 data panel
static lgfx::LGFX_Sprite sprHits(&lcd);    // 46 x 160 persistent detector screen (eraser)

// ───────────────────────── Button state machine ─────────────────────────
class Button {
  uint8_t  pin;
  bool     rawLast    = false;
  bool     stable     = false;
  bool     longFired  = false;
  uint32_t lastChange = 0;
  uint32_t pressedAt  = 0;
public:
  explicit Button(uint8_t p) : pin(p) {}
  void begin() { pinMode(pin, INPUT_PULLUP); }

  BtnEvent update(uint32_t now) {
    bool raw = (digitalRead(pin) == LOW);
    if (raw != rawLast) { rawLast = raw; lastChange = now; }

    if ((now - lastChange) >= DEBOUNCE_MS && raw != stable) {
      stable = raw;
      if (stable) {
        pressedAt = now;
        longFired = false;
      } else if (!longFired) {
        return BTN_SHORT;
      }
    }
    if (stable && !longFired && (now - pressedAt) >= LONG_PRESS_MS) {
      longFired = true;
      return BTN_LONG;
    }
    return BTN_NONE;
  }
};

static Button button(PIN_BTN);

// ───────────────────────── Random numbers ─────────────────────────
static uint64_t rngState = 0x9E3779B97F4A7C15ULL;
static uint32_t entropyFallbacks = 0;
static uint32_t visSeed = 2463534242UL;

static inline uint64_t splitmix64(uint64_t& x) {
  x += 0x9E3779B97F4A7C15ULL;
  uint64_t z = x;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static inline uint8_t rawNoiseBit() {
  uint16_t a = analogRead(PIN_ENTROPY);
  a ^= (a >> 2);
  a ^= (a >> 1);
  return a & 1;
}

static uint8_t whitenedBit() {
  for (int i = 0; i < 128; i++) {
    uint8_t b1 = rawNoiseBit();
    delayMicroseconds(3);
    uint8_t b2 = rawNoiseBit();
    if (b1 != b2) return b1;
  }
  entropyFallbacks++;
  return esp_random() & 1;
}

// Physical entropy (floating ADC pin, von Neumann whitened) -> RNG seed.
static void reseedFromNoise() {
  uint64_t seed = 0;
  for (int i = 0; i < 64; i++) seed = (seed << 1) | whitenedBit();
  rngState ^= seed;
  rngState ^= (uint64_t)micros() << 20;
  (void)splitmix64(rngState);
}

// Physical random float in [0,1): one fresh analog sample folded in per call.
static float physRand01() {
  rngState += (uint64_t)analogRead(PIN_ENTROPY) * 0x100000001B3ULL;
  return (float)(uint32_t)(splitmix64(rngState) >> 40) / 16777216.0f;
}

// Cheap visual-only randomness (never used for measurement outcomes).
static inline uint32_t visRand32() {
  visSeed ^= visSeed << 13;
  visSeed ^= visSeed >> 17;
  visSeed ^= visSeed << 5;
  return visSeed;
}
static inline float frand()       { return (float)(visRand32() & 0xFFFFFF) / 16777216.0f; }
static inline float frandSigned() { return frand() * 2.0f - 1.0f; }

// ───────────────────────── Small math helpers ─────────────────────────
static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static inline float smoothstep01(float t) { t = clamp01(t); return t * t * (3.0f - 2.0f * t); }

static inline Cx mkc(float r, float i) { Cx c; c.re = r; c.im = i; return c; }
static inline Cx cadd(Cx a, Cx b) { return mkc(a.re + b.re, a.im + b.im); }
static inline Cx cmul(Cx a, Cx b) { return mkc(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re); }
static inline float norm2(Cx c) { return c.re * c.re + c.im * c.im; }

// Generic single-qubit gate on an n-qubit state vector
static void applyGate1(Cx* psi, int nq, int q, Cx m00, Cx m01, Cx m10, Cx m11) {
  const int dim = 1 << nq, bit = 1 << q;
  for (int i = 0; i < dim; i++) {
    if (i & bit) continue;
    int j = i | bit;
    Cx a = psi[i], b = psi[j];
    psi[i] = cadd(cmul(m00, a), cmul(m01, b));
    psi[j] = cadd(cmul(m10, a), cmul(m11, b));
  }
}
static void gateH(Cx* psi, int nq, int q) {
  const float s = 0.70710678118f;
  applyGate1(psi, nq, q, mkc(s, 0), mkc(s, 0), mkc(s, 0), mkc(-s, 0));
}
static void gateX(Cx* psi, int nq, int q) {
  applyGate1(psi, nq, q, mkc(0, 0), mkc(1, 0), mkc(1, 0), mkc(0, 0));
}
static void gatePhase(Cx* psi, int nq, int q, float th) {
  applyGate1(psi, nq, q, mkc(1, 0), mkc(0, 0), mkc(0, 0), mkc(cosf(th), sinf(th)));
}
static void gateRx(Cx* psi, int nq, int q, float th) {
  float c = cosf(th * 0.5f), s = sinf(th * 0.5f);
  applyGate1(psi, nq, q, mkc(c, 0), mkc(0, -s), mkc(0, -s), mkc(c, 0));
}
static void gateCNOT(Cx* psi, int nq, int ctl, int tgt) {
  const int dim = 1 << nq, cb = 1 << ctl, tb = 1 << tgt;
  for (int i = 0; i < dim; i++) {
    if ((i & cb) && !(i & tb)) { Cx t = psi[i]; psi[i] = psi[i | tb]; psi[i | tb] = t; }
  }
}
static void stateInitUniform(Cx* psi, int nq) {
  const int dim = 1 << nq;
  for (int i = 0; i < dim; i++) psi[i] = mkc(0, 0);
  psi[0] = mkc(1, 0);
  for (int q = 0; q < nq; q++) gateH(psi, nq, q);
}
// Born-rule sampling with the physically seeded generator
static int sampleIndex(const Cx* psi, int dim) {
  float total = 0;
  for (int i = 0; i < dim; i++) total += norm2(psi[i]);
  float r = physRand01() * total, acc = 0;
  for (int i = 0; i < dim; i++) { acc += norm2(psi[i]); if (r < acc) return i; }
  return dim - 1;
}

// 1-qubit helpers
static void applyHadamard(Qubit& q) {
  const float s = 0.70710678118f;
  Cx a = q.a, b = q.b;
  q.a = mkc((a.re + b.re) * s, (a.im + b.im) * s);
  q.b = mkc((a.re - b.re) * s, (a.im - b.im) * s);
}
static Qubit applyPhase(Qubit q, float phi) {
  q.b = cmul(q.b, mkc(cosf(phi), sinf(phi)));
  return q;
}
static Vec3 blochVector(const Qubit& q) {
  Vec3 v;
  v.x = 2.0f * (q.a.re * q.b.re + q.a.im * q.b.im);
  v.y = 2.0f * (q.a.re * q.b.im - q.a.im * q.b.re);
  v.z = norm2(q.a) - norm2(q.b);
  return v;
}

// ───────────────────────── Global application state ─────────────────────────
static AppState state     = STATE_MENU;
static ModeId   curMode   = MODE_HADAMARD;
static int      menuIndex = 0;

static uint32_t collapseStart  = 0;
static uint32_t collapseDur    = 650;
static uint32_t frozenTime     = 0;
static uint8_t  collapseResult = 0;

static ModeStats stats[MODE_COUNT];
static bool      panelDirty = true;
static uint32_t  lastFrame = 0, lastPanelLive = 0;

static const char* MENU_ITEMS[MODE_COUNT] = {
  "1. Hadamard Gate", "2. EPR Entangle", "3. Q-Tunneling", "4. Quantum Eraser", "5. Q-Chaos CPU"
};

// Mode 1 (Hadamard)
static Qubit psiPlus;
static float hadXm = 100.0f;

// Mode 2 (EPR)
static Cx       bell[4];
static uint32_t eprMismatch = 0;

// Mode 3 (Tunneling)
static const int T_BX0 = 120, T_BX1 = 124;
static float    tR[LEFT_W], tI[LEFT_W], tV[LEFT_W], tDamp[LEFT_W];
static uint32_t tSteps = 0;
static float    tNorm0 = 1.0f, tT = 0.0f, tSideArea = 1.0f, tXm = 100.0f;
static bool     tValid = false;
static uint8_t  tSide  = 0;

// Mode 4 (Quantum eraser)
static const int E_NB = 80;
static const int E_PLANE_X = 120;
static const int E_SCREEN_X = 158;
static float    eTag[E_NB], eInt[E_NB], eCdf[E_NB];
static Photon   phot[28];
static float    eV = 0.0f, eSpawn = 0.0f;
static uint8_t  eTarget = 0;
static uint32_t eLastToggle = 0, eHits = 0, eToggles = 0, eWipe = 0;
static uint32_t detFlashUntil[2] = {0, 0};

// Mode 5 (Chaos CPU)
static Cx       qI[16], qN[16];
static GateRec  gLog[10];
static int      gCount = 0;
static uint32_t chaosLast = 0, chaosReset = 0, chaosGates = 0, chaosRecal = 0, chaosRecalAt = 0;
static float    chaosTemp = 18.0f, chaosSigma = 0.1f, chaosF = 1.0f, chaosT2 = 0.0f;
static const uint32_t CHAOS_GATE_MS = 280;
static char     basisLbl[16][5];

// ───────────────────────── Drawing helpers ─────────────────────────
static void txt(lgfx::LGFX_Sprite& s, const lgfx::IFont* f, float size, lgfx::textdatum_t d, uint16_t col) {
  s.setFont(f);
  s.setTextSize(size);
  s.setTextDatum(d);
  s.setTextColor(col);
}

static void drawHeader(lgfx::LGFX_Sprite& s, const char* title) {
  txt(s, &fonts::Font2, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  s.drawString(title, 6, 3);
  s.drawFastHLine(0, 22, LEFT_W, WHITE);
}

static void drawFooter(lgfx::LGFX_Sprite& s, const char* hint) {
  txt(s, &fonts::Font0, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  s.drawString(hint, 6, 229);
}

static void drawThickLine(lgfx::LGFX_Sprite& s, int x0, int y0, int x1, int y1, uint16_t c) {
  s.drawLine(x0, y0, x1, y1, c);
  s.drawLine(x0 + 1, y0, x1 + 1, y1, c);
  s.drawLine(x0, y0 + 1, x1, y1 + 1, c);
}

static void drawArrow(lgfx::LGFX_Sprite& s, int x0, int y0, int x1, int y1, uint16_t c) {
  float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 3.0f) { s.fillCircle(x1, y1, 3, c); return; }
  dx /= len; dy /= len;
  float head = (len > 20.0f) ? 11.0f : len * 0.5f;
  float bx = x1 - dx * head, by = y1 - dy * head;
  float px = -dy * head * 0.45f, py = dx * head * 0.45f;
  drawThickLine(s, x0, y0, (int)lroundf(bx), (int)lroundf(by), c);
  s.fillTriangle(x1, y1, (int)lroundf(bx + px), (int)lroundf(by + py),
                 (int)lroundf(bx - px), (int)lroundf(by - py), c);
}

static void dottedH(lgfx::LGFX_Sprite& s, int x0, int x1, int y, int step, uint16_t c) {
  for (int x = x0; x <= x1; x += step) s.drawPixel(x, y, c);
}
static void dottedV(lgfx::LGFX_Sprite& s, int x, int y0, int y1, int step, uint16_t c) {
  for (int y = y0; y <= y1; y += step) s.drawPixel(x, y, c);
}

// ───────────────────────── Stats ─────────────────────────
static void commitResult(ModeId m, uint8_t r) {
  ModeStats& st = stats[m];
  memmove(&st.hist[1], &st.hist[0], HIST_LEN - 1);
  st.hist[0] = r;
  st.total++;
  if (m != MODE_CHAOS) { if (r) st.c1++; else st.c0++; }
  st.last = r;
  st.have = true;
  panelDirty = true;
}

// ═════════════════════════════════════════════════════════════════════════
//  MODE 1 : HADAMARD (Bloch sphere + wave function)
// ═════════════════════════════════════════════════════════════════════════
static void initHadamard() {
  Qubit q;
  q.a = mkc(1, 0);
  q.b = mkc(0, 0);
  applyHadamard(q);          // |0> --H--> |+>
  psiPlus = q;
}

static void renderHadamard(uint32_t t, float ct) {
  lgfx::LGFX_Sprite& s = sprLeft;
  s.fillScreen(BLACK);
  const bool col   = (ct >= 0.0f);
  const bool flash = col && ct < 0.07f;
  const uint16_t fg = flash ? BLACK : WHITE;
  if (flash) s.fillRect(0, 24, LEFT_W, 118, WHITE);
  drawHeader(s, "1-QUBIT HADAMARD");

  float shake = (col && ct < 0.5f) ? (1.0f - ct * 2.0f) * 3.5f : 0.0f;
  int ox = (int)lroundf(frandSigned() * shake);
  int oy = (int)lroundf(frandSigned() * shake);

  const int cx = 105 + ox, cy = 90 + oy, R = 46;
  const float tc = cosf(0.35f), ts = sinf(0.35f);

  s.drawCircle(cx, cy, R, fg);
  s.drawEllipse(cx, cy, R, (int)lroundf(R * ts), fg);
  dottedV(s, cx, (int)(cy - R * tc), (int)(cy + R * tc), 4, fg);
  txt(s, &fonts::Font2, 1.0f, lgfx::textdatum_t::top_left, fg);
  s.drawString("|0>", cx + 8, cy - R - 2);
  s.drawString("|1>", cx + 8, cy + R - 14);

  Vec3 v = blochVector(applyPhase(psiPlus, (float)t * 0.0025f));
  if (col) {
    float u = 0.0f;
    if (ct > 0.2f) {
      float sN = clamp01((ct - 0.2f) / 0.8f);
      u = 1.0f - expf(-6.0f * sN) * cosf(10.0f * sN);     // damped snap with overshoot
    }
    float jit = (ct < 0.45f) ? (1.0f - ct / 0.45f) : 0.0f;
    float pz  = collapseResult ? -1.0f : 1.0f;
    v.x = v.x * (1.0f - u) + frandSigned() * 0.4f * jit;
    v.y = v.y * (1.0f - u) + frandSigned() * 0.4f * jit;
    v.z = v.z * (1.0f - u) + pz * u + frandSigned() * 0.4f * jit;
  }
  int tx = (int)lroundf(cx + R * v.x);
  int ty = (int)lroundf(cy + R * (-v.z * tc + v.y * ts));
  drawArrow(s, cx, cy, tx, ty, fg);
  s.fillCircle(cx, cy, 2, fg);

  if (col && ct < 0.3f) {                                   // starburst at the tip
    float k = 1.0f - ct / 0.3f;
    for (int i = 0; i < 16; i++) {
      float a = i * 0.3927f + ct * 3.0f;
      float r1 = 5.0f + 38.0f * k * (0.4f + 0.6f * frand());
      s.drawLine(tx + (int)(5 * cosf(a)), ty + (int)(5 * sinf(a)),
                 tx + (int)(r1 * cosf(a)), ty + (int)(r1 * sinf(a)), fg);
    }
  }
  if (col && ct > 0.04f && ct < 0.95f) {                    // shock rings
    s.drawCircle(cx, cy, (int)(ct * 95.0f), fg);
    s.drawCircle(cx, cy, (int)(ct * 60.0f), fg);
  }

  // wave function strip
  const int cyw = 184;
  const float A0 = 30.0f;
  float amp = A0, noiseAmp = 0.0f, spikeH = 0.0f, spikeW = 18.0f;
  if (col) {
    float u1 = smoothstep01(clamp01((ct - 0.15f) / 0.85f));
    amp      = A0 * (1.0f - u1);
    noiseAmp = (ct < 0.5f) ? 14.0f * sinf(3.14159265f * ct / 0.5f) : 0.0f;
    spikeH   = 40.0f * u1;
    spikeW   = 18.0f * (1.0f - u1) + 2.5f;
  }
  dottedH(s, 0, LEFT_W - 1, 142, 6, WHITE);
  const float k = 6.2831853f / 64.0f, ph = (float)t * 0.0045f;
  int prevY = cyw;
  for (int x = 0; x < LEFT_W; x++) {
    float y = cyw - amp * sinf(k * x - ph);
    if (noiseAmp > 0.0f) y += noiseAmp * frandSigned();
    float dxm = ((float)x - hadXm) / spikeW;
    y -= spikeH * expf(-dxm * dxm);
    int yi = (int)lroundf(y);
    if (yi < 146) yi = 146;
    if (yi > 221) yi = 221;
    if (x > 0) {
      s.drawLine(x - 1, prevY, x, yi, WHITE);
      s.drawLine(x - 1, prevY + 1, x, yi + 1, WHITE);
    }
    prevY = yi;
  }
  txt(s, &fonts::Font0, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  s.drawString("PSI(x,t)", 4, 146);
  drawFooter(s, "CLICK:MEASURE HOLD:MENU");
}

// ═════════════════════════════════════════════════════════════════════════
//  MODE 2 : EPR PAIR (Bell state)
// ═════════════════════════════════════════════════════════════════════════
static void initEPR() {
  for (int i = 0; i < 4; i++) bell[i] = mkc(0, 0);
  bell[0] = mkc(1, 0);
  gateH(bell, 2, 0);
  gateCNOT(bell, 2, 0, 1);       // (|00> + |11>)/sqrt2
}

static void renderEPR(uint32_t t, float ct) {
  lgfx::LGFX_Sprite& s = sprLeft;
  s.fillScreen(BLACK);
  drawHeader(s, "EPR PAIR |PHI+>");
  const bool col = (ct >= 0.0f);
  const int ay = 96, ax = 40, bx = 170;
  const float ph = (float)t * 0.006f;
  const float u  = col ? smoothstep01(clamp01((ct - 0.15f) / 0.85f)) : 0.0f;
  const float fs = 1.0f - u;

  // two sub-atomic fields, vibrating in perfect harmony
  for (int k = 0; k < 4; k++) {
    float r = (fmodf((float)t * 0.018f + k * 8.5f, 34.0f) + 1.5f * sinf(ph * 3.0f + k)) * fs;
    int ri = (int)r;
    if (ri >= 1) { s.drawCircle(ax, ay, ri, WHITE); s.drawCircle(bx, ay, ri, WHITE); }
  }
  int nr = 3 + (int)(1.2f * sinf(ph * 2.0f));
  s.fillCircle(ax, ay, nr, WHITE);
  s.fillCircle(bx, ay, nr, WHITE);
  int oa = (int)(20.0f * fs);
  if (oa > 3) {
    int ob = oa * 2 / 5;
    s.drawEllipse(ax, ay, oa, ob, WHITE);
    s.drawEllipse(bx, ay, oa, ob, WHITE);
    float ea = ph * 1.5f;
    for (int e = 0; e < 2; e++) {
      float a = ea + e * 3.14159265f;
      s.fillCircle(ax + (int)(oa * cosf(a)), ay + (int)(ob * sinf(a)), 2, WHITE);
      s.fillCircle(bx + (int)(oa * cosf(a)), ay + (int)(ob * sinf(a)), 2, WHITE);
    }
  }

  // entanglement link: double helix with correlation rungs
  const int hx0 = 76, hx1 = 134;
  const float rD = u * 30.0f;
  int px = -1, py1 = 0, py2 = 0;
  for (int x = hx0; x <= hx1; x++) {
    if (col && fabsf((float)x - 105.0f) < rD) { px = -1; continue; }
    float ux  = (float)(x - hx0) / (float)(hx1 - hx0);
    float env = sinf(3.14159265f * ux) * 16.0f;
    float a   = ux * 12.566371f - ph * 2.0f;
    int y1 = (int)lroundf(ay + env * sinf(a));
    int y2 = (int)lroundf(ay - env * sinf(a));
    if (px >= 0) { s.drawLine(px, py1, x, y1, WHITE); s.drawLine(px, py2, x, y2, WHITE); }
    if (((x - hx0) % 5) == 0) s.drawLine(x, y1, x, y2, WHITE);
    px = x; py1 = y1; py2 = y2;
  }

  if (col && ct < 0.25f) {                                  // non-local lightning bolt
    float k = 1.0f - ct / 0.25f;
    int lx = 54, ly = ay;
    for (int x = 60; x <= 156; x += 6) {
      int ny = ay + (int)(frandSigned() * 8.0f * k);
      drawThickLine(s, lx, ly, x, ny, WHITE);
      lx = x; ly = ny;
    }
  }
  if (col && ct < 0.1f) {                                   // simultaneous flash
    s.fillCircle(ax, ay, 24, WHITE);
    s.fillCircle(bx, ay, 24, WHITE);
  }
  if (col && ct > 0.45f) {                                  // matched outcome glyphs
    int gr = (int)(14.0f * clamp01((ct - 0.45f) / 0.3f));
    if (gr >= 3) {
      if (collapseResult) { s.fillCircle(ax, ay, gr, WHITE); s.fillCircle(bx, ay, gr, WHITE); }
      else                { s.drawCircle(ax, ay, gr, WHITE); s.drawCircle(bx, ay, gr, WHITE); }
      if (gr >= 12) {
        txt(s, &fonts::Font4, 1.0f, lgfx::textdatum_t::middle_center, collapseResult ? BLACK : WHITE);
        s.drawString(collapseResult ? "1" : "0", ax, ay);
        s.drawString(collapseResult ? "1" : "0", bx, ay);
      }
    }
  }

  txt(s, &fonts::Font2, 1.0f, lgfx::textdatum_t::middle_center, WHITE);
  s.drawString("A", ax, 140);
  s.drawString("B", bx, 140);
  if (col) {
    txt(s, &fonts::Font0, 1.0f, lgfx::textdatum_t::middle_center, WHITE);
    s.drawString("dt = 0", 105, 128);
  }

  // scrolling traces of A and B (identical -> perfect correlation)
  for (int ch = 0; ch < 2; ch++) {
    const int cyT = ch ? 196 : 168;
    int prev = cyT;
    for (int x = 0; x < LEFT_W; x++) {
      float ph2 = (float)x * 0.13f - (float)t * 0.009f;
      float v = (sinf(ph2) + 0.3f * sinf(2.0f * ph2 + 0.7f)) * 9.0f * fs
                + (collapseResult ? -6.0f : 6.0f) * u;
      int y = (int)lroundf(cyT - v);
      if (x > 0 && (ch == 0 || (x & 2))) s.drawLine(x - 1, prev, x, y, WHITE);
      prev = y;
    }
  }
  txt(s, &fonts::Font0, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  s.drawString("corr(A,B)=+1.00", 6, 212);
  drawFooter(s, "CLICK:MEASURE HOLD:MENU");
}

// ═════════════════════════════════════════════════════════════════════════
//  MODE 3 : QUANTUM TUNNELING (1-D time dependent Schroedinger, hbar=m=1)
// ═════════════════════════════════════════════════════════════════════════
static void initTunnel() {
  const float x0 = 58.0f, sg = 14.0f, k0 = 0.5f, V0 = 0.22f;
  const int zone = 20;
  tNorm0 = 0.0f;
  for (int i = 0; i < LEFT_W; i++) {
    float x = (float)i;
    float e = expf(-(x - x0) * (x - x0) / (2.0f * sg * sg));
    tR[i] = e * cosf(k0 * x);
    tI[i] = e * sinf(k0 * x);
    tV[i] = (i >= T_BX0 && i < T_BX1) ? V0 : 0.0f;
    tNorm0 += e * e;
    tDamp[i] = 1.0f;
    if (i < zone) { float f = (float)(zone - i) / zone; tDamp[i] = 1.0f - 0.04f * f * f; }
    if (i >= LEFT_W - zone) { float f = (float)(i - (LEFT_W - zone) + 1) / zone; tDamp[i] = 1.0f - 0.04f * f * f; }
  }
  tSteps = 0;
  tValid = false;
  tT = 0.0f;
}

static void stepTunnel() {
  const float dt = 0.25f;
  for (int i = 1; i < LEFT_W - 1; i++)
    tR[i] += dt * (-0.5f * (tI[i + 1] - 2.0f * tI[i] + tI[i - 1]) + tV[i] * tI[i]);
  for (int i = 1; i < LEFT_W - 1; i++)
    tI[i] += dt * (0.5f * (tR[i + 1] - 2.0f * tR[i] + tR[i - 1]) - tV[i] * tR[i]);
  for (int i = 0; i < 20; i++) { tR[i] *= tDamp[i]; tI[i] *= tDamp[i]; }
  for (int i = LEFT_W - 20; i < LEFT_W; i++) { tR[i] *= tDamp[i]; tI[i] *= tDamp[i]; }
}

static void updateTunnel() {
  for (int k = 0; k < 12; k++) stepTunnel();
  tSteps += 12;
  float pL = 0.0f, pR = 0.0f;
  for (int i = 20; i < T_BX0; i++)       pL += tR[i] * tR[i] + tI[i] * tI[i];
  for (int i = T_BX1; i < LEFT_W - 20; i++) pR += tR[i] * tR[i] + tI[i] * tI[i];
  if (tSteps > 450 && (pL + pR) > 0.5f * tNorm0) { tT = pR / (pL + pR); tValid = true; }
  if (tSteps > 2200) initTunnel();
}

static void prepareTunnelCollapse() {
  float aL = 0.0f, aR = 0.0f;
  for (int i = 0; i < LEFT_W; i++) {
    float d = tR[i] * tR[i] + tI[i] * tI[i];
    if (i < T_BX0) aL += d; else if (i >= T_BX1) aR += d;
  }
  float tot = aL + aR;
  if (tot < 1e-6f) { aL = 1.0f; aR = 0.0f; tot = 1.0f; }
  tSide = (physRand01() < aR / tot) ? 1 : 0;
  float target = physRand01() * (tSide ? aR : aL), acc = 0.0f;
  tXm = tSide ? 150.0f : 60.0f;
  for (int i = 0; i < LEFT_W; i++) {
    bool inSide = tSide ? (i >= T_BX1) : (i < T_BX0);
    if (!inSide) continue;
    acc += tR[i] * tR[i] + tI[i] * tI[i];
    if (acc >= target) { tXm = (float)i; break; }
  }
  tSideArea = tSide ? aR : aL;
  collapseResult = tSide;
}

static void renderTunnel(uint32_t t, float ct) {
  (void)t;
  lgfx::LGFX_Sprite& s = sprLeft;
  s.fillScreen(BLACK);
  drawHeader(s, "QUANTUM TUNNELING");
  const bool col = (ct >= 0.0f);
  const float u = col ? smoothstep01(clamp01((ct - 0.1f) / 0.7f)) : 0.0f;
  const int base = 204, yR = 76;
  const float dScale = 78.0f, rScale = 34.0f;
  const float spikeH = tSideArea / (2.5f * 1.7725f);

  s.drawFastHLine(0, base + 1, LEFT_W, WHITE);
  dottedH(s, 0, LEFT_W - 1, yR, 5, WHITE);

  int prevD = base, prevR = yR;
  for (int i = 0; i < LEFT_W; i++) {
    float mask = 1.0f;
    if (col) {
      bool sideR = (i >= T_BX1);
      bool inWall = (i >= T_BX0 && i < T_BX1);
      mask = inWall ? (1.0f - u) : ((sideR == (tSide == 1)) ? 1.0f : (1.0f - u));
    }
    float d  = tR[i] * tR[i] + tI[i] * tI[i];
    float dd = d * mask * (1.0f - u);
    if (col) {
      float g = ((float)i - tXm) / 2.5f;
      dd += u * spikeH * expf(-g * g);
    }
    int h = (int)(dd * dScale);
    if (h > 100) h = 100;
    int yd = base - h;
    int yr = (int)lroundf(yR - tR[i] * mask * (1.0f - u) * rScale);
    if (yr < 30) yr = 30;
    if (yr > 120) yr = 120;
    if (i > 0) {
      s.drawLine(i - 1, prevD, i, yd, WHITE);
      s.drawLine(i - 1, prevR, i, yr, WHITE);
    }
    if ((i % 3) == 0 && h > 0) s.drawFastVLine(i, yd, h, WHITE);
    prevD = yd; prevR = yr;
  }

  s.fillRect(T_BX0, 34, T_BX1 - T_BX0 + 1, 172, WHITE);     // the solid potential wall
  txt(s, &fonts::Font0, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  s.drawString("V0", T_BX0 - 4, 25);

  if (col) {
    if (ct < 0.5f) dottedV(s, (int)tXm, 30, base, 4, WHITE);
    if (ct > 0.3f) {
      int r = (int)((ct - 0.3f) / 0.7f * 60.0f);
      if (r > 0) s.drawCircle((int)tXm, base - 10, r, WHITE);
    }
    if (ct > 0.7f) {
      int cx = (int)tXm, cy = base - 12;
      s.drawFastHLine(cx - 8, cy, 17, WHITE);
      s.drawFastVLine(cx, cy - 8, 17, WHITE);
    }
  }
  char buf[40];
  if (tValid) snprintf(buf, sizeof(buf), "T=%.1f%%  R=%.1f%%", tT * 100.0f, (1.0f - tT) * 100.0f);
  else        snprintf(buf, sizeof(buf), "T=--  R=--");
  s.drawString(buf, 4, 214);
  drawFooter(s, "CLICK:MEASURE HOLD:MENU");
}

// ═════════════════════════════════════════════════════════════════════════
//  MODE 4 : QUANTUM ERASER (delayed choice)
// ═════════════════════════════════════════════════════════════════════════
static void initEraser(uint32_t now) {
  float sumT = 0.0f, sumI = 0.0f;
  for (int i = 0; i < E_NB; i++) {
    float d  = (36.0f + 2.0f * i + 1.0f) - 116.0f;
    float g1 = (d - 14.0f) / 16.0f, g2 = (d + 14.0f) / 16.0f, g0 = d / 52.0f;
    eTag[i] = expf(-g1 * g1) + expf(-g2 * g2);
    eInt[i] = expf(-g0 * g0) * 0.5f * (1.0f + cosf(6.2831853f * d / 18.0f));
    sumT += eTag[i];
    sumI += eInt[i];
  }
  for (int i = 0; i < E_NB; i++) { eTag[i] /= sumT; eInt[i] /= sumI; }
  for (int i = 0; i < 28; i++) phot[i].alive = false;
  eV = 0.0f; eTarget = 0; eSpawn = 0.0f;
  eLastToggle = now; eWipe = 0;
  detFlashUntil[0] = detFlashUntil[1] = 0;
  sprHits.fillScreen(BLACK);
}

static void toggleEraser(uint32_t now) {
  eTarget ^= 1;
  eLastToggle = now;
  eToggles++;
  eWipe = now;
  sprHits.fillScreen(BLACK);
  commitResult(MODE_ERASER, eTarget);
}

static void spawnPhoton() {
  for (int i = 0; i < 28; i++) {
    if (phot[i].alive) continue;
    Photon& p = phot[i];
    int slit = (physRand01() < 0.5f) ? 0 : 1;
    p.slitY = slit ? 130.0f : 102.0f;
    p.x = 10.0f; p.y = 116.0f;
    p.vx = 140.0f;
    p.vy = (p.slitY - 116.0f) / 48.0f * p.vx;
    p.stage = 0;
    p.alive = true;
    return;
  }
}

static void updateEraser(uint32_t now, float dt) {
  float tv = eTarget ? 1.0f : 0.0f;
  if (eV < tv) eV = fminf(tv, eV + 0.6f * dt);
  else if (eV > tv) eV = fmaxf(tv, eV - 0.6f * dt);
  if ((now - eLastToggle) > 14000UL) toggleEraser(now);      // autonomous lab cycle

  float acc = 0.0f;
  for (int i = 0; i < E_NB; i++) { acc += (1.0f - eV) * eTag[i] + eV * eInt[i]; eCdf[i] = acc; }

  eSpawn += dt * 16.0f;
  while (eSpawn >= 1.0f) { eSpawn -= 1.0f; spawnPhoton(); }

  for (int i = 0; i < 28; i++) {
    Photon& p = phot[i];
    if (!p.alive) continue;
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    if (p.stage == 0 && p.x >= 58.0f) {
      p.x = 58.0f; p.y = p.slitY; p.stage = 1;
      float yPlane = 116.0f + (p.slitY - 116.0f) * 0.45f;
      p.vy = (yPlane - p.slitY) * p.vx / (float)(E_PLANE_X - 58);
      if (eV < 0.5f) detFlashUntil[p.slitY > 116.0f ? 1 : 0] = now + 140;   // which-way click
    } else if (p.stage == 1 && p.x >= (float)E_PLANE_X) {
      float r = physRand01();                                // delayed choice: decided here
      int idx = E_NB - 1;
      for (int k = 0; k < E_NB; k++) { if (r < eCdf[k]) { idx = k; break; } }
      p.targetY = 36.0f + 2.0f * idx + frand() * 2.0f;
      float rem = (float)E_SCREEN_X - p.x;
      if (rem < 1.0f) rem = 1.0f;
      p.vy = (p.targetY - p.y) * p.vx / rem;
      p.stage = 2;
    } else if (p.stage == 2 && p.x >= (float)E_SCREEN_X) {
      int hx = (int)(frand() * 44.0f);
      int hy = (int)(p.targetY - 36.0f);
      if (hy < 0) hy = 0;
      if (hy > 158) hy = 158;
      sprHits.fillRect(hx, hy, 2, 2, WHITE);
      eHits++;
      p.alive = false;
    }
  }
  for (int k = 0; k < 4; k++)                                // slow persistence decay
    sprHits.drawPixel((int)(frand() * 46.0f), (int)(frand() * 160.0f), BLACK);
}

static void renderEraser(uint32_t t) {
  lgfx::LGFX_Sprite& s = sprLeft;
  s.fillScreen(BLACK);
  drawHeader(s, "QUANTUM ERASER");
  txt(s, &fonts::Font0, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  s.drawString(eTarget ? "WHICH-WAY ERASED" : "WHICH-WAY RECORDED", 6, 26);
  s.drawString("EQ", 114, 25);

  // photon source with pulsing rays
  s.fillCircle(8, 116, 3, WHITE);
  int rl = 5 + (int)(3.0f * sinf((float)t * 0.012f));
  for (int i = 0; i < 6; i++) {
    float a = -0.9f + i * 0.36f;
    s.drawLine(8 + (int)(4 * cosf(a)), 116 + (int)(4 * sinf(a)),
               8 + (int)((4 + rl) * cosf(a)), 116 + (int)((4 + rl) * sinf(a)), WHITE);
  }
  // double-slit wall
  s.fillRect(57, 34, 4, 64, WHITE);
  s.fillRect(57, 106, 4, 20, WHITE);
  s.fillRect(57, 134, 4, 72, WHITE);

  // wavefront arcs behind the slits (dashed while path info exists)
  for (int sl = 0; sl < 2; sl++) {
    int sy = sl ? 130 : 102;
    for (int k = 0; k < 4; k++) {
      float r = fmodf((float)t * 0.03f + k * 14.0f, 56.0f);
      for (int a = -7; a <= 7; a++) {
        if (eV < 0.5f && (a & 1)) continue;
        float th = a * 0.1222f;
        s.drawPixel(60 + (int)(r * cosf(th)), sy + (int)(r * sinf(th)), WHITE);
      }
    }
  }

  // eraser device (hatching density follows the erasure level)
  dottedV(s, E_PLANE_X, 34, 206, 4, WHITE);
  if (eV > 0.05f) {
    s.drawRect(112, 40, 17, 152, WHITE);
    int step = (int)(16.0f - 11.0f * eV);
    for (int yy = 40; yy + 8 <= 192; yy += step) s.drawLine(112, yy, 128, yy + 8, WHITE);
  }

  // which-way detector markers
  for (int sl = 0; sl < 2; sl++) {
    int yc = sl ? 130 : 102;
    if (eV < 0.5f) {
      if (t < detFlashUntil[sl]) s.fillRect(65, yc - 6, 12, 12, WHITE);
      else { s.drawRect(65, yc - 6, 12, 12, WHITE); s.fillCircle(71, yc, 2, WHITE); }
    } else {
      dottedH(s, 65, 76, yc - 6, 3, WHITE);
      dottedH(s, 65, 76, yc + 5, 3, WHITE);
      dottedV(s, 65, yc - 6, yc + 5, 3, WHITE);
      dottedV(s, 76, yc - 6, yc + 5, 3, WHITE);
      s.drawLine(65, yc - 6, 76, yc + 5, WHITE);
    }
  }

  // photons
  for (int i = 0; i < 28; i++) {
    const Photon& p = phot[i];
    if (!p.alive) continue;
    s.fillRect((int)p.x - 1, (int)p.y - 1, 2, 2, WHITE);
    s.drawPixel((int)(p.x - p.vx * 0.03f), (int)(p.y - p.vy * 0.03f), WHITE);
  }

  // detector screen with persistent hits
  sprHits.pushSprite(&s, 160, 36);
  s.drawRect(159, 35, 48, 162, WHITE);

  // delayed-choice wipe front
  if (eWipe != 0 && (t - eWipe) < 500) {
    int xw = E_PLANE_X + (int)((float)(t - eWipe) / 500.0f * 86.0f);
    s.drawFastVLine(xw, 30, 176, WHITE);
    s.drawFastVLine(xw + 1, 30, 176, WHITE);
  }

  char buf[48];
  float D = sqrtf(fmaxf(0.0f, 1.0f - eV * eV));
  snprintf(buf, sizeof(buf), "V=%.2f D=%.2f HITS %lu", eV, D, (unsigned long)eHits);
  txt(s, &fonts::Font0, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  s.drawString(buf, 6, 212);
  drawFooter(s, "CLICK:TOGGLE HOLD:MENU");
}

// ═════════════════════════════════════════════════════════════════════════
//  MODE 5 : Q-CHAOS CPU (4-qubit processor)
// ═════════════════════════════════════════════════════════════════════════
static void applyChaosGate(Cx* psi, int type, int a, int b, float th) {
  switch (type) {
    case G_H:    gateH(psi, 4, a); break;
    case G_X:    gateX(psi, 4, a); break;
    case G_Z:    gatePhase(psi, 4, a, 3.14159265f); break;
    case G_T:    gatePhase(psi, 4, a, 0.78539816f); break;
    case G_PH:   gatePhase(psi, 4, a, th); break;
    case G_CNOT: gateCNOT(psi, 4, a, b); break;
    default: break;
  }
}

static float gaussApprox() { return (physRand01() + physRand01() + physRand01() - 1.5f) * 2.0f; }

static void chaosKick(int q, float scale) {
  gateRx(qN, 4, q, gaussApprox() * chaosSigma * scale);
  gatePhase(qN, 4, q, gaussApprox() * chaosSigma * scale);
}

static void initChaos(uint32_t now) {
  stateInitUniform(qI, 4);
  stateInitUniform(qN, 4);
  chaosReset = now;
  chaosLast = now;
  chaosF = 1.0f;
  chaosT2 = 0.0f;
}

static void updateChaos(uint32_t now) {
  if ((now - chaosLast) < CHAOS_GATE_MS) return;
  chaosLast = now;

  float r = physRand01();
  int a = (int)(physRand01() * 4.0f); if (a > 3) a = 3;
  int b = (a + 1 + (int)(physRand01() * 3.0f)) & 3;
  int type;
  if (r < 0.18f) type = G_H; else if (r < 0.33f) type = G_X; else if (r < 0.48f) type = G_Z;
  else if (r < 0.63f) type = G_T; else if (r < 0.78f) type = G_PH; else type = G_CNOT;
  float th = physRand01() * 6.2831853f;

  applyChaosGate(qI, type, a, b, th);       // ideal processor
  applyChaosGate(qN, type, a, b, th);       // physical processor
  chaosKick(a, 1.0f);                       // thermal gate error
  if (type == G_CNOT) chaosKick(b, 1.0f);
  for (int q = 0; q < 4; q++) {             // idle dephasing
    if (q == a || (type == G_CNOT && q == b)) continue;
    gatePhase(qN, 4, q, gaussApprox() * chaosSigma * 0.4f);
  }

  memmove(&gLog[0], &gLog[1], sizeof(GateRec) * 9);
  gLog[9].type = (uint8_t)type; gLog[9].a = (uint8_t)a; gLog[9].b = (uint8_t)b;
  if (gCount < 10) gCount++;
  chaosGates++;

  chaosTemp += (physRand01() - 0.5f) * 3.0f;
  if (chaosTemp < 12.0f) chaosTemp = 12.0f;
  if (chaosTemp > 38.0f) chaosTemp = 38.0f;
  chaosSigma = 0.05f + 0.006f * (chaosTemp - 12.0f);

  Cx ov = mkc(0, 0);
  for (int i = 0; i < 16; i++) {
    Cx c = mkc(qI[i].re, -qI[i].im);
    ov = cadd(ov, cmul(c, qN[i]));
  }
  chaosF = norm2(ov);
  float ms = (float)(now - chaosReset);
  chaosT2 = (chaosF < 0.999f && chaosF > 0.001f) ? (-(ms / 1000.0f) / logf(chaosF)) : 0.0f;

  if (chaosF < 0.45f) {                     // automatic recalibration
    chaosRecal++;
    chaosRecalAt = now;
    initChaos(now);
  }
}

static void drawGateGlyph(lgfx::LGFX_Sprite& s, int type, int x, int a, int b) {
  const int yA = 196 + a * 8;
  if (type == G_CNOT) {
    const int yB = 196 + b * 8;
    int y0 = yA < yB ? yA : yB, y1 = yA < yB ? yB : yA;
    s.fillRect(x - 4, y0 - 4, 9, (y1 - y0) + 8, BLACK);
    s.drawFastVLine(x, y0, y1 - y0, WHITE);
    s.fillCircle(x, yA, 2, WHITE);
    s.drawCircle(x, yB, 3, WHITE);
    s.drawFastHLine(x - 3, yB, 7, WHITE);
    return;
  }
  s.fillRect(x - 4, yA - 4, 9, 8, BLACK);
  switch (type) {
    case G_H:
      s.fillRect(x - 3, yA - 3, 7, 7, WHITE);
      s.drawFastVLine(x - 1, yA - 2, 5, BLACK);
      s.drawFastVLine(x + 1, yA - 2, 5, BLACK);
      s.drawPixel(x, yA, WHITE);
      break;
    case G_X:
      s.drawCircle(x, yA, 3, WHITE);
      s.drawFastHLine(x - 3, yA, 7, WHITE);
      s.drawFastVLine(x, yA - 3, 7, WHITE);
      break;
    case G_Z:
      s.drawFastHLine(x - 3, yA - 3, 7, WHITE);
      s.drawLine(x + 3, yA - 3, x - 3, yA + 3, WHITE);
      s.drawFastHLine(x - 3, yA + 3, 7, WHITE);
      break;
    case G_T:
      s.fillTriangle(x - 3, yA + 3, x + 3, yA + 3, x, yA - 3, WHITE);
      break;
    default:
      s.drawRect(x - 3, yA - 3, 7, 7, WHITE);
      s.drawLine(x - 3, yA + 3, x + 3, yA - 3, WHITE);
      break;
  }
}

static void renderChaos(uint32_t t, float ct) {
  lgfx::LGFX_Sprite& s = sprLeft;
  s.fillScreen(BLACK);
  drawHeader(s, "4-QUBIT Q-CPU");
  const bool col = (ct >= 0.0f);
  const float scanY = col ? (25.0f + ct * 1.25f * 160.0f) : 1000.0f;

  for (int i = 0; i < 16; i++) {
    const int x = 13 + (i % 4) * 46, y = 25 + (i / 4) * 39;
    float p  = norm2(qN[i]);
    float pi = norm2(qI[i]);
    float r  = 13.0f * powf(p, 0.35f) * (1.0f + 0.08f * sinf((float)t * 0.01f + i * 0.7f));
    float uLoc = col ? clamp01((scanY - (float)(y + 18)) / 22.0f) : 0.0f;
    bool  meas = col && ((uint8_t)i == collapseResult);
    float rr = r * (1.0f - uLoc);
    if (meas) rr += (13.0f + 2.0f * sinf(ct * 30.0f)) * uLoc;

    s.drawRect(x, y, 45, 37, WHITE);
    const int cxm = x + 22, cym = y + 14;
    int ri = (int)rr;
    bool flicker = col && uLoc <= 0.001f && frand() < 0.25f;
    if (ri >= 1 && !flicker) {
      s.fillCircle(cxm, cym, ri, WHITE);
      if (!col) {
        float ang = atan2f(qN[i].im, qN[i].re);
        float ca = cosf(ang), sa = sinf(ang);
        if (ri >= 4) s.drawLine(cxm, cym, cxm + (int)((ri - 1) * ca), cym + (int)((ri - 1) * sa), BLACK);
        s.drawLine(cxm + (int)(ri * ca), cym + (int)(ri * sa),
                   cxm + (int)((ri + 4) * ca), cym + (int)((ri + 4) * sa), WHITE);
      }
    }
    txt(s, &fonts::Font0, 1.0f, lgfx::textdatum_t::top_left, WHITE);
    s.drawString(basisLbl[i], x + 2, y + 27);
    if (!col) {
      int w = (int)(clamp01(fabsf(p - pi) * 300.0f / 41.0f) * 41.0f);
      if (w > 0) s.fillRect(x + 2, y + 35, w, 2, WHITE);
    }
  }
  if (col && ct > 0.3f) {
    int mi = collapseResult;
    int r = (int)((ct - 0.3f) * 90.0f);
    if (r > 0) s.drawCircle(13 + (mi % 4) * 46 + 22, 25 + (mi / 4) * 39 + 14, r, WHITE);
  }
  if (col && scanY < 180.0f) {
    s.drawFastHLine(0, (int)scanY, LEFT_W, WHITE);
    s.drawFastHLine(0, (int)scanY + 1, LEFT_W, WHITE);
  }

  char buf[40];
  snprintf(buf, sizeof(buf), "F=%.2f T=%.0fmK s=%.2f", chaosF, chaosTemp, chaosSigma);
  txt(s, &fonts::Font0, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  s.drawString(buf, 4, 182);
  if ((t - chaosRecalAt) < 700 && chaosRecal > 0 && ((t / 120) & 1)) s.drawString("RECAL", 168, 182);

  // scrolling gate circuit
  for (int q = 0; q < 4; q++) {
    int y = 196 + q * 8;
    s.drawFastHLine(14, y, 192, WHITE);
    char lb[3] = { 'q', (char)('0' + q), 0 };
    s.drawString(lb, 0, y - 4);
  }
  float frac = clamp01((float)(t - chaosLast) / (float)CHAOS_GATE_MS);
  for (int age = 0; age < gCount; age++) {
    const GateRec& g = gLog[9 - age];
    int x = 186 - age * 22 + (int)((1.0f - frac) * 22.0f);
    if (x < 26 || x > 204) continue;
    drawGateGlyph(s, g.type, x, g.a, g.b);
  }
  drawFooter(s, "CLICK:MEASURE HOLD:MENU");
}

// ═════════════════════════════════════════════════════════════════════════
//  MENU
// ═════════════════════════════════════════════════════════════════════════
static void drawIcon(lgfx::LGFX_Sprite& s, int m, int cx, int cy, uint16_t c, uint32_t t) {
  switch (m) {
    case MODE_HADAMARD: {
      int py = cy;
      for (int i = -12; i <= 12; i++) {
        int y = cy + (int)(6.0f * sinf(i * 0.5f + (float)t * 0.01f));
        if (i > -12) s.drawLine(cx + i - 1, py, cx + i, y, c);
        py = y;
      }
      break;
    }
    case MODE_EPR:
      s.drawCircle(cx - 10, cy, 4, c);
      s.drawCircle(cx + 10, cy, 4, c);
      if ((t / 200) & 1) dottedH(s, cx - 5, cx + 5, cy, 2, c);
      else               dottedH(s, cx - 5, cx + 5, cy, 3, c);
      break;
    case MODE_TUNNEL:
      s.fillRect(cx - 1, cy - 8, 3, 16, c);
      s.drawLine(cx - 14, cy, cx - 4, cy, c);
      dottedH(s, cx + 4, cx + 13, cy, 2, c);
      break;
    case MODE_ERASER:
      s.drawFastVLine(cx - 6, cy - 9, 5, c);
      s.drawFastVLine(cx - 6, cy - 2, 5, c);
      s.drawFastVLine(cx - 6, cy + 5, 5, c);
      for (int i = 0; i < 4; i++) s.drawPixel(cx + 4 + i * 3, cy - 6 + ((i & 1) ? 6 : 0) + ((t / 150) & 1), c);
      break;
    default:
      for (int i = 0; i < 4; i++) {
        int rr = 1 + (int)(1.5f + 1.2f * sinf((float)t * 0.01f + i));
        s.fillCircle(cx - 6 + (i % 2) * 12, cy - 5 + (i / 2) * 10, rr, c);
      }
      break;
  }
}

static void renderMenu(uint32_t t) {
  lgfx::LGFX_Sprite& s = sprLeft;
  s.fillScreen(BLACK);
  txt(s, &fonts::Font2, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  s.drawString("QUANTUM LAB", 8, 5);
  s.drawString("SIMULATOR v2", 8, 24);
  s.drawFastHLine(0, 46, LEFT_W, WHITE);

  const int ax = 168, ay = 24;                             // animated atom
  s.fillCircle(ax, ay, 3, WHITE);
  for (int k = 0; k < 3; k++) {
    float rot = k * 1.0472f + (float)t * 0.0004f;
    float cr = cosf(rot), sr = sinf(rot);
    for (int a = 0; a < 24; a++) {
      float th = a * 0.2618f;
      float ex = 18.0f * cosf(th), ey = 7.0f * sinf(th);
      s.drawPixel(ax + (int)(ex * cr - ey * sr), ay + (int)(ex * sr + ey * cr), WHITE);
    }
    float th = (float)t * 0.004f * (1.0f + k * 0.3f) + k * 2.0f;
    float ex = 18.0f * cosf(th), ey = 7.0f * sinf(th);
    s.fillCircle(ax + (int)(ex * cr - ey * sr), ay + (int)(ex * sr + ey * cr), 2, WHITE);
  }

  for (int i = 0; i < MODE_COUNT; i++) {
    int y = 52 + i * 35;
    uint16_t c;
    if (i == menuIndex) { s.fillRoundRect(6, y, LEFT_W - 12, 31, 5, WHITE); c = BLACK; }
    else                { s.drawRoundRect(6, y, LEFT_W - 12, 31, 5, WHITE); c = WHITE; }
    txt(s, &fonts::Font2, 1.0f, lgfx::textdatum_t::middle_left, c);
    s.drawString(MENU_ITEMS[i], 14, y + 16);
    drawIcon(s, i, LEFT_W - 28, y + 16, c, t);
  }
  drawFooter(s, "CLICK:NEXT  HOLD:ENTER");
}

// ═════════════════════════════════════════════════════════════════════════
//  RIGHT DATA PANEL
// ═════════════════════════════════════════════════════════════════════════
static void renderPanel(uint32_t now) {
  lgfx::LGFX_Sprite& s = sprRight;
  s.fillScreen(BLACK);
  s.drawFastVLine(0, 0, SCR_H, WHITE);

  const ModeId pm = (state == STATE_MENU) ? (ModeId)menuIndex : curMode;
  const ModeStats& st = stats[pm];
  char big[8]; char ln[4][16]; char ft[3][20];
  const char* title = "RESULT";
  PanelKind pk = PK_BINARY;
  big[0] = 0;
  for (int i = 0; i < 4; i++) ln[i][0] = 0;
  for (int i = 0; i < 3; i++) ft[i][0] = 0;
  const unsigned long N = (unsigned long)st.total;

  switch (pm) {
    case MODE_HADAMARD:
      title = "H-GATE";
      strcpy(big, !st.have ? "|?>" : (st.last ? "|1>" : "|0>"));
      snprintf(ln[0], 16, "N   %lu", N);
      snprintf(ln[1], 16, "|0> %lu", (unsigned long)st.c0);
      snprintf(ln[2], 16, "|1> %lu", (unsigned long)st.c1);
      snprintf(ln[3], 16, "P0 %.2f", st.total ? (float)st.c0 / st.total : 0.5f);
      strcpy(ft[0], "H|0> = |+>");
      strcpy(ft[1], "P0=P1=0.50");
      snprintf(ft[2], 20, "ADC FALLBK %lu", (unsigned long)entropyFallbacks);
      break;
    case MODE_EPR: {
      title = "BELL PAIR";
      strcpy(big, !st.have ? "|??>" : (st.last ? "|11>" : "|00>"));
      snprintf(ln[0], 16, "N    %lu", N);
      snprintf(ln[1], 16, "|00> %lu", (unsigned long)st.c0);
      snprintf(ln[2], 16, "|11> %lu", (unsigned long)st.c1);
      if (st.total) snprintf(ln[3], 16, "E=%+.2f", 1.0f - 2.0f * (float)eprMismatch / st.total);
      else          snprintf(ln[3], 16, "E=--");
      strcpy(ft[0], "(|00>+|11>)/r2");
      strcpy(ft[1], "CHSH 2.83 > 2");
      snprintf(ft[2], 20, "MISMATCH %lu", (unsigned long)eprMismatch);
      break;
    }
    case MODE_TUNNEL:
      title = "TUNNELING";
      strcpy(big, !st.have ? "---" : (st.last ? "TUN" : "REF"));
      if (tValid) {
        snprintf(ln[0], 16, "T %.1f%%", tT * 100.0f);
        snprintf(ln[1], 16, "R %.1f%%", (1.0f - tT) * 100.0f);
      } else {
        snprintf(ln[0], 16, "T --");
        snprintf(ln[1], 16, "R --");
      }
      snprintf(ln[2], 16, "REF %lu", (unsigned long)st.c0);
      snprintf(ln[3], 16, "TUN %lu", (unsigned long)st.c1);
      strcpy(ft[0], "V0=.22 E=.125");
      strcpy(ft[1], "kappa=0.44 a=4");
      snprintf(ft[2], 20, "N=%lu", N);
      break;
    case MODE_ERASER: {
      title = "ERASER";
      strcpy(big, eTarget ? "WAVE" : "PATH");
      float D = sqrtf(fmaxf(0.0f, 1.0f - eV * eV));
      snprintf(ln[0], 16, "V %.2f", eV);
      snprintf(ln[1], 16, "D %.2f", D);
      snprintf(ln[2], 16, "HITS %lu", (unsigned long)eHits);
      snprintf(ln[3], 16, "FLIPS %lu", (unsigned long)eToggles);
      strcpy(ft[0], "D^2 + V^2 = 1");
      strcpy(ft[1], "DELAYED CHOICE");
      long rem = 14 - (long)((now - eLastToggle) / 1000UL);
      if (rem < 0) rem = 0;
      snprintf(ft[2], 20, "AUTO FLIP %lds", rem);
      break;
    }
    default: {
      title = "Q-CPU";
      pk = PK_HEX;
      if (!st.have) strcpy(big, "????");
      else for (int b = 0; b < 4; b++) big[b] = (st.last & (8 >> b)) ? '1' : '0';
      if (st.have) big[4] = 0;
      snprintf(ln[0], 16, "F   %.1f%%", chaosF * 100.0f);
      snprintf(ln[1], 16, "ERR %.1f%%", (1.0f - chaosF) * 100.0f);
      snprintf(ln[2], 16, "T2 %.1fs", chaosT2);
      snprintf(ln[3], 16, "T %.0fmK", chaosTemp);
      snprintf(ft[0], 20, "GATES %lu", (unsigned long)chaosGates);
      snprintf(ft[1], 20, "RECAL %lu", (unsigned long)chaosRecal);
      snprintf(ft[2], 20, "MEAS %lu", N);
      break;
    }
  }

  txt(s, &fonts::Font2, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  s.drawString(title, 8, 6);
  txt(s, &fonts::Font4, 1.3f, lgfx::textdatum_t::middle_center, WHITE);
  s.drawString(big, RIGHT_W / 2 + 1, 44);
  s.drawFastHLine(1, 70, RIGHT_W - 1, WHITE);
  txt(s, &fonts::Font2, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  for (int i = 0; i < 4; i++) s.drawString(ln[i], 8, 76 + i * 16);
  s.drawFastHLine(1, 144, RIGHT_W - 1, WHITE);
  txt(s, &fonts::Font0, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  s.drawString("HISTORY", 8, 148);

  int n = (st.total < (uint32_t)HIST_LEN) ? (int)st.total : HIST_LEN;
  for (int i = 0; i < n; i++) {
    int cx = 14 + (i % 5) * 20, cy = 172 + (i / 5) * 20;
    if (pk == PK_BINARY) {
      if (st.hist[i]) s.fillCircle(cx, cy, 6, WHITE);
      else            s.drawCircle(cx, cy, 6, WHITE);
    } else {
      char h[2] = { "0123456789ABCDEF"[st.hist[i] & 15], 0 };
      txt(s, &fonts::Font0, 2.0f, lgfx::textdatum_t::middle_center, WHITE);
      s.drawString(h, cx, cy);
    }
  }
  txt(s, &fonts::Font0, 1.0f, lgfx::textdatum_t::top_left, WHITE);
  for (int i = 0; i < 3; i++) s.drawString(ft[i], 8, 206 + i * 10);
}

// ═════════════════════════════════════════════════════════════════════════
//  Mode management & input
// ═════════════════════════════════════════════════════════════════════════
static void initMode(ModeId m, uint32_t now) {
  switch (m) {
    case MODE_HADAMARD: initHadamard();   break;
    case MODE_EPR:      initEPR();        break;
    case MODE_TUNNEL:   initTunnel();     break;
    case MODE_ERASER:   initEraser(now);  break;
    case MODE_CHAOS:    initChaos(now);   break;
    default: break;
  }
}

static void startCollapse(uint32_t now) {
  reseedFromNoise();                       // physical entropy -> wave function collapse
  switch (curMode) {
    case MODE_HADAMARD: {
      float p0 = norm2(psiPlus.a);
      collapseResult = (physRand01() < p0) ? 0 : 1;
      hadXm = 30.0f + physRand01() * 150.0f;
      collapseDur = 650;
      break;
    }
    case MODE_EPR: {
      int idx = sampleIndex(bell, 4);
      if (idx == 1 || idx == 2) eprMismatch++;
      collapseResult = (idx == 3) ? 1 : 0;
      collapseDur = 700;
      break;
    }
    case MODE_TUNNEL:
      prepareTunnelCollapse();
      collapseDur = 700;
      break;
    case MODE_CHAOS:
      collapseResult = (uint8_t)sampleIndex(qN, 16);
      collapseDur = 800;
      break;
    default:
      return;
  }
  collapseStart = now;
  frozenTime    = now;
  state         = STATE_COLLAPSE;
}

static void finishCollapse(uint32_t now) {
  commitResult(curMode, collapseResult);
  initMode(curMode, now);                  // instant reset to full superposition
  state = STATE_RUN;
  panelDirty = true;
}

static void handleButton(BtnEvent ev, uint32_t now) {
  if (ev == BTN_NONE) return;
  switch (state) {
    case STATE_MENU:
      if (ev == BTN_SHORT) {
        menuIndex = (menuIndex + 1) % MODE_COUNT;
      } else {
        curMode = (ModeId)menuIndex;
        initMode(curMode, now);
        state = STATE_RUN;
      }
      panelDirty = true;
      break;
    case STATE_RUN:
      if (ev == BTN_SHORT) {
        if (curMode == MODE_ERASER) toggleEraser(now);
        else startCollapse(now);
      } else {
        state = STATE_MENU;
        menuIndex = (int)curMode;
        panelDirty = true;
      }
      break;
    case STATE_COLLAPSE:
      break;
    default:
      break;
  }
}

static void renderFrame(uint32_t now, float dt) {
  float ct = -1.0f;
  if (state == STATE_COLLAPSE) {
    ct = (float)(now - collapseStart) / (float)collapseDur;
    if (ct >= 1.0f) { finishCollapse(now); ct = -1.0f; }
  }
  const uint32_t tv = (state == STATE_COLLAPSE) ? frozenTime : now;

  if (state == STATE_MENU) {
    renderMenu(now);
  } else {
    if (state == STATE_RUN) {
      switch (curMode) {
        case MODE_TUNNEL: updateTunnel();          break;
        case MODE_ERASER: updateEraser(now, dt);   break;
        case MODE_CHAOS:  updateChaos(now);        break;
        default: break;
      }
    }
    switch (curMode) {
      case MODE_HADAMARD: renderHadamard(tv, ct); break;
      case MODE_EPR:      renderEPR(tv, ct);      break;
      case MODE_TUNNEL:   renderTunnel(tv, ct);   break;
      case MODE_ERASER:   renderEraser(now);      break;
      case MODE_CHAOS:    renderChaos(tv, ct);    break;
      default: break;
    }
  }
  sprLeft.pushSprite(&lcd, 0, 0);

  if (state != STATE_MENU && (curMode == MODE_TUNNEL || curMode == MODE_ERASER || curMode == MODE_CHAOS) &&
      (now - lastPanelLive) >= PANEL_LIVE_MS) {
    lastPanelLive = now;
    panelDirty = true;
  }
  if (panelDirty) {                         // right zone redrawn only when its data changed
    panelDirty = false;
    renderPanel(now);
    sprRight.pushSprite(&lcd, LEFT_W, 0);
  }
}

// ───────────────────────── Arduino entry points ─────────────────────────
void setup() {
  Serial.begin(115200);
  button.begin();

  pinMode(PIN_ENTROPY, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_ENTROPY, ADC_11db);

  lcd.init();
  lcd.setRotation(1);                       // 320 x 240
  lcd.setColorDepth(16);
  lcd.fillScreen(BLACK);

  sprLeft.setColorDepth(8);
  sprRight.setColorDepth(8);
  sprHits.setColorDepth(8);
  bool ok = sprLeft.createSprite(LEFT_W, SCR_H) &&
            sprRight.createSprite(RIGHT_W, SCR_H) &&
            sprHits.createSprite(46, 160);
  if (!ok) {
    lcd.setTextColor(WHITE);
    lcd.drawString("SPRITE ALLOC FAILED", 10, 10);
    Serial.println("Sprite allocation failed");
    while (true) delay(1000);
  }
  sprHits.fillScreen(BLACK);

  for (int i = 0; i < 16; i++) {
    for (int b = 0; b < 4; b++) basisLbl[i][b] = (i & (8 >> b)) ? '1' : '0';
    basisLbl[i][4] = 0;
  }
  memset(stats, 0, sizeof(stats));
  memset(phot, 0, sizeof(phot));
  memset(gLog, 0, sizeof(gLog));
  visSeed ^= (uint32_t)micros() | 1u;

  initHadamard();
  initEPR();
  initTunnel();
  initChaos(millis());
  reseedFromNoise();

  state = STATE_MENU;
  panelDirty = true;
  lastFrame = millis();
}

void loop() {
  uint32_t now = millis();
  handleButton(button.update(now), now);

  now = millis();
  if ((now - lastFrame) >= FRAME_MS) {
    float dt = (float)(now - lastFrame) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    lastFrame = now;
    renderFrame(now, dt);
  }
  delay(1);
}
