#include <openssl/evp.h>
#include <cstring>
#include <stdexcept>

/**
 * Minimal AES-256-CBC decryption with OpenSSL.
 *
 * @param in            Ciphertext input buffer
 * @param in_len        Length of ciphertext
 * @param out           Plaintext output buffer (must be allocated to at least in_len)
 * @param key           32-byte (256-bit) key
 * @param key_len       Must be 32 for AES-256
 * @param iv            16-byte IV (InnoDB uses a 32-byte space for the IV but effectively needs 16 bytes for CBC)
 */
bool my_custom_aes_decrypt(const unsigned char *in, int in_len,
                           unsigned char *out,
                           const unsigned char *key, int key_len,
                           const unsigned char *iv)
{
  if (key_len != 32) { // AES-256 requires 32 bytes
    // handle error
    return false;
  }


  // OpenSSL typically expects a 16-byte IV for AES CBC.
  // InnoDB stores a 32-byte "iv" area, but effectively it uses 16 bytes.
  // If your InnoDB has a special usage, adjust accordingly.
  //const int AES_BLOCK_SIZE = 16;

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;

  bool success = true;
  int out_len1 = 0, out_len2 = 0;

  do {
    // Initialize the decryption context
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
      success = false; break;
    }

    // Disable any padding (InnoDB generally does its own block-handling)
    if (EVP_CIPHER_CTX_set_padding(ctx, 0) != 1) {
      success = false; break;
    }

    // Decrypt the data
    if (EVP_DecryptUpdate(ctx, out, &out_len1, in, in_len) != 1) {
      success = false; break;
    }

    // Finalize
    if (EVP_DecryptFinal_ex(ctx, out + out_len1, &out_len2) != 1) {
      // Typically means padding error or truncated block, etc.
      success = false; 
      break;
    }
  } while (0);

  EVP_CIPHER_CTX_free(ctx);

  if (!success)
    return false;

  // total plaintext size = out_len1 + out_len2
  // but if there's no padding, often out_len2=0

  return true;
}

// Minimal example of an ECB decryption for the tablespace key
// A simplified version based on OpenSSL:
bool my_custom_aes_decrypt_ecb(const unsigned char *in, int in_len,
                               unsigned char *out,
                               const unsigned char *key, int key_len)
{
  if (key_len != 32) return false; // for AES-256
  const EVP_CIPHER *cipher = EVP_aes_256_ecb();

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;

  bool success = true;
  int out_len1 = 0, out_len2 = 0;
  do {
    if (EVP_DecryptInit_ex(ctx, cipher, NULL, key, NULL) != 1) {
      success = false; break;
    }
    // No IV for ECB, so pass null
    if (EVP_CIPHER_CTX_set_padding(ctx, 0) != 1) {
      success = false; break;
    }

    if (EVP_DecryptUpdate(ctx, out, &out_len1, in, in_len) != 1) {
      success = false; break;
    }
    if (EVP_DecryptFinal_ex(ctx, out + out_len1, &out_len2) != 1) {
      success = false; break;
    }
  } while (0);

  EVP_CIPHER_CTX_free(ctx);
  if (!success) return false;

  return true;
}

