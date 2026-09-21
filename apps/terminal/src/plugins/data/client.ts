import type { RequestClient } from "../../api/requests";
import type { components } from "../../api/schema";
export type ObservationRecord = components["schemas"]["ObservationRecord"];
export type ObservationPage = components["schemas"]["ObservationPage"];
export type ObservationPreview = components["schemas"]["ObservationPreview"];
export type CoverageReport = components["schemas"]["CoverageReport"];
export type RefillResult = components["schemas"]["RefillResult"];
export type Provider = components["schemas"]["ProviderStatus"] & {
  configuration: components["schemas"]["ConfigurationSpec"] & {
    fields: components["schemas"]["ConfigurationField"][];
  };
};
export type Snapshot = components["schemas"]["Snapshot"];
export type Bar = components["schemas"]["Bar"];

export async function readProviders(api: RequestClient): Promise<Provider[]> {
  const values = await api.request<Provider[]>("/data/providers");
  if (
    !Array.isArray(values) ||
    values.some(
      (p) =>
        p.api_version !== 2 ||
        !p.lifecycle ||
        !p.verification ||
        !p.configuration ||
        !Array.isArray(p.configuration.fields) ||
        !["enabled", "disabled", "archived"].includes(p.lifecycle.state),
    )
  )
    throw new Error("数据源状态不符合当前接口规范，请使用同一构建的终端与后台");
  return values;
}
