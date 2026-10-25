/* Churn: each step replaces 1000 temporaries that were alive at the previous
   checkpoint. Measures whether dropped objects leave the chain (tombstones),
   then restores from base + all checkpoints and checks the restored heap. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "quickjs.h"
#define STEPS 30
static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3+t.tv_nsec/1e6; }
static const JSSnapshotHost host = { 0 };
static int status;
static void die(JSContext *ctx,const char*w){ JSValue e=JS_GetException(ctx); const char*s=JS_ToCString(ctx,e); fprintf(stderr,"FATAL %s: %s\n",w,s?s:"?"); exit(1); }
static char *ev_str(JSContext *ctx,const char*src){ JSValue v=JS_Eval(ctx,src,strlen(src),"<e>",JS_EVAL_TYPE_GLOBAL); if(JS_IsException(v)) die(ctx,src); const char*s=JS_ToCString(ctx,v); char*o=strdup(s?s:"?"); JS_FreeCString(ctx,s); JS_FreeValue(ctx,v); return o; }
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    JSRuntime *rt=JS_NewRuntime(); JSContext *ctx=JS_NewContext(rt);
    size_t blen; uint8_t *b=JS_WriteSnapshot(ctx,&blen,&host,NULL,0,JS_SNAPSHOT_BASE);
    uint8_t *base=malloc(blen); memcpy(base,b,blen); js_free_rt(rt,b);
    uint8_t *chain[STEPS]; size_t clen[STEPS]; size_t total=0;
    printf("%5s %12s %10s %12s %12s\n","step","checkpoint","write","live objs","heap KB");
    for(int s=0;s<STEPS;s++){
        char src[256]; snprintf(src,sizeof src,"globalThis.keep=(globalThis.keep||0)+1; globalThis.tmp=Array.from({length:1000},(_, i)=>({i, s:%d}));",s);
        free(ev_str(ctx,src));
        JS_RunGC(rt);
        double t=now_ms(); size_t len; b=JS_WriteSnapshot(ctx,&len,&host,NULL,0,JS_SNAPSHOT_CHAIN); double ms=now_ms()-t; if(!b) die(ctx,"ckpt");
        chain[s]=malloc(len); memcpy(chain[s],b,len); clen[s]=len; total+=len; js_free_rt(rt,b);
        JSMemoryUsage u; JS_ComputeMemoryUsage(rt,&u);
        if(s%5==0||s==STEPS-1) printf("%5d %12zu %8.2fms %12lld %12lld\n",s,len,ms,(long long)u.obj_count,(long long)(u.memory_used_size/1024));
    }
    char *expect=ev_str(ctx,"JSON.stringify({keep, n:tmp.length, last:tmp[999]})");
    JS_FreeContext(ctx); JS_FreeRuntime(rt);

    rt=JS_NewRuntime(); JSSnapshotResult res; char err[256]; double t=now_ms();
    if(JS_ReadSnapshotChain(rt,base,blen,(const uint8_t*const*)chain,clen,STEPS,&host,&res,err,sizeof err)){ printf("restore FAILED: %s\n",err); return 1; }
    double ms=now_ms()-t; ctx=res.contexts[0]; JS_FreeSnapshotResult(rt,&res); JS_RunGC(rt);
    JSMemoryUsage u; JS_ComputeMemoryUsage(rt,&u);
    char *got=ev_str(ctx,"JSON.stringify({keep, n:tmp.length, last:tmp[999]})");
    printf("\nrestore base + %d checkpoints (%zu bytes total): %.2f ms; live objs %lld, heap %lld KB\n",STEPS,total,ms,(long long)u.obj_count,(long long)(u.memory_used_size/1024));
    printf("expected %s\nrestored %s\n=> %s\n",expect,got,strcmp(expect,got)?"MISMATCH":"identical"); if(strcmp(expect,got)) status=1;
    /* the restored runtime continues: drop tmp, checkpoint, restore again */
    free(ev_str(ctx,"globalThis.tmp=null; 0")); JS_RunGC(rt);
    size_t len; b=JS_WriteSnapshot(ctx,&len,&host,NULL,0,JS_SNAPSHOT_CHAIN); if(!b) die(ctx,"ckpt2");
    uint8_t *extra=malloc(len); memcpy(extra,b,len); js_free_rt(rt,b);
    printf("checkpoint after dropping tmp in the restored runtime: %zu bytes\n",len);
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    uint8_t *all[STEPS+1]; size_t al[STEPS+1]; memcpy(all,chain,sizeof chain); memcpy(al,clen,sizeof clen); all[STEPS]=extra; al[STEPS]=len;
    rt=JS_NewRuntime();
    if(JS_ReadSnapshotChain(rt,base,blen,(const uint8_t*const*)all,al,STEPS+1,&host,&res,err,sizeof err)){ printf("restore 2 FAILED: %s\n",err); return 1; }
    ctx=res.contexts[0]; JS_FreeSnapshotResult(rt,&res); JS_RunGC(rt); JS_ComputeMemoryUsage(rt,&u);
    char *g2=ev_str(ctx,"JSON.stringify({keep, tmp})");
    printf("restore base + %d checkpoints: %s; live objs %lld, heap %lld KB\n",STEPS+1,g2,(long long)u.obj_count,(long long)(u.memory_used_size/1024));
    if(strcmp(g2,"{\"keep\":30,\"tmp\":null}")) status=1;
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    free(expect); free(got); free(g2); free(extra); free(base); for(int i=0;i<STEPS;i++) free(chain[i]);
    printf(status?"FAILED\n":"OK\n"); return status;
}
