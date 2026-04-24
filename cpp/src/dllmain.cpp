// AdmiralsPanel-Native — C++ UE4SS companion mod.
// Provides the UFunctions UE4SS-Lua can't safely call directly (complex structs / RPCs).
//
// v0.2 — first real admin actions. Calls UGameplayStatics::ApplyDamage via ProcessEvent
// (server-authoritative; synthesizes a valid FDamageEvent internally).

#include <cmath>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include "http_server.hpp"
#include "ap_app.hpp"
#include "ap_config.hpp"
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/GameplayStatics.hpp>
#include <Unreal/Transform.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/Quat.hpp>
#include <Unreal/Core/Containers/Array.hpp>

using namespace RC;
using namespace RC::Unreal;

// Forward declarations for helpers defined further down (used by lua_lootitems
// and lua_giveitem before their definition).
static std::unordered_set<uintptr_t> gather_live_uobjects();
static std::wstring get_loot_source_name_safe(
    UObject* loot_view,
    const std::unordered_set<uintptr_t>& live);

// ---------------------------------------------------------------------------
// Small string helpers (wide <-> narrow, lossy ASCII-only — fine for names)
// ---------------------------------------------------------------------------

static std::wstring to_wlower(std::string_view s)
{
    std::wstring out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<wchar_t>(std::towlower(static_cast<unsigned char>(c))));
    return out;
}

static std::wstring wlower(std::wstring_view s)
{
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back(static_cast<wchar_t>(std::towlower(c)));
    return out;
}

static std::string w_to_narrow(std::wstring_view s)
{
    std::string out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back(static_cast<char>(c & 0xFF));
    return out;
}

// ---------------------------------------------------------------------------
// Player lookup: scan all UObjects for a PlayerController whose
// PlayerState.PlayerNamePrivate matches (case-insensitive).
// ---------------------------------------------------------------------------

struct PlayerRef
{
    UObject* controller = nullptr;
    UObject* state      = nullptr;
    AActor*  pawn       = nullptr;
    std::wstring name;
};

static PlayerRef find_player_by_name(std::wstring_view target_lower)
{
    PlayerRef out{};

    UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) {
        if (!obj) return LoopAction::Continue;
        auto* cls = obj->GetClassPrivate();
        if (!cls) return LoopAction::Continue;

        // Filter to PlayerController subclasses
        auto cls_name = cls->GetName();
        if (std::wstring_view(cls_name).find(STR("PlayerController")) == std::wstring_view::npos)
            return LoopAction::Continue;

        // Read PlayerState (UObject*)
        UObject* ps = nullptr;
        {
            auto* prop = obj->GetPropertyByNameInChain(STR("PlayerState"));
            if (!prop) return LoopAction::Continue;
            auto** ptr = prop->ContainerPtrToValuePtr<UObject*>(obj);
            if (!ptr || !*ptr) return LoopAction::Continue;
            ps = *ptr;
        }

        // Read PlayerState.PlayerNamePrivate (FString)
        std::wstring name_str;
        {
            auto* prop = ps->GetPropertyByNameInChain(STR("PlayerNamePrivate"));
            if (!prop) return LoopAction::Continue;
            auto* fs = prop->ContainerPtrToValuePtr<FString>(ps);
            if (!fs) return LoopAction::Continue;
            const TCHAR* chars = **fs; // FString::operator* -> TCHAR*
            if (!chars) return LoopAction::Continue;
            name_str = chars;
        }

        if (wlower(name_str) != target_lower) return LoopAction::Continue;

        // Match. Read Pawn.
        AActor* pawn = nullptr;
        {
            auto* prop = obj->GetPropertyByNameInChain(STR("Pawn"));
            if (!prop) return LoopAction::Continue;
            auto** ptr = prop->ContainerPtrToValuePtr<AActor*>(obj);
            if (!ptr || !*ptr) return LoopAction::Continue;
            pawn = *ptr;
        }

        out.controller = obj;
        out.state      = ps;
        out.pawn       = pawn;
        out.name       = name_str;
        return LoopAction::Break;
    });

    return out;
}

// ---------------------------------------------------------------------------
// GAS AttributeSet direct access
// Windrose stores Health/Stamina/etc as FGameplayAttributeData inside
// PlayerState.R5AttributeSet. Writing CurrentValue directly changes the stat.
// ---------------------------------------------------------------------------

struct FGameplayAttributeData
{
    // FGameplayAttributeData has a virtual destructor in the engine, so it
    // carries a vtable pointer at offset 0. Real float fields sit after.
    // Layout: { vtable*, float BaseValue, float CurrentValue }  -> 16 bytes.
    void* _vtable;
    float BaseValue;
    float CurrentValue;
};
static_assert(sizeof(FGameplayAttributeData) == 16, "FGameplayAttributeData must be 16 bytes");

// Find PlayerState.R5AttributeSet from a found player (the main set)
static UObject* get_attribute_set(const PlayerRef& p)
{
    if (!p.state) return nullptr;
    auto* prop = p.state->GetPropertyByNameInChain(STR("R5AttributeSet"));
    if (!prop) return nullptr;
    auto** ptr = prop->ContainerPtrToValuePtr<UObject*>(p.state);
    return (ptr && *ptr) ? *ptr : nullptr;
}

// Walk ALL spawned attribute sets on the ASC to find the one that has attr_name.
// Returns the set (as UObject*) or nullptr.
static UObject* find_set_with_attribute(const PlayerRef& p, const TCHAR* attr_name)
{
    if (!p.state) return nullptr;
    auto* asc_prop = p.state->GetPropertyByNameInChain(STR("R5AbilitySystemComponent"));
    if (!asc_prop) return nullptr;
    auto** asc_ptr = asc_prop->ContainerPtrToValuePtr<UObject*>(p.state);
    if (!asc_ptr || !*asc_ptr) return nullptr;
    UObject* asc = *asc_ptr;
    auto* sa_prop = asc->GetPropertyByNameInChain(STR("SpawnedAttributes"));
    if (!sa_prop) return nullptr;
    auto* arr = sa_prop->ContainerPtrToValuePtr<TArray<UObject*>>(asc);
    if (!arr) return nullptr;
    for (int32 i = 0; i < arr->Num(); ++i) {
        UObject* attrset = (*arr)[i];
        if (!attrset) continue;
        auto* cls = attrset->GetClassPrivate();
        if (!cls) continue;
        if (cls->GetPropertyByNameInChain(attr_name)) return attrset;
    }
    return nullptr;
}

// Read Attribute by name (e.g. "Health", "MaxHealth", "Stamina")
static bool read_attribute(UObject* attrset, const TCHAR* attr_name,
                           float* out_current, float* out_base = nullptr)
{
    if (!attrset) return false;
    auto* prop = attrset->GetPropertyByNameInChain(attr_name);
    if (!prop) return false;
    auto offset = prop->GetOffset_Internal();
    auto* data = reinterpret_cast<FGameplayAttributeData*>(
        reinterpret_cast<uint8_t*>(attrset) + offset);
    if (out_current) *out_current = data->CurrentValue;
    if (out_base)    *out_base    = data->BaseValue;
    return true;
}

// Write CurrentValue (and optionally BaseValue) on an attribute
static bool write_attribute(UObject* attrset, const TCHAR* attr_name,
                            float new_current, bool also_base = true)
{
    if (!attrset) return false;
    auto* prop = attrset->GetPropertyByNameInChain(attr_name);
    if (!prop) return false;
    auto offset = prop->GetOffset_Internal();
    auto* data = reinterpret_cast<FGameplayAttributeData*>(
        reinterpret_cast<uint8_t*>(attrset) + offset);
    data->CurrentValue = new_current;
    if (also_base) data->BaseValue = new_current;
    return true;
}

// ---------------------------------------------------------------------------
// ApplyDamage via UGameplayStatics UFunction (server-authoritative,
// synthesizes FDamageEvent internally)
// ---------------------------------------------------------------------------

struct ApplyDamage_Params
{
    AActor*  DamagedActor      = nullptr; // 0x00
    float    BaseDamage        = 0.0f;    // 0x08 (4 bytes padding before this on 64-bit)
    char     _pad0[4]          = {};
    UObject* EventInstigator   = nullptr; // 0x10 (AController*)
    AActor*  DamageCauser      = nullptr; // 0x18
    UClass*  DamageTypeClass   = nullptr; // 0x20
    float    ReturnValue       = 0.0f;    // 0x28
    char     _pad1[4]          = {};
};
static_assert(sizeof(ApplyDamage_Params) >= 0x30, "ApplyDamage params struct too small");

static UObject*  g_gameplay_statics_cdo = nullptr;
static UFunction* g_apply_damage_fn     = nullptr;

static bool ensure_gameplay_statics()
{
    if (g_gameplay_statics_cdo && g_apply_damage_fn) return true;

    if (!g_gameplay_statics_cdo)
    {
        g_gameplay_statics_cdo = UObjectGlobals::FindObject(
            nullptr, nullptr,
            STR("/Script/Engine.Default__GameplayStatics"));
    }
    if (!g_gameplay_statics_cdo) return false;

    if (!g_apply_damage_fn)
    {
        g_apply_damage_fn = g_gameplay_statics_cdo->GetFunctionByNameInChain(STR("ApplyDamage"));
    }
    return g_apply_damage_fn != nullptr;
}

static bool apply_damage(AActor* target, float amount, float* out_return = nullptr)
{
    if (!target || !ensure_gameplay_statics()) return false;
    ApplyDamage_Params p{};
    p.DamagedActor    = target;
    p.BaseDamage      = amount;
    p.EventInstigator = nullptr;
    p.DamageCauser    = nullptr;
    p.DamageTypeClass = nullptr;
    g_gameplay_statics_cdo->ProcessEvent(g_apply_damage_fn, &p);
    if (out_return) *out_return = p.ReturnValue;
    return true;
}

// ---------------------------------------------------------------------------
// Lua bindings
// ---------------------------------------------------------------------------

// ap_native_version() -> string
static int lua_version(const LuaMadeSimple::Lua& lua)
{
    lua.set_string("0.2.0");
    return 1;
}

// ap_native_find(name) -> string ("found: <pawn fullname>" | "not found")
static int lua_find(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_find(name)"); return 1; }
    auto target = to_wlower(lua.get_string()); // pops stack
    auto found = find_player_by_name(target);
    if (!found.pawn) { lua.set_string("not found"); return 1; }
    auto fn = found.pawn->GetFullName();
    std::string msg = "found: " + w_to_narrow(fn);
    lua.set_string(msg);
    return 1;
}

// ap_native_heal(name, amount) -> string
// Direct AttributeSet write. Heals to min(current+amount, max).
static int lua_heal(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_heal(name, amount)"); return 1; }
    auto target = to_wlower(lua.get_string());
    if (!lua.is_number()) { lua.set_string("usage: ap_native_heal(name, amount)"); return 1; }
    float amt = lua.get_float();

    auto found = find_player_by_name(target);
    if (!found.pawn) { lua.set_string("player not found"); return 1; }
    auto* attrset = get_attribute_set(found);
    if (!attrset) { lua.set_string("AttributeSet not found"); return 1; }

    float hp = 0, max_hp = 0;
    if (!read_attribute(attrset, STR("Health"), &hp) ||
        !read_attribute(attrset, STR("MaxHealth"), &max_hp))
    {
        lua.set_string("Failed to read Health/MaxHealth");
        return 1;
    }
    float new_hp = hp + amt;
    if (new_hp > max_hp) new_hp = max_hp;
    if (new_hp < 0) new_hp = 0;
    bool ok = write_attribute(attrset, STR("Health"), new_hp);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Health %.1f -> %.1f (max %.1f) %s",
                  static_cast<double>(hp), static_cast<double>(new_hp),
                  static_cast<double>(max_hp), ok ? "OK" : "FAIL");
    lua.set_string(std::string(buf));
    return 1;
}

// ap_native_feed(name) -> string. Refills survival-style attributes across
// all spawned attribute sets: Health, Stamina, Comfort (Windrose's "hunger"),
// Posture. Also zeroes Corruption if present. Uses an explicit allowlist so
// we don't accidentally max out debuff meters.
static int lua_feed(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_feed(name)"); return 1; }
    auto target = to_wlower(lua.get_string());
    auto found = find_player_by_name(target);
    if (!found.pawn) { lua.set_string("player not found"); return 1; }
    if (!found.state) { lua.set_string("PlayerState not found"); return 1; }

    // Walk ASC.SpawnedAttributes
    auto* asc_prop = found.state->GetPropertyByNameInChain(STR("R5AbilitySystemComponent"));
    if (!asc_prop) { lua.set_string("ASC prop missing"); return 1; }
    auto** asc_ptr = asc_prop->ContainerPtrToValuePtr<UObject*>(found.state);
    if (!asc_ptr || !*asc_ptr) { lua.set_string("ASC null"); return 1; }
    UObject* asc = *asc_ptr;

    auto* sa_prop = asc->GetPropertyByNameInChain(STR("SpawnedAttributes"));
    if (!sa_prop) { lua.set_string("SpawnedAttributes missing"); return 1; }
    auto* arr = sa_prop->ContainerPtrToValuePtr<TArray<UObject*>>(asc);
    if (!arr) { lua.set_string("SpawnedAttributes null"); return 1; }

    // Attributes we want at MAX after feed (each has a matching "Max" + non-Max
    // pair in Windrose; the non-Max is what we write).
    struct Refill { const TCHAR* cur; const TCHAR* max_name; };
    static const Refill refills[] = {
        { STR("Health"),  STR("MaxHealth")  },
        { STR("Stamina"), STR("MaxStamina") },
        { STR("Posture"), STR("MaxPosture") },
        // Comfort deliberately omitted: the raw attribute doesn't drive
        // Windrose's visible comfort/hunger UI, so refilling it has no effect.
    };
    // Debuff meters we want at ZERO after feed.
    static const TCHAR* zeroOut[] = {
        STR("CorruptionStatus"),
    };

    std::string out;

    for (int32 i = 0; i < arr->Num(); ++i) {
        UObject* attrset = (*arr)[i];
        if (!attrset) continue;

        // Refill pairs
        for (const auto& r : refills) {
            float cur = 0, mx = 0;
            if (read_attribute(attrset, r.cur, &cur) &&
                read_attribute(attrset, r.max_name, &mx) && mx > 0.0f)
            {
                write_attribute(attrset, r.cur, mx);
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s %.1f->%.1f ",
                              w_to_narrow(r.cur).c_str(),
                              static_cast<double>(cur), static_cast<double>(mx));
                out += buf;
            }
        }

        // Zero-out debuffs
        for (const TCHAR* name : zeroOut) {
            float cur = 0;
            if (read_attribute(attrset, name, &cur) && cur > 0.0f) {
                write_attribute(attrset, name, 0.0f);
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s %.1f->0 ",
                              w_to_narrow(name).c_str(),
                              static_cast<double>(cur));
                out += buf;
            }
        }
    }
    if (out.empty()) out = "no refillable attributes found";
    lua.set_string(out);
    return 1;
}

// ap_native_damage(name, amount) -> string. Actually reduces Health via AttributeSet.
static int lua_damage(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_damage(name, amount)"); return 1; }
    auto target = to_wlower(lua.get_string());
    if (!lua.is_number()) { lua.set_string("usage: ap_native_damage(name, amount)"); return 1; }
    float amt = lua.get_float();

    auto found = find_player_by_name(target);
    if (!found.pawn) { lua.set_string("player not found"); return 1; }
    auto* attrset = get_attribute_set(found);
    if (!attrset) { lua.set_string("AttributeSet not found"); return 1; }

    float hp = 0;
    if (!read_attribute(attrset, STR("Health"), &hp)) {
        lua.set_string("Failed to read Health");
        return 1;
    }
    float new_hp = hp - amt;
    if (new_hp < 0) new_hp = 0;
    bool ok = write_attribute(attrset, STR("Health"), new_hp);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Health %.1f -> %.1f (took %.1f damage) %s",
                  static_cast<double>(hp), static_cast<double>(new_hp),
                  static_cast<double>(amt), ok ? "OK" : "FAIL");
    lua.set_string(std::string(buf));
    return 1;
}

// ap_native_revive(name) -> string. Sets Health to MaxHealth (works on dead or alive).
static int lua_revive(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_revive(name)"); return 1; }
    auto target = to_wlower(lua.get_string());
    auto found = find_player_by_name(target);
    if (!found.pawn) { lua.set_string("player not found"); return 1; }
    auto* attrset = get_attribute_set(found);
    if (!attrset) { lua.set_string("AttributeSet not found"); return 1; }

    float hp = 0, mx = 0;
    if (!read_attribute(attrset, STR("Health"), &hp) ||
        !read_attribute(attrset, STR("MaxHealth"), &mx))
    {
        lua.set_string("Failed to read Health/MaxHealth");
        return 1;
    }
    bool ok = write_attribute(attrset, STR("Health"), mx);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Health %.1f -> %.1f (full revive) %s",
                  static_cast<double>(hp), static_cast<double>(mx), ok ? "OK" : "FAIL");
    lua.set_string(std::string(buf));
    return 1;
}

// ap_native_setattr(name, attrname, value) -> string. Generic AttributeSet write.
static int lua_setattr(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_setattr(name, attr, value)"); return 1; }
    auto target = to_wlower(lua.get_string());
    if (!lua.is_string()) { lua.set_string("usage: ap_native_setattr(name, attr, value)"); return 1; }
    auto attr_narrow = std::string(lua.get_string());
    std::wstring attr_wide;
    for (char c : attr_narrow) attr_wide.push_back(static_cast<wchar_t>(c));
    if (!lua.is_number()) { lua.set_string("value must be a number"); return 1; }
    float val = lua.get_float();

    auto found = find_player_by_name(target);
    if (!found.pawn) { lua.set_string("player not found"); return 1; }

    // Walk SpawnedAttributes to find whichever set has this attribute
    UObject* attrset = find_set_with_attribute(found, attr_wide.c_str());
    if (!attrset) {
        lua.set_string("attribute '" + attr_narrow + "' not found on any AttributeSet");
        return 1;
    }

    float before = 0;
    read_attribute(attrset, attr_wide.c_str(), &before);
    bool ok = write_attribute(attrset, attr_wide.c_str(), val);
    char buf[256];
    const char* setname = "?";
    auto* cls = attrset->GetClassPrivate();
    std::string setname_str;
    if (cls) { setname_str = w_to_narrow(cls->GetName()); setname = setname_str.c_str(); }
    std::snprintf(buf, sizeof(buf), "%s [on %s]: %.2f -> %.2f %s",
                  attr_narrow.c_str(), setname,
                  static_cast<double>(before),
                  static_cast<double>(val), ok ? "OK" : "FAIL");
    lua.set_string(std::string(buf));
    return 1;
}

// ap_native_readattr(name, attrname) -> string. Read any attribute's CurrentValue + BaseValue
// across all spawned attribute sets.
static int lua_readattr(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_readattr(name, attr)"); return 1; }
    auto target = to_wlower(lua.get_string());
    if (!lua.is_string()) { lua.set_string("usage: ap_native_readattr(name, attr)"); return 1; }
    auto attr_narrow = std::string(lua.get_string());
    std::wstring attr_wide;
    for (char c : attr_narrow) attr_wide.push_back(static_cast<wchar_t>(c));

    auto found = find_player_by_name(target);
    if (!found.pawn) { lua.set_string("player not found"); return 1; }

    UObject* attrset = find_set_with_attribute(found, attr_wide.c_str());
    if (!attrset) {
        lua.set_string("attribute '" + attr_narrow + "' not found on any AttributeSet");
        return 1;
    }

    float cur = 0, base = 0;
    read_attribute(attrset, attr_wide.c_str(), &cur, &base);
    char buf[256];
    const char* setname = "?";
    auto* cls = attrset->GetClassPrivate();
    std::string setname_str;
    if (cls) { setname_str = w_to_narrow(cls->GetName()); setname = setname_str.c_str(); }
    std::snprintf(buf, sizeof(buf), "%s [on %s]: current=%.2f base=%.2f",
                  attr_narrow.c_str(), setname,
                  static_cast<double>(cur), static_cast<double>(base));
    lua.set_string(std::string(buf));
    return 1;
}

// ap_native_inspect(name) -> detailed property dump of HealthComponent + R5Character
// Used to find what field actually holds health on this game build.
static int lua_inspect(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_inspect(name)"); return 1; }
    auto target = to_wlower(lua.get_string());
    auto found = find_player_by_name(target);
    if (!found.pawn) { lua.set_string("player not found"); return 1; }

    std::string out;
    auto dump_class_props = [&](UObject* obj, const char* label) {
        if (!obj) { out += label; out += ": <null>\n"; return; }
        auto* cls = obj->GetClassPrivate();
        if (!cls) { out += label; out += ": <no class>\n"; return; }
        out += label;
        out += " (class: ";
        out += w_to_narrow(cls->GetName());
        out += "):\n";
        int count = 0;
        for (auto* prop : cls->ForEachPropertyInChain()) {
            if (!prop) continue;
            if (count++ >= 120) { out += "  ...(truncated)\n"; break; }
            auto name = w_to_narrow(prop->GetName());
            auto cpp_type = w_to_narrow(std::wstring_view(*prop->GetCPPType()));
            auto offset = prop->GetOffset_Internal();
            char buf[256];
            std::snprintf(buf, sizeof(buf), "  +0x%04X %-30s %s\n",
                          offset, name.c_str(), cpp_type.c_str());
            out += buf;
        }
    };

    // Dump R5Character's health-related fields
    dump_class_props(found.pawn, "R5Character");

    // Helper: deref a UObject* property then dump its class's properties
    auto dump_child = [&](UObject* container, const TCHAR* prop_name, const char* label) {
        if (!container) { out += label; out += ": <null container>\n"; return; }
        auto* prop = container->GetPropertyByNameInChain(prop_name);
        if (!prop) { out += label; out += ": <no prop>\n"; return; }
        auto** ptr = prop->ContainerPtrToValuePtr<UObject*>(container);
        if (!ptr || !*ptr) { out += label; out += ": <null>\n"; return; }
        dump_class_props(*ptr, label);
    };

    // Dump GAS-relevant components on the pawn
    dump_child(found.pawn, STR("HealthComponent"),              "HealthComponent");
    dump_child(found.pawn, STR("CombatComponent"),              "CombatComponent");
    dump_child(found.pawn, STR("GameplayEffectProxyComponent"), "GameplayEffectProxyComponent");
    dump_child(found.pawn, STR("AbilitySystemParams"),          "AbilitySystemParams");
    // Inventory-adjacent components
    dump_child(found.pawn,  STR("Equipment"),                   "Equipment");
    dump_child(found.pawn,  STR("DefaultEquipment"),            "DefaultEquipment");
    dump_child(found.state, STR("ProximityStorageComponent"),   "ProximityStorageComponent");
    dump_child(found.state, STR("AmmoComponent"),               "AmmoComponent");

    // DropItemsComponent — path B for give-item (drop on ground, player picks up naturally)
    dump_child(found.pawn, STR("DropItemsComponent"), "DropItemsComponent");

    // Scan all UObjects for classes containing "Inventory" to find the storage subsystem
    out += "\nObjects with 'Inventory' in class name (first 30):\n";
    int inv_count = 0;
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) {
        if (!obj || inv_count >= 30) return inv_count >= 30 ? LoopAction::Break : LoopAction::Continue;
        auto* cls = obj->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        auto cname = cls->GetName();
        if (cname.find(STR("Inventory")) == std::wstring::npos &&
            cname.find(STR("ItemSubsystem")) == std::wstring::npos &&
            cname.find(STR("ItemStore")) == std::wstring::npos) return LoopAction::Continue;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  [%d] %s  (class: %s)\n",
                      inv_count,
                      w_to_narrow(obj->GetFullName()).substr(0, 120).c_str(),
                      w_to_narrow(cname).c_str());
        out += buf;
        inv_count++;
        return LoopAction::Continue;
    });

    // PlayerState hosts R5AttributeSet (where Health lives in this GAS game)
    dump_child(found.state, STR("R5AttributeSet"),            "R5AttributeSet");
    dump_child(found.state, STR("R5AbilitySystemComponent"),  "R5AbilitySystemComponent");
    dump_child(found.state, STR("PostureAttributeSet"),       "PostureAttributeSet");
    dump_child(found.state, STR("RangeWeaponAttributeSet"),   "RangeWeaponAttributeSet");

    // Walk ASC.SpawnedAttributes — every attribute set GAS knows about.
    do {
        if (!found.state) break;
        auto* asc_prop = found.state->GetPropertyByNameInChain(STR("R5AbilitySystemComponent"));
        if (!asc_prop) break;
        auto** asc_ptr = asc_prop->ContainerPtrToValuePtr<UObject*>(found.state);
        if (!asc_ptr || !*asc_ptr) break;
        UObject* asc = *asc_ptr;

        auto* sa_prop = asc->GetPropertyByNameInChain(STR("SpawnedAttributes"));
        if (!sa_prop) { out += "SpawnedAttributes: <no prop>\n"; break; }
        auto* arr = sa_prop->ContainerPtrToValuePtr<TArray<UObject*>>(asc);
        if (!arr) { out += "SpawnedAttributes: <null>\n"; break; }

        out += "\nASC.SpawnedAttributes (";
        out += std::to_string(arr->Num());
        out += " set(s)):\n";
        for (int32 i = 0; i < arr->Num(); ++i) {
            UObject* attrset = (*arr)[i];
            if (!attrset) { out += "  [" + std::to_string(i) + "] <null>\n"; continue; }
            char label[64];
            std::snprintf(label, sizeof(label), "SpawnedAttributes[%d]", i);
            dump_class_props(attrset, label);
        }
    } while (false);

    lua.set_string(out);
    return 1;
}

// ap_native_allstats() -> string. JSON-ish map of all online players' key stats.
// Walks PlayerControllers, then each player's ASC.SpawnedAttributes, picks out
// Health/Stamina/Comfort/Posture + maxes. Used for live HP/stam/comfort bars.
static int lua_allstats(const LuaMadeSimple::Lua& lua)
{
    std::string out = "{";
    bool first = true;

    // Attributes we want per player (name on AttributeSet -> output key).
    // Comfort deliberately excluded: the server attribute doesn't drive the
    // in-game comfort / hunger UI (that's derived client-side from inventory /
    // warmth / shelter). Setting Comfort directly has no visible effect.
    static const TCHAR* wanted[] = {
        STR("Health"), STR("MaxHealth"),
        STR("Stamina"), STR("MaxStamina"),
        STR("Posture"), STR("MaxPosture"),
    };

    UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) {
        if (!obj) return LoopAction::Continue;
        auto* cls = obj->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        auto cname = cls->GetName();
        if (std::wstring_view(cname).find(STR("PlayerController")) == std::wstring_view::npos) {
            return LoopAction::Continue;
        }

        // Get PlayerState + name
        UObject* ps = nullptr;
        {
            auto* prop = obj->GetPropertyByNameInChain(STR("PlayerState"));
            if (!prop) return LoopAction::Continue;
            auto** ptr = prop->ContainerPtrToValuePtr<UObject*>(obj);
            if (!ptr || !*ptr) return LoopAction::Continue;
            ps = *ptr;
        }
        std::wstring name_str;
        {
            auto* prop = ps->GetPropertyByNameInChain(STR("PlayerNamePrivate"));
            if (!prop) return LoopAction::Continue;
            auto* fs = prop->ContainerPtrToValuePtr<FString>(ps);
            if (!fs) return LoopAction::Continue;
            const TCHAR* chars = **fs;
            if (!chars) return LoopAction::Continue;
            name_str = chars;
        }
        if (name_str.empty()) return LoopAction::Continue;

        // Walk ASC.SpawnedAttributes to find our attrs
        auto* asc_prop = ps->GetPropertyByNameInChain(STR("R5AbilitySystemComponent"));
        if (!asc_prop) return LoopAction::Continue;
        auto** asc_ptr = asc_prop->ContainerPtrToValuePtr<UObject*>(ps);
        if (!asc_ptr || !*asc_ptr) return LoopAction::Continue;
        UObject* asc = *asc_ptr;
        auto* sa_prop = asc->GetPropertyByNameInChain(STR("SpawnedAttributes"));
        if (!sa_prop) return LoopAction::Continue;
        auto* arr = sa_prop->ContainerPtrToValuePtr<TArray<UObject*>>(asc);
        if (!arr) return LoopAction::Continue;

        if (!first) out += ",";
        first = false;
        out += "\"" + w_to_narrow(name_str) + "\":{";
        bool first_attr = true;

        for (const TCHAR* wanted_name : wanted) {
            float cur = 0;
            bool found = false;
            for (int32 i = 0; i < arr->Num() && !found; ++i) {
                UObject* attrset = (*arr)[i];
                if (!attrset) continue;
                if (read_attribute(attrset, wanted_name, &cur)) {
                    found = true;
                    if (!first_attr) out += ",";
                    first_attr = false;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "\"%s\":%.2f",
                                  w_to_narrow(wanted_name).c_str(),
                                  static_cast<double>(cur));
                    out += buf;
                }
            }
        }
        out += "}";
        return LoopAction::Continue;
    });

    out += "}";
    lua.set_string(out);
    return 1;
}

// ap_native_classprobe(class_short_name) -> string. Locates a UClass by short
// name (tries /Script/R5BusinessRules., /Script/R5., /Script/Engine.) and
// dumps its properties + own UFunctions. Used for give-item reverse-eng.
static int lua_classprobe(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_classprobe(ClassName)"); return 1; }
    auto name_narrow = std::string(lua.get_string());
    std::wstring name_wide;
    for (char c : name_narrow) name_wide.push_back(static_cast<wchar_t>(c));

    static const TCHAR* prefixes[] = {
        STR("/Script/R5BusinessRules."),
        STR("/Script/R5."),
        STR("/Script/R5ActionManager."),
        STR("/Script/R5Core."),
        STR("/Script/R5Gameplay."),
        STR("/Script/GameplayAbilities."),
        STR("/Script/Engine."),
        STR("/Script/CoreUObject."),
    };
    UObject* classObj = nullptr;
    for (const TCHAR* pref : prefixes) {
        std::wstring path = std::wstring(pref) + name_wide;
        classObj = UObjectGlobals::FindObject(nullptr, nullptr, path.c_str());
        if (classObj) break;
    }

    // Fallback: scan all loaded UObjects for any struct-like object with this
    // exact name. Handles classes in packages we didn't guess + BP classes.
    std::string found_via;
    if (!classObj) {
        UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) {
            if (!obj) return LoopAction::Continue;
            auto own_name = obj->GetName();
            if (std::wstring_view(own_name) != name_wide) return LoopAction::Continue;
            auto* meta = obj->GetClassPrivate();
            if (!meta) return LoopAction::Continue;
            auto meta_name = meta->GetName();
            // Only struct-like (Class / ScriptStruct / BlueprintGeneratedClass)
            if (std::wstring_view(meta_name).find(STR("Class")) == std::wstring_view::npos &&
                std::wstring_view(meta_name).find(STR("Struct")) == std::wstring_view::npos) {
                return LoopAction::Continue;
            }
            classObj = obj;
            found_via = " (found via UObject scan)";
            return LoopAction::Break;
        });
    }

    if (!classObj) {
        lua.set_string("Class not found: " + name_narrow + " (tried prefixes + UObject scan)");
        return 1;
    }

    std::string out = "Class '" + name_narrow + "'" + found_via + " = " + w_to_narrow(classObj->GetFullName()) + "\n";
    auto* meta = classObj->GetClassPrivate();
    if (meta) out += "meta-class: " + w_to_narrow(meta->GetName()) + "\n";

    // foundObj IS a UClass (we looked up /Script/.../ClassName). Cast to UStruct.
    auto* asStruct = reinterpret_cast<RC::Unreal::UStruct*>(classObj);

    // Properties — these are the FIELDS instances of this class carry.
    // For a Rule class, these are the params you'd set before applying.
    out += "\nProperties (own + inherited):\n";
    int pcount = 0;
    for (auto* prop : asStruct->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (pcount++ >= 80) { out += "  ...(truncated)\n"; break; }
        auto pname = w_to_narrow(prop->GetName());
        auto ptype = w_to_narrow(std::wstring_view(*prop->GetCPPType()));
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  +0x%04X %-36s %s\n",
                      prop->GetOffset_Internal(), pname.c_str(), ptype.c_str());
        out += buf;
    }
    out += "  [props total: " + std::to_string(pcount) + "]\n";

    lua.set_string(out);
    return 1;
}

// ap_native_scan(substring) -> string. Scans all UObjects for objects whose
// own name OR class-name contains the substring. Matches class definitions
// (UClass objects whose GetName() is the class name) AND instances (whose
// class's GetName() matches). Used for reverse-engineering.
static int lua_scan(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_scan(substring)"); return 1; }
    auto needle_narrow = std::string(lua.get_string());
    std::wstring needle;
    for (char c : needle_narrow) needle.push_back(static_cast<wchar_t>(c));

    std::string out = "UObjects whose own-name or class-name contains '" + needle_narrow + "' (first 80):\n";
    int count = 0;
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) {
        if (!obj) return LoopAction::Continue;
        if (count >= 80) return LoopAction::Break;
        auto own_name = obj->GetName();
        auto* cls = obj->GetClassPrivate();
        auto cname = cls ? cls->GetName() : std::wstring(STR(""));
        bool own_match = std::wstring_view(own_name).find(needle) != std::wstring_view::npos;
        bool cls_match = std::wstring_view(cname).find(needle) != std::wstring_view::npos;
        if (!own_match && !cls_match) return LoopAction::Continue;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  [%d] %s  (class: %s)\n",
                      count,
                      w_to_narrow(obj->GetFullName()).substr(0, 140).c_str(),
                      w_to_narrow(cname).c_str());
        out += buf;
        count++;
        return LoopAction::Continue;
    });
    out += "  [total matched: " + std::to_string(count) + "]\n";
    lua.set_string(out);
    return 1;
}

// ap_native_scanpath(substring) -> string. Scans all UObjects for those whose
// FullName (e.g. "BlueprintGeneratedClass /Game/Items/Food/BP_Bread.BP_Bread_C")
// contains the substring. Use this to hunt for asset paths under /Game/.
static int lua_scanpath(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_scanpath(substring)"); return 1; }
    auto needle_narrow = std::string(lua.get_string());
    std::wstring needle = to_wlower(needle_narrow);

    std::string out = "UObjects whose full-path contains '" + needle_narrow + "' (first 80):\n";
    int count = 0;
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) {
        if (!obj) return LoopAction::Continue;
        if (count >= 80) return LoopAction::Break;
        auto full = obj->GetFullName();
        if (wlower(full).find(needle) == std::wstring::npos) {
            return LoopAction::Continue;
        }
        auto* cls = obj->GetClassPrivate();
        auto cname = cls ? cls->GetName() : std::wstring(STR(""));
        char buf[320];
        std::snprintf(buf, sizeof(buf), "  [%d] %s  (class: %s)\n",
                      count,
                      w_to_narrow(full).substr(0, 200).c_str(),
                      w_to_narrow(cname).c_str());
        out += buf;
        count++;
        return LoopAction::Continue;
    });
    out += "  [total matched: " + std::to_string(count) + "]\n";
    lua.set_string(out);
    return 1;
}

// ap_native_rawdump(target, [bytes]) -> hex/ascii dump of UObject memory.
// target is either:
//   - a class short name (e.g. "R5BLInventorySlotView") — finds first LIVE instance (skips CDO)
//   - a full path starting with "/" (e.g. "/Engine/Transient.XYZ") — exact FindObject
// bytes defaults to 256, clamped to [16, 4096].
//
// Used to reverse-engineer non-reflected C++ field layouts on classes that
// have zero UPROPERTY fields (R5BLInventorySlotView, R5BLInventoryView, etc).
static int lua_rawdump(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_rawdump(target, [bytes])"); return 1; }
    auto target_narrow = std::string(lua.get_string());
    std::wstring target_wide;
    for (char c : target_narrow) target_wide.push_back(static_cast<wchar_t>(c));

    int bytes = 256;
    if (lua.is_number()) {
        bytes = static_cast<int>(lua.get_float());
        if (bytes < 16)   bytes = 16;
        if (bytes > 4096) bytes = 4096;
    }

    UObject* obj = nullptr;
    std::string how;

    if (!target_narrow.empty() && target_narrow[0] == '/') {
        obj = UObjectGlobals::FindObject(nullptr, nullptr, target_wide.c_str());
        how = "FindObject by full path";
    } else {
        // Find first live instance (not Default__) of a class with matching name
        UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
            if (!cur || obj) return obj ? LoopAction::Break : LoopAction::Continue;
            auto* cls = cur->GetClassPrivate();
            if (!cls) return LoopAction::Continue;
            if (std::wstring_view(cls->GetName()) != target_wide) return LoopAction::Continue;
            auto full = cur->GetFullName();
            if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
            obj = cur;
            return LoopAction::Break;
        });
        how = "first live instance of class";
    }

    if (!obj) {
        lua.set_string("object not found: " + target_narrow + " (" + how + ")");
        return 1;
    }

    std::string out;
    auto fullp = w_to_narrow(obj->GetFullName());
    out += "obj: " + fullp.substr(0, 200) + "\n";
    auto* cls = obj->GetClassPrivate();
    if (cls) {
        out += "class: " + w_to_narrow(cls->GetName()) + "\n";
    }
    out += "addr: ";
    {
        char buf[32]; std::snprintf(buf, sizeof(buf), "0x%p", (void*)obj); out += buf;
    }
    out += "\ndump: " + std::to_string(bytes) + " bytes\n\n";

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(obj);
    for (int i = 0; i < bytes; i += 16) {
        char line[256];
        int pos = std::snprintf(line, sizeof(line), "%04x: ", i);
        for (int j = 0; j < 16; j++) {
            if (i + j < bytes)
                pos += std::snprintf(line + pos, sizeof(line) - pos, "%02x ", ptr[i + j]);
            else
                pos += std::snprintf(line + pos, sizeof(line) - pos, "   ");
        }
        pos += std::snprintf(line + pos, sizeof(line) - pos, " |");
        for (int j = 0; j < 16 && (i + j) < bytes; j++) {
            char c = static_cast<char>(ptr[i + j]);
            char out_c = (c >= 32 && c < 127) ? c : '.';
            pos += std::snprintf(line + pos, sizeof(line) - pos, "%c", out_c);
        }
        pos += std::snprintf(line + pos, sizeof(line) - pos, "|\n");
        out += line;
    }

    lua.set_string(out);
    return 1;
}

// ap_native_dumpclass(classname, N) -> list the first N live instances of a class
// with their full paths, so we can then rawdump a specific one.
static int lua_dumpclass(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_dumpclass(classname, [N])"); return 1; }
    auto cls_narrow = std::string(lua.get_string());
    std::wstring cls_wide;
    for (char c : cls_narrow) cls_wide.push_back(static_cast<wchar_t>(c));

    int N = 10;
    if (lua.is_number()) {
        N = static_cast<int>(lua.get_float());
        if (N < 1) N = 1;
        if (N > 200) N = 200;
    }

    std::string out = "Live instances of '" + cls_narrow + "' (first " + std::to_string(N) + "):\n";
    int count = 0;
    UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
        if (!cur || count >= N) return count >= N ? LoopAction::Break : LoopAction::Continue;
        auto* cls = cur->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        if (std::wstring_view(cls->GetName()) != cls_wide) return LoopAction::Continue;
        auto full = cur->GetFullName();
        if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
        char buf[320];
        std::snprintf(buf, sizeof(buf), "  [%d] @0x%p  %s\n",
                      count, (void*)cur,
                      w_to_narrow(full).substr(0, 220).c_str());
        out += buf;
        count++;
        return LoopAction::Continue;
    });
    out += "  [total: " + std::to_string(count) + "]\n";
    lua.set_string(out);
    return 1;
}

// ---------------------------------------------------------------------------
// Actor spawning via UGameplayStatics (give-item path B: spawn a pickup
// actor at the player's feet; the player's R5Ability_Loot_AutoPickup magnets
// it into their inventory on proximity).
// ---------------------------------------------------------------------------

// Read actor location via K2_GetActorLocation UFunction.
static FVector get_actor_location(AActor* actor)
{
    FVector loc{};
    if (!actor) return loc;
    UFunction* fn = actor->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
    if (!fn) return loc;
    struct { FVector ReturnValue; } params{};
    actor->ProcessEvent(fn, &params);
    return params.ReturnValue;
}

// ap_native_loc(player) -> string. Returns player's K2_GetActorLocation. Use
// this to verify the location primitive works on its own before spawning.
static int lua_loc(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_loc(player)"); return 1; }
    auto target = to_wlower(lua.get_string());
    auto p = find_player_by_name(target);
    if (!p.pawn) { lua.set_string("player not found"); return 1; }
    UFunction* fn = p.pawn->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
    if (!fn) { lua.set_string("K2_GetActorLocation not found on pawn"); return 1; }
    FVector v{};
    struct { FVector ReturnValue; } params{};
    p.pawn->ProcessEvent(fn, &params);
    v = params.ReturnValue;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(%.1f, %.1f, %.1f)",
                  static_cast<double>(v.X()),
                  static_cast<double>(v.Y()),
                  static_cast<double>(v.Z()));
    lua.set_string(std::string(buf));
    return 1;
}

// ap_native_funcparams(funcpath) -> dump a UFunction's parameter layout
// (property name, offset, size, type). Works for any reflected UFunction.
static int lua_funcparams(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_funcparams(path)"); return 1; }
    auto p_narrow = std::string(lua.get_string());
    std::wstring p_wide;
    for (char c : p_narrow) p_wide.push_back(static_cast<wchar_t>(c));
    UObject* obj = UObjectGlobals::FindObject(nullptr, nullptr, p_wide.c_str());
    if (!obj) { lua.set_string("function not found: " + p_narrow); return 1; }
    auto* meta = obj->GetClassPrivate();
    if (!meta || std::wstring_view(meta->GetName()) != STR("Function")) {
        lua.set_string("not a UFunction (meta-class: " + (meta ? w_to_narrow(meta->GetName()) : std::string("null")) + ")");
        return 1;
    }
    auto* fn = reinterpret_cast<UFunction*>(obj);
    std::string out = "function: " + w_to_narrow(fn->GetName()) + "\n";
    out += "PropertiesSize: " + std::to_string(fn->GetPropertiesSize()) + " bytes\n";
    auto* asStruct = reinterpret_cast<RC::Unreal::UStruct*>(fn);
    out += "\nProperties:\n";
    int i = 0;
    for (auto* prop : asStruct->ForEachPropertyInChain()) {
        if (!prop) continue;
        if (i++ >= 30) { out += "  ...(truncated)\n"; break; }
        auto name = w_to_narrow(prop->GetName());
        auto cpp = w_to_narrow(std::wstring_view(*prop->GetCPPType()));
        auto offset = prop->GetOffset_Internal();
        auto size = prop->GetSize();
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  +0x%04X  size=0x%04X  %-28s  %s\n",
                      offset, size, name.c_str(), cpp.c_str());
        out += buf;
    }
    lua.set_string(out);
    return 1;
}

// ap_native_yankactor(player, path) -> teleport any actor (by full path) to player.
// Use to test game mechanics (e.g. yank a populated R5LootActor to the player and
// see if auto-pickup triggers).
static int lua_yankactor(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_yankactor(player, path)"); return 1; }
    auto target = to_wlower(lua.get_string());
    if (!lua.is_string()) { lua.set_string("path required"); return 1; }
    auto path_narrow = std::string(lua.get_string());
    std::wstring path_wide;
    for (char c : path_narrow) path_wide.push_back(static_cast<wchar_t>(c));

    auto player = find_player_by_name(target);
    if (!player.pawn) { lua.set_string("player not found"); return 1; }

    UObject* found = UObjectGlobals::FindObject(nullptr, nullptr, path_wide.c_str());
    if (!found) { lua.set_string("actor not found at path"); return 1; }
    AActor* target_actor = reinterpret_cast<AActor*>(found);

    // Read player location
    FVector loc{};
    {
        UFunction* fn = player.pawn->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
        if (!fn) { lua.set_string("K2_GetActorLocation missing"); return 1; }
        struct { FVector ReturnValue; } p{};
        player.pawn->ProcessEvent(fn, &p);
        loc = p.ReturnValue;
    }

    // Teleport via K2_TeleportTo(FVector, FRotator) — signature: bool Teleport(FVector dest, FRotator rot)
    UFunction* tp_fn = target_actor->GetFunctionByNameInChain(STR("K2_TeleportTo"));
    if (!tp_fn) {
        // Fall back to K2_SetActorLocation
        tp_fn = target_actor->GetFunctionByNameInChain(STR("K2_SetActorLocation"));
        if (!tp_fn) { lua.set_string("neither K2_TeleportTo nor K2_SetActorLocation on target"); return 1; }
    }

    // Build params: FVector DestLocation at +0x00 (24 bytes), FRotator at +0x18,
    // then bools for sweep/teleport, then ReturnValue. Use oversized buffer.
    std::vector<uint8_t> params(0x80, 0);
    std::memcpy(params.data() + 0x00, &loc, sizeof(FVector));
    // K2_TeleportTo bool flags — 0 default is fine
    target_actor->ProcessEvent(tp_fn, params.data());

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "yanked %s to (%.1f, %.1f, %.1f) via %s",
                  path_narrow.substr(0, 120).c_str(),
                  static_cast<double>(loc.X()),
                  static_cast<double>(loc.Y()),
                  static_cast<double>(loc.Z()),
                  w_to_narrow(tp_fn->GetName()).c_str());
    lua.set_string(std::string(buf));
    return 1;
}

// ap_native_giveloot(player, [count]) -> teleport N random R5LootActors to player.
// Each actor auto-picks-up on proximity via the player's R5Ability_Loot_AutoPickup.
// Delivers whatever items are already populated on those loot actors.
static int lua_giveloot(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_giveloot(player, [count])"); return 1; }
    auto target = to_wlower(lua.get_string());
    int count = 1;
    if (lua.is_number()) {
        count = static_cast<int>(lua.get_float());
        if (count < 1) count = 1;
        if (count > 20) count = 20;
    }
    auto player = find_player_by_name(target);
    if (!player.pawn) { lua.set_string("player not found"); return 1; }

    FVector loc{};
    {
        UFunction* fn = player.pawn->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
        if (!fn) { lua.set_string("K2_GetActorLocation missing"); return 1; }
        struct { FVector ReturnValue; } p{};
        player.pawn->ProcessEvent(fn, &p);
        loc = p.ReturnValue;
    }

    // Collect all R5LootActor instances that have a non-null LootView (populated),
    // skipping the CDO. Empty loot actors (LootView=null) won't deliver anything.
    std::vector<AActor*> populated;
    int total_seen = 0, empty_count = 0;
    UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
        if (!cur) return LoopAction::Continue;
        auto* cls = cur->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        if (std::wstring_view(cls->GetName()) != STR("R5LootActor")) return LoopAction::Continue;
        auto full = cur->GetFullName();
        if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
        total_seen++;
        // Check LootView at +0x310 (verified via rawdump on an existing populated actor)
        UObject* loot_view = *reinterpret_cast<UObject**>(
            reinterpret_cast<uint8_t*>(cur) + 0x310);
        if (!loot_view) { empty_count++; return LoopAction::Continue; }
        populated.push_back(reinterpret_cast<AActor*>(cur));
        return LoopAction::Continue;
    });

    if (populated.empty()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "found %d R5LootActor(s) but all are empty (LootView=null). "
                      "Kill mobs or break resource nodes to populate world loot first.",
                      total_seen);
        lua.set_string(std::string(buf));
        return 1;
    }

    // Teleport up to `count` populated ones
    int to_take = static_cast<int>(populated.size());
    if (to_take > count) to_take = count;

    int teleported = 0;
    FVector last_post{};
    double last_dist2 = -1;
    for (int i = 0; i < to_take; ++i) {
        AActor* a = populated[i];
        UFunction* tp_fn = a->GetFunctionByNameInChain(STR("K2_TeleportTo"));
        if (!tp_fn) tp_fn = a->GetFunctionByNameInChain(STR("K2_SetActorLocation"));
        if (!tp_fn) continue;
        // Offset loot around player's feet (Z-100 from capsule center),
        // spread horizontally so multiple drops don't stack at the same point.
        double angle = (double)i * 1.0471975512; // ~60° spacing
        double dx = 80.0 * std::cos(angle);
        double dy = 80.0 * std::sin(angle);
        FVector drop(loc.X() + dx, loc.Y() + dy, loc.Z() - 80.0);
        std::vector<uint8_t> params(0x80, 0);
        std::memcpy(params.data() + 0x00, &drop, sizeof(FVector));
        a->ProcessEvent(tp_fn, params.data());
        teleported++;

        // Diagnostic: read actor's location post-teleport
        UFunction* loc_fn2 = a->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
        if (loc_fn2) {
            struct { FVector ReturnValue; } p2{};
            a->ProcessEvent(loc_fn2, &p2);
            last_post = p2.ReturnValue;
            double dx = last_post.X() - loc.X();
            double dy = last_post.Y() - loc.Y();
            double dz = last_post.Z() - loc.Z();
            last_dist2 = dx*dx + dy*dy + dz*dz;
        }
    }

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "world has %d loot actor(s): %d populated, %d empty. "
                  "Teleported %d to you. Player at (%.1f,%.1f,%.1f). "
                  "Last actor post-tp at (%.1f,%.1f,%.1f), dist2=%.1f.",
                  total_seen, (int)populated.size(), empty_count, teleported,
                  loc.X(), loc.Y(), loc.Z(),
                  last_post.X(), last_post.Y(), last_post.Z(),
                  last_dist2);
    lua.set_string(std::string(buf));
    return 1;
}

// ap_native_lootinspect([bytes]) -> find first populated R5LootActor and dump its
// LootView raw memory. Used for reverse-engineering the DropView's non-reflected
// inventory structure (where item stacks actually live in memory).
static int lua_lootinspect(const LuaMadeSimple::Lua& lua)
{
    int bytes = 512;
    if (lua.is_number()) {
        bytes = static_cast<int>(lua.get_float());
        if (bytes < 64) bytes = 64;
        if (bytes > 4096) bytes = 4096;
    }

    AActor* found_actor = nullptr;
    UObject* loot_view = nullptr;
    UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
        if (!cur || found_actor) return found_actor ? LoopAction::Break : LoopAction::Continue;
        auto* cls = cur->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        if (std::wstring_view(cls->GetName()) != STR("R5LootActor")) return LoopAction::Continue;
        auto full = cur->GetFullName();
        if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
        UObject* lv = *reinterpret_cast<UObject**>(
            reinterpret_cast<uint8_t*>(cur) + 0x310);
        if (!lv) return LoopAction::Continue;
        found_actor = reinterpret_cast<AActor*>(cur);
        loot_view = lv;
        return LoopAction::Break;
    });

    if (!found_actor) {
        lua.set_string("no populated R5LootActor found in world");
        return 1;
    }

    std::string out;
    {
        char b[160];
        std::snprintf(b, sizeof(b), "actor: %s\n",
                      w_to_narrow(found_actor->GetFullName()).substr(0, 140).c_str());
        out += b;
        std::snprintf(b, sizeof(b), "loot_view addr: 0x%p\n", (void*)loot_view);
        out += b;
        auto* lv_cls = loot_view->GetClassPrivate();
        if (lv_cls) {
            std::snprintf(b, sizeof(b), "loot_view class: %s\n",
                          w_to_narrow(lv_cls->GetName()).c_str());
            out += b;
        }
    }
    out += "\nLootView memory dump:\n";

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(loot_view);
    for (int i = 0; i < bytes; i += 16) {
        char line[256];
        int pos = std::snprintf(line, sizeof(line), "%04x: ", i);
        for (int j = 0; j < 16; j++) {
            if (i + j < bytes)
                pos += std::snprintf(line + pos, sizeof(line) - pos, "%02x ", ptr[i + j]);
            else
                pos += std::snprintf(line + pos, sizeof(line) - pos, "   ");
        }
        pos += std::snprintf(line + pos, sizeof(line) - pos, " |");
        for (int j = 0; j < 16 && (i + j) < bytes; j++) {
            char c = static_cast<char>(ptr[i + j]);
            char out_c = (c >= 32 && c < 127) ? c : '.';
            pos += std::snprintf(line + pos, sizeof(line) - pos, "%c", out_c);
        }
        pos += std::snprintf(line + pos, sizeof(line) - pos, "|\n");
        out += line;
    }

    lua.set_string(out);
    return 1;
}

// ap_native_lootlist() -> list populated R5LootActors with their paths + LootView addr.
// Helps the specific-item-give workflow: first list what's available, then yank a specific one.
static int lua_lootlist(const LuaMadeSimple::Lua& lua)
{
    int N = 20;
    if (lua.is_number()) {
        N = static_cast<int>(lua.get_float());
        if (N < 1) N = 1;
        if (N > 100) N = 100;
    }
    std::string out = "Populated R5LootActors (LootView != null):\n";
    int shown = 0, empty = 0;
    UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
        if (!cur || shown >= N) return shown >= N ? LoopAction::Break : LoopAction::Continue;
        auto* cls = cur->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        if (std::wstring_view(cls->GetName()) != STR("R5LootActor")) return LoopAction::Continue;
        auto full = cur->GetFullName();
        if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
        UObject* lv = *reinterpret_cast<UObject**>(
            reinterpret_cast<uint8_t*>(cur) + 0x310);
        if (!lv) { empty++; return LoopAction::Continue; }
        char buf[320];
        std::snprintf(buf, sizeof(buf), "  [%d] actor=0x%p  lv=0x%p  %s\n",
                      shown, (void*)cur, (void*)lv,
                      w_to_narrow(full).substr(0, 180).c_str());
        out += buf;
        shown++;
        return LoopAction::Continue;
    });
    out += "  [populated shown: " + std::to_string(shown) +
           ", empty skipped: " + std::to_string(empty) + "]\n";
    lua.set_string(out);
    return 1;
}

// ---------------------------------------------------------------------------
// Specific-item give (v0.5) — identify items referenced by a populated
// R5LootActor so admins can target a specific item (e.g. "banana") rather
// than taking whatever random loot happens to be on the nearest populated
// actor.
//
// Approach: UR5BLInventoryItem data assets are all pre-loaded in the UObject
// table (300+ assets under /R5BusinessRules/InventoryItems/ plus /Tests/).
// The slot data inside a DropView is non-reflected, but it *does* reference
// the item data asset somewhere — either as a cached UObject* or via a
// TSoftObjectPtr whose FWeakObjectPtr.ObjectIndex matches the data asset's
// UObject::GetInternalIndex(). We scan the LootView's first ~4KB of memory
// for either:
//   (a) an 8-byte-aligned pointer value matching a known data-asset address
//   (b) a 4-byte-aligned int32 matching a known data-asset InternalIndex
//
// Caveats: identifies by the FIRST item referenced; a loot actor holding
// a mixed stack (rare) would only surface one. FName-table hash matches
// are possible but noisier — we avoid them to keep false positives low.
// ---------------------------------------------------------------------------

struct InvItemEntry
{
    UObject*      obj;          // data asset address (stable for process lifetime)
    int32         index;        // UObject::GetInternalIndex()
    std::wstring  short_name;   // lowercased — for user-search substring
    std::wstring  display_name; // original — for reporting
};

static std::vector<InvItemEntry> gather_all_items()
{
    std::vector<InvItemEntry> out;
    out.reserve(512);
    UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
        if (!cur) return LoopAction::Continue;
        auto* cls = cur->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        if (std::wstring_view(cls->GetName()) != STR("R5BLInventoryItem")) return LoopAction::Continue;
        auto full = cur->GetFullName();
        if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
        InvItemEntry e;
        e.obj = cur;
        e.index = cur->GetInternalIndex();
        std::wstring nm(cur->GetName());
        e.display_name = nm;
        e.short_name = wlower(nm);
        out.push_back(std::move(e));
        return LoopAction::Continue;
    });
    return out;
}

// Heuristic: pointer looks like a code pointer (vtable). Windows x64 Shipping
// game code typically loads in the [0x00007FF0_00000000 .. 0x00007FFF_FFFFFFFF]
// range. Heap pointers we've observed sit much lower (0x0000019D..., etc).
static bool is_code_like_ptr(uintptr_t v)
{
    return v >= 0x00007FF000000000ull && v <= 0x00007FFFFFFFFFFFull;
}

// Check whether `len` bytes starting at `p` are safely readable in the current
// process. Uses VirtualQuery to avoid SEGV on arbitrary user pointers we pull
// from a memory scan.
static bool is_readable(const void* p, size_t len)
{
    if (!p || len == 0) return false;
    const uint8_t* cur = reinterpret_cast<const uint8_t*>(p);
    const uint8_t* end = cur + len;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        DWORD prot = mbi.Protect;
        if (prot & PAGE_GUARD) return false;
        if (prot & PAGE_NOACCESS) return false;
        const bool readable = (prot & (PAGE_READONLY | PAGE_READWRITE |
                                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                        PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (!readable) return false;
        const uint8_t* region_end = reinterpret_cast<const uint8_t*>(mbi.BaseAddress)
                                  + mbi.RegionSize;
        cur = (region_end > cur) ? region_end : (cur + 4096);
    }
    return true;
}

// Core scanner. Matches UR5BLInventoryItem refs by:
//   (a) 8-byte-aligned pointer value
//   (b) 8-byte-aligned FWeakObjectPtr {int32 InternalIndex, int32 SerialNumber}
//       where the serial is a plausible nonzero small int (cuts false positives
//       from e.g. stack counts that randomly equal a low item's InternalIndex).
// Results appended (deduped) to out_hits.
static void scan_region(
    const uint8_t*                                          bytes,
    size_t                                                  region_bytes,
    const std::unordered_map<uintptr_t, const InvItemEntry*>& by_ptr,
    const std::unordered_map<int32,     const InvItemEntry*>& by_idx,
    std::vector<const InvItemEntry*>&                       out_hits,
    size_t                                                  max_hits)
{
    auto already_have = [&](const InvItemEntry* e) {
        for (auto* h : out_hits) if (h == e) return true;
        return false;
    };

    // Pointer scan (8-byte aligned).
    for (size_t i = 0; i + 8 <= region_bytes; i += 8) {
        uintptr_t v = *reinterpret_cast<const uintptr_t*>(bytes + i);
        if (v < 0x10000) continue;
        auto it = by_ptr.find(v);
        if (it != by_ptr.end() && !already_have(it->second)) {
            out_hits.push_back(it->second);
            if (out_hits.size() >= max_hits) return;
        }
    }

    // FWeakObjectPtr pair scan: aligned {InternalIndex, SerialNumber}.
    // Require serial > 0 and < 0x10000000 to rule out coincidental ints.
    for (size_t i = 0; i + 8 <= region_bytes; i += 8) {
        int32 idx    = *reinterpret_cast<const int32*>(bytes + i);
        int32 serial = *reinterpret_cast<const int32*>(bytes + i + 4);
        if (idx <= 0 || idx > 0x00FFFFFF) continue;
        if (serial <= 0 || serial > 0x10000000) continue;
        auto it = by_idx.find(idx);
        if (it != by_idx.end() && !already_have(it->second)) {
            out_hits.push_back(it->second);
            if (out_hits.size() >= max_hits) return;
        }
    }
}

// Scan an object's memory for references to known UR5BLInventoryItem data
// assets, following 1 level of plausible heap pointers. The DropView's slot
// pointers live at ~+0x180 and target 0x60-byte slot structures elsewhere on
// the heap (~MB offsets); the pointer-chase pass follows each plausible
// pointer we find into its target region and scans 0x100 bytes there too.
static void scan_for_item_refs(
    const void*                               region_start,
    size_t                                    region_bytes,
    const std::vector<InvItemEntry>&          items,
    std::vector<const InvItemEntry*>&         out_hits,
    size_t                                    max_hits = 8)
{
    if (!region_start || region_bytes == 0 || items.empty()) return;

    std::unordered_map<uintptr_t, const InvItemEntry*> by_ptr;
    std::unordered_map<int32,     const InvItemEntry*> by_idx;
    by_ptr.reserve(items.size() * 2);
    by_idx.reserve(items.size() * 2);
    for (const auto& e : items) {
        by_ptr[reinterpret_cast<uintptr_t>(e.obj)] = &e;
        by_idx[e.index] = &e;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(region_start);

    // Pass 1: scan the region itself.
    scan_region(bytes, region_bytes, by_ptr, by_idx, out_hits, max_hits);
    if (out_hits.size() >= max_hits) return;

    // Pass 2+3: chase pointers into valid heap regions and scan a 0x200-byte
    // window at each, two levels deep. Only follow pointers in the same heap
    // arena as the region we're scanning (top 24 bits match). Bounded total
    // work so we don't hang on a bad scan target.
    uintptr_t region_arena = reinterpret_cast<uintptr_t>(region_start) >> 40;
    std::unordered_set<uintptr_t> already_chased;
    int chased = 0;

    auto chase_one_level = [&](const uint8_t* src_bytes, size_t src_len) {
        for (size_t i = 0; i + 8 <= src_len && chased < 128; i += 8) {
            uintptr_t v = *reinterpret_cast<const uintptr_t*>(src_bytes + i);
            if ((v >> 40) != region_arena) continue;
            if ((v & 7) != 0) continue;
            if (!already_chased.insert(v).second) continue;
            const void* p = reinterpret_cast<const void*>(v);
            if (!is_readable(p, 0x200)) continue;
            scan_region(reinterpret_cast<const uint8_t*>(p), 0x200,
                        by_ptr, by_idx, out_hits, max_hits);
            ++chased;
            if (out_hits.size() >= max_hits) return;
        }
    };

    // Level 1: from the original region.
    chase_one_level(bytes, region_bytes);
    if (out_hits.size() >= max_hits) return;

    // Level 2: from every region we reached in level 1.
    std::vector<uintptr_t> level1(already_chased.begin(), already_chased.end());
    for (uintptr_t p : level1) {
        if (chased >= 128) break;
        if (out_hits.size() >= max_hits) return;
        const void* pp = reinterpret_cast<const void*>(p);
        if (!is_readable(pp, 0x200)) continue;
        chase_one_level(reinterpret_cast<const uint8_t*>(pp), 0x200);
    }
}

// Given a populated R5LootActor, resolve the short source-actor name — the BP
// class the loot came from (e.g. "BP_Segment_Coast_Jungle_Ficus_1800cm_C").
// LootView+0x180 is an array of pointers to StaticMeshComponents of the
// source actor; that component's Outer is the source actor itself.
// Returns empty string if we can't resolve.
// Generic container classes — skip when picking a "source" identifier.
static bool is_generic_container_class(std::wstring_view name)
{
    static const std::wstring_view kGeneric[] = {
        // UE core types (meta-classes, containers).
        STR("World"), STR("Level"), STR("Package"),
        STR("LevelStreamingDynamic"),
        STR("Class"), STR("Function"), STR("Struct"),
        STR("BlueprintGeneratedClass"), STR("ScriptStruct"), STR("Enum"),
        STR("GameEngine"), STR("GameInstance"),
        STR("Object"), STR("UserDefinedEnum"), STR("UserDefinedStruct"),
        // R5 / Windrose subsystems.
        STR("R5BLIslandView"),
        STR("R5GameplayOrchestrator"),
        STR("R5GOS_GameplaySpawners"),
        STR("R5DataCacheUe"),
        STR("R5DataKeeperForServerCoop"),
        STR("R5DataKeeperForServer_Account"),
        STR("R5GameInstance"),
        STR("R5GameMode"),
        STR("R5BusinessActionManager"),
        STR("R5ActionManager"),
        STR("R5Environment"),
    };
    for (auto g : kGeneric) if (name == g) return true;
    // Prefix skips.
    if (name.size() > 6 && name.substr(0, 6) == STR("R5GOS_")) return true;
    if (name.size() > 7 && name.substr(0, 7) == STR("Default")) return true;
    return false;
}

// Safely read one object's class name. Single hop only. Returns empty on any
// failed validation — no deep walking (each hop compounds crash risk).
static std::wstring safe_class_name(UObject* obj, uintptr_t arena)
{
    if (!obj || !is_readable(obj, 0x30)) return L"";
    uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
    if (!is_code_like_ptr(vt)) return L"";
    uintptr_t cls_raw = *reinterpret_cast<const uintptr_t*>(
        reinterpret_cast<const uint8_t*>(obj) + 0x10);
    bool cls_loc = ((cls_raw >> 40) == arena) || is_code_like_ptr(cls_raw);
    if (!cls_loc) return L"";
    if (!is_readable(reinterpret_cast<const void*>(cls_raw), 0x30)) return L"";
    if (!is_code_like_ptr(*reinterpret_cast<const uintptr_t*>(cls_raw))) return L"";
    auto* cls = obj->GetClassPrivate();
    if (!cls) return L"";
    return std::wstring(cls->GetName());
}

static std::wstring get_loot_source_name(UObject* loot_view)
{
    if (!loot_view || !is_readable(loot_view, 0x200)) return L"";
    uintptr_t lv_arena = reinterpret_cast<uintptr_t>(loot_view) >> 40;
    const uint8_t* lv_bytes = reinterpret_cast<const uint8_t*>(loot_view);

    // Pass 1: scan +0x28..+0x2C0 for a candidate pointer. For each, try the
    // candidate's Outer (safe single hop); if the Outer class is specific,
    // use it. Skip generic containers.
    for (size_t off = 0x28; off + 8 <= 0x2C0; off += 8) {
        uintptr_t v = *reinterpret_cast<const uintptr_t*>(lv_bytes + off);
        if ((v >> 40) != lv_arena) continue;
        if ((v & 7) != 0) continue;
        const void* p = reinterpret_cast<const void*>(v);
        if (!is_readable(p, 0x30)) continue;
        uintptr_t v_vtable = *reinterpret_cast<const uintptr_t*>(p);
        if (!is_code_like_ptr(v_vtable)) continue;

        UObject* obj = reinterpret_cast<UObject*>(const_cast<void*>(p));
        uintptr_t outer_raw = *reinterpret_cast<const uintptr_t*>(
            reinterpret_cast<const uint8_t*>(p) + 0x20);
        if (outer_raw == 0) continue;
        UObject* outer = reinterpret_cast<UObject*>(outer_raw);
        std::wstring cn = safe_class_name(outer, lv_arena);
        if (cn.empty() || is_generic_container_class(cn)) continue;
        return cn;
    }

    // Pass 2: DropView's own Outer.
    uintptr_t dv_outer = *reinterpret_cast<const uintptr_t*>(lv_bytes + 0x20);
    if (dv_outer != 0) {
        UObject* ofrom = reinterpret_cast<UObject*>(dv_outer);
        std::wstring cn = safe_class_name(ofrom, lv_arena);
        if (!cn.empty() && !is_generic_container_class(cn)) return cn;
    }

    return L"";
}

// Spatial fallback — find nearest source-like actor to the loot actor's
// world position. Used for mob/chest drops where Outer resolution gives a
// generic container. Only reads reflected UFunctions (K2_GetActorLocation),
// so it's crash-safe.
struct SourceCandidate { AActor* actor; FVector loc; std::wstring cname; };

static std::vector<SourceCandidate> gather_source_candidates()
{
    std::vector<SourceCandidate> out;
    out.reserve(4096);
    UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
        if (!cur) return LoopAction::Continue;
        auto* cls = cur->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        auto cname = cls->GetName();
        std::wstring_view cn_view(cname);
        bool source_like =
            cn_view.find(STR("BP_Segment_"))  != std::wstring_view::npos ||
            cn_view.find(STR("BP_Mob_"))      != std::wstring_view::npos ||
            cn_view.find(STR("BP_Dead"))      != std::wstring_view::npos ||
            cn_view.find(STR("AICharacter"))  != std::wstring_view::npos ||
            cn_view.find(STR("Chest"))        != std::wstring_view::npos ||
            cn_view.find(STR("Pickup"))       != std::wstring_view::npos ||
            cn_view.find(STR("R5BLActor_"))   != std::wstring_view::npos;
        if (!source_like) return LoopAction::Continue;
        auto full = cur->GetFullName();
        if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;

        AActor* actor = reinterpret_cast<AActor*>(cur);
        UFunction* loc_fn = actor->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
        if (!loc_fn) return LoopAction::Continue;
        struct { FVector ReturnValue; } p{};
        actor->ProcessEvent(loc_fn, &p);

        out.push_back({ actor, p.ReturnValue, std::wstring(cname) });
        return LoopAction::Continue;
    });
    return out;
}

static std::wstring find_nearest_source(AActor* loot_actor,
                                        const std::vector<SourceCandidate>& cands,
                                        double radius)
{
    UFunction* loc_fn = loot_actor->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
    if (!loc_fn) return L"";
    struct { FVector ReturnValue; } p{};
    loot_actor->ProcessEvent(loc_fn, &p);
    FVector ll = p.ReturnValue;

    double best = radius * radius;
    std::wstring best_name;
    for (const auto& c : cands) {
        double dx = c.loc.X() - ll.X();
        double dy = c.loc.Y() - ll.Y();
        double dz = c.loc.Z() - ll.Z();
        double d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best) { best = d2; best_name = c.cname; }
    }
    return best_name;
}

// ap_native_lootitems([N]) -> list the first N populated R5LootActors and
// the source BP class for each ("Ficus_1800cm_C", "BP_Mob_Chicken_C", etc).
// The actual item identity lives in a central subsystem table we can't reach
// from DropView memory; the source name is the next best identifier.
static int lua_lootitems(const LuaMadeSimple::Lua& lua)
{
    int N = 20;
    if (lua.is_number()) {
        N = static_cast<int>(lua.get_float());
        if (N < 1)   N = 1;
        if (N > 100) N = 100;
    }

    // Build spatial candidate index once (covers mob / chest drops that
    // the Outer-walk can't identify).
    auto live = gather_live_uobjects();
    std::string out = "Populated R5LootActors (source-actor class names):\n";
    int shown = 0, empty = 0;
    UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
        if (!cur || shown >= N) return shown >= N ? LoopAction::Break : LoopAction::Continue;
        auto* cls = cur->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        if (std::wstring_view(cls->GetName()) != STR("R5LootActor")) return LoopAction::Continue;
        auto full = cur->GetFullName();
        if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
        UObject* lv = *reinterpret_cast<UObject**>(
            reinterpret_cast<uint8_t*>(cur) + 0x310);
        if (!lv) { empty++; return LoopAction::Continue; }

        std::wstring src = get_loot_source_name_safe(lv, live);
        std::string src_narrow = src.empty() ? "<unknown source>" : w_to_narrow(src);

        char buf[320];
        std::snprintf(buf, sizeof(buf), "  [%d] actor=0x%p  source=%s\n",
                      shown, (void*)cur, src_narrow.c_str());
        out += buf;
        shown++;
        return LoopAction::Continue;
    });
    out += "  [populated shown: " + std::to_string(shown) +
           ", empty skipped: " + std::to_string(empty) + "]\n";
    lua.set_string(out);
    return 1;
}

// ap_native_giveitem(player, search) -> find a populated R5LootActor whose
// SOURCE actor class (the BP that produced the drop) matches `search`
// (case-insensitive substring) and teleport it to the player.
//
// The DropView's actual item data is held in a central subsystem table we
// can't reach from LootView memory, so identifying the item directly isn't
// viable. Matching on source BP name is the next-best identifier: it tells
// you what tree/mob/resource-node produced the drop (e.g. "ficus" matches
// any Ficus-tree drop, "chicken" matches chicken drops).
static int lua_giveitem(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_giveitem(player, search)"); return 1; }
    auto target_name = to_wlower(lua.get_string());
    if (!lua.is_string()) { lua.set_string("search required"); return 1; }
    auto search_lower = to_wlower(lua.get_string());

    auto player = find_player_by_name(target_name);
    if (!player.pawn) { lua.set_string("player not found"); return 1; }

    FVector loc{};
    {
        UFunction* fn = player.pawn->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
        if (!fn) { lua.set_string("K2_GetActorLocation missing"); return 1; }
        struct { FVector ReturnValue; } p{};
        player.pawn->ProcessEvent(fn, &p);
        loc = p.ReturnValue;
    }

    auto live = gather_live_uobjects();
    AActor*      chosen_actor = nullptr;
    std::wstring chosen_src;
    AActor*      fallback_actor = nullptr;  // first populated; used if search misses
    int          scanned_actors   = 0;
    int          populated_actors = 0;

    UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
        if (!cur || chosen_actor) return chosen_actor ? LoopAction::Break : LoopAction::Continue;
        auto* cls = cur->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        if (std::wstring_view(cls->GetName()) != STR("R5LootActor")) return LoopAction::Continue;
        auto full = cur->GetFullName();
        if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
        scanned_actors++;
        UObject* lv = *reinterpret_cast<UObject**>(
            reinterpret_cast<uint8_t*>(cur) + 0x310);
        if (!lv) return LoopAction::Continue;
        populated_actors++;
        if (!fallback_actor) fallback_actor = reinterpret_cast<AActor*>(cur);

        std::wstring src = get_loot_source_name_safe(lv, live);
        if (src.empty()) return LoopAction::Continue;
        std::wstring src_lower = wlower(src);
        if (src_lower.find(search_lower) == std::wstring::npos) return LoopAction::Continue;

        chosen_actor = reinterpret_cast<AActor*>(cur);
        chosen_src   = src;
        return LoopAction::Break;
    });

    bool used_fallback = false;
    if (!chosen_actor) {
        if (!fallback_actor) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "No populated loot actors in the world "
                          "(loot actors scanned: %d). Break trees / kill mobs first.",
                          scanned_actors);
            lua.set_string(std::string(buf));
            return 1;
        }
        chosen_actor  = fallback_actor;
        used_fallback = true;
    }

    UFunction* tp_fn = chosen_actor->GetFunctionByNameInChain(STR("K2_TeleportTo"));
    if (!tp_fn) tp_fn = chosen_actor->GetFunctionByNameInChain(STR("K2_SetActorLocation"));
    if (!tp_fn) { lua.set_string("no teleport function on loot actor"); return 1; }
    // Drop at player feet, offset ahead so it's not inside the pawn capsule.
    FVector drop(loc.X() + 80.0, loc.Y(), loc.Z() - 80.0);
    std::vector<uint8_t> params(0x80, 0);
    std::memcpy(params.data() + 0x00, &drop, sizeof(FVector));
    chosen_actor->ProcessEvent(tp_fn, params.data());

    char buf[512];
    if (used_fallback) {
        std::snprintf(buf, sizeof(buf),
                      "No loot actor matched '%s' specifically. Teleported a "
                      "random populated loot actor to %s instead. "
                      "Run 'ap.lootitems' to see what specific sources are "
                      "identifiable in the current world state.",
                      w_to_narrow(search_lower).c_str(),
                      w_to_narrow(player.name).c_str());
    } else {
        std::snprintf(buf, sizeof(buf),
                      "Teleported loot actor (source: %s) to %s. "
                      "Auto-pickup should deliver its contents shortly.",
                      w_to_narrow(chosen_src).c_str(),
                      w_to_narrow(player.name).c_str());
    }
    lua.set_string(std::string(buf));
    return 1;
}

// ap_native_itemlist([search]) -> list known UR5BLInventoryItem data assets,
// optionally filtered by a case-insensitive substring on the short name.
static int lua_itemlist(const LuaMadeSimple::Lua& lua)
{
    std::wstring search;
    if (lua.is_string()) search = to_wlower(lua.get_string());

    auto items = gather_all_items();
    std::string out;

    int shown = 0;
    for (const auto& e : items) {
        if (!search.empty() && e.short_name.find(search) == std::wstring::npos) continue;
        if (shown == 0) {
            out = "UR5BLInventoryItem data assets";
            if (!search.empty()) out += " matching '" + w_to_narrow(search) + "'";
            out += ":\n";
        }
        out += "  " + w_to_narrow(e.display_name) + "\n";
        shown++;
        if (shown >= 200) {
            out += "  [capped at 200]\n";
            break;
        }
    }
    if (shown == 0) {
        out = "No items";
        if (!search.empty()) out += " matching '" + w_to_narrow(search) + "'";
        out += ". Known item count: " + std::to_string(items.size());
    } else {
        out += "  [total shown: " + std::to_string(shown) +
               " / known: " + std::to_string(items.size()) + "]\n";
    }
    lua.set_string(out);
    return 1;
}

// ap_native_itemscan(target, [bytes]) -> scan an object's raw memory for any
// references to known UR5BLInventoryItem data assets. Debug tool to verify
// the ref-scan technique on live inventory views (chests, etc.) without
// needing a populated loot actor in the world.
static int lua_itemscan(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_itemscan(target, [bytes])"); return 1; }
    auto target_narrow = std::string(lua.get_string());
    std::wstring target_wide;
    for (char c : target_narrow) target_wide.push_back(static_cast<wchar_t>(c));

    int bytes = 4096;
    if (lua.is_number()) {
        bytes = static_cast<int>(lua.get_float());
        if (bytes < 64)    bytes = 64;
        if (bytes > 65536) bytes = 65536;
    }

    UObject* obj = nullptr;
    if (!target_narrow.empty() && target_narrow[0] == '/') {
        obj = UObjectGlobals::FindObject(nullptr, nullptr, target_wide.c_str());
    } else {
        UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
            if (!cur || obj) return obj ? LoopAction::Break : LoopAction::Continue;
            auto* cls = cur->GetClassPrivate();
            if (!cls) return LoopAction::Continue;
            if (std::wstring_view(cls->GetName()) != target_wide) return LoopAction::Continue;
            auto full = cur->GetFullName();
            if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
            obj = cur;
            return LoopAction::Break;
        });
    }
    if (!obj) { lua.set_string("object not found: " + target_narrow); return 1; }

    auto items = gather_all_items();
    std::vector<const InvItemEntry*> hits;
    scan_for_item_refs(obj, static_cast<size_t>(bytes), items, hits, 32);

    std::string out = "target: " + w_to_narrow(obj->GetFullName()).substr(0, 180) + "\n";
    {
        char b[64]; std::snprintf(b, sizeof(b), "addr: 0x%p\n", (void*)obj);
        out += b;
    }
    out += "scanned " + std::to_string(bytes) + " bytes against " +
           std::to_string(items.size()) + " known items\n";
    out += "matches: " + std::to_string(hits.size()) + "\n";
    for (size_t i = 0; i < hits.size(); ++i) {
        out += "  [" + std::to_string(i) + "] " + w_to_narrow(hits[i]->display_name) + "\n";
    }
    lua.set_string(out);
    return 1;
}

// Build a set of live UObject addresses — checking candidate pointers
// against this set before calling virtual methods makes source-walking
// crash-safe (no SEH needed).
static std::unordered_set<uintptr_t> gather_live_uobjects()
{
    std::unordered_set<uintptr_t> out;
    out.reserve(131072);
    UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
        if (cur) out.insert(reinterpret_cast<uintptr_t>(cur));
        return LoopAction::Continue;
    });
    return out;
}

// Resolve source name by scanning DropView for pointers that are confirmed
// live UObjects, then walking their Outer chain through the known-object set
// to the first non-generic class. Crash-safe because every dereferenced
// pointer is validated against the UObject table.
static std::wstring get_loot_source_name_safe(UObject* loot_view,
                                              const std::unordered_set<uintptr_t>& live)
{
    if (!loot_view) return L"";
    uintptr_t lv_arena = reinterpret_cast<uintptr_t>(loot_view) >> 40;
    const uint8_t* lv_bytes = reinterpret_cast<const uint8_t*>(loot_view);

    auto walk = [&](UObject* obj) -> std::wstring {
        for (int hop = 0; hop < 6 && obj; ++hop) {
            if (live.find(reinterpret_cast<uintptr_t>(obj)) == live.end()) return L"";
            auto* cls = obj->GetClassPrivate();
            if (!cls) return L"";
            std::wstring cn(cls->GetName());
            if (!is_generic_container_class(cn)) return cn;
            uintptr_t outer_raw = *reinterpret_cast<const uintptr_t*>(
                reinterpret_cast<const uint8_t*>(obj) + 0x20);
            if (outer_raw == 0) return L"";
            obj = reinterpret_cast<UObject*>(outer_raw);
        }
        return L"";
    };

    for (size_t off = 0x28; off + 8 <= 0x2C0; off += 8) {
        uintptr_t v = *reinterpret_cast<const uintptr_t*>(lv_bytes + off);
        if ((v & 7) != 0) continue;
        if (live.find(v) == live.end()) continue;
        if (v == reinterpret_cast<uintptr_t>(loot_view)) continue;

        UObject* obj = reinterpret_cast<UObject*>(v);
        // Try walking from Outer first (gets away from generic components
        // like StaticMeshComponent immediately).
        uintptr_t outer_raw = *reinterpret_cast<const uintptr_t*>(
            reinterpret_cast<const uint8_t*>(obj) + 0x20);
        if (outer_raw != 0) {
            UObject* outer = reinterpret_cast<UObject*>(outer_raw);
            std::wstring cn = walk(outer);
            if (!cn.empty()) return cn;
        }
        // Also try the pointed-to object itself (in case it's directly
        // the source).
        std::wstring cn2 = walk(obj);
        if (!cn2.empty()) return cn2;
    }

    // Fallback: DropView's own Outer.
    uintptr_t dv_outer = *reinterpret_cast<const uintptr_t*>(lv_bytes + 0x20);
    if (dv_outer != 0) {
        UObject* o = reinterpret_cast<UObject*>(dv_outer);
        std::wstring cn = walk(o);
        if (!cn.empty()) return cn;
    }
    return L"";
}

#if 0
// Diagnostic retained for reference; disabled due to crashes on non-UObject
// pointers that pass heuristic validation. The safer path is
// get_loot_source_name_safe above, which only dereferences pointers that
// exist in the live UObject table.
static int lua_lootsource(const LuaMadeSimple::Lua& lua)
{
    AActor*  actor = nullptr;
    UObject* loot_view = nullptr;
    UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
        if (!cur || actor) return actor ? LoopAction::Break : LoopAction::Continue;
        auto* cls = cur->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        if (std::wstring_view(cls->GetName()) != STR("R5LootActor")) return LoopAction::Continue;
        auto full = cur->GetFullName();
        if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
        UObject* lv = *reinterpret_cast<UObject**>(
            reinterpret_cast<uint8_t*>(cur) + 0x310);
        if (!lv) return LoopAction::Continue;
        actor = reinterpret_cast<AActor*>(cur);
        loot_view = lv;
        return LoopAction::Break;
    });
    if (!actor) { lua.set_string("no populated R5LootActor found"); return 1; }

    std::string out;
    {
        char b[320];
        std::snprintf(b, sizeof(b), "actor: %s\nloot_view: 0x%p\n\n",
                      w_to_narrow(actor->GetFullName()).substr(0, 180).c_str(),
                      (void*)loot_view);
        out += b;
    }

    uintptr_t lv_arena = reinterpret_cast<uintptr_t>(loot_view) >> 40;
    const uint8_t* lv_bytes = reinterpret_cast<const uint8_t*>(loot_view);
    std::unordered_set<uintptr_t> seen;

    for (size_t off = 0x28; off + 8 <= 0x2C0; off += 8) {
        uintptr_t v = *reinterpret_cast<const uintptr_t*>(lv_bytes + off);
        if ((v >> 40) != lv_arena) continue;
        if ((v & 7) != 0) continue;
        if (v == reinterpret_cast<uintptr_t>(loot_view)) continue;
        if (!seen.insert(v).second) continue;
        const void* p = reinterpret_cast<const void*>(v);
        if (!is_readable(p, 0x30)) continue;
        uintptr_t vt = *reinterpret_cast<const uintptr_t*>(p);
        if (!is_code_like_ptr(vt)) continue;

        UObject* obj = reinterpret_cast<UObject*>(const_cast<void*>(p));
        std::wstring own_cn = safe_class_name(obj, lv_arena);
        if (own_cn.empty()) continue;

        std::wstring outer_cn;
        std::wstring gp_cn;
        uintptr_t outer_raw = *reinterpret_cast<const uintptr_t*>(
            reinterpret_cast<const uint8_t*>(p) + 0x20);
        if (outer_raw != 0) {
            UObject* outer = reinterpret_cast<UObject*>(outer_raw);
            outer_cn = safe_class_name(outer, lv_arena);
            uintptr_t gp_raw = *reinterpret_cast<const uintptr_t*>(
                reinterpret_cast<const uint8_t*>(outer) + 0x20);
            if (gp_raw != 0) {
                UObject* gp = reinterpret_cast<UObject*>(gp_raw);
                gp_cn = safe_class_name(gp, lv_arena);
            }
        }

        char b[512];
        std::snprintf(b, sizeof(b),
                      "  +0x%03zx = 0x%p\n"
                      "    own:    %s\n"
                      "    outer:  %s\n"
                      "    grand:  %s\n",
                      off, (void*)v,
                      w_to_narrow(own_cn).c_str(),
                      outer_cn.empty() ? "<?>" : w_to_narrow(outer_cn).c_str(),
                      gp_cn.empty()    ? "<?>" : w_to_narrow(gp_cn).c_str());
        out += b;
    }

    lua.set_string(out);
    return 1;
}
#endif

// ap_native_lootslots([N]) -> find the first populated R5LootActor and
// hex-dump the first N slots' 0x60 bytes. Slots live on the heap at
// addresses held in a pointer array at ~LootView+0x180. Ground-truth
// inspection so we can see exactly where an item reference sits within
// a slot when the pointer-chasing scan doesn't find matches.
static int lua_lootslots(const LuaMadeSimple::Lua& lua)
{
    int N = 4;
    if (lua.is_number()) {
        N = static_cast<int>(lua.get_float());
        if (N < 1)  N = 1;
        if (N > 16) N = 16;
    }

    AActor*  actor = nullptr;
    UObject* loot_view = nullptr;
    UObjectGlobals::ForEachUObject([&](UObject* cur, int32, int32) {
        if (!cur || actor) return actor ? LoopAction::Break : LoopAction::Continue;
        auto* cls = cur->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        if (std::wstring_view(cls->GetName()) != STR("R5LootActor")) return LoopAction::Continue;
        auto full = cur->GetFullName();
        if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
        UObject* lv = *reinterpret_cast<UObject**>(
            reinterpret_cast<uint8_t*>(cur) + 0x310);
        if (!lv) return LoopAction::Continue;
        actor = reinterpret_cast<AActor*>(cur);
        loot_view = lv;
        return LoopAction::Break;
    });
    if (!actor) { lua.set_string("no populated R5LootActor found"); return 1; }

    // Scan LootView[+0x100..+0x2C0] for slot pointers. Real slot pointers
    // share the top 24 bits with the LootView itself (same heap arena) and
    // cluster within a few MB of each other.
    std::vector<uintptr_t> slots;
    const uint8_t* lv_bytes = reinterpret_cast<const uint8_t*>(loot_view);
    uintptr_t lv_arena_tag = reinterpret_cast<uintptr_t>(loot_view) >> 40;
    for (size_t off = 0x100; off + 8 <= 0x2C0; off += 8) {
        uintptr_t v = *reinterpret_cast<const uintptr_t*>(lv_bytes + off);
        if ((v >> 40) != lv_arena_tag) continue;
        if ((v & 7) != 0) continue;
        slots.push_back(v);
        if ((int)slots.size() >= N) break;
    }

    auto items = gather_all_items();
    std::unordered_map<uintptr_t, const InvItemEntry*> by_ptr;
    std::unordered_map<int32,     const InvItemEntry*> by_idx;
    for (const auto& e : items) {
        by_ptr[reinterpret_cast<uintptr_t>(e.obj)] = &e;
        by_idx[e.index] = &e;
    }

    std::string out;
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "actor: %s\nloot_view: 0x%p (class %s)\n",
                      w_to_narrow(actor->GetFullName()).substr(0, 180).c_str(),
                      (void*)loot_view,
                      loot_view->GetClassPrivate()
                        ? w_to_narrow(loot_view->GetClassPrivate()->GetName()).c_str()
                        : "?");
        out += buf;
    }
    out += "candidate slot pointers: " + std::to_string(slots.size()) + "\n\n";

    for (size_t i = 0; i < slots.size(); ++i) {
        uintptr_t addr = slots[i];
        const void* p = reinterpret_cast<const void*>(addr);
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "=== slot[%zu] @ 0x%p ===\n", i, p);
            out += buf;
        }

        if (!is_readable(p, 0x60)) { out += "  <unreadable>\n\n"; continue; }

        const uint8_t* sb = reinterpret_cast<const uint8_t*>(p);

        // Only probe the class name if vtable and ClassPrivate are both
        // strongly UObject-shaped (code vtable, same-arena class pointer
        // whose own vtable is also in code range).
        uintptr_t v_vtable = *reinterpret_cast<const uintptr_t*>(sb);
        uintptr_t cls_raw  = *reinterpret_cast<const uintptr_t*>(sb + 0x10);
        bool      probed   = false;
        bool cls_valid_loc = ((cls_raw >> 40) == (addr >> 40)) || is_code_like_ptr(cls_raw);
        if (is_code_like_ptr(v_vtable) &&
            cls_valid_loc &&
            is_readable(reinterpret_cast<const void*>(cls_raw), 0x30))
        {
            uintptr_t cls_vtable = *reinterpret_cast<const uintptr_t*>(cls_raw);
            if (is_code_like_ptr(cls_vtable)) {
                UObject* as_obj = reinterpret_cast<UObject*>(const_cast<void*>(p));
                auto* maybe_cls = as_obj->GetClassPrivate();
                if (maybe_cls) {
                    std::string cn = w_to_narrow(maybe_cls->GetName());
                    char b[128];
                    std::snprintf(b, sizeof(b), "  class: %s\n", cn.c_str());
                    out += b;
                    probed = true;
                }
            }
        }
        if (!probed) out += "  class: <not a UObject>\n";
        for (int row = 0; row < 0x60; row += 16) {
            char line[256];
            int pos = std::snprintf(line, sizeof(line), "  %04x: ", row);
            for (int j = 0; j < 16; j++)
                pos += std::snprintf(line + pos, sizeof(line) - pos, "%02x ", sb[row + j]);
            pos += std::snprintf(line + pos, sizeof(line) - pos, " |");
            for (int j = 0; j < 16; j++) {
                char c = static_cast<char>(sb[row + j]);
                char oc = (c >= 32 && c < 127) ? c : '.';
                pos += std::snprintf(line + pos, sizeof(line) - pos, "%c", oc);
            }
            pos += std::snprintf(line + pos, sizeof(line) - pos, "|\n");
            out += line;
        }

        // Callout any word/dword that matches a known inventory item.
        for (int off = 0; off + 8 <= 0x60; off += 8) {
            uintptr_t v = *reinterpret_cast<const uintptr_t*>(sb + off);
            if (v >= 0x10000) {
                auto hit = by_ptr.find(v);
                if (hit != by_ptr.end()) {
                    char b[256];
                    std::snprintf(b, sizeof(b), "  >> +0x%02x (ptr) -> ITEM '%s'\n",
                                  off, w_to_narrow(hit->second->display_name).c_str());
                    out += b;
                }
            }
        }
        for (int off = 0; off + 4 <= 0x60; off += 4) {
            int32 v = *reinterpret_cast<const int32*>(sb + off);
            if (v > 0 && v <= 0x00FFFFFF) {
                auto hit = by_idx.find(v);
                if (hit != by_idx.end()) {
                    char b[256];
                    std::snprintf(b, sizeof(b), "  >> +0x%02x (idx=%d) -> ITEM '%s'\n",
                                  off, v, w_to_narrow(hit->second->display_name).c_str());
                    out += b;
                }
            }
        }
        out += "\n";
    }

    lua.set_string(out);
    return 1;
}

// ap_native_findclass(path) -> string. Resolves a full object path and reports
// what we got back (class-of, name, full name). Use to verify class paths.
static int lua_findclass(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_findclass(path)"); return 1; }
    auto p_narrow = std::string(lua.get_string());
    std::wstring p_wide;
    for (char c : p_narrow) p_wide.push_back(static_cast<wchar_t>(c));
    UObject* obj = UObjectGlobals::FindObject(nullptr, nullptr, p_wide.c_str());
    if (!obj) { lua.set_string("not found: " + p_narrow); return 1; }
    std::string out = "full: " + w_to_narrow(obj->GetFullName()) + "\n";
    auto* cls = obj->GetClassPrivate();
    if (cls) out += "class: " + w_to_narrow(cls->GetName()) + "\n";
    // Addr for debugging
    char b[64]; std::snprintf(b, sizeof(b), "addr: 0x%p", (void*)obj);
    out += b;
    lua.set_string(out);
    return 1;
}

// ap_native_spawn(player_name, class_path, [dx, dy, dz])
// Spawns the given actor class at player's location + offset.
// Heavily instrumented — logs to UE4SS.log at every step so we can diagnose crashes.
static int lua_spawn(const LuaMadeSimple::Lua& lua)
{
    using namespace RC;
    Output::send<LogLevel::Verbose>(STR("[spawn] ENTER\n"));

    if (!lua.is_string()) { lua.set_string("usage: ap_native_spawn(player, class_path, [dx, dy, dz])"); return 1; }
    auto target_name = to_wlower(lua.get_string());
    if (!lua.is_string()) { lua.set_string("class_path required"); return 1; }
    auto class_path_narrow = std::string(lua.get_string());
    std::wstring class_path_wide;
    for (char c : class_path_narrow) class_path_wide.push_back(static_cast<wchar_t>(c));

    double dx = 0, dy = 0, dz = 200;  // default: 200 units up (well above feet)
    if (lua.is_number()) { dx = lua.get_float(); }
    if (lua.is_number()) { dy = lua.get_float(); }
    if (lua.is_number()) { dz = lua.get_float(); }

    Output::send<LogLevel::Verbose>(STR("[spawn] finding player\n"));
    auto player = find_player_by_name(target_name);
    if (!player.pawn) { lua.set_string("player not found"); return 1; }

    Output::send<LogLevel::Verbose>(STR("[spawn] finding class\n"));
    UObject* class_obj = UObjectGlobals::FindObject(nullptr, nullptr, class_path_wide.c_str());
    if (!class_obj) {
        lua.set_string("class not found at path: " + class_path_narrow);
        return 1;
    }

    // Verify it's a UClass-like object before reinterpret_cast
    auto* meta = class_obj->GetClassPrivate();
    if (!meta) { lua.set_string("class_obj has no class"); return 1; }
    auto meta_name = w_to_narrow(meta->GetName());
    if (meta_name != "Class" && meta_name != "BlueprintGeneratedClass") {
        lua.set_string("not a UClass: meta-class is " + meta_name);
        return 1;
    }
    auto* pickup_class = reinterpret_cast<UClass*>(class_obj);

    Output::send<LogLevel::Verbose>(STR("[spawn] getting actor location\n"));
    UFunction* loc_fn = player.pawn->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
    if (!loc_fn) { lua.set_string("K2_GetActorLocation not on pawn"); return 1; }
    struct { FVector ReturnValue; } loc_params{};
    player.pawn->ProcessEvent(loc_fn, &loc_params);
    FVector loc = loc_params.ReturnValue;
    double px = loc.X(), py = loc.Y(), pz = loc.Z();

    Output::send<LogLevel::Verbose>(STR("[spawn] building transform\n"));
    FVector spawn_loc(px + dx, py + dy, pz + dz);
    FQuat rot(0, 0, 0, 1);
    FVector scale(1, 1, 1);
    FTransform xform(rot, spawn_loc, scale);

    // Locate UGameplayStatics CDO + both UFunctions. We call ProcessEvent manually
    // with buffers sized by the function's reflected GetPropertiesSize(), bypassing
    // UE4SS's wrappers (which omit the UE5.2+ TransformScaleMethod field in their
    // FinishSpawningActor_Params struct and corrupt adjacent memory on ProcessEvent).
    UObject* gs_cdo = UObjectGlobals::FindObject(nullptr, nullptr,
        STR("/Script/Engine.Default__GameplayStatics"));
    if (!gs_cdo) { lua.set_string("GameplayStatics CDO missing"); return 1; }

    UFunction* begin_fn  = gs_cdo->GetFunctionByNameInChain(STR("BeginDeferredActorSpawnFromClass"));
    UFunction* finish_fn = gs_cdo->GetFunctionByNameInChain(STR("FinishSpawningActor"));
    if (!begin_fn || !finish_fn) { lua.set_string("Begin/Finish UFunctions not found"); return 1; }

    // UE5.2+ reflected layouts we verified via ap.funcparamsn:
    //
    // BeginDeferredActorSpawnFromClass (0x90 = 144 bytes):
    //   +0x00 WorldContextObject*   +0x08 ActorClass*   +0x10 FTransform (96B)
    //   +0x70 CollisionHandling(u8) +0x78 Owner*        +0x80 ScaleMethod(u8)
    //   +0x88 ReturnValue*
    //
    // FinishSpawningActor (0x80 = 128 bytes):
    //   +0x00 Actor* +0x10 FTransform (96B) +0x70 ScaleMethod(u8) +0x78 ReturnValue*

    Output::send<LogLevel::Verbose>(STR("[spawn] calling BeginDeferredActorSpawnFromClass (manual)\n"));
    uint32_t begin_size = begin_fn->GetPropertiesSize();
    if (begin_size < 0x90 || begin_size > 0x200) begin_size = 0x100;
    std::vector<uint8_t> begin_buf(begin_size, 0);
    *reinterpret_cast<UObject**>(begin_buf.data() + 0x00) = player.pawn;
    *reinterpret_cast<UClass**>(begin_buf.data() + 0x08)  = pickup_class;
    std::memcpy(begin_buf.data() + 0x10, &xform, sizeof(FTransform));
    begin_buf[0x70] = 2;  // AdjustIfPossibleButAlwaysSpawn
    *reinterpret_cast<AActor**>(begin_buf.data() + 0x78) = nullptr;  // Owner
    begin_buf[0x80] = 1;  // MultiplyWithRoot
    gs_cdo->ProcessEvent(begin_fn, begin_buf.data());
    AActor* actor = *reinterpret_cast<AActor**>(begin_buf.data() + 0x88);

    if (!actor) {
        lua.set_string("BeginDeferredActorSpawnFromClass returned null");
        return 1;
    }

    Output::send<LogLevel::Verbose>(STR("[spawn] calling FinishSpawningActor (manual)\n"));
    uint32_t finish_size = finish_fn->GetPropertiesSize();
    if (finish_size < 0x80 || finish_size > 0x200) finish_size = 0x100;
    std::vector<uint8_t> finish_buf(finish_size, 0);
    *reinterpret_cast<AActor**>(finish_buf.data() + 0x00) = actor;
    std::memcpy(finish_buf.data() + 0x10, &xform, sizeof(FTransform));
    finish_buf[0x70] = 1;  // MultiplyWithRoot
    gs_cdo->ProcessEvent(finish_fn, finish_buf.data());

    Output::send<LogLevel::Verbose>(STR("[spawn] DONE\n"));
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "spawned %s at (%.1f, %.1f, %.1f) OK",
                  class_path_narrow.c_str(),
                  static_cast<double>(spawn_loc.X()),
                  static_cast<double>(spawn_loc.Y()),
                  static_cast<double>(spawn_loc.Z()));
    lua.set_string(std::string(buf));
    return 1;
}

// ap_native_kill(name) -> string. Sets Health to 0 via AttributeSet direct write.
static int lua_kill(const LuaMadeSimple::Lua& lua)
{
    if (!lua.is_string()) { lua.set_string("usage: ap_native_kill(name)"); return 1; }
    auto target = to_wlower(lua.get_string());
    auto found = find_player_by_name(target);
    if (!found.pawn) { lua.set_string("player not found"); return 1; }
    auto* attrset = get_attribute_set(found);
    if (!attrset) { lua.set_string("AttributeSet not found"); return 1; }
    bool ok = write_attribute(attrset, STR("Health"), 0.0f);
    lua.set_string(ok ? "Health set to 0 (should trigger death)" : "write failed");
    return 1;
}

// ---------------------------------------------------------------------------
// Mod class
// ---------------------------------------------------------------------------

class AdmiralsPanelNative : public CppUserModBase
{
public:
    AdmiralsPanelNative() : CppUserModBase()
    {
        ModName = STR("AdmiralsPanel-Native");
        ModVersion = STR("0.6.0");
        ModDescription = STR("Native UFunction bridge + standalone HTTP server for AdmiralsPanel");
        ModAuthors = STR("Mancavegaming");
        Output::send<LogLevel::Verbose>(STR("[AdmiralsPanel-Native] loaded (v0.6.0)\n"));

        // v0.6 standalone mode: load config, start our own HTTP server +
        // session + spool bridge. Running alongside WindrosePlus on port
        // 8790 during the migration window.
        std::string gd = ap_cfg::discover_game_dir();
        ap_cfg::load(gd);
        const auto& cfg = ap_cfg::get();

        if (m_app.start()) {
            Output::send<LogLevel::Verbose>(
                STR("[AdmiralsPanel-Native] HTTP server listening on :{}\n"),
                cfg.http_port);
        } else {
            Output::send<LogLevel::Warning>(
                STR("[AdmiralsPanel-Native] HTTP server failed to bind :{}\n"),
                cfg.http_port);
        }
    }

    ~AdmiralsPanelNative() override
    {
        m_app.stop();
    }

private:
    ap_app::App m_app;
public:

    auto on_lua_start(StringViewType mod_name,
                      LuaMadeSimple::Lua& lua,
                      LuaMadeSimple::Lua& /*main_lua*/,
                      LuaMadeSimple::Lua& /*async_lua*/,
                      LuaMadeSimple::Lua* /*hook_lua*/) -> void override
    {
        lua.register_function("admiralspanel_native_version", lua_version);
        lua.register_function("admiralspanel_native_find",    lua_find);
        lua.register_function("admiralspanel_native_heal",    lua_heal);
        lua.register_function("admiralspanel_native_feed",    lua_feed);
        lua.register_function("admiralspanel_native_damage",  lua_damage);
        lua.register_function("admiralspanel_native_revive",  lua_revive);
        lua.register_function("admiralspanel_native_setattr", lua_setattr);
        lua.register_function("admiralspanel_native_readattr",lua_readattr);
        lua.register_function("admiralspanel_native_allstats",lua_allstats);
        lua.register_function("admiralspanel_native_classprobe",lua_classprobe);
        lua.register_function("admiralspanel_native_scan",     lua_scan);
        lua.register_function("admiralspanel_native_scanpath", lua_scanpath);
        lua.register_function("admiralspanel_native_rawdump",  lua_rawdump);
        lua.register_function("admiralspanel_native_dumpclass",lua_dumpclass);
        lua.register_function("admiralspanel_native_spawn",    lua_spawn);
        lua.register_function("admiralspanel_native_loc",      lua_loc);
        lua.register_function("admiralspanel_native_findclass",lua_findclass);
        lua.register_function("admiralspanel_native_funcparams",lua_funcparams);
        lua.register_function("admiralspanel_native_yankactor",lua_yankactor);
        lua.register_function("admiralspanel_native_giveloot", lua_giveloot);
        lua.register_function("admiralspanel_native_lootlist", lua_lootlist);
        lua.register_function("admiralspanel_native_lootinspect", lua_lootinspect);
        lua.register_function("admiralspanel_native_lootitems", lua_lootitems);
        lua.register_function("admiralspanel_native_giveitem",  lua_giveitem);
        lua.register_function("admiralspanel_native_itemlist",  lua_itemlist);
        lua.register_function("admiralspanel_native_itemscan",  lua_itemscan);
        lua.register_function("admiralspanel_native_lootslots", lua_lootslots);
        lua.register_function("admiralspanel_native_inspect", lua_inspect);
        lua.register_function("admiralspanel_native_kill",    lua_kill);
        Output::send<LogLevel::Verbose>(
            STR("[AdmiralsPanel-Native] registered bindings for mod: {}\n"), mod_name);
    }
};

#define AP_NATIVE_API __declspec(dllexport)
extern "C"
{
    AP_NATIVE_API RC::CppUserModBase* start_mod()
    {
        return new AdmiralsPanelNative();
    }

    AP_NATIVE_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
