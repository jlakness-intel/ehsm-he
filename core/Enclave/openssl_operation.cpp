/*
 * Copyright (C) 2020-2022 Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *   3. Neither the name of Intel Corporation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "enclave_hsm_t.h"
#include "log_utils.h"
#include "sgx_tseal.h"

#include <string>
#include <stdio.h>
#include <stdbool.h>
#include <mbusafecrt.h>

#include "sgx_report.h"
#include "sgx_utils.h"
#include "sgx_tkey_exchange.h"

#include "datatypes.h"
#include "openssl/rsa.h"
#include "openssl/evp.h"
#include "openssl/ec.h"
#include "openssl/pem.h"
#include "openssl/bio.h"
#include "openssl/err.h"

#include "datatypes.h"
#include "key_operation.h"
#include "openssl_operation.h"

#define MAX_DIGEST_LENGTH 64
#define SM4_NO_PAD 0
#define SM4_PAD 1

// https://github.com/openssl/openssl/blob/master/test/aesgcmtest.c#L38
sgx_status_t aes_gcm_encrypt(uint8_t *key,
                             uint8_t *cipherblob,
                             const EVP_CIPHER *block_mode,
                             uint8_t *plaintext,
                             uint32_t plaintext_len,
                             uint8_t *aad,
                             uint32_t aad_len,
                             uint8_t *iv,
                             uint32_t iv_len,
                             uint8_t *tag,
                             uint32_t tag_len)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;
    int temp_len = 0;
    EVP_CIPHER_CTX *pctx = NULL;

    // Create and init ctx
    if (!(pctx = EVP_CIPHER_CTX_new()))
        goto out;

    if (1 != EVP_EncryptInit_ex(pctx, block_mode, NULL, NULL, NULL))
        goto out;

    if (iv_len != SGX_AESGCM_IV_SIZE)
        if (1 != EVP_CIPHER_CTX_ctrl(pctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, NULL))
            goto out;

    // Initialise encrypt/decrpty, key and IV
    if (1 != EVP_EncryptInit_ex(pctx, NULL, NULL, key, iv))
        goto out;

    // Provide AAD data if exist
    if (aad != NULL && aad_len > 0)
        if (1 != EVP_EncryptUpdate(pctx, NULL, &temp_len, aad, aad_len))
            goto out;

    if (plaintext != NULL && plaintext_len > 0)
    {
        // Provide the message to be encrypted, and obtain the encrypted output.
        if (1 != EVP_EncryptUpdate(pctx, cipherblob, &temp_len, plaintext, plaintext_len))
            goto out;
    }
    else
    {
        ret = SGX_ERROR_INVALID_PARAMETER;
        goto out;
    }

    // Finalise the encryption/decryption
    if (1 != EVP_EncryptFinal_ex(pctx, cipherblob + temp_len, &temp_len))
        goto out;

    // Get tag
    if (1 != EVP_CIPHER_CTX_ctrl(pctx, EVP_CTRL_GCM_GET_TAG, tag_len, tag))
        goto out;

    ret = SGX_SUCCESS;

out:
    EVP_CIPHER_CTX_free(pctx);
    return ret;
}

// https://github.com/openssl/openssl/blob/master/test/aesgcmtest.c#L66
sgx_status_t aes_gcm_decrypt(uint8_t *key,
                             uint8_t *plaintext,
                             const EVP_CIPHER *block_mode,
                             uint8_t *ciphertext,
                             uint32_t ciphertext_len,
                             uint8_t *aad,
                             uint32_t aad_len,
                             uint8_t *iv,
                             uint32_t iv_len,
                             uint8_t *tag,
                             uint32_t tag_len)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    int temp_len = 0;
    EVP_CIPHER_CTX *pctx = NULL;
    // Create and initialise the context
    if (!(pctx = EVP_CIPHER_CTX_new()))
        goto out;

    if (1 != EVP_EncryptInit_ex(pctx, block_mode, NULL, NULL, NULL))
        goto out;

    if (iv_len != SGX_AESGCM_IV_SIZE)
        if (1 != EVP_CIPHER_CTX_ctrl(pctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, NULL))
            goto out;

    // Initialise decrypt, key and IV
    if (!EVP_DecryptInit_ex(pctx, NULL, NULL, key, iv))
        goto out;

    if (aad != NULL && aad_len > 0)
        if (!EVP_DecryptUpdate(pctx, NULL, &temp_len, aad, aad_len))
            goto out;

    // Decrypt message, obtain the plaintext output
    if (ciphertext != NULL && ciphertext_len > 0)
    {
        if (!EVP_DecryptUpdate(pctx, plaintext, &temp_len, ciphertext, ciphertext_len))
            goto out;
    }
    else
    {
        ret = SGX_ERROR_INVALID_PARAMETER;
        goto out;
    }

    // Update expected tag value
    if (!EVP_CIPHER_CTX_ctrl(pctx, EVP_CTRL_GCM_SET_TAG, tag_len, tag))
        goto out;

    // Finalise the decryption. A positive return value indicates success,
    // anything else is a failure - the plaintext is not trustworthy.
    if (EVP_DecryptFinal_ex(pctx, plaintext + temp_len, &temp_len) <= 0)
    {
        ret = SGX_ERROR_MAC_MISMATCH;
        goto out;
    }

    ret = SGX_SUCCESS;

out:
    EVP_CIPHER_CTX_free(pctx);
    return ret;
}

sgx_status_t sm4_ctr_encrypt(uint8_t *key,
                             uint8_t *cipherblob,
                             uint8_t *plaintext,
                             uint32_t plaintext_len,
                             uint8_t *iv)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;
    int temp_len = 0;
    EVP_CIPHER_CTX *pctx = NULL;

    // Create and initialize pState
    if (!(pctx = EVP_CIPHER_CTX_new()))
    {
        log_d("Error: failed to initialize EVP_CIPHER_CTX\n");
        goto out;
    }
    // Initialize encrypt, key and ctr
    if (EVP_EncryptInit_ex(pctx, EVP_sm4_ctr(), NULL, key, iv) != 1)
    {
        log_d("Error: failed to initialize encrypt, key and ctr\n");
        goto out;
    }

    // 3. Encrypt the plaintext and obtain the encrypted output
    if (EVP_EncryptUpdate(pctx, cipherblob, &temp_len, plaintext, plaintext_len) != 1)
    {
        log_d("Error: failed to encrypt the plaintext\n");
        goto out;
    }

    // 4. Finalize the encryption
    if (EVP_EncryptFinal_ex(pctx, cipherblob + temp_len, &temp_len) != 1)
    {
        log_d("Error: failed to finalize the encryption\n");
        goto out;
    }

    ret = SGX_SUCCESS;

out:
    EVP_CIPHER_CTX_free(pctx);
    return ret;
}

sgx_status_t sm4_ctr_decrypt(uint8_t *key,
                             uint8_t *plaintext,
                             uint8_t *cipherblob,
                             uint32_t ciphertext_len,
                             uint8_t *iv)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;
    int temp_len = 0;
    EVP_CIPHER_CTX *pctx = NULL;

    // Create and initialize ctx
    if (!(pctx = EVP_CIPHER_CTX_new()))
    {
        log_d("Error: failed to initialize EVP_CIPHER_CTX\n");
        goto out;
    }
    // Initialize decrypt, key and ctr
    if (!EVP_DecryptInit_ex(pctx, EVP_sm4_ctr(), NULL, (unsigned char *)key, iv))
    {
        log_d("Error: failed to initialize decrypt, key and ctr\n");
        goto out;
    }

    // Decrypt the ciphertext and obtain the decrypted output
    if (!EVP_DecryptUpdate(pctx, plaintext, &temp_len, cipherblob, ciphertext_len))
    {
        log_d("Error: failed to decrypt the ciphertext\n");
        goto out;
    }

    // Finalize the decryption:
    // - A positive return value indicates success;
    // - Anything else is a failure - the msg is not trustworthy.
    if (EVP_DecryptFinal_ex(pctx, plaintext + temp_len, &temp_len) <= 0)
    {
        log_d("Error: failed to finalize the decryption\n");
        goto out;
    }

    ret = SGX_SUCCESS;

out:
    EVP_CIPHER_CTX_free(pctx);
    return ret;
}

sgx_status_t sm4_cbc_encrypt(uint8_t *key,
                             uint8_t *cipherblob,
                             uint8_t *plaintext,
                             uint32_t plaintext_len,
                             uint8_t *iv)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    int temp_len = 0;
    EVP_CIPHER_CTX *pctx = NULL;
    // set padding mode
    int pad = (plaintext_len % 16 == 0) ? SM4_NO_PAD : SM4_PAD;

    // Create and initialize ctx
    if (!(pctx = EVP_CIPHER_CTX_new()))
    {
        log_d("Error: failed to initialize EVP_CIPHER_CTX\n");
        goto out;
    }
    // Initialize encrypt, key and ctr
    if (EVP_EncryptInit_ex(pctx, EVP_sm4_cbc(), NULL, key, iv) != 1)
    {
        log_d("Error: failed to initialize encrypt, key and ctr\n");
        goto out;
    }

    if (EVP_CIPHER_CTX_set_padding(pctx, pad) != 1)
    {
        log_d("Error: failed to set padding\n");
        goto out;
    }

    // Encrypt the plaintext and obtain the encrypted output
    if (EVP_EncryptUpdate(pctx, cipherblob, &temp_len, plaintext, plaintext_len) != 1)
    {
        log_d("Error: failed to encrypt the plaintext\n");
        goto out;
    }
    // Finalize the encryption
    if (EVP_EncryptFinal_ex(pctx, cipherblob + temp_len, &temp_len) != 1)
    {
        log_d("Error: failed to finalize the encryption\n");
        goto out;
    }

    ret = SGX_SUCCESS;

out:
    EVP_CIPHER_CTX_free(pctx);
    return ret;
}

sgx_status_t sm4_cbc_decrypt(uint8_t *key,
                             uint8_t *plaintext,
                             uint8_t *ciphertext,
                             uint32_t ciphertext_len,
                             uint8_t *iv)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    int temp_len = 0;
    EVP_CIPHER_CTX *pctx = NULL;

    int pad = (ciphertext_len % 16 == 0) ? 0 : 1;
    // Create and initialize ctx
    if (!(pctx = EVP_CIPHER_CTX_new()))
    {
        log_d("Error: failed to initialize EVP_CIPHER_CTX\n");
        goto out;
    }
    // Initialize decrypt, key and IV
    if (!EVP_DecryptInit_ex(pctx, EVP_sm4_cbc(), NULL, key, iv))
    {
        log_d("Error: failed to initialize decrypt, key and IV\n");
        goto out;
    }

    if (EVP_CIPHER_CTX_set_padding(pctx, pad) != 1)
    {
        log_d("Error: failed to set padding\n");
        goto out;
    }

    // Decrypt the ciphertext and obtain the decrypted output
    if (!EVP_DecryptUpdate(pctx, plaintext, &temp_len, ciphertext, ciphertext_len - 16))
    {
        log_d("Error: failed to decrypt the ciphertext\n");
        goto out;
    }

    // Finalize the decryption:
    // If length of decrypted data is integral multiple of 16, do not execute EVP_DecryptFinal_ex(), or it will failed to decrypt
    // - A positive return value indicates success;
    // - Anything else is a failure - the plaintext is not trustworthy.
    if (ciphertext_len % 16 != 0)
    {
        if (EVP_DecryptFinal_ex(pctx, plaintext + temp_len, &temp_len) <= 0)
        {
            log_d("Error: failed to finalize the decryption\n");
            goto out;
        }
    }

    ret = SGX_SUCCESS;

out:
    EVP_CIPHER_CTX_free(pctx);
    return ret;
}

sgx_status_t rsa_sign(RSA *rsa_prikey,
                      const EVP_MD *digestMode,
                      ehsm_padding_mode_t padding_mode,
                      const uint8_t *data,
                      uint32_t data_len,
                      uint8_t *signature,
                      uint32_t signature_len)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    EVP_PKEY *evpkey = NULL;
    EVP_MD_CTX *mdctx = NULL;
    EVP_PKEY_CTX *pkey_ctx = NULL;
    size_t temp_signature_size = 0;

    evpkey = EVP_PKEY_new();
    if (evpkey == NULL)
    {
        log_d("ecall rsa_sign generate evpkey failed.\n");
        ret = SGX_ERROR_OUT_OF_MEMORY;
        goto out;
    }

    // use EVP_PKEY store RSA private key
    if (EVP_PKEY_set1_RSA(evpkey, rsa_prikey) != 1)
    {
        log_d("ecall rsa_sign failed to set the evpkey by RSA_KEY\n");
        goto out;
    }

    // verify digestmode and padding mode
    if (padding_mode == RSA_PKCS1_PSS_PADDING)
    {
        if (EVP_MD_size(digestMode) * 2 + 2 > (size_t)EVP_PKEY_size(evpkey))
        {
            log_d("ecall rsa_sign unsupported padding mode.\n");
            ret = SGX_ERROR_INVALID_PARAMETER;
            goto out;
        }
    }

    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL)
    {
        log_d("ecall rsa_sign failed to create a EVP_MD_CTX.\n");
        ret = SGX_ERROR_OUT_OF_MEMORY;
        goto out;
    }

    if (EVP_MD_CTX_init(mdctx) != 1)
    {
        log_d("ecall rsa_sign EVP_MD_CTX initialize failed.\n");
        goto out;
    }

    // Signature initialization, set digest mode
    if (EVP_DigestSignInit(mdctx, &pkey_ctx, digestMode, nullptr, evpkey) != 1)
    {
        log_d("ecall rsa_sign EVP_DigestSignInit failed.\n");
        goto out;
    }

    // set padding mode
    if (EVP_PKEY_CTX_set_rsa_padding(pkey_ctx, padding_mode) != 1)
    {
        log_d("ecall rsa_sign EVP_PKEY_CTX_set_rsa_padding failed.\n");
        goto out;
    }

    if (padding_mode == RSA_PKCS1_PSS_PADDING)
    {
        if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey_ctx, EVP_MD_size(digestMode)) != 1)
        {
            log_d("ecall rsa_sign EVP_PKEY_CTX_set_rsa_pss_saltlen failed.\n");
            goto out;
        }
    }

    // update sign
    if (EVP_DigestUpdate(mdctx, data, data_len) != 1)
    {
        log_d("ecall rsa_sign EVP_DigestSignUpdate failed.\n");
        goto out;
    }

    // start sign
    if (EVP_DigestSignFinal(mdctx, NULL, &temp_signature_size) != 1)
    {
        log_d("ecall rsa_sign first EVP_DigestSignFinal failed.\n");
        goto out;
    }

    if (EVP_DigestSignFinal(mdctx, signature, &temp_signature_size) != 1)
    {
        log_d("ecall rsa_sign last EVP_DigestSignFinal failed.\n");
        goto out;
    }

    ret = SGX_SUCCESS;

out:
    EVP_PKEY_free(evpkey);
    EVP_MD_CTX_free(mdctx);
    return ret;
}

sgx_status_t ecc_sign(EC_KEY *ec_key,
                      const EVP_MD *digestMode,
                      const uint8_t *data,
                      uint32_t data_len,
                      uint8_t *signature,
                      uint32_t *signature_len)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    EVP_MD_CTX *mdctx = NULL;
    uint8_t *digestMessage = NULL;
    uint32_t digestMessage_len = MAX_DIGEST_LENGTH;

    digestMessage = (uint8_t *)malloc(digestMessage_len);

    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL)
    {
        log_d("ecall ec_sign failed to create a EVP_MD_CTX.\n");
        ret = SGX_ERROR_OUT_OF_MEMORY;
        goto out;
    }

    if (EVP_MD_CTX_init(mdctx) != 1)
    {
        log_d("ecall ec_sign EVP_MD_CTX initialize failed.\n");
        goto out;
    }

    // digest message
    if (EVP_DigestInit(mdctx, digestMode) != 1)
    {
        log_d("ecall ec_sign EVP_DigestInit failed.\n");
        goto out;
    }

    if (EVP_DigestUpdate(mdctx, data, data_len) != 1)
    {
        log_d("ecall ec_sign EVP_DigestUpdate failed.\n");
        goto out;
    }

    if (EVP_DigestFinal(mdctx, digestMessage, &digestMessage_len) != 1)
    {
        log_d("ecall ec_sign EVP_DigestFinal failed.\n");
        goto out;
    }

    if (ECDSA_sign(0, digestMessage, digestMessage_len, signature, signature_len, ec_key) != 1)
    {
        log_d("ecall ecdsa_sign failed.\n");
        goto out;
    }

    ret = SGX_SUCCESS;

out:
    EVP_MD_CTX_free(mdctx);

    SAFE_MEMSET(digestMessage, digestMessage_len, 0, digestMessage_len);
    SAFE_FREE(digestMessage);

    return ret;
}

sgx_status_t sm2_sign(EC_KEY *ec_key,
                      const EVP_MD *digestMode,
                      const uint8_t *data,
                      uint32_t data_len,
                      uint8_t *signature,
                      uint32_t *signature_len,
                      const uint8_t *id,
                      uint32_t id_len)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    EVP_PKEY *evpkey = NULL;
    EVP_MD_CTX *mdctx = NULL;
    EVP_PKEY_CTX *pkey_ctx = NULL;
    size_t temp_signature_size = 0;

    evpkey = EVP_PKEY_new();
    if (evpkey == NULL)
    {
        log_d("ecall sm2_sign generate evpkey failed.\n");
        ret = SGX_ERROR_OUT_OF_MEMORY;
        goto out;
    }

    if (EVP_PKEY_set1_EC_KEY(evpkey, ec_key) != 1)
    {
        log_d("ecall sm2_sign failed to set the evpkey by EC_KEY\n");
        goto out;
    }

    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL)
    {
        log_d("ecall sm2_sign failed to create a EVP_MD_CTX.\n");
        ret = SGX_ERROR_OUT_OF_MEMORY;
        goto out;
    }

    if (EVP_MD_CTX_init(mdctx) != 1)
    {
        log_d("ecall sm2_sign EVP_MD_CTX initialize failed.\n");
        goto out;
    }

    // set sm2 evp pkey
    if (EVP_PKEY_set_alias_type(evpkey, EVP_PKEY_SM2) != 1)
    {
        log_d("ecall sm2_sign failed to modify the evpkey to use SM2\n");
        goto out;
    }

    pkey_ctx = EVP_PKEY_CTX_new(evpkey, NULL);
    if (pkey_ctx == NULL)
    {
        log_d("ecall sm2_sign failed to create a EVP_PKEY_CTX\n");
        goto out;
    }

    if (EVP_PKEY_CTX_set1_id(pkey_ctx, id, id_len) != 1)
    {
        log_d("ecall sm2_sign failed to set sm2_user_id to the EVP_PKEY_CTX\n");
        goto out;
    }

    EVP_MD_CTX_set_pkey_ctx(mdctx, pkey_ctx);
    if (EVP_DigestSignInit(mdctx, &pkey_ctx, digestMode, nullptr, evpkey) != 1)
    {
        log_d("ecall sm2_sign EVP_DigestSignInit failed.\n");
        goto out;
    }

    if (EVP_DigestUpdate(mdctx, data, data_len) != 1)
    {
        log_d("ecall sm2_sign EVP_DigestSignUpdate failed.\n");
        goto out;
    }

    if (EVP_DigestSignFinal(mdctx, NULL, &temp_signature_size) != 1)
    {
        log_d("ecall sm2_sign EVP_DigestSignFinal1 failed.\n");
        goto out;
    }

    if (EVP_DigestSignFinal(mdctx, signature, &temp_signature_size) != 1)
    {
        log_d("ecall sm2_sign EVP_DigestSignFinal failed.\n");
        goto out;
    }

    // return the exact length
    *signature_len = (uint32_t)temp_signature_size;

    ret = SGX_SUCCESS;

out:
    EVP_PKEY_free(evpkey);
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_CTX_free(pkey_ctx);

    return ret;
}

sgx_status_t rsa_verify(RSA *rsa_pubkey,
                        const EVP_MD *digestMode,
                        ehsm_padding_mode_t padding_mode,
                        const uint8_t *data,
                        uint32_t data_len,
                        const uint8_t *signature,
                        uint32_t signature_len,
                        bool *result,
                        int saltlen)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    EVP_PKEY *evpkey = NULL;
    EVP_MD_CTX *mdctx = NULL;
    EVP_PKEY_CTX *pkey_ctx = NULL;

    evpkey = EVP_PKEY_new();
    if (evpkey == NULL)
    {
        log_d("ecall rsa_verify generate evpkey failed.\n");
        ret = SGX_ERROR_OUT_OF_MEMORY;
        goto out;
    }

    // use EVP_PKEY store RSA public key
    if (EVP_PKEY_set1_RSA(evpkey, rsa_pubkey) != 1)
    {
        log_d("ecall rsa_verify failed to set the evpkey by RSA_KEY\n");
        goto out;
    }

    // verify digestmode and padding mode
    if (padding_mode == RSA_PKCS1_PSS_PADDING)
    {
        // https://android.googlesource.com/platform/system/keymaster/+/refs/heads/master/km_openssl/rsa_operation.cpp#264
        if (EVP_MD_size(digestMode) * 2 + 2 > (size_t)EVP_PKEY_size(evpkey))
        {
            log_d("ecall rsa_sign unsupported padding mode.\n");
            ret = SGX_ERROR_INVALID_PARAMETER;
            goto out;
        }
    }

    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL)
    {
        log_d("ecall rsa_verify failed to create a EVP_MD_CTX.\n");
        ret = SGX_ERROR_OUT_OF_MEMORY;
        goto out;
    }

    if (EVP_MD_CTX_init(mdctx) != 1)
    {
        log_d("ecall rsa_verify EVP_MD_CTX initialize failed.\n");
        goto out;
    }

    // verify initialization, set digest mode
    if (EVP_DigestVerifyInit(mdctx, &pkey_ctx, digestMode, nullptr, evpkey) != 1)
    {
        log_d("ecall rsa_verify EVP_DigestVerifyInit failed.\n");
        goto out;
    }

    // set padding mode
    if (EVP_PKEY_CTX_set_rsa_padding(pkey_ctx, padding_mode) != 1)
    {
        log_d("ecall rsa_verify EVP_PKEY_CTX_set_rsa_padding failed.\n");
        goto out;
    }

    if (padding_mode == RSA_PKCS1_PSS_PADDING)
    {
        // If saltlen is -1, sets the salt length to the digest length
        if (saltlen == -1)
        {
            if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey_ctx, EVP_MD_size(digestMode)) != 1)
            {
                log_d("ecall rsa_verify EVP_PKEY_CTX_set_rsa_pss_saltlen failed.\n");
                goto out;
            }
        }
        else
        {
            if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey_ctx, saltlen) != 1)
            {
                log_d("ecall rsa_verify EVP_PKEY_CTX_set_rsa_pss_saltlen failed.\n");
                goto out;
            }
        }
    }

    // update verify
    if (EVP_DigestVerifyUpdate(mdctx, data, data_len) != 1)
    {
        log_d("ecall rsa_verify EVP_DigestVerifyUpdate failed.\n");
        goto out;
    }

    // start verify
    switch (EVP_DigestVerifyFinal(mdctx, signature, signature_len))
    {
    case 1:
        *result = true;
        break;
    case 0:
        // data digest did not match the original data or the signature had an invalid form
        *result = false;
        break;
    default:
        log_d("ecall rsa_verify EVP_DigestVerifyFinal failed.\n");
        goto out;
    }

    ret = SGX_SUCCESS;

out:
    EVP_PKEY_free(evpkey);
    EVP_MD_CTX_free(mdctx);

    return ret;
}

sgx_status_t ecc_verify(EC_KEY *ec_key,
                        const EVP_MD *digestMode,
                        const uint8_t *data,
                        uint32_t data_len,
                        const uint8_t *signature,
                        uint32_t signature_len,
                        bool *result)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    EVP_MD_CTX *mdctx = NULL;
    uint8_t *digestMessage = NULL;
    uint32_t digestMessage_len = MAX_DIGEST_LENGTH;

    digestMessage = (uint8_t *)malloc(digestMessage_len);

    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL)
    {
        log_d("ecall ec_verify failed to create a EVP_MD_CTX.\n");
        ret = SGX_ERROR_OUT_OF_MEMORY;
        goto out;
    }

    if (EVP_MD_CTX_init(mdctx) != 1)
    {
        log_d("ecall ec_verify EVP_MD_CTX initialize failed.\n");
        goto out;
    }

    // digest message
    if (EVP_DigestInit(mdctx, digestMode) != 1)
    {
        log_d("ecall ec_verify EVP_DigestInit failed.\n");
        goto out;
    }

    if (EVP_DigestUpdate(mdctx, data, data_len) != 1)
    {
        log_d("ecall ec_verify EVP_DigestUpdate failed.\n");
        goto out;
    }

    if (EVP_DigestFinal(mdctx, digestMessage, &digestMessage_len) != 1)
    {
        log_d("ecall ec_verify EVP_DigestFinal failed.\n");
        goto out;
    }

    switch (ECDSA_verify(0, digestMessage, digestMessage_len, signature, signature_len, ec_key))
    {
    case 1:
        *result = true;
        break;
    case 0:
        // data digest did not match the original data or the signature had an invalid form
        *result = false;
        break;
    default:
        log_d("ecall ECDSA_verify failed.\n");
        goto out;
    }

    ret = SGX_SUCCESS;

out:
    EVP_MD_CTX_free(mdctx);

    SAFE_MEMSET(digestMessage, MAX_DIGEST_LENGTH, 0, MAX_DIGEST_LENGTH);
    SAFE_FREE(digestMessage);

    return ret;
}

sgx_status_t sm2_verify(EC_KEY *ec_key,
                        const EVP_MD *digestMode,
                        const uint8_t *data,
                        uint32_t data_len,
                        const uint8_t *signature,
                        uint32_t signature_len,
                        bool *result,
                        const uint8_t *id,
                        uint32_t id_len)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    EVP_PKEY *evpkey = NULL;
    EVP_MD_CTX *mdctx = NULL;
    EVP_PKEY_CTX *pkey_ctx = NULL;

    evpkey = EVP_PKEY_new();
    if (evpkey == NULL)
    {
        log_d("ecall sm2_verify generate evpkey failed.\n");
        ret = SGX_ERROR_OUT_OF_MEMORY;
        goto out;
    }

    if (EVP_PKEY_set1_EC_KEY(evpkey, ec_key) != 1)
    {
        log_d("ecall sm2_verify failed to set the evpkey by RSA_KEY\n");
        goto out;
    }

    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL)
    {
        log_d("ecall sm2_verify failed to create a EVP_MD_CTX.\n");
        ret = SGX_ERROR_OUT_OF_MEMORY;
        goto out;
    }

    if (EVP_MD_CTX_init(mdctx) != 1)
    {
        log_d("ecall sm2_verify EVP_MD_CTX initialize failed.\n");
        goto out;
    }

    // set sm2 evp pkey
    if (EVP_PKEY_set_alias_type(evpkey, EVP_PKEY_SM2) != 1)
    {
        log_d("ecall sm2_verify failed to modify the evpkey to use SM2\n");
        goto out;
    }

    pkey_ctx = EVP_PKEY_CTX_new(evpkey, NULL);
    if (pkey_ctx == NULL)
    {
        log_d("ecall sm2_verify failed to create a EVP_PKEY_CTX\n");
        goto out;
    }

    // set sm2 id and len to pkeyctx
    if (EVP_PKEY_CTX_set1_id(pkey_ctx, id, id_len) != 1)
    {
        log_d("ecall sm2_verify failed to set sm2_user_id to the EVP_PKEY_CTX\n");
        goto out;
    }
    
    EVP_MD_CTX_set_pkey_ctx(mdctx, pkey_ctx);

    if (EVP_DigestVerifyInit(mdctx, &pkey_ctx, digestMode, nullptr, evpkey) != 1)
    {
        log_d("ecall sm2_verify EVP_DigestVerifyInit failed.\n");
        goto out;
    }

    if (EVP_DigestVerifyUpdate(mdctx, data, data_len) != 1)
    {
        log_d("ecall sm2_verify EVP_DigestVerifyUpdate failed.\n");
        goto out;
    }

    switch (EVP_DigestVerifyFinal(mdctx, signature, signature_len))
    {
    case 1:
        *result = true;
        break;
    case 0:
        // tbs did not match the original data or the signature had an invalid form
        *result = false;
        break;
    default:
        log_d("ecall sm2_verify EVP_DigestVerifyFinal failed.\n");
        goto out;
    }

    ret = SGX_SUCCESS;

out:
    EVP_PKEY_free(evpkey);
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_CTX_free(pkey_ctx);

    return ret;
}