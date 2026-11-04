#include "hal/sa515m/openssl_crypto_hal.h"
#include "common/json_mini.h"

#include <cstdio>
#include <fstream>
#include <memory>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

namespace tcu::hal {

std::string OpenSslCryptoHAL::sha256FileHex(const std::string& file_path) const {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs.is_open()) {
        return "";
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    // Bounded 64 KB buffer per SDD Section 5
    std::vector<char> chunk(65536);
    while (ifs.good()) {
        ifs.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        std::streamsize got = ifs.gcount();
        if (got > 0) {
            EVP_DigestUpdate(ctx, chunk.data(), static_cast<size_t>(got));
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);
    return common::bytesToHex(digest, digest_len);
}

std::string OpenSslCryptoHAL::sha256BufferHex(std::string_view data) const {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    return common::bytesToHex(digest, digest_len);
}

bool OpenSslCryptoHAL::verifySignature(std::string_view message,
                                       const std::vector<uint8_t>& signature,
                                       const std::string& public_key_pem_path) const {
    if (signature.empty() || public_key_pem_path.empty()) {
        return false;
    }

    FILE* fp = std::fopen(public_key_pem_path.c_str(), "rb");
    if (!fp) {
        return false;
    }
    EVP_PKEY* pubkey = PEM_read_PUBKEY(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (!pubkey) {
        return false;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pubkey);
        return false;
    }

    bool verified = false;
    int key_id = EVP_PKEY_base_id(pubkey);
    if (key_id == EVP_PKEY_ED25519) {
        if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pubkey) == 1) {
            verified = (EVP_DigestVerify(
                ctx,
                signature.data(),
                signature.size(),
                reinterpret_cast<const unsigned char*>(message.data()),
                message.size()) == 1);
        }
    } else {
        // RSA / ECDSA with SHA-256
        if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pubkey) == 1 &&
            EVP_DigestVerifyUpdate(ctx, message.data(), message.size()) == 1) {
            verified = (EVP_DigestVerifyFinal(ctx, signature.data(), signature.size()) == 1);
        }
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pubkey);
    return verified;
}

std::string OpenSslCryptoHAL::computeHmacSha256Hex(std::string_view key,
                                                   std::string_view message) const {
    unsigned int out_len = 0;
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned char* res = HMAC(
        EVP_sha256(),
        key.data(),
        static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char*>(message.data()),
        message.size(),
        out,
        &out_len);
    if (!res) {
        return "";
    }
    return common::bytesToHex(out, out_len);
}

std::vector<uint8_t> OpenSslCryptoHAL::encryptAes256Gcm(
    std::string_view plaintext,
    const std::vector<uint8_t>& key32) const {
    if (key32.size() < 32) {
        return {};
    }

    constexpr size_t kIvLen  = 12;
    constexpr size_t kTagLen = 16;

    std::vector<uint8_t> out(kIvLen + plaintext.size() + kTagLen);
    if (RAND_bytes(out.data(), static_cast<int>(kIvLen)) != 1) {
        return {};
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    int len = 0;
    int ciphertext_len = 0;
    bool ok = (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1) &&
              (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr) == 1) &&
              (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key32.data(), out.data()) == 1) &&
              (EVP_EncryptUpdate(ctx,
                                 out.data() + kIvLen,
                                 &len,
                                 reinterpret_cast<const unsigned char*>(plaintext.data()),
                                 static_cast<int>(plaintext.size())) == 1);
    ciphertext_len = len;
    if (ok && EVP_EncryptFinal_ex(ctx, out.data() + kIvLen + len, &len) == 1) {
        ciphertext_len += len;
        ok = (EVP_CIPHER_CTX_ctrl(
                  ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, out.data() + kIvLen + ciphertext_len) == 1);
    } else {
        ok = false;
    }

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        return {};
    }
    out.resize(kIvLen + static_cast<size_t>(ciphertext_len) + kTagLen);
    return out;
}

std::string OpenSslCryptoHAL::decryptAes256Gcm(
    const std::vector<uint8_t>& ciphertext_blob,
    const std::vector<uint8_t>& key32) const {
    constexpr size_t kIvLen  = 12;
    constexpr size_t kTagLen = 16;

    if (key32.size() < 32 || ciphertext_blob.size() < (kIvLen + kTagLen)) {
        return "";
    }

    const size_t ct_len = ciphertext_blob.size() - kIvLen - kTagLen;
    std::vector<unsigned char> plain(ct_len + 1, 0);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";

    int len = 0;
    int plain_len = 0;
    std::vector<uint8_t> tag(
        ciphertext_blob.end() - static_cast<std::ptrdiff_t>(kTagLen),
        ciphertext_blob.end());

    bool ok = (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1) &&
              (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr) == 1) &&
              (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key32.data(), ciphertext_blob.data()) == 1) &&
              (EVP_DecryptUpdate(ctx,
                                 plain.data(),
                                 &len,
                                 ciphertext_blob.data() + kIvLen,
                                 static_cast<int>(ct_len)) == 1);
    plain_len = len;
    if (ok &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen, tag.data()) == 1 &&
        EVP_DecryptFinal_ex(ctx, plain.data() + len, &len) == 1) {
        plain_len += len;
    } else {
        ok = false;
    }

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(plain.data()), static_cast<size_t>(plain_len));
}

bool OpenSslCryptoHAL::generateEd25519Keypair(const std::string& priv_pem_path,
                                              const std::string& pub_pem_path) {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!pctx) return false;

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    EVP_PKEY_CTX_free(pctx);

    FILE* fpriv = std::fopen(priv_pem_path.c_str(), "wb");
    if (!fpriv) {
        EVP_PKEY_free(pkey);
        return false;
    }
    bool ok_priv = (PEM_write_PrivateKey(fpriv, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1);
    std::fclose(fpriv);

    FILE* fpub = std::fopen(pub_pem_path.c_str(), "wb");
    if (!fpub) {
        EVP_PKEY_free(pkey);
        return false;
    }
    bool ok_pub = (PEM_write_PUBKEY(fpub, pkey) == 1);
    std::fclose(fpub);

    EVP_PKEY_free(pkey);
    return ok_priv && ok_pub;
}

std::vector<uint8_t> OpenSslCryptoHAL::signEd25519(std::string_view message,
                                                   const std::string& priv_pem_path) {
    FILE* fp = std::fopen(priv_pem_path.c_str(), "rb");
    if (!fp) return {};
    EVP_PKEY* pkey = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (!pkey) return {};

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return {};
    }

    size_t sig_len = 0;
    std::vector<uint8_t> sig;
    if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
        EVP_DigestSign(ctx,
                       nullptr,
                       &sig_len,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) == 1) {
        sig.resize(sig_len);
        if (EVP_DigestSign(ctx,
                           sig.data(),
                           &sig_len,
                           reinterpret_cast<const unsigned char*>(message.data()),
                           message.size()) == 1) {
            sig.resize(sig_len);
        } else {
            sig.clear();
        }
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return sig;
}

} // namespace tcu::hal

