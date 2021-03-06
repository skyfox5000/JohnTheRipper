/* OpenSSL "enc" cracker for JtR.
 *
 * This software is Copyright (c) 2013, Dhiru Kholia <dhiru at openwall.com>
 *
 * $ openssl enc -aes-256-cbc -p -e -a -salt -in hello.txt -out hello.txt.enc
 * enter aes-256-cbc encryption password:
 * Verifying - enter aes-256-cbc encryption password:
 * salt=305CEDC2A0521011
 * key=E08A1E6E1493BD3D3DAA25E112259D1688F7A0302AC8C16208DBDCEF179765F0
 * iv =582FDDF9603B9B03A54FC0BB34370DDE
 *
 * $ cat hello.txt
 * 123456789012
 *
 * Input Format:
 *
 * $openssl$cipher$md$salt-size$salt$last-chunks$inlined$known-plaintext$plaintext
 * $openssl$cipher$md$salt-size$salt$last-chunks$0$datalen$data$known-plaintext$plaintext
 */

#if FMT_EXTERNS_H
extern struct fmt_main fmt_openssl;
#elif FMT_REGISTERS_H
john_register_one(&fmt_openssl);
#else

#if AC_BUILT
#include "autoconfig.h"
#endif

#ifdef __CYGWIN__
// cygwin has HORRIBLE performance GOMP for this format it runs at 1/#cpu's the speed of OMP_NUM_THREADS=1 or non-GMP build
#undef _OPENMP
#undef FMT_OMP
#define FMT_OMP 0
#endif

#include <string.h>
#include <errno.h>
#if !AC_BUILT || HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <stdlib.h>
#include "stdint.h"
#include <sys/types.h>
#include <openssl/evp.h>
#include <openssl/aes.h>
#include "md5.h"
#include "arch.h"
#include "misc.h"
#include "params.h"
#include "common.h"
#include "formats.h"
#include "jumbo.h"
#ifdef _OPENMP
#include <omp.h>
#define OMP_SCALE               8
#endif
#include "memdbg.h"

#define FORMAT_LABEL        "openssl-enc"
#define FORMAT_NAME         "OpenSSL \"enc\" encryption"
#define ALGORITHM_NAME      "32/" ARCH_BITS_STR
#define BENCHMARK_COMMENT   ""
#define BENCHMARK_LENGTH    -1
#define BINARY_SIZE         0
#define SALT_SIZE           sizeof(struct custom_salt)
#define BINARY_ALIGN		1
#define SALT_ALIGN			sizeof(int)
#define MIN_KEYS_PER_CRYPT  8
#define MAX_KEYS_PER_CRYPT  8
#define PLAINTEXT_LENGTH    125
#define FORMAT_TAG          "$openssl$"
#define TAG_LENGTH          (sizeof(FORMAT_TAG) - 1)

static char (*saved_key)[PLAINTEXT_LENGTH + 1];
static int *cracked;

static struct custom_salt {
	unsigned int saltlen;
	unsigned char salt[16];
	int cipher;
	int md;
	int inlined;
	int kpa;
	int datalen;
	unsigned char kpt[256];
	unsigned char data[16 * 16];
	unsigned char last_chunks[32];
} *cur_salt;

static struct fmt_tests openssl_tests[] = {
	{"$openssl$1$0$8$a1a5e529c8d92da5$8de763bf61377d365243993137ad9729$1$0", "password"},
	{"$openssl$1$1$8$844527fb2f5d7ad5$ebccb1fcd2b1b30c5c3624d4016978ea$1$0", "password"},
	{"$openssl$0$0$8$305cedc2a0521011$bf11609a01e78ec3f50f0cc483e636f9$1$0", "password"},
	{"$openssl$0$0$8$305cedc2a0521011$bf11609a01e78ec3f50f0cc483e636f9$1$1$123456", "password"},
	{"$openssl$0$0$8$3993671be477e8f0$95384ad4fb11d737dc7ba884ccece94698b46d68d28c5cc4297ce37aea91064e$0$256$9bbbc2af64ba27444370e3b3db6f4077a5b83c099a9b0a13d0c03dbc89185aad078266470bb15c44e7b35aef66f456ba7f44fb0f60824331f5b598347cd471c6745374c7dbecf49a1dd0378e938bb9d3d68703e3038805fb3c7bf0623222bcc8e9375b10853aa7c991ddd086b8e2a97dd9ddd351ee0facde9bc3529742f0ffab990db046f5a64765d7a4b1c83b0290acae3eaa09278933cddcf1fed0ab14d408cd43fb73d830237dcd681425cd878bf4b542c108694b90e82f912c4aa4de02bd002dce975c2bb308aad933bfcfd8375d91837048d110f007ba3852dbb498a54595384ad4fb11d737dc7ba884ccece94698b46d68d28c5cc4297ce37aea91064e$0", "password"},
	{"$openssl$0$0$8$3993671be477e8f0$95384ad4fb11d737dc7ba884ccece94698b46d68d28c5cc4297ce37aea91064e$0$256$9bbbc2af64ba27444370e3b3db6f4077a5b83c099a9b0a13d0c03dbc89185aad078266470bb15c44e7b35aef66f456ba7f44fb0f60824331f5b598347cd471c6745374c7dbecf49a1dd0378e938bb9d3d68703e3038805fb3c7bf0623222bcc8e9375b10853aa7c991ddd086b8e2a97dd9ddd351ee0facde9bc3529742f0ffab990db046f5a64765d7a4b1c83b0290acae3eaa09278933cddcf1fed0ab14d408cd43fb73d830237dcd681425cd878bf4b542c108694b90e82f912c4aa4de02bd002dce975c2bb308aad933bfcfd8375d91837048d110f007ba3852dbb498a54595384ad4fb11d737dc7ba884ccece94698b46d68d28c5cc4297ce37aea91064e$1$00000000", "password"},
	{NULL}
};

static void init(struct fmt_main *self)
{
#if defined (_OPENMP)
	int omp_t = omp_get_max_threads();
	self->params.min_keys_per_crypt *= omp_t;
	omp_t *= OMP_SCALE;
	self->params.max_keys_per_crypt *= omp_t;
#endif
	saved_key = mem_calloc_tiny(sizeof(*saved_key) *
			self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
	cracked = mem_calloc_tiny(sizeof(*cracked) *
			self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
}

//#define DEBUG_VALID
#ifdef DEBUG_VALID
// Awesome debug macro for valid()
#define return if(printf("\noriginal: %s\n",ciphertext)+printf("fail line %u: '%s' p=%p q=%p q-p-1=%u\n",__LINE__,p,p,q,(unsigned int)(q-p-1)))return
#endif

#define HEX_DIGITS "0123456789abcdefABCDEF"
#define DEC_DIGITS "0123456789"
static int valid(char *ciphertext, struct fmt_main *self)
{
	char *p = ciphertext, *q = NULL;
	int len;

	if (strncmp(ciphertext, FORMAT_TAG,  TAG_LENGTH) != 0)
		return 0;
	p += TAG_LENGTH;		// cipher

	q = strchr(p, '$');
	if (!q)
		return 0;
	q = q + 1;
	if ((q - p - 1) != 1)
		return 0;
	if (*p != '0' && *p != '1')
		return 0;
	p = q; q = strchr(p, '$');	// md
	if (!q)
		return 0;
	q = q + 1;
	if ((q - p - 1) != 1)
		return 0;
	if (*p != '0' && *p != '1')
		return 0;
	p = q; q = strchr(p, '$');	// salt-size
	if (!q)
		return 0;
	q = q + 1;
	len = strspn(p, DEC_DIGITS);
	if (len < 1 || len > 2 || len != q - p - 1)
		return 0;
	len = atoi(p);
	if (len < 1 || len > sizeof(cur_salt->salt))
		return 0;
	p = q; q = strchr(p, '$');	// salt
	if (!q)
		return 0;
	q = q + 1;
	if (2 * len != q - p - 1 || 2 * len != strspn(p, HEX_DIGITS))
		return 0;
	p = q; q = strchr(p, '$');	// last-chunks
	if (!q)
		return 0;
	q = q + 1;
	len = strspn(p, HEX_DIGITS);
	if (len != q - p - 1 || len < 2 || len & 1 || len > sizeof(cur_salt->data))
		return 0;
	p = q; q = strchr(p, '$');	// inlined
	if (!q)
		return 0;
	q = q + 1;
	if ((q - p - 1) != 1)
		return 0;
	if (*p != '0' && *p != '1')
		return 0;
	if (*p == '0') {
		p = q; q = strchr(p, '$');	// datalen
		if (!q)
			return 0;
		q = q + 1;
		len = strspn(p, DEC_DIGITS);
		if (len < 1 || len > 3 || len != q - p - 1)
			return 0;
		len = atoi(p);
		if (len < 1 || len > sizeof(cur_salt->data))
			return 0;
		p = q; q = strchr(p, '$');	// data
		if (!q)
			return 0;
		q = q + 1;
		if (2 * len != q - p - 1 || 2 * len != strspn(p, HEX_DIGITS))
			return 0;
	}
	p = q; q = strchr(p, '$');	// known-plaintext
	if (!q)
		return !strcmp(p, "0");
	if(strlen(q) == 1)
		return 0;
	q = q + 1;
	if ((q - p - 1) != 1)
		return 0;
	if (*p != '0' && *p != '1')
		return 0;
	if (strlen(q) > sizeof(cur_salt->kpt) - 1)
		return 0;

#ifdef DEBUG_VALID
#undef return
#endif
	return 1;
}

static void *get_salt(char *ciphertext)
{
	char *ctcopy = strdup(ciphertext);
	char *keeptr = ctcopy;
	int i, res;
	char *p;
	static struct custom_salt cs;
	memset(&cs, 0, sizeof(cs));
	ctcopy += TAG_LENGTH;
	p = strtok(ctcopy, "$");
	cs.cipher = atoi(p);
	p = strtok(NULL, "$");
	cs.md = atoi(p);
	p = strtok(NULL, "$");
	cs.saltlen = atoi(p);
	p = strtok(NULL, "$");
	for (i = 0; i < cs.saltlen; i++)
		cs.salt[i] = atoi16[ARCH_INDEX(p[i * 2])] * 16 + atoi16[ARCH_INDEX(p[i * 2 + 1])];
	p = strtok(NULL, "$");
	res = strlen(p) / 2;
	for (i = 0; i < res; i++)
		cs.last_chunks[i] = atoi16[ARCH_INDEX(p[i * 2])] * 16
			+ atoi16[ARCH_INDEX(p[i * 2 + 1])];
	p = strtok(NULL, "$");
	cs.inlined = atoi(p);
	if (cs.inlined) {
		p = strtok(NULL, "$");
		cs.kpa = atoi(p);
		if (cs.kpa) {
			p = strtok(NULL, "$");
			strncpy((char*)cs.kpt, p, 255);
		}
	}
	else {
		p = strtok(NULL, "$");
		cs.datalen = atoi(p);
		p = strtok(NULL, "$");
		for (i = 0; i < cs.datalen; i++)
		cs.data[i] = atoi16[ARCH_INDEX(p[i * 2])] * 16
			+ atoi16[ARCH_INDEX(p[i * 2 + 1])];
		p = strtok(NULL, "$");
		cs.kpa = atoi(p);
		if (cs.kpa) {
			p = strtok(NULL, "$");
			strncpy((char*)cs.kpt, p, 255);
		}
	}

	MEM_FREE(keeptr);
	return (void *)&cs;
}

static int kpa(unsigned char *key, unsigned char *iv, int inlined)
{
	AES_KEY akey;
	unsigned char out[16*16];
	if(AES_set_decrypt_key(key, 256, &akey) < 0) {
		fprintf(stderr, "AES_set_decrypt_key failed in crypt!\n");
	}
	if (inlined) {
		AES_cbc_encrypt(cur_salt->last_chunks, out, 16, &akey, iv, AES_DECRYPT);
		if (memmem(out, 16, cur_salt->kpt, strlen((char*)cur_salt->kpt)))
			return 0;
	}
	else {
		AES_cbc_encrypt(cur_salt->data, out, cur_salt->datalen, &akey, iv, AES_DECRYPT);
		if (memmem(out, cur_salt->datalen, cur_salt->kpt, strlen((char*)cur_salt->kpt)))
			return 0;
	}
	return -1;
}

static int decrypt(char *password)
{
	unsigned char out[16];
	int pad, n, i;
	AES_KEY akey;
	unsigned char iv[16];
	unsigned char biv[16];
	unsigned char key[32];
	int nrounds = 1;
	// FIXME handle more stuff
	switch(cur_salt->cipher) {
		case 0:
			switch(cur_salt->md) {
				case 0:
					EVP_BytesToKey(EVP_aes_256_cbc(), EVP_md5(),
						cur_salt->salt, (unsigned char*)password,
						strlen(password), nrounds, key, iv);
					AES_set_decrypt_key(key, 256, &akey);
					break;
				case 1:
					EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha1(),
						cur_salt->salt, (unsigned char*)password,
						strlen(password), nrounds, key, iv);
					AES_set_decrypt_key(key, 256, &akey);
					break;
			}
			break;
		case 1:
			switch(cur_salt->md) {
				case 0:
					EVP_BytesToKey(EVP_aes_128_cbc(), EVP_md5(),
						cur_salt->salt, (unsigned char*)password,
						strlen(password), nrounds, key, iv);
					AES_set_decrypt_key(key, 128, &akey);
					break;
				case 1:
					EVP_BytesToKey(EVP_aes_128_cbc(), EVP_sha1(),
						cur_salt->salt, (unsigned char*)password,
						strlen(password), nrounds, key, iv);
					AES_set_decrypt_key(key, 128, &akey);
					break;
			}
			break;
	}
	memcpy(biv, iv, 16);

	if (cur_salt->inlined)
		AES_cbc_encrypt(cur_salt->last_chunks, out, 16, &akey, iv, AES_DECRYPT);
	else {
		memcpy(iv, cur_salt->last_chunks, 16);
		AES_cbc_encrypt(cur_salt->last_chunks + 16, out, 16, &akey, iv, AES_DECRYPT);
	}

	// FIXME use padding check for CBC mode only
	// now check padding
	pad = out[16 - 1];
	if(pad < 1 || pad > 16)
		return -1;
	n = 16 - pad;
	for(i = n; i < 16; i++)
		if(out[i] != pad)
			return -1;

	if(cur_salt->kpa)
		return kpa(key, biv, cur_salt->inlined);
	return 0;
}

static void set_salt(void *salt)
{
	cur_salt = (struct custom_salt *)salt;
}

static void openssl_set_key(char *key, int index)
{
	int saved_key_length = strlen(key);
	if (saved_key_length > PLAINTEXT_LENGTH)
		saved_key_length = PLAINTEXT_LENGTH;
	memcpy(saved_key[index], key, saved_key_length);
	saved_key[index][saved_key_length] = 0;
}

static char *get_key(int index)
{
	return saved_key[index];
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	int count = *pcount;
	int index;

#ifdef _OPENMP
#pragma omp parallel for
#endif
	for (index = 0; index < count; index++)
	{
		if (decrypt(saved_key[index]) == 0)
			cracked[index] = 1;
		else
			cracked[index] = 0;
	}
	return count;
}

static int cmp_all(void *binary, int count)
{
	int index;
	for (index = 0; index < count; index++)
		if (cracked[index])
			return 1;
	return 0;
}

static int cmp_one(void *binary, int index)
{
	return cracked[index];
}

static int cmp_exact(char *source, int index)
{
	return 1;
}

struct fmt_main fmt_openssl = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		0,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		BINARY_ALIGN,
		SALT_SIZE,
		SALT_ALIGN,
		MIN_KEYS_PER_CRYPT,
		MAX_KEYS_PER_CRYPT,
		FMT_CASE | FMT_8_BIT | FMT_OMP | FMT_NOT_EXACT,
/*
 * FIXME: if there wouldn't be so many false positives,
 *        it would be useful to report some tunable costs
 */
#if FMT_MAIN_VERSION > 11
		{ NULL },
#endif
		openssl_tests
	}, {
		init,
		fmt_default_done,
		fmt_default_reset,
		fmt_default_prepare,
		valid,
		fmt_default_split,
		fmt_default_binary,
		get_salt,
#if FMT_MAIN_VERSION > 11
		{ NULL },
#endif
		fmt_default_source,
		{
			fmt_default_binary_hash
		},
		fmt_default_salt_hash,
		NULL,
		set_salt,
		openssl_set_key,
		get_key,
		fmt_default_clear_keys,
		crypt_all,
		{
			fmt_default_get_hash
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};

#endif /* plugin stanza */
