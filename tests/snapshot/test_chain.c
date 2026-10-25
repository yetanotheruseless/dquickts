/* Chained checkpoints vs base-relative deltas, in lockstep on two runtimes.
   A: JS_SNAPSHOT_DELTA (today).  B: JS_SNAPSHOT_CHAIN (prototype). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "quickjs.h"

#define STEPS 20
#define HEAP 10000

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3+t.tv_nsec/1e6; }
static JSValue js_print(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv){
    for(int i=0;i<argc;i++){ const char *s=JS_ToCString(ctx,argv[i]); printf("%s%s",i?" ":"",s?s:"?"); JS_FreeCString(ctx,s);} printf("\n"); return JS_UNDEFINED; }
static JSValue pending[2] = { JS_UNDEFINED, JS_UNDEFINED }; /* per runtime slot, set by host before eval */
static int cur_slot;
static int status; /* exit status: 1 on any mismatch or accepted bad chain */
static JSValue js_call(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv){
    JSValue fns[2]; JSValue p = JS_NewPromiseCapability(ctx, fns);
    pending[cur_slot] = fns[0]; JS_FreeValue(ctx, fns[1]); return p; }
static const void * const natives[] = { (const void*)js_print, (const void*)js_call };
static const JSSnapshotHost host = { .natives = natives, .native_count = 2 };

static void die(JSContext *ctx, const char *what){ JSValue e=JS_GetException(ctx); const char*s=JS_ToCString(ctx,e); fprintf(stderr,"FATAL %s: %s\n",what,s?s:"?"); exit(1); }
static JSRuntime *mk(JSContext **pctx){
    JSRuntime *rt=JS_NewRuntime(); JSContext *ctx=JS_NewContext(rt); JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"print",JS_NewCFunction(ctx,js_print,"print",1));
    JS_SetPropertyStr(ctx,g,"__call",JS_NewCFunction(ctx,js_call,"__call",2));
    JS_FreeValue(ctx,g); *pctx=ctx; return rt; }
static void ev(JSContext *ctx, const char *src){ JSValue v=JS_Eval(ctx,src,strlen(src),"<step>",JS_EVAL_TYPE_GLOBAL); if(JS_IsException(v)) die(ctx,src); JS_FreeValue(ctx,v); }
static void run_jobs(JSRuntime *rt){ JSContext *c; int r; while((r=JS_ExecutePendingJob(rt,&c))!=0){ if(r<0) die(c,"job"); } }
static char *checksum(JSContext *ctx){
    const char *src="(()=>{let h=0; for(const it of items) for(const ch of it.name) h=(h*31+ch.charCodeAt(0))>>>0; return JSON.stringify({n:items.length,counter,log:log.length,h,last:items.at(-1).id,r:globalThis.result??null})})()";
    JSValue v=JS_Eval(ctx,src,strlen(src),"<sum>",JS_EVAL_TYPE_GLOBAL); if(JS_IsException(v)) die(ctx,"checksum");
    const char *s=JS_ToCString(ctx,v); char *out=strdup(s); JS_FreeCString(ctx,s); JS_FreeValue(ctx,v); return out; }
/* write a snapshot and copy it to plain malloc memory (the runtime may be freed later) */
static uint8_t *snap(JSContext *ctx, size_t *len, JSValueConst *roots, int n, int flags, double *ms){
    double t=now_ms(); uint8_t *b=JS_WriteSnapshot(ctx,len,&host,roots,n,flags); *ms=now_ms()-t; if(!b) die(ctx,"snapshot");
    uint8_t *c=malloc(*len); memcpy(c,b,*len); js_free_rt(JS_GetRuntime(ctx),b); return c; }
static int restore_chain(JSRuntime **prt, JSContext **pctx, const uint8_t *base, size_t blen, uint8_t **chain, size_t *lens, int n, JSValue *root, double *ms, char *err, size_t errsz){
    JSRuntime *rt=JS_NewRuntime(); JSSnapshotResult res; double t=now_ms();
    int rc=JS_ReadSnapshotChain(rt,base,blen,(const uint8_t * const*)chain,lens,n,&host,&res,err,errsz);
    *ms=now_ms()-t;
    if(rc){ JS_FreeRuntime(rt); return -1; }
    *prt=rt; *pctx=res.contexts[0]; if(root) *root = res.root_count ? res.roots[0] : JS_UNDEFINED; JS_FreeSnapshotResult(rt,&res); return 0; }

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    JSContext *ca,*cb; JSRuntime *ra=mk(&ca), *rb=mk(&cb);
    char setup[256]; snprintf(setup,sizeof setup,"globalThis.items=[]; globalThis.n=0; globalThis.counter=0; globalThis.log=[]; for(let i=0;i<%d;i++) items.push({id:n++,name:'item'+i,tags:['a','b']});",HEAP);
    ev(ca,setup); ev(cb,setup);
    size_t balen,bblen; double ms;
    uint8_t *basea=snap(ca,&balen,NULL,0,JS_SNAPSHOT_BASE,&ms), *baseb=snap(cb,&bblen,NULL,0,JS_SNAPSHOT_BASE,&ms);
    printf("base after setup (%d objects): %zu bytes; A and B bases identical: %s\n",HEAP,bblen,(balen==bblen&&!memcmp(basea,baseb,balen))?"yes":"NO");

    uint8_t *chain[STEPS+2]; size_t clen[STEPS+2]; size_t last_delta=0, chain_total=0; double tA=0,tB=0;
    printf("\n%5s %14s %18s %10s %10s\n","step","delta(today)","checkpoint(chain)","write A","write B");
    for(int s=0;s<STEPS;s++){
        char step[512];
        snprintf(step,sizeof step,"for(let j=0;j<200;j++) items.push({id:n++,name:'x'+n,tags:['a','b']}); items[%d].name='changed%d'; counter++; log.push(%d);",s,s,s);
        ev(ca,step); ev(cb,step);
        JSValue roots[1]; int nroots=0;
        if(s==STEPS-1){ /* last step: a host call is pending across the checkpoint */
            const char *async_src="globalThis.result=null; (async()=>{ const r=await __call('github.issues.list',{}); globalThis.result='got:'+r.n; counter+=100; })()";
            cur_slot=0; ev(ca,async_src); run_jobs(ra);
            cur_slot=1; ev(cb,async_src); run_jobs(rb);
            nroots=1;
        }
        double ma,mb; size_t la;
        roots[0]=pending[0]; uint8_t *da=snap(ca,&la,roots,nroots,JS_SNAPSHOT_DELTA,&ma); free(da);
        roots[0]=pending[1]; chain[s]=snap(cb,&clen[s],roots,nroots,JS_SNAPSHOT_CHAIN,&mb);
        last_delta=la; chain_total+=clen[s]; tA+=ma; tB+=mb;
        printf("%5d %14zu %18zu %8.2fms %8.2fms\n",s,la,clen[s],ma,mb);
    }
    printf("sum of %d checkpoints: %zu bytes; last base-relative delta: %zu bytes; avg write A %.2f ms, B %.2f ms\n",STEPS,chain_total,last_delta,tA/STEPS,tB/STEPS);

    /* complete the pending call in A (reference run, never restored) */
    cur_slot=0; { JSValue arg=JS_NewObject(ca); JS_SetPropertyStr(ca,arg,"n",JS_NewInt32(ca,42)); JSValue r=JS_Call(ca,pending[0],JS_UNDEFINED,1,(JSValueConst*)&arg); JS_FreeValue(ca,r); JS_FreeValue(ca,arg); JS_FreeValue(ca,pending[0]); run_jobs(ra); }
    char *suma=checksum(ca);

    /* B: destroy, restore from base + the whole chain, complete the call */
    JS_FreeValue(cb,pending[1]); JS_FreeContext(cb); JS_FreeRuntime(rb);
    char err[256]; JSValue root;
    if(restore_chain(&rb,&cb,baseb,bblen,chain,clen,STEPS,&root,&ms,err,sizeof err)){ printf("restore chain FAILED: %s\n",err); return 1; }
    printf("\nrestore base + %d checkpoints: %.2f ms, base id after restore = %016llx\n",STEPS,ms,(unsigned long long)JS_GetSnapshotBaseId(rb));
    { JSValue arg=JS_NewObject(cb); JS_SetPropertyStr(cb,arg,"n",JS_NewInt32(cb,42)); JSValue r=JS_Call(cb,root,JS_UNDEFINED,1,(JSValueConst*)&arg); JS_FreeValue(cb,r); JS_FreeValue(cb,arg); JS_FreeValue(cb,root); run_jobs(rb); }
    char *sumb=checksum(cb);
    printf("state A (never restored): %s\nstate B (restored):       %s\n=> %s\n",suma,sumb,strcmp(suma,sumb)?"MISMATCH":"identical"); if(strcmp(suma,sumb)) status=1;

    /* B continues the chain after the restore: one more step, one more checkpoint */
    ev(cb,"items[5].name='after-restore'; counter++; log.push('r');");
    chain[STEPS]=snap(cb,&clen[STEPS],NULL,0,JS_SNAPSHOT_CHAIN,&ms);
    printf("\ncheckpoint %d written by the restored runtime: %zu bytes (%.2f ms)\n",STEPS,clen[STEPS],ms);
    char *sumb2=checksum(cb); JS_FreeContext(cb); JS_FreeRuntime(rb);
    JSContext *cc; JSRuntime *rc;
    if(restore_chain(&rc,&cc,baseb,bblen,chain,clen,STEPS+1,NULL,&ms,err,sizeof err)){ printf("restore extended chain FAILED: %s\n",err); return 1; }
    char *sumc=checksum(cc); printf("restore base + %d checkpoints: %.2f ms => %s\n",STEPS+1,ms,strcmp(sumb2,sumc)?"MISMATCH":"identical to the runtime that wrote it"); if(strcmp(sumb2,sumc)) status=1;
    JS_FreeContext(cc); JS_FreeRuntime(rc);

    /* negative tests */
    printf("\nnegative tests:\n");
    { uint8_t *bad[STEPS]; size_t bl[STEPS]; int k=0; for(int i=0;i<STEPS;i++){ if(i==3) continue; bad[k]=chain[i]; bl[k]=clen[i]; k++; }
      if(restore_chain(&rc,&cc,baseb,bblen,bad,bl,k,NULL,&ms,err,sizeof err)==0){ printf("  missing checkpoint #3: ACCEPTED (bad)\n"); status=1; JS_FreeContext(cc); JS_FreeRuntime(rc);} else printf("  missing checkpoint #3: rejected: %s\n",err); }
    { uint8_t *bad[STEPS]; size_t bl[STEPS]; memcpy(bad,chain,sizeof(uint8_t*)*STEPS); memcpy(bl,clen,sizeof(size_t)*STEPS); uint8_t *t=bad[4]; bad[4]=bad[5]; bad[5]=t; size_t tl=bl[4]; bl[4]=bl[5]; bl[5]=tl;
      if(restore_chain(&rc,&cc,baseb,bblen,bad,bl,STEPS,NULL,&ms,err,sizeof err)==0){ printf("  swapped #4/#5: ACCEPTED (bad)\n"); status=1; JS_FreeContext(cc); JS_FreeRuntime(rc);} else printf("  swapped #4/#5: rejected: %s\n",err); }
    { /* a plain delta followed by a checkpoint */
      size_t la; double m; cur_slot=0; uint8_t *da=snap(ca,&la,NULL,0,JS_SNAPSHOT_DELTA,&m); uint8_t *bad[2]={da,chain[0]}; size_t bl[2]={la,clen[0]};
      if(restore_chain(&rc,&cc,basea,balen,bad,bl,2,NULL,&ms,err,sizeof err)==0){ printf("  plain delta then checkpoint: ACCEPTED (bad)\n"); status=1; JS_FreeContext(cc); JS_FreeRuntime(rc);} else printf("  plain delta then checkpoint: rejected: %s\n",err); free(da); }
    { /* backwards compatibility: base + single plain delta from A still restores */
      size_t la; double m; cur_slot=0; uint8_t *da=snap(ca,&la,NULL,0,JS_SNAPSHOT_DELTA,&m); uint8_t *one[1]={da}; size_t ol[1]={la};
      if(restore_chain(&rc,&cc,basea,balen,one,ol,1,NULL,&ms,err,sizeof err)){ printf("  plain base+delta (today's API): FAILED: %s\n",err); return 1; }
      char *sa2=checksum(ca), *sc2=checksum(cc); printf("  plain base+delta (today's API): %s, restored base id = %016llx (base id)\n",strcmp(sa2,sc2)?(status=1,"MISMATCH"):"restores identically",(unsigned long long)JS_GetSnapshotBaseId(rc)); free(sa2); free(sc2); JS_FreeContext(cc); JS_FreeRuntime(rc); free(da); }

    free(suma); free(sumb); free(sumb2); free(sumc);
    for(int i=0;i<=STEPS;i++) free(chain[i]); free(basea); free(baseb);
    JS_FreeContext(ca); JS_FreeRuntime(ra);
    printf(status?"\nFAILED\n":"\nOK\n");
    return status;
}
