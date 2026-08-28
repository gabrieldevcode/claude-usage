# A placa: CYD ESP32-2432S028

Este documento descreve a placa **que está na mesa**, não um modelo tirado de
catálogo. Cada linha aqui saiu de uma leitura feita na própria placa, e a coluna
da direita explica o que aquilo significa na prática para este firmware.

Os logs brutos de cada medição estão em [`logs/`](logs/):

| Log | O que contém |
|---|---|
| [`01-deteccao-chip-i2c-painel.log`](logs/01-deteccao-chip-i2c-painel.log) | chip, memória, varredura I²C, leitura crua do ID do painel |
| [`02-deteccao-madctl-roundtrip.log`](logs/02-deteccao-madctl-roundtrip.log) | round-trip de MADCTL — a prova de que o MISO está ligado |
| [`03-calibracao-touch-xpt2046.log`](logs/03-calibracao-touch-xpt2046.log) | os quatro pontos de calibração e as constantes resultantes |

Os sketches que produziram esses logs estão em
[`../firmware/detect/`](../firmware/detect/) e
[`../firmware/bringup_cyd/`](../firmware/bringup_cyd/).

---

## 1. Identificação

A serigrafia do PCB diz **`ESP32-2432S028`** (sem o sufixo `R`). A etiqueta da
caixa em que a placa veio diz **`2.8 TFT · 240 RGB · 7789 · 60609`**.

O firmware de fábrica que veio gravado confirma a origem: o dump dos 4 MB tem a
string de caminho de compilação

```
C:\Users\Administrator\Desktop\JYC\demo\libraries\TFT_eSPI\Processors/TFT_eSPI_ESP32.c
```

junto com `I am LVGL_Arduino`, `LVGL v8` e `Widgets demo` — o demo de fábrica
padrão do fabricante (JYC/Sunton) dessa família de placas. O descritor da imagem
diz `esp32-arduino-lib-builder`, IDF `v3.3.5`, compilado em **26/03/2021**.

> **A `nvs` e a `spiffs` de fábrica estavam inteiramente apagadas** (todos os
> bytes `0xFF`). A placa nunca tinha sido configurada, então regravá-la não
> destruiu nada.

---

## 2. Processador e memória

Lido com `esptool chip-id`, `espefuse summary` e, já rodando na placa, pelas
funções `ESP.*` do core Arduino.

| Item | Valor medido | O que significa para este projeto |
|---|---|---|
| Chip | **ESP32-D0WD-V3**, revisão v3.1 | ESP32 clássico (Xtensa LX6), não o S3 do projeto original. Muda o FQBN, o core compilado e todo o pipeline de vídeo. |
| Núcleos / clock | 2 núcleos, **240 MHz** | Folga de CPU. O LVGL roda no loop cooperativo sem apertar; o segundo núcleo fica com o stack de Wi-Fi. |
| Rádio | Wi-Fi + BT + BLE | O firmware só usa Wi-Fi. BT/BLE ficam desligados e liberam RAM. |
| Package (`PKG_VERSION`) | `1` → **D0WD** | Encapsulamento **sem** flash nem PSRAM embutidas. A flash é um chip externo; PSRAM não existe. |
| Cristal | 40 MHz | Padrão. Nenhum ajuste de clock necessário. |
| MAC | `30:76:f5:e9:23:a0` | Identifica a placa; vira também o hostname mDNS. |
| **PSRAM** | **0 bytes** (`ESP.getPsramSize()`) | **A restrição mais dura do port.** O firmware original aloca o framebuffer do LVGL em PSRAM (`MALLOC_CAP_SPIRAM`) e usa `LV_DISPLAY_RENDER_MODE_FULL`. Aqui isso é impossível — tem de virar buffer parcial na RAM interna. |
| Heap total | 367.704 bytes | — |
| Heap livre no boot | **321.752 bytes** | É o orçamento inteiro: buffers do LVGL + objetos da UI + handshake TLS + buffers de Wi-Fi. |
| Maior bloco contíguo | **110.580 bytes** | Teto de uma alocação única. Um framebuffer 320×240×2 = 153.600 bytes **não caberia nem se a heap total permitisse** — o bloco contíguo não existe. Confirma buffers parciais. |
| Calibração ADC | `ADC_VREF = 1114 mV` em eFuse | A leitura do LDR sai calibrada de fábrica, sem precisar de ajuste em software. |
| Segurança | Sem flash encryption, sem secure boot, JTAG e download UART habilitados | A placa é livre para gravar e depurar. Nenhum fusível queimado atrapalha. |

### O que a ausência de PSRAM custa, em números

```
original (JC4832W535, 8 MB PSRAM)     esta placa (sem PSRAM)
─────────────────────────────────     ──────────────────────────────
Arduino_Canvas 320×480×2 = 300 KB     não existe (desenha direto no driver)
buffer LVGL   480×320×2 = 300 KB      2 × (320×40×2) = 50 KB
RENDER_MODE_FULL                      RENDER_MODE_PARTIAL
total ~600 KB em PSRAM                ~50 KB em RAM interna
```

---

## 3. Flash

| Item | Valor medido | O que significa |
|---|---|---|
| Tamanho | **4 MB** (`flash-id` e `ESP.getFlashChipSize()`) | Um quarto dos 16 MB do projeto original. O `partitions.csv` tem de ser refeito. |
| Velocidade | 80 MHz | Modo rápido, sem penalidade de leitura de código. |
| Fabricante / device | `0x5e` / `0x4016` | — |
| Tensão | 3,3 V, definida por strapping | Sem risco de configuração errada de VDD_SDIO. |

### Particionamento de fábrica (o que veio na placa)

```
nvs       data nvs      0x009000   20 K
otadata   data ota      0x00e000    8 K
app0      app  ota_0    0x010000 1280 K
app1      app  ota_1    0x150000 1280 K
spiffs    data spiffs   0x290000 1472 K
```

O firmware **não usa OTA** — não há nenhuma referência a `Update.`, `esp_ota`,
`ArduinoOTA` ou `httpUpdate` em todo o código. A partição `app1` é 1,25 MB
parados. O port usa partição única e devolve esse espaço para o app, que é o
recurso apertado (LVGL + Arduino_GFX + Wi-Fi + TLS + os sprites embutidos).

---

## 4. Display

### Como o painel foi identificado

Ler o ID de um controlador de tela por SPI só vale alguma coisa se o MISO
estiver de fato ligado e o alinhamento de bits for conhecido — e em muitas
placas dessa família o SDO do painel simplesmente não é conectado. Então o
primeiro passo foi provar o barramento, não adivinhar o CI:

**Round-trip de MADCTL.** Escreve um valor conhecido no registrador `0x36`
(MADCTL) e lê de volta com `0x0B` (RDDMADCTL), extraindo o byte em todos os nove
alinhamentos de bit possíveis. Se o valor escrito voltar, o MISO está ligado e o
alinhamento está achado. Resultado nesta placa:

```
-- HSPI: SCK14 MOSI13 MISO12 CS15 DC2
   escrito 0x48 -> 0:48* 1:90 2:20 3:40 4:80 5:00 6:00 7:00 8:00
   escrito 0x28 -> 0:28* 1:50 2:A0 3:40 4:80 5:00 6:00 7:00 8:00
   >>> MISO LIGADO e alinhado em dummy=0 bits
   RDID1/2/3 (0xDA/DB/DC) = 0x81 0x81 0xB3

-- VSPI: SCK18 MOSI23 MISO19 CS5 DC2
   escrito 0x48 -> 0:FF 1:FF ... 8:FF
   >>> o MISO do painel nao esta ligado nesta placa
```

Dois valores diferentes voltaram exatos no mesmo alinhamento. O painel está no
**HSPI**, o MISO responde, e o ID de fabricante é **`0x81 0x81 0xB3`** — que não
é nem ILI9341 (`00 93 41`) nem ST7789 padrão (`85 85 52`). É um ID de fabricante
não padronizado, comum em painéis dessa faixa de preço.

Como o ID não decide, os dois inits candidatos foram desenhados na tela e
comparados a olho: barras de cor rotuladas, escada de cinza, moldura de 1 px
encostada nas bordas e os cantos nomeados. **Os dois renderizam corretamente** —
o que é esperado, já que ST7789 e ILI9341 compartilham a maior parte do conjunto
de comandos MIPI-DCS. O desempate veio da etiqueta da caixa, que diz `7789`.

**Driver adotado: `Arduino_ST7789`.**

### Configuração validada

| Item | Valor | O que significa |
|---|---|---|
| Controlador | **ST7789** (ID de fabricante `0x81 0x81 0xB3`) | Driver `Arduino_ST7789` da GFX Library for Arduino. |
| Interface | **SPI (HSPI)** a 40 MHz | O original é QSPI de 4 vias. Aqui é SPI de 1 via — menos banda, o que reforça a escolha de redesenhar só a região suja em vez da tela inteira. |
| Resolução nativa | 240 × 320 (retrato) | — |
| Resolução em uso | **320 × 240** (paisagem, `rotation = 3`) | Contra 480 × 320 do original: **58 % dos pixels**. Todo o layout, que é posicionado em coordenadas absolutas, precisa ser refeito. |
| Ordem de cor | BGR (`ips = false`) | Barras VERMELHO/VERDE/AZUL saíram nas cores certas e a escada de cinza vai de escuro para claro na ordem correta. |
| Confirmação da resolução | Moldura de 1 px fecha nos **quatro** lados | Se a resolução configurada fosse maior ou menor que a real, faltaria borda ou sobraria faixa preta. |
| Orientação | `rotation = 3` → **USB à esquerda** | Mesma convenção do projeto original. Confirmado com uma seta desenhada na tela apontando para a borda do conector, e com o texto legível na posição normal. |

### Pinos do display

```
TFT_SCK   14      TFT_CS   15
TFT_MOSI  13      TFT_DC    2
TFT_MISO  12      TFT_RST  -1   (sem reset por GPIO)
TFT_BL    21      (backlight, HIGH = aceso; PWM para o ajuste de brilho)
```

O backlight foi identificado piscando cada GPIO candidato e observando a tela.

---

## 5. Touch

A varredura I²C passou por cinco pares de pino candidatos
(`21/22`, `33/32`, `27/26`, `4/5`, `15/2`) e **não achou nenhum dispositivo**.
Isso descarta touch capacitivo — o original usa um AXS15231B capacitivo em
I²C `0x3B`.

O que existe é um **XPT2046 resistivo**, em barramento SPI próprio. A prova é
funcional, não inferida: com o dedo fora da tela, `Z1 = 0` e `Z2 ≈ 4091`; com o
dedo encostado, `Z1` sobe acima de 200 e `X`/`Y` passam a valores plausíveis.

```
TS_SCK  25    TS_CS   33
TS_MOSI 32    TS_IRQ  36
TS_MISO 39
```

### Calibração

Resistivo não devolve coordenada — devolve tensão. O mapeamento para pixels tem
de ser medido. Quatro alvos, um por vez, registrando na **soltura** do toque
(não na descida, que é transiente) e com mediana de 7 leituras (o resistivo é
ruidoso e um valor único mente):

| Alvo na tela | Cru X | Cru Y |
|---|---|---|
| (24, 24) | 3480 | 3573 |
| (296, 24) | 3381 | 377 |
| (24, 216) | 685 | 3597 |
| (296, 216) | 714 | 411 |

Entre os alvos da esquerda e os da direita, o cru **X** quase não mexe (−35)
enquanto o cru **Y** varia 3191. Ou seja: os eixos estão **trocados**, e ambos
**invertidos**. Extrapolando dos centros dos alvos até as bordas reais:

```c
#define TOUCH_SWAP_XY   1      // o cru Y alimenta a tela X
#define TOUCH_RAW_MIN_X 3867   // min > max: eixo invertido, e a conta linear
#define TOUCH_RAW_MAX_X 112    // lida com isso sozinha
#define TOUCH_RAW_MIN_Y 3771
#define TOUCH_RAW_MAX_Y 358
```

Verificado arrastando o dedo sobre uma grade: o ponto acompanha o dedo.

> ⚠️ **O canto inferior direito responde mal ao toque.** Confirmado na placa.
> É uma limitação física do painel resistivo, não do mapeamento. O layout
> portado não deve colocar alvo de toque crítico nesse canto.

---

## 6. Alimentação e USB

| Item | Valor | O que significa |
|---|---|---|
| Conversor USB-serial | **CH340** (`USB\VID_1A86&PID_7523`) | O original tem USB nativo no ESP32-S3. Aqui a serial passa por um CI externo: o FQBN não leva `CDCOnBoot` nem `USBMode`, e a porta some do sistema se o cabo for ruim. |
| Porta neste PC | `COM8` | Não é `COM6` — aquela é um CP210x de outro dispositivo, registrado mas ausente. |
| Reset / boot | Automáticos por DTR/RTS | Gravação sem apertar botão. |

---

## 7. Resumo do que muda no firmware

| | Original (JC4832W535) | Esta CYD | Consequência |
|---|---|---|---|
| Chip | ESP32-S3, USB nativo | ESP32-D0WD-V3, CH340 | Outro FQBN e outro core |
| Flash | 16 MB, app 3 MB + OTA 3 MB | **4 MB** | `partitions.csv` refeito, partição única |
| PSRAM | 8 MB OPI | **nenhuma** | Buffers parciais em vez de framebuffer cheio |
| Display | AXS15231B QSPI 480×320 | **ST7789 SPI 320×240** | Outro driver, outro barramento, 58 % dos pixels |
| Touch | AXS15231B capacitivo I²C | **XPT2046 resistivo SPI** | Outro driver + calibração medida |
| Render | `RENDER_MODE_FULL` + `Arduino_Canvas` | `RENDER_MODE_PARTIAL` direto no driver | `disp_flush_cb` reescrito |
| Orientação | USB à esquerda (giro manual 270° no flush) | USB à esquerda (`rotation = 3` no driver) | O giro sai do flush e vai para o driver |
| Layout | coordenadas absolutas em 480×320 | 320×240 | Todos os tiles e telas reposicionados |
