/*
 * This software is Copyright (c) 2014 magnum,
 * and it is hereby released to the general public under the following terms:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 */

#ifdef HAVE_OPENCL

#if FMT_EXTERNS_H
extern struct fmt_main fmt_ocl_cryptsha1;
#elif FMT_REGISTERS_H
john_register_one(&fmt_ocl_cryptsha1);
#else

#include <string.h>

#include "arch.h"
#include "base64.h"
#include "sha.h"
#include "params.h"
#include "common.h"
#include "formats.h"
#include "options.h"
#include "common-opencl.h"
#define OUTLEN 20
#include "opencl_pbkdf1_hmac_sha1.h"

#define SHA1_MAGIC "$sha1$"
#define SHA1_SIZE 20

#define FORMAT_LABEL                "sha1crypt-opencl"
#define FORMAT_NAME                 "(NetBSD)"
#define ALGORITHM_NAME              "PBKDF1-SHA1 OpenCL"
#define BENCHMARK_COMMENT           ""
#define BENCHMARK_LENGTH            -1001

#define BINARY_SIZE                 20
// max valid salt len in hash is shorter than this (by length of "$sha1$" and length of base10 string of rounds)
#define SALT_LENGTH                 64

#define CHECKSUM_LENGTH             28

#define BINARY_ALIGN                4
#define SALT_SIZE                   sizeof(pbkdf1_salt)
#define SALT_ALIGN                  4

#define MIN_KEYS_PER_CRYPT          1
#define MAX_KEYS_PER_CRYPT          1

/* An example hash (of password) is $sha1$40000$jtNX3nZ2$hBNaIXkt4wBI2o5rsi8KejSjNqIq.
 * An sha1-crypt hash string has the format $sha1$rounds$salt$checksum, where:
 *
 * $sha1$ is the prefix used to identify sha1-crypt hashes, following the Modular Crypt Format
 * rounds is the decimal number of rounds to use (40000 in the example).
 * salt is 0-64 characters drawn from [./0-9A-Za-z] (jtNX3nZ2 in the example).
 * checksum is 28 characters drawn from the same set, encoding a 168-bit checksum.
 */

static struct fmt_tests tests[] = {
	{"$sha1$64000$wnUR8T1U$vt1TFQ50tBMFgkflAFAOer2CwdYZ", "password"},
	{"$sha1$40000$jtNX3nZ2$hBNaIXkt4wBI2o5rsi8KejSjNqIq", "password"},
	{"$sha1$64000$wnUR8T1U$wmwnhQ4lpo/5isi5iewkrHN7DjrT", "123456"},
	{"$sha1$64000$wnUR8T1U$azjCegpOIk0FjE61qzGWhdkpuMRL", "complexlongpassword@123456"},
	{NULL}
};

/*
 * HASH_LOOPS is ideally made by factors of (iteration count - 1) and should
 * be chosen for a kernel duration of not more than 200 ms
 */
#define HASH_LOOPS		1024
#define LOOP_COUNT		(((host_salt->iterations - 1 + HASH_LOOPS - 1)) / HASH_LOOPS)

#define STEP			0
#define SEED			64

#define OCL_CONFIG		"sha1crypt"

#define MIN(a, b)		(((a) < (b)) ? (a) : (b))
#define MAX(a, b)		(((a) > (b)) ? (a) : (b))
#define ITERATIONS		(64000*2+2)

/* This handles all widths */
#define GETPOS(i, index)	(((index) % v_width) * 4 + ((i) & ~3U) * v_width + (((i) & 3) ^ 3) + ((index) / v_width) * 64 * v_width)

static unsigned int *inbuffer;
static pbkdf1_out *host_crack;
static pbkdf1_salt *host_salt;
static cl_int cl_error;
static cl_mem mem_in, mem_out, mem_salt, mem_state;
static size_t key_buf_size;
static unsigned int v_width = 1;	/* Vector width of kernel */
static cl_kernel pbkdf1_init, pbkdf1_loop, pbkdf1_final;
static int new_keys;

static const char * warn[] = {
        "in xfer: "  ,  ", init: "   , ", crypt: " , ", final: ", ", out xfer: "
};

static int split_events[] = { 2, -1, -1 };

static int crypt_all(int *pcount, struct db_salt *_salt);
static int crypt_all_benchmark(int *pcount, struct db_salt *_salt);

//This file contains auto-tuning routine(s). Has to be included after formats definitions.
#include "opencl_autotune.h"
#include "memdbg.h"

static void create_clobj(size_t gws, struct fmt_main *self)
{
	size_t kpc = gws * v_width;

#define CL_RO CL_MEM_READ_ONLY
#define CL_WO CL_MEM_WRITE_ONLY
#define CL_RW CL_MEM_READ_WRITE

#define CLCREATEBUFFER(_flags, _size)	  \
	clCreateBuffer(context[gpu_id], _flags, _size, NULL, &cl_error); \
	HANDLE_CLERROR(cl_error, "Error allocating GPU memory");

#define CLKERNELARG(kernel, id, arg)	  \
	HANDLE_CLERROR(clSetKernelArg(kernel, id, sizeof(arg), &arg), \
	               "Error setting kernel argument");

#ifdef DEBUG
	fprintf(stderr, "%s(%zu) kpc %zu\n", __FUNCTION__, gws, kpc);
#endif
	key_buf_size = PLAINTEXT_LENGTH * kpc;
	inbuffer = mem_calloc(key_buf_size);
	host_crack = mem_calloc(kpc * sizeof(pbkdf1_out));

	mem_in = CLCREATEBUFFER(CL_RO, key_buf_size);
	mem_salt = CLCREATEBUFFER(CL_RO, sizeof(pbkdf1_salt));
	mem_state = CLCREATEBUFFER(CL_RW, kpc * sizeof(pbkdf1_state));
	mem_out = CLCREATEBUFFER(CL_WO, kpc * sizeof(pbkdf1_out));

	CLKERNELARG(pbkdf1_init, 0, mem_in);
	CLKERNELARG(pbkdf1_init, 1, mem_salt);
	CLKERNELARG(pbkdf1_init, 2, mem_state);

	CLKERNELARG(pbkdf1_loop, 0, mem_state);

	CLKERNELARG(pbkdf1_final, 0, mem_salt);
	CLKERNELARG(pbkdf1_final, 1, mem_out);
	CLKERNELARG(pbkdf1_final, 2, mem_state);
	global_work_size = gws;
}

/* ------- Helper functions ------- */
static size_t get_task_max_work_group_size()
{
	size_t s;

	s = common_get_task_max_work_group_size(FALSE, 0, pbkdf1_init);
	s = MIN(s, common_get_task_max_work_group_size(FALSE, 0, pbkdf1_loop));
	s = MIN(s, common_get_task_max_work_group_size(FALSE, 0, pbkdf1_final));
	return s;
}

static size_t get_task_max_size()
{
	return 0;
}

static size_t get_default_workgroup()
{
	if (cpu(device_info[gpu_id]))
		return get_platform_vendor_id(platform_id) == DEV_INTEL ?
			8 : 1;
	else
		return 64;
}

static void release_clobj(void)
{
	HANDLE_CLERROR(clReleaseMemObject(mem_in), "Release mem in");
	HANDLE_CLERROR(clReleaseMemObject(mem_salt), "Release mem salt");
	HANDLE_CLERROR(clReleaseMemObject(mem_out), "Release mem out");

	MEM_FREE(inbuffer);
	MEM_FREE(host_crack);
}

static void init(struct fmt_main *self)
{
	char build_opts[64];
	size_t gws_limit;
	static char valgo[sizeof(ALGORITHM_NAME) + 8] = "";

	if ((v_width = opencl_get_vector_width(gpu_id,
	                                       sizeof(cl_int))) > 1) {
		/* Run vectorized kernel */
		snprintf(valgo, sizeof(valgo),
		         ALGORITHM_NAME " %ux", v_width);
		self->params.algorithm_name = valgo;
	}

	snprintf(build_opts, sizeof(build_opts),
	         "-DHASH_LOOPS=%u -DOUTLEN=%u "
	         "-DPLAINTEXT_LENGTH=%u -DV_WIDTH=%u",
	         HASH_LOOPS, OUTLEN, PLAINTEXT_LENGTH, v_width);
        opencl_init("$JOHN/kernels/pbkdf1_hmac_sha1_kernel.cl",
                    gpu_id, build_opts);

	pbkdf1_init = clCreateKernel(program[gpu_id], "pbkdf1_init", &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating kernel");
	crypt_kernel = pbkdf1_loop = clCreateKernel(program[gpu_id], "pbkdf1_loop", &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating kernel");
	pbkdf1_final = clCreateKernel(program[gpu_id], "pbkdf1_final", &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating kernel");

	gws_limit = get_max_mem_alloc_size(gpu_id) /
		(PLAINTEXT_LENGTH + sizeof(pbkdf1_out));

	//Initialize openCL tuning (library) for this format.
	opencl_init_auto_setup(SEED, HASH_LOOPS, split_events,
		warn, 2, self, create_clobj, release_clobj,
	        PLAINTEXT_LENGTH + sizeof(pbkdf1_out), gws_limit);

	//Auto tune execution from shared/included code.
	self->methods.crypt_all = crypt_all_benchmark;
	common_run_auto_tune(self, ITERATIONS, gws_limit, 10000000000ULL);
	self->methods.crypt_all = crypt_all;

	self->params.min_keys_per_crypt = local_work_size;
}

static void done(void)
{
	release_clobj();
	HANDLE_CLERROR(clReleaseKernel(pbkdf1_init), "Release kernel");
	HANDLE_CLERROR(clReleaseKernel(pbkdf1_loop), "Release kernel");
	HANDLE_CLERROR(clReleaseKernel(pbkdf1_final), "Release kernel");
	HANDLE_CLERROR(clReleaseProgram(program[gpu_id]),
	    "Release Program");
}

static void set_salt(void *salt)
{
	host_salt = (pbkdf1_salt*)salt;
	HANDLE_CLERROR(clEnqueueWriteBuffer(queue[gpu_id], mem_salt, CL_FALSE, 0, sizeof(pbkdf1_salt), host_salt, 0, NULL, NULL), "Copy salt to gpu");
}

static int binary_hash_0(void *binary)
{
#ifdef DEBUG
	dump_stuff_msg("binary_hash[0]", (uint32_t*)binary, 20);
#endif
	return (((uint32_t *) binary)[0] & 0xf);
}

static int get_hash_0(int index)
{
#ifdef DEBUG
	dump_stuff_msg("\nget_hash", host_crack[index].dk, 20);
#endif
	return host_crack[index].dk[0] & 0xf;
}
static int get_hash_1(int index) { return host_crack[index].dk[0] & 0xff; }
static int get_hash_2(int index) { return host_crack[index].dk[0] & 0xfff; }
static int get_hash_3(int index) { return host_crack[index].dk[0] & 0xffff; }
static int get_hash_4(int index) { return host_crack[index].dk[0] & 0xfffff; }
static int get_hash_5(int index) { return host_crack[index].dk[0] & 0xffffff; }
static int get_hash_6(int index) { return host_crack[index].dk[0] & 0x7ffffff; }

static int valid(char *ciphertext, struct fmt_main *self) {
	char *pos, *start, *endp;
	if (strncmp(ciphertext, SHA1_MAGIC, sizeof(SHA1_MAGIC) - 1))
		return 0;

	// validate checksum
        pos = start = strrchr(ciphertext, '$') + 1;
	if (strlen(pos) != CHECKSUM_LENGTH)
		return 0;
	while (atoi64[ARCH_INDEX(*pos)] != 0x7F) pos++;
	if (*pos || pos - start != CHECKSUM_LENGTH)
		return 0;

	// validate "rounds"
	start = ciphertext + sizeof(SHA1_MAGIC) - 1;
	if (!strtoul(start, &endp, 10))
		return 0;

	// validate salt
	start = pos = strchr(start, '$') + 1;
	while (atoi64[ARCH_INDEX(*pos)] != 0x7F && *pos != '$') pos++;
	if (pos - start != 8)
		return 0;

	return 1;
}

#define TO_BINARY(b1, b2, b3) \
	value = (ARCH_WORD_32)atoi64[ARCH_INDEX(pos[0])] | \
		((ARCH_WORD_32)atoi64[ARCH_INDEX(pos[1])] << 6) | \
		((ARCH_WORD_32)atoi64[ARCH_INDEX(pos[2])] << 12) | \
		((ARCH_WORD_32)atoi64[ARCH_INDEX(pos[3])] << 18); \
	pos += 4; \
	out[b1] = value >> 16; \
	out[b2] = value >> 8; \
	out[b3] = value;

static void *get_binary(char * ciphertext)
{       static union {
                unsigned char c[BINARY_SIZE + 16];
                ARCH_WORD dummy;
        } buf;
        unsigned char *out = buf.c;
	ARCH_WORD_32 value;

	char *pos = strrchr(ciphertext, '$') + 1;
	int i = 0;

	// XXX is this even correct?
	do {
		TO_BINARY(i, i + 1, i + 2);
		i = i + 3;
	} while (i <= 18);

	return (void *)out;
}

static int crypt_all_benchmark(int *pcount, struct db_salt *salt)
{
	int lazy = 0;
	size_t scalar_gws = global_work_size * v_width;

#ifdef DEBUG
	fprintf(stderr, "%s(%d) lws %zu gws %zu sgws %zu\n", __FUNCTION__,
	        *pcount, local_work_size, global_work_size, scalar_gws);
#endif
	/// Run kernels, no iterations for fast enumeration
	BENCH_CLERROR(clEnqueueWriteBuffer(queue[gpu_id], mem_in, CL_FALSE, 0, key_buf_size, inbuffer, 0, NULL, multi_profilingEvent[lazy++]), "Copy data to gpu");

	BENCH_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], pbkdf1_init, 1, NULL, &global_work_size, &local_work_size, 0, NULL, multi_profilingEvent[lazy++]), "Run initial kernel");

	BENCH_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], pbkdf1_loop, 1, NULL, &global_work_size, &local_work_size, 0, NULL, multi_profilingEvent[lazy++]), "Run loop kernel");
	BENCH_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], pbkdf1_final, 1, NULL, &global_work_size, &local_work_size, 0, NULL, multi_profilingEvent[lazy++]), "Run final kernel");

	BENCH_CLERROR(clEnqueueReadBuffer(queue[gpu_id], mem_out, CL_FALSE, 0, sizeof(pbkdf1_out) * scalar_gws, host_crack, 0, NULL, multi_profilingEvent[lazy++]), "Copy result back");

	BENCH_CLERROR(clFinish(queue[gpu_id]), "Failed running kernels");

	return *pcount;
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	int count = *pcount;
	int i;
	size_t scalar_gws;

	global_work_size = ((count + (v_width * local_work_size - 1)) / (v_width * local_work_size)) * local_work_size;
	scalar_gws = global_work_size * v_width;
#ifdef DEBUG
	fprintf(stderr, "%s(%d) lws %zu gws %zu sgws %zu\n", __FUNCTION__,
	        count, local_work_size, global_work_size, scalar_gws);
#endif
	/// Copy data to gpu
	if (new_keys) {
		HANDLE_CLERROR(clEnqueueWriteBuffer(queue[gpu_id], mem_in, CL_FALSE, 0, key_buf_size, inbuffer, 0, NULL, NULL), "Copy data to gpu");
		new_keys = 0;
	}

	/// Run kernels
	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], pbkdf1_init, 1, NULL, &global_work_size, &local_work_size, 0, NULL, firstEvent), "Run initial kernel");

	for (i = 0; i < LOOP_COUNT; i++) {
		HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], pbkdf1_loop, 1, NULL, &global_work_size, &local_work_size, 0, NULL, NULL), "Run loop kernel");
		HANDLE_CLERROR(clFinish(queue[gpu_id]), "Error running loop kernel");
		opencl_process_event();
	}

	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], pbkdf1_final, 1, NULL, &global_work_size, &local_work_size, 0, NULL, NULL), "Run intermediate kernel");

	/// Read the result back
	HANDLE_CLERROR(clEnqueueReadBuffer(queue[gpu_id], mem_out, CL_TRUE, 0, sizeof(pbkdf1_out) * scalar_gws, host_crack, 0, NULL, NULL), "Copy result back");

	return count;
}

static void *get_salt(char *ciphertext)
{
	static pbkdf1_salt out;
	char tmp[256];
	char *p;

	memset(&out, 0, sizeof(out));
	p = strrchr(ciphertext, '$') + 1;
	strncpy(tmp, ciphertext, p - ciphertext -1);
	tmp[p-ciphertext-1] = 0;
	out.iterations = strtoul(&ciphertext[sizeof(SHA1_MAGIC)-1], NULL, 10);
	p = strrchr(tmp, '$') + 1;
	// real salt used is: <salt><magic><iterations>
	out.length = snprintf((char*)out.salt, sizeof(out.salt), "%.*s%s%u",
	                      (int)strlen(p), p, SHA1_MAGIC, out.iterations);
	return &out;
}

static int cmp_all(void *binary, int count)
{
	int i;

	for (i = 0; i < count; i++)
		if (host_crack[i].dk[0] == ((uint32_t *) binary)[0])
			return 1;
	return 0;
}

static int cmp_one(void *binary, int index)
{
	int i;

	for (i = 0; i < BINARY_SIZE / 4; i++)
		if (host_crack[index].dk[i] != ((uint32_t *) binary)[i])
			return 0;
	return 1;
}

static int cmp_exact(char *source, int index)
{
	return 1;
}

static void clear_keys(void) {
	memset(inbuffer, 0, key_buf_size);
}

static void set_key(char *key, int index)
{
	int i;
	int length = strlen(key);

	for (i = 0; i < length; i++)
		((char*)inbuffer)[GETPOS(i, index)] = key[i];

	new_keys = 1;
}

static char* get_key(int index)
{
	static char ret[PLAINTEXT_LENGTH + 1];
	int i = 0;

	while (i < PLAINTEXT_LENGTH &&
	       (ret[i] = ((char*)inbuffer)[GETPOS(i, index)]))
		i++;
	ret[i] = 0;

	return ret;
}

// Public domain hash function by DJ Bernstein
// We are hashing the entire struct
static int salt_hash(void *salt)
{
	unsigned char *s = salt;
	unsigned int hash = 5381;
	unsigned int i;

	for (i = 0; i < SALT_SIZE; i++)
		hash = ((hash << 5) + hash) ^ s[i];

	return hash & (SALT_HASH_SIZE - 1);
}

#if FMT_MAIN_VERSION > 11
static unsigned int iteration_count(void *salt)
{
	pbkdf1_salt *p = salt;
	return p->iterations;
}
#endif

struct fmt_main fmt_ocl_cryptsha1 = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		BINARY_ALIGN,
		SALT_SIZE,
		SALT_ALIGN,
		MIN_KEYS_PER_CRYPT,
		MAX_KEYS_PER_CRYPT,
		FMT_CASE | FMT_8_BIT,
#if FMT_MAIN_VERSION > 11
		{
			"iteration count",
		},
#endif
		tests
	}, {
		init,
		done,
		fmt_default_reset,
		fmt_default_prepare,
		valid,
		fmt_default_split,
		get_binary,
		get_salt,
#if FMT_MAIN_VERSION > 11
		{
			iteration_count,
		},
#endif
		fmt_default_source,
		{
			binary_hash_0,
			fmt_default_binary_hash_1,
			fmt_default_binary_hash_2,
			fmt_default_binary_hash_3,
			fmt_default_binary_hash_4,
			fmt_default_binary_hash_5,
			fmt_default_binary_hash_6
		},
		salt_hash,
		set_salt,
		set_key,
		get_key,
		clear_keys,
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
#endif /* HAVE_OPENCL */