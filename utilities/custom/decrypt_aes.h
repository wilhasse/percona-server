// decrypt.h (a new header for your utility)
#ifndef DECRYPT_H
#define DECRYPT_H

bool my_custom_aes_decrypt_ecb(const unsigned char *in, int in_len,
                               unsigned char *out,
                               const unsigned char *key, int key_len);

// (Include your custom AES code: my_custom_aes_decrypt)
bool my_custom_aes_decrypt(const unsigned char *in, int in_len,
                           unsigned char *out,
                           const unsigned char *key, int key_len,
                           const unsigned char *iv);

#endif
