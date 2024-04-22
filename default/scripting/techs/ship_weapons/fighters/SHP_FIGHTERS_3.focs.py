from focs._effects import (
    DesignHasPart,
    EffectsGroup,
    OwnedBy,
    PartsInShipDesign,
    SetMaxCapacity,
    SetMaxSecondaryStat,
    Ship,
    Source,
    Target,
    Value,
)
from focs._tech import *
from macros.base_prod import TECH_COST_MULTIPLIER
from macros.misc import FIGHTER_DAMAGE_FACTOR

Tech(
    name="SHP_FIGHTERS_3",
    description="SHP_FIGHTERS_3_DESC",
    short_description="SHIP_WEAPON_IMPROVE_SHORT_DESC",
    category="SHIP_WEAPONS_CATEGORY",
    researchcost=396 * TECH_COST_MULTIPLIER,
    researchturns=9,
    tags=["PEDIA_FIGHTER_TECHS"],
    prerequisites=["SHP_FIGHTERS_2"],
    effectsgroups=[
        EffectsGroup(
            scope=Ship
            & OwnedBy(empire=Source.Owner)
            & (
                DesignHasPart(name="FT_HANGAR_1")
                | DesignHasPart(name="FT_HANGAR_2")
                | DesignHasPart(name="FT_HANGAR_3")
                | DesignHasPart(name="FT_HANGAR_4")
            ),
            accountinglabel="SHP_FIGHTERS_3",
            effects=[
                SetMaxCapacity(
                    partname="FT_HANGAR_1", value=Value + PartsInShipDesign(name="FT_HANGAR_1", design=Target.DesignID)
                ),
                SetMaxSecondaryStat(partname="FT_HANGAR_2", value=Value + 2 * FIGHTER_DAMAGE_FACTOR),
                SetMaxSecondaryStat(partname="FT_HANGAR_3", value=Value + 3 * FIGHTER_DAMAGE_FACTOR),
                SetMaxSecondaryStat(partname="FT_HANGAR_4", value=Value + 6 * FIGHTER_DAMAGE_FACTOR),
            ],
        )
    ],
    graphic="icons/ship_parts/fighter05.png",
)
