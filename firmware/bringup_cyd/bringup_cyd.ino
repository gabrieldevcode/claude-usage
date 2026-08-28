/**
 * bringup_cyd.ino - bring-up validado da CYD (ESP32-2432S028).
 *
 * O que ja esta provado no hardware (ver docs/HARDWARE-CYD.md):
 *   - painel no HSPI: SCK=14 MOSI=13 MISO=12 CS=15 DC=2, backlight GPIO21
 *   - round-trip de MADCTL funciona (escreve 0x36, le de volta em 0x0B)
 *   - RDID1/2/3 = 0x81 0x81 0xB3 (ID de fabricante, nao padronizado)
 *   - a etiqueta da caixa diz "2.8 TFT / 240 RGB / 7789" -> ST7789
 *   - resolucao 320x240 em paisagem: a moldura de 1 px fecha nos 4 lados
 *   - nenhum dispositivo I2C -> touch resistivo XPT2046, confirmado por
 *     toque real (Z1 sobe de 0 para acima de 200 com o dedo na tela)
 *
 * Este sketch fecha a peca que falta: a CALIBRACAO do touch. Resistivo nao
 * tem coordenada absoluta - devolve tensoes cruas, e o mapeamento para
 * pixels tem de sair de uma medicao. O sketch mostra quatro alvos, um por
 * vez, guarda o valor cru de cada um e imprime as constantes prontas para
 * colar no config.h. Depois entra em verificacao, com um ponto seguindo o
 * dedo, que e como se confere se a conta ficou certa.
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>

// -- Painel (pinos comprovados) -------------------------------------------
#define TFT_SCK   14
#define TFT_MOSI  13
#define TFT_MISO  12
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST   -1
#define TFT_BL    21

// rotation 3 = paisagem com o USB a esquerda, mesma convencao do projeto
// original. Com rotation 1 o USB fica a direita (confirmado na placa).
#define TFT_ROTATION 3

// -- Touch resistivo XPT2046 (barramento proprio, bit-bang) ---------------
#define TS_SCK    25
#define TS_MOSI   32
#define TS_MISO   39
#define TS_CS     33
#define TS_IRQ    36
#define TS_Z_MIN  250        // limiar de pressao para considerar toque

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX     *gfx = nullptr;

// ============================================================
// XPT2046 em SPI de software
// ============================================================
static void ts_pulse() {
  digitalWrite(TS_SCK, HIGH); delayMicroseconds(2);
  digitalWrite(TS_SCK, LOW);  delayMicroseconds(2);
}
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
  ts_pulse();
  uint16_t v = 0;
  for (int i = 0; i < 12; i++) {
    digitalWrite(TS_SCK, HIGH); delayMicroseconds(2);
    v = (uint16_t)((v << 1) | (digitalRead(TS_MISO) ? 1 : 0));
    digitalWrite(TS_SCK, LOW);  delayMicroseconds(2);
  }
  digitalWrite(TS_CS, HIGH);
  return v;
}
static void ts_begin() {
  pinMode(TS_SCK, OUTPUT);  digitalWrite(TS_SCK, LOW);
  pinMode(TS_MOSI, OUTPUT); digitalWrite(TS_MOSI, LOW);
  pinMode(TS_MISO, INPUT);
  pinMode(TS_CS, OUTPUT);   digitalWrite(TS_CS, HIGH);
  pinMode(TS_IRQ, INPUT);
}
// Mediana de 7 leituras: o resistivo e ruidoso e um valor unico mente.
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
static bool ts_pressed() { return ts_median(0xB1) > TS_Z_MIN; }

// ============================================================
// Calibracao guiada
// ============================================================
#define NPTS 4
static int      g_step = 0;                 // 0..NPTS-1 calibrando, NPTS = pronto
static uint16_t g_rawX[NPTS], g_rawY[NPTS];
static int      g_margin = 24;

// Constantes resultantes
static bool g_swap = false;                 // o cru X alimenta a tela Y?
static int  g_minX, g_maxX, g_minY, g_maxY;
static bool g_ready = false;

static void draw_target(int i) {
  int w = gfx->width(), h = gfx->height();
  const int M = g_margin;
  const int P[NPTS][2] = { {M, M}, {w - M, M}, {M, h - M}, {w - M, h - M} };

  gfx->fillScreen(RGB565_BLACK);
  gfx->drawRect(0, 0, w, h, RGB565_DARKGREY);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(60, h / 2 - 18);
  gfx->print("CALIBRACAO DO TOUCH");
  gfx->setCursor(60, h / 2 - 4);
  gfx->print("toque no centro da mira amarela");
  gfx->setCursor(60, h / 2 + 10);
  gfx->print("ponto ");
  gfx->print(i + 1);
  gfx->print(" de ");
  gfx->print(NPTS);

  int x = P[i][0], y = P[i][1];
  gfx->drawCircle(x, y, 10, RGB565_YELLOW);
  gfx->drawCircle(x, y, 3, RGB565_YELLOW);
  gfx->drawLine(x - 16, y, x + 16, y, RGB565_YELLOW);
  gfx->drawLine(x, y - 16, x, y + 16, RGB565_YELLOW);

  Serial.print("alvo ");
  Serial.print(i + 1);
  Serial.print("/");
  Serial.print(NPTS);
  Serial.print(" em tela (");
  Serial.print(x);
  Serial.print(",");
  Serial.print(y);
  Serial.println(") - toque agora");
}

static void finish_calibration() {
  // Qual eixo cru acompanha a tela X? Comparamos os alvos da esquerda (0,2)
  // com os da direita (1,3) e vemos qual dos dois crus se move mais.
  long dRawX = ((long)g_rawX[1] + g_rawX[3]) / 2 - ((long)g_rawX[0] + g_rawX[2]) / 2;
  long dRawY = ((long)g_rawY[1] + g_rawY[3]) / 2 - ((long)g_rawY[0] + g_rawY[2]) / 2;
  g_swap = (labs(dRawY) > labs(dRawX));

  const uint16_t *sx = g_swap ? g_rawY : g_rawX;   // cru que vira tela X
  const uint16_t *sy = g_swap ? g_rawX : g_rawY;   // cru que vira tela Y

  long leftRaw   = ((long)sx[0] + sx[2]) / 2;      // alvos da esquerda
  long rightRaw  = ((long)sx[1] + sx[3]) / 2;      // alvos da direita
  long topRaw    = ((long)sy[0] + sy[1]) / 2;      // alvos de cima
  long bottomRaw = ((long)sy[2] + sy[3]) / 2;      // alvos de baixo

  int w = gfx->width(), h = gfx->height();
  int m = g_margin;

  // Extrapola do centro dos alvos ate as bordas reais da tela.
  double spanX = (double)(rightRaw - leftRaw) / (double)(w - 2 * m);
  double spanY = (double)(bottomRaw - topRaw) / (double)(h - 2 * m);
  g_minX = (int)lround(leftRaw - spanX * m);
  g_maxX = (int)lround(rightRaw + spanX * m);
  g_minY = (int)lround(topRaw - spanY * m);
  g_maxY = (int)lround(bottomRaw + spanY * m);
  g_ready = true;

  Serial.println("");
  Serial.println("=== CONSTANTES DE CALIBRACAO (colar no config.h) ===");
  Serial.print("#define TOUCH_SWAP_XY   ");
  Serial.println(g_swap ? 1 : 0);
  Serial.print("#define TOUCH_RAW_MIN_X ");
  Serial.println(g_minX);
  Serial.print("#define TOUCH_RAW_MAX_X ");
  Serial.println(g_maxX);
  Serial.print("#define TOUCH_RAW_MIN_Y ");
  Serial.println(g_minY);
  Serial.print("#define TOUCH_RAW_MAX_Y ");
  Serial.println(g_maxY);
  Serial.println("=== fim ===");
  Serial.println("");
  Serial.println("Verificacao: arraste o dedo. O ponto vermelho tem de ficar");
  Serial.println("embaixo do dedo, inclusive nos 4 cantos.");

  gfx->fillScreen(RGB565_BLACK);
  gfx->drawRect(0, 0, w, h, RGB565_DARKGREY);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(8, 8);
  gfx->print("VERIFICACAO - arraste o dedo pela tela");
  gfx->setCursor(8, 20);
  gfx->print("o ponto deve ficar sob o dedo, ate nas bordas");
  for (int x = 40; x < w; x += 40) gfx->drawLine(x, 34, x, h - 2, RGB565_NAVY);
  for (int y = 40; y < h; y += 40) gfx->drawLine(2, y, w - 2, y, RGB565_NAVY);
}

static void map_touch(uint16_t rx, uint16_t ry, int *px, int *py) {
  long cx = g_swap ? ry : rx;
  long cy = g_swap ? rx : ry;
  int w = gfx->width(), h = gfx->height();
  long x = (cx - g_minX) * (w - 1) / (g_maxX - g_minX);
  long y = (cy - g_minY) * (h - 1) / (g_maxY - g_minY);
  if (x < 0) x = 0;
  if (x > w - 1) x = w - 1;
  if (y < 0) y = 0;
  if (y > h - 1) y = h - 1;
  *px = (int)x;
  *py = (int)y;
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("");
  Serial.println("=== BRING-UP CYD: ST7789 + calibracao do XPT2046 ===");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  ts_begin();

  bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO, HSPI);
  gfx = new Arduino_ST7789(bus, TFT_RST, TFT_ROTATION, false, 240, 320);
  if (!gfx->begin(40000000UL)) {
    Serial.println("FATAL: gfx->begin() falhou");
    while (1) delay(1000);
  }
  Serial.print("display ");
  Serial.print(gfx->width());
  Serial.print("x");
  Serial.print(gfx->height());
  Serial.print("  rotation=");
  Serial.println(TFT_ROTATION);

  draw_target(0);
}

void loop() {
  static bool was = false;
  static uint32_t downAt = 0;

  if (g_step < NPTS) {
    bool now = ts_pressed();
    if (now && !was) downAt = millis();
    if (now) {
      g_rawX[g_step] = ts_median(0xD1);
      g_rawY[g_step] = ts_median(0x91);
    }
    // Registra na SOLTURA, com o toque tendo durado o bastante: pega o valor
    // estavel do fim do toque em vez do transiente da descida.
    if (!now && was && millis() - downAt > 120) {
      Serial.print("  cru X=");
      Serial.print(g_rawX[g_step]);
      Serial.print(" Y=");
      Serial.println(g_rawY[g_step]);
      g_step++;
      delay(350);
      if (g_step < NPTS) draw_target(g_step);
      else               finish_calibration();
    }
    was = now;
    delay(20);
    return;
  }

  if (!g_ready) return;
  if (ts_pressed()) {
    uint16_t rx = ts_median(0xD1);
    uint16_t ry = ts_median(0x91);
    int px, py;
    map_touch(rx, ry, &px, &py);
    gfx->fillCircle(px, py, 3, RGB565_RED);
    Serial.print("cru X=");
    Serial.print(rx);
    Serial.print(" Y=");
    Serial.print(ry);
    Serial.print("  -> tela ");
    Serial.print(px);
    Serial.print(",");
    Serial.println(py);
  }
  delay(30);
}
