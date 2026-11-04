#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tcu::hal {

/// Pure Virtual HAL for Cryptographic Verification, SHA-256, HMAC, and AES-256-GCM.
class ICryptoHAL {
public:
    virtual ~ICryptoHAL() = default;

    /// Compute SHA-256 hex digest over a file using bounded 64KB streaming buffers.
    virtual std::string sha256FileHex(const std::string& file_path) const = 0;

    /// Compute SHA-256 hex digest over an in-memory buffer.
    virtual std::string sha256BufferHex(std::string_view data) const = 0;

    /// Verify Ed25519 or RSA-2048-SHA256 detached signature (PEM public key) over message.
    virtual bool verifySignature(std::string_view message,
                                 const std::vector<uint8_t>& signature,
                                 const std::string& public_key_pem_path) const = 0;

    /// Compute & verify HMAC-SHA256 for authenticated Cloud SOC commands (e.g. Immobilizer).
    virtual std::string computeHmacSha256Hex(std::string_view key,
                                             std::string_view message) const = 0;

    /// AES-256-GCM authenticated encryption/decryption for offline SVT location breadcrumbs.
    virtual std::vector<uint8_t> encryptAes256Gcm(std::string_view plaintext,
                                                  const std::vector<uint8_t>& key32) const = 0;
    virtual std::string decryptAes256Gcm(const std::vector<uint8_t>& ciphertext_blob,
                                         const std::vector<uint8_t>& key32) const = 0;
};

} // namespace tcu::hal

