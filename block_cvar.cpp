#include <stdio.h>
#include <sstream>
#include <string>
#include <map>
#include <deque>
#include <unordered_map>
#include "block_cvar.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include "filesystem.h"
#include "utlbuffer.h"

block_cvar g_block_cvar;
PLUGIN_EXPOSE(block_cvar, g_block_cvar);
IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;

IUtilsApi* g_pUtils;
IPlayersApi* g_pPlayers;
IClientCvarValue* g_pClientCvarValue;

static const char* kBlockedCvarsPath = "addons/configs/blocked_cvars.txt";

std::map<std::string, std::string> g_BlockedCvars;

struct PlayerCvarCheckState
{
	std::deque<std::string> pending;
	bool active = false;
	CTimer* timer = nullptr;
};

static std::unordered_map<int, PlayerCvarCheckState> g_PlayerChecks;
static CTimer* g_pCheckWatch[64];
static constexpr float kCvarQueryIntervalSec = 0.2f;

static void ClearAllPlayerChecks()
{
	if (g_pUtils)
	{
		for (auto& entry : g_PlayerChecks)
		{
			if (entry.second.timer)
			{
				g_pUtils->RemoveTimer(entry.second.timer);
				entry.second.timer = nullptr;
			}
		}
	}

	g_PlayerChecks.clear();
}

static void OnRoundEvent(const char*  , IGameEvent*  , bool  )
{
	ClearAllPlayerChecks();
}

static std::string Trim(const std::string& text)
{
	const auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return "";
	const auto last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

static void LoadBlockedCvars()
{
	g_BlockedCvars.clear();
	if (!g_pFullFileSystem)
	{
		return;
	}
	CUtlBuffer buf;
	if (!g_pFullFileSystem->ReadFile(kBlockedCvarsPath, "GAME", buf))
	{
		return;
	}
	std::string content;
	content.assign(static_cast<const char*>(buf.Base()), buf.TellPut());
	std::istringstream iss(content);
	std::string line;
	while (std::getline(iss, line))
	{
		line = Trim(line);

		if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
			line = line.substr(3);
		if (line.empty())
			continue;

		if (line.rfind("#", 0) == 0 || line.rfind("//", 0) == 0)
			continue;
		
		std::istringstream lineStream(line);
		std::string cvarName, expectedValue;
		lineStream >> cvarName >> expectedValue;
		
		if (!cvarName.empty() && !expectedValue.empty())
		{
			g_BlockedCvars[cvarName] = expectedValue;
		}
	}
}

void OnCvarValueReceived(CPlayerSlot nSlot, ECvarValueStatus eStatus, const char* pszCvarName, const char* pszCvarValue)
{
	int iSlot = nSlot.Get();
	
	if (!g_pPlayers || !g_pPlayers->IsConnected(iSlot) || !g_pPlayers->IsInGame(iSlot))
	{
		return;
	}
	
	if (eStatus == ECvarValueStatus::ValueIntact && pszCvarName && pszCvarValue)
	{
		auto it = g_BlockedCvars.find(pszCvarName);
		if (it != g_BlockedCvars.end())
		{
			const std::string& expectedValue = it->second;
			bool isMatch = (strcmp(pszCvarValue, expectedValue.c_str()) == 0);
			
			if (expectedValue == "false" || expectedValue == "0")
			{
				isMatch = (strcmp(pszCvarValue, "0") == 0 || strcmp(pszCvarValue, "false") == 0);
			}
			else if (expectedValue == "true" || expectedValue == "1")
			{
				isMatch = (strcmp(pszCvarValue, "1") == 0 || strcmp(pszCvarValue, "true") == 0);
			}
			
			if (!isMatch)
			{
				if (g_pUtils && engine)
				{
					g_pUtils->NextFrame([iSlot]() {
						if (g_pPlayers && g_pPlayers->IsConnected(iSlot))
						{
							engine->DisconnectClient(CPlayerSlot(iSlot), NETWORK_DISCONNECT_KICKED);
						}
					});
				}
			}
		}
	}
}

static void StartPlayerCvarCheck(int iSlot)
{
	if (!g_pClientCvarValue || !g_pPlayers || !g_pPlayers->IsInGame(iSlot) || g_pPlayers->IsFakeClient(iSlot))
	{
		return;
	}

	auto& state = g_PlayerChecks[iSlot];
	if (state.active)
	{
		return;
	}

	state.pending.clear();
	for (const auto& cvar : g_BlockedCvars)
	{
		state.pending.push_back(cvar.first);
	}

	if (state.pending.empty() || !g_pUtils)
	{
		g_PlayerChecks.erase(iSlot);
		return;
	}

	state.active = true;
	state.timer = g_pUtils->CreateTimer(kCvarQueryIntervalSec, [iSlot]() -> float {
		if (!g_pClientCvarValue || !g_pPlayers || !g_pPlayers->IsConnected(iSlot) || !g_pPlayers->IsInGame(iSlot))
		{
			auto it = g_PlayerChecks.find(iSlot);
			if (it != g_PlayerChecks.end())
			{
				g_PlayerChecks.erase(iSlot);
			}
			return -1.0f;
		}

		auto it = g_PlayerChecks.find(iSlot);
		if (it == g_PlayerChecks.end())
		{
			return -1.0f;
		}

		auto& stateRef = it->second;
		if (stateRef.pending.empty())
		{
			g_PlayerChecks.erase(iSlot);
			return -1.0f;
		}

		const std::string cvarName = std::move(stateRef.pending.front());
		stateRef.pending.pop_front();

		g_pClientCvarValue->QueryCvarValue(CPlayerSlot(iSlot), cvarName.c_str(), OnCvarValueReceived);
		return kCvarQueryIntervalSec;
	});
}

void OnClientAuthorized(int iSlot, uint64 iSteamID64)
{
	if (g_pUtils)
	{
		if (iSlot < 0 || iSlot >= 64 || g_pCheckWatch[iSlot]) return;
		g_pCheckWatch[iSlot] = g_pUtils->CreateTimer(3.0f, [iSlot]() -> float {
			if (!g_pPlayers || !g_pPlayers->IsConnected(iSlot))
			{
				g_pCheckWatch[iSlot] = nullptr;
				return -1.0f;
			}
			StartPlayerCvarCheck(iSlot);
			return 5.0f;
		});
	}
}

void StartupServer()
{
	g_pGameEntitySystem = GameEntitySystem();
	g_pEntitySystem = g_pUtils->GetCEntitySystem();
	gpGlobals = g_pUtils->GetCGlobalVars();
}

bool block_cvar::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceService, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

	g_SMAPI->AddListener( this, this );

	LoadBlockedCvars();

	return true;
}

bool block_cvar::Unload(char *error, size_t maxlen)
{
	int ret;
	if (!g_SMAPI->MetaFactory(UTILS_INTERFACE, &ret, nullptr)) g_pUtils = nullptr;
	ClearAllPlayerChecks();
	if (g_pUtils)
	{
		for (auto& t : g_pCheckWatch)
		{
			if (t) g_pUtils->RemoveTimer(t);
			t = nullptr;
		}
	}
	ConVar_Unregister();
	
	return true;
}

void block_cvar::AllPluginsLoaded()
{
	char error[64];
	int ret;
	
	g_pUtils = (IUtilsApi *)g_SMAPI->MetaFactory(UTILS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	
	g_pPlayers = (IPlayersApi *)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	
	g_pClientCvarValue = (IClientCvarValue *)g_SMAPI->MetaFactory(CLIENTCVARVALUE_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	
	g_pPlayers->HookOnClientAuthorized(g_PLID, OnClientAuthorized);
	g_pUtils->StartupServer(g_PLID, StartupServer);
	g_pUtils->HookEvent(g_PLID, "round_start", OnRoundEvent);
	g_pUtils->HookEvent(g_PLID, "round_end", OnRoundEvent);
	g_pUtils->MapStartHook(g_PLID, [](const char*  ) {
		ClearAllPlayerChecks();
	});
	g_pUtils->MapEndHook(g_PLID, []() {
		ClearAllPlayerChecks();
	});
}

const char* block_cvar::GetLicense()
{
	return "GPL";
}

const char* block_cvar::GetVersion()
{
	return "1.0.1";
}

const char* block_cvar::GetDate()
{
	return __DATE__;
}

const char *block_cvar::GetLogTag()
{
	return "[block_cvar]";
}

const char* block_cvar::GetAuthor()
{
	return "ABKAM";
}

const char* block_cvar::GetDescription()
{
	return "Block Cvar";
}

const char* block_cvar::GetName()
{
	return "Block Cvar";
}

const char* block_cvar::GetURL()
{
	return "https://discord.gg/ChYfTtrtmS";
}