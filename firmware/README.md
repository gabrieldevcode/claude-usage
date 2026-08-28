# firmware/ — Claude Usage Stick na CYD ESP32-2432S028

Firmware para a **CYD `ESP32-2432S028`**: ESP32 clássico (D0WD-V3), 4 MB de
flash, **sem PSRAM**, tela **ST7789** 320×240 em SPI e touch **XPT2046**
resistivo. A especificação completa, medida na placa, está em
[`../docs/HARDWARE-CYD.md`](../docs/HARDWARE-CYD.md).

## O firmware

- **`claude_stick/`** — o projeto. Sketch `arduino-cli` completo: busca o uso do
  Claude nos cabeçalhos da `api.anthropic.com`, a saúde dos modelos em
  `status.claude.com`, e renderiza tudo em LVGL 9 com navegação por toque. Token
  OAuth guardado cifrado (AES-256-GCM + PIN). Build em
  [`claude_stick/build.ps1`](claude_stick/build.ps1) (Windows) ou
  [`claude_stick/build.sh`](claude_stick/build.sh).

## Os sketches que levantaram a placa

Estão aqui porque cada número do documento de hardware saiu de um deles. Se você
for portar para outra CYD, é por aqui que se começa — nenhum depende de
biblioteca externa a não ser onde indicado.

- **`detect/`** — detecção pura, **sem nenhuma biblioteca**. Varre o barramento
  I²C em 5 pares de pinos candidatos, lê o ID do painel por SPI *bit-bang*
  testando os 9 alinhamentos de bit de *dummy*, faz o *round-trip* de MADCTL que
  prova a ligação do MISO, varre backlight, LED RGB e LDR, e fica lendo o
  XPT2046. É o sketch que produziu a evidência de que o painel é ST7789 em HSPI.
- **`bringup_cyd/`** — bring-up visual e **calibração guiada de 4 pontos** do
  touch. Registra no *soltar* do dedo, com mediana de 7 amostras, detecta
  sozinho a troca de eixos comparando o movimento entre os alvos da esquerda e
  da direita, extrapola dos centros até as bordas e imprime os `#define` prontos
  para colar no `config.h`.
- **`orient/`** — resolve a ambiguidade de orientação sem depender de descrição
  em palavras: desenha uma seta apontando para uma borda específica e letra os
  cantos A/B/C/D, para a resposta não poder ser mal interpretada.
- **`touchmap/`** — divide a tela em células de 20 px e acende cada uma que
  responde ao toque. Foi ele que mediu a zona morta do painel (5 células de 192,
  todas em `y ≥ 220`), de onde vem a constante `TOUCH_SAFE_BOTTOM = 216`.

## Do projeto original

Mantidos como referência, **não** compilam para esta placa:

- **`bringup/`** — o bring-up do upstream, para a Guition JC4832W535 (ESP32-S3,
  AXS15231B QSPI 480×320).
- **`REFERENCIA-HARDWARE-LVGL.md`** — pinos, bibliotecas testadas e armadilhas
  daquela placa, incluindo a rotação de 270° feita à mão no flush e a exigência
  de PSRAM OPI. Aqui nada disso vale: a rotação é do driver e PSRAM não existe.

Comece por [`../README.md`](../README.md) para a visão geral e o passo a passo.
