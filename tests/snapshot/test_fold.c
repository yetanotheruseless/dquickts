/* Folds relative to an anchor layer: a base (engine), a chained checkpoint
   that becomes the anchor (harness), checkpoints, a fold, more checkpoints;
   restored with the anchor, the runtime folds again; every restore matches
   the reference, and a fold is far smaller than a full base. Exit 1 on any
   mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static JSValue js_print(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv){
    for(int i=0;i<argc;i++){ const char *s=JS_ToCString(ctx,argv[i]); printf("%s%s",i?" ":"",s?s:"?"); JS_FreeCString(ctx,s);} printf("\n"); return JS_UNDEFINED; }
static const void * const natives[] = { (const void*)js_print };
static const JSSnapshotHost host = { .natives = natives, .native_count = 1 };
static int status;
static void die(JSContext *ctx, const char *what){ JSValue e=JS_GetException(ctx); const char*s=JS_ToCString(ctx,e); fprintf(stderr,"FATAL %s: %s\n",what,s?s:"?"); exit(1); }
static void ev(JSContext *ctx, const char *src){ JSValue v=JS_Eval(ctx,src,strlen(src),"<step>",JS_EVAL_TYPE_GLOBAL); if(JS_IsException(v)) die(ctx,src); JS_FreeValue(ctx,v); }
static char *checksum(JSContext *ctx){
    const char *src="(()=>{let h=0; for(const k of Object.keys(state).sort()) { const v=JSON.stringify(state[k]); for(const ch of v) h=(h*31+ch.charCodeAt(0))>>>0; } return JSON.stringify({keys:Object.keys(state).length,h,big:typeof harness.big==='object'?harness.big.length:-1,fn:harness.fn(3)})})()";
    JSValue v=JS_Eval(ctx,src,strlen(src),"<sum>",JS_EVAL_TYPE_GLOBAL); if(JS_IsException(v)) die(ctx,"checksum");
    const char *s=JS_ToCString(ctx,v); char *out=strdup(s); JS_FreeCString(ctx,s); JS_FreeValue(ctx,v); return out; }
static uint8_t *snap(JSContext *ctx, size_t *len, int flags){
    uint8_t *b=JS_WriteSnapshot(ctx,len,&host,NULL,0,flags); if(!b) die(ctx,"snapshot");
    uint8_t *c=malloc(*len); memcpy(c,b,*len); js_free_rt(JS_GetRuntime(ctx),b); return c; }
static JSContext *restore(const uint8_t *base, size_t blen, uint8_t **chain, size_t *lens, int n, int anchor){
    JSRuntime *rt=JS_NewRuntime(); JSSnapshotResult res; char err[256];
    if(JS_ReadSnapshotChainAnchored(rt,base,blen,(const uint8_t * const*)chain,lens,n,anchor,&host,&res,err,sizeof err)){ fprintf(stderr,"restore failed: %s\n",err); exit(1); }
    JSContext *ctx=res.contexts[0]; JS_FreeSnapshotResult(rt,&res); return ctx; }
#define CHECK(cond, msg) do { if(!(cond)){ printf("FAIL: %s\n", msg); status=1; } else printf("ok: %s\n", msg); } while(0)

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    JSRuntime *rt=JS_NewRuntime(); JSContext *ctx=JS_NewContext(rt);
    JSValue g=JS_GetGlobalObject(ctx); JS_SetPropertyStr(ctx,g,"print",JS_NewCFunction(ctx,js_print,"print",1)); JS_FreeValue(ctx,g);
    size_t blen, hlen; 
    uint8_t *engine=snap(ctx,&blen,JS_SNAPSHOT_BASE);
    /* the harness layer: a big shared structure and a function */
    ev(ctx,"globalThis.harness={big:Array.from({length:20000},(_,i)=>({i,s:'h'+i})),fn:(x)=>x*2}; globalThis.state={}; globalThis.tmp=[];");
    uint8_t *harness=snap(ctx,&hlen,JS_SNAPSHOT_CHAIN);
    if(JS_SetSnapshotAnchor(ctx)) die(ctx,"anchor");
    uint64_t anchor_id=JS_GetSnapshotAnchorId(rt);
    CHECK(anchor_id==JS_GetSnapshotBaseId(rt),"anchor id is the harness layer id");

    uint8_t *chain[64]; size_t lens[64]; int n=0;
    chain[n]=harness; lens[n]=hlen; n++;
    /* 5 steps of agent work, some churn */
    for(int s=0;s<5;s++){ char src[256]; snprintf(src,sizeof src,"state.k%d=Array.from({length:300},(_,i)=>'v%d_'+i); tmp=Array.from({length:500},(_,i)=>({i})); delete state.k%d;",s,s,s-1); ev(ctx,src); chain[n]=snap(ctx,&lens[n],JS_SNAPSHOT_CHAIN); n++; }
    /* the fold: relative to the anchor */
    int fold_at=n; size_t flen; uint8_t *fold=snap(ctx,&flen,JS_SNAPSHOT_FOLD);
    size_t full; uint8_t *fullbase=snap(ctx,&full,0); free(fullbase);
    printf("harness layer %zu B, fold %zu B, full snapshot %zu B\n",hlen,flen,full);
    CHECK(flen*4<full,"the fold is at least 4x smaller than a full snapshot");
    CHECK(JS_GetSnapshotAnchorId(rt)==anchor_id,"the anchor stays after a fold");
    /* the chain continues after the fold */
    uint8_t *chain2[64]; size_t lens2[64]; int n2=0;
    chain2[n2]=harness; lens2[n2]=hlen; n2++;
    chain2[n2]=fold; lens2[n2]=flen; n2++;
    for(int s=5;s<8;s++){ char src[256]; snprintf(src,sizeof src,"state.k%d=Array.from({length:300},(_,i)=>'v%d_'+i); delete state.k%d;",s,s,s-1); ev(ctx,src); chain2[n2]=snap(ctx,&lens2[n2],JS_SNAPSHOT_CHAIN); n2++; }
    char *ref=checksum(ctx);

    /* restore from engine + harness + fold + after, anchoring at the harness */
    JSContext *b=restore(engine,blen,chain2,lens2,n2,0);
    char *sb=checksum(b);
    CHECK(!strcmp(ref,sb),"restored through the fold matches the reference");
    CHECK(JS_GetSnapshotAnchorId(JS_GetRuntime(b))==anchor_id,"the reader rebuilt the anchor");
    /* the restored runtime folds again, against the rebuilt anchor */
    ev(b,"state.k8=Array.from({length:300},(_,i)=>'v8_'+i); delete state.k7; tmp=null;");
    size_t f2len; uint8_t *fold2=snap(b,&f2len,JS_SNAPSHOT_FOLD);
    char *sb2=checksum(b);
    uint8_t *chain3[2]={harness,fold2}; size_t lens3[2]={hlen,f2len};
    JSContext *c=restore(engine,blen,chain3,lens3,2,0);
    char *sc=checksum(c);
    CHECK(!strcmp(sb2,sc),"a fold written by a restored runtime restores on its own");
    printf("fold 2: %zu B\n",f2len);
    /* the old chain (without the fold) still restores too */
    JSContext *d=restore(engine,blen,chain,lens,fold_at,-1);
    char *sd=checksum(d);
    ev(ctx,"1"); /* no-op */
    CHECK(strstr(sd,"\"keys\":1")!=NULL,"the pre-fold chain restores the pre-fold state");
    /* a fold against a stale anchor is rejected by the reader: fold2 after the pre-fold chain */
    { uint8_t *bad[8]; size_t bl[8]; int m=0; for(int i=0;i<fold_at;i++){bad[m]=chain[i]; bl[m]=lens[i]; m++;} bad[m]=fold2; bl[m]=f2len; m++;
      JSRuntime *r2=JS_NewRuntime(); JSSnapshotResult res; char err[256];
      int rc=JS_ReadSnapshotChainAnchored(r2,engine,blen,(const uint8_t * const*)bad,bl,m,0,&host,&res,err,sizeof err);
      CHECK(rc!=0,"a fold does not apply after checkpoints that followed the anchor");
      if(!rc){ for(int i=0;i<res.context_count;i++) JS_FreeContext(res.contexts[i]); JS_FreeSnapshotResult(r2,&res);} JS_FreeRuntime(r2); }
    printf("%s\n", status ? "FAILED" : "all ok");
    return status;
}
