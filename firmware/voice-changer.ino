// voice-changer.ino — Week 3: Music Week (Half Life)
// Talk into the mic, hear yourself back transformed.
//   Button 1 -> CHIPMUNK (pitch up 2x)   Button 2 -> DEMON (pitch down 0.5x)
//   Button 3 -> ROBOT (ring modulation)
//
// Hardware: ESP32-S3 Supermini + INMP441 I2S mic + MAX98357A I2S amp + speaker.
// The mic and the amp SHARE the BCLK and WS clock lines — one I2S port runs
// full-duplex (RX channel from mic, TX channel to amp) at 16 kHz.
//
// Written for the ESP32 Arduino core 3.x (new i2s_std API).
// Compile-verified against core 3.3.12 for ESP32-S3 (Oct 2026).
// Not yet run on hardware — first bring-up will need level/tuning.
//
// Arduino IDE settings (Tools menu) for the Supermini's native USB:
//   USB CDC On Boot -> Enabled   (needed for Serial output + easy uploads)
//
// Wiring (Supermini pin numbers — verify against your specific board!):
//   INMP441: VDD->3V3, GND->GND, SD->GPIO6, WS->GPIO5, SCK->GPIO4, L/R->GND
//   MAX98357A: VIN->5V (or 3V3, quieter), GND->GND, DIN->GPIO7,
//              BCLK->GPIO4, LRCK->GPIO5, SD->GPIO11 (pull high = enable)
//   Buttons: one leg -> GPIO8/9/10, other leg -> GND (uses internal pull-ups,
//            no resistors needed). Avoid GPIO0 (boot) and GPIO19/20 (USB).
//
// How the effects work:
//   CHIPMUNK/DEMON: overlap-add pitch shifter. The incoming audio is kept in
//   a ring buffer; each output grain is a resampled slice of the input
//   (faster = higher pitch, slower = lower pitch) and overlapping grains are
//   crossfaded with a Hann window. Smooth, no stutter, ~30-80 ms latency.
//   ROBOT: real-time ring modulation — the voice is multiplied by a 30 Hz
//   sine wave. Zero extra latency.

#include <driver/i2s_std.h>
#include <math.h>

// ---------- pins ----------
#define I2S_BCLK     GPIO_NUM_4
#define I2S_WS       GPIO_NUM_5
#define I2S_DIN      GPIO_NUM_6    // mic -> ESP32
#define I2S_DOUT     GPIO_NUM_7    // ESP32 -> amp
#define AMP_SD       GPIO_NUM_11   // amp shutdown, HIGH = on
#define BTN_CHIPMUNK GPIO_NUM_8
#define BTN_DEMON    GPIO_NUM_9
#define BTN_ROBOT    GPIO_NUM_10

// ---------- audio ----------
#define SAMPLE_RATE   16000
#define I2S_PORT      I2S_NUM_0
#define MIC_GAIN      3                  // digital gain, tune to taste

// ---------- pitch shifter ----------
#define GRAIN   512                      // output grain length (samples)
#define HOP     256                      // output hop (50% overlap -> Hann COLA)
#define RING_N  8192                     // ring buffer (512 ms), must be power of 2
#define RING_M  (RING_N - 1)

static int16_t ringBuf[RING_N];
static uint32_t writePos = 0;            // monotonic input sample counter
static float readPos = 0;                // monotonic input position for grains
static float olaBuf[GRAIN + HOP];
static float hannWin[GRAIN];
static bool shifterSync = true;          // true -> re-sync readPos on next grain

static inline int16_t clamp16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

// linear-interpolated read from the ring buffer at a fractional position
static inline float ringRead(float pos) {
  int32_t i0 = (int32_t)pos;
  float frac = pos - (float)i0;
  int16_t a = ringBuf[i0 & RING_M];
  int16_t b = ringBuf[(i0 + 1) & RING_M];
  return a + (b - a) * frac;
}

// Produce HOP output samples at pitch factor P (2.0 = chipmunk, 0.5 = demon).
// Reads a resampled window of the input and overlap-adds it into olaBuf.
static void pitchGrain(float P, int16_t* out) {
  float winLen = GRAIN * P;
  // make sure we have enough buffered input; if not, re-sync
  if ((float)writePos - readPos < winLen + HOP) {
    readPos = (float)writePos - (winLen + HOP);
    for (int i = 0; i < GRAIN + HOP; i++) olaBuf[i] = 0;
  }
  for (int i = 0; i < GRAIN; i++)
    olaBuf[i] += ringRead(readPos + (float)i * P) * hannWin[i];
  for (int i = 0; i < HOP; i++) {
    out[i] = clamp16((int32_t)olaBuf[i]);
    olaBuf[i] = olaBuf[i + HOP];
  }
  for (int i = GRAIN; i < GRAIN + HOP; i++) olaBuf[i] = 0;
  readPos += HOP;
}

// ---------- modes ----------
enum Mode { CHIPMUNK, DEMON, ROBOT };
volatile Mode currentMode = CHIPMUNK;
const char* modeName[] = { "CHIPMUNK", "DEMON", "ROBOT" };
const float modePitch[] = { 2.0f, 0.5f, 1.0f };  // robot ignores pitch

// ---------- i2s channels ----------
static i2s_chan_handle_t tx_chan = NULL; // ESP32 -> amp
static i2s_chan_handle_t rx_chan = NULL; // mic -> ESP32

// robot voice: 30 Hz sine table for ring modulation
#define CARRIER_N 256
static int16_t carrier[CARRIER_N];
static float carrierPhase = 0;
const float CARRIER_STEP = 30.0f * CARRIER_N / SAMPLE_RATE;

// INMP441 sends 24-bit audio left-justified in a 32-bit frame.
static inline int16_t micTo16(int32_t raw) {
  return clamp16((raw >> 14) * MIC_GAIN);
}

// MAX98357A accepts 16-bit left-justified in a 32-bit frame.
static inline int32_t toAmp(int16_t s) {
  return ((int32_t)s) << 16;
}

static inline int16_t robotSample(int16_t s) {
  int idx = ((int)carrierPhase) & (CARRIER_N - 1);
  carrierPhase += CARRIER_STEP;
  if (carrierPhase >= CARRIER_N) carrierPhase -= CARRIER_N;
  return (int16_t)(((int32_t)s * carrier[idx]) >> 15);
}

// ---------- buttons (internal pull-ups, active LOW) ----------
struct Button { gpio_num_t pin; uint32_t lastMs; bool lastState; };
Button buttons[3] = {
  { BTN_CHIPMUNK, 0, HIGH }, { BTN_DEMON, 0, HIGH }, { BTN_ROBOT, 0, HIGH }
};
Mode buttonMode[3] = { CHIPMUNK, DEMON, ROBOT };
static uint32_t muteUntil = 0;   // non-blocking pop suppression

void pollButtons() {
  uint32_t now = millis();
  for (int i = 0; i < 3; i++) {
    bool s = digitalRead(buttons[i].pin);
    if (s != buttons[i].lastState && now - buttons[i].lastMs > 50) {
      buttons[i].lastMs = now;
      buttons[i].lastState = s;
      if (s == LOW) {
        currentMode = buttonMode[i];
        shifterSync = true;              // re-sync pitch shifter on switch
        digitalWrite(AMP_SD, LOW);        // mute briefly: no pops
        muteUntil = now + 30;
        Serial.printf("mode -> %s\n", modeName[currentMode]);
      }
    }
  }
  if (muteUntil && (int32_t)(now - muteUntil) >= 0) {
    digitalWrite(AMP_SD, HIGH);
    muteUntil = 0;
  }
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== voice changer ===");
  Serial.println("buttons: 8=chipmunk 9=demon 10=robot");

  pinMode(BTN_CHIPMUNK, INPUT_PULLUP);
  pinMode(BTN_DEMON, INPUT_PULLUP);
  pinMode(BTN_ROBOT, INPUT_PULLUP);
  pinMode(AMP_SD, OUTPUT);
  digitalWrite(AMP_SD, HIGH);

  for (int i = 0; i < CARRIER_N; i++)
    carrier[i] = (int16_t)(32767.0f * sinf(2.0f * M_PI * i / CARRIER_N));
  for (int i = 0; i < GRAIN; i++)       // periodic Hann: exact COLA at 50%
    hannWin[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / GRAIN));

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan));

  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                    I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = I2S_BCLK,
      .ws   = I2S_WS,
      .dout = I2S_DOUT,
      .din  = I2S_DIN,
    },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

  Serial.printf("mode -> %s\n", modeName[currentMode]);
}

// ---------- main loop ----------
static uint32_t lastVuMs = 0;
static int16_t vuPeak = 0;
static int32_t rxTmp[HOP];
static int16_t in16[HOP];
static int16_t out16[HOP];
static int32_t tx32[HOP];

void loop() {
  pollButtons();

  // 1. read exactly HOP samples from the mic
  int got = 0;
  while (got < HOP) {
    size_t br = 0;
    i2s_channel_read(rx_chan, rxTmp, (HOP - got) * 4, &br, portMAX_DELAY);
    int cnt = br / 4;
    for (int i = 0; i < cnt; i++) {
      in16[got + i] = micTo16(rxTmp[i]);
      int16_t a = in16[got + i] >= 0 ? in16[got + i] : -in16[got + i];
      if (a > vuPeak) vuPeak = a;
    }
    got += cnt;
  }

  if (currentMode == ROBOT) {
    // real-time ring modulation, no buffering
    for (int i = 0; i < HOP; i++) out16[i] = robotSample(in16[i]);
  } else {
    // pitch shift via overlap-add
    for (int i = 0; i < HOP; i++) ringBuf[(writePos + i) & RING_M] = in16[i];
    writePos += HOP;
    if (shifterSync) {                       // fresh start after mode switch
      readPos = (float)writePos - (GRAIN * modePitch[currentMode] + HOP);
      for (int i = 0; i < GRAIN + HOP; i++) olaBuf[i] = 0;
      shifterSync = false;
    }
    pitchGrain(modePitch[currentMode], out16);
  }

  // 2. write to the amp
  for (int i = 0; i < HOP; i++) tx32[i] = toAmp(out16[i]);
  size_t bw = 0;
  i2s_channel_write(tx_chan, tx32, sizeof(tx32), &bw, portMAX_DELAY);

  // VU meter: peak input level twice a second
  uint32_t now = millis();
  if (now - lastVuMs > 500) {
    lastVuMs = now;
    int bars = vuPeak / 2048;
    Serial.print("in [");
    for (int b = 0; b < 16; b++) Serial.print(b < bars ? '#' : '-');
    Serial.printf("] %d\n", vuPeak);
    vuPeak = 0;
  }
}
