/*
 * Claude Usage Stick - CYD ESP32-2432S028
 *
 * Mostra quanto das janelas de 5 horas e da semana ja foi consumido no Claude
 * Code, lendo os cabeçalhos anthropic-ratelimit-unified-* de um POST
 * /v1/messages com max_tokens:1. Nao ha serviço no meio: a placa fala direto
 * com a api.anthropic.com.
 *
 * Uma tela so. O token fica cifrado em AES-256-GCM na NVS, com chave derivada
 * do proprio chip - o que evita texto claro, nao protege de quem tem a placa
 * (ver crypto.cpp). Wi-Fi e token sao configurados na tela e num formulario
 * web local; nada disso entra no binario.
 *
 * Port do benevid/claude-usage-stick-SVGL, originalmente para uma Guition
 * JC4832W535 (ESP32-S3, 480x320, PSRAM). Aqui: ESP32 classico, 4 MB de flash,
 * SEM PSRAM, ST7789 320x240 em SPI e XPT2046 resistivo.
 *
 * A restricao que manda em tudo neste arquivo e bloco contiguo de heap: a
 * validacao da cadeia TLS da Anthropic (ECDSA P-384) precisa de ~78 KB
 * contiguos, e falha com um HTTP -1 que nao menciona memoria em momento
 * nenhum. Ver docs/HARDWARE-CYD.md antes de alocar qualquer coisa aqui.
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <math.h>
#include "config.h"
#include "touch.h"
#include "wifi_manager.h"
#include "api.h"
#include "crypto.h"
#include "accounts.h"
#include "logo_assets.h"
#include "pixel_anims.h"   // Clawd + logotipo oficiais (gerado por tools/gen_logo_assets.py)

// ---- Paleta ----
// Contraste medido contra o fundo do card (C_SURFACE), nao contra o fundo da
// tela: e sobre o card que quase todo o texto vive. Nenhum tom de texto fica
// a menos de ~4:1 do card.
#define C_BG       0x14141C   // fundo da tela
#define C_SURFACE  0x22222C   // cards
#define C_SURFACE2 0x2E2E3A   // teclas / botoes secundarios
#define C_TRACK    0x33333F   // trilho de barras
#define C_GRID     0x2A2A34   // linhas de grade dentro de cards
#define C_BORDER   0x3C3C4A   // hairlines raras
#define C_TEXT     0xFFFCF8   // texto principal
#define C_MUTED    0xB4B4C2   // texto secundario (era 0x8C8C98)
#define C_FAINT    0x9494A4   // terciario; antes 0x5C5C68, que sumia no card
#define C_ACCENT   0xE8865F   // coral Claude, um grau mais claro
#define C_OK       0x5CF08E
#define C_WARN     0xFFC93C
#define C_BAD      0xFF8A8A

// ---- Idioma (0 = portugues, 1 = english; Ajustes -> NVS "lang") ----
static uint8_t g_lang = 0;
#define TRS(pt, en) (g_lang ? (en) : (pt))

// ---- Zona morta do touch ----
// O painel resistivo desta placa nao responde no canto inferior direito.
// Medido com firmware/touchmap: das 192 celulas de 20 px, 5 nao respondem, e
// todas ficam na ultima linha (y >= 220), concentradas em x >= 240. O resto da
// tela responde inteiro, bordas incluidas.
// Regra que segue disso: nenhum alvo de toque pode depender dessa faixa. Os
// widgets abaixo param em y = 216 por causa dela, nao por estetica.
#define TOUCH_DEAD_Y 220
#define TOUCH_SAFE_BOTTOM 216

// ---- Hardware ----
// Sem PSRAM nesta placa: nao ha Arduino_Canvas (framebuffer inteiro em RAM).
// O LVGL desenha em buffers parciais e cada regiao suja vai direto ao driver.
Arduino_GFX *gfx = nullptr;
XPT2046_Touch touch_dev(TOUCH_SCK, TOUCH_MOSI, TOUCH_MISO, TOUCH_CS, TOUCH_IRQ);
WiFiManager g_wifi;
Preferences g_prefs;

// ---- Estado da aplicação ----
enum State {
  ST_BOOT, ST_UNLOCK, ST_PIN, ST_WIFI, ST_TOKEN, ST_PARTY,
  ST_LOADING, ST_MAIN, ST_SETTINGS, ST_ACCOUNTS, ST_ACCT_NAME, ST_ABOUT, ST_ERROR
};
static State g_state = ST_BOOT;
static State g_pending = ST_BOOT;
static bool  g_dirty = false;
static void request_state(State s) { g_pending = s; g_dirty = true; }

// ---- Dados ----
static UsageData   g_usage = {};


// ---- Tokens por sessao (vindos do bridge via POST /tokens) ----
struct TokenStats { long long tin, tout, cache; int sessions; uint32_t atMs; };
static TokenStats g_tok = {0, 0, 0, 0, 0};
#define TOK_FRESH_MS (15UL * 60UL * 1000UL)

// ---- Token / segurança ----
static AccountSlots g_accts;
static EncryptedBlob g_blob;
static bool g_hasToken = false;              // existe conta salva no NVS
static bool g_onboarding = false;            // primeiro setup em andamento
static char g_token[200] = {0};              // token decifrado (só em RAM)
static char g_pendingToken[200] = {0};       // token digitado, aguardando PIN
// PIN da sessao: fica em RAM do desbloqueio ate o reboot, porque trocar de conta
// e adicionar conta precisam decifrar/cifrar OUTROS slots sem pedir o PIN de novo.
// Trade-off aceito: nao enfraquece o modelo — o token JA vive decifrado em
// g_token, entao quem consegue ler a RAM ja tem o que interessa. O que continua
// valendo: nada disso vai para o NVS, e factory_reset() zera este buffer.
// Migracao: verdadeiro quando existe um token gravado que a chave do chip nao
// abre, ou seja, cifrado pela versao com PIN. Nesse caso, e so nesse, o teclado
// numerico aparece - uma vez.
static bool g_migratingPin = false;
static int  g_tokenTargetSlot = 0;
static char g_pendingLabel[ACCT_LBL_MAX] = {0};
static char g_pinEntry[PIN_LEN + 1] = {0};   // dígitos sendo digitados
static int  g_pinAttempts = 0;               // tentativas erradas (persistido)
static uint32_t g_lockoutUntil = 0;          // millis até liberar nova tentativa
static bool g_timeInit = false;

// ---- Refresh em background ----
static bool g_wantRefresh = false;        // botão de refresh pediu atualização
static bool g_refreshing = false;         // busca em andamento
static bool g_lastFetchOk = true;         // último fetch deu certo?
static uint32_t g_lastOkMs = 0;           // millis do último sucesso (p/ "atualizado há Xs")
static lv_obj_t *g_hdrStatus = nullptr;   // texto de status no cabeçalho do dashboard

// ---- Brilho ----
static const uint8_t BRI_LEVELS[3] = {60, 160, 255};
static int g_briIdx = 1;

static uint32_t g_lastPollMs = 0;         // millis do último poll (p/ barra de refresh)
static const int POLL_OPTS[4] = {10, 20, 30, 60};
static int g_pollSec = DEFAULT_POLL_SEC;  // intervalo de atualização (config, NVS)
static int g_tzOffset = -3;               // fuso GMT (horas), config NVS


// ---- Ponteiros de UI do dashboard (zerados a cada build de ST_MAIN) ----
struct DashUI {
  lv_obj_t *refArc;
  lv_obj_t *agChip, *agPct5, *agCd5, *agAt5;
  lv_obj_t *agPct7, *agCd7, *agAt7, *agTok;
  lv_obj_t *bar5, *bar7;
  lv_obj_t *anim;
};
static DashUI g_ui;
static lv_obj_t *g_pinDots = nullptr, *g_pinMsg = nullptr;


// ---- Forward declarations ----
static void render_state();
static void refresh_ui_values();
static void dash_tick();
static void set_hdr_status();
static void apply_tz();
static void ui_pin();
static void ui_unlock();
static void ui_party();
static void ui_wifi();
static void ui_token();
static void ui_loading(const char *sub);
static void ui_main();
static void ui_settings();
static void ui_accounts();
static void ui_account_name();
static void ui_message(const char *title, const char *sub, uint32_t color);
static void nav_cb(lv_event_t *e);
static void start_data_web();
static void update_tok_row();

// ============================================================
// Pipeline de display/touch (validado no bring-up)
// ============================================================
// O original copiava a tela inteira para o framebuffer do Canvas girando 270
// grau a grau, porque o driver AXS15231B so aceitava o buffer na orientacao
// nativa. Aqui a rotacao e feita pelo proprio ST7789 (TFT_ROTATION), entao o
// flush so precisa empurrar o retangulo sujo — que e o unico jeito de caber
// numa placa sem PSRAM.
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_display_flush_ready(disp);
}
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  uint16_t x, y;
  if (touch_dev.touched()) {
    touch_dev.readData(&x, &y);
    data->point.x = x; data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ============================================================
// Helpers de UI
// ============================================================
static lv_obj_t *mklabel(lv_obj_t *p, const char *txt, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}
static void no_box(lv_obj_t *o) {
  lv_obj_set_style_bg_opa(o, 0, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}
// Botão pílula com label centralizado; user_data leva o State alvo (nav_cb).
static lv_obj_t *mkbtn(lv_obj_t *p, const char *txt, const lv_font_t *font,
                       uint32_t bg, uint32_t fg) {
  lv_obj_t *b = lv_button_create(p);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_center(mklabel(b, txt, font, fg));
  return b;
}
static uint32_t pct_color(float p) {
  if (p < 70.0f) return C_OK;
  if (p < 90.0f) return C_WARN;
  return C_BAD;
}
// gradiente contínuo verde -> âmbar -> vermelho conforme o uso cresce
static lv_color_t grad_color(float p) {
  if (p < 0) p = 0; if (p > 100) p = 100;
  if (p <= 50.0f)
    return lv_color_mix(lv_color_hex(C_WARN), lv_color_hex(C_OK), (uint8_t)(p * 255.0f / 50.0f));
  return lv_color_mix(lv_color_hex(C_BAD), lv_color_hex(C_WARN), (uint8_t)((p - 50.0f) * 255.0f / 50.0f));
}
// Medidor: uma barra so, com a parte cheia na cor do gradiente.
// Eram 18 retangulos de 5 px com 1 px de folga entre eles. Nesta largura o
// olho nao conta tracinho: ou o desenho vira uma serrilha cinza, ou uma linha
// laranja - em nenhum dos dois casos da para ler quanto e. E cada segmento era
// um objeto LVGL com estilo proprio; 18 por card, 36 no total, so para
// desenhar uma barra.
// Porcentagem para exibicao. A utilizacao vem da API como fracao e pode passar
// de 1.0 quando o limite estourou; "101%" nao quer dizer nada para quem le, e
// ainda contradizia a barra, que sempre limitou em 100. O valor cru continua
// indo inteiro para o log do serial.
static int pct_show(float p) {
  if (p < 0) p = 0;
  if (p > 100.0f) p = 100.0f;
  return (int)(p + 0.5f);
}

static void set_meter(lv_obj_t *bar, float pct) {
  if (!bar) return;
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  int v = (int)(pct * 10.0f + 0.5f);
  if (pct > 0.05f && v == 0) v = 1;          // 0,1% ja aparece
  lv_bar_set_value(bar, v, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, grad_color(pct), LV_PART_INDICATOR);
}
// "reseta em 1h 23m" / "2d 4h" / "agora" / "--" (relógio não sincronizado)
static void fmt_eta(uint32_t epoch, char *out, int sz) {
  time_t now = time(nullptr);
  if (now < 1000000000L || epoch == 0) { snprintf(out, sz, "--"); return; }
  long d = (long)epoch - (long)now;
  if (d <= 0) { snprintf(out, sz, "%s", TRS("agora", "now")); return; }
  int days = d / 86400; d %= 86400;
  int hrs  = d / 3600;  d %= 3600;
  int mins = d / 60;
  if (days > 0)      snprintf(out, sz, "%dd %dh", days, hrs);
  else if (hrs > 0)  snprintf(out, sz, "%dh %02dm", hrs, mins);
  else               snprintf(out, sz, "%dm", mins);
}
static void fmt_clock(uint32_t epoch, char *out, int sz) {
  if (epoch == 0 || time(nullptr) < 1000000000L) { strlcpy(out, "--:--", sz); return; }
  time_t t = (time_t)epoch; struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, sz, "%a %H:%M", &tmv);
}
static void fmt_hm(uint32_t epoch, char *out, int sz) {
  if (epoch == 0 || time(nullptr) < 1000000000L) { strlcpy(out, "--:--", sz); return; }
  time_t t = (time_t)epoch; struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, sz, "%H:%M", &tmv);
}
// 1234 -> "1.2k", 2345678 -> "2.3M"
static void fmt_tok(long long v, char *out, int sz) {
  if (v >= 100000000LL)     snprintf(out, sz, "%lldM", v / 1000000LL);
  else if (v >= 1000000LL)  snprintf(out, sz, "%.1fM", v / 1e6);
  else if (v >= 10000LL)    snprintf(out, sz, "%lldk", v / 1000LL);
  else if (v >= 1000LL)     snprintf(out, sz, "%.1fk", v / 1e3);
  else                      snprintf(out, sz, "%lld", v);
}

// ============================================================
// NVS / persistência
// ============================================================
static void load_persisted() {
  g_prefs.begin(NVS_NAMESPACE, false);
  accountsLoad(g_prefs, g_accts);
  if (g_accts.used[g_accts.active] &&
      accountLoadBlob(g_prefs, g_accts.active, g_blob)) {
    g_hasToken = true;
  }
  g_pinAttempts = g_prefs.getInt("pinatt", 0);
  g_briIdx = g_prefs.getInt("bri", 1);
  if (g_briIdx < 0 || g_briIdx > 2) g_briIdx = 1;
  g_pollSec = g_prefs.getInt("poll", DEFAULT_POLL_SEC);
  {
    // O valor tem de estar no menu, nao so dentro da faixa: um 120 gravado
    // pela versao anterior sobreviveria calado, e a placa continuaria lenta
    // sem nada na tela explicando por que.
    bool known = false;
    for (int i = 0; i < 4; i++) if (POLL_OPTS[i] == g_pollSec) known = true;
    if (!known) { g_pollSec = DEFAULT_POLL_SEC; g_prefs.putInt("poll", g_pollSec); }
  }
  g_tzOffset = g_prefs.getInt("tz", -3);
  if (g_tzOffset < -12 || g_tzOffset > 14) g_tzOffset = -3;
  g_lang = g_prefs.getInt("lang", 0) ? 1 : 0;
}
static void save_attempts() { g_prefs.putInt("pinatt", g_pinAttempts); }
static void apply_brightness() { ledcWrite(TFT_BL, BRI_LEVELS[g_briIdx]); }


static void factory_reset() {
  g_prefs.clear();              // apaga blob, pinatt, bri do namespace claude
  g_wifi.forgetAll();
  for (int i = 0; i < ACCT_MAX; i++) {
    char pth[16]; snprintf(pth, sizeof(pth), "/hist%d.bin", i);
    LittleFS.remove(pth);
  }
  memset(&g_accts, 0, sizeof(g_accts));
  g_pendingLabel[0] = 0;
  g_tokenTargetSlot = 0;
  memset(&g_tok, 0, sizeof(g_tok));
  g_hasToken = false;
  g_token[0] = 0; g_pendingToken[0] = 0;
  g_pinAttempts = 0;
  g_onboarding = true;
  Serial.println("[RESET] tudo apagado");
}

// ============================================================
// Tela: PIN (keypad touch) — entra PIN p/ decifrar OU define novo no setup
// ============================================================
// Sem tecla OK: pin_kb_cb submete sozinho ao completar o 4o digito, entao ela
// nunca teve efeito nenhum - e ainda por cima caia na zona morta do painel.
static const char *pin_map[] = {
  "1", "2", "3", "\n",
  "4", "5", "6", "\n",
  "7", "8", "9", "\n",
  LV_SYMBOL_LEFT, "0", ""
};

static void pin_update_dots() {
  if (!g_pinDots) return;
  char dots[24] = {0};
  int len = strlen(g_pinEntry);
  for (int i = 0; i < PIN_LEN; i++) {
    strcat(dots, i < len ? "*" : "_");
    if (i < PIN_LEN - 1) strcat(dots, " ");
  }
  lv_label_set_text(g_pinDots, dots);
}

// So roda na migracao: decifra com o PIN antigo e regrava com a chave do chip.
static void pin_submit() {
  if (decryptToken(g_blob, g_pinEntry, g_token, sizeof(g_token))) {
    g_pinAttempts = 0; save_attempts();
    memset(g_pinEntry, 0, sizeof(g_pinEntry));

    EncryptedBlob nb;
    if (encryptToken(g_token, deviceSecret(), nb)) {
      accountSave(g_prefs, g_accts, g_accts.active, nb, g_accts.label[g_accts.active]);
      g_blob = nb;
      Serial.println("[PIN] token migrado para a chave do chip; nao sera mais pedido");
    } else {
      Serial.println("[PIN] FALHA ao regravar com a chave do chip");
    }
    g_migratingPin = false;

    if (!g_wifi.isConnected()) g_wifi.autoConnect(WIFI_CONNECT_TIMEOUT_MS);
    request_state(g_wifi.isConnected() ? ST_LOADING : ST_WIFI);
  } else {
    g_pinAttempts++; save_attempts();
    g_pinEntry[0] = 0; pin_update_dots();
    if (g_pinAttempts >= MAX_PIN_ATTEMPTS) {
      Serial.println("[PIN] limite estourado -> wipe");
      factory_reset();
      request_state(ST_WIFI);
      return;
    }
    int wait = LOCKOUT_BASE_SEC * (1 << (g_pinAttempts - 1));
    if (wait > 3600) wait = 3600;
    g_lockoutUntil = millis() + (uint32_t)wait * 1000;
    if (g_pinMsg) {
      char m[64];
      snprintf(m, sizeof(m), TRS("PIN errado (%d/%d). Aguarde %ds", "Wrong PIN (%d/%d). Wait %ds"),
               g_pinAttempts, MAX_PIN_ATTEMPTS, wait);
      lv_label_set_text(g_pinMsg, m);
    }
  }
}

static void pin_kb_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  if (millis() < g_lockoutUntil) return;     // travado
  lv_obj_t *bm = (lv_obj_t *)lv_event_get_target(e);
  uint32_t id = lv_buttonmatrix_get_selected_button(bm);
  const char *txt = lv_buttonmatrix_get_button_text(bm, id);
  if (!txt) return;
  int len = strlen(g_pinEntry);
  if (strcmp(txt, LV_SYMBOL_LEFT) == 0) {
    if (len > 0) g_pinEntry[len - 1] = 0;
    pin_update_dots();
  } else if (len < PIN_LEN) {
    g_pinEntry[len] = txt[0];
    g_pinEntry[len + 1] = 0;
    pin_update_dots();
    if (len + 1 == PIN_LEN) pin_submit();     // auto-submit ao completar
  }
}

static void ui_pin() {
  lv_obj_t *scr = lv_screen_active();
  const char *title = TRS("PIN, pela ultima vez", "PIN, one last time");
  lv_obj_t *t = mklabel(scr, title, &lv_font_montserrat_18, C_TEXT);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);

  g_pinDots = mklabel(scr, "", &lv_font_montserrat_24, C_ACCENT);
  lv_obj_align(g_pinDots, LV_ALIGN_TOP_MID, 0, 28);
  pin_update_dots();

  const char *sub = TRS("Para migrar o token; nao sera pedido de novo.",
                        "To migrate the token; it won't be asked again.");
  g_pinMsg = mklabel(scr, sub, &lv_font_montserrat_12, C_MUTED);
  lv_obj_align(g_pinMsg, LV_ALIGN_TOP_MID, 0, 58);

  lv_obj_t *bm = lv_buttonmatrix_create(scr);
  lv_buttonmatrix_set_map(bm, pin_map);
  // A tecla OK e a ultima da ultima fila: encostada embaixo ela cairia na zona
  // morta do painel, e nao haveria como confirmar o PIN.
  lv_obj_set_size(bm, 240, 140);
  lv_obj_align(bm, LV_ALIGN_BOTTOM_MID, 0, -(SCREEN_HEIGHT - TOUCH_SAFE_BOTTOM));
  lv_obj_set_style_bg_color(bm, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(bm, 0, 0);
  lv_obj_set_style_text_font(bm, &lv_font_montserrat_20, 0);
  lv_obj_set_style_bg_color(bm, lv_color_hex(C_SURFACE2), LV_PART_ITEMS);
  lv_obj_set_style_text_color(bm, lv_color_hex(C_TEXT), LV_PART_ITEMS);
  lv_obj_add_event_cb(bm, pin_kb_cb, LV_EVENT_VALUE_CHANGED, NULL);

  if (millis() < g_lockoutUntil && g_pinMsg) {
    int rem = (g_lockoutUntil - millis()) / 1000;
    char m[48]; snprintf(m, sizeof(m), TRS("Aguarde %ds", "Wait %ds"), rem);
    lv_label_set_text(g_pinMsg, m);
  }
}

// ============================================================
// Tela: festa — o Clawd grande, no meio, sem mais nada
// ============================================================
// Um toque no Clawd do painel chega aqui. Nao mostra dado nenhum de proposito:
// e o unico lugar do firmware que existe so para ser bonito.
static void ui_party() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E0E14), 0);

  // 20x20 a escala 11 = 220 px: o maior quadrado que cabe nos 240 de altura.
  lv_obj_t *box = pa_mount(scr, 11);
  lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 2);
  pa_set(PA_DANCE_DJMIX);

  lv_obj_t *hint = mklabel(scr, TRS("toque para voltar", "tap to go back"),
                           &lv_font_montserrat_12, C_FAINT);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

  // A tela inteira volta: nao ha nada aqui para errar o alvo.
  lv_obj_t *back = lv_obj_create(scr);
  lv_obj_set_pos(back, 0, 0);
  lv_obj_set_size(back, SCREEN_WIDTH, TOUCH_SAFE_BOTTOM);
  lv_obj_set_style_bg_opa(back, 0, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(back, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ST_MAIN);
}

// ============================================================
// Tela: desbloqueio — um toque e entra
// ============================================================
// Substitui o teclado de PIN. Nao guarda segredo nenhum: o token ja foi aberto
// no setup() pela chave do chip. Existe so para a placa nao cair direto no
// dashboard com dados velhos enquanto o WiFi ainda sobe, e para dar um lugar
// obvio de tocar quando ela e ligada na tomada.
static void unlock_cb(lv_event_t *e) {
  (void)e;
  request_state(g_wifi.isConnected() ? ST_LOADING : ST_WIFI);
}

static void ui_unlock() {
  lv_obj_t *scr = lv_screen_active();

  lv_obj_t *box = pa_mount(scr, 5);                // 100x100
  lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 14);
  pa_set(PA_IDLE_LOOK_AROUND);                     // esperando alguem chegar

  lv_obj_t *t = mklabel(scr, "Claude Usage Stick", &lv_font_montserrat_18, C_TEXT);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 118);

  // O alvo e a tela inteira acima da zona morta: nao ha como errar o toque.
  lv_obj_t *b = mkbtn(scr, TRS("toque para comecar", "tap to start"),
                      &lv_font_montserrat_16, C_SURFACE2, C_ACCENT);
  lv_obj_set_size(b, 260, 52);
  lv_obj_set_ext_click_area(b, 40);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 150);
  lv_obj_add_event_cb(b, unlock_cb, LV_EVENT_CLICKED, NULL);
}

// ============================================================
// Tela: WiFi (scan + teclado)
// ============================================================
static lv_obj_t *wifi_list = nullptr, *wifi_ta = nullptr, *wifi_kb = nullptr, *wifi_status = nullptr;
static char sel_ssid[33] = {0};
static void wifi_item_cb(lv_event_t *e);

static void wifi_populate() {
  lv_obj_clean(wifi_list);
  lv_label_set_text(wifi_status, TRS("Escaneando redes...", "Scanning networks..."));
  lv_refr_now(NULL);
  WiFiManager::NetworkInfo nets[12];
  int n = g_wifi.scanNetworks(nets, 12);
  for (int i = 0; i < n; i++) {
    lv_obj_t *b = lv_list_add_button(wifi_list, LV_SYMBOL_WIFI, nets[i].ssid);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_SURFACE), 0);
    lv_obj_set_style_text_color(b, lv_color_hex(C_TEXT), 0);
    lv_obj_add_event_cb(b, wifi_item_cb, LV_EVENT_CLICKED, NULL);  // clique direto no botão
  }
  lv_label_set_text(wifi_status, n > 0 ? TRS("Toque na sua rede", "Tap your network")
                                       : TRS("Nenhuma rede. Toque em Reescanear.", "No networks. Tap Rescan."));
}
static void wifi_item_cb(lv_event_t *e) {
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  const char *txt = lv_list_get_button_text(wifi_list, btn);
  if (!txt) return;
  strlcpy(sel_ssid, txt, sizeof(sel_ssid));
  lv_label_set_text_fmt(wifi_status, TRS("Senha de \"%s\":", "Password for \"%s\":"), sel_ssid);
  lv_obj_add_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);
  lv_textarea_set_text(wifi_ta, "");
  lv_obj_clear_flag(wifi_ta, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
  lv_keyboard_set_textarea(wifi_kb, wifi_ta);
}
static void wifi_kb_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    const char *pass = lv_textarea_get_text(wifi_ta);
    lv_label_set_text(wifi_status, TRS("Conectando...", "Connecting..."));
    lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_ta, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);
    bool ok = g_wifi.connectTo(sel_ssid, pass, 15000);
    if (ok) request_state(g_onboarding ? ST_TOKEN : ST_LOADING);
    else { lv_label_set_text(wifi_status, TRS("Falhou. Toque numa rede de novo.", "Failed. Tap a network again.")); lv_obj_clear_flag(wifi_list, LV_OBJ_FLAG_HIDDEN); }
  } else if (code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(wifi_status, TRS("Toque na sua rede", "Tap your network"));
  }
}
static void wifi_rescan_cb(lv_event_t *e) { (void)e; wifi_populate(); }

static void ui_wifi() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *title = mklabel(scr, TRS("Configurar WiFi", "Configure WiFi"), &lv_font_montserrat_16, C_TEXT);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 8);

  lv_obj_t *mark = pa_mount(scr, 2);               // 40x40, cabe no cabecalho
  lv_obj_align(mark, LV_ALIGN_TOP_LEFT, 160, 0);
  pa_set(PA_IDLE_BLINK);

  lv_obj_t *rb = mkbtn(scr, TRS("Reescanear", "Rescan"), &lv_font_montserrat_12, C_SURFACE2, C_ACCENT);
  lv_obj_set_size(rb, 88, 30);
  lv_obj_align(rb, LV_ALIGN_TOP_RIGHT, -6, 4);
  lv_obj_add_event_cb(rb, wifi_rescan_cb, LV_EVENT_CLICKED, NULL);

  if (!g_onboarding && g_hasToken) {
    lv_obj_t *bk = mkbtn(scr, TRS(LV_SYMBOL_LEFT " Voltar", LV_SYMBOL_LEFT " Back"),
                         &lv_font_montserrat_12, C_SURFACE2, C_MUTED);
    lv_obj_set_size(bk, 78, 30);
    lv_obj_align(bk, LV_ALIGN_TOP_RIGHT, -100, 4);
    lv_obj_add_event_cb(bk, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(g_usage.ok ? ST_MAIN : ST_SETTINGS));
  }

  wifi_status = mklabel(scr, "...", &lv_font_montserrat_12, C_MUTED);
  lv_obj_align(wifi_status, LV_ALIGN_TOP_LEFT, 10, 34);

  wifi_list = lv_list_create(scr);
  lv_obj_set_size(wifi_list, 304, 162);   // termina em y=216: ver TOUCH_DEAD_Y
  lv_obj_align(wifi_list, LV_ALIGN_TOP_MID, 0, 54);
  lv_obj_set_style_bg_color(wifi_list, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_color(wifi_list, lv_color_hex(C_BORDER), 0);
  // clique é anexado por botão em wifi_populate()

  wifi_ta = lv_textarea_create(scr);
  lv_textarea_set_one_line(wifi_ta, true);
  lv_textarea_set_password_mode(wifi_ta, true);
  lv_textarea_set_placeholder_text(wifi_ta, TRS("senha do WiFi", "WiFi password"));
  lv_obj_set_size(wifi_ta, 304, 38);
  lv_obj_align(wifi_ta, LV_ALIGN_TOP_MID, 0, 52);
  lv_obj_add_flag(wifi_ta, LV_OBJ_FLAG_HIDDEN);

  wifi_kb = lv_keyboard_create(scr);
  // Sem este recuo a tecla de confirmar cai na zona morta do painel e nao ha
  // como enviar a senha (ver TOUCH_DEAD_Y).
  lv_obj_set_size(wifi_kb, SCREEN_WIDTH, 130);
  lv_obj_align(wifi_kb, LV_ALIGN_BOTTOM_MID, 0, -(SCREEN_HEIGHT - TOUCH_SAFE_BOTTOM));
  lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(wifi_kb, wifi_kb_cb, LV_EVENT_ALL, NULL);

  wifi_populate();
}

// ============================================================
// WebServer: token (onboarding) + dados (bridge de tokens)
// ============================================================
static WebServer *g_web = nullptr;
static volatile bool g_tokenGot = false;
static lv_obj_t *g_tokMsg = nullptr;            // status na tela do device

static void stop_web() { if (g_web) { g_web->stop(); delete g_web; g_web = nullptr; } }

static bool g_mdnsUp = false;
static void ensure_mdns() {
  if (g_mdnsUp || !g_wifi.isConnected()) return;
  if (MDNS.begin("claude-stick")) {
    MDNS.addService("http", "tcp", 80);
    g_mdnsUp = true;
    Serial.println("[MDNS] claude-stick.local");
  }
}



// ---- páginas HTML ----
#define WEB_CSS \
  ":root{--bg:#0F0F12;--card:#1A1A20;--bd:#30303A;--tx:#F2F0EC;--mut:#8C8C98;--cor:#D97757}" \
  "*{box-sizing:border-box}" \
  "body{margin:0;background:var(--bg);color:var(--tx);font-family:-apple-system,Segoe UI,Roboto,sans-serif;" \
  "display:flex;min-height:100vh;align-items:center;justify-content:center}" \
  ".card{background:var(--card);border:1px solid var(--bd);border-radius:16px;padding:26px;max-width:520px;width:92%}" \
  "h1{font-size:19px;margin:0 0 6px;display:flex;align-items:center;gap:10px}" \
  "p{color:var(--mut);font-size:14px;line-height:1.5;margin:6px 0 14px}" \
  "textarea{width:100%;background:var(--bg);color:var(--tx);border:1px solid var(--bd);border-radius:10px;" \
  "padding:12px;font-family:ui-monospace,monospace;font-size:13px;min-height:96px;resize:vertical}" \
  "input{width:100%;background:var(--bg);color:var(--tx);border:1px solid var(--bd);border-radius:10px;" \
  "padding:12px;font-size:14px;margin-bottom:10px}" \
  "button{margin-top:14px;width:100%;background:var(--cor);color:#1A1A20;border:0;border-radius:10px;" \
  "padding:14px;font-size:16px;font-weight:700;cursor:pointer}" \
  ".spark{width:26px;height:26px;flex:0 0 auto}code,a{color:var(--cor)}"

#define WEB_SPARK \
  "<svg class=spark viewBox='0 0 100 100'><g stroke='#D97757' stroke-width='12' stroke-linecap='round'>" \
  "<line x1=50 y1=9 x2=50 y2=91/><line x1=9 y1=50 x2=91 y2=50/>" \
  "<line x1=21 y1=21 x2=79 y2=79/><line x1=79 y1=21 x2=21 y2=79/>" \
  "<line x1=34 y1=11 x2=66 y2=89/><line x1=66 y1=11 x2=34 y2=89/></g></svg>"

static String web_form() {
  String h = F("<!doctype html><html lang=pt><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Claude Usage Stick</title><style>" WEB_CSS "</style></head><body><div class=card>"
               "<h1>" WEB_SPARK " Claude Usage Stick</h1>"
               "<p>Cole o seu token OAuth do Claude (<code>sk-ant-oat01-...</code>) e toque em <b>Salvar</b>. "
               "O gadget vai <b>validar</b> o token e pedir um PIN na tela.</p>"
               "<form method=POST action='/token'>"
               "<input name=label maxlength=16 placeholder='rotulo da conta (ex.: Pessoal, Trabalho)' autocomplete=off>"
               "<textarea name=token placeholder='sk-ant-oat01-...' autocomplete=off autofocus></textarea>"
               "<button type=submit>Salvar e validar</button></form></div></body></html>");
  return h;
}
static String web_result(bool ok, const String &msg) {
  String h = F("<!doctype html><html lang=pt><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Claude Usage Stick</title><style>" WEB_CSS "</style></head><body><div class=card>");
  if (ok) {
    h += F("<h1>" WEB_SPARK " Token validado</h1>"
           "<p>Token aceito pela API. Agora <b>defina um PIN de 4 dígitos</b> na tela do gadget para finalizar. "
           "Pode fechar esta página.</p>");
  } else {
    h += F("<h1>" WEB_SPARK " Token recusado</h1><p>");
    h += msg;
    h += F("</p><p><a href='/'>Voltar e tentar de novo</a></p>");
  }
  h += F("</div></body></html>");
  return h;
}

static void handleRoot()     { g_web->send(200, "text/html; charset=utf-8", web_form()); }
static void handleNotFound() { g_web->sendHeader("Location", "/"); g_web->send(302, "text/plain", ""); }

static void handleTokenPost() {
  String lb = g_web->arg("label");
  lb.trim();
  strlcpy(g_pendingLabel, lb.c_str(), sizeof(g_pendingLabel));
  String t = g_web->arg("token");
  t.trim();
  if (t.length() < 8) {
    if (g_tokMsg) lv_label_set_text(g_tokMsg, TRS("token vazio", "empty token"));
    g_web->send(200, "text/html; charset=utf-8", web_result(false, "Token vazio ou muito curto."));
    return;
  }
  // feedback no device antes da chamada bloqueante
  if (g_tokMsg) { lv_label_set_text(g_tokMsg, TRS("validando token...", "validating token...")); lv_refr_now(NULL); }

  UsageData tmp = {};
  bool ok = fetchUsage(t.c_str(), tmp);
  if (ok) {
    strlcpy(g_pendingToken, t.c_str(), sizeof(g_pendingToken));
    g_usage = tmp;                              // já temos dados p/ o dashboard
    g_tokenGot = true;                          // loop -> finalize_pending_token()
    if (g_tokMsg) lv_label_set_text(g_tokMsg, TRS("token OK!", "token OK!"));
    g_web->send(200, "text/html; charset=utf-8", web_result(true, ""));
  } else {
    String m = String("A API recusou o token (") + tmp.error + "). Confira e cole de novo.";
    if (g_tokMsg) lv_label_set_text(g_tokMsg, TRS("token recusado, tente de novo", "token rejected, try again"));
    g_web->send(200, "text/html; charset=utf-8", web_result(false, m));
  }
}

// ---- Endpoints de dados (bridge de tokens; ver tools/token_bridge.py) ----
static long long jll(const String &s, const char *key) {
  String k = String("\"") + key + "\"";
  int i = s.indexOf(k); if (i < 0) return 0;
  i = s.indexOf(':', i + k.length() - 1); if (i < 0) return 0;
  return atoll(s.c_str() + i + 1);
}
static bool jstr(const String &s, const char *key, char *out, size_t sz) {
  String k = String("\"") + key + "\"";
  int i = s.indexOf(k); if (i < 0) return false;
  i = s.indexOf(':', i + k.length()); if (i < 0) return false;
  i = s.indexOf('"', i); if (i < 0) return false;
  int e = s.indexOf('"', i + 1); if (e < 0) return false;
  strlcpy(out, s.substring(i + 1, e).c_str(), sz);
  return true;
}
static void handleWindow() {
  char b[256];
  snprintf(b, sizeof(b),
           "{\"now\":%lu,\"h5_reset\":%lu,\"d7_reset\":%lu,\"h5_util\":%.4f,\"d7_util\":%.4f,"
           "\"account\":\"%s\",\"slot\":%d}",
           (unsigned long)time(nullptr),
           (unsigned long)g_usage.h5ResetEpoch, (unsigned long)g_usage.d7ResetEpoch,
           g_usage.h5 / 100.0f, g_usage.d7 / 100.0f,
           g_accts.label[g_accts.active], g_accts.active);
  g_web->send(200, "application/json", b);
}
static void handleTokensPost() {
  String body = g_web->arg("plain");
  char acct[ACCT_LBL_MAX];
  if (jstr(body, "account", acct, sizeof(acct)) && acct[0] &&
      strcmp(acct, g_accts.label[g_accts.active]) != 0) {
    char r[96];
    snprintf(r, sizeof(r), "{\"error\":\"account_mismatch\",\"active\":\"%s\"}",
             g_accts.label[g_accts.active]);
    g_web->send(409, "application/json", r);
    return;
  }
  g_tok.tin      = jll(body, "in");
  g_tok.tout     = jll(body, "out");
  g_tok.cache    = jll(body, "cache");
  g_tok.sessions = (int)jll(body, "sessions");
  g_tok.atMs     = millis();
  Serial.printf("[TOK] in=%lld out=%lld cache=%lld sess=%d\n",
                g_tok.tin, g_tok.tout, g_tok.cache, g_tok.sessions);
  g_web->send(200, "application/json", "{\"ok\":true}");
  update_tok_row();
}
static void handleInfo() {
  String h = F("<!doctype html><html lang=pt><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Claude Usage Stick</title><style>" WEB_CSS "</style></head><body><div class=card>"
               "<h1>" WEB_SPARK " Claude Usage Stick</h1>"
               "<p>Device online. Endpoints: <code>GET /window</code> (janela atual) e "
               "<code>POST /tokens</code> (bridge de tokens por sessao — ver tools/token_bridge.py).</p>"
               "</div></body></html>");
  g_web->send(200, "text/html; charset=utf-8", h);
}
static void start_data_web() {
  stop_web();
  ensure_mdns();
  g_web = new WebServer(80);
  g_web->on("/", HTTP_GET, handleInfo);
  g_web->on("/window", HTTP_GET, handleWindow);
  g_web->on("/tokens", HTTP_POST, handleTokensPost);
  g_web->onNotFound([]() { g_web->send(404, "application/json", "{\"error\":\"not_found\"}"); });
  g_web->begin();
}

static void ui_token() {
  stop_web();
  lv_obj_t *scr = lv_screen_active();

  if (!g_onboarding && g_hasToken) {
    lv_obj_t *bk = mkbtn(scr, TRS(LV_SYMBOL_LEFT " Voltar", LV_SYMBOL_LEFT " Back"),
                         &lv_font_montserrat_12, C_SURFACE2, C_MUTED);
    lv_obj_set_size(bk, 78, 30);
    lv_obj_align(bk, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_add_event_cb(bk, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(g_usage.ok ? ST_MAIN : ST_SETTINGS));
  }

  lv_obj_t *mark = pa_mount(scr, 3);               // 60x60
  lv_obj_align(mark, LV_ALIGN_TOP_MID, 0, 2);
  pa_set(PA_WORK_THINK);

  lv_obj_t *cap = mklabel(scr, TRS("Cole o token pelo navegador, em:",
                                   "Paste the token via browser, at:"), &lv_font_montserrat_12, C_MUTED);
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 94);

  String url = String("http://") + WiFi.localIP().toString();
  lv_obj_t *ip = mklabel(scr, url.c_str(), &lv_font_montserrat_20, C_ACCENT);
  lv_obj_align(ip, LV_ALIGN_TOP_MID, 0, 110);

  lv_obj_t *hint = mklabel(scr, TRS("abra esse endereco no PC/celular na MESMA rede WiFi",
                                    "open this address on a PC/phone on the SAME WiFi"),
                           &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_width(hint, 300);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 138);

  lv_obj_t *sp = lv_spinner_create(scr);
  lv_spinner_set_anim_params(sp, 1200, 70);
  lv_obj_set_size(sp, 26, 26);
  lv_obj_align(sp, LV_ALIGN_BOTTOM_MID, 0, -42);
  lv_obj_set_style_arc_color(sp, lv_color_hex(C_SURFACE2), LV_PART_MAIN);
  lv_obj_set_style_arc_color(sp, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(sp, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_width(sp, 5, LV_PART_INDICATOR);

  g_tokMsg = mklabel(scr, TRS("aguardando o token...", "waiting for the token..."), &lv_font_montserrat_12, C_MUTED);
  lv_obj_align(g_tokMsg, LV_ALIGN_BOTTOM_MID, 0, -8);

  // sobe o servidor web (formulario do token)
  g_web = new WebServer(80);
  g_web->on("/", HTTP_GET, handleRoot);
  g_web->on("/token", HTTP_POST, handleTokenPost);
  g_web->onNotFound(handleNotFound);
  g_web->begin();
  Serial.printf("[WEB] servidor em %s\n", url.c_str());
}

// ============================================================
// Tela: loading / mensagem
// ============================================================
static void ui_message(const char *title, const char *sub, uint32_t color) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *mark = pa_mount(scr, 4);               // 80x80
  lv_obj_align(mark, LV_ALIGN_CENTER, 0, -66);
  pa_set(PA_EXPRESSION_SLEEP);
  lv_obj_t *t = mklabel(scr, title, &lv_font_montserrat_20, color);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -14);
  if (sub && sub[0]) {
    lv_obj_t *s = mklabel(scr, sub, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_width(s, 300);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_obj_align(s, LV_ALIGN_CENTER, 0, 18);
  }
}
static void ui_loading(const char *sub) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *mark = pa_mount(scr, 4);               // 80x80
  lv_obj_align(mark, LV_ALIGN_CENTER, 0, -52);
  pa_set(PA_WORK_CODING);
  lv_obj_t *t = mklabel(scr, TRS("Carregando seu uso...", "Loading your usage..."), &lv_font_montserrat_16, C_TEXT);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 22);
  if (sub && sub[0]) {
    lv_obj_t *s = mklabel(scr, sub, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(s, LV_ALIGN_CENTER, 0, 44);
  }
  lv_obj_t *spn = lv_spinner_create(scr);
  lv_spinner_set_anim_params(spn, 1200, 70);
  lv_obj_set_size(spn, 28, 28);
  lv_obj_align(spn, LV_ALIGN_CENTER, 0, 76);
  lv_obj_set_style_arc_color(spn, lv_color_hex(C_SURFACE2), LV_PART_MAIN);
  lv_obj_set_style_arc_color(spn, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(spn, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spn, 4, LV_PART_INDICATOR);
}

// ---- Boot ----
//
// O boot nao passa por request_state(): quem desenha os estados e o loop(), e ele
// so comeca depois que setup() retorna. Entre o fillScreen(preto) e esse primeiro
// render havia LittleFS (que formata na primeira vez), a migracao do historico e o
// autoConnect — bloqueante por ate 8s POR rede salva, 24s no pior caso — tudo com o
// backlight ja aceso sobre uma tela preta. Quem acabou de gravar le isso como
// travamento e desliga na tomada no meio do boot.
static lv_obj_t *g_bootSub = nullptr;

static void boot_splash(const char *sub) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  lv_obj_t *mark = pa_mount(scr, 4);               // 80x80
  lv_obj_align(mark, LV_ALIGN_CENTER, 0, -52);
  pa_set(PA_IDLE_BREATHE);
  lv_obj_t *t = mklabel(scr, "Claude Usage Stick", &lv_font_montserrat_16, C_TEXT);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 22);
  g_bootSub = mklabel(scr, sub ? sub : "", &lv_font_montserrat_12, C_MUTED);
  lv_obj_align(g_bootSub, LV_ALIGN_CENTER, 0, 44);

  lv_obj_t *spn = lv_spinner_create(scr);
  lv_spinner_set_anim_params(spn, 1200, 70);
  lv_obj_set_size(spn, 28, 28);
  lv_obj_align(spn, LV_ALIGN_CENTER, 0, 76);
  lv_obj_set_style_arc_color(spn, lv_color_hex(C_SURFACE2), LV_PART_MAIN);
  lv_obj_set_style_arc_color(spn, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(spn, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spn, 4, LV_PART_INDICATOR);

  lv_task_handler();
  lv_refr_now(NULL);                       // o loop() ainda nao roda: forca o desenho
}

// O boot desenha antes de o loop() existir, entao quem faz a animacao andar
// durante ele sao boot_status() e boot_wifi_tick(), que ja sao chamados em
// intervalos curtos enquanto o WiFi sobe.

static void boot_status(const char *sub) {
  if (!g_bootSub) return;
  lv_label_set_text(g_bootSub, sub);
  pa_tick();
  lv_task_handler();
  lv_refr_now(NULL);
}

// Passado ao autoConnect: mantem o spinner girando e diz qual rede esta sendo
// tentada. So redesenha quando o texto muda — a cada 100ms um refresh completo
// de 320x240 competiria com o proprio WiFi.
static void boot_wifi_tick(const char *ssid, int idx, int total) {
  static char ultimo[64] = "";
  char s[64];
  if (total > 1) snprintf(s, sizeof(s), "%s (%d/%d)", ssid, idx, total);
  else           snprintf(s, sizeof(s), "%s", ssid);
  if (g_bootSub && strcmp(s, ultimo) != 0) {
    snprintf(ultimo, sizeof(ultimo), "%s", s);
    lv_label_set_text(g_bootSub, s);
  }
  pa_tick();
  lv_task_handler();
}

// Falha antes do LVGL existir: desenha direto no Arduino_GFX. Tambem acende o
// backlight, que fica em duty 0 desde o ledcAttach ate apply_brightness() — sem
// isso a mensagem seria escrita numa tela apagada, indistinguivel de placa morta.
static void fatal_screen(const char *msg) {
  if (gfx) {
    ledcWrite(TFT_BL, 200);
    gfx->fillScreen(0x0000);
    gfx->setTextColor(0xDBAA);             // C_ACCENT em RGB565
    gfx->setTextSize(2);
    gfx->setCursor(14, 120);
    gfx->println("FALHA AO INICIAR");
    gfx->setTextColor(0xFFFF);
    gfx->setTextSize(1);
    gfx->setCursor(14, 154);
    gfx->println(msg);
    gfx->setCursor(14, 174);
    gfx->println("Desligue e ligue a placa.");
    gfx->setCursor(14, 188);
    gfx->println("Se continuar, regrave o firmware.");
  }
  while (1) delay(1000);
}







static bool switch_account(int slot) {
  if (slot < 0 || slot >= ACCT_MAX || !g_accts.used[slot] || slot == g_accts.active)
    return false;
  EncryptedBlob b;
  if (!accountLoadBlob(g_prefs, slot, b)) return false;
  char tok[200];
  if (!decryptToken(b, deviceSecret(), tok, sizeof(tok))) return false;

  accountSetActive(g_prefs, g_accts, slot);
  g_blob = b;
  strlcpy(g_token, tok, sizeof(g_token));
  memset(tok, 0, sizeof(tok));
  memset(&g_tok, 0, sizeof(g_tok));
  memset(&g_usage, 0, sizeof(g_usage));
  Serial.printf("[ACCT] conta ativa -> slot %d (%s)\n", slot, g_accts.label[slot]);
  request_state(ST_LOADING);
  return true;
}

static void finalize_pending_token() {
  EncryptedBlob nb;
  if (!encryptToken(g_pendingToken, deviceSecret(), nb)) {
    memset(g_pendingToken, 0, sizeof(g_pendingToken));
    request_state(ST_SETTINGS);
    return;
  }
  bool replacing = g_accts.used[g_tokenTargetSlot];
  bool switching = (g_tokenTargetSlot != g_accts.active);
  const char *lbl = g_pendingLabel[0] ? g_pendingLabel
                    : (replacing ? g_accts.label[g_tokenTargetSlot] : "");
  accountSave(g_prefs, g_accts, g_tokenTargetSlot, nb, lbl);
  if (switching) {
    accountSetActive(g_prefs, g_accts, g_tokenTargetSlot);
    memset(&g_tok, 0, sizeof(g_tok));
  }
  g_blob = nb;
  strlcpy(g_token, g_pendingToken, sizeof(g_token));
  memset(g_pendingToken, 0, sizeof(g_pendingToken));
  g_pendingLabel[0] = 0;
  g_hasToken = true;
  g_lastOkMs = g_lastPollMs = millis();
  g_lastFetchOk = true;
  Serial.printf("[ACCT] token salvo no slot %d (%s)\n",
                g_tokenTargetSlot, g_accts.label[g_tokenTargetSlot]);
  request_state(ST_MAIN);
}

// ============================================================
// Animacao de pixel art (pixel_anims.h)
// ============================================================
// Uma lv_canvas de 20x20 em ARGB8888 - 1.600 bytes, estaticos - redesenhada
// celula a celula e ampliada por lv_image_set_scale. O buffer e estatico de
// proposito: alocar e liberar 1,6 KB a cada troca de tela fragmentaria a heap,
// e bloco contiguo e exatamente o recurso que o handshake TLS disputa aqui.
//
// A ampliacao usa vizinho mais proximo (antialias desligado): interpolar pixel
// art de 20x20 para 80x80 borraria justamente as bordas que a definem.
static uint8_t g_paBuf[LV_CANVAS_BUF_SIZE(PA_W, PA_H, 32, LV_DRAW_BUF_STRIDE_ALIGN)];
static int      g_paAnim = -1;      // indice em PA_ANIMS
static int      g_paStep = 0;
static uint32_t g_paStepAt = 0;

static void pa_paint(int animIdx, int step) {
  if (!g_ui.anim || animIdx < 0 || animIdx >= PA_COUNT) return;
  const pa_anim_t &a = PA_ANIMS[animIdx];
  if (step < 0 || step >= a.stepN) return;

  uint8_t gi = pgm_read_byte(&a.steps[step].grid);
  const uint8_t *g = a.grids + (uint32_t)gi * PA_GRID_BYTES;

  for (int y = 0; y < PA_H; y++) {
    for (int x = 0; x < PA_W; x += 2) {
      uint8_t b = pgm_read_byte(g + (y * PA_W + x) / 2);
      for (int k = 0; k < 2; k++) {
        uint8_t idx = k ? (b & 0x0f) : (b >> 4);
        if (idx == 0 || idx >= a.palN) {                 // 0 = transparente
          lv_canvas_set_px(g_ui.anim, x + k, y, lv_color_black(), LV_OPA_TRANSP);
        } else {
          uint16_t c = pgm_read_word(&a.pal[idx]);
          lv_canvas_set_px(g_ui.anim, x + k, y, lv_color_hex(
              ((c & 0xF800) << 8) | ((c & 0x07E0) << 5) | ((c & 0x001F) << 3)), LV_OPA_COVER);
        }
      }
    }
  }
}

// Cria o canvas da animacao numa tela. Uma por tela: o buffer e unico, e so
// existe uma tela viva por vez (render_state limpa a anterior).
//
// O objeto do canvas continua com 20x20 mesmo ampliado - a escala e de
// desenho, nao de layout. Entao o alvo de toque, quando existe, e um objeto
// transparente por cima, do tamanho JA ampliado.
static lv_obj_t *pa_mount(lv_obj_t *parent, int scale) {
  // O container existe porque o canvas mede 20x20 em layout por mais que
  // desenhe 20*scale: sem ele, lv_obj_align centraria um quadrado de 20 px e o
  // desenho vazaria para baixo e para a direita. O container tem o tamanho
  // real, entao alinhar e receber toque funcionam como em qualquer widget.
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_set_size(box, PA_W * scale, PA_H * scale);
  lv_obj_set_style_bg_opa(box, 0, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *c = lv_canvas_create(box);
  lv_canvas_set_buffer(c, g_paBuf, PA_W, PA_H, LV_COLOR_FORMAT_ARGB8888);
  lv_canvas_fill_bg(c, lv_color_black(), LV_OPA_TRANSP);
  lv_image_set_antialias(c, false);            // pixel art: vizinho mais proximo
  lv_image_set_pivot(c, 0, 0);
  lv_image_set_scale(c, 256 * scale);
  lv_obj_set_pos(c, 0, 0);
  g_ui.anim = c;
  g_paAnim = -1;                               // canvas novo: forca o primeiro paint
  return box;
}

// Troca a animacao em exibicao. Repetir a mesma nao reinicia o ciclo — senao
// cada atualizacao de dados travaria o bicho no primeiro quadro.
static void pa_set(int animIdx) {
  if (animIdx == g_paAnim || animIdx < 0 || animIdx >= PA_COUNT) return;
  g_paAnim = animIdx;
  g_paStep = 0;
  g_paStepAt = millis();
  pa_paint(g_paAnim, 0);
}

static void pa_tick() {
  if (!g_ui.anim || g_paAnim < 0) return;
  const pa_anim_t &a = PA_ANIMS[g_paAnim];
  uint16_t hold = pgm_read_word(&a.steps[g_paStep].hold_ms);
  if (millis() - g_paStepAt < hold) return;
  g_paStepAt = millis();
  g_paStep = (g_paStep + 1) % a.stepN;
  pa_paint(g_paAnim, g_paStep);
}

// ============================================================
// Dashboard — helpers visuais
// ============================================================
static uint32_t status_color(const char *s) {
  if (!s || !s[0]) return C_MUTED;
  if (!strcmp(s, "rejected") || !strcmp(s, "rate_limited") || !strcmp(s, "exceeded")) return C_BAD;
  if (strstr(s, "warning")) return C_WARN;
  return C_OK;
}
static const char *overall_label(const char *s) {
  if (!s || !s[0]) return "--";
  if (!strcmp(s, "allowed")) return "OK";
  if (strstr(s, "warning"))  return TRS("ATENCAO", "WARNING");
  if (!strcmp(s, "rejected")) return TRS("BLOQUEADO", "BLOCKED");
  return s;
}

// label posicionado vazio (preenchido em refresh_ui_values/dash_tick)
static lv_obj_t *tlabel(lv_obj_t *p, const lv_font_t *f, uint32_t c, int x, int y) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
  lv_label_set_text(l, "");
  lv_obj_set_pos(l, x, y);
  return l;
}
static lv_obj_t *tstatic(lv_obj_t *p, const char *txt, const lv_font_t *f, uint32_t c, int x, int y) {
  lv_obj_t *l = mklabel(p, txt, f, c);
  lv_obj_set_pos(l, x, y);
  return l;
}
// card moderno: superfície arredondada SEM borda (estrutura por cor, não caixa)
static lv_obj_t *card(lv_obj_t *p, int x, int y, int w, int h) {
  lv_obj_t *c = lv_obj_create(p);
  lv_obj_set_pos(c, x, y); lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_SURFACE), 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 18, 0);
  lv_obj_set_style_pad_all(c, 14, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}
// chip com fundo tintado + texto na cor (mais leve que chip sólido)
static lv_obj_t *mkchip(lv_obj_t *p, int x, int y) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, LV_SIZE_CONTENT, 24);
  lv_obj_set_style_radius(o, 12, 0);
  lv_obj_set_style_pad_hor(o, 10, 0);
  lv_obj_set_style_pad_ver(o, 0, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *l = lv_label_create(o);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  lv_label_set_text(l, ""); lv_obj_center(l);
  return o;
}
static void set_chip(lv_obj_t *o, const char *txt, uint32_t col) {
  if (!o) return;
  lv_color_t c = lv_color_hex(col);
  lv_obj_set_style_bg_color(o, lv_color_mix(c, lv_color_hex(C_BG), 60), 0);
  lv_obj_t *l = lv_obj_get_child(o, 0);
  if (l) { lv_label_set_text(l, txt[0] ? txt : "--"); lv_obj_set_style_text_color(l, c, 0); }
}
// peça retangular arredondada do mascote
static lv_obj_t *rrect(lv_obj_t *p, int x, int y, int w, int h, int r, uint32_t col) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, r, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

// Escala do sprite do mascote, em 1/256 (LVGL usa 256 = 100%). 200/256 leva o
// img_clawd_md de 88x56 para 69x44, que e o que cabe no passo de 80 px do tile.
#define MASC_SCALE 200
#define MS(v) ((int)((v) * MASC_SCALE / 256))




// ============================================================
// Builders dos 4 tiles
// ============================================================
// Tile 0 — AGORA: janelas 5h/semana com % grande, medidor segmentado
// (verde -> vermelho conforme o uso) e countdown grande.
// Card de uma janela. 216x84 com 10 de padding: 196x64 uteis, em tres faixas.
//
//   USO AGORA                  73%     <- o que e, e quanto ja foi
//   [========------------------]       <- o mesmo numero, de relance
//   faltam 2h14        zera 21:04      <- quando volta ao zero
//
// A leitura desce em ordem de utilidade: primeiro o quanto, depois ate quando.
static void build_win_card(lv_obj_t *t, int y, const char *title,
                           lv_obj_t **pct, lv_obj_t **bar, lv_obj_t **at, lv_obj_t **cd) {
  lv_obj_t *c = lv_obj_create(t);
  lv_obj_set_pos(c, 4, y); lv_obj_set_size(c, 216, 84);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_SURFACE), 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 14, 0);
  lv_obj_set_style_pad_all(c, 10, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  tstatic(c, title, &lv_font_montserrat_12, C_MUTED, 0, 4);

  *pct = tlabel(c, &lv_font_montserrat_28, C_OK, 0, 0);
  lv_obj_set_width(*pct, 196);
  lv_obj_set_style_text_align(*pct, LV_TEXT_ALIGN_RIGHT, 0);

  *bar = lv_bar_create(c);
  lv_obj_set_size(*bar, 196, 12);
  lv_obj_set_pos(*bar, 0, 32);
  lv_bar_set_range(*bar, 0, 1000);            // decimos de %: 0,1% ja move
  lv_bar_set_value(*bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_radius(*bar, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(*bar, 6, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(*bar, lv_color_hex(C_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(*bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(*bar, LV_OBJ_FLAG_CLICKABLE);

  *cd = tlabel(c, &lv_font_montserrat_14, C_TEXT, 0, 48);
  *at = tlabel(c, &lv_font_montserrat_12, C_FAINT, 0, 50);
  lv_obj_set_width(*at, 196);
  lv_obj_set_style_text_align(*at, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_dashboard(lv_obj_t *t) {
  // "5 HORAS" so diz alguma coisa para quem ja sabe o que e a janela de 5h. O
  // que a pessoa quer saber e quanto ela gastou agora e quanto gastou na
  // semana; quando a conta zera esta logo abaixo, com hora.
  build_win_card(t, 44,  TRS("USO AGORA", "USED NOW"),  &g_ui.agPct5, &g_ui.bar5, &g_ui.agAt5, &g_ui.agCd5);
  build_win_card(t, 134, TRS("NA SEMANA", "THIS WEEK"), &g_ui.agPct7, &g_ui.bar7, &g_ui.agAt7, &g_ui.agCd7);

  // Coluna da direita, 88 px: o estado geral em cima e o Clawd embaixo.
  g_ui.agChip = mkchip(t, 230, 46);

  // Um toque no Clawd leva a tela de festa.
  lv_obj_t *box = pa_mount(t, 4);                // 20x20 -> 80x80
  lv_obj_set_pos(box, 232, 84);
  lv_obj_set_ext_click_area(box, 6);
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(box, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ST_PARTY);

  // Contagem de tokens da janela: so aparece quando o tools/token_bridge.py
  // esta rodando na maquina do usuario. Fica no rodape, abaixo dos cards.
  g_ui.agTok = tlabel(t, &lv_font_montserrat_12, C_FAINT, 4, 222);
  lv_obj_set_width(g_ui.agTok, 312);
  lv_obj_set_style_text_align(g_ui.agTok, LV_TEXT_ALIGN_CENTER, 0);
}


// ============================================================
// Atualização de valores
// ============================================================
static void update_tok_row() {
  if (!g_ui.agTok) return;
  if (g_tok.atMs == 0 || millis() - g_tok.atMs > TOK_FRESH_MS) {
    lv_label_set_text(g_ui.agTok, "");
    return;
  }
  char a[16], b[16], s[96];
  fmt_tok(g_tok.tin, a, sizeof(a));
  fmt_tok(g_tok.tout, b, sizeof(b));
  snprintf(s, sizeof(s), TRS("tokens na janela: %s entrada \xE2\x80\xA2 %s saida",
                             "window tokens: %s in \xE2\x80\xA2 %s out"), a, b);
  lv_label_set_text(g_ui.agTok, s);
}

// Contadores/relógios (1s) — separado dos valores de fetch.
static void dash_tick() {
  if (g_state != ST_MAIN || !g_ui.agCd5) return;
  char e[32], c[24], b[64];

  // Janela de 5h: zera ainda hoje, entao basta a hora.
  fmt_eta(g_usage.h5ResetEpoch, e, sizeof(e));
  snprintf(b, sizeof(b), TRS("faltam %s", "%s left"), e);
  lv_label_set_text(g_ui.agCd5, b);
  fmt_hm(g_usage.h5ResetEpoch, c, sizeof(c));
  snprintf(b, sizeof(b), TRS("zera %s", "resets %s"), c);
  lv_label_set_text(g_ui.agAt5, b);

  // Semanal: pode zerar daqui a dias, entao o dia da semana entra junto.
  fmt_eta(g_usage.d7ResetEpoch, e, sizeof(e));
  snprintf(b, sizeof(b), TRS("faltam %s", "%s left"), e);
  lv_label_set_text(g_ui.agCd7, b);
  fmt_clock(g_usage.d7ResetEpoch, c, sizeof(c));
  snprintf(b, sizeof(b), TRS("zera %s", "resets %s"), c);
  lv_label_set_text(g_ui.agAt7, b);

  set_hdr_status();
  mood_update();
}



// ============================================================
// Limiares de uso (25/50/70/100% nas janelas 5h e semanal)
// ============================================================
// Antes isto abria um overlay em lv_layer_top que tomava a tela inteira por
// 4,6 segundos. Numa tela que existe para ser olhada de relance, tapar o
// numero e o oposto do que se quer - ainda mais quando a informacao que ele
// dava ("voce chegou a 70%") ja esta escrita em letras grandes atras dele.
//
// Agora o limiar cruzado so muda o humor do Clawd, no canto. A informacao
// continua, o bloqueio nao.
static const uint8_t THR[4] = {25, 50, 70, 100};
static int g_pendWin = -1, g_pendThr = 0;      // momento aguardando exibição
static uint8_t g_thrFired[2] = {0, 0};         // bits já disparados por janela
static float g_thrPrev[2] = {-1, -1};
static bool g_thrBase = false;
static lv_point_precise_t g_moXPts[4][2];      // olhos em X (KO)

// Detecta cruzamento de limiar após cada fetch. Baseline no 1º fetch (não
// dispara pelo que já estava acima); zera quando a janela reseta (queda >15pp).
static void check_thresholds() {
  float c[2] = {g_usage.h5, g_usage.d7};
  for (int w = 0; w < 2; w++) {
    if (!g_thrBase || (g_thrPrev[w] - c[w]) > 15.0f) {
      g_thrFired[w] = 0;
      for (int i = 0; i < 4; i++) if (c[w] >= THR[i]) g_thrFired[w] |= 1 << i;
    } else {
      int hit = -1;
      for (int i = 0; i < 4; i++)
        if (c[w] >= THR[i] && !(g_thrFired[w] & (1 << i))) { g_thrFired[w] |= 1 << i; hit = i; }
      if (hit >= 0) { g_pendWin = w; g_pendThr = THR[hit]; }   // consumido em dash_tick
    }
    g_thrPrev[w] = c[w];
  }
  g_thrBase = true;
}


// Escala do Clawd XL no overlay, em 1/256. 184/256 leva 176x110 para 126x79.



// Humor do Clawd: o pior dos dois estados manda. Um limiar recem-cruzado
// tem prioridade por alguns segundos, para o momento nao passar despercebido.
//
// A ordem aqui e deliberada: falha de rede vence tudo (nao adianta o bicho
// dancar se o numero na tela esta velho), depois a busca em andamento, depois
// a comemoracao do reset, e so entao o nivel de uso.
#define MOOD_HOLD_MS 9000
static uint32_t g_moodUntil = 0;
static int      g_moodPin = -1;        // animacao fixada ate g_moodUntil
static float    g_moodPrevH5 = -1.0f;

static void mood_pin(int anim, uint32_t ms) {
  g_moodPin = anim;
  g_moodUntil = millis() + ms;
}

static void mood_update() {
  if (!g_ui.anim) return;

  if (!g_lastFetchOk)  { pa_set(PA_EXPRESSION_SLEEP); return; }
  if (g_refreshing)    { pa_set(PA_WORK_CODING);      return; }

  if (g_moodPin >= 0) {
    if (millis() < g_moodUntil) { pa_set(g_moodPin); return; }
    g_moodPin = -1;
  }

  float p = g_usage.h5 > g_usage.d7 ? g_usage.h5 : g_usage.d7;
  if      (p >= 90.0f) pa_set(PA_EXPRESSION_SURPRISE);
  else if (p >= 70.0f) pa_set(PA_WORK_THINK);
  else if (p >= 50.0f) pa_set(PA_IDLE_LOOK_AROUND);
  else                 pa_set(PA_DANCE_BOUNCE);      // tudo tranquilo: dança
}

// Preenche todos os valores vindos do fetch (sem rebuild de tela).
static void refresh_ui_values() {
  if (g_state != ST_MAIN || !g_ui.agPct5) return;
  char b[96];

  // Agora: percentuais + medidores segmentados (cor desliza verde -> vermelho)
  snprintf(b, sizeof(b), "%d%%", pct_show(g_usage.h5)); lv_label_set_text(g_ui.agPct5, b);
  lv_obj_set_style_text_color(g_ui.agPct5, grad_color(g_usage.h5), 0);
  set_meter(g_ui.bar5, g_usage.h5);
  snprintf(b, sizeof(b), "%d%%", pct_show(g_usage.d7)); lv_label_set_text(g_ui.agPct7, b);
  lv_obj_set_style_text_color(g_ui.agPct7, grad_color(g_usage.d7), 0);
  set_meter(g_ui.bar7, g_usage.d7);

  set_chip(g_ui.agChip, overall_label(g_usage.statusOverall), status_color(g_usage.statusOverall));
  update_tok_row();

  // A janela zerou: a utilizacao despencou entre duas leituras. E o unico
  // momento francamente bom do dia nesta tela, entao o bicho dança.
  if (g_moodPrevH5 >= 0 && (g_moodPrevH5 - g_usage.h5) > 15.0f)
    mood_pin(PA_DANCE_BOUNCE_DJ, MOOD_HOLD_MS);   // a janela zerou: festa curta
  else if (g_pendWin >= 0) {
    // Limiar cruzado: surpresa nos altos, piscadela nos baixos.
    mood_pin(g_pendThr >= 70 ? PA_EXPRESSION_SURPRISE : PA_EXPRESSION_WINK, MOOD_HOLD_MS);
    g_pendWin = -1;
  }
  g_moodPrevH5 = g_usage.h5;
  mood_update();

  dash_tick();
}

// Estado do cabecalho. Quando esta tudo bem, nao ha texto nenhum: o anel ja
// diz que a proxima busca esta vindo, e um relogio contando "ha 2s, ha 3s" so
// chama atencao para uma informacao que ninguem usa. Texto so quando algo
// exige acao.
static void set_hdr_status() {
  if (!g_hdrStatus) return;
  const char *txt = "";
  uint32_t color = C_MUTED;
  if (g_refreshing)        { txt = TRS("buscando", "fetching"); color = C_ACCENT; }
  else if (!g_lastFetchOk) { txt = TRS("sem conexao", "offline"); color = C_BAD; }
  lv_label_set_text(g_hdrStatus, txt);
  lv_obj_set_style_text_color(g_hdrStatus, lv_color_hex(color), 0);

  if (g_ui.refArc) {
    uint32_t ac = g_refreshing ? C_ACCENT : (g_lastFetchOk ? C_ACCENT : C_BAD);
    lv_obj_set_style_arc_color(g_ui.refArc, lv_color_hex(ac), LV_PART_INDICATOR);
  }
}
// Botão de refresh: só pede; a busca acontece em background no loop()
static void refresh_cb(lv_event_t *e) { (void)e; g_wantRefresh = true; }

static void ui_main() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);

  start_data_web();

  // Header: Clawd + logotipo (duplo toque = demo das animacoes de limiar),
  // botao de ATUALIZAR no centro, engrenagem a direita.
  lv_obj_t *hIcon = lv_image_create(scr);
  lv_image_set_src(hIcon, &img_clawd_sm);
  lv_image_set_pivot(hIcon, 0, 0);
  lv_image_set_scale(hIcon, 200);              // 42x26 -> 33x20
  lv_obj_set_pos(hIcon, 6, 7);
  lv_obj_t *hWord = lv_image_create(scr);
  lv_image_set_src(hWord, &img_wordmark);
  lv_image_set_pivot(hWord, 0, 0);
  lv_image_set_scale(hWord, 180);              // 56x26 -> 39x18
  lv_obj_set_pos(hWord, 43, 8);

  lv_obj_t *logoSpot = lv_obj_create(scr);     // hotspot icone+nome (so demo)
  lv_obj_set_pos(logoSpot, 4, 2); lv_obj_set_size(logoSpot, 82, 34);
  lv_obj_set_style_bg_opa(logoSpot, 0, 0);
  lv_obj_set_style_border_width(logoSpot, 0, 0);
  lv_obj_clear_flag(logoSpot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(logoSpot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(logoSpot, [](lv_event_t *e) {
    (void)e;
    static uint32_t lastClick = 0;             // duplo clique manual (9.2 nao tem nativo)
    static int di = 0;
    uint32_t now = millis();
    if (now - lastClick < 450) {
      mood_pin(di % PA_COUNT, MOOD_HOLD_MS);   // passeia por todas as animacoes
      mood_update();
      di++;
      lastClick = 0;
    } else {
      lastClick = now;
    }
  }, LV_EVENT_CLICKED, NULL);

  // Anel do proximo refresh. Substitui a barra de 320x3 que descia no topo da
  // tela: mesma informacao, num canto, sem uma regua atravessando o campo de
  // visao. E ele mesmo e o botao de atualizar agora - o alvo e deliberado
  // (26 px mais 14 de area estendida), diferente da barra, que ficou sem
  // clique justamente por disparar refresh sem querer.
  g_ui.refArc = lv_arc_create(scr);
  lv_obj_set_size(g_ui.refArc, 26, 26);
  lv_obj_align(g_ui.refArc, LV_ALIGN_TOP_MID, 0, 8);
  lv_arc_set_rotation(g_ui.refArc, 270);
  lv_arc_set_bg_angles(g_ui.refArc, 0, 360);
  lv_arc_set_range(g_ui.refArc, 0, 1000);
  lv_arc_set_value(g_ui.refArc, 1000);
  lv_obj_remove_style(g_ui.refArc, NULL, LV_PART_KNOB);      // sem alca de arrastar
  lv_obj_clear_flag(g_ui.refArc, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  lv_obj_set_style_arc_width(g_ui.refArc, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_width(g_ui.refArc, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(g_ui.refArc, lv_color_hex(C_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_color(g_ui.refArc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_ext_click_area(g_ui.refArc, 14);
  lv_obj_add_event_cb(g_ui.refArc, refresh_cb, LV_EVENT_CLICKED, NULL);

  g_hdrStatus = mklabel(scr, "", &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_width(g_hdrStatus, 76);
  lv_obj_set_style_text_align(g_hdrStatus, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_long_mode(g_hdrStatus, LV_LABEL_LONG_DOT);
  lv_obj_align(g_hdrStatus, LV_ALIGN_TOP_RIGHT, -56, 12);

  // Badge da conta ativa. A faixa livre do cabecalho e estreita, e em 320 px
  // ficou ainda mais: o hotspot do logo termina em x=86 e o botao de atualizar
  // (44 px centrado, mais 8 px de ext_click_area) passa a capturar toque em
  // x=130. Ficar em 88..126 deixa 4 px de folga — encostar em 130 faria o
  // toque "no badge" disparar o refresh. LONG_DOT corta o que nao couber.
  if (accountCount(g_accts) > 1) {
    char ab[ACCT_LBL_MAX + 1];
    snprintf(ab, sizeof(ab), "@%s", g_accts.label[g_accts.active]);
    lv_obj_t *acct = mklabel(scr, ab, &lv_font_montserrat_12, C_ACCENT);
    lv_obj_set_width(acct, 38);
    lv_label_set_long_mode(acct, LV_LABEL_LONG_DOT);
    lv_obj_align(acct, LV_ALIGN_TOP_LEFT, 88, 12);
  }

  lv_obj_t *gear = mkbtn(scr, LV_SYMBOL_SETTINGS, &lv_font_montserrat_20, C_SURFACE2, C_TEXT);
  lv_obj_set_size(gear, 48, 32);
  lv_obj_set_ext_click_area(gear, 12);
  lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -4, 4);
  lv_obj_add_event_cb(gear, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ST_SETTINGS);

  build_dashboard(scr);
  g_paAnim = -1;                               // canvas novo: forca o primeiro paint
  mood_update();

  refresh_ui_values();
  Serial.printf("[MEM] dashboard montado: livre=%u  maior bloco=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

// ============================================================
// Tela: settings (lista rolável; linhas >=44px de toque)
// ============================================================
static bool g_wipeArmed = false;
static lv_obj_t *g_briLbl = nullptr, *g_wipeLbl = nullptr, *g_pollLbl = nullptr,
                *g_tzLbl = nullptr;
// Ordem escolhida para o custo de voltar ser baixo, nao por geografia: o
// padrao (-3) vem primeiro e o vizinho mais provavel de um engano (-4) vem por
// ultimo, entao de -4 para -3 e UM toque. Na ordem crescente anterior eram 11.
static const int TZ_OPTS[] = {-3, -2, -1, 0, 1, 2, 3, -8, -7, -6, -5, -4};
#define NTZ ((int)(sizeof(TZ_OPTS) / sizeof(TZ_OPTS[0])))

static int g_acctDelArmed = -1;

// Texto da linha de fuso. Mostrar a hora local ao lado do offset e o que torna
// um fuso errado visivel: sem isso, escolher GMT-4 no lugar de GMT-3 desloca
// todos os relogios da tela em uma hora sem nenhum sinal de que algo mudou.
static void tz_row_text(char *out, size_t n) {
  char hm[8] = "--:--";
  time_t agora = time(nullptr);
  if (agora > 1000000000L) {
    struct tm lt; localtime_r(&agora, &lt);
    strftime(hm, sizeof(hm), "%H:%M", &lt);
  }
  snprintf(out, n, TRS(LV_SYMBOL_GPS "  Fuso: GMT%+d  (agora %s)",
                       LV_SYMBOL_GPS "  Timezone: GMT%+d  (now %s)"), g_tzOffset, hm);
}

static void settings_action_cb(lv_event_t *e) {
  int act = (int)(intptr_t)lv_event_get_user_data(e);
  switch (act) {
    case 0: request_state(ST_LOADING); break;          // atualizar
    case 1: g_onboarding = false; request_state(ST_WIFI); break;
    case 2:                                            // trocar token
      g_tokenTargetSlot = g_accts.active;
      g_pendingLabel[0] = 0;
      request_state(ST_TOKEN);
      break;
    case 3:                                            // brilho
      g_briIdx = (g_briIdx + 1) % 3; g_prefs.putInt("bri", g_briIdx); apply_brightness();
      if (g_briLbl) {
        const char *n[3] = {TRS("baixo", "low"), TRS("medio", "medium"), TRS("alto", "high")};
        char m[40]; snprintf(m, sizeof(m), TRS(LV_SYMBOL_EYE_OPEN "  Brilho: %s",
                                               LV_SYMBOL_EYE_OPEN "  Brightness: %s"), n[g_briIdx]);
        lv_label_set_text(g_briLbl, m);
      }
      break;
    case 4:                                            // apagar tudo (2 toques)
      if (!g_wipeArmed) {
        g_wipeArmed = true;
        if (g_wipeLbl) lv_label_set_text(g_wipeLbl, TRS(LV_SYMBOL_TRASH "  Toque de novo p/ confirmar",
                                                        LV_SYMBOL_TRASH "  Tap again to confirm"));
      } else {
        g_wipeArmed = false;
        factory_reset();
        request_state(ST_WIFI);
      }
      break;
    case 5: request_state(ST_MAIN); break;             // voltar
    case 6: {                                          // intervalo de atualização
      int idx = 0;
      for (int i = 0; i < 4; i++) if (POLL_OPTS[i] == g_pollSec) idx = i;
      g_pollSec = POLL_OPTS[(idx + 1) % 4];
      g_prefs.putInt("poll", g_pollSec);
      if (g_pollLbl) {
        char m[40];
        if (g_pollSec < 60) snprintf(m, sizeof(m), TRS(LV_SYMBOL_LOOP "  Atualizar: %ds",
                                                       LV_SYMBOL_LOOP "  Refresh: %ds"), g_pollSec);
        else snprintf(m, sizeof(m), TRS(LV_SYMBOL_LOOP "  Atualizar: %dmin",
                                        LV_SYMBOL_LOOP "  Refresh: %dmin"), g_pollSec / 60);
        lv_label_set_text(g_pollLbl, m);
      }
      break;
    }
    case 7: {                                          // fuso horário (GMT)
      int idx = 0;
      for (int i = 0; i < NTZ; i++) if (TZ_OPTS[i] == g_tzOffset) idx = i;
      g_tzOffset = TZ_OPTS[(idx + 1) % NTZ];
      g_prefs.putInt("tz", g_tzOffset);
      apply_tz();
      if (g_tzLbl) { char m[48]; tz_row_text(m, sizeof(m)); lv_label_set_text(g_tzLbl, m); }
      break;
    }
    case 9:                                            // idioma / language
      g_lang ^= 1;
      g_prefs.putInt("lang", g_lang);
      request_state(ST_SETTINGS);                      // redesenha tudo no novo idioma
      break;
    case 10: request_state(ST_ABOUT); break;           // sobre / about
    case 11: g_acctDelArmed = -1; request_state(ST_ACCOUNTS); break;
  }
}
static void add_setting_row(lv_obj_t *p, const char *txt, int act, uint32_t fg, lv_obj_t **out) {
  lv_obj_t *b = lv_button_create(p);
  lv_obj_set_size(b, 296, 44);   // 44 px de altura: alvo de toque confortavel
  lv_obj_set_style_bg_color(b, lv_color_hex(C_SURFACE), 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t *l = mklabel(b, txt, &lv_font_montserrat_14, fg);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
  lv_obj_add_event_cb(b, settings_action_cb, LV_EVENT_CLICKED, (void *)(intptr_t)act);
  if (out) *out = l;
}
static void ui_settings() {
  lv_obj_t *scr = lv_screen_active();
  g_wipeArmed = false;
  start_data_web();
  lv_obj_t *title = mklabel(scr, TRS("Ajustes", "Settings"), &lv_font_montserrat_16, C_TEXT);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 8);

  lv_obj_t *bk = mkbtn(scr, TRS(LV_SYMBOL_LEFT " Voltar", LV_SYMBOL_LEFT " Back"),
                       &lv_font_montserrat_12, C_SURFACE2, C_MUTED);
  lv_obj_set_size(bk, 84, 30);
  lv_obj_set_ext_click_area(bk, 6);
  lv_obj_align(bk, LV_ALIGN_TOP_RIGHT, -6, 4);
  lv_obj_add_event_cb(bk, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(g_usage.ok ? ST_MAIN : ST_SETTINGS));

  // lista rolável
  lv_obj_t *lst = lv_obj_create(scr);
  lv_obj_set_pos(lst, 8, 38);
  lv_obj_set_size(lst, 304, 178);         // termina em y=216: ver TOUCH_DEAD_Y
  lv_obj_set_style_bg_opa(lst, 0, 0);
  lv_obj_set_style_border_width(lst, 0, 0);
  lv_obj_set_style_pad_all(lst, 0, 0);
  lv_obj_set_style_pad_row(lst, 8, 0);
  lv_obj_set_flex_flow(lst, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(lst, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(lst, LV_SCROLLBAR_MODE_AUTO);

  const char *n[3] = {TRS("baixo", "low"), TRS("medio", "medium"), TRS("alto", "high")};
  char bri[40]; snprintf(bri, sizeof(bri), TRS(LV_SYMBOL_EYE_OPEN "  Brilho: %s",
                                               LV_SYMBOL_EYE_OPEN "  Brightness: %s"), n[g_briIdx]);
  char pollTxt[40];
  if (g_pollSec < 60) snprintf(pollTxt, sizeof(pollTxt), TRS(LV_SYMBOL_LOOP "  Atualizar: %ds",
                                                             LV_SYMBOL_LOOP "  Refresh: %ds"), g_pollSec);
  else                snprintf(pollTxt, sizeof(pollTxt), TRS(LV_SYMBOL_LOOP "  Atualizar: %dmin",
                                                             LV_SYMBOL_LOOP "  Refresh: %dmin"), g_pollSec / 60);
  char tzTxt[48]; tz_row_text(tzTxt, sizeof(tzTxt));

  add_setting_row(lst, TRS(LV_SYMBOL_REFRESH "  Atualizar agora",
                           LV_SYMBOL_REFRESH "  Refresh now"),   0, C_TEXT, nullptr);
  add_setting_row(lst, pollTxt,                                  6, C_TEXT, &g_pollLbl);
  add_setting_row(lst, TRS(LV_SYMBOL_LIST "  Idioma: Portugues",
                           LV_SYMBOL_LIST "  Language: English"), 9, C_TEXT, nullptr);
  add_setting_row(lst, tzTxt,                                    7, C_TEXT, &g_tzLbl);
  add_setting_row(lst, bri,                                      3, C_TEXT, &g_briLbl);
  add_setting_row(lst, TRS(LV_SYMBOL_WIFI "  Configurar WiFi",
                           LV_SYMBOL_WIFI "  Configure WiFi"),   1, C_TEXT, nullptr);
  char acctTxt[64];
  snprintf(acctTxt, sizeof(acctTxt), TRS(LV_SYMBOL_DIRECTORY "  Contas: %s (%d/%d)",
                                         LV_SYMBOL_DIRECTORY "  Accounts: %s (%d/%d)"),
           g_accts.label[g_accts.active], accountCount(g_accts), ACCT_MAX);
  add_setting_row(lst, acctTxt,                                 11, C_TEXT, nullptr);
  add_setting_row(lst, TRS(LV_SYMBOL_KEYBOARD "  Trocar token",
                           LV_SYMBOL_KEYBOARD "  Change token"), 2, C_TEXT, nullptr);
  add_setting_row(lst, TRS(LV_SYMBOL_FILE "  Sobre",
                           LV_SYMBOL_FILE "  About"),           10, C_TEXT, nullptr);
  add_setting_row(lst, TRS(LV_SYMBOL_TRASH "  Apagar tudo",
                           LV_SYMBOL_TRASH "  Erase everything"), 4, C_BAD, &g_wipeLbl);
}

static void acct_switch_cb(lv_event_t *e) {
  int slot = (int)(intptr_t)lv_event_get_user_data(e);
  g_acctDelArmed = -1;
  if (slot == g_accts.active) return;
  if (!switch_account(slot)) request_state(ST_ACCOUNTS);
}
static void acct_del_cb(lv_event_t *e) {
  int slot = (int)(intptr_t)lv_event_get_user_data(e);
  if (accountCount(g_accts) <= 1) return;
  if (g_acctDelArmed != slot) {
    g_acctDelArmed = slot;
    request_state(ST_ACCOUNTS);
    return;
  }
  g_acctDelArmed = -1;
  bool wasActive = (slot == g_accts.active);
  accountRemove(g_prefs, g_accts, slot);
  char pth[16]; snprintf(pth, sizeof(pth), "/hist%d.bin", slot);
  LittleFS.remove(pth);
  if (wasActive) {
    EncryptedBlob b; char tok[200];
    if (accountLoadBlob(g_prefs, g_accts.active, b) &&
        decryptToken(b, deviceSecret(), tok, sizeof(tok))) {
      g_blob = b;
      strlcpy(g_token, tok, sizeof(g_token));
      memset(tok, 0, sizeof(tok));
      memset(&g_tok, 0, sizeof(g_tok));
      memset(&g_usage, 0, sizeof(g_usage));
      request_state(ST_LOADING);
      return;
    }
  }
  request_state(ST_ACCOUNTS);
}
static void acct_add_cb(lv_event_t *e) {
  (void)e;
  int slot = accountFirstFree(g_accts);
  if (slot < 0) return;
  g_tokenTargetSlot = slot;
  g_pendingLabel[0] = 0;
  request_state(ST_TOKEN);
}
static int g_renameSlot = -1;
static lv_obj_t *g_nameTa = nullptr;

static void acct_edit_cb(lv_event_t *e) {
  g_renameSlot = (int)(intptr_t)lv_event_get_user_data(e);
  g_acctDelArmed = -1;
  request_state(ST_ACCT_NAME);
}
static void acct_name_kb_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    accountSetLabel(g_prefs, g_accts, g_renameSlot, lv_textarea_get_text(g_nameTa));
    request_state(ST_ACCOUNTS);
  } else if (code == LV_EVENT_CANCEL) {
    request_state(ST_ACCOUNTS);
  }
}

static void ui_account_name() {
  if (g_renameSlot < 0 || g_renameSlot >= ACCT_MAX || !g_accts.used[g_renameSlot]) {
    request_state(ST_ACCOUNTS);
    return;
  }
  lv_obj_t *scr = lv_screen_active();

  lv_obj_t *title = mklabel(scr, TRS("Renomear conta", "Rename account"),
                            &lv_font_montserrat_16, C_TEXT);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 8);

  lv_obj_t *bk = mkbtn(scr, TRS(LV_SYMBOL_LEFT " Voltar", LV_SYMBOL_LEFT " Back"),
                       &lv_font_montserrat_12, C_SURFACE2, C_MUTED);
  lv_obj_set_size(bk, 84, 30);
  lv_obj_set_ext_click_area(bk, 6);
  lv_obj_align(bk, LV_ALIGN_TOP_RIGHT, -6, 4);
  lv_obj_add_event_cb(bk, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ST_ACCOUNTS);

  g_nameTa = lv_textarea_create(scr);
  lv_textarea_set_one_line(g_nameTa, true);
  lv_textarea_set_max_length(g_nameTa, ACCT_LBL_MAX - 1);
  lv_textarea_set_text(g_nameTa, g_accts.label[g_renameSlot]);
  lv_textarea_set_placeholder_text(g_nameTa, TRS("rotulo (ex.: Pessoal, Trabalho)",
                                                 "label (e.g. Personal, Work)"));
  lv_obj_set_size(g_nameTa, 304, 38);
  lv_obj_align(g_nameTa, LV_ALIGN_TOP_MID, 0, 40);

  lv_obj_t *kb = lv_keyboard_create(scr);
  // Mesmo recuo do teclado do WiFi: a tecla de confirmar nao pode cair na
  // zona morta do painel (ver TOUCH_DEAD_Y).
  lv_obj_set_size(kb, SCREEN_WIDTH, 130);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -(SCREEN_HEIGHT - TOUCH_SAFE_BOTTOM));
  lv_keyboard_set_textarea(kb, g_nameTa);
  lv_obj_add_event_cb(kb, acct_name_kb_cb, LV_EVENT_ALL, NULL);
}

static void ui_accounts() {
  lv_obj_t *scr = lv_screen_active();
  start_data_web();

  lv_obj_t *title = mklabel(scr, TRS("Contas", "Accounts"), &lv_font_montserrat_16, C_TEXT);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 8);

  lv_obj_t *bk = mkbtn(scr, TRS(LV_SYMBOL_LEFT " Voltar", LV_SYMBOL_LEFT " Back"),
                       &lv_font_montserrat_12, C_SURFACE2, C_MUTED);
  lv_obj_set_size(bk, 84, 30);
  lv_obj_set_ext_click_area(bk, 6);
  lv_obj_align(bk, LV_ALIGN_TOP_RIGHT, -6, 4);
  lv_obj_add_event_cb(bk, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ST_SETTINGS);

  lv_obj_t *lst = lv_obj_create(scr);
  lv_obj_set_pos(lst, 8, 38);
  lv_obj_set_size(lst, 304, 168);
  lv_obj_set_style_bg_opa(lst, 0, 0);
  lv_obj_set_style_border_width(lst, 0, 0);
  lv_obj_set_style_pad_all(lst, 0, 0);
  lv_obj_set_style_pad_row(lst, 8, 0);
  lv_obj_set_flex_flow(lst, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(lst, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(lst, LV_SCROLLBAR_MODE_AUTO);

  bool canDelete = accountCount(g_accts) > 1;
  for (int i = 0; i < ACCT_MAX; i++) {
    if (!g_accts.used[i]) continue;
    bool active = (i == g_accts.active);

    lv_obj_t *row = lv_obj_create(lst);
    lv_obj_set_size(row, 296, 44);
    no_box(row);

    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_size(b, canDelete ? 188 : 240, 44);
    lv_obj_set_pos(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_SURFACE), 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    char txt[40];
    snprintf(txt, sizeof(txt), "%s%s", g_accts.label[i],
             active ? TRS("  \xE2\x80\xA2  ativa", "  \xE2\x80\xA2  active") : "");
    lv_obj_t *l = mklabel(b, txt, &lv_font_montserrat_14, active ? C_ACCENT : C_TEXT);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(b, acct_switch_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *ed = lv_button_create(row);
    lv_obj_set_size(ed, 48, 44);
    lv_obj_set_pos(ed, canDelete ? 196 : 248, 0);
    lv_obj_set_style_bg_color(ed, lv_color_hex(C_SURFACE2), 0);
    lv_obj_set_style_radius(ed, 12, 0);
    lv_obj_set_style_shadow_width(ed, 0, 0);
    lv_obj_center(mklabel(ed, LV_SYMBOL_EDIT, &lv_font_montserrat_14, C_MUTED));
    lv_obj_add_event_cb(ed, acct_edit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    if (canDelete) {
      bool armed = (g_acctDelArmed == i);
      lv_obj_t *d = lv_button_create(row);
      lv_obj_set_size(d, 48, 44);
      lv_obj_set_pos(d, 248, 0);
      lv_obj_set_style_bg_color(d, lv_color_hex(armed ? C_BAD : C_SURFACE2), 0);
      lv_obj_set_style_radius(d, 12, 0);
      lv_obj_set_style_shadow_width(d, 0, 0);
      lv_obj_center(mklabel(d, armed ? LV_SYMBOL_WARNING : LV_SYMBOL_TRASH,
                            &lv_font_montserrat_14, armed ? C_BG : C_MUTED));
      lv_obj_add_event_cb(d, acct_del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
  }

  if (accountFirstFree(g_accts) >= 0) {
    lv_obj_t *add = lv_button_create(lst);
    lv_obj_set_size(add, 296, 44);
    lv_obj_set_style_bg_color(add, lv_color_hex(C_SURFACE2), 0);
    lv_obj_set_style_radius(add, 12, 0);
    lv_obj_set_style_shadow_width(add, 0, 0);
    lv_obj_t *al = mklabel(add, TRS(LV_SYMBOL_PLUS "  Adicionar conta",
                                    LV_SYMBOL_PLUS "  Add account"),
                           &lv_font_montserrat_14, C_ACCENT);
    lv_obj_align(al, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(add, acct_add_cb, LV_EVENT_CLICKED, NULL);
  }

  lv_obj_t *hint = mklabel(scr, TRS("So a conta ativa e consultada na API (as outras ficam dormentes).",
                                    "Only the active account is polled (the others stay dormant)."),
                           &lv_font_montserrat_12, C_FAINT);
  lv_obj_set_width(hint, 300);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 10, -4);
}

// ============================================================
// Tela: sobre / about
// ============================================================
static void ui_about() {
  lv_obj_t *scr = lv_screen_active();
  start_data_web();

  lv_obj_t *bk = mkbtn(scr, TRS(LV_SYMBOL_LEFT " Voltar", LV_SYMBOL_LEFT " Back"),
                       &lv_font_montserrat_12, C_SURFACE2, C_MUTED);
  lv_obj_set_size(bk, 84, 30);
  lv_obj_set_ext_click_area(bk, 6);
  lv_obj_align(bk, LV_ALIGN_TOP_RIGHT, -6, 4);
  lv_obj_add_event_cb(bk, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ST_SETTINGS);

  lv_obj_t *mark = pa_mount(scr, 4);               // 80x80
  lv_obj_align(mark, LV_ALIGN_TOP_MID, 0, 2);
  pa_set(PA_EXPRESSION_WINK);

  lv_obj_t *t = mklabel(scr, "Claude Usage Stick", &lv_font_montserrat_18, C_TEXT);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 92);

  char v[64];
  snprintf(v, sizeof(v), "v" FW_VERSION " \xE2\x80\xA2 ESP32 \xE2\x80\xA2 LVGL 9.2");
  lv_obj_t *ver = mklabel(scr, v, &lv_font_montserrat_12, C_FAINT);
  lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 114);

  lv_obj_t *d = mklabel(scr, TRS("Uso do Claude Code em tempo real: janela de 5h e "
                                 "semanal, lidas direto dos cabecalhos da API.",
                                 "Live Claude Code usage: the 5-hour and weekly "
                                 "windows, read straight from the API headers."),
                        &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_width(d, 300);
  lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
  lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 132);

  lv_obj_t *h = mklabel(scr, TRS("Tela: CYD ESP32-2432S028 \xE2\x80\xA2 2.8\" 320x240 touch (ST7789/XPT2046)",
                                 "Display: CYD ESP32-2432S028 \xE2\x80\xA2 2.8\" 320x240 touch (ST7789/XPT2046)"),
                        &lv_font_montserrat_12, C_FAINT);
  lv_obj_set_width(h, 300);
  lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 168);

  lv_obj_t *devCap = mklabel(scr, TRS("Desenvolvido por", "Developed by"), &lv_font_montserrat_12, C_FAINT);
  lv_obj_align(devCap, LV_ALIGN_TOP_MID, 0, 196);
  lv_obj_t *dev = mklabel(scr, "Benevid Felix", &lv_font_montserrat_14, C_TEXT);
  lv_obj_align(dev, LV_ALIGN_TOP_MID, 0, 212);
  lv_obj_t *mail = mklabel(scr, "benevid@gmail.com", &lv_font_montserrat_12, C_ACCENT);
  lv_obj_align(mail, LV_ALIGN_TOP_MID, 0, 228);
}

// ============================================================
// Navegação genérica
// ============================================================
static void nav_cb(lv_event_t *e) {
  State s = (State)(intptr_t)lv_event_get_user_data(e);
  request_state(s);
}

// ============================================================
// Render do estado atual
// ============================================================
static void render_state() {
  g_state = g_pending;
  stop_web();                                 // cada tela sobe o servidor que precisa
  lv_obj_clean(lv_layer_top());
  // invalida ponteiros vivos antes de destruir a tela antiga
  memset(&g_ui, 0, sizeof(g_ui));
  g_pinDots = g_pinMsg = nullptr;
  g_tokMsg = nullptr;
  g_nameTa = nullptr;
  g_hdrStatus = nullptr;
  g_briLbl = g_wipeLbl = g_pollLbl = g_tzLbl = nullptr;

  lv_obj_clean(lv_screen_active());
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

  switch (g_state) {
    case ST_UNLOCK:    ui_unlock(); break;
    case ST_PARTY:     ui_party(); break;
    case ST_PIN:       ui_pin(); break;
    case ST_WIFI:      ui_wifi(); break;
    case ST_TOKEN:     ui_token(); break;
    case ST_LOADING:   ui_loading(g_wifi.isConnected() ? g_wifi.getSSID().c_str()
                                                       : TRS("conectando WiFi", "connecting WiFi")); break;
    case ST_MAIN:      ui_main(); break;
    case ST_SETTINGS:  ui_settings(); break;
    case ST_ACCOUNTS:  ui_accounts(); break;
    case ST_ACCT_NAME: ui_account_name(); break;
    case ST_ABOUT:     ui_about(); break;
    case ST_ERROR:     ui_message(TRS("Falha", "Failed"),
                                  g_usage.error[0] ? g_usage.error : TRS("sem dados", "no data"), C_BAD); break;
    default: break;
  }
}

// ============================================================
// Tempo (NTP) e ciclo de dados
// ============================================================
static void apply_tz() {
  configTime(g_tzOffset * 3600, 0, NTP_SERVER_1, NTP_SERVER_2);
  const char *tz = getenv("TZ");
  Serial.printf("[TZ] offset=%+d  TZ=\"%s\"\n", g_tzOffset, tz ? tz : "(vazio)");
}
static void ensure_time() {
  if (g_timeInit || !g_wifi.isConnected()) return;
  apply_tz();
  g_timeInit = true;
  Serial.println("[NTP] sync iniciado");
}


// Primeiro load (mostra a tela de carregamento). Vai p/ ST_MAIN ou ST_ERROR.
static void do_refresh() {
  ensure_time();
  bool ok = fetchUsage(g_token, g_usage);
  if (ok) {
    g_lastOkMs = millis(); g_lastFetchOk = true;
    check_thresholds();
  } else g_lastFetchOk = false;
  g_lastPollMs = millis();
  request_state(ok ? ST_MAIN : ST_ERROR);
}

// Atualização EM BACKGROUND: não troca de tela; mantém o dashboard e os dados
// antigos se falhar. A chamada à API é bloqueante (~1-2s), então mostra
// "atualizando..." no cabeçalho durante a busca.
static void bg_refresh() {
  if (!g_wifi.isConnected()) g_wifi.autoConnect(WIFI_CONNECT_TIMEOUT_MS);
  ensure_time();
  g_refreshing = true; set_hdr_status(); lv_refr_now(NULL);
  Serial.printf("[MEM] antes do fetch: livre=%u  maior bloco=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  UsageData u = {};
  bool ok = fetchUsage(g_token, u);
  if (ok) {
    g_usage = u; g_lastOkMs = millis(); g_lastFetchOk = true;
    check_thresholds();
  } else g_lastFetchOk = false;
  g_refreshing = false;
  g_lastPollMs = millis();
  refresh_ui_values();                    // in-place: nao remonta a tela
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Claude Usage Stick (touch) ===");

  // Display: ST7789 em SPI de hardware (HSPI), com a rotacao feita no driver.
  Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO, HSPI);
  gfx = new Arduino_ST7789(bus, TFT_RST, TFT_ROTATION, false, PANEL_WIDTH, PANEL_HEIGHT);
  if (!gfx->begin(SPI_FREQ)) { Serial.println("FATAL display"); while (1) delay(1000); }
  gfx->fillScreen(0x0000);

  // Backlight via PWM (brilho ajustável)
  ledcAttach(TFT_BL, 5000, 8);
  touch_dev.begin();

  // LVGL
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  // Dois buffers parciais na RAM interna. O original alocava a tela inteira em
  // PSRAM e usava RENDER_MODE_FULL; aqui nao ha PSRAM, e o maior bloco
  // contiguo da placa (~110 KB) nem comportaria o framebuffer de 153 KB.
  //
  // O tamanho vem de LVGL_BYTES_PER_PX, nao de sizeof(lv_color_t): na LVGL 9
  // lv_color_t e uma struct RGB888 de 3 bytes, mas o formato de render deste
  // display e RGB565, de 2. Usar o sizeof reservava 50% a mais de RAM do que o
  // LVGL chega a usar — desperdicio que numa placa sem PSRAM custa caro, e que
  // aqui era a diferenca entre o TLS conectar ou nao.
  uint32_t bufSize = SCREEN_WIDTH * LVGL_BUF_LINES * LVGL_BYTES_PER_PX;
  // Buffer UNICO, nao duplo: a verificacao da cadeia TLS da Anthropic
  // (ECDSA P-384 + SHA-384) precisa de blocos grandes, e o segundo buffer
  // custava 20 KB que fazem falta exatamente no momento do handshake.
  lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(bufSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  lv_color_t *buf2 = nullptr;
  if (!buf1) { Serial.println("FATAL buffers LVGL"); fatal_screen("RAM insuficiente"); }
  Serial.printf("[MEM] buffer LVGL: 1 x %u B  heap livre=%u  maior bloco=%u\n",
                (unsigned)bufSize, (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
  lv_display_t *disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_display_set_flush_cb(disp, disp_flush_cb);
  lv_display_set_buffers(disp, buf1, buf2, bufSize, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);

  load_persisted();
  apply_brightness();
  boot_splash(TRS("Iniciando...", "Starting..."));

  if (!LittleFS.begin(true)) Serial.println("LittleFS: falhou");

  g_wifi.begin();

  boot_status(TRS("Conectando ao WiFi...", "Connecting to WiFi..."));
  if (g_hasToken) {
    g_wifi.autoConnect(WIFI_CONNECT_TIMEOUT_MS, boot_wifi_tick);
    // A chave do chip abre o token sem perguntar nada. Se nao abrir, o token
    // foi cifrado pela versao com PIN: pede o PIN uma vez e regrava.
    if (decryptToken(g_blob, deviceSecret(), g_token, sizeof(g_token))) {
      Serial.printf("[AUTH] token aberto pela chave do chip (%d chars)\n", (int)strlen(g_token));
      request_state(ST_UNLOCK);
    } else {
      Serial.println("[AUTH] chave do chip nao abriu: token da versao com PIN, migrando");
      g_migratingPin = true;
      request_state(ST_PIN);
    }
  } else {
    g_onboarding = true;
    // Se já há WiFi salvo (reboot no meio do onboarding), pula direto p/ o token
    request_state(g_wifi.autoConnect(WIFI_CONNECT_TIMEOUT_MS, boot_wifi_tick) ? ST_TOKEN : ST_WIFI);
  }
  g_bootSub = nullptr;                     // render_state() destroi a tela de boot
}

void loop() {
  lv_task_handler();

  // Servidor web (token no onboarding; /window + /tokens no dashboard)
  if (g_web) {
    g_web->handleClient();
    if (g_state == ST_TOKEN && g_tokenGot) {
      g_tokenGot = false;
      finalize_pending_token();
    }
  }

  if (g_dirty) {
    g_dirty = false;
    render_state();
    if (g_state == ST_LOADING) {
      lv_task_handler();
      lv_refr_now(NULL);
      do_refresh();
    }
  }

  // Poll automático EM BACKGROUND (sem trocar de tela) + refresh manual
  if (g_state == ST_MAIN &&
      (g_wantRefresh || millis() - g_lastPollMs > (uint32_t)g_pollSec * 1000)) {
    g_wantRefresh = false;
    bg_refresh();           // seta g_lastPollMs no fim
  }

  // Atualização viva: contadores de reset (1s) e anel do próximo refresh (250ms)
  pa_tick();                    // a animacao roda em qualquer tela que a tenha

  if (g_state == ST_MAIN) {
    uint32_t now = millis();
    static uint32_t lastTick = 0, lastBar = 0;
    if (now - lastTick > 1000) { lastTick = now; dash_tick(); update_tok_row(); }
    if (now - lastBar > 250 && g_ui.refArc) {
      lastBar = now;
      int v;
      if (g_refreshing) v = 1000;
      else {
        uint32_t el = now - g_lastPollMs, per = (uint32_t)g_pollSec * 1000;
        v = el >= per ? 0 : (int)(1000 - (uint64_t)el * 1000 / per);
      }
      lv_arc_set_value(g_ui.refArc, v);
    }

  }

  delay(5);
}
