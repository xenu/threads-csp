struct promise;
typedef struct promise Promise;

Promise* promise_alloc(UV);
SV* S_promise_get(pTHX_ Promise* promise);
#define promise_get(promise) S_promise_get(aTHX_ promise)
void S_promise_abandon(pTHX_ Promise* promise);
#define promise_abandon(promise) S_promise_abandon(aTHX_ promise)
void promise_set_value(Promise* promise, SV* value);
void promise_set_exception(Promise* promise, SV* value);
void promise_refcount_dec(Promise* promise);

SV* S_promise_to_sv(pTHX_ Promise* promise);
#define promise_to_sv(promise) S_promise_to_sv(aTHX_ promise)
Promise* S_sv_to_promise(pTHX_ SV* sv);
#define sv_to_promise(sv) S_sv_to_promise(aTHX_ sv)
