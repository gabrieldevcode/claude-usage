/**
 * touchmap.ino - mede a zona morta do touch resistivo desta placa.
 *
 * No bring-up ficou claro que o canto inferior direito responde mal, mas
 * "mal" nao e um numero. Sem saber o tamanho da regiao, qualquer correcao de
 * layout seria chute: o botao de confirmar do teclado LVGL cai exatamente ali.
 *
 * A tela vira uma grade de celulas. Cada celula tocada fica verde. Arrastando
 * o dedo pela tela inteira, o que sobrar preto e a regiao que o painel nao
 * enxerga - e a lista dessas celulas sai pelo serial, em pixels.
 *
 * Usa a mesma configuracao ja validada: ST7789 320x240 rotation 3, XPT2046
 * com a calibracao medida em firmware/bringup_cyd.
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#define TFT_SCK   14
#define TFT_MOSI  13
#define TFT_MISO  12
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST   -1
#define TFT_BL    21
#define TFT_ROTATION 3

#define TS_SCK    25
#define TS_MOSI   32
#define TS_MISO   39
#define TS_CS     33
#define TS_Z_MIN  250

// Calibracao medida (ver docs/HARDWARE-CYD.md)
#define TOUCH_SWAP_XY   1
#define TOUCH_RAW_MIN_X 3867
#define TOUCH_RAW_MAX_X 112
#define TOUCH_RAW_MIN_Y 3771
#define TOUCH_RAW_MAX_Y 358

#define SCR_W 320
#define SCR_H 240
#define CELL  20                 // celulas de 20 px: 16 colunas x 12 linhas
#define COLS  (SCR_W / CELL)
#define ROWS  (SCR_H / CELL)

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX     *gfx = nullptr;
static bool hit[ROWS][COLS];
static uint32_t lastReport = 0;

// ---- XPT2046 bit-bang ----
static void ts_write8(uint8_t v) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(TS_MOSI, (v >> i) & 1);
    delayMicroseconds(2);
    digitalWrite(TS_SCK, HIGH); delayMicroseconds(2);
    digitalWrite(TS_SCK, LOW);
  }
}
static uint16_t ts_read12(uint8_t cmd) {
  digitalWrite(TS_CS, LOW);
  ts_write8(cmd);
  digitalWrite(TS_SCK, HIGH); delayMicroseconds(2);
  digitalWrite(TS_SCK, LOW);  delayMicroseconds(2);
  uint16_t v = 0;
  for (int i = 0; i < 12; i++) {
    digitalWrite(TS_SCK, HIGH); delayMicroseconds(2);
    v = (uint16_t)((v << 1) | (digitalRead(TS_MISO) ? 1 : 0));
    digitalWrite(TS_SCK, LOW);  delayMicroseconds(2);
  }
  digitalWrite(TS_CS, HIGH);
  return v;
}
static uint16_t ts_median(uint8_t cmd) {
  uint16_t s[7];
  for (int i = 0; i < 7; i++) s[i] = ts_read12(cmd);
  for (int i = 1; i < 7; i++) {
    uint16_t k = s[i]; int j = i - 1;
    while (j >= 0 && s[j] > k) { s[j + 1] = s[j]; j--; }
    s[j + 1] = k;
  }
  return s[3];
}
static long ts_map(long v, long lo, long hi, long size) {
  long r = (v - lo) * (size - 1) / (hi - lo);
  if (r < 0) r = 0;
  if (r > size - 1) r = size - 1;
  return r;
}

static void draw_grid() {
  gfx->fillScreen(RGB565_BLACK);
  for (int c = 0; c <= COLS; c++) gfx->drawFastVLine(c * CELL, 0, SCR_H, RGB565_NAVY);
  for (int r = 0; r <= ROWS; r++) gfx->drawFastHLine(0, r * CELL, SCR_W, RGB565_NAVY);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(6, 4);
  gfx->print("ARRASTE O DEDO POR TODA A TELA");
  gfx->setCursor(6, 16);
  gfx->print("o que ficar preto e zona morta");
}

static void report() {
  int dead = 0, total = ROWS * COLS;
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (!hit[r][c]) dead++;
  Serial.println("");
  Serial.printf("=== MAPA (celula de %d px) - %d de %d sem resposta ===\n", CELL, dead, total);
  Serial.println("     col:  0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15");
  for (int r = 0; r < ROWS; r++) {
    Serial.printf("lin %2d (y=%3d..%3d) ", r, r * CELL, r * CELL + CELL - 1);
    for (int c = 0; c < COLS; c++) Serial.print(hit[r][c] ? "  #  " : "  .  ");
    Serial.println("");
  }
  // Retangulo util: maior area onde todas as celulas responderam nas bordas.
  int maxX = 0, maxY = 0;
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (hit[r][c]) {
        if (c * CELL + CELL > maxX) maxX = c * CELL + CELL;
        if (r * CELL + CELL > maxY) maxY = r * CELL + CELL;
      }
  Serial.printf("maior x com resposta: %d de %d  (margem morta a direita: %d px)\n",
                maxX, SCR_W, SCR_W - maxX);
  Serial.printf("maior y com resposta: %d de %d  (margem morta embaixo:  %d px)\n",
                maxY, SCR_H, SCR_H - maxY);
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("");
  Serial.println("=== MAPA DE TOQUE ===");

  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  pinMode(TS_SCK, OUTPUT);  digitalWrite(TS_SCK, LOW);
  pinMode(TS_MOSI, OUTPUT); digitalWrite(TS_MOSI, LOW);
  pinMode(TS_MISO, INPUT);
  pinMode(TS_CS, OUTPUT);   digitalWrite(TS_CS, HIGH);

  bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO, HSPI);
  gfx = new Arduino_ST7789(bus, TFT_RST, TFT_ROTATION, false, 240, 320);
  if (!gfx->begin(40000000UL)) { Serial.println("FATAL display"); while (1) delay(1000); }

  memset(hit, 0, sizeof(hit));
  draw_grid();
  Serial.println("Arraste o dedo cobrindo a tela toda, com atencao as bordas.");
  Serial.println("O mapa e reimpresso a cada 5 s.");
}

void loop() {
  if (ts_median(0xB1) > TS_Z_MIN) {
    uint16_t rx = ts_median(0xD1), ry = ts_median(0x91);
#if TOUCH_SWAP_XY
    long cx = ry, cy = rx;
#else
    long cx = rx, cy = ry;
#endif
    int px = (int)ts_map(cx, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X, SCR_W);
    int py = (int)ts_map(cy, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y, SCR_H);
    int c = px / CELL, r = py / CELL;
    if (c >= 0 && c < COLS && r >= 0 && r < ROWS && !hit[r][c]) {
      hit[r][c] = true;
      gfx->fillRect(c * CELL + 1, r * CELL + 1, CELL - 1, CELL - 1, RGB565_DARKGREEN);
    }
  }
  if (millis() - lastReport > 5000) { lastReport = millis(); report(); }
  delay(20);
}
