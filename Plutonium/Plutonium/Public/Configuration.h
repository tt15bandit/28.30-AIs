#pragma once

struct FConfiguration
{
    static inline auto Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
    static inline auto MaxTickRate = 30;
    static inline auto bLateGame = false;
    static inline auto LateGameZone = 3;          // starting zone
    static inline auto bLateGameLongZone = false; // zone doesnt close for a long time
    static inline auto bEnableCheats = true;
    static inline auto SiphonAmount = 50; // set to 0 to disable
    static inline auto bInfiniteMats = false;
    static inline auto bInfiniteAmmo = false;
    static inline auto bForceRespawns = false; // build your client with this too!
    static inline auto bJoinInProgress = false;
    static inline auto bAutoRestart = false;
    static inline auto bKeepInventory = false;
    static inline auto Port = 7777;
    static inline auto bEnableIris = true;

    // bot lobby (see Plutonium/Private/Bots.cpp)
    static inline auto bBotLobby = true;
    static inline auto BotCount = 100;               // number of AI players added to the match
    static inline auto bBotsFillLobby = false;       // true = BotCount is the total lobby size (bots = BotCount - real players)
    static inline auto bBotsWaitForHuman = true;     // only start spawning bots once a real player joins warmup
    static inline auto BotsPerTick = 4;              // bots spawned per server tick, keeps warmup from hitching
    static inline auto BotMode = 0;                  // 0 = Fortnite's own bot AI with Plutonium AI fallback, 1 = Fortnite AI only, 2 = Plutonium AI only
    static inline auto BotSpawnerData = L"";         // optional path to a FortAthenaPlayerBotSpawnerData blueprint class to spawn bots with
    static inline auto bCreateBotServiceIfMissing = true;
    static inline auto bBotStormDamageNative = true; // apply storm damage to Fortnite AI bots too (Plutonium AI bots always take it)
    static inline auto bBotsSyncPlayersLeft = true;  // count bots in the players left counter
    static inline auto BotMaxGlideDistance = 60000.f; // how far from the bus path bots will pick a landing spot
    static inline auto BotAccuracy = 0.6f;           // Plutonium AI aim (0-1)

    // items that never spawn (matched against the item definition name, e.g. WID_Shotgun_Pump_Paprika_Athena_UR_Boss)
    static inline constexpr const char* RemovedItems[] = {
        "Paprika",    // Peter Griffin's Hammer Pump Shotgun
        "Boss_Midas", // Jules' Drum Gun
        "Jules",      // any other Jules weapon
    };
    static inline constexpr auto bGUI = true;
    static inline constexpr auto bCustomCrashReporter = true;
    static inline constexpr auto bUseStdoutLog = false;
    static inline constexpr auto WebhookURL = ""; // fill in if you want status to send to a webhook
};
