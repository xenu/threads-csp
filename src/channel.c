#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"

#include <stdatomic.h>

#include "channel.h"

/*
 * Message channels
 */

enum state { HAS_NOTHING, HAS_READER, HAS_WRITER, HAS_MESSAGE };
static const char* state_names[] = { "nothing", "has-reader", "has-writer", "has-message" };

struct channel {
	perl_mutex data_mutex;
	perl_mutex reader_mutex;
	perl_mutex writer_mutex;
	perl_cond  data_condvar;

	atomic_size_t refcount;
	enum state state;
	SV* message;
};

Channel* channel_alloc(UV refcount) {
	Channel* ret = PerlMemShared_calloc(1, sizeof(Channel));
	MUTEX_INIT(&ret->data_mutex);
	MUTEX_INIT(&ret->reader_mutex);
	MUTEX_INIT(&ret->writer_mutex);
	COND_INIT(&ret->data_condvar);
	atomic_init(&ret->refcount, refcount);
	return ret;
}

void channel_send(Channel* channel, SV* message) {
	MUTEX_LOCK(&channel->writer_mutex);
	MUTEX_LOCK(&channel->data_mutex);

	channel->message = message;
	if (channel->state == HAS_READER) {
		channel->state = HAS_MESSAGE;
		COND_SIGNAL(&channel->data_condvar);
	}
	else {
		assert(channel->state == HAS_NOTHING);
		channel->state = HAS_WRITER;
	}

	do COND_WAIT(&channel->data_condvar, &channel->data_mutex);
	while (channel->state != HAS_NOTHING && channel->state != HAS_READER);

	MUTEX_UNLOCK(&channel->data_mutex);
	MUTEX_UNLOCK(&channel->writer_mutex);
}

SV* S_channel_receive(pTHX_ Channel* channel) {
	MUTEX_LOCK(&channel->reader_mutex);
	MUTEX_LOCK(&channel->data_mutex);

	if (channel->state == HAS_NOTHING) {
		channel->state = HAS_READER;
		do COND_WAIT(&channel->data_condvar, &channel->data_mutex);
		while (channel->state != HAS_MESSAGE);
	}
	else
		assert(channel->state == HAS_WRITER);

	CLONE_PARAMS params = { 0 };
	params.flags = CLONEf_JOIN_IN;
	SAVEPPTR(PL_ptr_table);
	PL_ptr_table = ptr_table_new();
	SAVEDESTRUCTOR_X(Perl_ptr_table_free, PL_ptr_table);
	SV* result = sv_dup_inc(channel->message, &params);

	channel->message = NULL;
	channel->state = HAS_NOTHING;
	COND_SIGNAL(&channel->data_condvar);

	MUTEX_UNLOCK(&channel->data_mutex);
	MUTEX_UNLOCK(&channel->reader_mutex);

	return result;
}

void channel_refcount_dec(Channel* channel) {
	if (atomic_fetch_sub_explicit(&channel->refcount, 1, memory_order_acq_rel) == 1) {
		COND_DESTROY(&channel->data_condvar);
		MUTEX_DESTROY(&channel->writer_mutex);
		MUTEX_DESTROY(&channel->reader_mutex);
		MUTEX_DESTROY(&channel->data_mutex);
		PerlMemShared_free(channel);
	}
}

static int channel_magic_destroy(pTHX_ SV* sv, MAGIC* magic) {
	channel_refcount_dec((Channel*)magic->mg_ptr);
	return 0;
}

static int channel_magic_dup(pTHX_ MAGIC* magic, CLONE_PARAMS* param) {
	Channel* channel = (Channel*)magic->mg_ptr;
	atomic_fetch_add_explicit(&channel->refcount, 1, memory_order_relaxed);
	return 0;
}

static const MGVTBL channel_magic = { 0, 0, 0, 0, channel_magic_destroy, 0, channel_magic_dup };

SV* S_channel_to_sv(pTHX_ Channel* channel, SV* stash_name) {
	SV* referent = newSV(0);
	MAGIC* magic = sv_magicext(referent, NULL, PERL_MAGIC_ext, &channel_magic, (char*)channel, 0);
	magic->mg_flags |= MGf_DUP;
	return sv_bless(newRV_noinc(referent), gv_stashsv(stash_name, 0));
}

Channel* S_sv_to_channel(pTHX_ SV* sv) {
	if (!sv_derived_from(sv, "threads::csp::channel"))
		Perl_croak(aTHX_ "Object is not a threads::csp::channel");
	MAGIC* magic = SvMAGICAL(SvRV(sv)) ? mg_findext(SvRV(sv), PERL_MAGIC_ext, &channel_magic) : NULL;
	if (magic)
		return (Channel*)magic->mg_ptr;
	else
		Perl_croak(aTHX_ "threads::csp::channel object is lacking magic");
}