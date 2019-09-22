#include "secret.h"
#include <apdu.h>
#include <ecc.h>
#include <fs.h>
#include <hmac.h>
#include <memzero.h>
#include <rand.h>

static int read_pri_key(uint8_t *pri_key) {
  int ret = read_attr(CTAP_CERT_FILE, KEY_ATTR, pri_key, ECC_KEY_SIZE);
  if (ret < 0) return ret;
  return 0;
}

static int read_kh_key(uint8_t *kh_key) {
  int ret = read_attr(CTAP_CERT_FILE, KH_KEY_ATTR, kh_key, KH_KEY_SIZE);
  if (ret < 0) return ret;
  return 0;
}

int get_sign_counter(uint32_t *counter) {
  int ret = read_attr(CTAP_CERT_FILE, SIGN_CTR_ATTR, counter, sizeof(uint32_t));
  if (ret < 0) return ret;
  return 0;
}

int increase_counter(uint32_t *counter) {
  int ret = read_attr(CTAP_CERT_FILE, SIGN_CTR_ATTR, counter, sizeof(uint32_t));
  if (ret < 0) return ret;
  ++*counter;
  ret = write_attr(CTAP_CERT_FILE, SIGN_CTR_ATTR, counter, sizeof(uint32_t));
  if (ret < 0) return ret;
  return 0;
}

int generate_key_handle(CredentialId *kh, uint8_t *pubkey) {
  int ret = read_kh_key(pubkey); // use pubkey as key buffer
  if (ret < 0) return ret;
  do {
    random_buffer(kh->nonce, sizeof(kh->nonce));
    // private key = hmac-sha256(device private key, nonce), stored in pubkey[0:32)
    hmac_sha256(pubkey, KH_KEY_SIZE, kh->nonce, sizeof(kh->nonce), pubkey);
    // tag = left(hmac-sha256(private key, rpIdHash or appid), 16), stored in pubkey[32, 64)
    hmac_sha256(pubkey, KH_KEY_SIZE, kh->rpIdHash, sizeof(kh->rpIdHash), pubkey + KH_KEY_SIZE);
    memcpy(kh->tag, pubkey + KH_KEY_SIZE, sizeof(kh->tag));
  } while (ecc_get_public_key(ECC_SECP256R1, pubkey, pubkey) < 0);
  return 0;
}

int verify_key_handle(CredentialId *kh) {
  uint8_t kh_key[KH_KEY_SIZE];
  int ret = read_kh_key(kh_key);
  if (ret < 0) return ret;
  // get private key
  hmac_sha256(kh_key, KH_KEY_SIZE, kh->nonce, sizeof(kh->nonce), kh_key);
  // get tag, store in rpIdHash, which should be verified first outside of this function
  hmac_sha256(kh_key, KH_KEY_SIZE, kh->rpIdHash, sizeof(kh->rpIdHash), kh->rpIdHash);
  if (memcmp(kh->rpIdHash, kh->tag, sizeof(kh->tag)) == 0) {
    // store prikey to rpIdHash
    memcpy(kh->rpIdHash, kh_key, sizeof(kh_key));
    memzero(kh_key, sizeof(kh_key));
    return 0;
  }
  memzero(kh_key, sizeof(kh_key));
  return 1;
}

size_t sign_with_device_key(const uint8_t *digest, uint8_t *sig) {
  uint8_t key[32];
  int ret = read_pri_key(key);
  if (ret < 0) return ret;
  ecdsa_sign(ECC_SECP256R1, key, digest, sig);
  return ecdsa_sig2ansi(sig, sig);
}

size_t sign_with_private_key(const uint8_t *key, const uint8_t *digest, uint8_t *sig) {
  ecdsa_sign(ECC_SECP256R1, key, digest, sig);
  return ecdsa_sig2ansi(sig, sig);
}

int get_cert(uint8_t *buf) { return read_file(CTAP_CERT_FILE, buf, 0, MAX_CERT_SIZE); }

int has_pin(void) {
  uint8_t tmp;
  return read_attr(CTAP_CERT_FILE, PIN_ATTR, &tmp, 1);
}

int set_pin(uint8_t *buf, uint8_t length) {
  int err;
  if (length == 0) {
    err = write_attr(CTAP_CERT_FILE, PIN_ATTR, NULL, 0);
  } else {
    sha256_raw(buf, length, buf);
    err = write_attr(CTAP_CERT_FILE, PIN_ATTR, buf, PIN_HASH_SIZE);
  }
  if (err < 0) return err;
  uint8_t ctr = 8;
  return write_attr(CTAP_CERT_FILE, PIN_CTR_ATTR, &ctr, 1);
}

int verify_pin_hash(uint8_t *buf) {
  uint8_t storedPinHash[PIN_HASH_SIZE];
  int err = read_attr(CTAP_CERT_FILE, PIN_ATTR, storedPinHash, PIN_HASH_SIZE);
  if (err < 0) return err;
  if (memcmp(storedPinHash, buf, PIN_HASH_SIZE) == 0) return 0;
  return 1;
}

int get_pin_retries(void) {
  uint8_t ctr;
  int err = read_attr(CTAP_CERT_FILE, PIN_CTR_ATTR, &ctr, 1);
  if (err < 0) return err;
  return ctr;
}

int set_pin_retries(uint8_t ctr) { return write_attr(CTAP_CERT_FILE, PIN_CTR_ATTR, &ctr, 1); }

int find_rk_by_credential_id(CTAP_residentKey *rk) {
  int size = get_file_size(RK_FILE);
  if (size < 0) return -2;
  uint8_t buf[sizeof(CredentialId)];
  memcpy(buf, &rk->credential_id, sizeof(buf));
  int nRk = size / sizeof(CTAP_residentKey);
  for (int i = 0; i != nRk; ++i) {
    size = read_file(RK_FILE, rk, i * sizeof(CTAP_residentKey), sizeof(CTAP_residentKey));
    if (size < 0) return -2;
    if (memcmp(buf, &rk->credential_id, sizeof(buf)) == 0) return i;
  }
  return -1;
}

int write_rk(CTAP_residentKey *rk, int idx) {
  if (idx == -1) {
    int size = get_file_size(RK_FILE);
    if (size < 0) return -1;
    int nRk = size / sizeof(CTAP_residentKey);
    return write_file(RK_FILE, rk, nRk * sizeof(CTAP_residentKey), sizeof(CTAP_residentKey), 0);
  } else {
    return write_file(RK_FILE, rk, idx * sizeof(CTAP_residentKey), sizeof(CTAP_residentKey), 0);
  }
}
