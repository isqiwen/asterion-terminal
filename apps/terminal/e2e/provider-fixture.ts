// Current provider response fixture; all production-required state is explicit.
export function providerFixture<T extends {id:string;name:string;plugin_id?:string}>(provider:T) {
  return {
    lifecycle: {id:provider.id,provider:provider.plugin_id ?? provider.id,name:provider.name,state:"enabled",revision:0},
    verification: {status:"never",message:"尚未验证已保存配置",checked_at:null,revision:null},
    configuration:{schema_version:1,fields:[]},
    ...provider,
  };
}
