// Collectible Magnet
//
// Pulls nearby collectibles (notes, eggs, feathers, extra lives, honeycombs, Mumbo tokens and
// Jiggies) toward the player. The magnet's field can be toggled with a D-pad button during
// gameplay and its range widened or narrowed with D-pad Left/Right, which writes back to the
// menu slider.
//
// Collectibles exist in two forms:
//   1. Actor props - a Prop whose marker has an Actor behind it (Jiggies, tokens, lives, the
//      note actors that note retention and rando spawn, anything an enemy drops).
//   2. Static sprite props - a Prop stored directly in a Cube's prop list (world-placed eggs
//      and feathers, and notes when note retention is off).
//
// Actor props are moved with the engine's own marker position setter (func_8032F64C), which
// re-buckets the prop into the correct Cube, so they can be pulled from any distance and are
// still collected by the game's normal prop collision.
//
// Static sprite props are moved in place and stay in their original Cube - the engine has no
// sprite-prop equivalent of func_8032F64C, and re-bucketing one by hand would churn prop arrays
// that note retention, rando and the frame interpolator all read. The player's own prop
// collision sweep (func_80303D78) only visits the 3x3x3 Cube neighbourhood around the player,
// so sprite props are only pulled while their Cube is inside that neighbourhood. That keeps the
// invariant that anything the magnet moves is also something the game will collect.
//
// Moving an actor prop can realloc or shift a Cube's prop array, so a frame is split into a scan
// pass that collects ActorMarkers and an apply pass that moves them. Marker pointers come from a
// fixed pool and stay valid across the reshuffle; Prop pointers do not.
//
// A sprite prop that was displaced but never collected (magnet switched off mid-pull, player
// outran the pull) eases back to where the map placed it. Origins live in a small registry keyed
// by Cube + asset + exact current position, because __cube_sort reorders a Cube's props every
// frame, which rules out both prop pointers and slot indices as identity.

#include <libultraship/bridge.h>
#include "port/UI/cvar_prefixes.h"
#include "port/Enhancements/Events/Hooks/Events.h"
#include "port/ShipInit.hpp"

#include <vector>

#include "functions.h"
extern "C" {
#include "enums.h"
#include "prop.h"
#include "bk_time.h"
#include "core2/core2.h"

Cube* cubeList_GetCubeAtPosition_s32(s32 position[3]);
Cube* func_80303658(void); // out-of-bounds fallback cube
Cube* func_8030364C(void); // always-loaded "global" cube
}

#define CVAR_MAGNET CVAR_ENHANCEMENT("Cheats.CollectibleMagnet")
#define CVAR_MAGNET_RANGE CVAR_ENHANCEMENT("Cheats.CollectibleMagnetRange")
#define CVAR_MAGNET_SPEED CVAR_ENHANCEMENT("Cheats.CollectibleMagnetSpeed")
#define CVAR_MAGNET_HOTKEY CVAR_ENHANCEMENT("Cheats.CollectibleMagnetHotkey")
#define CVAR_MAGNET_DPAD_RANGE CVAR_ENHANCEMENT("Cheats.CollectibleMagnetDpadRange")
#define CVAR_MAGNET_NOTES CVAR_ENHANCEMENT("Cheats.CollectibleMagnetNotes")
#define CVAR_MAGNET_EGGS CVAR_ENHANCEMENT("Cheats.CollectibleMagnetEggs")
#define CVAR_MAGNET_LIVES CVAR_ENHANCEMENT("Cheats.CollectibleMagnetLives")
#define CVAR_MAGNET_TOKENS CVAR_ENHANCEMENT("Cheats.CollectibleMagnetTokens")
#define CVAR_MAGNET_JIGGIES CVAR_ENHANCEMENT("Cheats.CollectibleMagnetJiggies")

namespace {

constexpr s32 kCubeSize = 1000;            // engine cube edge length
constexpr s32 kMaxCubeReach = 3;           // hard cap on the cube sweep
constexpr s32 kSpriteCubeReach = 1;        // player prop collision neighbourhood
constexpr f32 kTargetHeightOffset = 40.0f; // aim at the body, not the feet
constexpr f32 kSnapDistance = 8.0f;
constexpr f32 kMinSpeedFraction = 0.25f;   // pull speed at the edge of the field
constexpr f32 kReturnSpeedFraction = 0.5f; // speed used to undo a pull
constexpr f32 kRangeStep = 100.0f;
constexpr f32 kRangeMin = 100.0f;
constexpr f32 kRangeMax = 3000.0f;
constexpr f32 kDefaultRange = 800.0f;
constexpr f32 kDefaultSpeed = 1400.0f;
constexpr f32 kMaxDeltaTime = 0.1f; // clamp hitches and loading spikes
constexpr s32 kMaxPendingMarkers = 192;
constexpr s32 kDisplacedEntryTimeout = 90; // frames a displaced entry survives unmatched

constexpr s32 kSpriteAssetBase = 0x572; // sprite prop asset ids start here

enum MagnetCategory {
    MAGNET_NOTES = 1 << 0,
    MAGNET_EGGS_FEATHERS = 1 << 1,
    MAGNET_LIVES_HONEYCOMBS = 1 << 2,
    MAGNET_TOKENS = 1 << 3,
    MAGNET_JIGGIES = 1 << 4,
};

// A static sprite prop we moved, so it can be put back if it is never collected.
struct DisplacedSprite {
    Cube* cube;
    s32 assetId;
    s16 origin[3];
    s16 current[3];
    s32 unseenFrames;
    bool restored;
};

bool sMagnetOn = true;
f32 sActiveRange = kDefaultRange;
f32 sActiveSpeed = kDefaultSpeed;
std::vector<DisplacedSprite> sDisplaced;
std::vector<ActorMarker*> sPending;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

u32 Categories() {
    u32 categories = 0;
    if (CVarGetInteger(CVAR_MAGNET_NOTES, 1)) {
        categories |= MAGNET_NOTES;
    }
    if (CVarGetInteger(CVAR_MAGNET_EGGS, 1)) {
        categories |= MAGNET_EGGS_FEATHERS;
    }
    if (CVarGetInteger(CVAR_MAGNET_LIVES, 1)) {
        categories |= MAGNET_LIVES_HONEYCOMBS;
    }
    if (CVarGetInteger(CVAR_MAGNET_TOKENS, 1)) {
        categories |= MAGNET_TOKENS;
    }
    if (CVarGetInteger(CVAR_MAGNET_JIGGIES, 1)) {
        categories |= MAGNET_JIGGIES;
    }
    return categories;
}

s32 HotkeyButton() {
    switch (CVarGetInteger(CVAR_MAGNET_HOTKEY, 1)) {
        case 1:
            return BUTTON_D_UP;
        case 2:
            return BUTTON_D_DOWN;
        case 3:
            return BUTTON_D_LEFT;
        case 4:
            return BUTTON_D_RIGHT;
        default:
            return -1;
    }
}

f32 ClampRange(f32 range) {
    if (range < kRangeMin) {
        return kRangeMin;
    }
    if (range > kRangeMax) {
        return kRangeMax;
    }
    return range;
}

u32 MarkerCategory(u32 markerId) {
    switch (markerId) {
        case MARKER_5F_MUSIC_NOTE:
            return MAGNET_NOTES;

        case MARKER_60_BLUE_EGG_COLLECTIBLE:
        case MARKER_B5_RED_FEATHER_COLLECTIBLE:
        case MARKER_1E5_GOLD_FEATHER_COLLECTIBLE:
            return MAGNET_EGGS_FEATHERS;

        case MARKER_61_EXTRA_LIFE:
        case MARKER_53_EMPTY_HONEYCOMB:
        case MARKER_55_HONEYCOMB:
            return MAGNET_LIVES_HONEYCOMBS;

        case MARKER_39_MUMBO_TOKEN:
            return MAGNET_TOKENS;

        case MARKER_52_JIGGY:
            return MAGNET_JIGGIES;

        default:
            return 0;
    }
}

u32 SpriteCategory(s32 assetId) {
    switch (assetId) {
        case ASSET_6D6_SPRITE_MUSIC_NOTE:
            return MAGNET_NOTES;

        case ASSET_6D7_SPRITE_BLUE_EGGS:
        case ASSET_580_SPRITE_RED_FEATHER:
        case ASSET_6D1_SPRITE_GOLDFEATHER:
            return MAGNET_EGGS_FEATHERS;

        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------

f32 Distance(const f32 from[3], const f32 to[3]) {
    const f32 offset[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    return gu_sqrtf(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
}

// Moves `position` up to `maxStep` units toward `destination`. Returns true once it arrives.
bool MoveToward(f32 position[3], const f32 destination[3], f32 distance, f32 maxStep) {
    if (distance <= kSnapDistance || maxStep >= distance) {
        position[0] = destination[0];
        position[1] = destination[1];
        position[2] = destination[2];
        return true;
    }

    const f32 scale = maxStep / distance;
    for (s32 i = 0; i < 3; i++) {
        position[i] += (destination[i] - position[i]) * scale;
    }
    return false;
}

// Advances `position` toward the player, `distance` units away from it.
void PullStep(f32 position[3], const f32 target[3], f32 distance, f32 deltaTime) {
    // Ease in as the collectible closes on the player.
    const f32 closeness = 1.0f - (distance / sActiveRange);
    const f32 speed = sActiveSpeed * (kMinSpeedFraction + (1.0f - kMinSpeedFraction) * closeness * closeness);

    MoveToward(position, target, distance, speed * deltaTime);
}

s16 ClampS16(f32 value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (s16)value;
}

void LoadPropPosition(const Prop* prop, f32 position[3]) {
    position[0] = (f32)prop->unk4[0];
    position[1] = (f32)prop->unk4[1];
    position[2] = (f32)prop->unk4[2];
}

void StorePropPosition(Prop* prop, const f32 position[3]) {
    prop->unk4[0] = ClampS16(position[0]);
    prop->unk4[1] = ClampS16(position[1]);
    prop->unk4[2] = ClampS16(position[2]);
}

// ---------------------------------------------------------------------------
// Displaced sprite registry
// ---------------------------------------------------------------------------

DisplacedSprite* FindDisplaced(const Cube* cube, s32 assetId, const Prop* prop) {
    for (DisplacedSprite& entry : sDisplaced) {
        if (entry.cube == cube && entry.assetId == assetId && entry.current[0] == prop->unk4[0] &&
            entry.current[1] == prop->unk4[1] && entry.current[2] == prop->unk4[2]) {
            return &entry;
        }
    }
    return nullptr;
}

DisplacedSprite& TrackDisplaced(Cube* cube, s32 assetId, const Prop* prop) {
    DisplacedSprite entry{};
    entry.cube = cube;
    entry.assetId = assetId;
    for (s32 i = 0; i < 3; i++) {
        entry.origin[i] = prop->unk4[i];
        entry.current[i] = prop->unk4[i];
    }
    entry.unseenFrames = 0;
    entry.restored = false;
    sDisplaced.push_back(entry);
    return sDisplaced.back();
}

void UpdateDisplacedPosition(DisplacedSprite& entry, const Prop* prop) {
    for (s32 i = 0; i < 3; i++) {
        entry.current[i] = prop->unk4[i];
    }
    entry.unseenFrames = 0;
}

// Last resort for an entry we stopped seeing: find the prop by its last known position in the
// cube we left it in and put it straight back, so nothing stays displaced for the rest of the
// visit. A prop that was collected in the meantime keeps its coordinates with its alive bit
// cleared, so writing the origin back to it is harmless.
void SnapBack(const DisplacedSprite& entry) {
    Cube* cube = entry.cube;
    if (cube == nullptr || cube->prop2Ptr == nullptr) {
        return;
    }

    for (s32 i = 0; i < (s32)cube->prop2Cnt; i++) {
        Prop* prop = &cube->prop2Ptr[i];
        if (prop->markerFlag || prop->unk8_1) {
            continue;
        }
        if (prop->unk4[0] == entry.current[0] && prop->unk4[1] == entry.current[1] &&
            prop->unk4[2] == entry.current[2]) {
            prop->unk4[0] = entry.origin[0];
            prop->unk4[1] = entry.origin[1];
            prop->unk4[2] = entry.origin[2];
            return;
        }
    }
}

// Drops entries whose prop was not seen for a while: collected, despawned, or left behind.
void AgeDisplacedEntries() {
    for (size_t i = sDisplaced.size(); i > 0; i--) {
        DisplacedSprite& entry = sDisplaced[i - 1];
        if (++entry.unseenFrames > kDisplacedEntryTimeout) {
            if (!entry.restored) {
                SnapBack(entry);
            }
            sDisplaced.erase(sDisplaced.begin() + (i - 1));
        }
    }
}

// ---------------------------------------------------------------------------
// Per-prop handling
// ---------------------------------------------------------------------------

void ReturnSpriteProp(Prop* prop, DisplacedSprite& entry, f32 deltaTime) {
    f32 position[3];
    LoadPropPosition(prop, position);

    const f32 origin[3] = { (f32)entry.origin[0], (f32)entry.origin[1], (f32)entry.origin[2] };
    const f32 distance = Distance(position, origin);
    const bool arrived = MoveToward(position, origin, distance, sActiveSpeed * kReturnSpeedFraction * deltaTime);

    StorePropPosition(prop, position);
    UpdateDisplacedPosition(entry, prop);

    if (arrived) {
        entry.restored = true;
        entry.unseenFrames = kDisplacedEntryTimeout; // aged out at the end of this frame
    }
}

void PullSpriteProp(Cube* cube, Prop* prop, s32 assetId, const f32 target[3], f32 deltaTime, bool pulling) {
    DisplacedSprite* entry = FindDisplaced(cube, assetId, prop);

    if (!pulling) {
        if (entry != nullptr) {
            ReturnSpriteProp(prop, *entry, deltaTime);
        }
        return;
    }

    f32 position[3];
    LoadPropPosition(prop, position);

    const f32 distance = Distance(position, target);
    if (distance > sActiveRange) {
        if (entry != nullptr) {
            ReturnSpriteProp(prop, *entry, deltaTime);
        }
        return;
    }

    if (entry == nullptr) {
        entry = &TrackDisplaced(cube, assetId, prop);
    }

    PullStep(position, target, distance, deltaTime);
    StorePropPosition(prop, position);
    UpdateDisplacedPosition(*entry, prop);
}

void ScanCube(Cube* cube, const f32 target[3], f32 deltaTime, bool pulling, u32 categories, bool allowSprites) {
    const s32 propCount = cube->prop2Cnt;
    Prop* prop = cube->prop2Ptr;
    if (prop == nullptr || propCount == 0) {
        return;
    }

    for (s32 i = 0; i < propCount; i++, prop++) {
        // Cleared once a collectible has been taken, despawned or suppressed on load.
        if (!prop->unk8_4) {
            continue;
        }

        if (prop->markerFlag) {
            if (!pulling) {
                continue;
            }

            ActorMarker* marker = prop->actorProp.marker;
            if (marker == nullptr || marker->unk5C == 0) {
                continue;
            }
            // Only pull things that can actually be picked up right now.
            if (!marker->collidable) {
                continue;
            }
            if ((MarkerCategory(marker->id) & categories) == 0) {
                continue;
            }
            if (marker->unk3E_0) {
                Actor* actor = marker_getActor(marker);
                if (actor == nullptr || actor->despawn_flag || actor->is_bundle) {
                    continue;
                }
            }

            f32 position[3];
            LoadPropPosition(prop, position);
            if (Distance(position, target) > sActiveRange) {
                continue;
            }

            // Deferred: moving a marker can realloc or reorder this cube's prop array.
            if ((s32)sPending.size() < kMaxPendingMarkers) {
                sPending.push_back(marker);
            }
        } else if (allowSprites && !prop->unk8_1) {
            const s32 assetId = (s32)prop->spriteProp.spriteId + kSpriteAssetBase;
            const u32 spriteCategory = SpriteCategory(assetId);
            if (spriteCategory == 0) {
                continue;
            }
            PullSpriteProp(cube, prop, assetId, target, deltaTime, pulling && (spriteCategory & categories) != 0);
        }
    }
}

void ApplyPending(const f32 target[3], f32 deltaTime) {
    for (ActorMarker* marker : sPending) {
        if (marker->unk5C == 0 || marker->propPtr == nullptr) {
            continue;
        }

        Actor* actor = marker->unk3E_0 ? marker_getActor(marker) : nullptr;

        f32 position[3];
        if (actor != nullptr) {
            position[0] = actor->position[0];
            position[1] = actor->position[1];
            position[2] = actor->position[2];
        } else {
            position[0] = (f32)marker->propPtr->x;
            position[1] = (f32)marker->propPtr->y;
            position[2] = (f32)marker->propPtr->z;
        }

        const f32 distance = Distance(position, target);
        if (distance > sActiveRange) {
            continue;
        }
        PullStep(position, target, distance, deltaTime);

        if (actor != nullptr) {
            actor->position[0] = position[0];
            actor->position[1] = position[1];
            actor->position[2] = position[2];
            // Keep the actor's own physics from fighting the magnet.
            actor->velocity[0] = 0.0f;
            actor->velocity[1] = 0.0f;
            actor->velocity[2] = 0.0f;
        }

        // Re-buckets the prop when it crosses a cube border.
        func_8032F64C(position, marker);
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void PlayFeedback(enum sfx_e sfxId) {
    func_8030E6D4(sfxId);
}

void AdjustRange(f32 amount) {
    const f32 newRange = ClampRange(sActiveRange + amount);
    if (newRange == sActiveRange) {
        return;
    }

    sActiveRange = newRange;
    CVarSetFloat(CVAR_MAGNET_RANGE, sActiveRange);
    CVarSave();
    PlayFeedback(SFX_145_SINGLE_CAMERA_CLICK);
}

void HandleInput() {
    const s32 hotkey = HotkeyButton();
    if (hotkey >= 0 && bakey_pressed(hotkey)) {
        sMagnetOn = !sMagnetOn;
        PlayFeedback(sMagnetOn ? SFX_CE_PAUSEMENU_HOIP : SFX_CF_PAUSEMENU_SHWOOP);
    }

    if (!CVarGetInteger(CVAR_MAGNET_DPAD_RANGE, 1)) {
        return;
    }

    if (hotkey != BUTTON_D_RIGHT && bakey_pressed(BUTTON_D_RIGHT)) {
        AdjustRange(kRangeStep);
    }
    if (hotkey != BUTTON_D_LEFT && bakey_pressed(BUTTON_D_LEFT)) {
        AdjustRange(-kRangeStep);
    }
}

// ---------------------------------------------------------------------------
// Frame update
// ---------------------------------------------------------------------------

void Update() {
    const bool enabled = CVarGetInteger(CVAR_MAGNET, 0) != 0;
    if (!enabled && sDisplaced.empty()) {
        return;
    }

    // Regular gameplay only: keeps the magnet out of the pause menu, file select, the attract
    // demos and any state where the cube list is not live.
    if (getGameMode() != GAME_MODE_3_NORMAL) {
        return;
    }

    sActiveRange = ClampRange(CVarGetFloat(CVAR_MAGNET_RANGE, kDefaultRange));
    sActiveSpeed = CVarGetFloat(CVAR_MAGNET_SPEED, kDefaultSpeed);

    if (enabled) {
        HandleInput();
    }

    f32 deltaTime = time_getDelta();
    if (deltaTime <= 0.0f) {
        return;
    }
    if (deltaTime > kMaxDeltaTime) {
        deltaTime = kMaxDeltaTime;
    }

    const u32 categories = Categories();
    const bool pulling = enabled && sMagnetOn && categories != 0;

    f32 playerPosition[3];
    player_getPosition(playerPosition);
    const f32 target[3] = { playerPosition[0], playerPosition[1] + kTargetHeightOffset, playerPosition[2] };

    sPending.clear();

    s32 reach = kSpriteCubeReach;
    if (pulling) {
        // Cover every cube that can hold a collectible inside the field.
        reach = (s32)(sActiveRange / (f32)kCubeSize) + 1;
        if (reach > kMaxCubeReach) {
            reach = kMaxCubeReach;
        } else if (reach < kSpriteCubeReach) {
            reach = kSpriteCubeReach;
        }
    }

    Cube* fallbackCube = func_80303658();

    for (s32 z = -reach; z <= reach; z++) {
        for (s32 y = -reach; y <= reach; y++) {
            for (s32 x = -reach; x <= reach; x++) {
                s32 sample[3] = { (s32)playerPosition[0] + x * kCubeSize, (s32)playerPosition[1] + y * kCubeSize,
                                  (s32)playerPosition[2] + z * kCubeSize };

                Cube* cube = cubeList_GetCubeAtPosition_s32(sample);
                if (cube == nullptr || cube == fallbackCube) {
                    continue;
                }

                // Static sprite props stay in their own cube, so they may only be moved while
                // that cube is part of the player's prop collision neighbourhood.
                const bool allowSprites = (x >= -kSpriteCubeReach && x <= kSpriteCubeReach) &&
                                          (y >= -kSpriteCubeReach && y <= kSpriteCubeReach) &&
                                          (z >= -kSpriteCubeReach && z <= kSpriteCubeReach);

                ScanCube(cube, target, deltaTime, pulling, categories, allowSprites);
            }
        }
    }

    // Markers that live outside the cube grid (always-loaded props).
    if (pulling) {
        Cube* globalCube = func_8030364C();
        if (globalCube != nullptr && globalCube != fallbackCube) {
            ScanCube(globalCube, target, deltaTime, pulling, categories, false);
        }
    }

    ApplyPending(target, deltaTime);
    AgeDisplacedEntries();
}

} // namespace

void RegisterCollectibleMagnet_Init() {
    sMagnetOn = true;
    sDisplaced.clear();
    sPending.clear();

    REGISTER_LISTENER(GameFrameUpdate, EVENT_PRIORITY_NORMAL, [](IEvent*) { Update(); });

    // Cube pointers and prop arrays are rebuilt on every map load, so nothing tracked survives it.
    // Props respawn at their authored positions, so there is nothing to put back either.
    REGISTER_LISTENER(OnMapLoad, EVENT_PRIORITY_NORMAL, [](IEvent*) {
        sDisplaced.clear();
        sPending.clear();
    });
}

static RegisterShipInitFunc initFunc(RegisterCollectibleMagnet_Init);
