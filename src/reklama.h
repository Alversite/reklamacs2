/**
 * Reklama - CS2 Metamod:Source advertisement plugin
 * Rewritten from scratch to build against the latest hl2sdk-cs2 + Metamod:Source.
 *
 * Reads the ORIGINAL settings.ini (Valve KeyValues) untouched and broadcasts
 * chat + center-screen advertisement messages on independent timers.
 */

#pragma once

#include <ISmmPlugin.h>
#include <iserver.h>

#include "eiface.h" // ISource2Server/GameClients, CPlayerSlot, ENetworkDisconnectionReason

#include <string>
#include <vector>

class Reklama : public ISmmPlugin
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
	bool Unload(char* error, size_t maxlen);
	bool Pause(char* error, size_t maxlen) { return true; }
	bool Unpause(char* error, size_t maxlen) { return true; }
	void AllPluginsLoaded() {}

public: // SourceHook callbacks
	void Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick);

public: // logic
	void LoadConfig();
	void SendNextChatMessage();
	void SendNextCenterMessage();

public: // ISmmPlugin metadata
	const char* GetAuthor() { return "Killhaus (restored)"; }
	const char* GetName() { return "Reklama"; }
	const char* GetDescription() { return "Chat & center advertisement broadcaster"; }
	const char* GetURL() { return "https://killhaus.su"; }
	const char* GetLicense() { return "MIT"; }
	const char* GetVersion() { return "1.0.0"; }
	const char* GetDate() { return __DATE__; }
	const char* GetLogTag() { return "REKLAMA"; }

public: // state
	int m_iChatInterval = 25;
	int m_iCenterInterval = 40;

	// Each chat "group" is an ordered list of individual chat lines.
	std::vector<std::vector<std::string>> m_vecChatGroups;
	std::vector<std::string> m_vecCenterMessages;

	size_t m_iChatIndex = 0;
	size_t m_iCenterIndex = 0;

	float m_flChatAccum = 0.0f;
	float m_flCenterAccum = 0.0f;
	float m_flLastCurtime = 0.0f;
};

extern Reklama g_Reklama;

PLUGIN_GLOBALVARS();
