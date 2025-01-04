// decrypt.h (a new header for your utility)
#ifndef DECRYPT_H
#define DECRYPT_H

bool my_custom_aes_decrypt_ecb(const unsigned char *in, int in_len,
                               unsigned char *out,
                               const unsigned char *key, int key_len);

#endif
