/**
 * detect.ino - identificacao do hardware da CYD, lida da propria placa.
 *
 * Nao depende de nenhuma biblioteca externa: so o core Arduino-ESP32. A ideia
 * e descobrir, sem supor nada, o que esta soldado nesta placa:
 *
 *   1. chip, flash, PSRAM e heap (o orcamento que o firmware vai ter)
 *   2. varredura I2C nos pares de pino candidatos (touch capacitivo)
 *   3. ID do controlador de tela por SPI bit-bang, testando todos os
 *      alinhamentos de bit dummy (o numero de clocks dummy varia por CI)
 *   4. varredura de backlight, LED RGB e LDR (o usuario confirma olhando)
 *   5. sondagem do XPT2046 (touch resistivo) com leitura continua
 *
 * O passo 5 fica em loop: encostar o dedo na tela move Z1/X/Y, e e isso que
 * prova que o controlador existe e esta ligado nesses pinos.
 */
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <string.h>
#include <esp_chip_info.h>

// -- Conjuntos de pino candidatos para o barramento da tela ---------------
struct SpiSet { const char *name; int sck, mosi, miso, cs, dc, rst; };
static const SpiSet TFT_SETS[] = {
  { "HSPI (CYD tipico): SCK14 MOSI13 MISO12 CS15 DC2", 14, 13, 12, 15, 2, -1 },
  { "VSPI: SCK18 MOSI23 MISO19 CS5 DC2",               18, 23, 19,  5, 2, -1 },
};
static const int NTFT = sizeof(TFT_SETS) / sizeof(TFT_SETS[0]);

// -- Conjuntos de pino candidatos para o XPT2046 --------------------------
struct TouchSet { const char *name; int sck, mosi, miso, cs, irq; };
static const TouchSet TS_SETS[] = {
  { "XPT2046 (CYD tipico): SCK25 MOSI32 MISO39 CS33 IRQ36", 25, 32, 39, 33, 36 },
};
static const int NTS = sizeof(TS_SETS) / sizeof(TS_SETS[0]);

// -- Pares I2C candidatos (touch capacitivo) ------------------------------
struct I2cSet { int sda, scl; };
static const I2cSet I2C_SETS[] = {
  { 21, 22 }, { 33, 32 }, { 27, 26 }, { 4, 5 }, { 15, 2 },
};
static const int NI2C = sizeof(I2C_SETS) / sizeof(I2C_SETS[0]);

// -- Candidatos a backlight (o usuario confirma olhando a tela) -----------
static const int BL_CANDIDATES[] = { 21, 27, 32, 4 };
static const int NBL = sizeof(BL_CANDIDATES) / sizeof(BL_CANDIDATES[0]);

// -- LED RGB e LDR: marcadores caracteristicos da familia CYD -------------
static const int RGB_PINS[3] = { 4, 16, 17 };            // R, G, B (ativo em LOW)
static const char *RGB_NAMES[3] = { "VERMELHO", "VERDE", "AZUL" };
static const int LDR_PIN = 34;

// ============================================================
// SPI bit-bang (modo 0: amostra na subida do clock)
// ============================================================
static int g_sck, g_mosi, g_miso;

static inline void bb_init(int sck, int mosi, int miso) {
  g_sck = sck; g_mosi = mosi; g_miso = miso;
  pinMode(sck, OUTPUT);  digitalWrite(sck, LOW);
  pinMode(mosi, OUTPUT); digitalWrite(mosi, LOW);
  if (miso >= 0) pinMode(miso, INPUT);
}
static inline void bb_write8(uint8_t v) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(g_mosi, (v >> i) & 1);
    delayMicroseconds(2);
    digitalWrite(g_sck, HIGH);
    delayMicroseconds(2);
    digitalWrite(g_sck, LOW);
  }
}
static inline uint8_t bb_read8() {
  uint8_t v = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(g_sck, HIGH);
    delayMicroseconds(2);
    v |= (digitalRead(g_miso) ? 1 : 0) << i;
    digitalWrite(g_sck, LOW);
    delayMicroseconds(2);
  }
  return v;
}
static inline void bb_dummy_clock() {
  digitalWrite(g_sck, HIGH);
  delayMicroseconds(2);
  digitalWrite(g_sck, LOW);
  delayMicroseconds(2);
}
// Le nbits crus, um bit por posicao. Como o numero de clocks dummy varia por
// controlador, guardamos o fluxo bruto e extraimos bytes em cada alinhamento.
static void bb_read_bits(int nbits, uint8_t *bits) {
  for (int i = 0; i < nbits; i++) {
    digitalWrite(g_sck, HIGH);
    delayMicroseconds(2);
    bits[i] = digitalRead(g_miso) ? 1 : 0;
    digitalWrite(g_sck, LOW);
    delayMicroseconds(2);
  }
}
static void bytes_at(const uint8_t *bits, int off, uint8_t *out, int n) {
  for (int b = 0; b < n; b++) {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) v = (uint8_t)((v << 1) | bits[off + b * 8 + i]);
    out[b] = v;
  }
}

// ============================================================
// 1. Chip, flash, PSRAM, heap
// ============================================================
static void report_chip() {
  Serial.println("");
  Serial.println("=== 1. CHIP / MEMORIA ===");
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  const char *model = "desconhecido";
  switch (ci.model) {
    case CHIP_ESP32:   model = "ESP32";    break;
    case CHIP_ESP32S2: model = "ESP32-S2"; break;
    case CHIP_ESP32S3: model = "ESP32-S3"; break;
    case CHIP_ESP32C3: model = "ESP32-C3"; break;
    default: break;
  }
  Serial.printf("modelo            : %s\n", model);
  Serial.printf("chip model (sdk)  : %s\n", ESP.getChipModel());
  Serial.printf("revisao           : %d\n", ESP.getChipRevision());
  Serial.printf("nucleos           : %d\n", ci.cores);
  Serial.printf("cpu freq          : %u MHz\n", (unsigned)getCpuFrequencyMhz());
  Serial.printf("features          : WiFi%s%s\n",
                (ci.features & CHIP_FEATURE_BT)  ? " + BT"  : "",
                (ci.features & CHIP_FEATURE_BLE) ? " + BLE" : "");
  Serial.printf("flash (sdk)       : %u bytes (%u MB)\n",
                (unsigned)ESP.getFlashChipSize(),
                (unsigned)(ESP.getFlashChipSize() / (1024U * 1024U)));
  Serial.printf("flash speed       : %u Hz\n", (unsigned)ESP.getFlashChipSpeed());
  Serial.printf("PSRAM total       : %u bytes  <== 0 significa SEM PSRAM\n",
                (unsigned)ESP.getPsramSize());
  Serial.printf("heap total        : %u bytes\n", (unsigned)ESP.getHeapSize());
  Serial.printf("heap livre        : %u bytes  <== orcamento do LVGL + TLS\n",
                (unsigned)ESP.getFreeHeap());
  Serial.printf("maior bloco heap  : %u bytes\n", (unsigned)ESP.getMaxAllocHeap());
  Serial.printf("sketch size       : %u bytes\n", (unsigned)ESP.getSketchSize());
  Serial.printf("particao app livre: %u bytes\n", (unsigned)ESP.getFreeSketchSpace());
  Serial.printf("MAC               : %s\n", WiFi.macAddress().c_str());
}

// ============================================================
// 2. Varredura I2C (touch capacitivo)
// ============================================================
static const char *i2c_guess(uint8_t addr) {
  switch (addr) {
    case 0x5D: case 0x14: return "GT911 (capacitivo)";
    case 0x15: return "CST816 / CST820 (capacitivo)";
    case 0x38: return "FT6236 / FT6336 (capacitivo)";
    case 0x3B: return "AXS15231B (o da placa original do projeto)";
    case 0x48: return "TSC2007 / ADC";
    case 0x51: return "RTC PCF8563";
    default:   return "?";
  }
}
static void scan_i2c() {
  Serial.println("");
  Serial.println("=== 2. VARREDURA I2C (touch capacitivo) ===");
  bool anyFound = false;
  for (int s = 0; s < NI2C; s++) {
    int sda = I2C_SETS[s].sda, scl = I2C_SETS[s].scl;
    Wire.end();
    if (!Wire.begin(sda, scl, 100000)) {
      Serial.printf("SDA=%-2d SCL=%-2d : nao foi possivel iniciar\n", sda, scl);
      continue;
    }
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        Serial.printf("SDA=%-2d SCL=%-2d : dispositivo em 0x%02X -> %s\n",
                      sda, scl, a, i2c_guess(a));
        found++; anyFound = true;
      }
    }
    if (!found) Serial.printf("SDA=%-2d SCL=%-2d : nada\n", sda, scl);
  }
  Wire.end();
  if (!anyFound)
    Serial.println(">> nenhum dispositivo I2C: o touch desta placa NAO e capacitivo");
}

// ============================================================
// 3. ID do controlador de tela
// ============================================================
static const char *panel_guess(uint8_t a, uint8_t b, uint8_t c) {
  if (a == 0x00 && b == 0x93 && c == 0x41) return "ILI9341 (240x320)";
  if (a == 0x00 && b == 0x93 && c == 0x28) return "ILI9328";
  if (a == 0x85 && b == 0x85 && c == 0x52) return "ST7789 (240x320)";
  if (a == 0x00 && b == 0x77 && c == 0x89) return "ST7789 (240x320)";
  if (a == 0x00 && b == 0x77 && c == 0x96) return "ST7796 (320x480)";
  if (a == 0x00 && b == 0x94 && c == 0x88) return "ILI9488 (320x480)";
  if (a == 0x54 && b == 0x80 && c == 0x66) return "ST7796S (320x480)";
  return "desconhecido";
}
static void read_panel_id(const SpiSet &s) {
  Serial.println("");
  Serial.printf("-- %s\n", s.name);
  if (s.miso < 0) { Serial.println("   sem MISO nesse conjunto, pulando"); return; }

  pinMode(s.cs, OUTPUT); digitalWrite(s.cs, HIGH);
  pinMode(s.dc, OUTPUT); digitalWrite(s.dc, HIGH);
  if (s.rst >= 0) { pinMode(s.rst, OUTPUT); digitalWrite(s.rst, HIGH); }
  bb_init(s.sck, s.mosi, s.miso);

  // Acorda o painel: SLPOUT (0x11) e espera o boot do controlador.
  digitalWrite(s.cs, LOW);
  digitalWrite(s.dc, LOW); bb_write8(0x11);
  digitalWrite(s.cs, HIGH);
  delay(130);

  const uint8_t cmds[] = { 0x04, 0xD3, 0x09, 0x0A };
  const char *names[]  = { "0x04 RDDID", "0xD3 RDDID4", "0x09 RDDST", "0x0A RDDPM" };
  static uint8_t bits[80];
  for (int k = 0; k < 4; k++) {
    digitalWrite(s.cs, LOW);
    digitalWrite(s.dc, LOW);
    bb_write8(cmds[k]);
    digitalWrite(s.dc, HIGH);
    pinMode(s.mosi, INPUT);          // libera a linha; o painel responde no MISO
    bb_read_bits(72, bits);          // 9 bytes crus, sem assumir dummy
    pinMode(s.mosi, OUTPUT);
    digitalWrite(s.cs, HIGH);

    Serial.printf("   %s\n", names[k]);
    for (int off = 0; off <= 8; off++) {
      uint8_t p[4];
      bytes_at(bits, off, p, 4);
      const char *g1 = panel_guess(p[0], p[1], p[2]);
      const char *g2 = panel_guess(p[1], p[2], p[3]);
      Serial.printf("      dummy=%d -> %02X %02X %02X %02X", off, p[0], p[1], p[2], p[3]);
      if (strcmp(g1, "desconhecido") != 0) Serial.printf("  >>> %s", g1);
      if (strcmp(g2, "desconhecido") != 0) Serial.printf("  >>> %s (apos byte dummy)", g2);
      Serial.println("");
    }
  }
  Serial.println("   Se todas as linhas forem FF/00 ou lixo constante, o MISO do");
  Serial.println("   painel nao esta ligado nesta placa e o ID nao e legivel.");
}
static void probe_panels() {
  Serial.println("");
  Serial.println("=== 3. ID DO CONTROLADOR DE TELA ===");
  for (int i = 0; i < NTFT; i++) read_panel_id(TFT_SETS[i]);
}

// ============================================================
// 3b. Round-trip de MADCTL: prova de que o MISO esta alinhado
// ============================================================
// Escreve um valor conhecido em 0x36 (MADCTL) e le de volta com 0x0B
// (RDDMADCTL). O alinhamento de bit em que o byte lido bate com o escrito,
// para DOIS valores diferentes, e o alinhamento verdadeiro do barramento.
// Sem isso, qualquer leitura de ID e chute.
static int madctl_roundtrip(const SpiSet &s, uint8_t val) {
  static uint8_t bits[80];
  digitalWrite(s.cs, LOW);
  digitalWrite(s.dc, LOW); bb_write8(0x36);
  digitalWrite(s.dc, HIGH); bb_write8(val);
  digitalWrite(s.cs, HIGH);
  delay(20);

  digitalWrite(s.cs, LOW);
  digitalWrite(s.dc, LOW); bb_write8(0x0B);
  digitalWrite(s.dc, HIGH);
  pinMode(s.mosi, INPUT);
  bb_read_bits(72, bits);
  pinMode(s.mosi, OUTPUT);
  digitalWrite(s.cs, HIGH);

  int hit = -1;
  Serial.print("   escrito 0x");
  Serial.print(val, HEX);
  Serial.print(" -> lido por alinhamento:");
  for (int off = 0; off <= 8; off++) {
    uint8_t b[1];
    bytes_at(bits, off, b, 1);
    Serial.print(" ");
    Serial.print(off);
    Serial.print(":");
    if (b[0] < 16) Serial.print("0");
    Serial.print(b[0], HEX);
    if (b[0] == val) { Serial.print("*"); if (hit < 0) hit = off; }
  }
  Serial.println("");
  return hit;
}
static void probe_madctl() {
  Serial.println("");
  Serial.println("=== 3b. ROUND-TRIP DE MADCTL (alinhamento do MISO) ===");
  for (int i = 0; i < NTFT; i++) {
    const SpiSet &s = TFT_SETS[i];
    if (s.miso < 0) continue;
    Serial.print("-- ");
    Serial.println(s.name);
    pinMode(s.cs, OUTPUT); digitalWrite(s.cs, HIGH);
    pinMode(s.dc, OUTPUT); digitalWrite(s.dc, HIGH);
    bb_init(s.sck, s.mosi, s.miso);
    int a = madctl_roundtrip(s, 0x48);
    int b = madctl_roundtrip(s, 0x28);
    int c = madctl_roundtrip(s, 0x00);
    if (a >= 0 && a == b && b == c) {
      Serial.print("   >>> MISO LIGADO e alinhado em dummy=");
      Serial.print(a);
      Serial.println(" bits. As leituras de ID nesse alinhamento sao validas.");
      // Le os IDs individuais (0xDA/0xDB/0xDC) no alinhamento comprovado.
      static uint8_t bits[80];
      const uint8_t idc[3] = { 0xDA, 0xDB, 0xDC };
      Serial.print("   RDID1/2/3 (0xDA/DB/DC) =");
      for (int k = 0; k < 3; k++) {
        digitalWrite(s.cs, LOW);
        digitalWrite(s.dc, LOW); bb_write8(idc[k]);
        digitalWrite(s.dc, HIGH);
        pinMode(s.mosi, INPUT);
        bb_read_bits(72, bits);
        pinMode(s.mosi, OUTPUT);
        digitalWrite(s.cs, HIGH);
        uint8_t v[1];
        bytes_at(bits, a, v, 1);
        Serial.print(" 0x");
        if (v[0] < 16) Serial.print("0");
        Serial.print(v[0], HEX);
      }
      Serial.println("");
      Serial.println("   (ILI9341 = 0x00 0x93 0x41 / ST7789 = 0x85 0x85 0x52)");
    } else {
      Serial.println("   >>> o valor escrito NAO volta em nenhum alinhamento:");
      Serial.println("   >>> o MISO do painel nao esta ligado nesta placa.");
      Serial.println("   >>> a identificacao tera de ser visual (bring-up).");
    }
  }
}

// ============================================================
// 4. Backlight, LED RGB e LDR
// ============================================================
static void sweep_backlight() {
  Serial.println("");
  Serial.println("=== 4. BACKLIGHT (olhe a tela) ===");
  for (int i = 0; i < NBL; i++) {
    int p = BL_CANDIDATES[i];
    Serial.printf("piscando GPIO%d por 3s...\n", p);
    Serial.flush();
    pinMode(p, OUTPUT);
    for (int k = 0; k < 6; k++) { digitalWrite(p, k & 1); delay(500); }
    digitalWrite(p, HIGH);
    delay(400);
  }
  Serial.println(">> o GPIO que fez a tela piscar/acender e o TFT_BL");
}
static void sweep_rgb() {
  Serial.println("");
  Serial.println("=== 4b. LED RGB (olhe o LED perto do USB) ===");
  for (int i = 0; i < 3; i++) { pinMode(RGB_PINS[i], OUTPUT); digitalWrite(RGB_PINS[i], HIGH); }
  for (int i = 0; i < 3; i++) {
    Serial.printf("GPIO%-2d deveria acender %s por 2s\n", RGB_PINS[i], RGB_NAMES[i]);
    Serial.flush();
    digitalWrite(RGB_PINS[i], LOW);  delay(2000);
    digitalWrite(RGB_PINS[i], HIGH); delay(300);
  }
}
static void read_ldr() {
  Serial.println("");
  Serial.println("=== 4c. LDR (sensor de luz) ===");
  pinMode(LDR_PIN, INPUT);
  Serial.printf("GPIO%d bruto: ", LDR_PIN);
  for (int i = 0; i < 5; i++) { Serial.printf("%d ", analogRead(LDR_PIN)); delay(120); }
  Serial.println("");
  Serial.println("(tapar o sensor com o dedo deve mudar esse valor)");
}

// ============================================================
// 5. XPT2046 - leitura continua (toque a tela)
// ============================================================
static const TouchSet *g_ts = nullptr;

static uint16_t xpt_read(uint8_t cmd) {
  digitalWrite(g_ts->cs, LOW);
  bb_write8(cmd);
  bb_dummy_clock();              // 12 bits comecam apos 1 clock dummy
  uint8_t hi = bb_read8();
  uint8_t lo = bb_read8();
  digitalWrite(g_ts->cs, HIGH);
  return (uint16_t)(((uint16_t)hi << 8 | lo) >> 4);
}
static void setup_touch() {
  Serial.println("");
  Serial.println("=== 5. XPT2046 (touch resistivo) ===");
  if (NTS == 0) return;
  g_ts = &TS_SETS[0];
  Serial.printf("-- %s\n", g_ts->name);
  pinMode(g_ts->cs, OUTPUT); digitalWrite(g_ts->cs, HIGH);
  if (g_ts->irq >= 0) pinMode(g_ts->irq, INPUT);
  bb_init(g_ts->sck, g_ts->mosi, g_ts->miso);
  Serial.println("ENCOSTE O DEDO NA TELA. Se Z1 subir e X/Y virarem numeros");
  Serial.println("plausiveis (nem 0 nem 4095), o XPT2046 esta nesses pinos.");
  Serial.println("IRQ vai a 0 durante o toque.");
  Serial.println("");
}
static void poll_touch() {
  if (!g_ts) return;
  uint16_t z1 = xpt_read(0xB1);
  uint16_t z2 = xpt_read(0xC1);
  uint16_t x  = xpt_read(0xD1);
  uint16_t y  = xpt_read(0x91);
  int irq = (g_ts->irq >= 0) ? digitalRead(g_ts->irq) : -1;
  Serial.printf("Z1=%4u Z2=%4u X=%4u Y=%4u IRQ=%d%s\n",
                z1, z2, x, y, irq, (z1 > 100) ? "   <== TOQUE" : "");
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("");
  Serial.println("################################################");
  Serial.println("#  DETECCAO DE HARDWARE - CYD                  #");
  Serial.println("################################################");
  report_chip();
  scan_i2c();
  probe_panels();
  probe_madctl();
  sweep_backlight();
  sweep_rgb();
  read_ldr();
  setup_touch();
}

void loop() {
  poll_touch();
  delay(250);
}
