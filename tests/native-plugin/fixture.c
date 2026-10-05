/* Synthetic data ONLY for contract tests. Never installed in production plugins/. */
#include "fixture.h"
#include <asterion/plugin/history.h>
#include <asterion/plugin/history_connection.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int running, fail_start, daily; } Instance;
static uint32_t destroyed_count, stopped_count;
static int32_t running(void* p) { return ((Instance*)p)->running; }
static uint32_t destroyed(void) { return destroyed_count; }
static uint32_t stopped(void) { return stopped_count; }
static const TestLifetime lifetime = {sizeof(TestLifetime),1,running,destroyed,stopped};
static AstStatus sources(void* self,void* ctx,AstStatus(*emit)(void*,const AstHistorySource*)) {
  (void)self;
  const char* venues[]={"SHFE"}; const uint32_t mins[]={1,5},days[]={0};
  const AstHistorySource m={"fixture.minutes","Third-party test minutes","fixture.v1","Asia/Shanghai","bar_end",venues,1,mins,2,60,0};
  const AstHistorySource d={"fixture.daily","Third-party test daily","fixture.v1","Asia/Shanghai","trading_day",venues,1,days,1,60,0};
  if(!emit) return AST_INVALID;
  AstStatus result=emit(ctx,&m); return result==AST_OK?emit(ctx,&d):result;
}
static AstStatus catalog(void* self,const char* venue,const char* product,AstCancellation cancel,void* ctx,AstStatus(*emit)(void*,const AstListing*)) {
  if(!self || !venue || !product || !emit) return AST_INVALID;
  if(!((Instance*)self)->running) return AST_UNAVAILABLE;
  if(cancel.requested && cancel.requested(cancel.context)) return AST_CANCELLED;
  if(strcmp(venue,"SHFE") || strcmp(product,"cu")) return AST_UNSUPPORTED;
  const AstListing row={{"SHFE","cu","2024-03"},"Fixture","2024-03-01","2024-03-02","vendor/cu2403",1,0,500000000,0,"tonnes",NULL};
  return emit(ctx,&row);
}
static AstStatus minutes(void* self,const AstHistoryQuery* q,AstCancellation cancel,AstRequestBudget budget,void* ctx,AstStatus(*emit)(void*,const AstMinute*)) {
  if(!self || !q || !emit) return AST_INVALID;
  if(!((Instance*)self)->running) return AST_UNAVAILABLE;
  if(((Instance*)self)->daily) return AST_UNSUPPORTED;
  if(cancel.requested && cancel.requested(cancel.context)) return AST_CANCELLED;
  if(!budget.acquire) return AST_INVALID;
  AstStatus permit=budget.acquire(budget.context); if(permit!=AST_OK) return permit;
  const AstMinute row={q->begin_ns,100000000,200000000,100000000,200000000,300000000,400000000,500000000,NULL};
  return emit(ctx,&row);
}
static AstStatus daily(void* self,const AstHistoryQuery* q,AstCancellation cancel,AstRequestBudget budget,void* ctx,AstStatus(*emit)(void*,const AstDaily*)) {
  if(!self || !q || !q->begin_day || !emit) return AST_INVALID;
  if(!((Instance*)self)->running) return AST_UNAVAILABLE;
  if(!((Instance*)self)->daily) return AST_UNSUPPORTED;
  if(cancel.requested && cancel.requested(cancel.context)) return AST_CANCELLED;
  if(!budget.acquire) return AST_INVALID;
  AstStatus permit=budget.acquire(budget.context); if(permit!=AST_OK) return permit;
  const AstDaily row={q->begin_day,100000000,200000000,100000000,200000000,300000000,400000000,500000000,0,0,1,0,0,200000000};
  return emit(ctx,&row);
}
static const AstHistoryV2 history={sizeof(AstHistoryV2),2,sources,catalog,minutes,daily};
static AstStatus describe_connection(void* self,const char* source,void* ctx,AstStatus(*emit)(void*,const AstHistoryConnectionSchema*)) {
  (void)self; if(!source || !emit) return AST_INVALID;
  const AstHistoryConnectionSchema schema={"Test credential","测试凭据",0,256,1,30,60}; return emit(ctx,&schema);
}
static AstStatus verify_connection(void* self,const char* source,const char* credential,AstCancellation cancel,void* ctx,AstStatus(*emit)(void*,const AstConnectionCheck*)) {
  (void)self;(void)source;if(!credential || !emit) return AST_INVALID;
  if(cancel.requested && cancel.requested(cancel.context)) return AST_CANCELLED;
  uint32_t state=!strcmp(credential,"invalid")?AST_CHECK_INVALID_CREDENTIAL:!strcmp(credential,"limited")?AST_CHECK_RATE_LIMIT:AST_CHECK_VERIFIED;
  const AstConnectionCheck catalog={"catalog",state},history={"history",state==AST_CHECK_VERIFIED?AST_CHECK_VERIFIED:AST_CHECK_NOT_CHECKED};
  AstStatus result=emit(ctx,&catalog);return result==AST_OK?emit(ctx,&history):result;
}
static const AstHistoryConnectionV1 connection={sizeof(AstHistoryConnectionV1),1,describe_connection,verify_connection};
static AstStatus create(const char* capability,const AstSetting* settings,uint32_t count,void** out) {
  if(!out) return AST_INVALID; *out=NULL;
  if(!capability || (strcmp(capability,AST_TEST_LIFETIME) && strcmp(capability,AST_HISTORY_V2) && strcmp(capability,AST_HISTORY_CONNECTION_V1))) return AST_UNSUPPORTED;
  if(count>64 || (count && !settings)) return AST_INVALID;
  Instance* p=calloc(1,sizeof(Instance)); if(!p) return AST_LIMIT;
  for(uint32_t i=0;i<count;++i) {
    if(!settings[i].key || !settings[i].value) {free(p);return AST_INVALID;}
    if(!strcmp(settings[i].key,"fail_create")) {free(p);return AST_PERMISSION;}
    if(!strcmp(settings[i].key,"fail_start")) p->fail_start=1;
    if(!strcmp(settings[i].key,"source")) p->daily=!strcmp(settings[i].value,"fixture.daily");
  }
  *out=p;return AST_OK;
}
static AstStatus start(void* object) {
  if(!object) return AST_INVALID;
  Instance* p=object;p->running=1;return p->fail_start?AST_UNAVAILABLE:AST_OK;
}
static void stop(void* object) { if(object) { ((Instance*)object)->running=0;++stopped_count; } }
static void destroy(void* object) { if(object) {free(object);++destroyed_count;} }
static AstStatus query(void* self,const char* id,uint32_t version,uint32_t size,const void** out) {
  if(!out) return AST_INVALID;*out=NULL;
  if(!self || !id) return AST_UNSUPPORTED;
#ifdef EMPTY_TABLE
  return AST_OK;
#endif
  if(!strcmp(id,AST_TEST_LIFETIME) && version==1 && size==sizeof(TestLifetime)) {*out=&lifetime;return AST_OK;}
  if(!strcmp(id,AST_HISTORY_V2) && version==2 && size==sizeof(AstHistoryV2)) {*out=&history;return AST_OK;}
  if(!strcmp(id,AST_HISTORY_CONNECTION_V1) && version==1 && size==sizeof(AstHistoryConnectionV1)) {*out=&connection;return AST_OK;}
  return AST_UNSUPPORTED;
}
static const AstCapability capabilities[]={{AST_TEST_LIFETIME,1,"tool"},{AST_HISTORY_V2,2,"data"},{AST_HISTORY_CONNECTION_V1,1,"data"}};
#ifndef BAD_ABI
#define BAD_ABI ASTERION_PLUGIN_ABI_VERSION
#endif
#ifndef FIXTURE_VERSION
#define FIXTURE_VERSION "1.0.0"
#endif
static const AstPluginV1 plugin={sizeof(AstPluginV1),BAD_ABI,"test.independent.c",FIXTURE_VERSION,ASTERION_PLUGIN_PLATFORM,capabilities,3,create,start,stop,destroy,query};
#ifdef MISSING_ENTRY
ASTERION_PLUGIN_EXPORT int unrelated_entry(void) {return 1;}
#else
ASTERION_PLUGIN_EXPORT const AstPluginV1* asterion_plugin_entry_v1(uint32_t abi,uint32_t size) {
  return abi==ASTERION_PLUGIN_ABI_VERSION && size==sizeof(AstPluginV1)?&plugin:NULL;
}
#endif
