/* Copyright (c) 2020 Dovecot authors, see the included COPYING file */

#include "lib.h"

#ifdef HAVE_ZSTD

#include "ostream.h"
#include "ostream-private.h"
#include "settings.h"
#include "ostream-zlib.h"

#include "zstd.h"
#include "zstd_errors.h"
#include "iostream-zstd-private.h"

#if HAVE_DECL_ZSTD_MINCLEVEL != 1
#  define ZSTD_minCLevel() 1
#endif

struct zstd_ostream {
	struct ostream_private ostream;

	ZSTD_CStream *cstream;
	ZSTD_outBuffer output;

	unsigned char *outbuf;

	bool flushed:1;
	bool closed:1;
	bool finished:1;
};

struct zstd_settings {
	pool_t pool;
	unsigned int compress_zstd_level;
};

static bool zstd_settings_check(void *_set, pool_t pool, const char **error_r);

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct zstd_settings)
static const struct setting_define zstd_setting_defines[] = {
	DEF(UINT, compress_zstd_level),

	SETTING_DEFINE_LIST_END
};
static const struct zstd_settings zstd_default_settings = {
	.compress_zstd_level = 3,
};

const struct setting_parser_info zstd_setting_parser_info = {
	.name = "zstd",

	.defines = zstd_setting_defines,
	.defaults = &zstd_default_settings,

	.struct_size = sizeof(struct zstd_settings),
	.pool_offset1 = 1 + offsetof(struct zstd_settings, pool),
#ifndef CONFIG_BINARY
	.check_func = zstd_settings_check,
#endif
};

static bool zstd_settings_check(void *_set, pool_t pool ATTR_UNUSED,
				 const char **error_r)
{
	struct zstd_settings *set = _set;

	if ((int)set->compress_zstd_level < ZSTD_minCLevel() ||
	    (int)set->compress_zstd_level > ZSTD_maxCLevel()) {
		*error_r = t_strdup_printf(
			"compress_zstd_level must be between %d..%d",
			ZSTD_minCLevel(), ZSTD_maxCLevel());
		return FALSE;
	}
	return TRUE;
}

static void o_stream_zstd_write_error(struct zstd_ostream *zstream, size_t err)
{
	ZSTD_ErrorCode errcode = zstd_version_errcode(ZSTD_getErrorCode(err));
	const char *error = ZSTD_getErrorName(err);
	if (errcode == ZSTD_error_memory_allocation)
		i_fatal_status(FATAL_OUTOFMEM, "zstd.write(%s): Out of memory",
			       o_stream_get_name(&zstream->ostream.ostream));
	else if (errcode == ZSTD_error_prefix_unknown ||
#if HAVE_DECL_ZSTD_ERROR_PARAMETER_UNSUPPORTED == 1
		 errcode == ZSTD_error_parameter_unsupported ||
#endif
		 errcode == ZSTD_error_dictionary_wrong ||
		 errcode == ZSTD_error_init_missing)
		zstream->ostream.ostream.stream_errno = EINVAL;
	else
		zstream->ostream.ostream.stream_errno = EIO;

	io_stream_set_error(&zstream->ostream.iostream,
			    "zstd.write(%s): %s at %"PRIuUOFF_T,
			    o_stream_get_name(&zstream->ostream.ostream), error,
			    zstream->ostream.ostream.offset);
}

static ssize_t o_stream_zstd_send_outbuf(struct zstd_ostream *zstream)
{
	ssize_t ret;
	/* nothing to send */
	if (zstream->output.pos == 0)
		return 1;
	ret = o_stream_send(zstream->ostream.parent, zstream->output.dst,
			    zstream->output.pos);
	if (ret < 0) {
		o_stream_copy_error_from_parent(&zstream->ostream);
		return -1;
	} else {
		memmove(zstream->outbuf, zstream->outbuf+ret, zstream->output.pos-ret);
		zstream->output.pos -= ret;
	}
	if (zstream->output.pos > 0) {
		/* We couldn't send everything to parent stream, but we
		   accepted all the input already. Set the ostream's flush
		   pending so when there's more space in the parent stream
		   we'll continue sending the rest of the data. */
		o_stream_set_flush_pending(&zstream->ostream.ostream, TRUE);
		return 0;
	}
	return 1;
}

static ssize_t
o_stream_zstd_sendv(struct ostream_private *stream,
		    const struct const_iovec *iov, unsigned int iov_count)
{
	struct zstd_ostream *zstream =
		container_of(stream, struct zstd_ostream, ostream);
	ssize_t total = 0;
	size_t ret;

	for (unsigned int i = 0; i < iov_count; i++) {
		/* does it actually fit there */
		ZSTD_inBuffer input = {
			.src = iov[i].iov_base,
			.pos = 0,
			.size = iov[i].iov_len
		};
		bool flush_attempted = FALSE;
		for (;;) {
			size_t prev_pos = input.pos;
			ret = ZSTD_compressStream(zstream->cstream, &zstream->output,
						  &input);
			if (ZSTD_isError(ret) != 0) {
				o_stream_zstd_write_error(zstream, ret);
				return -1;
			}
			size_t new_input_size = input.pos - prev_pos;
			if (new_input_size == 0 && flush_attempted) {
				/* non-blocking output buffer full */
				return total;
			}
			stream->ostream.offset += new_input_size;
			total += new_input_size;
			if (input.pos == input.size)
				break;
			/* output buffer full. try to flush it. */
			if (o_stream_zstd_send_outbuf(zstream) < 0)
				return -1;
			flush_attempted = TRUE;
		}
	}
	if (o_stream_zstd_send_outbuf(zstream) < 0)
		return -1;
	return total;
}

static int o_stream_zstd_send_flush(struct zstd_ostream *zstream, bool final)
{
	int ret;

	if (zstream->flushed) {
		i_assert(zstream->output.pos == 0);
		return 1;
	}

	if ((ret = o_stream_flush_parent_if_needed(&zstream->ostream)) <= 0)
		return ret;

	if (zstream->output.pos == 0)
		ZSTD_flushStream(zstream->cstream, &zstream->output);

	if ((ret = o_stream_zstd_send_outbuf(zstream)) <= 0)
		return ret;

	if (!final)
		return 1;

	if (!zstream->finished) {
		ret = ZSTD_endStream(zstream->cstream, &zstream->output);
		if (ZSTD_isError(ret) != 0) {
			o_stream_zstd_write_error(zstream, ret);
			return -1;
		}
		zstream->finished = TRUE;
	}

	if ((ret = o_stream_zstd_send_outbuf(zstream)) <= 0)
		return ret;

	if (final)
		zstream->flushed = TRUE;
	i_assert(zstream->output.pos == 0);
	return 1;
}

static int o_stream_zstd_flush(struct ostream_private *stream)
{
	struct zstd_ostream *zstream =
		container_of(stream, struct zstd_ostream, ostream);

	int ret;
	if ((ret = o_stream_zstd_send_flush(zstream, stream->finished)) < 0)
		return -1;
	else if (ret > 0)
		return o_stream_flush_parent(stream);
	return ret;
}

static void o_stream_zstd_close(struct iostream_private *stream,
				bool close_parent)
{
	struct ostream_private *_ostream =
		container_of(stream, struct ostream_private, iostream);
	struct zstd_ostream *zstream =
		container_of(_ostream, struct zstd_ostream, ostream);

	i_assert(zstream->ostream.finished ||
		 zstream->ostream.ostream.stream_errno != 0 ||
		 zstream->ostream.error_handling_disabled);
	if (zstream->cstream != NULL) {
		ZSTD_freeCStream(zstream->cstream);
		zstream->cstream = NULL;
	}
	i_free(zstream->outbuf);
	i_zero(&zstream->output);
	if (close_parent)
		o_stream_close(zstream->ostream.parent);
}

static struct ostream *
o_stream_create_zstd(struct ostream *output, int level)
{
	struct zstd_ostream *zstream;
	size_t ret;

	i_assert(level >= ZSTD_minCLevel() && level <= ZSTD_maxCLevel());

	zstd_version_check();

	zstream = i_new(struct zstd_ostream, 1);
	zstream->ostream.sendv = o_stream_zstd_sendv;
	zstream->ostream.flush = o_stream_zstd_flush;
	zstream->ostream.iostream.close = o_stream_zstd_close;
	zstream->cstream = ZSTD_createCStream();
	if (zstream->cstream == NULL)
		i_fatal_status(FATAL_OUTOFMEM, "zstd: Out of memory");
	ret = ZSTD_initCStream(zstream->cstream, level);
	if (ZSTD_isError(ret) != 0)
		o_stream_zstd_write_error(zstream, ret);
	else {
		zstream->outbuf = i_malloc(ZSTD_CStreamOutSize());
		zstream->output.dst = zstream->outbuf;
		zstream->output.size = ZSTD_CStreamOutSize();
	}
	o_stream_init_buffering_flush(&zstream->ostream, output);
	return o_stream_create(&zstream->ostream, output,
			       o_stream_get_fd(output));
}

struct ostream *
o_stream_create_zstd_auto(struct ostream *output, struct event *event)
{
	const struct zstd_settings *set;
	const char *error;

	if (settings_get(event, &zstd_setting_parser_info, 0,
			 &set, &error) < 0)
		return o_stream_create_error_str(EIO, "%s", error);
	int level = set->compress_zstd_level;
	settings_free(set);
	return o_stream_create_zstd(output, level);
}

#endif
