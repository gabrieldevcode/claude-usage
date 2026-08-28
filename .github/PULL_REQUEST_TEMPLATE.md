# O que este PR faz?

<!-- Descricao curta da mudanca. -->

## Foto do dispositivo

<!-- OBRIGATORIA em mudancas de hardware: uma foto da SUA placa rodando este
     firmware (tela de boot, de PIN ou o dashboard). Arraste a imagem aqui. -->

## Checklist

- [ ] Compila: `arduino-cli compile` (ou `build.ps1` / `build.sh`) sem erro
- [ ] Testado na placa de verdade — modelo: <!-- ex.: CYD ESP32-2432S028 -->
- [ ] O log `[MEM] ... maior bloco` continua **acima de 78 KB** depois de montar
      a tela — abaixo disso a validacao da cadeia TLS da Anthropic falha, e o
      erro que aparece (`HTTP -1`) nao menciona memoria
      (ver [`docs/HARDWARE-CYD.md`](../docs/HARDWARE-CYD.md))
- [ ] Nenhum alvo de toque abaixo de `y = 216` (zona morta do painel)
- [ ] Nenhuma credencial, token ou senha no diff

## Se for um port para outra placa

- [ ] `docs/` atualizado com as specs **lidas da placa**, nao de catalogo
- [ ] Logs de deteccao em `docs/logs/`, com o MAC mascarado
- [ ] `partitions.csv` conferido contra o tamanho real da flash
