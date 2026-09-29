import type { HistoryContractCatalog, LiveMarket } from "../../src/bridge/client";

// Display labels verified against the supplied reference market screenshot.
// These never change instrument identifiers or synthesize continuous contracts.
const productLabels: Record<string, string> = {
  "SHFE.br": "合成橡胶",
  "INE.nr": "20号胶",
  "SHFE.ru": "橡胶",
  "SHFE.au": "沪金",
  "SHFE.ag": "沪银",
  "SHFE.sn": "沪锡",
  "SHFE.rb": "螺纹钢",
  "SHFE.wr": "线材",
  "SHFE.ao": "氧化铝",
  "SHFE.hc": "热卷",
  "SHFE.cu": "沪铜",
  "SHFE.pb": "沪铅",
  "INE.bc": "国际铜",
  "SHFE.sp": "纸浆",
  "SHFE.op": "双胶纸",
  "SHFE.ad": "铝合金",
  "SHFE.zn": "沪锌",
  "SHFE.al": "沪铝",
  "SHFE.ss": "不锈钢",
  "SHFE.fu": "燃油",
  "SHFE.bu": "沥青",
  "SHFE.ni": "沪镍",
  "INE.sc": "原油",
  "INE.lu": "低硫燃油",
  "INE.ec": "欧线集运",
  "CFFEX.IC": "中证500",
  "CFFEX.IM": "中证1000",
  "CFFEX.TL": "三十年国债",
  "CFFEX.T": "十年国债",
  "CFFEX.IF": "沪深300",
  "CFFEX.TF": "五年国债",
  "CFFEX.TS": "二年国债",
  "CFFEX.IH": "上证50",
  "CZCE.OI": "菜油",
  "CZCE.RM": "菜粕",
  "CZCE.TA": "PTA",
  "CZCE.PL": "丙烯",
  "CZCE.PF": "短纤",
  "CZCE.PR": "瓶片",
  "CZCE.FG": "玻璃",
  "CZCE.PX": "对二甲苯",
  "CZCE.SR": "白糖",
  "CZCE.UR": "尿素",
  "CZCE.SH": "烧碱",
  "CZCE.CF": "棉花",
  "CZCE.SM": "锰硅",
  "CZCE.MA": "甲醇",
  "CZCE.AP": "苹果",
  "CZCE.CY": "棉纱",
  "CZCE.CJ": "红枣",
  "CZCE.PK": "花生",
  "CZCE.SF": "硅铁",
  "CZCE.SA": "纯碱",
  "CZCE.RS": "菜籽",
  "CZCE.JR": "粳稻",
  "CZCE.PM": "普麦",
  "CZCE.RI": "早籼稻",
  "CZCE.WH": "强麦",
  "CZCE.ZC": "动力煤",
  "DCE.a": "豆一",
  "DCE.b": "豆二",
  "DCE.lh": "生猪",
  "DCE.v": "PVC",
  "DCE.y": "豆油",
  "DCE.bb": "胶合板",
  "DCE.i": "铁矿石",
  "DCE.m": "豆粕",
  "DCE.j": "焦炭",
  "DCE.rr": "粳米",
  "DCE.fb": "纤维板",
  "DCE.p": "棕榈油",
  "DCE.cs": "玉米淀粉",
  "DCE.l": "塑料",
  "DCE.lg": "原木",
  "DCE.c": "玉米",
  "DCE.eb": "苯乙烯",
  "DCE.bz": "纯苯",
  "DCE.eg": "乙二醇",
  "DCE.pp": "聚丙烯",
  "DCE.jm": "焦煤",
  "DCE.pg": "LPG",
  "DCE.jd": "鸡蛋",
  "GFEX.si": "工业硅",
  "GFEX.ps": "多晶硅",
  "GFEX.lc": "碳酸锂",
  "GFEX.pt": "铂",
  "GFEX.pd": "钯",
};

// Product display label for a catalog contract, e.g. "合成橡胶" for SHFE.br.
export function productLabel(row: { venue: string; symbol: string }, live?: LiveMarket["catalog"]) {
  const contract = live?.contracts.find(
    item => item.venue === row.venue && item.symbol === row.symbol,
  );
  return contract
    ? (productLabels[`${row.venue}.${contract.product}`] ?? contract.product)
    : undefined;
}

// Prefer provider names; known display labels supplement code-only live names.
// Quote identity remains venue + symbol; names never become request identifiers.
export function contractName(
  row: { venue: string; symbol: string },
  catalog: HistoryContractCatalog | undefined,
  live?: LiveMarket["catalog"],
) {
  const contract = live?.contracts.find(
    item => item.venue === row.venue && item.symbol === row.symbol,
  );
  const liveName = contract?.name.trim();
  if (liveName && liveName !== row.symbol) return liveName;
  const label = contract && productLabels[`${row.venue}.${contract.product}`];
  if (label && liveName === row.symbol && row.symbol.startsWith(contract.product))
    return `${label}${row.symbol.slice(contract.product.length)}`;
  if (catalog?.exchange.toUpperCase() !== row.venue.toUpperCase()) return undefined;
  const name = catalog.items
    .find(item => item.code.split(".")[0].toUpperCase() === row.symbol.toUpperCase())
    ?.name.trim();
  return name || undefined;
}
