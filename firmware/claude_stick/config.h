#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// Claude Usage Stick — CYD ESP32-2432S028 (ESP32, ST7789 SPI)
// Tudo aqui foi medido na placa: ver docs/HARDWARE-CYD.md e os logs
// brutos em docs/logs/.
// ============================================================

// ── Firmware ─────────────────────────────────────────────
#define FW_VERSION              "2.3"

// ── Display SPI (ST7789) ─────────────────────────────────
// Barramento provado por round-trip de MADCTL: escreve 0x36, le de volta em
// 0x0B. So o HSPI responde; no VSPI o MISO volta 0xFF (nao esta ligado).
#define TFT_SCK   14
#define TFT_MOSI  13
#define TFT_MISO  12
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST   -1        // sem reset por GPIO nesta placa
#define TFT_BL    21        // backlight, HIGH = aceso (PWM para o brilho)

// rotation 3 = paisagem com o USB a esquerda, mesma convencao do projeto
// original. Confirmado na placa com uma seta desenhada na tela.
#define TFT_ROTATION   3
#define PANEL_WIDTH    240  // nativo do ST7789 (retrato)
#define PANEL_HEIGHT   320

#define SCREEN_WIDTH   320  // depois da rotacao (paisagem)
#define SCREEN_HEIGHT  240
#define SPI_FREQ       40000000UL

// Buffer parcial do LVGL, em linhas. Esta placa NAO tem PSRAM: o maior bloco
// contiguo alocavel e de ~110 KB, e um framebuffer cheio (320x240x2) pede
// 153 KB. Sao dois buffers de 40 linhas = 2 x 25.600 = 50 KB.
#define LVGL_BUF_LINES 40

// ── Touch resistivo XPT2046 (barramento SPI proprio) ─────
// A varredura I2C nao achou nada: esta placa nao tem touch capacitivo.
#define TOUCH_SCK   25
#define TOUCH_MOSI  32
#define TOUCH_MISO  39
#define TOUCH_CS    33
#define TOUCH_IRQ   36
#define TOUCH_Z_MIN 250     // limiar de pressao para considerar toque

// Calibracao medida com quatro alvos (firmware/bringup_cyd). Resistivo nao
// devolve coordenada, devolve tensao — estes numeros sao a medicao desta
// placa e nao valem para outra unidade.
//   MIN > MAX de proposito: os dois eixos sao invertidos, e a conta linear
//   do driver lida com isso sozinha.
#define TOUCH_SWAP_XY   1   // o cru Y alimenta a tela X
#define TOUCH_RAW_MIN_X 3867
#define TOUCH_RAW_MAX_X 112
#define TOUCH_RAW_MIN_Y 3771
#define TOUCH_RAW_MAX_Y 358

// ── Polling ──────────────────────────────────────────────
#define DEFAULT_POLL_SEC        120
#define MIN_POLL_SEC            30
#define MAX_POLL_SEC            300
#define STATUS_POLL_SEC         300      // status.claude.com a cada 5 min

// ── Segurança (PIN + AES-256-GCM) ────────────────────────
#define PIN_LEN                 4
#define MAX_PIN_ATTEMPTS        10
#define LOCKOUT_BASE_SEC        60       // dobra a cada falha
#define KDF_ROUNDS              10000

// ── Rede / API Claude ────────────────────────────────────
#define WIFI_CONNECT_TIMEOUT_MS 8000
#define API_TIMEOUT_MS          15000
#define MESSAGES_ENDPOINT       "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION       "2023-06-01"
#define PROBE_MODEL             "claude-haiku-4-5-20251001"
// status.anthropic.com redireciona para cá — consultar o host canônico direto
#define STATUS_ENDPOINT         "https://status.claude.com/api/v2/incidents/unresolved.json"

// NTP (necessário para os contadores de reset)
#define NTP_SERVER_1            "pool.ntp.org"
#define NTP_SERVER_2            "time.cloudflare.com"

// ── NVS ──────────────────────────────────────────────────
#define NVS_NAMESPACE           "claude"

#endif // CONFIG_H
