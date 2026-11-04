#pragma once

#include "hal/i_crypto_hal.h"
#include <string>
#include <vector>

namespace tcu::hal {

/// Production OpenSSL 3.x Cryptographic HAL (SHA-256, Ed25519/RSA-2048, HMAC, AES-256-GCM).
class OpenSslCryptoHAL : public ICryptoHAL {
public:
    OpenSslCryptoHAL() = default;
    ~OpenSslCryptoHAL() override = default;

    std::string sha256FileHex(const std::string& file_path) const override;
    std::string sha256BufferHex(std::string_view data) const override;

    bool verifySignature(std::string_view message,
                         const std::vector<uint8_t>& signature,
                         const std::string& public_key_pem_path) const override;

    std::string computeHmacSha256Hex(std::string_view key,
                                     std::string_view message) const override;

    std::vector<uint8_t> encryptAes256Gcm(std::string_view plaintext,
                                          const std::vector<uint8_t>& key32) const override;
    std::string decryptAes256Gcm(const std::vector<uint8_t>& ciphertext_blob,
                                 const std::vector<uint8_t>& key32) const override;

    /// Helper for OTA bundle signing tools and automated unit/integration tests:
    /// Generates an Ed25519 keypair, writes PEM files, and signs a message.
    static bool generateEd25519Keypair(const std::string& priv_pem_path,
                                       const std::string& pub_pem_path);
    static std::vector<uint8_t> signEd25519(std::string_view message,
                                            const std::string& priv_pem_path);
};

} // namespace tcu::hal

