// Self-contained AES-128 (ECB/CTR/XTS as Switch content uses them), SHA-256 and CRC-32 for the on-device importer.
// Portable C++ (no OpenSSL/mbedTLS): the same code runs in the PC test harness and in the APK.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nxi {

class Aes128 {
public:
    void set_key(const uint8_t key[16]);
    void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;
    void decrypt_block(const uint8_t in[16], uint8_t out[16]) const;
    ~Aes128();  // wipes the round keys

private:
    uint32_t rk_[44] = {};
};

// ECB decrypt of n bytes (n a multiple of 16), in place.
void aes_ecb_decrypt(const Aes128& k, uint8_t* data, size_t n);
// CTR over n bytes in place; iv is the 16-byte counter block of data[0] (big-endian, incremented per block).
void aes_ctr(const Aes128& k, const uint8_t iv[16], uint8_t* data, size_t n);
// XTS with Nintendo's tweak (sector number big-endian), decrypting n bytes (a whole number of sectors) in place.
void aes_xts_decrypt(const Aes128& data_key, const Aes128& tweak_key, uint64_t first_sector, size_t sector_size,
                     uint8_t* data, size_t n);

struct Sha256 {
    Sha256();
    void update(const void* data, size_t n);
    void final(uint8_t out[32]);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint64_t len_ = 0;
    uint8_t buf_[64];
    size_t used_ = 0;
};
void sha256(const void* data, size_t n, uint8_t out[32]);

uint32_t crc32_update(uint32_t crc, const void* data, size_t n);  // start with 0

// Zeroes key material in a way the optimiser keeps.
void secure_zero(void* p, size_t n);

bool crypto_self_test();  // FIPS-197 / SHA-256 / CRC known answers

}  // namespace nxi
