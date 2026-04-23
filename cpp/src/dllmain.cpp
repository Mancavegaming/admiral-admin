// AdmiralsPanel-Native — C++ UE4SS companion mod.
// Provides the UFunctions UE4SS-Lua can't safely call directly (complex structs / RPCs).
//
// v0.2 — first real admin actions. Calls UGameplayStatics::ApplyDamage via ProcessEvent
// (server-authoritative; synthesizes a valid FDamageEvent internally).

#include <cwctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Core/Containers/Array.hpp>

using namespace RC;
using namespace RC::Unreal;

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

// Find PlayerState.R5AttributeSet from a found player
static UObject* get_attribute_set(const PlayerRef& p)
{
    if (!p.state) return nullptr;
    auto* prop = p.state->GetPropertyByNameInChain(STR("R5AttributeSet"));
    if (!prop) return nullptr;
    auto** ptr = prop->ContainerPtrToValuePtr<UObject*>(p.state);
    return (ptr && *ptr) ? *ptr : nullptr;
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
        { STR("Comfort"), STR("MaxComfort") },  // Windrose's "hunger" proxy
        { STR("Posture"), STR("MaxPosture") },
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
    auto* attrset = get_attribute_set(found);
    if (!attrset) { lua.set_string("AttributeSet not found"); return 1; }

    float before = 0;
    bool read_ok = read_attribute(attrset, attr_wide.c_str(), &before);
    if (!read_ok) {
        lua.set_string("attribute '" + attr_narrow + "' not found on R5AttributeSet");
        return 1;
    }
    bool ok = write_attribute(attrset, attr_wide.c_str(), val);
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s: %.2f -> %.2f %s",
                  attr_narrow.c_str(), static_cast<double>(before),
                  static_cast<double>(val), ok ? "OK" : "FAIL");
    lua.set_string(std::string(buf));
    return 1;
}

// ap_native_readattr(name, attrname) -> string. Read any attribute's CurrentValue + BaseValue.
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
    auto* attrset = get_attribute_set(found);
    if (!attrset) { lua.set_string("AttributeSet not found"); return 1; }

    float cur = 0, base = 0;
    if (!read_attribute(attrset, attr_wide.c_str(), &cur, &base)) {
        lua.set_string("attribute '" + attr_narrow + "' not found");
        return 1;
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s: current=%.2f base=%.2f",
                  attr_narrow.c_str(),
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
        ModVersion = STR("0.2.0");
        ModDescription = STR("Native UFunction bridge for AdmiralsPanel");
        ModAuthors = STR("Mancavegaming");
        Output::send<LogLevel::Verbose>(STR("[AdmiralsPanel-Native] loaded (v0.2.0)\n"));
    }

    ~AdmiralsPanelNative() override = default;

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
