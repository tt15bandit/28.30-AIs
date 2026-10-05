#pragma once
#include "../../pch.h"

// Bot lobby: fills Battle Royale matches with AI players.
//
// Bots are spawned through Fortnite's own player bot service (UAthenaAIServicePlayerBots::SpawnAI),
// which gives them the stock BP_PhoebePlayerController behavior trees (looting, fighting, building,
// rotating with the storm, etc). Plutonium runs its own battle bus, so the bus flight + jump + skydive is
// driven from here, after which control is handed back to the native AI.
//
// If the native service can't be used (missing service or spawner data), bots fall back to a
// built-in brain (Plutonium AI) that walks, loots chests/floor loot, fights and follows the storm.
class Bots
{
public:
    static void Tick();

    // Spawns a single bot at the location. Returns the bot's pawn (or nullptr).
    static AActor* SpawnBot(FVector Location);

    static bool IsBotController(const UObject* Controller);
    static int32 GetAliveBotCount();
    static int32 GetSpawnedBotCount();
};
