#include "encrypted_store.h"
// THICKET: these four were unprefixed upstream, for the same reason as the
// Identity.h include in the header.
#include <microReticulum/Cryptography/HKDF.h>
#include <microReticulum/Cryptography/HMAC.h>
#include <microReticulum/Cryptography/Random.h>
#include <microReticulum/Utilities/OS.h>
#include <microStore/File.h>

// ── constant-time comparison ──────────────────────────────────────────────────
static bool ct_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

// ── key derivation ────────────────────────────────────────────────────────────
// Derives 64 bytes from the identity's X25519 private key via HKDF-SHA256.
// The identity hash is used as salt, binding the derived keys to this specific
// identity (not just the raw key bytes).
//   out[0:32]  → AES-256-CTR encryption key
//   out[32:64] → HMAC-SHA256 authentication key
static void derive_keys(const RNS::Identity& identity, uint8_t out[64]) {
    RNS::Bytes k = RNS::Cryptography::hkdf(64,
        identity.encryptionPrivateKey(),
        identity.hash());
    memcpy(out, k.data(), 64);
}

// ── HMAC helper ───────────────────────────────────────────────────────────────
// Computes HMAC-SHA256 over: version(1) + IV(16) + ciphertext(N)
static RNS::Bytes compute_mac(const uint8_t* mac_key,
                               uint8_t version,
                               const uint8_t* iv,
                               const uint8_t* ct, size_t ctlen) {
    RNS::Bytes key_bytes(mac_key, 32);
    RNS::Cryptography::HMAC hmac(key_bytes);
    uint8_t ver = version;
    hmac.update(RNS::Bytes(&ver, 1));
    hmac.update(RNS::Bytes(iv, 16));
    hmac.update(RNS::Bytes(ct, ctlen));
    return hmac.digest();
}

// ── public API ────────────────────────────────────────────────────────────────
// File layout: version(1) + IV(16) + ciphertext(N) + HMAC-SHA256(32)

// THICKET: encstore_write/encstore_read were the only entry points upstream.
// The crypto is split out here so a caller holding its own storage can use the
// same format without the file handling; the two path-based functions are now
// thin wrappers. The format, the ordering and the constant-time compare are
// unchanged -- this moves code, it does not alter what lands on disk.

bool encstore_encrypt(const RNS::Identity& identity,
                      const uint8_t* data, size_t len, RNS::Bytes& out) {
    uint8_t keys[64];
    derive_keys(identity, keys);

    RNS::Bytes iv_bytes = RNS::Cryptography::random(16);
    const uint8_t* iv = iv_bytes.data();

    uint8_t* ct = (uint8_t*)malloc(len);
    if (!ct) { memset(keys, 0, 64); return false; }

    CTR<AES256> ctr;
    ctr.setKey(keys, 32);
    ctr.setIV(iv, 16);
    ctr.encrypt(ct, data, len);

    RNS::Bytes mac = compute_mac(keys + 32, ENCSTORE_FILE_VERSION, iv, ct, len);

    size_t file_len = ENCSTORE_FILE_OVERHEAD + len;
    uint8_t* fbuf = out.writable(file_len);
    fbuf[0] = (uint8_t)ENCSTORE_FILE_VERSION;
    memcpy(fbuf + 1,        iv,         16);
    memcpy(fbuf + 17,       ct,         len);
    memcpy(fbuf + 17 + len, mac.data(), 32);
    out.resize(file_len);

    free(ct);
    memset(keys, 0, 64);
    return true;
}

bool encstore_decrypt(const RNS::Identity& identity,
                      const uint8_t* blob, size_t len, RNS::Bytes& out) {
    if (len < ENCSTORE_FILE_OVERHEAD) return false;
    const size_t ctlen = len - ENCSTORE_FILE_OVERHEAD;

    uint8_t        version    = blob[0];
    const uint8_t* iv         = blob + 1;
    const uint8_t* ct         = blob + 17;
    const uint8_t* mac_stored = blob + 17 + ctlen;

    uint8_t keys[64];
    derive_keys(identity, keys);

    // Verify before decrypting -- wrong identity or tampered blob stops here.
    RNS::Bytes mac_expected = compute_mac(keys + 32, version, iv, ct, ctlen);
    if (!ct_equal(mac_expected.data(), mac_stored, 32)) {
        memset(keys, 0, 64);
        return false;
    }

    CTR<AES256> ctr;
    ctr.setKey(keys, 32);
    ctr.setIV(iv, 16);
    ctr.decrypt(out.writable(ctlen), ct, ctlen);
    out.resize(ctlen);

    memset(keys, 0, 64);
    return true;
}

bool encstore_write(const char* path, const RNS::Identity& identity,
                    const uint8_t* data, size_t len) {
    RNS::Bytes file_data;
    if (!encstore_encrypt(identity, data, len, file_data)) return false;
    return RNS::Utilities::OS::write_file(path, file_data) == file_data.size();
}

bool encstore_read(const char* path, const RNS::Identity& identity,
                   uint8_t* data, size_t len) {
    RNS::Bytes file_data;
    size_t read = RNS::Utilities::OS::read_file(path, file_data);
    if (read != len + ENCSTORE_FILE_OVERHEAD) return false;

    RNS::Bytes plain;
    if (!encstore_decrypt(identity, file_data.data(), read, plain)) return false;
    if (plain.size() != len) return false;
    memcpy(data, plain.data(), len);
    return true;
}

size_t encstore_size(const char* path) {
    microStore::File f = RNS::Utilities::OS::open_file(path, microStore::File::ModeRead);
    if (!f) return 0;
    size_t sz = f.size();
    return (sz > ENCSTORE_FILE_OVERHEAD) ? (sz - ENCSTORE_FILE_OVERHEAD) : 0;
}
