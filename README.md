# 28.30 AI Bot Lobby (Erbium)

A Fortnite **28.30** (Chapter 5 Season 2) gameserver that fills Battle Royale matches with **100 AI players**. It's built on [Erbium](https://github.com/plooshi/Erbium) by plooshi.

## Bot lobby

Once the first real player loads into warmup, the server spawns 100 bots, a few per tick. Bots run the full match:

- **Warmup**: bots spawn on the warmup island alongside players.
- **Battle bus**: every bot boards the bus. Each one picks a landing spot (usually a named POI near the bus path), jumps when the bus passes it, skydives and steers toward it. Any bot still on the bus at the end of the drop window gets kicked off, the same as real players.
- **On the ground**: by default bots run **Fortnite's own player-bot AI**. They're spawned through `AthenaAIServicePlayerBots::SpawnAI` with the `BP_PhoebePlayerController` behavior trees, so they loot, fight, build and rotate the same way Epic's bots do.
- **Storm**: bots take storm damage and get eliminated by it.
- **Eliminations**: kills on bots go to the kill feed, the killer's stats and siphon. `Players left` counts bots. When the last bot dies and one team of real players is left, that team wins.

If Fortnite's bot service can't be used on a build, bots automatically fall back to **Erbium AI**. That's a built-in brain that walks, opens chests, picks up floor loot and weapons, fights with strafing and burst fire, reloads, and follows the storm.

### Settings (`Erbium/Erbium/Public/Configuration.h`)

| Option | Default | What it does |
| --- | --- | --- |
| `bBotLobby` | `true` | Turns bots on or off |
| `BotCount` | `100` | Number of bots to add |
| `bBotsFillLobby` | `false` | When `true`, `BotCount` is the total lobby size, so bots = `BotCount` minus real players |
| `bBotsWaitForHuman` | `true` | Waits until a real player joins before spawning bots |
| `BotsPerTick` | `4` | How many bots spawn per server tick |
| `BotMode` | `0` | `0` = Fortnite AI with Erbium AI fallback, `1` = Fortnite AI only, `2` = Erbium AI only |
| `BotSpawnerData` | `L""` | Optional path to a `FortAthenaPlayerBotSpawnerData` blueprint class. Leave it empty to auto-detect |
| `bBotStormDamageNative` | `true` | Applies storm damage to Fortnite-AI bots |
| `bBotsSyncPlayersLeft` | `true` | Counts bots in the players-left counter |
| `BotMaxGlideDistance` | `60000` | Maximum distance (in UE units) from the bus path that a bot will choose to land |
| `BotAccuracy` | `0.6` | Aim accuracy for Erbium AI, from 0 to 1 |

Cheat: `cheat spawnbot <count>` spawns bots next to you.

### Notes

- The server logs `[Bots] ...` lines showing which mode was used. Look for `Using spawner data ...` or `falling back to Erbium AI`.
- If Fortnite AI bots spawn but don't walk around, the server most likely has no navmesh. Set `BotMode = 2` to use Erbium AI, which steers directly and doesn't need a navmesh.

---

# Erbium

Erbium is a WIP universal gameserver for Fortnite.

[**Join our Discord!**](https://discord.gg/WxNEGBxfKq)

## Features
- **Version support**: Erbium supports version 3.4 (season 3) to 19.40 (chapter 3 season 1).
> Erbium also has partial support for 1.7.2 (season 0) to 3.3 (season 3) & 20.00 (chapter 3 season 2) to 30.00 (chapter 5 season 3)
- **Easy to use**: Erbium is designed to be fast & easy to configure for new users.

## To-do
- **Creative**
- **XP**
- **Quests**

## Configuration
- In order to change options, go to Erbium/Public/Configuration.h & configure to your liking!

> If you have issues with a specific version or find a bug, please make an issue.

> Contributions are highly appreciated!

> If you use this, please give me credit.
> Credit to Milxnor for parts of Finders.cpp
