/*
 * IO verification helpers
 */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include "fio.h"
#include "verify.h"
#include "smalloc.h"
#include "lib/rand.h"

#include "crc/md5.h"
#include "crc/crc64.h"
#include "crc/crc32.h"
#include "crc/crc32c.h"
#include "crc/crc16.h"
#include "crc/crc7.h"
#include "crc/sha256.h"
#include "crc/sha512.h"
#include "crc/sha1.h"

void fill_pattern(struct thread_data *td, void *p, unsigned int len, struct io_u *io_u)
{
	switch (td->o.verify_pattern_bytes) {
	case 0:
		dprint(FD_VERIFY, "fill random bytes len=%u\n", len);
		fill_random_buf(p, len);
		break;
	case 1:
		if (io_u->buf_filled && io_u->buf_filled_len >= len) {
			dprint(FD_VERIFY, "using already filled verify pattern b=0 len=%u\n", len);
			return;
		}
		dprint(FD_VERIFY, "fill verify pattern b=0 len=%u\n", len);
		memset(p, td->o.verify_pattern[0], len);
		io_u->buf_filled = 1;
		io_u->buf_filled_len = len;
		break;
	default: {
		unsigned int i = 0, size = 0;
		unsigned char *b = p;

		if (io_u->buf_filled && io_u->buf_filled_len >= len) {
			dprint(FD_VERIFY, "using already filled verify pattern b=%d len=%u\n",
					td->o.verify_pattern_bytes, len);
			return;
		}
		dprint(FD_VERIFY, "fill verify pattern b=%d len=%u\n",
					td->o.verify_pattern_bytes, len);

		while (i < len) {
			size = td->o.verify_pattern_bytes;
			if (size > (len - i))
				size = len - i;
			memcpy(b+i, td->o.verify_pattern, size);
			i += size;
		}
		io_u->buf_filled = 1;
		io_u->buf_filled_len = len;
		break;
		}
	}
}

static void memswp(void *buf1, void *buf2, unsigned int len)
{
	char swap[200];

	assert(len <= sizeof(swap));

	memcpy(&swap, buf1, len);
	memcpy(buf1, buf2, len);
	memcpy(buf2, &swap, len);
}

static void hexdump(void *buffer, int len)
{
	unsigned char *p = buffer;
	int i;

	for (i = 0; i < len; i++)
		log_err("%02x", p[i]);
	log_err("\n");
}

/*
 * Prepare for seperation of verify_header and checksum header
 */
static inline unsigned int __hdr_size(int verify_type)
{
	unsigned int len = len;

	switch (verify_type) {
	case VERIFY_NONE:
	case VERIFY_NULL:
		len = 0;
		break;
	case VERIFY_MD5:
		len = sizeof(struct vhdr_md5);
		break;
	case VERIFY_CRC64:
		len = sizeof(struct vhdr_crc64);
		break;
	case VERIFY_CRC32C:
	case VERIFY_CRC32:
	case VERIFY_CRC32C_INTEL:
		len = sizeof(struct vhdr_crc32);
		break;
	case VERIFY_CRC16:
		len = sizeof(struct vhdr_crc16);
		break;
	case VERIFY_CRC7:
		len = sizeof(struct vhdr_crc7);
		break;
	case VERIFY_SHA256:
		len = sizeof(struct vhdr_sha256);
		break;
	case VERIFY_SHA512:
		len = sizeof(struct vhdr_sha512);
		break;
	case VERIFY_META:
		len = sizeof(struct vhdr_meta);
		break;
	case VERIFY_SHA1:
		len = sizeof(struct vhdr_sha1);
		break;
	default:
		log_err("fio: unknown verify header!\n");
		assert(0);
	}

	return len + sizeof(struct verify_header);
}

static inline unsigned int hdr_size(struct verify_header *hdr)
{
	return __hdr_size(hdr->verify_type);
}

static void *hdr_priv(struct verify_header *hdr)
{
	void *priv = hdr;

	return priv + sizeof(struct verify_header);
}

/*
 * Verify container, pass info to verify handlers and allow them to
 * pass info back in case of error
 */
struct vcont {
	/*
	 * Input
	 */
	struct io_u *io_u;
	unsigned int hdr_num;

	/*
	 * Output, only valid in case of error
	 */
	const char *name;
	void *good_crc;
	void *bad_crc;
	unsigned int crc_len;
};

static void log_verify_failure(struct verify_header *hdr, struct vcont *vc)
{
	unsigned long long offset;

	offset = vc->io_u->offset;
	offset += vc->hdr_num * hdr->len;
	log_err("%.8s: verify failed at file %s offset %llu, length %u\n",
			vc->name, vc->io_u->file->file_name, offset, hdr->len);

	if (vc->good_crc && vc->bad_crc) {
		log_err("       Expected CRC: ");
		hexdump(vc->good_crc, vc->crc_len);
		log_err("       Received CRC: ");
		hexdump(vc->bad_crc, vc->crc_len);
	}
}

/*
 * Return data area 'header_num'
 */
static inline void *io_u_verify_off(struct verify_header *hdr, struct vcont *vc)
{
	return vc->io_u->buf + vc->hdr_num * hdr->len + hdr_size(hdr);
}

static int verify_io_u_meta(struct verify_header *hdr, struct thread_data *td,
			    struct vcont *vc)
{
	struct vhdr_meta *vh = hdr_priv(hdr);
	struct io_u *io_u = vc->io_u;

	dprint(FD_VERIFY, "meta verify io_u %p, len %u\n", io_u, hdr->len);

	if (vh->offset == io_u->offset + vc->hdr_num * td->o.verify_interval)
		return 0;

	vc->name = "meta";
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

static int verify_io_u_sha512(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_sha512 *vh = hdr_priv(hdr);
	uint8_t sha512[128];
	struct sha512_ctx sha512_ctx = {
		.buf = sha512,
	};

	dprint(FD_VERIFY, "sha512 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	sha512_init(&sha512_ctx);
	sha512_update(&sha512_ctx, p, hdr->len - hdr_size(hdr));

	if (!memcmp(vh->sha512, sha512_ctx.buf, sizeof(sha512)))
		return 0;

	vc->name = "sha512";
	vc->good_crc = vh->sha512;
	vc->bad_crc = sha512_ctx.buf;
	vc->crc_len = sizeof(vh->sha512);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

static int verify_io_u_sha256(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_sha256 *vh = hdr_priv(hdr);
	uint8_t sha256[64];
	struct sha256_ctx sha256_ctx = {
		.buf = sha256,
	};

	dprint(FD_VERIFY, "sha256 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	sha256_init(&sha256_ctx);
	sha256_update(&sha256_ctx, p, hdr->len - hdr_size(hdr));

	if (!memcmp(vh->sha256, sha256_ctx.buf, sizeof(sha256)))
		return 0;

	vc->name = "sha256";
	vc->good_crc = vh->sha256;
	vc->bad_crc = sha256_ctx.buf;
	vc->crc_len = sizeof(vh->sha256);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

static int verify_io_u_sha1(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_sha1 *vh = hdr_priv(hdr);
	uint32_t sha1[5];
	struct sha1_ctx sha1_ctx = {
		.H = sha1,
	};

	dprint(FD_VERIFY, "sha1 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	sha1_init(&sha1_ctx);
	sha1_update(&sha1_ctx, p, hdr->len - hdr_size(hdr));

	if (!memcmp(vh->sha1, sha1_ctx.H, sizeof(sha1)))
		return 0;

	vc->name = "sha1";
	vc->good_crc = vh->sha1;
	vc->bad_crc = sha1_ctx.H;
	vc->crc_len = sizeof(vh->sha1);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

static int verify_io_u_crc7(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc7 *vh = hdr_priv(hdr);
	unsigned char c;

	dprint(FD_VERIFY, "crc7 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = crc7(p, hdr->len - hdr_size(hdr));

	if (c == vh->crc7)
		return 0;

	vc->name = "crc7";
	vc->good_crc = &vh->crc7;
	vc->bad_crc = &c;
	vc->crc_len = 1;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

static int verify_io_u_crc16(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc16 *vh = hdr_priv(hdr);
	unsigned short c;

	dprint(FD_VERIFY, "crc16 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = crc16(p, hdr->len - hdr_size(hdr));

	if (c == vh->crc16)
		return 0;

	vc->name = "crc16";
	vc->good_crc = &vh->crc16;
	vc->bad_crc = &c;
	vc->crc_len = 2;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

static int verify_io_u_crc64(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc64 *vh = hdr_priv(hdr);
	unsigned long long c;

	dprint(FD_VERIFY, "crc64 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = crc64(p, hdr->len - hdr_size(hdr));

	if (c == vh->crc64)
		return 0;

	vc->name = "crc64";
	vc->good_crc = &vh->crc64;
	vc->bad_crc = &c;
	vc->crc_len = 8;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

static int verify_io_u_crc32(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc32 *vh = hdr_priv(hdr);
	uint32_t c;

	dprint(FD_VERIFY, "crc32 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = crc32(p, hdr->len - hdr_size(hdr));

	if (c == vh->crc32)
		return 0;

	vc->name = "crc32";
	vc->good_crc = &vh->crc32;
	vc->bad_crc = &c;
	vc->crc_len = 4;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

static int verify_io_u_crc32c(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc32 *vh = hdr_priv(hdr);
	uint32_t c;

	dprint(FD_VERIFY, "crc32c verify io_u %p, len %u\n", vc->io_u, hdr->len);

	if (hdr->verify_type == VERIFY_CRC32C_INTEL)
		c = crc32c_intel(p, hdr->len - hdr_size(hdr));
	else
		c = crc32c(p, hdr->len - hdr_size(hdr));

	if (c == vh->crc32)
		return 0;

	vc->name = "crc32c";
	vc->good_crc = &vh->crc32;
	vc->bad_crc = &c;
	vc->crc_len = 4;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

static int verify_io_u_md5(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_md5 *vh = hdr_priv(hdr);
	uint32_t hash[MD5_HASH_WORDS];
	struct md5_ctx md5_ctx = {
		.hash = hash,
	};

	dprint(FD_VERIFY, "md5 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	md5_init(&md5_ctx);
	md5_update(&md5_ctx, p, hdr->len - hdr_size(hdr));

	if (!memcmp(vh->md5_digest, md5_ctx.hash, sizeof(hash)))
		return 0;

	vc->name = "md5";
	vc->good_crc = vh->md5_digest;
	vc->bad_crc = md5_ctx.hash;
	vc->crc_len = sizeof(hash);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

static unsigned int hweight8(unsigned int w)
{
	unsigned int res = w - ((w >> 1) & 0x55);

	res = (res & 0x33) + ((res >> 2) & 0x33);
	return (res + (res >> 4)) & 0x0F;
}

int verify_io_u_pattern(char *pattern, unsigned long pattern_size,
			char *buf, unsigned int len, unsigned int mod)
{
	unsigned int i;

	for (i = 0; i < len; i++) {
		if (buf[i] != pattern[mod]) {
			unsigned int bits;

			bits = hweight8(buf[i] ^ pattern[mod]);
			log_err("fio: got pattern %x, wanted %x. Bad bits %d\n",
				buf[i], pattern[mod], bits);
			log_err("fio: bad pattern block offset %u\n", i);
			return EILSEQ;
		}
		mod++;
		if (mod == pattern_size)
			mod = 0;
	}

	return 0;
}

/*
 * Push IO verification to a separate thread
 */
int verify_io_u_async(struct thread_data *td, struct io_u *io_u)
{
	if (io_u->file)
		put_file_log(td, io_u->file);

	io_u->file = NULL;

	pthread_mutex_lock(&td->io_u_lock);
	
	if (io_u->flags & IO_U_F_IN_CUR_DEPTH) {
		td->cur_depth--;
		io_u->flags &= ~IO_U_F_IN_CUR_DEPTH;
	}
	flist_del(&io_u->list);
	flist_add_tail(&io_u->list, &td->verify_list);
	io_u->flags |= IO_U_F_FREE_DEF;
	pthread_mutex_unlock(&td->io_u_lock);

	pthread_cond_signal(&td->verify_cond);
	return 0;
}

int verify_io_u(struct thread_data *td, struct io_u *io_u)
{
	struct verify_header *hdr;
	unsigned int hdr_size, hdr_inc, hdr_num = 0;
	void *p;
	int ret;

	if (td->o.verify == VERIFY_NULL || io_u->ddir != DDIR_READ)
		return 0;

	hdr_inc = io_u->buflen;
	if (td->o.verify_interval)
		hdr_inc = td->o.verify_interval;

	ret = 0;
	for (p = io_u->buf; p < io_u->buf + io_u->buflen;
	     p += hdr_inc, hdr_num++) {
		struct vcont vc = {
			.io_u		= io_u,
			.hdr_num	= hdr_num,
		};

		if (ret && td->o.verify_fatal)
			break;

		hdr_size = __hdr_size(td->o.verify);
		if (td->o.verify_offset)
			memswp(p, p + td->o.verify_offset, hdr_size);
		hdr = p;

		if (hdr->fio_magic != FIO_HDR_MAGIC) {
			log_err("verify: bad magic header %x, wanted %x at file %s offset %llu, length %u\n",
				hdr->fio_magic, FIO_HDR_MAGIC,
				io_u->file->file_name,
				io_u->offset + hdr_num * hdr->len, hdr->len);
			return EILSEQ;
		}

		if (td->o.verify_pattern_bytes) {
			dprint(FD_VERIFY, "pattern verify io_u %p, len %u\n",
								io_u, hdr->len);
			ret = verify_io_u_pattern(td->o.verify_pattern,
				  td->o.verify_pattern_bytes,
				  p + hdr_size,
				  hdr_inc - hdr_size,
				  hdr_size % td->o.verify_pattern_bytes);

			if (ret) {
				log_err("pattern: verify failed at file %s offset %llu, length %u\n",
					io_u->file->file_name,
					io_u->offset + hdr_num * hdr->len,
					hdr->len);
			}

			/*
			 * Also verify the meta data, if applicable
			 */
			if (hdr->verify_type == VERIFY_META)
				ret |= verify_io_u_meta(hdr, td, &vc);
			continue;
		}

		switch (hdr->verify_type) {
		case VERIFY_MD5:
			ret = verify_io_u_md5(hdr, &vc);
			break;
		case VERIFY_CRC64:
			ret = verify_io_u_crc64(hdr, &vc);
			break;
		case VERIFY_CRC32C:
		case VERIFY_CRC32C_INTEL:
			ret = verify_io_u_crc32c(hdr, &vc);
			break;
		case VERIFY_CRC32:
			ret = verify_io_u_crc32(hdr, &vc);
			break;
		case VERIFY_CRC16:
			ret = verify_io_u_crc16(hdr, &vc);
			break;
		case VERIFY_CRC7:
			ret = verify_io_u_crc7(hdr, &vc);
			break;
		case VERIFY_SHA256:
			ret = verify_io_u_sha256(hdr, &vc);
			break;
		case VERIFY_SHA512:
			ret = verify_io_u_sha512(hdr, &vc);
			break;
		case VERIFY_META:
			ret = verify_io_u_meta(hdr, td, &vc);
			break;
		case VERIFY_SHA1:
			ret = verify_io_u_sha1(hdr, &vc);
			break;
		default:
			log_err("Bad verify type %u\n", hdr->verify_type);
			ret = EINVAL;
		}
	}

	if (ret && td->o.verify_fatal)
		td->terminate = 1;

	return ret;
}

static void fill_meta(struct verify_header *hdr, struct thread_data *td,
		      struct io_u *io_u, unsigned int header_num)
{
	struct vhdr_meta *vh = hdr_priv(hdr);

	vh->thread = td->thread_number;

	vh->time_sec = io_u->start_time.tv_sec;
	vh->time_usec = io_u->start_time.tv_usec;

	vh->numberio = td->io_issues[DDIR_WRITE];

	vh->offset = io_u->offset + header_num * td->o.verify_interval;
}

static void fill_sha512(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha512 *vh = hdr_priv(hdr);
	struct sha512_ctx sha512_ctx = {
		.buf = vh->sha512,
	};

	sha512_init(&sha512_ctx);
	sha512_update(&sha512_ctx, p, len);
}

static void fill_sha256(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha256 *vh = hdr_priv(hdr);
	struct sha256_ctx sha256_ctx = {
		.buf = vh->sha256,
	};

	sha256_init(&sha256_ctx);
	sha256_update(&sha256_ctx, p, len);
}

static void fill_sha1(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha1 *vh = hdr_priv(hdr);
	struct sha1_ctx sha1_ctx = {
		.H = vh->sha1,
	};

	sha1_init(&sha1_ctx);
	sha1_update(&sha1_ctx, p, len);
}

static void fill_crc7(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc7 *vh = hdr_priv(hdr);

	vh->crc7 = crc7(p, len);
}

static void fill_crc16(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc16 *vh = hdr_priv(hdr);

	vh->crc16 = crc16(p, len);
}

static void fill_crc32(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc32 *vh = hdr_priv(hdr);

	vh->crc32 = crc32(p, len);
}

static void fill_crc32c(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc32 *vh = hdr_priv(hdr);

	if (hdr->verify_type == VERIFY_CRC32C_INTEL)
		vh->crc32 = crc32c_intel(p, len);
	else
		vh->crc32 = crc32c(p, len);
}

static void fill_crc64(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc64 *vh = hdr_priv(hdr);

	vh->crc64 = crc64(p, len);
}

static void fill_md5(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_md5 *vh = hdr_priv(hdr);
	struct md5_ctx md5_ctx = {
		.hash = (uint32_t *) vh->md5_digest,
	};

	md5_init(&md5_ctx);
	md5_update(&md5_ctx, p, len);
}

/*
 * fill body of io_u->buf with random data and add a header with the
 * crc32 or md5 sum of that data.
 */
void populate_verify_io_u(struct thread_data *td, struct io_u *io_u)
{
	struct verify_header *hdr;
	void *p = io_u->buf, *data;
	unsigned int hdr_inc, data_len, header_num = 0;

	if (td->o.verify == VERIFY_NULL)
		return;

	fill_pattern(td, p, io_u->buflen, io_u);

	hdr_inc = io_u->buflen;
	if (td->o.verify_interval)
		hdr_inc = td->o.verify_interval;

	for (; p < io_u->buf + io_u->buflen; p += hdr_inc) {
		hdr = p;

		hdr->fio_magic = FIO_HDR_MAGIC;
		hdr->verify_type = td->o.verify;
		hdr->len = hdr_inc;
		data_len = hdr_inc - hdr_size(hdr);

		data = p + hdr_size(hdr);
		switch (td->o.verify) {
		case VERIFY_MD5:
			dprint(FD_VERIFY, "fill md5 io_u %p, len %u\n",
							io_u, hdr->len);
			fill_md5(hdr, data, data_len);
			break;
		case VERIFY_CRC64:
			dprint(FD_VERIFY, "fill crc64 io_u %p, len %u\n",
							io_u, hdr->len);
			fill_crc64(hdr, data, data_len);
			break;
		case VERIFY_CRC32C:
		case VERIFY_CRC32C_INTEL:
			dprint(FD_VERIFY, "fill crc32c io_u %p, len %u\n",
							io_u, hdr->len);
			fill_crc32c(hdr, data, data_len);
			break;
		case VERIFY_CRC32:
			dprint(FD_VERIFY, "fill crc32 io_u %p, len %u\n",
							io_u, hdr->len);
			fill_crc32(hdr, data, data_len);
			break;
		case VERIFY_CRC16:
			dprint(FD_VERIFY, "fill crc16 io_u %p, len %u\n",
							io_u, hdr->len);
			fill_crc16(hdr, data, data_len);
			break;
		case VERIFY_CRC7:
			dprint(FD_VERIFY, "fill crc7 io_u %p, len %u\n",
							io_u, hdr->len);
			fill_crc7(hdr, data, data_len);
			break;
		case VERIFY_SHA256:
			dprint(FD_VERIFY, "fill sha256 io_u %p, len %u\n",
							io_u, hdr->len);
			fill_sha256(hdr, data, data_len);
			break;
		case VERIFY_SHA512:
			dprint(FD_VERIFY, "fill sha512 io_u %p, len %u\n",
							io_u, hdr->len);
			fill_sha512(hdr, data, data_len);
			break;
		case VERIFY_META:
			dprint(FD_VERIFY, "fill meta io_u %p, len %u\n",
							io_u, hdr->len);
			fill_meta(hdr, td, io_u, header_num);
			break;
		case VERIFY_SHA1:
			dprint(FD_VERIFY, "fill sha1 io_u %p, len %u\n",
							io_u, hdr->len);
			fill_sha1(hdr, data, data_len);
			break;
		default:
			log_err("fio: bad verify type: %d\n", td->o.verify);
			assert(0);
		}
		if (td->o.verify_offset)
			memswp(p, p + td->o.verify_offset, hdr_size(hdr));
		header_num++;
	}
}

int get_next_verify(struct thread_data *td, struct io_u *io_u)
{
	struct io_piece *ipo = NULL;

	/*
	 * this io_u is from a requeue, we already filled the offsets
	 */
	if (io_u->file)
		return 0;

	if (!RB_EMPTY_ROOT(&td->io_hist_tree)) {
		struct rb_node *n = rb_first(&td->io_hist_tree);

		ipo = rb_entry(n, struct io_piece, rb_node);
		rb_erase(n, &td->io_hist_tree);
		td->io_hist_len--;
	} else if (!flist_empty(&td->io_hist_list)) {
		ipo = flist_entry(td->io_hist_list.next, struct io_piece, list);
		td->io_hist_len--;
		flist_del(&ipo->list);
	}

	if (ipo) {
		io_u->offset = ipo->offset;
		io_u->buflen = ipo->len;
		io_u->file = ipo->file;

		if (!fio_file_open(io_u->file)) {
			int r = td_io_open_file(td, io_u->file);

			if (r) {
				dprint(FD_VERIFY, "failed file %s open\n",
						io_u->file->file_name);
				return 1;
			}
		}

		get_file(ipo->file);
		assert(fio_file_open(io_u->file));
		io_u->ddir = DDIR_READ;
		io_u->xfer_buf = io_u->buf;
		io_u->xfer_buflen = io_u->buflen;
		free(ipo);
		dprint(FD_VERIFY, "get_next_verify: ret io_u %p\n", io_u);
		return 0;
	}

	dprint(FD_VERIFY, "get_next_verify: empty\n");
	return 1;
}

static void *verify_async_thread(void *data)
{
	struct thread_data *td = data;
	struct io_u *io_u;
	int ret = 0;

	if (td->o.verify_cpumask_set &&
	    fio_setaffinity(td->pid, td->o.verify_cpumask)) {
		log_err("fio: failed setting verify thread affinity\n");
		goto done;
	}

	do {
		FLIST_HEAD(list);

		read_barrier();
		if (td->verify_thread_exit)
			break;

		pthread_mutex_lock(&td->io_u_lock);

		while (flist_empty(&td->verify_list) &&
		       !td->verify_thread_exit) {
			ret = pthread_cond_wait(&td->verify_cond,
							&td->io_u_lock);
			if (ret) {
				pthread_mutex_unlock(&td->io_u_lock);
				break;
			}
		}

		flist_splice_init(&td->verify_list, &list);
		pthread_mutex_unlock(&td->io_u_lock);

		if (flist_empty(&list))
			continue;

		while (!flist_empty(&list)) {
			io_u = flist_entry(list.next, struct io_u, list);
			flist_del_init(&io_u->list);

			ret = verify_io_u(td, io_u);
			put_io_u(td, io_u);
			if (!ret)
				continue;
			if (td->o.continue_on_error &&
			    td_non_fatal_error(ret)) {
				update_error_count(td, ret);
				td_clear_error(td);
				ret = 0;
			}
		}
	} while (!ret);

	if (ret) {
		td_verror(td, ret, "async_verify");
		if (td->o.verify_fatal)
			td->terminate = 1;
	}

done:
	pthread_mutex_lock(&td->io_u_lock);
	td->nr_verify_threads--;
	pthread_mutex_unlock(&td->io_u_lock);

	pthread_cond_signal(&td->free_cond);
	return NULL;
}

int verify_async_init(struct thread_data *td)
{
	int i, ret;

	td->verify_thread_exit = 0;

	td->verify_threads = malloc(sizeof(pthread_t) * td->o.verify_async);
	for (i = 0; i < td->o.verify_async; i++) {
		ret = pthread_create(&td->verify_threads[i], NULL,
					verify_async_thread, td);
		if (ret) {
			log_err("fio: async verify creation failed: %s\n",
					strerror(ret));
			break;
		}
		ret = pthread_detach(td->verify_threads[i]);
		if (ret) {
			log_err("fio: async verify thread detach failed: %s\n",
					strerror(ret));
			break;
		}
		td->nr_verify_threads++;
	}

	if (i != td->o.verify_async) {
		log_err("fio: only %d verify threads started, exiting\n", i);
		td->verify_thread_exit = 1;
		write_barrier();
		pthread_cond_broadcast(&td->verify_cond);
		return 1;
	}

	return 0;
}

void verify_async_exit(struct thread_data *td)
{
	td->verify_thread_exit = 1;
	write_barrier();
	pthread_cond_broadcast(&td->verify_cond);

	pthread_mutex_lock(&td->io_u_lock);

	while (td->nr_verify_threads)
		pthread_cond_wait(&td->free_cond, &td->io_u_lock);

	pthread_mutex_unlock(&td->io_u_lock);
	free(td->verify_threads);
	td->verify_threads = NULL;
}
