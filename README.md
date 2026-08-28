<div align="center">

<img src="assets/brand/claudecode-color.svg" width="76" alt="Clawd">

# Claude Usage Stick — port para a CYD

**O consumo do seu Claude Code, ao vivo, numa telinha de 2,8".**<br>
Sem computador ligado, sem app, sem nuvem no meio.

</div>

> **Este repositório é um clone adaptado.** O firmware original é o
> [**claude-usage-stick-SVGL**](https://github.com/benevid/claude-usage-stick-SVGL),
> de [@benevid](https://github.com/benevid), que por sua vez nasceu do projeto de
> [@oauramos](https://github.com/oauramos). O que eu fiz foi **portar aquele
> firmware para outra placa** — uma CYD `ESP32-2432S028`, bem mais barata e bem
> mais limitada que a original. As funcionalidades são as mesmas; o que mudou foi
> tudo que encosta no hardware.

---

## Índice

- [O problema](#o-problema)
- [A placa](#a-placa)
- [Por que o firmware original não roda aqui](#por-que-o-firmware-original-não-roda-aqui)
- [O que precisou mudar](#o-que-precisou-mudar)
- [Compilar e gravar](#compilar-e-gravar)
- [Configurar a placa](#configurar-a-placa)
- [Como funciona por dentro](#como-funciona-por-dentro)
- [Arquivos](#arquivos)
- [Privacidade](#privacidade)
- [Avisos](#avisos)
- [Créditos e licença](#créditos-e-licença)

---

## O problema

Quem usa Claude Code na assinatura tem dois limites correndo ao mesmo tempo: uma
janela de 5 horas e uma janela semanal. Nenhum dos dois aparece em lugar nenhum
até o momento em que você bate na parede — no meio de uma tarefa, sem aviso.

Dá para checar rodando um comando e lendo cabeçalhos HTTP. Mas ninguém faz isso
de dez em dez minutos, e é justamente nos dez minutos que você não checou que o
limite estoura.

Uma telinha em cima da mesa resolve pela via mais boba possível: o número está
lá, o tempo todo, e você olha sem pedir. É só isso que este projeto é.

## A placa

Uma **CYD** — apelido de *Cheap Yellow Display*, aquelas placas ESP32 com tela
integrada que se acham por menos de R$ 100. O modelo aqui é o `ESP32-2432S028`
(sem o `R` final), de 2,8 polegadas.

Tudo abaixo foi **lido da placa**, não copiado de anúncio. Os logs brutos de cada
medição estão em [`docs/logs/`](docs/logs/), e o documento completo, com o que
cada linha significa na prática, em
[**`docs/HARDWARE-CYD.md`**](docs/HARDWARE-CYD.md).

| | |
|---|---|
| Chip | ESP32-D0WD-V3, revisão v3.1 — Xtensa LX6, **não** é o S3 |
| Núcleos / clock | 2 × 240 MHz |
| Flash | **4 MB** (externa; o encapsulamento D0WD não traz flash nem PSRAM) |
| PSRAM | **nenhuma** — a restrição que manda em todo o port |
| Heap livre no boot | 321.752 B · maior bloco contíguo 110.580 B |
| Display | **ST7789**, 320×240, SPI (HSPI) a 40 MHz, `rotation 3` (USB à esquerda) |
| Touch | **XPT2046** resistivo, SPI bit-bang, calibrado a 4 pontos |
| USB-serial | CH340 |
| Cristal | 40 MHz · `ADC_VREF` de fábrica: 1114 mV |

O painel não se identificou sozinho: o `RDID` respondeu `0x81 0x81 0xB3`, que não
bate com ILI9341 nem com ST7789. Em vez de chutar, o barramento foi provado com
um *round-trip* de MADCTL (escreve `0x36`, lê de volta em `0x0B`) e os dois inits
candidatos foram desenhados na tela para comparação visual. O desempate veio da
caixa do produto: `2.8 TFT, 240 RGB, 7789`.

### O touch tem uma zona morta, e ela foi medida

O painel resistivo desta unidade **não responde num canto**. Isso não foi
estimado — um sketch dividiu a tela em 192 células de 20 px e registrou quais
respondiam ao dedo:

```
5 de 192 células mortas, todas em y >= 220:
  um bloco de 80x20 px em x >= 240 (canto inferior direito)
  mais uma célula isolada em x = 180..199
```

Daí sai a constante `TOUCH_SAFE_BOTTOM = 216`: **nenhum alvo de toque pode
depender daquela faixa.** Os dois teclados, a matriz do PIN e as listas roláveis
param em `y = 216` por causa disso, não por estética. Sem essa regra, o botão de
confirmar do teclado cai exatamente no ponto que não clica — e não há como enviar
a senha do Wi-Fi.

## Por que o firmware original não roda aqui

| | Original (JC4832W535) | Esta CYD |
|---|---|---|
| Chip | ESP32-S3, USB nativo | ESP32-D0WD-V3, CH340 |
| Flash | 16 MB (app 3 MB + OTA 3 MB) | **4 MB** |
| PSRAM | 8 MB OPI | **nenhuma** |
| Display | AXS15231B QSPI 480×320 | ST7789 SPI 320×240 |
| Touch | AXS15231B capacitivo I²C | XPT2046 resistivo SPI |
| Render | `RENDER_MODE_FULL` + `Arduino_Canvas` | `RENDER_MODE_PARTIAL`, direto no driver |
| Layout | coordenadas absolutas em 480×320 | 320×240 — 58 % dos pixels |

O ponto mais duro é a memória. O `setup()` original fazia:

```cpp
gfx = new Arduino_Canvas(320, 480, g, 0, 0, 0);                          // 300 KB
lv_color_t *buf = heap_caps_malloc(480*320*2, MALLOC_CAP_SPIRAM | ...);  // + 300 KB
lv_display_set_buffers(disp, buf, NULL, bufSize, LV_DISPLAY_RENDER_MODE_FULL);
```

600 KB de buffers numa placa cujo maior bloco contíguo é 110 KB. Um framebuffer
de 320×240×2 = 153.600 B **não caberia nem se a heap total permitisse** — o bloco
não existe.

## O que precisou mudar

- **Particionamento.** 4 MB, sem OTA (o firmware nunca usou: zero referências a
  `Update.`, `esp_ota`, `ArduinoOTA` ou `httpUpdate`), partição única de app de
  3 MB. Ver [`partitions.csv`](firmware/claude_stick/partitions.csv).
- **Toolchain.** FQBN de ESP32 clássico, sem `PSRAM=opi` e sem `CDCOnBoot`. Como
  o desenvolvimento foi no Windows, entraram um `build.ps1` e um `flash.ps1` ao
  lado dos `.sh` do upstream, que continuam lá.
- **Pipeline de vídeo.** `Arduino_ESP32SPI` + `Arduino_ST7789` no lugar do QSPI +
  AXS15231B. O `Arduino_Canvas` sumiu; a rotação saiu do `disp_flush_cb` — que
  fazia um giro de 270° pixel a pixel — e foi para o driver. O flush virou um
  `draw16bitRGBBitmap` da região suja, e ficou mais curto que o original.
- **Touch.** Classe `XPT2046_Touch` no lugar da `AXS15231B_Touch`, com a **mesma
  interface** `begin()/touched()/readData()` — assim nada no resto do firmware
  precisou saber da troca. Filtro de mediana de 7 amostras e calibração de 4
  pontos com detecção automática de troca e inversão de eixos.
- **Layout inteiro reposicionado** para 320×240: os 4 tiles, Ajustes, Contas,
  PIN, token e os overlays. Os sprites do Clawd são ampliados com
  `lv_image_set_scale()` em vez de regerados — o gerador de assets do upstream
  depende do `rsvg-convert`, que não existe aqui.
- **LVGL sem pool estático.** `LV_USE_STDLIB_MALLOC` passou de `LV_STDLIB_BUILTIN`
  para `LV_STDLIB_CLIB`: o pool de 96 KB estourava o `dram0_0_seg` em 47.776 bytes
  já no link.

### A correção que custou mais caro

A busca na API funcionava **três vezes** depois do boot e falhava para sempre
depois disso, com `HTTP -1`. Foram descartados um a um, com número: Wi-Fi
(RSSI −49 dBm), DNS (resolve em 0 ms), TCP na 443 (conecta em 15 ms), relógio
(sincronizado, 118 s atrás do PC) e o próprio bundle de certificados — o OpenSSL
fecha a cadeia real da Anthropic com os **mesmos 3 certificados** de `certs.cpp`.

O que decidiu foi repetir a mesma requisição, no mesmo instante, com
`setInsecure()`: passou, com dados reais. A única diferença entre as duas era
percorrer a cadeia.

A causa era memória — mas não a que se mede com `getFreeHeap()`. A cadeia da
Anthropic é ECDSA P-384 com SHA-384, e verificá-la no mbedtls exige blocos
**contíguos** grandes. O limiar, medido nesta placa:

| Maior bloco contíguo | Validação da cadeia |
|---|---|
| 53 KB | falha sempre |
| **78 KB** | passa |

As três buscas que davam certo aconteciam antes de o dashboard existir, quando
ainda sobravam ~110 KB de bloco. Corrigido com buffer único de 24 linhas e o
cálculo certo de bytes por pixel — na LVGL 9 o `lv_color_t` é uma struct RGB888
de 3 bytes, mas o formato de render é RGB565, de 2, e dimensionar por
`sizeof(lv_color_t)` reservava 50 % a mais do que a LVGL chega a usar. Bloco
contíguo: **32.756 B → 77.812 B**.

> Se você for mexer neste firmware: confira o log `[MEM] ... maior bloco` depois
> de montar a tela. Abaixo de ~78 KB a API volta a falhar, e o erro que aparece
> (`HTTP -1`) não menciona memória em momento nenhum. O detalhamento está no
> [documento de hardware](docs/HARDWARE-CYD.md).

## Compilar e gravar

Precisa de [`arduino-cli`](https://arduino.github.io/arduino-cli/) — se você tem
o Arduino IDE instalado, já tem um embutido, e o `build.ps1` acha sozinho.

```bash
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install "lvgl@9.2.2"
arduino-cli lib install "GFX Library for Arduino@1.6.5"
```

**Windows:**

```powershell
cd firmware\claude_stick
.\build.ps1                  # só compila
.\build.ps1 upload           # compila e grava (acha a porta do CH340 sozinho)
.\build.ps1 monitor COM8     # serial a 115200
```

**macOS / Linux:**

```bash
cd firmware/claude_stick
./build.sh
./build.sh upload
```

O FQBN é este, e cada opção importa:

```
esp32:esp32:esp32:FlashSize=4M,PartitionScheme=custom,CPUFreq=240,FlashFreq=80,FlashMode=qio
```

Dois detalhes que custam tempo se você não souber:

- `PartitionScheme=custom` exige o `partitions.csv` **dentro da pasta do
  sketch**. Sem ele o build usa o layout padrão e a NVS cai em outro offset.
- O `-DLV_CONF_INCLUDE_SIMPLE -I<pasta do sketch>` precisa ir em
  `compiler.c.extra_flags`, `compiler.cpp.extra_flags` **e**
  `compiler.S.extra_flags`. O core do ESP32 monta os `.S` da LVGL com o terceiro,
  e sem ele o `lv_conf.h` não é encontrado na hora de assemblar. Os dois scripts
  de build já passam nos três.

## Configurar a placa

Nada disso entra no código-fonte. Tudo é digitado na placa ou no navegador.

1. **Wi-Fi** — a placa abre a tela de scan; escolha a rede e digite a senha no
   teclado da tela. Guarda até 3 redes na NVS.
2. **Token** — gere com `claude setup-token`, que devolve um token no formato
   `sk-ant-oat01-…`. A placa sobe um servidor web local e mostra o endereço; abra
   `http://claude-stick.local` (ou o IP) e cole o token no formulário.
3. **PIN de 4 dígitos** — o token é cifrado em AES-256-GCM com chave derivada
   dele, e o PIN é pedido a cada boot.

Opcional: `tools/token_bridge.py` roda **na sua máquina**, lê os transcripts
locais do Claude Code (`~/.claude/projects/**/*.jsonl`) e envia a contagem real
de tokens da janela para a placa. Existe porque a API não expõe contagem de
tokens para conta de assinatura — os cabeçalhos só trazem porcentagem.

```bash
python tools/token_bridge.py --host claude-stick.local --loop 120
```

## Como funciona por dentro

Não há serviço no meio. A placa fala direto com a Anthropic:

```
   ┌─────────┐   POST /v1/messages          ┌──────────────────┐
   │  CYD    │   max_tokens: 1              │  api.anthropic   │
   │ ESP32   │ ───────────────────────────► │      .com        │
   │         │ ◄─────────────────────────── │                  │
   └────┬────┘   corpo descartado;          └──────────────────┘
        │        o que interessa são os cabeçalhos
        │
        │        anthropic-ratelimit-unified-5h-utilization
        │        anthropic-ratelimit-unified-5h-reset
        │        anthropic-ratelimit-unified-7d-utilization
        │        anthropic-ratelimit-unified-7d-reset
        │        anthropic-ratelimit-unified-status
        ▼
   ┌─────────────────────────────────────────┐
   │  LVGL 9.2 · 4 telas com swipe           │
   │  Agora · Modelos · Janela 5h · Ritmo    │
   └─────────────────────────────────────────┘
```

O truque central — e é do projeto original — é pedir uma resposta de
`max_tokens: 1`, jogar o corpo fora e ler só os cabeçalhos de rate limit. Custa
praticamente nada e devolve exatamente o que interessa.

Também há uma sonda real por ciclo, rotacionando entre Haiku, Sonnet, Opus e
Fable, que registra código HTTP e latência de cada um; e uma consulta a
`status.claude.com` a cada 5 minutos, para incidentes.

## Arquivos

| Caminho | Papel |
|---|---|
| `firmware/claude_stick/` | o firmware — sketch principal + 8 módulos |
| `firmware/claude_stick/config.h` | pinos, calibração do touch, paleta, intervalos |
| `firmware/claude_stick/touch.h` | driver do XPT2046 (substitui o do AXS15231B) |
| `firmware/claude_stick/api.cpp` | a requisição e o parse dos cabeçalhos |
| `firmware/claude_stick/crypto.cpp` | AES-256-GCM do token, chave derivada do PIN |
| `firmware/detect/` | detecção de hardware, sem nenhuma biblioteca externa |
| `firmware/bringup_cyd/` | bring-up visual + calibração guiada de 4 pontos |
| `firmware/orient/` | resolve a ambiguidade de orientação com uma seta na tela |
| `firmware/touchmap/` | a varredura célula a célula que mediu a zona morta |
| `docs/HARDWARE-CYD.md` | a especificação completa da placa |
| `docs/logs/` | as capturas de serial de onde cada número saiu |
| `tools/token_bridge.py` | contagem real de tokens, a partir dos transcripts locais |
| `3D Case/` | as caixas do projeto original (ainda nas medidas da placa original) |

## Privacidade

- **Nada seu está neste repositório.** Wi-Fi, token e PIN são digitados na placa
  ou no formulário local, e ficam na NVS dela.
- Nenhum segredo entra no build: não há valor padrão embutido em lugar nenhum.
- O `.gitignore` cobre `.env`, `secrets.h`, `*.token` e a config local de
  ferramentas.
- Os logs em `docs/logs/` estão com o MAC zerado, e no documento de hardware os
  octetos que identificam a unidade estão mascarados — os três primeiros são
  apenas o OUI da Espressif, iguais em qualquer ESP32.
- O `token_bridge.py` roda só na sua máquina e fala só com a placa, na rede local.

## Avisos

**A Anthropic não permite token OAuth de assinatura em ferramentas de
terceiros**, e este firmware se apresenta como o Claude Code através dos
cabeçalhos (`anthropic-beta: oauth-2025-04-20` e o `User-Agent`). O aviso é do
projeto original e eu repito: a exposição é na sua conta. Funciona hoje; a
decisão é sua.

Este projeto não tem qualquer vínculo com a Anthropic.

## Créditos e licença

O firmware é do
[**benevid/claude-usage-stick-SVGL**](https://github.com/benevid/claude-usage-stick-SVGL),
de [@benevid](https://github.com/benevid), a partir do projeto original de
[@oauramos](https://github.com/oauramos). O README de lá credita ainda
[@jzimath-lab](https://github.com/jzimath-lab),
[@mpsd18](https://github.com/mpsd18),
[@ViniciusLoureiro67](https://github.com/ViniciusLoureiro67) e
[@renanravelli](https://github.com/renanravelli) pelas correções de build e pelo
suporte a múltiplas contas.

Deste lado é só o port: a placa, os drivers, o particionamento, o layout e a
investigação de memória.

**Sobre licença:** o repositório de origem não publica arquivo de licença nenhum.
Como isto é trabalho derivado, eu não tenho como licenciar o conjunto — seria
atribuir a mim termos sobre código que não é meu. Então este repositório também
não traz `LICENSE`, e quem quiser reutilizar o firmware deve falar com os autores
originais. O que é meu aqui — o port — eu libero sem restrição.
