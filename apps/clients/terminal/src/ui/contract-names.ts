import type { CatalogContract, CatalogState } from "../generated/asterion/market_pb";
import { getLocale } from "../i18n";

type Id = { venue: string; symbol: string };

// Each product's name in Chinese and in English, as the exchanges publish them.
// Names only label quotes; identity is always venue + symbol.
const products: Record<string, [string, string]> = {
  "SHFE.br": ["合成橡胶", "Butadiene Rubber"],
  "INE.nr": ["20号胶", "TSR 20"],
  "SHFE.ru": ["橡胶", "Natural Rubber"],
  "SHFE.au": ["沪金", "Gold"],
  "SHFE.ag": ["沪银", "Silver"],
  "SHFE.sn": ["沪锡", "Tin"],
  "SHFE.rb": ["螺纹钢", "Rebar"],
  "SHFE.wr": ["线材", "Wire Rod"],
  "SHFE.ao": ["氧化铝", "Alumina"],
  "SHFE.hc": ["热卷", "Hot-rolled Coil"],
  "SHFE.cu": ["沪铜", "Copper"],
  "SHFE.pb": ["沪铅", "Lead"],
  "INE.bc": ["国际铜", "Copper (BC)"],
  "SHFE.sp": ["纸浆", "Pulp"],
  "SHFE.op": ["双胶纸", "Offset Paper"],
  "SHFE.ad": ["铝合金", "Cast Aluminum Alloy"],
  "SHFE.zn": ["沪锌", "Zinc"],
  "SHFE.al": ["沪铝", "Aluminum"],
  "SHFE.ss": ["不锈钢", "Stainless Steel"],
  "SHFE.fu": ["燃油", "Fuel Oil"],
  "SHFE.bu": ["沥青", "Bitumen"],
  "SHFE.ni": ["沪镍", "Nickel"],
  "INE.sc": ["原油", "Crude Oil"],
  "INE.lu": ["低硫燃油", "Low Sulfur Fuel Oil"],
  "INE.ec": ["欧线集运", "Container Freight (Europe)"],
  "CFFEX.IC": ["中证500", "CSI 500"],
  "CFFEX.IM": ["中证1000", "CSI 1000"],
  "CFFEX.TL": ["三十年国债", "30-Year Treasury"],
  "CFFEX.T": ["十年国债", "10-Year Treasury"],
  "CFFEX.IF": ["沪深300", "CSI 300"],
  "CFFEX.TF": ["五年国债", "5-Year Treasury"],
  "CFFEX.TS": ["二年国债", "2-Year Treasury"],
  "CFFEX.IH": ["上证50", "SSE 50"],
  "CZCE.OI": ["菜油", "Rapeseed Oil"],
  "CZCE.RM": ["菜粕", "Rapeseed Meal"],
  "CZCE.TA": ["PTA", "PTA"],
  "CZCE.PL": ["丙烯", "Propylene"],
  "CZCE.PF": ["短纤", "Polyester Staple Fiber"],
  "CZCE.PR": ["瓶片", "PET Bottle Chips"],
  "CZCE.FG": ["玻璃", "Glass"],
  "CZCE.PX": ["对二甲苯", "Paraxylene"],
  "CZCE.SR": ["白糖", "White Sugar"],
  "CZCE.UR": ["尿素", "Urea"],
  "CZCE.SH": ["烧碱", "Caustic Soda"],
  "CZCE.CF": ["棉花", "Cotton"],
  "CZCE.SM": ["锰硅", "Silicon Manganese"],
  "CZCE.MA": ["甲醇", "Methanol"],
  "CZCE.AP": ["苹果", "Apple"],
  "CZCE.CY": ["棉纱", "Cotton Yarn"],
  "CZCE.CJ": ["红枣", "Red Jujube"],
  "CZCE.PK": ["花生", "Peanut"],
  "CZCE.SF": ["硅铁", "Ferrosilicon"],
  "CZCE.SA": ["纯碱", "Soda Ash"],
  "CZCE.RS": ["菜籽", "Rapeseed"],
  "CZCE.JR": ["粳稻", "Japonica Rice"],
  "CZCE.PM": ["普麦", "Common Wheat"],
  "CZCE.RI": ["早籼稻", "Early Indica Rice"],
  "CZCE.WH": ["强麦", "Strong Gluten Wheat"],
  "CZCE.ZC": ["动力煤", "Thermal Coal"],
  "DCE.a": ["豆一", "No.1 Soybean"],
  "DCE.b": ["豆二", "No.2 Soybean"],
  "DCE.lh": ["生猪", "Live Hog"],
  "DCE.v": ["PVC", "PVC"],
  "DCE.y": ["豆油", "Soybean Oil"],
  "DCE.bb": ["胶合板", "Plywood"],
  "DCE.i": ["铁矿石", "Iron Ore"],
  "DCE.m": ["豆粕", "Soybean Meal"],
  "DCE.j": ["焦炭", "Coke"],
  "DCE.rr": ["粳米", "Round-grain Rice"],
  "DCE.fb": ["纤维板", "Fiberboard"],
  "DCE.p": ["棕榈油", "Palm Oil"],
  "DCE.cs": ["玉米淀粉", "Corn Starch"],
  "DCE.l": ["塑料", "LLDPE"],
  "DCE.lg": ["原木", "Log"],
  "DCE.c": ["玉米", "Corn"],
  "DCE.eb": ["苯乙烯", "Styrene"],
  "DCE.bz": ["纯苯", "Pure Benzene"],
  "DCE.eg": ["乙二醇", "Ethylene Glycol"],
  "DCE.pp": ["聚丙烯", "Polypropylene"],
  "DCE.jm": ["焦煤", "Coking Coal"],
  "DCE.pg": ["LPG", "LPG"],
  "DCE.jd": ["鸡蛋", "Egg"],
  "GFEX.si": ["工业硅", "Industrial Silicon"],
  "GFEX.ps": ["多晶硅", "Polysilicon"],
  "GFEX.lc": ["碳酸锂", "Lithium Carbonate"],
  "GFEX.pt": ["铂", "Platinum"],
  "GFEX.pd": ["钯", "Palladium"],
};

const english = () => getLocale() === "en-US";
const productName = (venue: string, product: string) =>
  products[`${venue}.${product}`]?.[english() ? 1 : 0];
const listed = (row: Id, catalog?: CatalogState) =>
  catalog?.contracts.find(
    item => item.instrument?.venue === row.venue && item.instrument.symbol === row.symbol,
  );

// A product's name for a catalog contract, e.g. 螺纹钢 or Rebar for SHFE.rb.
export function productLabel(row: Id, catalog?: CatalogState) {
  const contract = listed(row, catalog);
  return contract ? (productName(row.venue, contract.product) ?? contract.product) : undefined;
}

// A dated contract's name, e.g. 螺纹钢2610 or Rebar 2610. In Chinese the
// provider's own name comes first; providers name contracts only in Chinese.
export function catalogName(contract: CatalogContract) {
  const { venue, symbol } = contract.instrument!;
  const provided = contract.name.trim();
  if (!english() && provided && provided !== symbol) return provided;
  const label = productName(venue, contract.product);
  return label && symbol.startsWith(contract.product)
    ? `${label}${english() ? " " : ""}${symbol.slice(contract.product.length)}`
    : undefined;
}

export function contractName(row: Id, catalog?: CatalogState) {
  const contract = listed(row, catalog);
  return contract && catalogName(contract);
}

// The name of a contract no live catalog lists, e.g. 沪铜2310 or Copper 2310
// for SHFE/cu/2023-10. `provided` is the stored Chinese name, used in Chinese.
export function identityName(contract: string, provided = "") {
  if (provided && !english()) return provided;
  const [venue, product, month] = contract.split("/");
  const label = productName(venue, product);
  return label && month
    ? `${label}${english() ? " " : ""}${month.slice(2, 4)}${month.slice(5, 7)}`
    : undefined;
}
