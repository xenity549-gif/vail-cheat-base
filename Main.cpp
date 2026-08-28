#include <Windows.h>
#include <fstream>
#include <string>
#include <unordered_map>
#include <iostream>
#include "include/MinHook.h"
#include "SDK/Engine_classes.hpp"
#include "SDK/BP_Firearm_classes.hpp"
#include "SDK/VAIL_classes.hpp"
#include "Strings.h"

static HANDLE g_Console = nullptr;

typedef void(__fastcall* ProcessEventFn)(SDK::UObject*, SDK::UFunction*, void*);
static ProcessEventFn OriginalProcessEvent = nullptr;

static bool bNoRecoil = true;
static bool bInfAmmo = true;

static SDK::UWorld* g_LastWorld = nullptr;
static SDK::ACVRCharacter* g_LocalPawn = nullptr;

static bool g_combatOwnHooked = false;
static void* g_origCombatOwnExec = nullptr;
static bool g_combatOwnershipEnabled = true;

static void ConsolePrint(const std::string& msg)
{
    if (g_Console) {
        DWORD written;
        WriteConsoleA(g_Console, msg.c_str(), (DWORD)msg.size(), &written, nullptr);
    }
    OutputDebugStringA(msg.c_str());
}

static uintptr_t ScanProcessEvent()
{
    uintptr_t base = (uintptr_t)GetModuleHandle(nullptr);
    return base + SDK::Offsets::ProcessEvent;
}

static void ApplyNoRecoil(SDK::ABP_Firearm_C* Gun)
{
    if (!Gun) return;

    Gun->LocationRecoilAmount = 0;
    Gun->RotationHorizontalRecoil = 0;
    Gun->RotationRecoilRate = 0;
    Gun->LocationRecoilDelay = 0;
    Gun->LocationRecoilRate = 0;
    Gun->RotationRecoilRate = 0;
    Gun->RotationRollRecoil = 0;
    Gun->RotationVerticalRecoil = 0;
    Gun->MovementPenalty = -100.0f;
}

static void ApplyInfAmmo(SDK::ACMagazine* Mag)
{
    if (!Mag) return;

    if (Mag->Ammo < 30 && Mag->Ammo > 0)
    {
        Mag->Ammo = 30;
    }
}

static void ApplyFeaturesToAllActors()
{
    if (!g_LocalPawn) return;

    auto* World = SDK::UWorld::GetWorld();
    if (!World || !World->PersistentLevel) return;

    auto& Actors = World->PersistentLevel->Actors;
    int GunCount = 0;
    int MagCount = 0;

    for (int i = 0; i < Actors.Num(); i++)
    {
        SDK::AActor* Actor = Actors[i];
        if (!Actor || !Actor->Class) continue;

        if (bNoRecoil && Actor->IsA(SDK::ABP_Firearm_C::StaticClass()))
        {
            auto* Gun = static_cast<SDK::ABP_Firearm_C*>(Actor);
            ApplyNoRecoil(Gun);
            GunCount++;
        }

        if (bInfAmmo && Actor->IsA(SDK::ACMagazine::StaticClass()))
        {
            auto* Mag = static_cast<SDK::ACMagazine*>(Actor);
            ApplyInfAmmo(Mag);
            MagCount++;
        }
    }

    if (GunCount > 0 || MagCount > 0)
    {
        ConsolePrint("[VAIL] Applied - Guns: " + std::to_string(GunCount) + ", Mags: " + std::to_string(MagCount) + "\n");
    }
}

void Hook_GetVAILCombatOwnership(SDK::UObject* ctx, void* stack, void* result)
{
    try
    {
        if (g_origCombatOwnExec && ctx)
        {
            auto orig = reinterpret_cast<void(*)(SDK::UObject*, void*, void*)>(g_origCombatOwnExec);
            orig(ctx, stack, result);
        }

        if (result && g_combatOwnershipEnabled && !IsBadWritePtr(result, sizeof(int)))
            *static_cast<int*>(result) = 2;
    }
    catch (...)
    {
    }
}

static void InstallCombatOwnershipHook()
{
    if (g_combatOwnHooked) return;

    try
    {
        auto& w = SDK::UObject::GObjects;
        const int num = w->Num();
        if (num <= 0 || num > 1000000) return;

        SDK::UObject* clsObj = nullptr;

        for (int i = 0; i < num; ++i)
        {
            try
            {
                SDK::UObject* o = w->GetByIndex(i);
                if (!o || IsBadReadPtr(o, sizeof(SDK::UObject))) continue;
                if (!o->HasTypeFlag(SDK::EClassCastFlags::Class)) continue;

                if (o->GetName() == "CABClientSubsystem")
                {
                    clsObj = o;
                    break;
                }
            }
            catch (...)
            {
                continue;
            }
        }

        if (!clsObj)
        {
            return;
        }

        SDK::UObject* fn = nullptr;

        for (int i = 0; i < num; ++i)
        {
            try
            {
                SDK::UObject* o = w->GetByIndex(i);
                if (!o || IsBadReadPtr(o, sizeof(SDK::UObject))) continue;
                if (o->Outer != clsObj) continue;

                if (o->GetName() == "GetVAILCombatOwnership")
                {
                    fn = o;
                    break;
                }
            }
            catch (...)
            {
                continue;
            }
        }

        if (!fn)
        {
            return;
        }

        void** execSlot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(fn) + 0xD8);
        if (IsBadReadPtr(execSlot, sizeof(void*))) return;

        g_origCombatOwnExec = *execSlot;
        *execSlot = reinterpret_cast<void*>(&Hook_GetVAILCombatOwnership);

        g_combatOwnHooked = true;
    }
    catch (...)
    {
    }
}

void ApplyDevUnlock(SDK::APlayerController* pc)
{
    if (!pc) return;

    InstallCombatOwnershipHook();
    SDK::APlayerState* psBase = pc->PlayerState;

    if (!psBase) return;

    auto* ps = reinterpret_cast<SDK::ACPlayerState*>(psBase);
    uint8_t* p = reinterpret_cast<uint8_t*>(ps);
    constexpr int SUB_ONYX = 5;
    *(p + 0x0390) = SUB_ONYX;
    *reinterpret_cast<bool*>(p + 0x0400) = true;
}

void __fastcall HookedProcessEvent(SDK::UObject* pObject, SDK::UFunction* pFunction, void* pParams)
{
    static SDK::UWorld* World = nullptr;
    static int frameCounter = 0;
    static DWORD lastApplyTime = 0;

    frameCounter++;

    World = SDK::UWorld::GetWorld();

    if (World != g_LastWorld)
    {
        g_LastWorld = World;
        ConsolePrint("[VAIL] World changed\n");
        g_LocalPawn = nullptr;
    }

    if (!g_LocalPawn && World && World->OwningGameInstance)
    {
        auto* LP = World->OwningGameInstance->LocalPlayers[0];
        if (LP && LP->PlayerController && LP->PlayerController->Pawn)
        {
            auto* Pawn = LP->PlayerController->Pawn;
            if (Pawn->IsA(SDK::ACVRCharacter::StaticClass()))
            {
                g_LocalPawn = static_cast<SDK::ACVRCharacter*>(Pawn);
                ConsolePrint("[VAIL] Local player found\n");
                ApplyFeaturesToAllActors();
            }
        }
    }

    if (bNoRecoil)
    {
        DWORD currentTime = GetTickCount();
        if (currentTime - lastApplyTime >= 1000)
        {
            lastApplyTime = currentTime;

            auto* World2 = SDK::UWorld::GetWorld();
            if (World2 && World2->PersistentLevel)
            {
                auto& Actors = World2->PersistentLevel->Actors;
                int GunCount = 0;

                for (int i = 0; i < Actors.Num(); i++)
                {
                    SDK::AActor* Actor = Actors[i];
                    if (Actor && Actor->Class && Actor->IsA(SDK::ABP_Firearm_C::StaticClass()))
                    {
                        ApplyNoRecoil(static_cast<SDK::ABP_Firearm_C*>(Actor));
                        GunCount++;
                    }
                }

                if (GunCount > 0)
                {
                    ConsolePrint("[VAIL] No Recoil applied to " + std::to_string(GunCount) + " guns\n");
                }
            }
        }
    }

    if (bNoRecoil && pObject && pObject->IsA(SDK::ABP_Firearm_C::StaticClass()))
    {
        ApplyNoRecoil(static_cast<SDK::ABP_Firearm_C*>(pObject));
    }

    if (g_LocalPawn && bInfAmmo && pObject && pObject->IsA(SDK::ACMagazine::StaticClass()))
    {
        auto* Mag = static_cast<SDK::ACMagazine*>(pObject);
        ApplyInfAmmo(Mag);
    }

    if (bInfAmmo && g_LocalPawn && frameCounter % 10 == 0)
    {
        auto* World2 = SDK::UWorld::GetWorld();
        if (World2 && World2->PersistentLevel)
        {
            auto& Actors = World2->PersistentLevel->Actors;
            for (int i = 0; i < Actors.Num(); i++)
            {
                SDK::AActor* Actor = Actors[i];
                if (Actor && Actor->Class && Actor->IsA(SDK::ACMagazine::StaticClass()))
                {
                    auto* Mag = static_cast<SDK::ACMagazine*>(Actor);
                    ApplyInfAmmo(Mag);
                }
            }
        }
    }

    if (World && World->OwningGameInstance && World->OwningGameInstance->LocalPlayers.Num() > 0)
    {
        auto* LP = World->OwningGameInstance->LocalPlayers[0];
        if (LP && LP->PlayerController)
        {
            ApplyDevUnlock(LP->PlayerController);
        }
    }

    if (GetAsyncKeyState(VK_END) & 1)
    {
        ConsolePrint("[VAIL] Exiting...\n");
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        FreeConsole();
        return;
    }

    OriginalProcessEvent(pObject, pFunction, pParams);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        AllocConsole();
        SetConsoleTitleA("VAIL Console");
        g_Console = GetStdHandle(STD_OUTPUT_HANDLE);

        ConsolePrint("[VAIL] Loaded\n");
        ConsolePrint("[VAIL] No Recoil is on\n");
        ConsolePrint("[VAIL] Infinite Ammo is on\n");
        ConsolePrint("[VAIL] Combat Unlocker is on\n");
        ConsolePrint("[VAIL] END - Exit\n");

        if (MH_Initialize() == MH_OK)
        {
            ConsolePrint("[VAIL] MinHook initialized\n");

            uintptr_t peAddr = ScanProcessEvent();
            ConsolePrint("[VAIL] ProcessEvent at: 0x" + std::to_string(peAddr) + "\n");

            if (MH_CreateHook((void*)peAddr, &HookedProcessEvent, (void**)&OriginalProcessEvent) == MH_OK)
            {
                MH_EnableHook((void*)peAddr);
                ConsolePrint("[VAIL] ProcessEvent is hooked\n");
            }
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_Console) FreeConsole();
    }

    return TRUE;
}
