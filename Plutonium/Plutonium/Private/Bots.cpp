#include "pch.h"
#include "../Public/Bots.h"
#include "../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include "../../FortniteGame/Public/FortKismetLibrary.h"
#include "../../FortniteGame/Public/FortLootPackage.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../../FortniteGame/Public/FortWeapon.h"
#include "../Public/Configuration.h"
#include <random>

extern uint64_t ApplyCharacterCustomization;
extern uint64_t NotifyGameMemberAdded_;

// thin reflection wrappers for the engine/AI bits plutonium doesn't define yet.
// everything is resolved by name at runtime, so these work across builds.
class UBotController : public AActor
{
public:
    DEFINE_PROP(Pawn, AActor*);
    DEFINE_PROP(PlayerState, AFortPlayerStateAthena*);
    DEFINE_PROP(BrainComponent, UObject*);

    DEFINE_FUNC(Possess, void);
    DEFINE_FUNC(SetControlRotation, void);
    DEFINE_FUNC(K2_SetFocus, void);
    DEFINE_FUNC(K2_ClearFocus, void);
    DEFINE_FUNC(LineOfSightTo, bool);
    DEFINE_FUNC(OnRep_PlayerState, void);
};

class UBotBrain : public UObject
{
public:
    DEFINE_FUNC(StopLogic, void);
    DEFINE_FUNC(RestartLogic, void);
};

class UBotMovement : public UCharacterMovementComponent
{
public:
    DEFINE_FUNC(DisableMovement, void);
    DEFINE_FUNC(IsMovingOnGround, bool);
    DEFINE_FUNC(IsFalling, bool);
};

class ABotPawn : public AFortPlayerPawnAthena
{
public:
    DEFINE_BITFIELD_PROP(bIsDying);

    DEFINE_FUNC(AddMovementInput, void);
    DEFINE_FUNC(Jump, void);
    DEFINE_FUNC(PawnStartFire, void);
    DEFINE_FUNC(PawnStopFire, void);
    DEFINE_FUNC(IsDead, bool);
    DEFINE_FUNC(ForceKill, void);
    DEFINE_FUNC(SetActorEnableCollision, void);
    DEFINE_FUNC(IsParachuteOpen, bool);
};

class ABotWeapon : public AActor
{
public:
    DEFINE_PROP(AmmoCount, int32);
    DEFINE_PROP(WeaponData, UFortWeaponItemDefinition*);
    DEFINE_FUNC(GetBulletsPerClip, int32);
};

class UAthenaAIServicePlayerBots : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UAthenaAIServicePlayerBots);

    DEFINE_PROP(CachedGameMode, AActor*);
    DEFINE_PROP(CachedGameState, AActor*);
    DEFINE_PROP(AIServiceManager, UObject*);
    DEFINE_PROP(DefaultAISpawnerDataComponentList, UObject*);
    DEFINE_PROP(DefaultBotAISpawnerData, const UClass*);
    DEFINE_BITFIELD_PROP(bDoSpawnBotFlow);

    DEFINE_FUNC(SpawnAI, AActor*);
    DEFINE_FUNC(OnPlaylistDataReady, void);
    DEFINE_FUNC(OnGamePhaseLogicReady, void);
    DEFINE_FUNC(OnGamePhaseChanged, void);
    DEFINE_FUNC(OnSafeZoneUpdated, void);
};

class UFortAthenaAISpawnerData : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortAthenaAISpawnerData);

    DEFINE_STATIC_FUNC(CreateComponentListFromClass, UObject*);
};

class AFortPlayerStateAthenaBot : public AFortPlayerStateAthena
{
public:
    DEFINE_FUNC(OnRep_TeamIndex, void);
};

enum class EBotState : uint8
{
    Warmup,
    OnBus,
    Skydiving,
    Grounded, // native AI is in control (or plutonium AI in fallback mode)
    Dead
};

struct FBotItem
{
    const UFortItemDefinition* Definition = nullptr;
    int32 Count = 1;
    int32 LoadedAmmo = 0;
};

struct FBot
{
    FWeakObjectPtr Pawn;
    FWeakObjectPtr Controller;
    FWeakObjectPtr PlayerState;
    bool bNative = false;
    EBotState State = EBotState::Warmup;

    // bus
    double JumpTime = -1;
    FVector LandingTarget;
    double SkydiveStartTime = 0;

    // death
    double DeathTime = 0;
    bool bHandledElimination = false;

    // storm
    double NextStormDamageTime = 0;

    // plutonium AI
    std::vector<FBotItem> Items;
    int32 EquippedItem = -1;
    FWeakObjectPtr Target;
    FWeakObjectPtr LootTarget;
    FWeakObjectPtr LastAttacker;
    FVector MoveGoal;
    bool bHasMoveGoal = false;
    double NextThinkTime = 0;
    double NextFireToggleTime = 0;
    bool bFiring = false;
    double ReloadEndTime = 0;
    FVector LastStuckCheckLocation;
    double NextStuckCheckTime = 0;
    int32 StuckCount = 0;
    double StrafeUntil = 0;
    float StrafeDirection = 1.f;
};

static std::vector<FBot> AllBots;
static int32 SpawnedBots = 0;
static int32 BotNameIndex = 0;
static bool bNativeUnavailable = false;
static bool bDeclaredWinner = false;
static bool bCreatedService = false;
static uint8 LastGamePhase = 0;
static std::mt19937 Rng((uint32_t)time(nullptr));

static float RandRange(float Min, float Max)
{
    return std::uniform_real_distribution<float>(Min, Max)(Rng);
}

static int RandInt(int Min, int Max)
{
    return std::uniform_int_distribution<int>(Min, Max)(Rng);
}

static double Now()
{
    return UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
}

static FRotator MakeRotator(double Pitch, double Yaw, double Roll = 0)
{
    FRotator Rot{};
    Rot.Pitch = Pitch;
    Rot.Yaw = Yaw;
    Rot.Roll = Roll;
    return Rot;
}

static FRotator LookAt(const FVector& From, const FVector& To)
{
    auto Dir = To - From;
    double Dist2D = sqrt(Dir.X * Dir.X + Dir.Y * Dir.Y);
    return MakeRotator(atan2(Dir.Z, Dist2D) * 57.29577951308232, atan2(Dir.Y, Dir.X) * 57.29577951308232);
}

static double Dist2D(const FVector& A, const FVector& B)
{
    double X = A.X - B.X, Y = A.Y - B.Y;
    return sqrt(X * X + Y * Y);
}

template <typename T = AActor>
static T* GetWeak(const FWeakObjectPtr& Ptr)
{
    auto Actor = (T*)Ptr.Get();
    if (!Actor || Actor->bActorIsBeingDestroyed)
        return nullptr;
    return Actor;
}

static AFortGameMode* GetGameMode()
{
    auto World = UWorld::GetWorld();
    return World ? (AFortGameMode*)World->AuthorityGameMode : nullptr;
}

static UFortGameStateComponent_BattleRoyaleGamePhaseLogic* GetGamePhaseLogic()
{
    if (VersionInfo.FortniteVersion < 25.20)
        return nullptr;
    return UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());
}

static uint8 GetGamePhase()
{
    if (auto GamePhaseLogic = GetGamePhaseLogic())
    {
        static auto GamePhaseOffset = GamePhaseLogic->GetOffset("GamePhase");
        return *(uint8*)(__int64(GamePhaseLogic) + GamePhaseOffset);
    }

    auto GameMode = GetGameMode();
    return GameMode && GameMode->GameState ? GameMode->GameState->GamePhase : 0;
}

static AFortAthenaAircraft* GetAircraft()
{
    if (auto GamePhaseLogic = GetGamePhaseLogic())
        return GamePhaseLogic->Aircrafts_GameState.Num() > 0 ? GamePhaseLogic->Aircrafts_GameState[0].Get() : nullptr;

    auto GameState = GetGameMode()->GameState;
    if (GameState->HasAircrafts())
        return GameState->Aircrafts.Num() > 0 ? GameState->Aircrafts[0] : nullptr;
    return GameState->HasAircraft() ? GameState->Aircraft : nullptr;
}

static AFortSafeZoneIndicator* GetSafeZoneIndicator()
{
    if (auto GamePhaseLogic = GetGamePhaseLogic())
        return GamePhaseLogic->SafeZoneIndicator;
    return GetGameMode()->SafeZoneIndicator;
}

static bool IsInSafeZone(const FVector& Location)
{
    // same as the storm check plutonium does for players
    if (auto GamePhaseLogic = GetGamePhaseLogic())
    {
        bool bInZone = GamePhaseLogic->IsInCurrentSafeZone(Location, false);
        if (UFortGameStateComponent_BattleRoyaleGamePhaseLogic::IsInCurrentSafeZone__Ptr)
            return bInZone;
    }

    return GetGameMode()->IsInCurrentSafeZone(Location, false);
}

static bool IsController(const UObject* Object)
{
    static auto ControllerClass = FindClass("Controller");
    return Object && Object->IsA(ControllerClass);
}

static bool IsPawnDead(ABotPawn* Pawn)
{
    if (!Pawn || Pawn->bActorIsBeingDestroyed)
        return true;
    if (Pawn->HasbIsDying() && Pawn->bIsDying)
        return true;
    if (Pawn->IsDead())
        return true;
    return Pawn->GetHealth() <= 0.f;
}

static AFortPlayerStateAthena* GetControllerPlayerState(AActor* Controller)
{
    return IsController(Controller) ? ((UBotController*)Controller)->PlayerState : nullptr;
}

static uint8 GetTeam(AActor* Pawn)
{
    auto PlayerState = Pawn ? (AFortPlayerStateAthena*)((AFortPlayerPawnAthena*)Pawn)->PlayerState : nullptr;
    return PlayerState ? PlayerState->TeamIndex : 0;
}

static TArray<AActor*> GetActorsOfClass(const UClass* Class)
{
    TArray<AActor*> Actors;
    if (Class)
        Utils::GetAll(Class, Actors);
    return Actors;
}

static UBotBrain* GetBrain(AActor* Controller)
{
    if (!Controller)
        return nullptr;
    static auto BrainOffset = Controller->GetOffset("BrainComponent");
    if (BrainOffset == -1)
        return nullptr;
    return (UBotBrain*)((UBotController*)Controller)->BrainComponent;
}

static void SetBrainActive(FBot& Bot, bool bActive)
{
    auto Brain = GetBrain(GetWeak(Bot.Controller));
    if (!Brain)
        return;

    if (bActive)
        Brain->RestartLogic();
    else
    {
        FString Reason(L"PlutoniumBotBus");
        Brain->StopLogic(Reason);
    }
}

static UAthenaAIServicePlayerBots* GetPlayerBotService()
{
    static UAthenaAIServicePlayerBots* Service = nullptr;
    static bool bSearched = false;

    if (Service || bSearched)
        return Service;

    auto ServiceClass = UAthenaAIServicePlayerBots::StaticClass();
    if (!ServiceClass)
    {
        bSearched = true;
        printf("[Bots] AthenaAIServicePlayerBots doesn't exist on this build.\n");
        return nullptr;
    }

    auto World = UWorld::GetWorld();
    UObject* ServiceManager = nullptr;
    static auto AISystemOffset = World->GetOffset("AISystem");
    auto AISystem = AISystemOffset != -1 ? GetFromOffset<UObject*>(World, AISystemOffset) : nullptr;

    if (AISystem)
    {
        static auto ServiceManagerOffset = AISystem->GetOffset("AIServiceManager");
        ServiceManager = ServiceManagerOffset != -1 ? GetFromOffset<UObject*>(AISystem, ServiceManagerOffset) : nullptr;
    }

    if (ServiceManager)
    {
        static auto ServicesOffset = ServiceManager->GetOffset("AIServices");
        if (ServicesOffset != -1)
        {
            auto& Services = GetFromOffset<TArray<UObject*>>(ServiceManager, ServicesOffset);
            for (auto& AIService : Services)
                if (AIService && AIService->IsA(ServiceClass))
                    Service = (UAthenaAIServicePlayerBots*)AIService;

            if (!Service && FConfiguration::bCreateBotServiceIfMissing)
            {
                Service = (UAthenaAIServicePlayerBots*)UGameplayStatics::SpawnObject(ServiceClass, ServiceManager);
                if (Service)
                {
                    auto GameMode = GetGameMode();
                    Service->CachedGameMode = GameMode;
                    Service->CachedGameState = GameMode->GameState;
                    Service->AIServiceManager = ServiceManager;
                    Services.Add(Service);
                    bCreatedService = true;
                    printf("[Bots] Created AthenaAIServicePlayerBots.\n");
                }
            }
        }
    }

    if (!Service)
        Service = (UAthenaAIServicePlayerBots*)TUObjectArray::FindFirstObject("AthenaAIServicePlayerBots");

    if (Service)
    {
        // we decide how many bots there are, not the matchmaking table
        if (Service->HasbDoSpawnBotFlow())
            Service->bDoSpawnBotFlow = false;

        if (bCreatedService)
        {
            auto GameMode = GetGameMode();
            auto GameState = GameMode->GameState;
            auto Playlist = GameState->HasCurrentPlaylistInfo() ? GameState->CurrentPlaylistInfo.BasePlaylist : nullptr;
            FGameplayTagContainer PlaylistContextTags{};

            if (Playlist)
                Service->OnPlaylistDataReady(GameState, Playlist, PlaylistContextTags);
            if (auto GamePhaseLogic = GetGamePhaseLogic())
                Service->OnGamePhaseLogicReady(FWeakObjectPtr(GamePhaseLogic));
        }
    }
    else
        printf("[Bots] Couldn't find or create AthenaAIServicePlayerBots.\n");

    bSearched = true;
    return Service;
}

static const UClass* FindPlayerBotSpawnerData(UAthenaAIServicePlayerBots* Service)
{
    static auto ClassClass = FindClass("Class");

    if (FConfiguration::BotSpawnerData && *FConfiguration::BotSpawnerData)
    {
        if (auto SpawnerData = FindObject<UClass>(FConfiguration::BotSpawnerData, ClassClass))
            return SpawnerData;
        printf("[Bots] Configured BotSpawnerData couldn't be loaded, searching for one instead.\n");
    }

    if (Service->HasDefaultBotAISpawnerData() && Service->DefaultBotAISpawnerData)
        return Service->DefaultBotAISpawnerData;

    // any loaded blueprint subclass of FortAthenaPlayerBotSpawnerData will do
    auto PlayerBotSpawnerDataClass = FindClass("FortAthenaPlayerBotSpawnerData");
    if (!PlayerBotSpawnerDataClass)
        return nullptr;

    const UClass* Found = nullptr;
    for (int i = 0; i < TUObjectArray::Num(); i++)
    {
        auto Object = TUObjectArray::GetObjectByIndex(i);

        if (!Object || Object == PlayerBotSpawnerDataClass || !Object->IsA(ClassClass))
            continue;

        auto DefaultObject = ((const UClass*)Object)->GetDefaultObj();
        if (DefaultObject && DefaultObject->IsA(PlayerBotSpawnerDataClass))
        {
            Found = (const UClass*)Object;
            break;
        }
    }

    return Found ? Found : PlayerBotSpawnerDataClass;
}

static UObject* GetBotComponentList(UAthenaAIServicePlayerBots* Service)
{
    static UObject* ComponentList = nullptr;
    if (ComponentList)
        return ComponentList;

    if (Service->HasDefaultAISpawnerDataComponentList() && Service->DefaultAISpawnerDataComponentList)
        return ComponentList = Service->DefaultAISpawnerDataComponentList;

    auto SpawnerData = FindPlayerBotSpawnerData(Service);
    if (!SpawnerData)
        return nullptr;

    printf("[Bots] Using spawner data %s\n", SpawnerData->Name.ToString().c_str());
    ComponentList = UFortAthenaAISpawnerData::CreateComponentListFromClass(SpawnerData, Service);

    // keep it referenced so GC doesn't eat it
    if (ComponentList && Service->HasDefaultAISpawnerDataComponentList())
        Service->DefaultAISpawnerDataComponentList = ComponentList;

    return ComponentList;
}

static void AddGameMemberInfo(AFortGameStateAthena* GameState, AFortPlayerStateAthena* PlayerState)
{
    if (!GameState->HasGameMemberInfoArray())
        return;

    auto Member = (FGameMemberInfo*)malloc(FGameMemberInfo::Size());
    memset((PBYTE)Member, 0, FGameMemberInfo::Size());

    Member->MostRecentArrayReplicationKey = -1;
    Member->ReplicationID = -1;
    Member->ReplicationKey = -1;
    Member->TeamIndex = PlayerState->TeamIndex;
    Member->SquadId = PlayerState->SquadId;
    Member->MemberUniqueId = PlayerState->HasUniqueID() ? PlayerState->UniqueID : PlayerState->UniqueId;

    auto& NewMember = GameState->GameMemberInfoArray.Members.Add(*Member, FGameMemberInfo::Size());
    GameState->GameMemberInfoArray.MarkItemDirty(NewMember);

    auto NotifyGameMemberAdded = (void (*)(AFortGameStateAthena*, uint8_t, uint8_t, FUniqueNetIdRepl*))NotifyGameMemberAdded_;
    if (NotifyGameMemberAdded)
        NotifyGameMemberAdded(GameState, Member->SquadId, Member->TeamIndex, &Member->MemberUniqueId);

    free(Member);
}

// gives the bot its own team using plutonium's team picker so humans that join later never share a team with a bot
static void AssignTeam(AFortGameMode* GameMode, AFortPlayerStateAthena* PlayerState, AActor* Controller)
{
    auto OldTeam = PlayerState->TeamIndex;
    PlayerState->TeamIndex = AFortGameMode::PickTeam(GameMode, 0, (AFortPlayerControllerAthena*)Controller);
    ((AFortPlayerStateAthenaBot*)PlayerState)->OnRep_TeamIndex(OldTeam);

    if (PlayerState->HasSquadId())
    {
        PlayerState->SquadId = PlayerState->TeamIndex - 3;
        PlayerState->OnRep_SquadId();
    }

    if (PlayerState->HasbIsABot())
        PlayerState->bIsABot = true;

    AddGameMemberInfo(GameMode->GameState, PlayerState);
}

static FString MakeBotName()
{
    static const char* Prefixes[] = { "Shadow", "Blaze", "Frost", "Viper", "Nova", "Rogue", "Echo", "Pixel", "Turbo", "Ghost", "Storm", "Lunar", "Hyper", "Crimson", "Atomic", "Silent" };
    static const char* Suffixes[] = { "Wolf", "Sniper", "Fox", "Ninja", "Rider", "Hawk", "Byte", "King", "Llama", "Raven", "Bolt", "Drift", "Fury", "Spark", "Ace", "Tiger" };

    std::string Name = std::string(Prefixes[RandInt(0, 15)]) + Suffixes[RandInt(0, 15)];
    if (RandInt(0, 2) != 0)
        Name += std::to_string(RandInt(1, 9999));

    BotNameIndex++;
    return FString(std::wstring(Name.begin(), Name.end()).c_str());
}

static AActor* SpawnNativeBot(const FVector& Location)
{
    auto Service = GetPlayerBotService();
    if (!Service)
        return nullptr;

    auto ComponentList = GetBotComponentList(Service);
    if (!ComponentList)
    {
        printf("[Bots] No player bot spawner data was found.\n");
        return nullptr;
    }

    auto Rotation = MakeRotator(0, RandRange(-180.f, 180.f));
    auto Pawn = Service->SpawnAI(Location, Rotation, ComponentList);
    if (!Pawn)
        return nullptr;

    auto GameMode = GetGameMode();
    auto BotPawn = (ABotPawn*)Pawn;
    auto Controller = BotPawn->Controller;

    if (Controller)
    {
        auto PlayerState = GetControllerPlayerState(Controller);
        if (PlayerState)
            AssignTeam(GameMode, PlayerState, Controller);
    }

    return Pawn;
}

static void GiveBotItem(FBot& Bot, ABotPawn* Pawn, const UFortItemDefinition* Definition, int32 Count, int32 LoadedAmmo);

static AActor* SpawnPlutoniumBot(const FVector& Location)
{
    auto GameMode = GetGameMode();
    auto GameState = GameMode->GameState;

    static auto AIControllerClass = FindClass("AIController");
    if (!AIControllerClass)
        return nullptr;

    auto Rotation = MakeRotator(0, RandRange(-180.f, 180.f));
    auto Pawn = (ABotPawn*)UWorld::SpawnActor(GameMode->DefaultPawnClass, Location, Rotation);
    auto Controller = (UBotController*)UWorld::SpawnActor(AIControllerClass, Location, Rotation);

    if (!Pawn || !Controller)
    {
        if (Pawn)
            Pawn->K2_DestroyActor();
        if (Controller)
            Controller->K2_DestroyActor();
        return nullptr;
    }

    auto PlayerState = (AFortPlayerStateAthena*)UWorld::SpawnActor(AFortPlayerStateAthena::StaticClass(), Location, Rotation);
    PlayerState->SetOwner(Controller);
    Controller->PlayerState = PlayerState;
    Controller->OnRep_PlayerState();

    Controller->Possess(Pawn);

    Pawn->PlayerState = PlayerState;
    Pawn->OnRep_PlayerState();

    AssignTeam(GameMode, PlayerState, Controller);

    for (auto& AbilitySet : AFortGameMode::AbilitySets)
        PlayerState->AbilitySystemComponent->GiveAbilitySet(AbilitySet);

    static auto Commando = FindObject(L"/Game/Athena/Heroes/HID_001_Athena_Commando_F.HID_001_Athena_Commando_F", nullptr);
    static auto Commando2 = FindObject(L"/Game/Athena/Heroes/HID_Commando_Athena_01.HID_Commando_Athena_01", nullptr);
    if (PlayerState->HasHeroType())
        PlayerState->HeroType = Commando ? Commando : Commando2;

    static auto CharacterPartsOff = PlayerState->GetOffset("CharacterParts");
    if (CharacterPartsOff != -1)
    {
        static auto CustomCharacterPartsStruct = FindStruct("CustomCharacterParts");
        static auto PartsOffset = CustomCharacterPartsStruct ? CustomCharacterPartsStruct->GetOffset("Parts") : 0;
        auto& CharacterParts = GetFromOffset<const UObject* [0x6]>(PlayerState, CharacterPartsOff + PartsOffset);

        static auto Head = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1");
        static auto Body = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Bodies/F_Med_Soldier_01.F_Med_Soldier_01");
        static auto Backpack = FindObject<UObject>(L"/Game/Characters/CharacterParts/Backpacks/NoBackpack.NoBackpack");

        CharacterParts[0] = Head;
        CharacterParts[1] = Body;
        CharacterParts[3] = Backpack;
    }

    UFortKismetLibrary::UpdatePlayerCustomCharacterPartsVisualization(PlayerState);
    if (!UFortKismetLibrary::UpdatePlayerCustomCharacterPartsVisualization__Ptr && ApplyCharacterCustomization)
        ((void (*)(AActor*, AActor*))ApplyCharacterCustomization)(PlayerState, Pawn);

    GameMode->ChangeName(Controller, MakeBotName(), true);
    PlayerState->OnRep_PlayerName();

    Pawn->SetMaxHealth(100.f);
    Pawn->SetHealth(100.f);
    Pawn->SetShield(0.f);

    return Pawn;
}

AActor* Bots::SpawnBot(FVector Location)
{
    AActor* Pawn = nullptr;
    bool bNative = false;

    if (FConfiguration::BotMode != 2 && !bNativeUnavailable)
    {
        Pawn = SpawnNativeBot(Location);
        bNative = Pawn != nullptr;

        if (!Pawn && FConfiguration::BotMode == 0)
        {
            printf("[Bots] Native Fortnite bots unavailable, falling back to Plutonium AI.\n");
            bNativeUnavailable = true;
        }
    }

    if (!Pawn && FConfiguration::BotMode != 1)
        Pawn = SpawnPlutoniumBot(Location);

    if (!Pawn)
        return nullptr;

    auto BotPawn = (ABotPawn*)Pawn;

    FBot Bot;
    Bot.Pawn = FWeakObjectPtr(Pawn);
    Bot.Controller = FWeakObjectPtr(BotPawn->Controller);
    Bot.PlayerState = FWeakObjectPtr(BotPawn->PlayerState);
    Bot.bNative = bNative;
    Bot.NextThinkTime = Now() + RandRange(0.f, 0.5f);

    if (!bNative)
    {
        static auto DefaultPickaxe = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01");
        GiveBotItem(Bot, BotPawn, DefaultPickaxe, 1, 0);
    }

    AllBots.push_back(Bot);
    SpawnedBots++;

    return Pawn;
}

bool Bots::IsBotController(const UObject* Controller)
{
    if (!Controller)
        return false;

    for (auto& Bot : AllBots)
        if (Bot.Controller.Get() == Controller)
            return true;

    return false;
}

int32 Bots::GetAliveBotCount()
{
    int32 Count = 0;
    for (auto& Bot : AllBots)
        if (Bot.State != EBotState::Dead)
            Count++;
    return Count;
}

int32 Bots::GetSpawnedBotCount()
{
    return SpawnedBots;
}

static int32 GetHumanCount(AFortGameMode* GameMode)
{
    int32 Count = 0;
    for (auto& Player : GameMode->AlivePlayers)
        if (Player && Player->IsA<AFortPlayerControllerAthena>())
            Count++;
    return Count;
}

static FVector PickWarmupSpawn()
{
    static TArray<AActor*> Starts;
    if (Starts.Num() == 0)
    {
        Starts = GetActorsOfClass(FindClass("FortPlayerStartWarmup"));
        if (Starts.Num() == 0)
            Starts = GetActorsOfClass(FindClass("PlayerStart"));
    }

    if (Starts.Num() == 0)
        return FVector(0, 0, 10000);

    auto Location = Starts[RandInt(0, Starts.Num() - 1)]->K2_GetActorLocation();
    Location.X += RandRange(-400.f, 400.f);
    Location.Y += RandRange(-400.f, 400.f);
    Location.Z += 150.f;
    return Location;
}

static void SpawnPendingBots(AFortGameMode* GameMode, uint8 GamePhase)
{
    if ((EAthenaGamePhase)GamePhase != EAthenaGamePhase::Warmup)
        return;

    auto Humans = GetHumanCount(GameMode);
    if (FConfiguration::bBotsWaitForHuman && Humans == 0)
        return;

    int32 Wanted = FConfiguration::bBotsFillLobby ? FConfiguration::BotCount - Humans : FConfiguration::BotCount;
    int32 ToSpawn = std::min(Wanted - Bots::GetAliveBotCount(), FConfiguration::BotsPerTick);

    for (int32 i = 0; i < ToSpawn; i++)
    {
        if (!Bots::SpawnBot(PickWarmupSpawn()))
        {
            if (Bots::GetSpawnedBotCount() == 0)
                printf("[Bots] Failed to spawn a bot!\n");
            break;
        }
    }
}

// keeps plutonium's assumption that AlivePlayers only holds player controllers, and PlayersLeft counting bots too
static void SyncPlayerCounts(AFortGameMode* GameMode, uint8 GamePhase)
{
    auto& AlivePlayers = GameMode->AlivePlayers;
    for (int i = AlivePlayers.Num() - 1; i >= 0; i--)
        if (!AlivePlayers[i] || !AlivePlayers[i]->IsA<AFortPlayerControllerAthena>())
            AlivePlayers.Remove(i);

    if (!FConfiguration::bBotsSyncPlayersLeft || (EAthenaGamePhase)GamePhase < EAthenaGamePhase::Warmup)
        return;

    auto GameState = GameMode->GameState;
    auto Wanted = AlivePlayers.Num() + Bots::GetAliveBotCount();
    if (GameState->PlayersLeft != Wanted)
    {
        GameState->PlayersLeft = Wanted;
        GameState->OnRep_PlayersLeft();
    }
}

// ----- bus -----

static FVector BusStartLocation;
static double BusStartTime = -1;
static FVector BusVelocity;
static bool bBusVelocityKnown = false;
static TArray<AActor*> POIs;

static void PutBotsOnBus()
{
    for (auto& Bot : AllBots)
    {
        if (Bot.State == EBotState::Dead)
            continue;

        auto Pawn = GetWeak<ABotPawn>(Bot.Pawn);
        if (!Pawn)
            continue;

        auto Weapon = (AActor*)Pawn->CurrentWeapon;
        if (Weapon)
            Pawn->PawnStopFire(0);

        SetBrainActive(Bot, false);
        Pawn->SetActorHiddenInGame(true);
        Pawn->SetActorEnableCollision(false);
        if (auto Movement = (UBotMovement*)Pawn->CharacterMovement)
            Movement->DisableMovement();

        Pawn->SetHealth(100.f);
        Bot.State = EBotState::OnBus;
        Bot.JumpTime = -1;
        Bot.Target = FWeakObjectPtr();
        Bot.LootTarget = FWeakObjectPtr();
    }

    BusStartTime = -1;
    bBusVelocityKnown = false;
    POIs = GetActorsOfClass(FindClass("FortPoiVolume"));
    printf("[Bots] %d bots boarded the bus (%d POIs).\n", Bots::GetAliveBotCount(), POIs.Num());
}

// picks where the bot wants to land and when it has to jump to get there
static void PlanJump(FBot& Bot, AFortAthenaAircraft* Aircraft, double Time)
{
    double DropStart = Aircraft->DropStartTime, DropEnd = Aircraft->DropEndTime;
    if (DropEnd <= DropStart)
        DropEnd = DropStart + 30.0;

    double Speed = BusVelocity.Magnitude();
    auto Direction = BusVelocity / Speed;

    // most bots go to a named POI near the bus path, the rest drop wherever
    std::vector<AActor*> Candidates;
    for (auto& POI : POIs)
    {
        if (!POI)
            continue;

        auto Location = POI->K2_GetActorLocation();
        auto Offset = Location - BusStartLocation;
        double Along = Offset.X * Direction.X + Offset.Y * Direction.Y;
        double PassTime = BusStartTime + Along / Speed;
        auto Closest = BusStartLocation + Direction * Along;

        if (PassTime > DropStart + 1.0 && PassTime < DropEnd - 2.0 && Dist2D(Closest, Location) < FConfiguration::BotMaxGlideDistance)
            Candidates.push_back(POI);
    }

    if (!Candidates.empty() && RandRange(0.f, 1.f) < 0.85f)
    {
        auto Location = Candidates[RandInt(0, (int)Candidates.size() - 1)]->K2_GetActorLocation();
        Location.X += RandRange(-3000.f, 3000.f);
        Location.Y += RandRange(-3000.f, 3000.f);

        auto Offset = Location - BusStartLocation;
        double Along = Offset.X * Direction.X + Offset.Y * Direction.Y;
        Bot.JumpTime = std::clamp(BusStartTime + Along / Speed - RandRange(0.f, 3.f), DropStart + 0.5, DropEnd - 1.0);
        Bot.LandingTarget = Location;
    }
    else
    {
        Bot.JumpTime = RandRange((float)(DropStart + 0.5), (float)(DropEnd - 1.0));
        auto JumpLocation = BusStartLocation + BusVelocity * (Bot.JumpTime - BusStartTime);
        auto Side = FVector(-Direction.Y, Direction.X, 0) * RandRange(-FConfiguration::BotMaxGlideDistance * 0.8f, FConfiguration::BotMaxGlideDistance * 0.8f);
        Bot.LandingTarget = JumpLocation + Direction * RandRange(0.f, 20000.f) + Side;
    }

    if (Bot.JumpTime < Time)
        Bot.JumpTime = Time + RandRange(0.f, 2.f);
}

static void JumpFromBus(FBot& Bot, ABotPawn* Pawn, FVector Location)
{
    Location.X += RandRange(-500.f, 500.f);
    Location.Y += RandRange(-500.f, 500.f);
    Location.Z -= 600.f;

    Pawn->K2_SetActorLocation(Location, false, nullptr, true);
    Pawn->SetActorHiddenInGame(false);
    Pawn->SetActorEnableCollision(true);
    if (auto Movement = Pawn->CharacterMovement)
        Movement->SetMovementMode((uint8)3 /* MOVE_Falling */, (uint8)0);
    Pawn->BeginSkydiving(true);

    if (auto Controller = GetWeak<UBotController>(Bot.Controller))
        Controller->SetControlRotation(LookAt(Location, Bot.LandingTarget));

    Bot.State = EBotState::Skydiving;
    Bot.SkydiveStartTime = Now();
}

static void TickBus(double Time)
{
    auto Aircraft = GetAircraft();
    if (!Aircraft)
    {
        // bus is gone (flight ended), nobody stays on it
        if ((EAthenaGamePhase)GetGamePhase() < EAthenaGamePhase::SafeZones)
            return;

        for (auto& Bot : AllBots)
        {
            auto Pawn = GetWeak<ABotPawn>(Bot.Pawn);
            if (Bot.State != EBotState::OnBus || !Pawn)
                continue;

            FVector Location = bBusVelocityKnown ? BusStartLocation + BusVelocity * (Time - BusStartTime) : Pawn->K2_GetActorLocation() + FVector(0, 0, 15000);
            if (BusStartTime >= 0)
                Location.Z = BusStartLocation.Z;
            Bot.LandingTarget = Location;
            JumpFromBus(Bot, Pawn, Location);
        }
        return;
    }

    auto BusLocation = Aircraft->K2_GetActorLocation();
    if (BusStartTime < 0)
    {
        BusStartTime = Time;
        BusStartLocation = BusLocation;
        return;
    }

    if (!bBusVelocityKnown)
    {
        if (Time - BusStartTime < 1.0)
            return;

        BusVelocity = (BusLocation - BusStartLocation) / (Time - BusStartTime);
        BusVelocity.Z = 0;
        if (BusVelocity.Magnitude() < 1.0)
            return;
        bBusVelocityKnown = true;
    }

    for (auto& Bot : AllBots)
    {
        if (Bot.State != EBotState::OnBus)
            continue;

        auto Pawn = GetWeak<ABotPawn>(Bot.Pawn);
        if (!Pawn)
            continue;

        if (Bot.JumpTime < 0)
            PlanJump(Bot, Aircraft, Time);

        // everyone is out by the end of the drop window, just like real players get kicked
        if (Time >= Bot.JumpTime || (Aircraft->DropEndTime > 0 && Time >= Aircraft->DropEndTime - 0.5))
            JumpFromBus(Bot, Pawn, BusLocation);
    }
}

static void TickSkydive(FBot& Bot, ABotPawn* Pawn, double Time)
{
    auto Location = Pawn->K2_GetActorLocation();
    auto Movement = (UBotMovement*)Pawn->CharacterMovement;

    bool bLanded = !Pawn->bIsSkydiving && Movement && Movement->IsMovingOnGround();
    if (bLanded || Time - Bot.SkydiveStartTime > 180.0)
    {
        Bot.State = EBotState::Grounded;
        if (Bot.bNative)
            SetBrainActive(Bot, true);
        return;
    }

    auto Direction = Bot.LandingTarget - Location;
    Direction.Z = 0;
    double Distance = Direction.Magnitude();
    if (Distance > 800.0)
    {
        Direction = Direction / Distance;
        Pawn->AddMovementInput(Direction, 1.f, true);

        if (auto Controller = GetWeak<UBotController>(Bot.Controller))
            Controller->SetControlRotation(MakeRotator(0, atan2(Direction.Y, Direction.X) * 57.29577951308232));
    }
}

// ----- storm -----

static void TickStorm(FBot& Bot, ABotPawn* Pawn, double Time)
{
    if (Bot.bNative && !FConfiguration::bBotStormDamageNative)
        return;
    if (Time < Bot.NextStormDamageTime)
        return;

    Bot.NextStormDamageTime = Time + 1.0;

    auto SafeZoneIndicator = GetSafeZoneIndicator();
    if (!SafeZoneIndicator)
        return;

    bool bInZone = IsInSafeZone(Pawn->K2_GetActorLocation());
    if (Pawn->bIsInsideSafeZone != bInZone || Pawn->bIsInAnyStorm != !bInZone)
    {
        Pawn->bIsInAnyStorm = !bInZone;
        Pawn->OnRep_IsInAnyStorm();
        Pawn->bIsInsideSafeZone = bInZone;
        Pawn->OnRep_IsInsideSafeZone();
    }

    if (bInZone)
        return;

    float Damage = 1.f;
    if (SafeZoneIndicator->HasCurrentDamageInfo())
    {
        auto& DamageInfo = SafeZoneIndicator->CurrentDamageInfo;
        Damage = DamageInfo.Damage;
        if (DamageInfo.bPercentageBasedDamage)
            Damage *= 100.f;
    }
    if (Damage <= 0.f)
        Damage = 1.f;

    auto Health = Pawn->GetHealth() - Damage;
    if (Health <= 0.f)
        Pawn->ForceKill(FGameplayTag(), (AActor*)nullptr, (AActor*)nullptr);
    else
        Pawn->SetHealth(Health);
}

// ----- plutonium AI (fallback brain) -----

static TArray<AActor*> CachedPickups;
static TArray<AActor*> CachedContainers;
static double NextLootRefreshTime = 0;

static void RefreshLootCache(double Time)
{
    if (Time < NextLootRefreshTime)
        return;

    NextLootRefreshTime = Time + 1.0;
    CachedPickups.Free();
    CachedContainers.Free();
    CachedPickups = GetActorsOfClass(AFortPickupAthena::StaticClass());

    TArray<AActor*> Containers = GetActorsOfClass(ABuildingContainer::StaticClass());
    for (auto& Container : Containers)
        if (Container && !((ABuildingContainer*)Container)->bAlreadySearched && ((ABuildingContainer*)Container)->SearchLootTierGroup.ToString().find("Treasure") != std::string::npos)
            CachedContainers.Add(Container);
    Containers.Free();
}

static bool IsRangedWeapon(const UFortItemDefinition* Definition)
{
    static auto RangedClass = FindClass("FortWeaponRangedItemDefinition");
    return Definition && Definition->IsA(RangedClass);
}

static int32 ScoreItem(const UFortItemDefinition* Definition)
{
    if (!Definition)
        return -1;
    if (IsRangedWeapon(Definition))
        return 100 + Definition->Rarity * 10;
    if (Definition->IsA<UFortWeaponMeleeItemDefinition>())
        return 1;
    return -1;
}

static void EquipBestItem(FBot& Bot, ABotPawn* Pawn)
{
    int32 Best = -1, BestScore = -1;
    for (int32 i = 0; i < (int32)Bot.Items.size(); i++)
    {
        auto Score = ScoreItem(Bot.Items[i].Definition);
        if (Score > BestScore)
        {
            BestScore = Score;
            Best = i;
        }
    }

    if (Best == -1 || Best == Bot.EquippedItem)
        return;

    auto Definition = Bot.Items[Best].Definition;
    if (auto Gadget = Definition->Cast<UFortGadgetItemDefinition>())
        Definition = Gadget->GetWeaponItemDefinition();

    FGuid ItemGuid{}, TrackerGuid{};
    CoCreateGuid((GUID*)&ItemGuid);
    CoCreateGuid((GUID*)&TrackerGuid);

    if (auto Weapon = (ABotWeapon*)Pawn->EquipWeaponDefinition(Definition, ItemGuid, TrackerGuid, false))
    {
        if (IsRangedWeapon(Definition) && Bot.Items[Best].LoadedAmmo > 0)
            Weapon->AmmoCount = Bot.Items[Best].LoadedAmmo;
        Bot.EquippedItem = Best;
    }
}

static void GiveBotItem(FBot& Bot, ABotPawn* Pawn, const UFortItemDefinition* Definition, int32 Count, int32 LoadedAmmo)
{
    if (!Definition || ScoreItem(Definition) < 0)
        return; // bots only care about weapons, ammo is simulated

    for (auto& Item : Bot.Items)
        if (Item.Definition == Definition)
            return;

    if (IsRangedWeapon(Definition) && LoadedAmmo <= 0)
        if (auto Stats = AFortInventory::GetStats((UFortWeaponItemDefinition*)Definition))
            LoadedAmmo = Stats->ClipSize;

    Bot.Items.push_back({ Definition, Count, LoadedAmmo });
    EquipBestItem(Bot, Pawn);
}

static bool HasRangedWeapon(FBot& Bot)
{
    for (auto& Item : Bot.Items)
        if (IsRangedWeapon(Item.Definition))
            return true;
    return false;
}

static void MoveTowards(FBot& Bot, ABotPawn* Pawn, const FVector& Goal, double Time, bool bFaceMovement = true)
{
    auto Location = Pawn->K2_GetActorLocation();
    auto Direction = Goal - Location;
    Direction.Z = 0;
    double Distance = Direction.Magnitude();
    if (Distance < 60.0)
        return;

    Direction = Direction / Distance;
    Pawn->AddMovementInput(Direction, 1.f, false);

    if (bFaceMovement)
        if (auto Controller = GetWeak<UBotController>(Bot.Controller))
            Controller->SetControlRotation(MakeRotator(0, atan2(Direction.Y, Direction.X) * 57.29577951308232));

    // no navmesh here, so hop over things and pick a new route when stuck
    if (Time >= Bot.NextStuckCheckTime)
    {
        if (Bot.NextStuckCheckTime > 0 && (Location - Bot.LastStuckCheckLocation).Magnitude() < 60.0)
        {
            Bot.StuckCount++;
            Pawn->Jump();

            if (Bot.StuckCount >= 3)
            {
                Bot.StuckCount = 0;
                Bot.bHasMoveGoal = false;
                Bot.LootTarget = FWeakObjectPtr();
                Bot.StrafeUntil = Time + 1.5;
                Bot.StrafeDirection = RandRange(0.f, 1.f) < 0.5f ? -1.f : 1.f;
            }
        }
        else
            Bot.StuckCount = 0;

        Bot.LastStuckCheckLocation = Location;
        Bot.NextStuckCheckTime = Time + 1.0;
    }

    if (Time < Bot.StrafeUntil)
        Pawn->AddMovementInput(FVector(-Direction.Y, Direction.X, 0) * Bot.StrafeDirection, 1.f, false);
}

static AActor* FindTarget(FBot& Bot, ABotPawn* Pawn, double Range)
{
    auto Location = Pawn->K2_GetActorLocation();
    auto Team = GetTeam(Pawn);
    auto Controller = GetWeak<UBotController>(Bot.Controller);
    AActor* Best = nullptr;
    double BestDistance = Range;

    auto Consider = [&](AActor* Other)
    {
        auto OtherPawn = (ABotPawn*)Other;
        if (!OtherPawn || OtherPawn == Pawn || IsPawnDead(OtherPawn) || OtherPawn->bIsSkydiving || GetTeam(OtherPawn) == Team)
            return;

        double Distance = (OtherPawn->K2_GetActorLocation() - Location).Magnitude();
        if (Distance < BestDistance && (!Controller || Controller->LineOfSightTo(OtherPawn, FVector(), false)))
        {
            BestDistance = Distance;
            Best = OtherPawn;
        }
    };

    for (auto& Player : GetGameMode()->AlivePlayers)
        if (Player)
            Consider(((AFortPlayerControllerAthena*)Player)->MyFortPawn);

    for (auto& Other : AllBots)
        if (Other.State == EBotState::Grounded)
            Consider(GetWeak(Other.Pawn));

    return Best;
}

static void TickCombat(FBot& Bot, ABotPawn* Pawn, AActor* Target, double Time)
{
    auto Controller = GetWeak<UBotController>(Bot.Controller);
    auto Location = Pawn->K2_GetActorLocation();
    auto TargetLocation = Target->K2_GetActorLocation();
    double Distance = (TargetLocation - Location).Magnitude();
    bool bRanged = Bot.EquippedItem != -1 && IsRangedWeapon(Bot.Items[Bot.EquippedItem].Definition);

    // aim with a bit of error that grows with distance, worse bots miss more
    float Error = (1.f - FConfiguration::BotAccuracy) * 6.f;
    auto Aim = LookAt(Location + FVector(0, 0, 60), TargetLocation + FVector(0, 0, 40));
    Aim.Pitch = Aim.Pitch + RandRange(-Error, Error) * 0.5f;
    Aim.Yaw = Aim.Yaw + RandRange(-Error, Error);
    if (Controller)
        Controller->SetControlRotation(Aim);

    double IdealDistance = bRanged ? 1500.0 : 120.0;
    if (Distance > IdealDistance + 300.0)
        MoveTowards(Bot, Pawn, TargetLocation, Time, false);
    else
    {
        if (Time >= Bot.StrafeUntil)
        {
            Bot.StrafeUntil = Time + RandRange(0.6f, 1.6f);
            Bot.StrafeDirection = RandRange(0.f, 1.f) < 0.5f ? -1.f : 1.f;
            if (RandInt(0, 6) == 0)
                Pawn->Jump();
        }

        auto Forward = TargetLocation - Location;
        Forward.Z = 0;
        Forward.Normalize();
        Pawn->AddMovementInput(FVector(-Forward.Y, Forward.X, 0) * Bot.StrafeDirection, 1.f, false);
        if (Distance < IdealDistance - 300.0)
            Pawn->AddMovementInput(Forward * -1.0, 1.f, false);
    }

    // fire in bursts
    auto Weapon = (ABotWeapon*)Pawn->CurrentWeapon;
    if (Weapon && bRanged)
    {
        if (Weapon->AmmoCount <= 0)
        {
            if (Bot.bFiring)
            {
                Pawn->PawnStopFire(0);
                Bot.bFiring = false;
            }

            if (Bot.ReloadEndTime == 0)
                Bot.ReloadEndTime = Time + 1.8;
            else if (Time >= Bot.ReloadEndTime)
            {
                Bot.ReloadEndTime = 0;
                auto ClipSize = Weapon->GetBulletsPerClip();
                Weapon->AmmoCount = ClipSize > 0 ? ClipSize : 30;
                Weapon->ForceNetUpdate();
            }
            return;
        }
    }

    if (Time >= Bot.NextFireToggleTime)
    {
        Bot.bFiring = !Bot.bFiring;
        if (Bot.bFiring)
        {
            Pawn->PawnStartFire(0);
            Bot.NextFireToggleTime = Time + RandRange(0.25f, 1.2f);
        }
        else
        {
            Pawn->PawnStopFire(0);
            Bot.NextFireToggleTime = Time + RandRange(0.2f, 0.7f);
        }
    }

    if (auto TargetBot = std::find_if(AllBots.begin(), AllBots.end(), [&](FBot& Other) { return Other.Pawn.Get() == Target; }); TargetBot != AllBots.end())
        TargetBot->LastAttacker = Bot.Controller;
}

static void TickLoot(FBot& Bot, ABotPawn* Pawn, double Time)
{
    auto Location = Pawn->K2_GetActorLocation();
    auto LootTarget = GetWeak(Bot.LootTarget);

    if (LootTarget)
    {
        if (LootTarget->IsA<AFortPickupAthena>() && ((AFortPickupAthena*)LootTarget)->bPickedUp)
            LootTarget = nullptr;
        else if (LootTarget->IsA<ABuildingContainer>() && ((ABuildingContainer*)LootTarget)->bAlreadySearched)
            LootTarget = nullptr;
    }

    if (!LootTarget)
    {
        // closest pickup or unopened chest
        double BestDistance = HasRangedWeapon(Bot) ? 3000.0 : 15000.0;
        for (auto& Pickup : CachedPickups)
        {
            if (!Pickup || Pickup->bActorIsBeingDestroyed || ((AFortPickupAthena*)Pickup)->bPickedUp)
                continue;

            auto Definition = ((AFortPickupAthena*)Pickup)->PrimaryPickupItemEntry.ItemDefinition;
            if (ScoreItem(Definition) <= (Bot.EquippedItem != -1 ? ScoreItem(Bot.Items[Bot.EquippedItem].Definition) : -1))
                continue;

            double Distance = (Pickup->K2_GetActorLocation() - Location).Magnitude();
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                LootTarget = Pickup;
            }
        }

        for (auto& Container : CachedContainers)
        {
            if (!Container || Container->bActorIsBeingDestroyed || ((ABuildingContainer*)Container)->bAlreadySearched)
                continue;

            double Distance = (Container->K2_GetActorLocation() - Location).Magnitude();
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                LootTarget = Container;
            }
        }

        Bot.LootTarget = FWeakObjectPtr(LootTarget);
    }

    if (!LootTarget)
    {
        // nothing around, roam somewhere inside the zone
        if (!Bot.bHasMoveGoal || Dist2D(Location, Bot.MoveGoal) < 300.0)
        {
            auto SafeZoneIndicator = GetSafeZoneIndicator();
            auto Center = SafeZoneIndicator ? SafeZoneIndicator->NextCenter : Location;
            double Radius = SafeZoneIndicator ? std::max(SafeZoneIndicator->NextRadius * 0.7, 2000.0) : 8000.0;
            double Angle = RandRange(0.f, 6.2831853f);
            double R = RandRange(0.f, 1.f) * Radius;
            Bot.MoveGoal = FVector(Center.X + cos(Angle) * R, Center.Y + sin(Angle) * R, Location.Z);
            Bot.bHasMoveGoal = true;
        }

        MoveTowards(Bot, Pawn, Bot.MoveGoal, Time);
        return;
    }

    auto LootLocation = LootTarget->K2_GetActorLocation();
    if ((LootLocation - Location).Magnitude() > 220.0)
    {
        MoveTowards(Bot, Pawn, LootLocation, Time);
        return;
    }

    if (auto Container = LootTarget->Cast<ABuildingContainer>())
    {
        UFortLootPackage::SpawnLootHook(Container);
        NextLootRefreshTime = 0;
    }
    else if (auto Pickup = LootTarget->Cast<AFortPickupAthena>())
    {
        auto& Entry = Pickup->PrimaryPickupItemEntry;
        GiveBotItem(Bot, Pawn, Entry.ItemDefinition, Entry.Count, Entry.LoadedAmmo);

        Pickup->bPickedUp = true;
        Pickup->OnRep_bPickedUp();
        Pickup->K2_DestroyActor();
    }

    Bot.LootTarget = FWeakObjectPtr();
}

static void TickPlutoniumBrain(FBot& Bot, ABotPawn* Pawn, double Time)
{
    if (Time >= Bot.NextThinkTime)
    {
        Bot.NextThinkTime = Time + 0.4;
        Bot.Target = FWeakObjectPtr(FindTarget(Bot, Pawn, HasRangedWeapon(Bot) ? 7000.0 : 2500.0));
    }

    auto Target = GetWeak(Bot.Target);
    if (Target && IsPawnDead((ABotPawn*)Target))
        Target = nullptr;

    if (Target)
        return TickCombat(Bot, Pawn, Target, Time);

    if (Bot.bFiring)
    {
        Pawn->PawnStopFire(0);
        Bot.bFiring = false;
    }

    // get back in the zone before anything else
    auto SafeZoneIndicator = GetSafeZoneIndicator();
    if (SafeZoneIndicator && SafeZoneIndicator->NextRadius > 0)
    {
        auto Location = Pawn->K2_GetActorLocation();
        if (Dist2D(Location, SafeZoneIndicator->NextCenter) > SafeZoneIndicator->NextRadius * 0.85)
            return MoveTowards(Bot, Pawn, SafeZoneIndicator->NextCenter, Time);
    }

    TickLoot(Bot, Pawn, Time);
}

static void TickWarmup(FBot& Bot, ABotPawn* Pawn, double Time)
{
    if (Bot.bNative)
        return;

    // wander around the spawn island
    auto Location = Pawn->K2_GetActorLocation();
    if (!Bot.bHasMoveGoal || Dist2D(Location, Bot.MoveGoal) < 200.0)
    {
        Bot.MoveGoal = Location + FVector(RandRange(-2500.f, 2500.f), RandRange(-2500.f, 2500.f), 0);
        Bot.bHasMoveGoal = true;
    }
    MoveTowards(Bot, Pawn, Bot.MoveGoal, Time);
}

// ----- eliminations -----

static AActor* FindKiller(FBot& Bot, ABotPawn* Pawn)
{
    if (Pawn && Pawn->HasDamagers())
    {
        auto& Damagers = Pawn->Damagers;
        for (int i = Damagers.Num() - 1; i >= 0; i--)
        {
            auto Causer = Damagers[i].DamageCauser;
            if (Causer && Causer != GetWeak(Bot.Controller) && GetControllerPlayerState(Causer))
                return Causer;
        }
    }

    return GetWeak(Bot.LastAttacker);
}

static void HandleElimination(FBot& Bot, ABotPawn* Pawn)
{
    auto GameMode = GetGameMode();
    auto GameState = GameMode->GameState;
    auto PlayerState = GetWeak<AFortPlayerStateAthena>(Bot.PlayerState);
    auto KillerController = FindKiller(Bot, Pawn);
    auto KillerPlayerState = KillerController ? GetControllerPlayerState(KillerController) : nullptr;
    auto DeathLocation = Pawn ? Pawn->K2_GetActorLocation() : FVector();

    // plutonium AI bots drop what they picked up
    if (!Bot.bNative)
    {
        for (auto& Item : Bot.Items)
            if (Item.Definition && Item.Definition->CanBeDropped())
                AFortInventory::SpawnPickup(DeathLocation, Item.Definition, Item.Count, Item.LoadedAmmo, EFortPickupSourceTypeFlag::GetPlayer(), EFortPickupSpawnSource::GetPlayerElimination(), Pawn);
        Bot.Items.clear();
    }

    if (!PlayerState)
        return;

    if (PlayerState->HasPlace())
    {
        PlayerState->Place = GameState->PlayersLeft;
        PlayerState->OnRep_Place();
    }

    if (PlayerState->HasDeathInfo())
    {
        memset(&PlayerState->DeathInfo, 0, FDeathInfo::Size());
        if (FDeathInfo::HasKiller())
            PlayerState->DeathInfo.Killer = KillerPlayerState;
        if (FDeathInfo::HasDeathLocation())
            PlayerState->DeathInfo.DeathLocation = DeathLocation;
        if (FDeathInfo::HasFinisherOrDowner())
            PlayerState->DeathInfo.FinisherOrDowner = KillerPlayerState ? KillerPlayerState : PlayerState;
        if (FDeathInfo::HasDistance() && KillerController)
        {
            auto KillerPawn = ((UBotController*)KillerController)->Pawn;
            PlayerState->DeathInfo.Distance = KillerPawn ? (float)(KillerPawn->K2_GetActorLocation() - DeathLocation).Magnitude() : 0.f;
        }
        if (FDeathInfo::HasbInitialized())
            PlayerState->DeathInfo.bInitialized = true;
        PlayerState->OnRep_DeathInfo();
    }

    if (KillerPlayerState && KillerPlayerState != PlayerState)
    {
        if (KillerPlayerState->HasKillScore())
            KillerPlayerState->KillScore++;
        else
            KillerPlayerState->Kills++;
        KillerPlayerState->OnRep_Kills();
        if (KillerPlayerState->HasTeamKillScore())
        {
            KillerPlayerState->TeamKillScore++;
            KillerPlayerState->OnRep_TeamKillScore();
        }

        struct
        {
            AFortPlayerStateAthena* PlayerState;
            uint8_t Padding[0x8];
        } KillReport{ PlayerState };
        KillerPlayerState->ClientReportKill(KillReport);
        if (KillerPlayerState->HasTeamKillScore())
            KillerPlayerState->ClientReportTeamKill(KillerPlayerState->TeamKillScore);

        auto KillerPawn = (AFortPlayerPawnAthena*)((UBotController*)KillerController)->Pawn;
        if (FConfiguration::SiphonAmount > 0 && KillerPawn && KillerController->IsA<AFortPlayerControllerAthena>())
        {
            auto Health = KillerPawn->GetHealth() + FConfiguration::SiphonAmount;
            auto Shield = KillerPawn->GetShield();
            if (Health > 100.f)
            {
                Shield = std::min(Shield + (Health - 100.f), 100.f);
                Health = 100.f;
            }
            KillerPawn->SetHealth(Health);
            KillerPawn->SetShield(Shield);
        }
    }
}

static void DeclareWinner(AFortGameMode* GameMode, uint8 Team)
{
    auto GameState = GameMode->GameState;
    bDeclaredWinner = true;

    for (auto& Player__Uncasted : GameMode->AlivePlayers)
    {
        auto Player = (AFortPlayerControllerAthena*)Player__Uncasted;
        auto PlayerState = Player ? Player->PlayerState : nullptr;
        if (!PlayerState || PlayerState->TeamIndex != Team)
            continue;

        auto Pawn = Player->MyFortPawn;

        if (PlayerState->HasPlace())
        {
            PlayerState->Place = 1;
            PlayerState->OnRep_Place();
        }

        if (VersionInfo.FortniteVersion >= 16)
            Player->PlayWinEffects(Pawn, (UFortWeaponItemDefinition*)nullptr, (uint8)0, false);
        Player->ClientNotifyWon(Pawn, (UFortWeaponItemDefinition*)nullptr, (uint8)0);
        Player->ClientNotifyTeamWon(Pawn, (UFortWeaponItemDefinition*)nullptr, (uint8)0);

        if (VersionInfo.FortniteVersion >= 19 && Player->WorldInventory)
        {
            if (auto Crown = FindObject<UFortItemDefinition>(L"/VictoryCrownsGameplay/Items/AGID_VictoryCrown.AGID_VictoryCrown"))
            {
                TArray<FFortItemEntryStateValue> StateValues{};
                auto Value = (FFortItemEntryStateValue*)malloc(FFortItemEntryStateValue::Size());
                memset((PBYTE)Value, 0, FFortItemEntryStateValue::Size());
                Value->IntValue = 1;
                Value->StateType = 2;
                StateValues.Add(*Value, FFortItemEntryStateValue::Size());
                free(Value);
                Player->WorldInventory->GiveItem(Crown, 1, 0, 0, true, true, 0, StateValues);
                StateValues.Free();
            }
        }

        if (GameState->HasWinningPlayerState())
        {
            GameState->WinningPlayerState = PlayerState;
            GameState->OnRep_WinningPlayerState();
        }
    }

    GameState->WinningTeam = Team;
    GameState->OnRep_WinningTeam();
    printf("[Bots] Team %d won the match.\n", Team);
}

// plutonium only declares winners on player kills, so cover the "last bot died" case here
static void CheckForWinner(AFortGameMode* GameMode, uint8 GamePhase)
{
    if (bDeclaredWinner || SpawnedBots == 0 || (EAthenaGamePhase)GamePhase < EAthenaGamePhase::SafeZones)
        return;

    auto GameState = GameMode->GameState;
    if (GameState->HasWinningPlayerState() && GameState->WinningPlayerState)
    {
        bDeclaredWinner = true; // plutonium already handled it (player killed the last player)
        return;
    }

    std::vector<uint8> AliveTeams;
    for (auto& Bot : AllBots)
    {
        if (Bot.State == EBotState::Dead)
            continue;
        auto Team = GetTeam(GetWeak(Bot.Pawn));
        if (std::find(AliveTeams.begin(), AliveTeams.end(), Team) == AliveTeams.end())
            AliveTeams.push_back(Team);
    }

    bool bAnyBotTeam = !AliveTeams.empty();
    for (auto& Player : GameMode->AlivePlayers)
    {
        auto PlayerController = (AFortPlayerControllerAthena*)Player;
        if (!PlayerController || !PlayerController->PlayerState)
            continue;
        auto Team = PlayerController->PlayerState->TeamIndex;
        if (std::find(AliveTeams.begin(), AliveTeams.end(), Team) == AliveTeams.end())
            AliveTeams.push_back(Team);
    }

    if (AliveTeams.size() == 1 && !bAnyBotTeam)
        DeclareWinner(GameMode, AliveTeams[0]);
}

void Bots::Tick()
{
    if (!FConfiguration::bBotLobby)
        return;

    auto World = UWorld::GetWorld();
    auto GameMode = World ? (AFortGameMode*)World->AuthorityGameMode : nullptr;
    if (!GameMode || !GameMode->GameState || !GameMode->HasWarmupRequiredPlayerCount())
        return;

    auto Time = Now();
    auto GamePhase = GetGamePhase();

    if (GamePhase != LastGamePhase)
    {
        if ((EAthenaGamePhase)GamePhase == EAthenaGamePhase::Aircraft)
            PutBotsOnBus();

        if (bCreatedService)
            if (auto Service = GetPlayerBotService())
                Service->OnGamePhaseChanged(GamePhase);

        LastGamePhase = GamePhase;
    }

    SpawnPendingBots(GameMode, GamePhase);

    if ((EAthenaGamePhase)GamePhase == EAthenaGamePhase::Aircraft || (EAthenaGamePhase)GamePhase == EAthenaGamePhase::SafeZones)
        TickBus(Time);

    bool bNeedLoot = false;
    for (auto& Bot : AllBots)
        if (!Bot.bNative && Bot.State == EBotState::Grounded)
            bNeedLoot = true;
    if (bNeedLoot)
        RefreshLootCache(Time);

    for (size_t i = 0; i < AllBots.size(); i++)
    {
        auto& Bot = AllBots[i];
        if (Bot.State == EBotState::Dead)
            continue;

        auto Pawn = GetWeak<ABotPawn>(Bot.Pawn);
        bool bDead = !Pawn || ((Bot.State != EBotState::OnBus) && IsPawnDead(Pawn));

        if (bDead)
        {
            if (Pawn && !Pawn->bActorIsBeingDestroyed && Pawn->GetHealth() <= 0.f && !Pawn->IsDead())
                Pawn->ForceKill(FGameplayTag(), (AActor*)nullptr, (AActor*)nullptr);

            Bot.State = EBotState::Dead;
            Bot.DeathTime = Time;

            if ((EAthenaGamePhase)GamePhase <= EAthenaGamePhase::Warmup)
                Bot.bHandledElimination = true; // warmup deaths don't count, we just refill
            else if (!Bot.bNative)
            {
                HandleElimination(Bot, Pawn);
                Bot.bHandledElimination = true;
            }
            continue;
        }

        switch (Bot.State)
        {
        case EBotState::Warmup:
            if ((EAthenaGamePhase)GamePhase >= EAthenaGamePhase::SafeZones)
                Bot.State = EBotState::Grounded; // playlist without a bus
            else
                TickWarmup(Bot, Pawn, Time);
            break;
        case EBotState::OnBus:
            break;
        case EBotState::Skydiving:
            TickSkydive(Bot, Pawn, Time);
            break;
        case EBotState::Grounded:
            if (!Bot.bNative)
                TickPlutoniumBrain(Bot, Pawn, Time);
            break;
        default:
            break;
        }

        if ((EAthenaGamePhase)GamePhase == EAthenaGamePhase::SafeZones && Bot.State == EBotState::Grounded)
            TickStorm(Bot, Pawn, Time);
    }

    // native bots handle their own elimination, but make sure the killer still gets credited if the game didn't do it
    for (auto& Bot : AllBots)
    {
        if (Bot.State != EBotState::Dead || Bot.bHandledElimination || Time - Bot.DeathTime < 1.0)
            continue;

        Bot.bHandledElimination = true;
        auto PlayerState = GetWeak<AFortPlayerStateAthena>(Bot.PlayerState);
        if (PlayerState && PlayerState->HasDeathInfo() && FDeathInfo::HasbInitialized() && !PlayerState->DeathInfo.bInitialized)
            HandleElimination(Bot, GetWeak<ABotPawn>(Bot.Pawn));
    }

    // forget bots that died a while ago, and clean up plutonium AI controllers
    for (size_t i = 0; i < AllBots.size();)
    {
        auto& Bot = AllBots[i];
        if (Bot.State == EBotState::Dead && Bot.bHandledElimination && Time - Bot.DeathTime > 10.0)
        {
            if (!Bot.bNative)
                if (auto Controller = GetWeak(Bot.Controller))
                    Controller->K2_DestroyActor();

            AllBots.erase(AllBots.begin() + i);
            continue;
        }
        i++;
    }

    SyncPlayerCounts(GameMode, GamePhase);
    CheckForWinner(GameMode, GamePhase);
}
