#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// Claude Usage Stick — CYD ESP32-2432S028 (ESP32, ST7789 SPI)
// Tudo aqui foi medido na placa: ver docs/HARDWARE-CYD.md e os logs
// brutos em docs/logs/.
// ============================================================

// ── Firmware ─────────────────────────────────────────────
#define FW_VERSION              "3.0-cyd"

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

// Buffer parcial do LVGL. Esta placa NAO tem PSRAM, e o tamanho daqui foi
// decidido por medicao, depois de uma investigacao inteira.
//
// O sintoma era: a busca na API funcionava as tres primeiras vezes, logo apos o
// boot, e falhava para sempre depois disso, com HTTP -1. Foram descartados, um
// a um e com numero: Wi-Fi (RSSI -49 dBm), DNS (resolve em 0 ms), TCP na 443
// (conecta em 15 ms), relogio (sincronizado, 118 s do PC) e o proprio bundle de
// certificados (o OpenSSL fecha a cadeia da Anthropic com os mesmos 3 certs de
// certs.cpp). O mbedtls dizia "X509 - Certificate verification failed", e a
// MESMA requisicao com setInsecure() passava no mesmo instante — ou seja, o que
// quebrava era so percorrer a cadeia.
//
// A cadeia da Anthropic e ECDSA P-384 com SHA-384, cuja verificacao no mbedtls
// pede blocos grandes e contiguos. O limiar medido nesta placa:
//
//   maior bloco 53 KB -> validacao falha sempre
//   maior bloco 78 KB -> validacao passa (HTTP 200 com o dashboard montado)
//
// As tres primeiras buscas passavam porque aconteciam antes de o dashboard
// existir, quando ainda havia ~110 KB de bloco livre. Nao tinha nada a ver com
// o relogio, que so por coincidencia sincronizava no mesmo intervalo.
//
// Dai buffer UNICO de 24 linhas (320 x 24 x 2 = 15.360 B) em vez de dois de 32.
// Custa um pouco de fluidez no redesenho; e o preco de o TLS fechar.
#define LVGL_BUF_LINES 24
#define LVGL_BYTES_PER_PX 2   // RGB565; ver a nota em setup()

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
// Intervalo entre buscas. Cada ciclo e UM POST /v1/messages com max_tokens:1
// - a sonda de modelo, que era a segunda requisicao por ciclo, saiu junto com
// a tela de Modelos.
//
// O piso de 10 s nao e arbitrario: a busca e bloqueante (~1 a 2 s de TLS) e a
// tela fica parada nesse intervalo. A 10 s isso e ~15% do tempo, que ja da
// para perceber; a 20 s cai para ~8%, que passa despercebido. Dai o padrao.
#define DEFAULT_POLL_SEC        20
#define MIN_POLL_SEC            10
#define MAX_POLL_SEC            300

// ── Segurança (PIN + AES-256-GCM) ────────────────────────
#define PIN_LEN                 4
#define MAX_PIN_ATTEMPTS        10
#define LOCKOUT_BASE_SEC        60       // dobra a cada falha
#define KDF_ROUNDS              10000
// Sal de compilacao da chave derivada do chip. Junto com o MAC de eFuse, e o
// que alimenta deviceSecret(). Trocar este valor invalida os tokens ja
// gravados: eles nao decifram mais, e a placa pede o token de novo.
#define DEVICE_KEY_SALT         "claude-usage-stick/cyd/v2"

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
