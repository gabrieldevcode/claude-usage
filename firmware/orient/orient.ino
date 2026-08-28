/**
 * orient.ino - confirmacao de orientacao, sem depender de vocabulario.
 *
 * "Esquerda" e "direita" dependem de como a placa esta na mao de quem olha,
 * e isso ja gerou confusao. Esta tela resolve por desenho: uma seta grande
 * apontando para uma borda especifica com a pergunta escrita ao lado, e uma
 * letra em cada canto para o canto problematico do touch poder ser nomeado
 * sem ambiguidade.
 *
 * Mesma configuracao validada no bring-up: ST7789 320x240, rotation 3.
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

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX     *gfx = nullptr;

// Seta cheia apontando para a esquerda, encostada na borda esquerda.
static void arrow_left(int tipX, int cy, int len, int halfH, uint16_t col) {
  gfx->fillTriangle(tipX, cy, tipX + 26, cy - halfH, tipX + 26, cy + halfH, col);
  gfx->fillRect(tipX + 26, cy - halfH / 2, len - 26, halfH, col);
}

static void corner_letter(int x, int y, const char *s, uint16_t col) {
  gfx->fillCircle(x, y, 15, col);
  gfx->setTextColor(RGB565_BLACK);
  gfx->setTextSize(2);
  gfx->setCursor(x - 5, y - 7);
  gfx->print(s);
}

void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO, HSPI);
  gfx = new Arduino_ST7789(bus, TFT_RST, TFT_ROTATION, false, 240, 320);
  if (!gfx->begin(40000000UL)) {
    Serial.println("FATAL: gfx->begin() falhou");
    while (1) delay(1000);
  }

  int w = gfx->width(), h = gfx->height();
  Serial.print("orient: ");
  Serial.print(w);
  Serial.print("x");
  Serial.print(h);
  Serial.print(" rotation=");
  Serial.println(TFT_ROTATION);

  gfx->fillScreen(RGB565_BLACK);
  gfx->drawRect(0, 0, w, h, RGB565_DARKGREY);

  // A pergunta, escrita para ser lida com a tela na posicao certa.
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(96, 96);
  gfx->print("A SETA");
  gfx->setCursor(96, 116);
  gfx->print("APONTA P/ O");
  gfx->setCursor(96, 136);
  gfx->print("CONECTOR USB?");

  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_YELLOW);
  gfx->setCursor(96, 162);
  gfx->print("se este texto esta de cabeca para baixo,");
  gfx->setCursor(96, 174);
  gfx->print("a rotacao esta errada - me avise");

  // Seta apontando para a borda esquerda da tela (nesta rotacao).
  arrow_left(6, h / 2, 82, 30, RGB565_ORANGE);

  // Cantos nomeados: A B em cima, C D embaixo.
  corner_letter(22, 22, "A", RGB565_GREEN);
  corner_letter(w - 22, 22, "B", RGB565_GREEN);
  corner_letter(22, h - 22, "C", RGB565_GREEN);
  corner_letter(w - 22, h - 22, "D", RGB565_RED);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(96, 60);
  gfx->print("cantos marcados A B C D");
  gfx->setCursor(96, 72);
  gfx->print("qual deles nao responde ao toque?");
}

void loop() { delay(1000); }
