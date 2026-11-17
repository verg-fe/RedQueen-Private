#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <ctype.h>

#include "ra3fog.h"
#include "ra3ipc.h"

// ============================================================
// Verified RA3 1.12 Engine Globals & Offsets (100% Read-Only)
// ============================================================
#define RA3_ADDR_GAME_LOGIC        0x00CD8CE4u
#define RA3_ADDR_PLAYER_LIST       0x00CE8C9Cu
#define RA3_ADDR_ASSET_NAME_TABLE  0x00CD7C38u
#define RA3_ADDR_CAM_MANAGER       0x00CEBCE0u

// ============================================================
// Safe Pointer & EA SuperFastHash Helpers (Plain POD / C-SEH)
// ============================================================
static inline bool ValidUserPtr(uintptr_t p) {
    return (p >= 0x00010000u && p < 0x7FFF0000u);
}

static inline bool ValidGameCodePtr(uintptr_t p) {
    return (p >= 0x00401000u && p < 0x00BF0000u);
}

static inline bool ValidGameRdataPtr(uintptr_t p) {
    return (p >= 0x00BC0000u && p < 0x00CE0000u);
}

// Exact match for 0x00417FA0 (lowercase) and 0x00417580 (exact case)
static uint32_t EaSuperFastHash(const char* s, bool toLowerCase) {
    if (!s || !*s) return 0;
    uint32_t len = (uint32_t)strlen(s);
    uint32_t h = len;
    uint32_t rem = len & 3u;
    uint32_t q = len >> 2;
    const unsigned char* p = (const unsigned char*)s;

    auto chAt = [toLowerCase](const unsigned char* ptr, size_t idx) -> int32_t {
        int c = (int)ptr[idx];
        if (toLowerCase) c = tolower(c);
        return (int32_t)(signed char)(unsigned char)c;
    };

    for (uint32_t i = 0; i < q; ++i) {
        int32_t b0 = chAt(p, 0);
        int32_t b1 = chAt(p, 1);
        int32_t b2 = chAt(p, 2);
        int32_t b3 = chAt(p, 3);
        h += (uint32_t)((b1 << 8) | b0);
        uint32_t tmp = ((uint32_t)((b3 << 8) | b2) ^ (h << 5)) << 11;
        h ^= tmp;
        h += (h >> 11);
        p += 4;
    }

    if (rem == 3) {
        int32_t b0 = chAt(p, 0);
        int32_t b1 = chAt(p, 1);
        int32_t b2 = chAt(p, 2);
        h += (uint32_t)((b1 << 8) | b0);
        h ^= (((uint32_t)(b2 << 2) ^ h) << 16);
        h += (h >> 11);
    } else if (rem == 2) {
        int32_t b0 = chAt(p, 0);
        int32_t b1 = chAt(p, 1);
        h += (uint32_t)((b1 << 8) | b0);
        h ^= (h << 11);
        h += (h >> 17);
    } else if (rem == 1) {
        int32_t b0 = chAt(p, 0);
        h += (uint32_t)b0;
        h ^= (h << 10);
        h += (h >> 1);
    }

    // Avalanche (0x0041765D)
    h ^= (h << 3);
    h += (h >> 5);
    h ^= (h << 2);
    h += (h >> 15);
    h ^= (h << 10);
    return h;
}

static const char* LookupAssetNameByHashSEH(uint32_t hash) {
    if (!hash) return NULL;
    uintptr_t arrPtr = *(const uintptr_t*)RA3_ADDR_ASSET_NAME_TABLE;
    if (!ValidUserPtr(arrPtr)) return NULL;
    const uintptr_t* tableArr = (const uintptr_t*)arrPtr;
    for (int i = 0; i < 4; ++i) {
        uintptr_t blk = tableArr[i];
        if (!ValidUserPtr(blk)) continue;
        uintptr_t map = blk + 4;
        uintptr_t buckets = *(const uintptr_t*)(map + 4);
        uint32_t numBuckets = *(const uint32_t*)(map + 8);
        if (!ValidUserPtr(buckets) || numBuckets == 0 || numBuckets > 0x200000u) continue;
        uintptr_t endSentinel = *(const uintptr_t*)(buckets + (size_t)numBuckets * 4);
        uint32_t idx = hash % numBuckets;
        uintptr_t node = *(const uintptr_t*)(buckets + (size_t)idx * 4);
        for (int step = 0; step < 64 && ValidUserPtr(node) && node != endSentinel; ++step) {
            if (*(const uint32_t*)node == hash) {
                uintptr_t s = *(const uintptr_t*)(node + 4);
                if (ValidUserPtr(s)) return (const char*)s;
                return NULL;
            }
            node = *(const uintptr_t*)(node + 8);
        }
    }
    return NULL;
}

// ============================================================
// Template & Special Power Classification Catalog
// ============================================================
enum FactionId {
    FAC_UNKNOWN = 0,
    FAC_ALLIED  = 1,
    FAC_SOVIET  = 2,
    FAC_JAPAN   = 3,
};

enum ObjClass {
    OBJ_NONE = 0,
    OBJ_CONYARD,
    OBJ_POWER,
    OBJ_POWER_ADV,   // Soviet Super Reactor -> T2
    OBJ_REFINERY,
    OBJ_BARRACKS,
    OBJ_WARFACTORY,
    OBJ_AIRFIELD,
    OBJ_NAVALYARD,
    OBJ_TECH_T2_BLD, // Japan *Tech2 upgraded production building
    OBJ_TECH_T3_BLD, // TechStructure (Defense Bureau / Battle Lab / Nanotech) or Japan *Tech3
    OBJ_SW_BASIC,    // Chronosphere / IronCurtain / Nanoswarm
    OBJ_SW_ADV,      // Proton Collider / Vacuum Imploder / Psionic Decimator
    OBJ_DEFENSE_GND, // Sentry / Multigunner / Tesla / Spectrum / Waveforce / Defender
    OBJ_DEFENSE_AIR, // Flak Cannon
    OBJ_ALERT_ENGI,
    OBJ_ALERT_SPY,
    OBJ_ALERT_CMD,
    OBJ_ALERT_BOMBER,
    OBJ_ALERT_TWIN,
    OBJ_ALERT_KIROV,
    OBJ_ALERT_TRANS,
    OBJ_UNIT_T2,
    OBJ_UNIT_T3,
};

struct TemplateRule {
    const char* name;
    uint32_t    hashLower;
    uint32_t    hashExact;
    ObjClass    cls;
    FactionId   faction;
    const char* shortCn;
    bool        isDefense;
    bool        isAA;
};

static TemplateRule g_tplRules[] = {
    // Allied structures
    { "AlliedConstructionYard",    0, 0, OBJ_CONYARD,     FAC_ALLIED, NULL,          false, false },
    { "AlliedPowerPlant",          0, 0, OBJ_POWER,       FAC_ALLIED, NULL,          false, false },
    { "AlliedRefinery",            0, 0, OBJ_REFINERY,    FAC_ALLIED, NULL,          false, false },
    { "AlliedBarracks",            0, 0, OBJ_BARRACKS,    FAC_ALLIED, NULL,          false, false },
    { "AlliedWarFactory",          0, 0, OBJ_WARFACTORY,  FAC_ALLIED, NULL,          false, false },
    { "AlliedAirfield",            0, 0, OBJ_AIRFIELD,    FAC_ALLIED, NULL,          false, false },
    { "AlliedNavalYard",           0, 0, OBJ_NAVALYARD,   FAC_ALLIED, NULL,          false, false },
    { "AlliedOutpost",             0, 0, OBJ_CONYARD,     FAC_ALLIED, NULL,          false, false },
    { "AlliedTechStructure",       0, 0, OBJ_TECH_T3_BLD, FAC_ALLIED, NULL,          false, false },
    { "AlliedSuperWeapon",         0, 0, OBJ_SW_BASIC,    FAC_ALLIED, "超时空传送仪", false, false },
    { "AlliedSuperWeaponAdvanced", 0, 0, OBJ_SW_ADV,      FAC_ALLIED, "质子撞击炮",   false, false },
    { "AlliedBaseDefense",         0, 0, OBJ_DEFENSE_GND, FAC_ALLIED, "多功能炮塔",   true,  true  },
    { "AlliedBaseDefenseAdvanced", 0, 0, OBJ_DEFENSE_GND, FAC_ALLIED, "光棱塔",       true,  false },

    // Soviet structures
    { "SovietConstructionYard",    0, 0, OBJ_CONYARD,     FAC_SOVIET, NULL,          false, false },
    { "SovietPowerPlant",          0, 0, OBJ_POWER,       FAC_SOVIET, NULL,          false, false },
    { "SovietPowerPlantAdvanced",  0, 0, OBJ_POWER_ADV,   FAC_SOVIET, NULL,          false, false },
    { "SovietRefinery",            0, 0, OBJ_REFINERY,    FAC_SOVIET, NULL,          false, false },
    { "SovietBarracks",            0, 0, OBJ_BARRACKS,    FAC_SOVIET, NULL,          false, false },
    { "SovietWarFactory",          0, 0, OBJ_WARFACTORY,  FAC_SOVIET, NULL,          false, false },
    { "SovietAirfield",            0, 0, OBJ_AIRFIELD,    FAC_SOVIET, NULL,          false, false },
    { "SovietNavalYard",           0, 0, OBJ_NAVALYARD,   FAC_SOVIET, NULL,          false, false },
    { "SovietOutpost",             0, 0, OBJ_CONYARD,     FAC_SOVIET, NULL,          false, false },
    { "SovietTechStructure",       0, 0, OBJ_TECH_T3_BLD, FAC_SOVIET, NULL,          false, false },
    { "SovietSuperWeapon",         0, 0, OBJ_SW_BASIC,    FAC_SOVIET, "铁幕装置",     false, false },
    { "SovietSuperWeaponAdvanced", 0, 0, OBJ_SW_ADV,      FAC_SOVIET, "真空内爆弹",   false, false },
    { "SovietBaseDefenseGround",   0, 0, OBJ_DEFENSE_GND, FAC_SOVIET, "哨戒炮",       true,  false },
    { "SovietBaseDefenseAir",      0, 0, OBJ_DEFENSE_AIR, FAC_SOVIET, "防空炮",       true,  true  },
    { "SovietBaseDefenseAdvanced", 0, 0, OBJ_DEFENSE_GND, FAC_SOVIET, "磁暴线圈",     true,  false },

    // Japan structures
    { "JapanConstructionYard",     0, 0, OBJ_CONYARD,     FAC_JAPAN,  NULL,          false, false },
    { "JapanPowerPlant",           0, 0, OBJ_POWER,       FAC_JAPAN,  NULL,          false, false },
    { "JapanRefinery",             0, 0, OBJ_REFINERY,    FAC_JAPAN,  NULL,          false, false },
    { "JapanBarracks",             0, 0, OBJ_BARRACKS,    FAC_JAPAN,  NULL,          false, false },
    { "JapanBarracksTech2",        0, 0, OBJ_TECH_T2_BLD, FAC_JAPAN,  NULL,          false, false },
    { "JapanBarracksTech3",        0, 0, OBJ_TECH_T3_BLD, FAC_JAPAN,  NULL,          false, false },
    { "JapanWarFactory",           0, 0, OBJ_WARFACTORY,  FAC_JAPAN,  NULL,          false, false },
    { "JapanWarFactoryTech2",      0, 0, OBJ_TECH_T2_BLD, FAC_JAPAN,  NULL,          false, false },
    { "JapanWarFactoryTech3",      0, 0, OBJ_TECH_T3_BLD, FAC_JAPAN,  NULL,          false, false },
    { "JapanNavalYard",            0, 0, OBJ_NAVALYARD,   FAC_JAPAN,  NULL,          false, false },
    { "JapanNavalYardTech2",       0, 0, OBJ_TECH_T2_BLD, FAC_JAPAN,  NULL,          false, false },
    { "JapanNavalYardTech3",       0, 0, OBJ_TECH_T3_BLD, FAC_JAPAN,  NULL,          false, false },
    { "JapanTechStructure",        0, 0, OBJ_TECH_T3_BLD, FAC_JAPAN,  NULL,          false, false },
    { "JapanSuperWeapon",          0, 0, OBJ_SW_BASIC,    FAC_JAPAN,  "纳米虫群",     false, false },
    { "JapanSuperWeaponAdvanced",  0, 0, OBJ_SW_ADV,      FAC_JAPAN,  "超能波毁灭仪", false, false },
    { "JapanBaseDefense",          0, 0, OBJ_DEFENSE_GND, FAC_JAPAN,  "防卫塔VX",     true,  true  },
    { "JapanBaseDefenseAdvanced",  0, 0, OBJ_DEFENSE_GND, FAC_JAPAN,  "波能塔",       true,  false },

    // High-Risk Units (HUD 3 Sneak Attack Radar)
    { "AlliedEngineer",            0, 0, OBJ_ALERT_ENGI,  FAC_ALLIED, "盟军工程师",   false, false },
    { "SovietEngineer",            0, 0, OBJ_ALERT_ENGI,  FAC_SOVIET, "苏联工程师",   false, false },
    { "JapanEngineer",             0, 0, OBJ_ALERT_ENGI,  FAC_JAPAN,  "帝国工程师",   false, false },
    { "AlliedInfiltrationInfantry",0, 0, OBJ_ALERT_SPY,   FAC_ALLIED, "盟军间谍",     false, false },
    { "AlliedSpy",                 0, 0, OBJ_ALERT_SPY,   FAC_ALLIED, "盟军间谍",     false, false },
    { "JapanInfiltrationInfantry", 0, 0, OBJ_ALERT_SPY,   FAC_JAPAN,  "帝国忍者",     false, false },
    { "AlliedCommandoTech1",       0, 0, OBJ_ALERT_CMD,   FAC_ALLIED, "谭雅",         false, false },
    { "SovietCommandoTech1",       0, 0, OBJ_ALERT_CMD,   FAC_SOVIET, "娜塔莎",       false, false },
    { "JapanCommandoTech1",        0, 0, OBJ_ALERT_CMD,   FAC_JAPAN,  "百合子",       false, false },
    { "AlliedBomberAircraft",      0, 0, OBJ_ALERT_BOMBER,FAC_ALLIED, "世纪轰炸机",   false, false },
    { "SovietAntiGroundAircraft",  0, 0, OBJ_ALERT_TWIN,  FAC_SOVIET, "双刃直升机",   false, false },
    { "SovietBomberAircraft",      0, 0, OBJ_ALERT_KIROV, FAC_SOVIET, "基洛夫飞艇",   false, false },
    { "AlliedAntiInfantryVehicle", 0, 0, OBJ_ALERT_TRANS, FAC_ALLIED, "激流ACV",      false, false },
    { "SovietAntiAirVehicleTech1", 0, 0, OBJ_ALERT_TRANS, FAC_SOVIET, "牛蛙载具",     false, false },
    { "SovietTransport",           0, 0, OBJ_ALERT_TRANS, FAC_SOVIET, "苏联运输艇",   false, false },
    { "JapanLightTransportVehicle",0, 0, OBJ_ALERT_TRANS, FAC_JAPAN,  "迅雷运输车",   false, false },

    // Aircraft & Tech indicators (useful for HUD 4 air vector tags & tech inference)
    { "AlliedAntiGroundAircraft",  0, 0, OBJ_UNIT_T2,     FAC_ALLIED, "维和轰炸机",   false, false },
    { "AlliedSupportAircraft",     0, 0, OBJ_UNIT_T2,     FAC_ALLIED, "冷冻直升机",   false, false },
    { "AlliedFighterAircraft",     0, 0, OBJ_UNIT_T2,     FAC_ALLIED, "阿波罗战机",   false, false },
    { "SovietFighterAircraft",     0, 0, OBJ_UNIT_T2,     FAC_SOVIET, "米格战机",     false, false },
    { "JapanAntiAirVehicleTech1",  0, 0, OBJ_UNIT_T2,     FAC_JAPAN,  "VX直升机",     false, false },
    { "JapanAntiInfantryVehicle",  0, 0, OBJ_UNIT_T2,     FAC_JAPAN,  "天狗战机",     false, false },
    { "AlliedAntiVehicleVehicleTech3", 0, 0, OBJ_UNIT_T3, FAC_ALLIED, NULL,          false, false },
    { "AlliedAntiStructureVehicle",0, 0, OBJ_UNIT_T3,     FAC_ALLIED, NULL,          false, false },
    { "SovietAntiVehicleVehicleTech3", 0, 0, OBJ_UNIT_T3, FAC_SOVIET, NULL,          false, false },
    { "SovietAntiStructureVehicle",0, 0, OBJ_UNIT_T3,     FAC_SOVIET, NULL,          false, false },
    { "JapanAntiVehicleVehicleTech3", 0, 0, OBJ_UNIT_T3,  FAC_JAPAN,  NULL,          false, false },
    { "JapanAntiStructureVehicle", 0, 0, OBJ_UNIT_T3,     FAC_JAPAN,  NULL,          false, false },
};

struct SpecialPowerRule {
    const char* name;
    uint32_t    hashLower;
    uint32_t    hashExact;
    const char* cnLabel;
    bool        isSuperweapon;
};

static SpecialPowerRule g_spRules[] = {
    { "SpecialPowerVacuumBomb",               0, 0, "真空内爆弹", true  },
    { "SpecialPowerParticleCannon",           0, 0, "质子撞击炮", true  },
    { "SpecialPowerTelekineticProjectorDevice",0,0, "超能波毁灭", true  },
    { "SpecialPowerIronCurtain",              0, 0, "铁幕装置",   true  },
    { "SpecialPowerChronosphereObjectSelect", 0, 0, "超时空传送", true  },
    { "SpecialPowerNanoSwarmHive",            0, 0, "纳米虫群",   true  },
    { "SpecialPowerCryoSatelliteLvl1",        0, 0, "冷冻卫星I",  false },
    { "SpecialPowerCryoSatelliteLvl2",        0, 0, "冷冻卫星II", false },
    { "SpecialPowerCryoSatelliteLvl3",        0, 0, "冷冻卫星III",false },
    { "SpecialPowerChronoRiftSelectLvl1",     0, 0, "超时空裂缝I",false },
    { "SpecialPowerChronoRiftSelectLvl2",     0, 0, "超时空裂缝II",false},
    { "SpecialPowerChronoRiftSelectLvl3",     0, 0, "超时空裂缝III",false},
    { "SpecialPowerPrecisionStrike",          0, 0, "精确打击",   false },
    { "SpecialPowerBombingRun",               0, 0, "轰炸空袭",   false },
    { "SpecialPowerTimeBombLvl1",             0, 0, "超时空炸弹I",false },
    { "SpecialPowerTimeBombLvl2",             0, 0, "超时空炸弹II",false},
    { "SpecialPowerTimeBombLvl3",             0, 0, "超时空炸弹III",false},
    { "SpecialPowerMagneticSatelliteLvl1",    0, 0, "磁力卫星I",  false },
    { "SpecialPowerMagneticSatelliteLvl2",    0, 0, "磁力卫星II", false },
    { "SpecialPowerMagneticSatelliteLvl3",    0, 0, "磁力卫星III",false },
    { "SpecialPowerMagneticSingularity",      0, 0, "磁力奇点",   false },
    { "SpecialPowerJapanAmbush",              0, 0, "决死奇袭",   false },
    { "SpecialPowerJapanEnhancedKamikaze",    0, 0, "神风冲撞",   false },
    { "SpecialPowerJapanNavalPower",          0, 0, "海军强化",   false },
    { "SpecialPowerSovietMassProduction",     0, 0, "大生产",     false },
    { "SpecialPowerHighTechnology",           0, 0, "高新科技",   false },
    { "SpecialPowerAirPower",                 0, 0, "制空权",     false },
    { "SpecialPowerSatelliteSweep",           0, 0, "轨道垃圾",   false },
    { "SpecialPowerOrbitalRefuse",            0, 0, "轨道抛射",   false },
    { "SpecialPowerParadropLvl1",             0, 0, "终极空降I",  false },
    { "SpecialPowerParadropLvl2",             0, 0, "终极空降II", false },
    { "SpecialPowerParadropLvl3",             0, 0, "终极空降III",false },
};

static uint32_t g_hashFactionAllies = 0;
static uint32_t g_hashFactionSoviet = 0;
static uint32_t g_hashFactionJapan  = 0;
static bool     g_catalogReady      = false;

static void EnsureCatalogHashes(void) {
    if (g_catalogReady) return;
    g_hashFactionAllies = EaSuperFastHash("FactionAllies", true);
    g_hashFactionSoviet = EaSuperFastHash("FactionSoviet", true);
    g_hashFactionJapan  = EaSuperFastHash("FactionJapan",  true);
    for (size_t i = 0; i < sizeof(g_tplRules) / sizeof(g_tplRules[0]); ++i) {
        g_tplRules[i].hashLower = EaSuperFastHash(g_tplRules[i].name, true);
        g_tplRules[i].hashExact = EaSuperFastHash(g_tplRules[i].name, false);
    }
    for (size_t i = 0; i < sizeof(g_spRules) / sizeof(g_spRules[0]); ++i) {
        g_spRules[i].hashLower = EaSuperFastHash(g_spRules[i].name, true);
        g_spRules[i].hashExact = EaSuperFastHash(g_spRules[i].name, false);
    }
    g_catalogReady = true;
}

static const TemplateRule* MatchTemplateRule(uint32_t ttHash, const char* resolvedName) {
    if (ttHash) {
        for (size_t i = 0; i < sizeof(g_tplRules) / sizeof(g_tplRules[0]); ++i) {
            if (g_tplRules[i].hashLower == ttHash || g_tplRules[i].hashExact == ttHash) {
                return &g_tplRules[i];
            }
        }
    }
    if (resolvedName && *resolvedName) {
        for (size_t i = 0; i < sizeof(g_tplRules) / sizeof(g_tplRules[0]); ++i) {
            if (_stricmp(resolvedName, g_tplRules[i].name) == 0) {
                return &g_tplRules[i];
            }
        }
    }
    return NULL;
}

static const SpecialPowerRule* MatchSpecialPowerRule(uint32_t spHash, const char* resolvedName) {
    if (spHash) {
        for (size_t i = 0; i < sizeof(g_spRules) / sizeof(g_spRules[0]); ++i) {
            if (g_spRules[i].hashLower == spHash || g_spRules[i].hashExact == spHash) {
                return &g_spRules[i];
            }
        }
    }
    if (resolvedName && *resolvedName) {
        for (size_t i = 0; i < sizeof(g_spRules) / sizeof(g_spRules[0]); ++i) {
            if (_stricmp(resolvedName, g_spRules[i].name) == 0) {
                return &g_spRules[i];
            }
        }
    }
    return NULL;
}

// ============================================================
// Camera Matrix Pod (Plain POD — Safe Inside SEH)
// ============================================================
struct CamMatrixPod {
    bool  valid;
    float v[12];  // cam + 0x04 .. + 0x30 (3 rows x 4 floats)
    float px[4];  // cam + 0x64, 0x74, 0x84, 0x94
    float py[4];  // cam + 0x68, 0x78, 0x88, 0x98
    float pw[4];  // cam + 0x70, 0x80, 0x90, 0xA0
    float nearClipBias; // cam + 0x214
};

static bool ReadCameraMatrixSEH(CamMatrixPod* outCam) {
    memset(outCam, 0, sizeof(*outCam));
    uintptr_t camMgr = *(const uintptr_t*)RA3_ADDR_CAM_MANAGER;
    if (!ValidUserPtr(camMgr)) return false;

    uintptr_t begin = *(const uintptr_t*)(camMgr + 0x24u);
    uintptr_t end   = *(const uintptr_t*)(camMgr + 0x28u);
    uint32_t  idx   = *(const uint32_t*)(camMgr + 0x34u);
    if (!ValidUserPtr(begin) || !ValidUserPtr(end) || end <= begin) return false;
    uint32_t count = (uint32_t)((end - begin) >> 2);
    if (idx >= count) return false;

    uintptr_t view = *(const uintptr_t*)(begin + (size_t)idx * 4);
    if (!ValidUserPtr(view)) return false;
    uintptr_t vtbl = *(const uintptr_t*)view;
    if (!ValidGameRdataPtr(vtbl)) return false;
    uintptr_t getCamFn = *(const uintptr_t*)(vtbl + 0x1Cu);
    if (!ValidGameCodePtr(getCamFn)) return false;

    typedef void* (__thiscall *PFN_GetCamera)(void*);
    uintptr_t cam = (uintptr_t)((PFN_GetCamera)getCamFn)((void*)view);
    if (!ValidUserPtr(cam)) return false;

    for (int i = 0; i < 12; ++i) {
        outCam->v[i] = *(const float*)(cam + 0x04u + (size_t)i * 4);
    }
    outCam->px[0] = *(const float*)(cam + 0x64u);
    outCam->px[1] = *(const float*)(cam + 0x74u);
    outCam->px[2] = *(const float*)(cam + 0x84u);
    outCam->px[3] = *(const float*)(cam + 0x94u);

    outCam->py[0] = *(const float*)(cam + 0x68u);
    outCam->py[1] = *(const float*)(cam + 0x78u);
    outCam->py[2] = *(const float*)(cam + 0x88u);
    outCam->py[3] = *(const float*)(cam + 0x98u);

    outCam->pw[0] = *(const float*)(cam + 0x70u);
    outCam->pw[1] = *(const float*)(cam + 0x80u);
    outCam->pw[2] = *(const float*)(cam + 0x90u);
    outCam->pw[3] = *(const float*)(cam + 0xA0u);

    outCam->nearClipBias = *(const float*)(cam + 0x214u);
    outCam->valid = true;
    return true;
}

static bool ProjectWorldToScreen(const CamMatrixPod& cam, float x, float y, float z,
                                 float vpW, float vpH, float* outSx, float* outSy, bool* outInFront) {
    if (!cam.valid) return false;
    float vx = cam.v[0] * x + cam.v[1] * y + cam.v[2]  * z + cam.v[3];
    float vy = cam.v[4] * x + cam.v[5] * y + cam.v[6]  * z + cam.v[7];
    float vz = cam.v[8] * x + cam.v[9] * y + cam.v[10] * z + cam.v[11];

    float maxZ = 1.0f - cam.nearClipBias;
    bool inFront = (vz <= maxZ);
    if (outInFront) *outInFront = inFront;

    float pw = cam.pw[0] * vx + cam.pw[1] * vy + cam.pw[2] * vz + cam.pw[3];
    if (fabsf(pw) < 1e-4f) return false;

    float px = cam.px[0] * vx + cam.px[1] * vy + cam.px[2] * vz + cam.px[3];
    float py = cam.py[0] * vx + cam.py[1] * vy + cam.py[2] * vz + cam.py[3];

    float invW = 1.0f / pw;
    float ndcX = px * invW;
    float ndcY = py * invW;

    // If behind camera, flip NDC away from center so edge radar arrows point backwards accurately
    if (!inFront) {
        ndcX = -ndcX;
        ndcY = -ndcY;
    }

    *outSx = (ndcX + 1.0f) * vpW * 0.5f;
    *outSy = (1.0f - ndcY) * vpH * 0.5f;
    return inFront;
}

static bool CaptureMacroCameraSEH(MacroCamPod* outCam) {
    memset(outCam, 0, sizeof(*outCam));
    __try {
        uintptr_t gameLogic = *(const uintptr_t*)RA3_ADDR_GAME_LOGIC;
        uintptr_t playerList = *(const uintptr_t*)RA3_ADDR_PLAYER_LIST;
        if (!ValidUserPtr(gameLogic) || !ValidUserPtr(playerList)) return false;

        uintptr_t objHead = *(const uintptr_t*)(gameLogic + 0x90u);
        if (!ValidUserPtr(objHead)) return false;

        CamMatrixPod cam;
        if (ReadCameraMatrixSEH(&cam)) {
            memcpy(outCam->v, cam.v, sizeof(outCam->v));
            memcpy(outCam->px, cam.px, sizeof(outCam->px));
            memcpy(outCam->py, cam.py, sizeof(outCam->py));
            memcpy(outCam->pw, cam.pw, sizeof(outCam->pw));
            outCam->nearClipBias = cam.nearClipBias;
            outCam->valid = cam.valid;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ============================================================
// Zero-Dependency DX9 Rendering Engine (GDI ClearType -> D3D9)
// ============================================================
struct HudVertex {
    float    x, y, z, rhw;
    uint32_t color;
    float    u, v;
};
#define HUD_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

struct GdiTexPanel {
    int                w, h;
    HDC                hdc;
    HBITMAP            hbm;
    HBITMAP            hOldBm;
    HFONT              hFont;
    HFONT              hFontBold;
    uint32_t*          bits;
    IDirect3DTexture9* tex;
    DWORD              lastUpdateMs;
    uint32_t           lastHash;
};

static IDirect3DStateBlock9* g_stateBlock = NULL;
static IDirect3DDevice9*     g_lastDev    = NULL;
static GdiTexPanel           g_panelMacro = {0};

static uint32_t Fnv1aBytes(const void* data, size_t n) {
    uint32_t h = 2166136261u;
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void DestroyPanel(GdiTexPanel* p) {
    if (p->tex) { p->tex->Release(); p->tex = NULL; }
    if (p->hdc) {
        if (p->hOldBm) SelectObject(p->hdc, p->hOldBm);
        if (p->hbm)    DeleteObject(p->hbm);
        if (p->hFont)  DeleteObject(p->hFont);
        if (p->hFontBold) DeleteObject(p->hFontBold);
        DeleteDC(p->hdc);
    }
    memset(p, 0, sizeof(*p));
}

static bool EnsurePanel(IDirect3DDevice9* dev, GdiTexPanel* p, int w, int h) {
    if (p->tex && p->hdc && p->w == w && p->h == h) return true;
    DestroyPanel(p);

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) return false;
    void* bits = NULL;
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits) {
        if (hbm) DeleteObject(hbm);
        DeleteDC(hdc);
        return false;
    }

    HRESULT hr = dev->CreateTexture((UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &p->tex, NULL);
    if (FAILED(hr) || !p->tex) {
        DeleteObject(hbm);
        DeleteDC(hdc);
        return false;
    }

    p->w = w;
    p->h = h;
    p->hdc = hdc;
    p->hbm = hbm;
    p->hOldBm = (HBITMAP)SelectObject(hdc, hbm);
    p->bits = (uint32_t*)bits;
    p->hFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
    p->hFontBold = CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
    SetBkMode(hdc, TRANSPARENT);
    return true;
}

// Converts black-background GDI AA rendering into crisp premultiplied/standard ARGB over glass card
static void BeginGdiCanvas(GdiTexPanel* p) {
    memset(p->bits, 0, (size_t)p->w * (size_t)p->h * 4);
}

// Draws clean text using a temporary green-intensity mask into the panel's ARGB DIB with 1px dark outline
static void DrawPanelTextW(GdiTexPanel* p, int x, int y, const wchar_t* txt, COLORREF rgb, bool bold) {
    if (!txt || !*txt) return;
    HFONT f = bold ? p->hFontBold : p->hFont;
    if (f) SelectObject(p->hdc, f);
    // Pass 1: 1px shadow at +1,+1 (drawn in dim non-zero marker color, or draw onto clean black text buffer)
    // Simpler ultra-clean approach: draw onto a static scratch row? Even simpler:
    // GDI leaves alpha=0 on pixels it draws, while untouched background stays 0x00000000!
    // Use GDI COLORREF: RGB -> BGR in memory; we tag drawn pixels later OR draw text in pure RGB after clearing!
    // Notice if any RGB channel is >= 8 on dark shadow or >= 32 on colored text, alpha can be boosted cleanly!
    SetTextColor(p->hdc, RGB(12, 14, 18));
    RECT rcS = { x + 1, y + 1, p->w - 4, p->h - 2 };
    DrawTextW(p->hdc, txt, -1, &rcS, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(p->hdc, rgb);
    RECT rc = { x, y, p->w - 4, p->h - 2 };
    DrawTextW(p->hdc, txt, -1, &rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
}

static void FinalizeAndUploadPanel(GdiTexPanel* p, uint32_t bgArgb, uint32_t borderArgb) {
    GdiFlush();
    const int n = p->w * p->h;
    const uint32_t bgA = (bgArgb >> 24) & 0xFFu;
    const uint32_t bgR = (bgArgb >> 16) & 0xFFu;
    const uint32_t bgG = (bgArgb >> 8)  & 0xFFu;
    const uint32_t bgB = (bgArgb)       & 0xFFu;

    for (int i = 0; i < n; ++i) {
        uint32_t px = p->bits[i];
        uint32_t r = (px >> 16) & 0xFFu;
        uint32_t g = (px >> 8)  & 0xFFu;
        uint32_t b = (px)       & 0xFFu;
        if ((r | g | b) == 0) {
            p->bits[i] = bgArgb;
        } else {
            // Boost text visibility against dark glass backdrop
            uint32_t lum = (r > g ? (r > b ? r : b) : (g > b ? g : b));
            uint32_t a = bgA + ((255u - bgA) * lum) / 255u;
            if (r == 12 && g == 14 && b == 18) {
                p->bits[i] = (0xE8u << 24) | 0x080A0Eu;
            } else {
                // Blend bright text over glass background subtly for crisp AA edges
                uint32_t outR = (r > bgR) ? r : (bgR + r / 2);
                uint32_t outG = (g > bgG) ? g : (bgG + g / 2);
                uint32_t outB = (b > bgB) ? b : (bgB + b / 2);
                if (outR > 255u) outR = 255u;
                if (outG > 255u) outG = 255u;
                if (outB > 255u) outB = 255u;
                p->bits[i] = ((a < 235u ? (a + 20u) : 255u) << 24) | (outR << 16) | (outG << 8) | outB;
            }
        }
    }

    // 1px outer border
    for (int x = 0; x < p->w; ++x) {
        p->bits[x] = borderArgb;
        p->bits[(p->h - 1) * p->w + x] = borderArgb;
    }
    for (int y = 0; y < p->h; ++y) {
        p->bits[y * p->w] = borderArgb;
        p->bits[y * p->w + (p->w - 1)] = borderArgb;
    }

    D3DLOCKED_RECT lr;
    if (SUCCEEDED(p->tex->LockRect(0, &lr, NULL, 0))) {
        const unsigned char* src = (const unsigned char*)p->bits;
        unsigned char* dst = (unsigned char*)lr.pBits;
        const int rowBytes = p->w * 4;
        for (int y = 0; y < p->h; ++y) {
            memcpy(dst + (size_t)y * lr.Pitch, src + (size_t)y * rowBytes, (size_t)rowBytes);
        }
        p->tex->UnlockRect(0);
    }
}

static void DrawTexturedQuad(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float x, float y, float w, float h) {
    if (!tex) return;
    HudVertex verts[4] = {
        { x - 0.5f,     y - 0.5f,     0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f },
        { x + w - 0.5f, y - 0.5f,     0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f },
        { x - 0.5f,     y + h - 0.5f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f },
        { x + w - 0.5f, y + h - 0.5f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f },
    };
    dev->SetTexture(0, tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(HudVertex));
}

// ============================================================
// Public HUD Lifecycle & DX9 State Hygiene
// ============================================================
void Hud_Init(void) {
    EnsureCatalogHashes();
}

void Hud_OnReset(void) {
    if (g_stateBlock) {
        g_stateBlock->Release();
        g_stateBlock = NULL;
    }
}

void Hud_Shutdown(void) {
    Hud_OnReset();
    DestroyPanel(&g_panelMacro);
    g_lastDev = NULL;
}

bool Hud_ProjectWorld(const MacroCamPod* cam, float x, float y, float z,
                      float vpW, float vpH, float* outSx, float* outSy, bool* outInFront) {
    if (!cam) return false;
    CamMatrixPod pod;
    memcpy(pod.v, cam->v, sizeof(pod.v));
    memcpy(pod.px, cam->px, sizeof(pod.px));
    memcpy(pod.py, cam->py, sizeof(pod.py));
    memcpy(pod.pw, cam->pw, sizeof(pod.pw));
    pod.nearClipBias = cam->nearClipBias;
    pod.valid = cam->valid;
    return ProjectWorldToScreen(pod, x, y, z, vpW, vpH, outSx, outSy, outInFront);
}

void Hud_LookupTemplateLabelW(uintptr_t tt, wchar_t* outBuf, int maxChars) {
    if (!outBuf || maxChars <= 0) return;
    outBuf[0] = L'\0';
    EnsureCatalogHashes();
    if (!ValidUserPtr(tt)) return;

    uint32_t ttHash = *(const uint32_t*)(tt + 0x04u);
    const char* ttName = LookupAssetNameByHashSEH(ttHash);
    const TemplateRule* rule = MatchTemplateRule(ttHash, ttName);
    if (rule && rule->shortCn && rule->shortCn[0]) {
        MultiByteToWideChar(CP_UTF8, 0, rule->shortCn, -1, outBuf, maxChars);
        return;
    }
    if (ttName && *ttName) {
        int k = 0;
        while (k < maxChars - 1 && ttName[k] != '\0') {
            outBuf[k] = (wchar_t)(unsigned char)ttName[k];
            ++k;
        }
        outBuf[k] = L'\0';
        return;
    }
    swprintf_s(outBuf, (size_t)maxChars, L"Tpl_%06X", ttHash & 0xFFFFFFu);
}

void Hud_DrawMacroBannerW(void* device, const wchar_t* line1, const wchar_t* line2,
                          float x, float y, float w, float h) {
    IDirect3DDevice9* dev = (IDirect3DDevice9*)device;
    if (!dev || !line1) return;
    int iw = (int)w;
    int ih = (int)h;
    if (!EnsurePanel(dev, &g_panelMacro, iw, ih)) return;

    const wchar_t* l2 = line2 ? line2 : L"";
    uint32_t h1 = Fnv1aBytes(line1, wcslen(line1) * sizeof(wchar_t));
    uint32_t h2 = Fnv1aBytes(l2, wcslen(l2) * sizeof(wchar_t));
    uint32_t hv = h1 ^ (h2 * 16777619u);
    if (g_panelMacro.lastHash != hv) {
        BeginGdiCanvas(&g_panelMacro);
        bool singleLine = (!l2[0]);
        if (singleLine) {
            bool isSuccess = (wcsstr(line1, L"\u2713") != NULL);
            COLORREF textCol = isSuccess ? RGB(130, 255, 168) : RGB(195, 245, 218);
            int ty = (ih - 16) / 2;
            if (ty < 2) ty = 2;
            DrawPanelTextW(&g_panelMacro, 12, ty, line1, textCol, true);
            FinalizeAndUploadPanel(&g_panelMacro, 0xB80B131Du, isSuccess ? 0xFF42C472u : 0xD03E8E58u);
        } else {
            DrawPanelTextW(&g_panelMacro, 10, 4,  line1, RGB(140, 245, 175), true);
            DrawPanelTextW(&g_panelMacro, 10, 24, l2,    RGB(220, 232, 248), false);
            FinalizeAndUploadPanel(&g_panelMacro, 0xC40D1520u, 0xFF3E8E58u);
        }
        g_panelMacro.lastHash = hv;
    }
    DrawTexturedQuad(dev, g_panelMacro.tex, x, y, (float)iw, (float)ih);
}

void Hud_OnPresent(void* device) {
    IDirect3DDevice9* dev = (IDirect3DDevice9*)device;
    if (!dev || !Macro_IsEnabled()) return;

    if (dev != g_lastDev) {
        Hud_Shutdown();
        g_lastDev = dev;
    }
    if (dev->TestCooperativeLevel() != D3D_OK) return;

    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp)) || vp.Width < 320 || vp.Height < 240) return;
    float vpW = (float)vp.Width;
    float vpH = (float)vp.Height;

    MacroCamPod mcam;
    if (!CaptureMacroCameraSEH(&mcam)) return;

    if (!g_stateBlock) {
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_stateBlock)) || !g_stateBlock) {
            return;
        }
    }
    if (FAILED(g_stateBlock->Capture())) return;

    if (SUCCEEDED(dev->BeginScene())) {
        dev->SetVertexShader(NULL);
        dev->SetPixelShader(NULL);
        dev->SetFVF(HUD_FVF);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

        Macro_RenderOnGameThread(dev, &mcam, vpW, vpH);

        dev->EndScene();
    }

    g_stateBlock->Apply();
}
