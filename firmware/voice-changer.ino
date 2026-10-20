// voice-changer.ino — Week 3: Music Week (Half Life)
// Talk into the mic, hear yourself back transformed.
//   Button 1 -> CHIPMUNK (2x pitch up)   Button 2 -> DEMON (0.5x pitch down)
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
// Wiring (Supermini pin numbers — verify against your specific board!):
//   INMP441: VDD->3V3, GND->GND, SD->GPIO6, WS->GPIO5, SCK->GPIO4, L/R->GND
//   MAX98357A: VIN->5V (or 3V3, quieter), GND->GND, DIN->GPIO7,
//              BCLK->GPIO4, LRCK->GPIO5, SD->GPIO11 (pull high = enable)
//   Buttons: one leg -> GPIO8/9/10, other leg -> GND (uses internal pull-ups,
//            no resistors needed). Avoid GPIO0 (boot) and GPIO19/20 (USB).

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
#define CHUNK         1600               // 100 ms of audio per chunk
#define MIC_GAIN      3                  // digital gain, tune to taste

// ---------- modes ----------
enum Mode { CHIPMUNK, DEMON, ROBOT };
volatile Mode currentMode = CHIPMUNK;
const char* modeName[] = { "CHIPMUNK", "DEMON", "ROBOT" };

// ---------- i2s channels ----------
static i2s_chan_handle_t tx_chan = NULL; // ESP32 -> amp
static i2s_chan_handle_t rx_chan = NULL; // mic -> ESP32

// ---------- buffers ----------
static int32_t rxBuf32[CHUNK];           // raw 32-bit from mic
static int16_t workBuf[CHUNK * 2];       // 16-bit working space (demon needs 2x)
static int32_t txBuf32[CHUNK * 2];       // 32-bit to amp

// robot voice: 30 Hz sine table for ring modulation (classic Dalek-ish effect)
#define CARRIER_N 256
static int16_t carrier[CARRIER_N];
static float carrierPhase = 0;
const float CARRIER_STEP = 30.0f * CARRIER_N / SAMPLE_RATE;

static inline int16_t clamp16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

// INMP441 sends 24-bit audio left-justified in a 32-bit frame.
// Shift down to 16-bit and apply a little gain.
static inline int16_t micTo16(int32_t raw) {
  int32_t s = (raw >> 14) * MIC_GAIN;
  return clamp16(s);
}

// MAX98357A accepts 16-bit left-justified in a 32-bit frame.
static inline int32_t toAmp(int16_t s) {
  return ((int32_t)s) << 16;
}

// CHIPMUNK: play the chunk back at 2x speed (every 2nd sample).
// Pitch AND speed double — the classic tape-trick voice changer.
// Returns number of output samples (CHUNK/2).
int chipmunk(int16_t* in, int16_t* out, int n) {
  int o = 0;
  for (int i = 0; i < n; i += 2) out[o++] = in[i];
  return o;
}

// DEMON: play the chunk back at half speed (each sample twice).
// Returns number of output samples (CHUNK*2). The mic keeps recording
// while we play slow, so the caller drains the extra input afterwards.
int demon(int16_t* in, int16_t* out, int n) {
  int o = 0;
  for (int i = 0; i < n; i++) { out[o++] = in[i]; out[o++] = in[i]; }
  return o;
}

// ROBOT: ring modulation — multiply the voice by a 30 Hz sine wave.
// Runs sample-by-sample in real time, no extra latency.
int16_t robotSample(int16_t s) {
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
        // pop suppression: mute the amp briefly across the switch
        digitalWrite(AMP_SD, LOW);
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
  digitalWrite(AMP_SD, HIGH);  // enable amp

  for (int i = 0; i < CARRIER_N; i++)
    carrier[i] = (int16_t)(32767.0f * sinf(2.0f * M_PI * i / CARRIER_N));

  // one I2S port, full duplex: TX channel -> amp, RX channel <- mic
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

void loop() {
  pollButtons();

  // 1. read one chunk from the mic
  size_t bytesRead = 0;
  i2s_channel_read(rx_chan, rxBuf32, sizeof(rxBuf32), &bytesRead, portMAX_DELAY);
  int n = bytesRead / 4;
  for (int i = 0; i < n; i++) {
    workBuf[i] = micTo16(rxBuf32[i]);
    int16_t a = workBuf[i] >= 0 ? workBuf[i] : -workBuf[i];
    if (a > vuPeak) vuPeak = a;
  }

  // 2. apply the effect
  int outN = 0;
  if (currentMode == ROBOT) {
    for (int i = 0; i < n; i++) workBuf[i] = robotSample(workBuf[i]);
    outN = n;
  } else if (currentMode == CHIPMUNK) {
    outN = chipmunk(workBuf, workBuf, n);          // in-place, shrinks
  } else {
    static int16_t stretched[CHUNK * 2];
    outN = demon(workBuf, stretched, n);
    for (int i = 0; i < outN; i++) workBuf[i] = stretched[i];
  }

  // 3. write to the amp
  for (int i = 0; i < outN; i++) txBuf32[i] = toAmp(workBuf[i]);
  size_t bytesWritten = 0;
  i2s_channel_write(tx_chan, txBuf32, outN * 4, &bytesWritten, portMAX_DELAY);

  // VU meter on serial: peak input level twice a second.
  // Silent mic reads ~0-500, normal speech 3k-15k, clipping at 32767.
  uint32_t now = millis();
  if (now - lastVuMs > 500) {
    lastVuMs = now;
    int bars = vuPeak / 2048;                 // 0..16
    Serial.print("in [");
    for (int b = 0; b < 16; b++) Serial.print(b < bars ? '#' : '-');
    Serial.printf("] %d\n", vuPeak);
    vuPeak = 0;
  }

  // 4. demon mode plays slower than the mic records: drain the backlog
  //    so the next chunk starts fresh instead of lagging further behind.
  if (currentMode == DEMON) {
    size_t extra = (outN > n ? outN - n : 0) * 4;
    static int32_t drain[256];
    while (extra > 0) {
      size_t got = 0, want = extra > sizeof(drain) ? sizeof(drain) : extra;
      i2s_channel_read(rx_chan, drain, want, &got, 0);
      if (got == 0) break;
      extra -= got;
    }
  }
}
