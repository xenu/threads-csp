#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"
#define NO_XSLOCKS
#include "XSUB.h"

#include "channel.h"
#include "mthread.h"

#ifdef WIN32
#  include <windows.h>
#  include <win32thread.h>
#else
#  include <pthread.h>
#  include <thread.h>
#endif

static perl_mutex counter_mutex;
static perl_cond counter_condvar;
static UV thread_counter;

static int (*old_hook)(pTHX);

static int S_threadhook(pTHX) {
	MUTEX_LOCK(&counter_mutex);
	while (thread_counter > 1)
		COND_WAIT(&counter_condvar, &counter_mutex);

	MUTEX_UNLOCK(&counter_mutex);
	MUTEX_DESTROY(&counter_mutex);
	COND_DESTROY(&counter_condvar);

	return old_hook(aTHX);
}

void global_init(pTHX) {
	if (thread_counter == 0) {
		MUTEX_INIT(&counter_mutex);
		COND_INIT(&counter_condvar);
		thread_counter = 1;

		old_hook = PL_threadhook;
		PL_threadhook = S_threadhook;
	}
	if (!PL_perl_destruct_level)
		PL_perl_destruct_level = 1;

	HV* channel_stash = gv_stashpvs("threads::csp::channel", GV_ADD);
	SvFLAGS(channel_stash) |= SVphv_CLONEABLE;
}

static void thread_count_inc() {
	MUTEX_LOCK(&counter_mutex);
	thread_counter++;
	MUTEX_UNLOCK(&counter_mutex);
}

static void thread_count_dec() {
	MUTEX_LOCK(&counter_mutex);
	if (--thread_counter == 0);
		COND_SIGNAL(&counter_condvar);
	MUTEX_UNLOCK(&counter_mutex);
}

void boot_DynaLoader(pTHX_ CV* cv);

static void xs_init(pTHX) {
	dXSUB_SYS;
	newXS((char*)"DynaLoader::boot_DynaLoader", boot_DynaLoader, (char*)__FILE__);
}

static void* run_thread(void* arg) {
	static const char* argv[] = { "perl", "-Mthreads::csp", "-e", "0", NULL };
	static const int argc = sizeof argv / sizeof *argv - 1;

	thread_count_inc();

	Channel* channel = (Channel*)arg;

	PerlInterpreter* my_perl = perl_alloc();
	perl_construct(my_perl);
	PERL_SET_CONTEXT(my_perl);
	PL_exit_flags |= PERL_EXIT_DESTRUCT_END;
	perl_parse(my_perl, xs_init, argc, (char**)argv, NULL);

	AV* to_run = (AV*)sv_2mortal(channel_receive(channel));
	channel_refcount_dec(channel);

	dXCPT;
	XCPT_TRY_START {
		SV* module = *av_fetch(to_run, 0, FALSE);
		load_module(PERL_LOADMOD_NOIMPORT, SvREFCNT_inc(module), NULL);
	} XCPT_TRY_END
	XCPT_CATCH {
		LEAVE;
	}
	else {
		dSP;
		PUSHMARK(SP);
		IV len = av_len(to_run) + 1;
		for(int i = 2; i < len; i++) {
			SV** entry = av_fetch(to_run, i, FALSE);
			XPUSHs(*entry);
		}
		PUTBACK;

		SV** call_ptr = av_fetch(to_run, 1, FALSE);
		call_sv(*call_ptr, G_VOID | G_EVAL | G_DISCARD);

		if (SvTRUE(ERRSV))
			warn("Thread got error %s\n", SvPV_nolen(ERRSV));
	}

	perl_destruct(my_perl);
	perl_free(my_perl);

	thread_count_dec();

	return NULL;
}

void thread_spawn(AV* to_run) {
	static const size_t stack_size = 512 * 1024;

	Channel* channel = channel_alloc(2);

#ifdef WIN32
	CreateThread(NULL, (DWORD)stack_size, run_thread, (LPVOID)channel, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);

#else
	pthread_attr_t attr;
	pthread_attr_init(&attr);

#ifdef PTHREAD_ATTR_SETDETACHSTATE
	PTHREAD_ATTR_SETDETACHSTATE(&attr, PTHREAD_CREATE_DETACHED);
#endif

#ifdef _POSIX_THREAD_ATTR_STACKSIZE
	pthread_attr_setstacksize(&attr, stack_size);
#endif

#if defined(HAS_PTHREAD_ATTR_SETSCOPE) && defined(PTHREAD_SCOPE_SYSTEM)
	pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
#endif

	/* Create the thread */
	pthread_t thr;
#ifdef OLD_PTHREADS_API
	pthread_create(&thr, attr, run_thread, (void *)channel);
#else
	pthread_create(&thr, &attr, run_thread, (void *)channel);
#endif

#endif

	/* This blocks on the other thread, so must run last */
	channel_send(channel, (SV*)to_run);
	channel_refcount_dec(channel);
}