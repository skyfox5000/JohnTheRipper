/*
 * This software is Copyright (c) 2004 bartavelle, <bartavelle at bandecon.com>, and it is hereby released to the general public under the following terms:
 * Redistribution and use in source and binary forms, with or without modification, are permitted.
 *
 * UTF-8 support and use of intrinsics by magnum 2011, same terms as above
 *
 * microsoft MS SQL cracker
 *
 */

#if FMT_EXTERNS_H
extern struct fmt_main fmt_mssql;
#elif FMT_REGISTERS_H
john_register_one(&fmt_mssql);
#else

#include <string.h>

#include "arch.h"

#ifdef MMX_COEF
#define NBKEYS	(MMX_COEF * SHA1_SSE_PARA)
#endif
#include "sse-intrinsics.h"

#include "misc.h"
#include "params.h"
#include "common.h"
#include "formats.h"
#include "options.h"
#include "unicode.h"
#include "sha.h"
#include "memdbg.h"

#define FORMAT_LABEL			"mssql"
#define FORMAT_NAME			"MS SQL"

#define ALGORITHM_NAME			"SHA1 " SHA1_ALGORITHM_NAME

#define BENCHMARK_COMMENT		""
#define BENCHMARK_LENGTH		0

#define PLAINTEXT_LENGTH		25
#define CIPHERTEXT_LENGTH		94

#define BINARY_SIZE			20
#define BINARY_ALIGN			4
#define SALT_SIZE			4
#define SALT_ALIGN			4

#ifdef MMX_COEF
#define MIN_KEYS_PER_CRYPT		NBKEYS
#define MAX_KEYS_PER_CRYPT		NBKEYS
#define GETPOS(i, index)		( (index&(MMX_COEF-1))*4 + ((i)&(0xffffffff-3))*MMX_COEF + (3-((i)&3)) + (index>>(MMX_COEF>>1))*SHA_BUF_SIZ*MMX_COEF*4 ) //for endianity conversion
#if (MMX_COEF==2)
#define SALT_EXTRA_LEN          0x40004
#else
#define SALT_EXTRA_LEN          0x4040404
#endif
#else
#define MIN_KEYS_PER_CRYPT		1
#define MAX_KEYS_PER_CRYPT		1
#endif

//microsoft unicode ...
#if ARCH_LITTLE_ENDIAN
#define ENDIAN_SHIFT_L
#define ENDIAN_SHIFT_R
#else
#define ENDIAN_SHIFT_L  << 8
#define ENDIAN_SHIFT_R  >> 8
#endif

static struct fmt_tests tests[] = {
	{"0x0100A607BA7C54A24D17B565C59F1743776A10250F581D482DA8B6D6261460D3F53B279CC6913CE747006A2E3254", "FOO"},
	{"0x01000508513EADDF6DB7DDD270CCA288BF097F2FF69CC2DB74FBB9644D6901764F999BAB9ECB80DE578D92E3F80D", "BAR"},
	{"0x01008408C523CF06DCB237835D701C165E68F9460580132E28ED8BC558D22CEDF8801F4503468A80F9C52A12C0A3", "CANARD"},
	{"0x0100BF088517935FC9183FE39FDEC77539FD5CB52BA5F5761881E5B9638641A79DBF0F1501647EC941F3355440A2", "LAPIN"},
	{NULL}
};

static unsigned char cursalt[SALT_SIZE];

#ifdef MMX_COEF
/* Cygwin would not guarantee the alignment if these were declared static */
#define saved_key mssql_saved_key
#define crypt_key mssql_crypt_key
JTR_ALIGN(16) char saved_key[SHA_BUF_SIZ*4*NBKEYS];
JTR_ALIGN(16) char crypt_key[BINARY_SIZE*NBKEYS];
static char plain_keys[NBKEYS][PLAINTEXT_LENGTH*3+1];
#else

static unsigned char *saved_key;
static ARCH_WORD_32 crypt_key[BINARY_SIZE / 4];
static unsigned int key_length;
static char *plain_keys[1];

#endif

static int valid(char *ciphertext, struct fmt_main *self)
{
	int i;

	if (strlen(ciphertext) != CIPHERTEXT_LENGTH) return 0;
	if(memcmp(ciphertext, "0x0100", 6))
		return 0;
	for (i = 6; i < CIPHERTEXT_LENGTH; i++){
		if (!(  (('0' <= ciphertext[i])&&(ciphertext[i] <= '9')) ||
					(('a' <= ciphertext[i])&&(ciphertext[i] <= 'f'))
					|| (('A' <= ciphertext[i])&&(ciphertext[i] <= 'F'))))
			return 0;
	}
	return 1;
}

static void set_salt(void *salt)
{
	memcpy(cursalt, salt, SALT_SIZE);
}

static void * get_salt(char * ciphertext)
{
	static unsigned char *out2;
	int l;

	if (!out2) out2 = mem_alloc_tiny(SALT_SIZE, MEM_ALIGN_WORD);

	for(l=0;l<SALT_SIZE;l++)
	{
		out2[l] = atoi16[ARCH_INDEX(ciphertext[l*2+6])]*16
			+ atoi16[ARCH_INDEX(ciphertext[l*2+7])];
	}

	return out2;
}

static void set_key_enc(char *_key, int index);
extern struct fmt_main fmt_mssql;

static void init(struct fmt_main *self)
{
	initUnicode(UNICODE_MS_OLD);
#ifdef MMX_COEF
	memset(saved_key, 0, sizeof(saved_key));
#else
	saved_key = mem_calloc_tiny(PLAINTEXT_LENGTH*2 + 1 + SALT_SIZE, MEM_ALIGN_WORD);
#endif
	if (pers_opts.target_enc == UTF_8)
		fmt_mssql.params.plaintext_length = PLAINTEXT_LENGTH * 3;

	if (pers_opts.target_enc != ISO_8859_1 &&
	    pers_opts.target_enc != ASCII)
		fmt_mssql.methods.set_key = set_key_enc;
}

#ifdef MMX_COEF
static void clear_keys(void) {
#if SHA_BUF_SIZ == 16
	memset(saved_key, 0, sizeof(saved_key));
#else
	int j=0;
	for (; j<SHA1_SSE_PARA; j++)
		memset(saved_key+j*4*SHA_BUF_SIZ*MMX_COEF, 0, 56*MMX_COEF);
#endif
}
#endif

static void set_key(char *key, int index) {
	UTF8 utf8[PLAINTEXT_LENGTH+1];
	int utf8len, orig_len;

#ifdef MMX_COEF
	int i;
	strnzcpy(plain_keys[index], key, PLAINTEXT_LENGTH + 1);
#else
	plain_keys[index] = key;
#endif
	orig_len = strlen(key);
	utf8len = enc_uc(utf8, sizeof(utf8), (unsigned char*)key, orig_len);
	if (utf8len <= 0 && *key)
		return;

#ifdef MMX_COEF
	((unsigned int *)saved_key)[15*MMX_COEF + (index&3) + (index>>2)*SHA_BUF_SIZ*MMX_COEF] = (2*utf8len+SALT_SIZE)<<3;
	for(i=0;i<utf8len;i++)
		saved_key[GETPOS((i*2), index)] = utf8[i];
	saved_key[GETPOS((i*2+SALT_SIZE) , index)] = 0x80;
#else
	key_length = 0;

	while( (((unsigned short *)saved_key)[key_length] = (utf8[key_length] ENDIAN_SHIFT_L ))  )
		key_length++;
#endif
}

static void set_key_enc(char *key, int index) {
	UTF16 utf16key[PLAINTEXT_LENGTH+1], utf16key_tmp[PLAINTEXT_LENGTH+1];
	int utf8len = strlen(key);
	int i;
	int utf16len;

#ifdef MMX_COEF
	strnzcpy(plain_keys[index], key, PLAINTEXT_LENGTH*3 + 1);
#else
	plain_keys[index] = key;
#endif
	utf16len = enc_to_utf16(utf16key_tmp, PLAINTEXT_LENGTH, (unsigned char*)key, utf8len);
	if (utf16len <= 0) {
		utf8len = -utf16len;
		plain_keys[index][utf8len] = 0; // match truncation!
		if (utf16len != 0)
			utf16len = strlen16(utf16key_tmp);
	}
	utf16len = utf16_uc(utf16key, PLAINTEXT_LENGTH, utf16key_tmp, utf16len);
	if (utf16len <= 0)
		utf16len *= -1;

#ifdef MMX_COEF
	((unsigned int *)saved_key)[15*MMX_COEF + (index&3) + (index>>2)*SHA_BUF_SIZ*MMX_COEF] = (2*utf16len+SALT_SIZE)<<3;
	for(i=0;i<utf16len;i++)
	{
		saved_key[GETPOS((i*2), index)] = (char)utf16key[i];
		saved_key[GETPOS((i*2+1), index)] = (char)(utf16key[i]>>8);
	}
	saved_key[GETPOS((i*2+SALT_SIZE) , index)] = 0x80;
#else
	for(i=0;i<utf16len;i++)
	{
		unsigned char *uc = (unsigned char*)&(utf16key[i]);
		saved_key[(i<<1)  ] = uc[0];
		saved_key[(i<<1)+1] = uc[1];
	}
	key_length = i;
#endif
}

static char *get_key(int index) {
	static UTF8 UC_Key[PLAINTEXT_LENGTH*3+1];
	// Calling this will ONLY upcase characters 'valid' in the code page. There are MANY
	// code pages which mssql WILL upcase the letter (in UCS-2), but there is no upper case value
	// in the code page.  Thus we MUST keep the lower cased letter in this case.
	enc_uc(UC_Key, sizeof(UC_Key), (UTF8*)plain_keys[index], strlen(plain_keys[index]));
	return (char*)UC_Key;
}

static int cmp_all(void *binary, int count) {
#ifdef MMX_COEF
	unsigned int x,y=0;
	for(;y<SHA1_SSE_PARA;y++)
	for(x=0;x<MMX_COEF;x++)
	{
		if( ((unsigned int *)binary)[0] == ((unsigned int *)crypt_key)[x+y*MMX_COEF*5] )
			return 1;
	}
	return 0;
#else
	return !memcmp(binary, crypt_key, BINARY_SIZE);
#endif
}

static int cmp_exact(char *source, int count){
  return (1);
}

static int cmp_one(void * binary, int index)
{
#ifdef MMX_COEF
	unsigned int x,y;
	x = index&(MMX_COEF-1);
	y = index>>(MMX_COEF>>1);

	if( (((unsigned int *)binary)[0] != ((unsigned int *)crypt_key)[x+y*MMX_COEF*5])   |
	    (((unsigned int *)binary)[1] != ((unsigned int *)crypt_key)[x+y*MMX_COEF*5+MMX_COEF]) |
	    (((unsigned int *)binary)[2] != ((unsigned int *)crypt_key)[x+y*MMX_COEF*5+2*MMX_COEF]) |
	    (((unsigned int *)binary)[3] != ((unsigned int *)crypt_key)[x+y*MMX_COEF*5+3*MMX_COEF])|
	    (((unsigned int *)binary)[4] != ((unsigned int *)crypt_key)[x+y*MMX_COEF*5+4*MMX_COEF]) )
		return 0;
	return 1;
#else
	return cmp_all(binary, index);
#endif
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	int count = *pcount;
#ifdef MMX_COEF
	unsigned i, index;

	for (index = 0; index < count; ++index)
	{
		unsigned len = (((((unsigned int *)saved_key)[15*MMX_COEF + (index&3) + (index>>2)*SHA_BUF_SIZ*MMX_COEF]) >> 3) & 0xff) - SALT_SIZE;
		for(i=0;i<SALT_SIZE;i++)
			saved_key[GETPOS((len+i), index)] = cursalt[i];
	}
	SSESHA1body(saved_key, (unsigned int *)crypt_key, NULL, SSEi_MIXED_IN);
#else
	SHA_CTX ctx;
	memcpy(saved_key+key_length*2, cursalt, SALT_SIZE);
	SHA1_Init( &ctx );
	SHA1_Update( &ctx, saved_key, key_length*2+SALT_SIZE );
	SHA1_Final( (unsigned char *) crypt_key, &ctx);
#endif

	return count;
}

static void * binary(char *ciphertext)
{
	static char *realcipher;
	int i;

	if(!realcipher) realcipher = mem_alloc_tiny(BINARY_SIZE, MEM_ALIGN_WORD);

	for(i=0;i<BINARY_SIZE;i++)
	{
		realcipher[i] = atoi16[ARCH_INDEX(ciphertext[i*2+54])]*16 + atoi16[ARCH_INDEX(ciphertext[i*2+55])];
	}
#ifdef MMX_COEF
	alter_endianity((unsigned char *)realcipher, BINARY_SIZE);
#endif
	return (void *)realcipher;
}

#ifdef MMX_COEF
#define KEY_OFF ((index/MMX_COEF)*MMX_COEF*5+(index&(MMX_COEF-1)))
static int get_hash_0(int index) { return ((ARCH_WORD_32 *)crypt_key)[KEY_OFF] & 0xf; }
static int get_hash_1(int index) { return ((ARCH_WORD_32 *)crypt_key)[KEY_OFF] & 0xff; }
static int get_hash_2(int index) { return ((ARCH_WORD_32 *)crypt_key)[KEY_OFF] & 0xfff; }
static int get_hash_3(int index) { return ((ARCH_WORD_32 *)crypt_key)[KEY_OFF] & 0xffff; }
static int get_hash_4(int index) { return ((ARCH_WORD_32 *)crypt_key)[KEY_OFF] & 0xfffff; }
static int get_hash_5(int index) { return ((ARCH_WORD_32 *)crypt_key)[KEY_OFF] & 0xffffff; }
static int get_hash_6(int index) { return ((ARCH_WORD_32 *)crypt_key)[KEY_OFF] & 0x7ffffff; }
#else
static int get_hash_0(int index) { return ((ARCH_WORD_32 *)crypt_key)[index] & 0xf; }
static int get_hash_1(int index) { return ((ARCH_WORD_32 *)crypt_key)[index] & 0xff; }
static int get_hash_2(int index) { return ((ARCH_WORD_32 *)crypt_key)[index] & 0xfff; }
static int get_hash_3(int index) { return ((ARCH_WORD_32 *)crypt_key)[index] & 0xffff; }
static int get_hash_4(int index) { return ((ARCH_WORD_32 *)crypt_key)[index] & 0xfffff; }
static int get_hash_5(int index) { return ((ARCH_WORD_32 *)crypt_key)[index] & 0xffffff; }
static int get_hash_6(int index) { return ((ARCH_WORD_32 *)crypt_key)[index] & 0x7ffffff; }
#endif

static int salt_hash(void *salt)
{
	// This gave much better distribution on a huge set I analysed
	return (*((ARCH_WORD_32 *)salt) >> 8) & (SALT_HASH_SIZE - 1);
}

static void done(void)
{
	initUnicode(UNICODE_UNICODE);
}

struct fmt_main fmt_mssql = {
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
		FMT_8_BIT | FMT_UNICODE | FMT_UTF8,
#if FMT_MAIN_VERSION > 11
		{ NULL },
#endif
		tests
	}, {
		init,
		done,
		fmt_default_reset,
		fmt_default_prepare,
		valid,
		fmt_default_split,
		binary,
		get_salt,
#if FMT_MAIN_VERSION > 11
		{ NULL },
#endif
		fmt_default_source,
		{
			fmt_default_binary_hash_0,
			fmt_default_binary_hash_1,
			fmt_default_binary_hash_2,
			fmt_default_binary_hash_3,
			fmt_default_binary_hash_4,
			fmt_default_binary_hash_5,
			fmt_default_binary_hash_6
		},
		salt_hash,
		NULL,
		set_salt,
		set_key,
		get_key,
#ifdef MMX_COEF
		clear_keys,
#else
		fmt_default_clear_keys,
#endif
		crypt_all,
		{
			get_hash_0,
			get_hash_1,
			get_hash_2,
			get_hash_3,
			get_hash_4,
			get_hash_5,
			get_hash_6
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};

#endif /* plugin stanza */
