#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#include "channel.h"
#include "mthread.h"

#define slurp_arguments(offset) av_make(items - offset, PL_stack_base + ax + offset)

typedef PerlIO* OutputStream;

MODULE = threads::csp              PACKAGE = threads::csp  PREFIX = thread_

BOOT:
	global_init(aTHX);

Promise* thread_spawn(SV* class, SV* module, SV* function, ...)
	C_ARGS:
		slurp_arguments(1)

MODULE = threads::csp              PACKAGE = threads::csp::promise  PREFIX = promise_

SV* promise_get(Promise* promise)

bool promise_is_finished(Promise* promise)

void promise_set_notify(Promise* promise, OutputStream handle, SV* value)

MODULE = threads::csp              PACKAGE = threads::csp::channel  PREFIX = channel_

SV* channel_new(SV* class)

void channel_send(Channel* channel, SV* argument)

SV* channel_receive(Channel* channel)

void channel_set_notify(Channel* channel, OutputStream stream, SV* value)

void channel_close(Channel* channel)
