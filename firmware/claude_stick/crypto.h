#pragma once
#include <stdint.h>
#include <stddef.h>

// Token cifrado com AES-256-GCM; chave derivada de uma senha (PBKDF2-ish via
// SHA-256). A senha e deviceSecret() no uso normal, e um PIN digitado apenas na
// migracao de placas que ainda tenham um token da versao com PIN.
// ciphertext[512] cabe folgado o token OAuth (sk-ant-oat01-..., ~110 chars).
struct EncryptedBlob {
    uint8_t  iv[12];
    uint8_t  tag[16];
    uint8_t  salt[6];
    uint16_t len;
    uint8_t  ciphertext[512];
};

// Segredo fixo desta placa: MAC de eFuse + DEVICE_KEY_SALT, em hex.
// NAO e secreto - o MAC e publico e o sal esta no binario. Serve para o token
// nao ficar em texto claro na NVS, nao para resistir a quem tem a placa.
const char* deviceSecret();

void deriveKey(const char* pin, const uint8_t* salt, size_t saltLen, uint8_t* keyOut32);
bool encryptToken(const char* plaintext, const char* pin, EncryptedBlob& blob);
bool decryptToken(const EncryptedBlob& blob, const char* pin, char* plainOut, size_t plainMaxLen);
