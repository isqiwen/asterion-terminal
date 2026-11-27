"""Cross-sectional strategies driven by PIT exchange warehouse receipts, inventory and member positions."""
import numpy as np

from asterion_strategy import cross_section_rank

from factors.fundamentals import inventory_zscore, member_net_change, member_net_ratio, receipt_zscore
from factors.term_structure import roll_yield_carry
from strategies.risk_premia import _Ranked


# Gorton, Hayashi & Rouwenhorst (2013), "The Fundamentals of Commodity Futures Returns", Review of Finance 17(1), 35-105: https://doi.org/10.1093/rof/rfs019
# Koijen, Moskowitz, Pedersen & Vrugt (2018), "Carry", JFE 127(2), 197-225: https://doi.org/10.1016/j.jfineco.2017.11.002
# Adaptation: blend cross-sectional ranks of PIT scarcity (-receipt_zscore, falling back to -inventory_zscore) and roll yield carry.
class InventoryCarryRank(_Ranked):
    """Long scarce-inventory high-carry futures, short abundant-inventory low-carry futures."""
    params = {**_Ranked.params, "window": 60, "carry_window": 5, "scarcity_share": 0.5}
    minimum_window = 2
    factor_extra_bar = 0

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if p["carry_window"] < 1 or not 0 <= p["scarcity_share"] <= 1:
            raise ValueError("invalid inventory-carry parameters")
        self.need = max(p["window"], p["carry_window"], p["volatility"] + 1)

    def scores(self, data):
        p = self.params
        share = p["scarcity_share"]
        scarcity, carry = {}, {}
        for s, row in data.items():
            rz = receipt_zscore(row["receipt_volume"], p["window"])[-1]
            sc = -rz if np.isfinite(rz) else -inventory_zscore(row["inventory_total"], p["window"])[-1]
            if np.isfinite(sc):
                scarcity[s] = sc
            cy = roll_yield_carry(row["roll_yield"], p["carry_window"])[-1]
            if np.isfinite(cy):
                carry[s] = cy
        if share == 1.0:
            return cross_section_rank(scarcity)
        if share == 0.0:
            return cross_section_rank(carry)
        common = set(scarcity) & set(carry)
        s_rank = cross_section_rank({s: scarcity[s] for s in common})
        c_rank = cross_section_rank({s: carry[s] for s in common})
        return {s: share * s_rank[s] + (1.0 - share) * c_rank[s] for s in common}


# Kang, Rouwenhorst & Tang (2020), "A Tale of Two Premiums: The Role of Hedgers and Speculators in Commodity Futures Markets", JF 75(1), 377-417: https://doi.org/10.1111/jofi.12845
# Adaptation: cross-sectional blend of PIT top-member net long-short imbalance level and change across China futures exchanges.
class MemberFlowRank(_Ranked):
    """Long futures with net-long rising top-member positioning, short net-short falling positioning."""
    params = {**_Ranked.params, "window": 5, "change_lookback": 10, "level_share": 0.5}
    factor_extra_bar = 0

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if p["change_lookback"] < 1 or not 0 <= p["level_share"] <= 1:
            raise ValueError("invalid member-flow parameters")
        self.need = max(p["window"], p["change_lookback"] + 1, p["volatility"] + 1)

    def scores(self, data):
        p = self.params
        share = p["level_share"]
        level, change = {}, {}
        for s, row in data.items():
            lv = member_net_ratio(row["member_long"], row["member_short"], p["window"])[-1]
            if np.isfinite(lv):
                level[s] = lv
            ch = member_net_change(row["member_long"], row["member_short"], p["change_lookback"])[-1]
            if np.isfinite(ch):
                change[s] = ch
        if share == 1.0:
            return cross_section_rank(level)
        if share == 0.0:
            return cross_section_rank(change)
        common = set(level) & set(change)
        l_rank = cross_section_rank({s: level[s] for s in common})
        c_rank = cross_section_rank({s: change[s] for s in common})
        return {s: share * l_rank[s] + (1.0 - share) * c_rank[s] for s in common}
