#ifndef TOUCH_H
#define TOUCH_H

#include <Arduino.h>
#include "config.h"

// Driver do XPT2046 (touch resistivo da CYD ESP32-2432S028).
//
// Substitui o AXS15231B_Touch capacitivo do projeto original. Mantem a mesma
// interface — begin() / touched() / readData() — para o resto do firmware nao
// precisar saber a diferenca.
//
// Duas coisas mudam de verdade em relacao ao capacitivo:
//
//   1. Resistivo nao devolve coordenada, devolve tensao. O mapeamento para
//      pixels vem das constantes TOUCH_RAW_* do config.h, medidas nesta placa
//      com quatro alvos (firmware/bringup_cyd).
//   2. A leitura e ruidosa. Cada eixo sai da MEDIANA de 7 amostras; um valor
//      unico mente com frequencia.
//
// O barramento e bit-bang em vez de SPI de hardware porque o XPT2046 fica num
// conjunto de pinos proprio, separado do painel, e a leitura acontece poucas
// vezes por segundo — nao ha nada a ganhar prendendo um periferico SPI para
// isso, e o bit-bang e o mesmo codigo ja validado no bring-up.
class XPT2046_Touch {
public:
    XPT2046_Touch(uint8_t sck, uint8_t mosi, uint8_t miso, uint8_t cs, uint8_t irq)
        : _sck(sck), _mosi(mosi), _miso(miso), _cs(cs), _irq(irq) {}

    bool begin() {
        pinMode(_sck, OUTPUT);  digitalWrite(_sck, LOW);
        pinMode(_mosi, OUTPUT); digitalWrite(_mosi, LOW);
        pinMode(_miso, INPUT);
        pinMode(_cs, OUTPUT);   digitalWrite(_cs, HIGH);
        pinMode(_irq, INPUT);
        return true;
    }

    // Le a pressao e, se houver toque, ja converte para pixels.
    bool touched() {
        if (_median(CMD_Z1) <= TOUCH_Z_MIN) return false;
        uint16_t rawX = _median(CMD_X);
        uint16_t rawY = _median(CMD_Y);
#if TOUCH_SWAP_XY
        long cx = rawY, cy = rawX;
#else
        long cx = rawX, cy = rawY;
#endif
        _point_x = (uint16_t)_map(cx, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X, SCREEN_WIDTH);
        _point_y = (uint16_t)_map(cy, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y, SCREEN_HEIGHT);
        return true;
    }

    void readData(uint16_t *x, uint16_t *y) {
        *x = _point_x;
        *y = _point_y;
    }

private:
    // Comandos do XPT2046: bit de start, canal, 12 bits, referencia diferencial.
    static const uint8_t CMD_X  = 0xD1;
    static const uint8_t CMD_Y  = 0x91;
    static const uint8_t CMD_Z1 = 0xB1;

    uint8_t _sck, _mosi, _miso, _cs, _irq;
    uint16_t _point_x = 0, _point_y = 0;

    void _write8(uint8_t v) {
        for (int i = 7; i >= 0; i--) {
            digitalWrite(_mosi, (v >> i) & 1);
            delayMicroseconds(2);
            digitalWrite(_sck, HIGH);
            delayMicroseconds(2);
            digitalWrite(_sck, LOW);
        }
    }
    uint16_t _read12(uint8_t cmd) {
        digitalWrite(_cs, LOW);
        _write8(cmd);
        // Um clock dummy: o conversor precisa dele antes do primeiro bit.
        digitalWrite(_sck, HIGH); delayMicroseconds(2);
        digitalWrite(_sck, LOW);  delayMicroseconds(2);
        uint16_t v = 0;
        for (int i = 0; i < 12; i++) {
            digitalWrite(_sck, HIGH);
            delayMicroseconds(2);
            v = (uint16_t)((v << 1) | (digitalRead(_miso) ? 1 : 0));
            digitalWrite(_sck, LOW);
            delayMicroseconds(2);
        }
        digitalWrite(_cs, HIGH);
        return v;
    }
    // Mediana de 7 por insercao: barato e imune ao pico isolado, que e
    // exatamente o defeito da leitura resistiva.
    uint16_t _median(uint8_t cmd) {
        uint16_t s[7];
        for (int i = 0; i < 7; i++) s[i] = _read12(cmd);
        for (int i = 1; i < 7; i++) {
            uint16_t k = s[i]; int j = i - 1;
            while (j >= 0 && s[j] > k) { s[j + 1] = s[j]; j--; }
            s[j + 1] = k;
        }
        return s[3];
    }
    // Mapeia cru -> pixel. lo pode ser MAIOR que hi (eixo invertido); a conta
    // continua valendo porque o divisor troca de sinal junto.
    static long _map(long v, long lo, long hi, long size) {
        long r = (v - lo) * (size - 1) / (hi - lo);
        if (r < 0) r = 0;
        if (r > size - 1) r = size - 1;
        return r;
    }
};

#endif // TOUCH_H
