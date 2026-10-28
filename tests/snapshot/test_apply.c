/* Live apply: checkpoints written by runtime A are applied to runtime B,
   which only restored the layer. After every apply B's state digest must
   equal A's; B then writes checkpoints and a fold of its own that restore
   on their own. Every runtime must free cleanly (JS_FreeRuntime asserts
   on leaks). Exit 1 on any mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static JSValue js_print(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv){
    for(int i=0;i<argc;i++){ const char *s=JS_ToCString(ctx,argv[i]); printf("%s%s",i?" ":"",s?s:"?"); JS_FreeCString(ctx,s);} printf("\n"); return JS_UNDEFINED; }
static const void * const natives[] = { (const void*)js_print };
static const JSSnapshotHost host = { .natives = natives, .native_count = 1 };
static int status;
#define CHECK(cond, ...) do { if(!(cond)){ printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); status=1; } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while(0)
static void die(JSContext *ctx, const char *what){ JSValue e=JS_GetException(ctx); const char*s=JS_ToCString(ctx,e); fprintf(stderr,"FATAL %s: %s\n",what,s?s:"?"); exit(1); }
static void run_jobs(JSRuntime *rt){ JSContext *c; int r; while((r=JS_ExecutePendingJob(rt,&c))!=0){ if(r<0) die(c,"job"); } }
static void ev(JSContext *ctx, const char *src){ JSValue v=JS_Eval(ctx,src,strlen(src),"<step>",JS_EVAL_TYPE_GLOBAL); if(JS_IsException(v)) die(ctx,src); JS_FreeValue(ctx,v); run_jobs(JS_GetRuntime(ctx)); }
static char *digest(JSContext *ctx){
    const char *src="JSON.stringify({count:S.count,items:S.items,map:S.map?[...S.map.entries()]:null,set:[...S.set],ta:[...S.ta],str:S.str.length+S.str.slice(-6),cls:(S.items&&S.items[0]&&S.items[0].y)??null,closures:S.closures.map(c=>c.get()),log,results,frozen:S.obj?Object.isFrozen(S.obj):null,getter:S.getter?S.getter.g:null,big2:S.big2?S.big2.length:null,d:S.d.getTime(),re:S.re.exec('xabcabc')?.index,err:S.err.message,cyc:S.cyc?S.cyc.self===S.cyc:null,bigsum:H.big.reduce((a,o)=>a+o.i,0),sym:S.sym?S.sym.toString():null,gen:S.gen?S.gen.next().value:null})";
    JSValue v=JS_Eval(ctx,src,strlen(src),"<sum>",JS_EVAL_TYPE_GLOBAL); if(JS_IsException(v)) die(ctx,"digest");
    const char *s=JS_ToCString(ctx,v); char *out=strdup(s); JS_FreeCString(ctx,s); JS_FreeValue(ctx,v); return out; }
static uint8_t *snap(JSContext *ctx, size_t *len, int flags){
    uint8_t *b=JS_WriteSnapshot(ctx,len,&host,NULL,0,flags); if(!b) die(ctx,"snapshot");
    uint8_t *c=malloc(*len); memcpy(c,b,*len); js_free_rt(JS_GetRuntime(ctx),b); return c; }
static JSContext *restore(const uint8_t *base, size_t blen, uint8_t **chain, size_t *lens, int n, int anchor){
    JSRuntime *rt=JS_NewRuntime(); JSSnapshotResult res; char err[256];
    if(JS_ReadSnapshotChainAnchored(rt,base,blen,(const uint8_t * const*)chain,lens,n,anchor,&host,&res,err,sizeof err)){ fprintf(stderr,"restore failed: %s\n",err); exit(1); }
    JSContext *ctx=res.contexts[0]; JS_FreeSnapshotResult(rt,&res); return ctx; }
static void freert(JSContext *ctx){ JSRuntime *rt=JS_GetRuntime(ctx); JS_FreeContext(ctx); JS_FreeRuntime(rt); }
static int apply(JSContext *ctx, uint8_t **chain, size_t *lens, int n, char *err, size_t errsz){
    JSRuntime *rt=JS_GetRuntime(ctx); JSSnapshotResult res;
    int rc=JS_ApplySnapshotChain(rt,(const uint8_t * const*)chain,lens,n,&host,&res,err,errsz);
    if(rc) return rc;
    for(int i=0;i<res.context_count;i++) JS_FreeContext(res.contexts[i]);
    for(int i=0;i<res.root_count;i++) JS_FreeValue(ctx,res.roots[i]);
    JS_FreeSnapshotResult(rt,&res); return 0; }

static const char *LAYER =
 "globalThis.H={big:Array.from({length:5000},(_,i)=>({i,s:'x'+i}))};"
 "globalThis.S={count:0,items:[],map:new Map(),set:new Set(),ta:new Uint8Array(16),re:/a(b)c/g,d:new Date(0),err:new Error('e'),cls:class A{constructor(x){this.x=x} get y(){return this.x*2}},closures:[],str:'',sym:null,gen:null};"
 "globalThis.pending={}; globalThis.call=(n)=>new Promise(r=>pending[n]=r); globalThis.log=[]; globalThis.results={};"
 "globalThis.task=async function(n){ log.push('start'+n); const a=await call(n); log.push('got'+a.v); const b=await call(n+'b'); log.push('got'+b.v); return a.v+b.v; };"
 "function* g(){ let i=0; for(;;) yield i++; }; globalThis.mkgen=g;";
static const char *STEPS[] = {
 "S.count++; S.items.push({a:1}); S.map.set('k1',{v:1}); S.set.add(5); S.ta[3]=9; S.str+='abc'.repeat(50); for(let i=0;i<3;i++){ let c=i; S.closures.push({get:()=>c,inc:()=>c++}); } task(1).then(v=>results.t1=v); S.gen=mkgen(); S.gen.next();",
 "S.closures[0].inc(); S.items[0].a=2; S.map.delete('k1'); S.map.set('k2',[1,2]); S.items.push(new S.cls(7)); pending[1]({v:10}); S.sym=Symbol('s'); S.gen.next();",
 "S.items.shift(); S.closures.pop(); S.count+=10; S.str=S.str.slice(0,20)+'zz'; task(2).then(v=>results.t2=v); pending['1b']({v:5}); S.re.lastIndex=0;",
 "S.items=null; S.big2=H.big.slice(0,100); S.cyc={}; S.cyc.self=S.cyc; pending[2]({v:1}); H.big[10].s='changed';",
 "S.cyc=null; S.obj=Object.freeze({q:1}); S.items=[1,2,3]; S.items.length=1; S.getter={get g(){return 42}}; S.map=null; S.closures[1].inc(); S.closures[1].inc(); S.err=new RangeError('r'); S.big2=null; pending['2b']({v:2});",
};
#define NSTEPS (int)(sizeof(STEPS)/sizeof(STEPS[0]))

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    JSRuntime *ra=JS_NewRuntime(); JSContext *a=JS_NewContext(ra);
    { JSValue g=JS_GetGlobalObject(a); JS_SetPropertyStr(a,g,"print",JS_NewCFunction(a,js_print,"print",1)); JS_FreeValue(a,g); }
    size_t elen, llen; uint8_t *engine=snap(a,&elen,JS_SNAPSHOT_BASE);
    ev(a,LAYER); uint8_t *layer=snap(a,&llen,JS_SNAPSHOT_CHAIN);
    if(JS_SetSnapshotAnchor(a)) die(a,"anchor");
    uint8_t *chain[16]; size_t lens[16]; char *sum[16];
    for(int s=0;s<NSTEPS;s++){ ev(a,STEPS[s]); chain[s]=snap(a,&lens[s],JS_SNAPSHOT_CHAIN); sum[s]=digest(a); }

    /* B: the layer, then every checkpoint applied live, one at a time */
    uint8_t *lay[1]={layer}; size_t ll[1]={llen};
    JSContext *b=restore(engine,elen,lay,ll,1,0);
    char err[512];
    for(int s=0;s<NSTEPS;s++){
        if(apply(b,&chain[s],&lens[s],1,err,sizeof err)){ printf("FAIL: apply #%d: %s\n",s,err); status=1; break; }
        JS_RunGC(JS_GetRuntime(b));
        char *d=digest(b);
        CHECK(!strcmp(d,sum[s]),"after live apply of checkpoint #%d the state matches (%zu B)",s,lens[s]);
        if(strcmp(d,sum[s])) printf("  A: %s\n  B: %s\n",sum[s],d);
        free(d);
    }
    /* B keeps running: the heap must be sound */
    for(int k=0;k<2;k++){ JSContext *x=k?b:a;
      ev(x,"S.count+=100; S.closures.forEach(c=>c.inc()); results.after=S.closures.map(c=>c.get()); pending['x']=1; task(9).then(v=>results.t9=v); pending[9]({v:1}); S.ta[5]=7;");
      ev(x,"pending['9b']({v:2}); S.ta2=new Uint16Array(S.ta.buffer, 2, 4); S.items.push(S.ta2[0]);"); }
    { char *da=digest(a), *db=digest(b); CHECK(!strcmp(da,db),"A and B agree after running more code on both"); free(da); free(db); }
    /* B writes a checkpoint and a fold of its own; both restore */
    size_t cl, fl; uint8_t *cp=snap(b,&cl,JS_SNAPSHOT_CHAIN);
    ev(b,"S.count++;"); uint8_t *fold=snap(b,&fl,JS_SNAPSHOT_FOLD);
    char *db=digest(b);
    { uint8_t *ch[8]={layer}; size_t cls[8]={llen}; int m=1; for(int s=0;s<NSTEPS;s++){ch[m]=chain[s]; cls[m]=lens[s]; m++;} ch[m]=cp; cls[m]=cl; m++;
      JSContext *c=restore(engine,elen,ch,cls,m,0); ev(c,"S.count++;"); char *dc=digest(c); CHECK(!strcmp(db,dc),"B's checkpoint after the live apply continues the chain (%zu B)",cl); free(dc); freert(c); }
    { uint8_t *ch[2]={layer,fold}; size_t cls[2]={llen,fl};
      JSContext *d=restore(engine,elen,ch,cls,2,0); char *dd=digest(d); CHECK(!strcmp(db,dd),"B's fold after the live apply restores on its own (%zu B)",fl); free(dd); freert(d); }
    free(db);
    /* C: all checkpoints applied in one call */
    JSContext *c=restore(engine,elen,lay,ll,1,0);
    if(apply(c,chain,lens,NSTEPS,err,sizeof err)){ printf("FAIL: apply all: %s\n",err); status=1; }
    else { char *dc=digest(c); CHECK(!strcmp(dc,sum[NSTEPS-1]),"all checkpoints applied in one call match"); free(dc); }
    /* a checkpoint that does not continue the base is refused */
    CHECK(apply(c,&chain[0],&lens[0],1,err,sizeof err)!=0,"a stale checkpoint is refused: %s",err);
    { JSRuntime *rc=JS_GetRuntime(c); JS_FreeContext(c); JS_FreeRuntime(rc); } /* inconsistent after the failure: freed, not used */
    freert(b);
    JS_FreeContext(a); JS_FreeRuntime(ra);
    for(int s=0;s<NSTEPS;s++){ free(chain[s]); free(sum[s]); }
    free(engine); free(layer); free(cp); free(fold);
    printf("%s\n", status ? "FAILED" : "all ok");
    return status;
}
