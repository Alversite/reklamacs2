/**
 * Reklama - CS2 Metamod:Source advertisement plugin
 *
 * Broadcasts chat + center-screen messages on independent timers, configured
 * via the ORIGINAL settings.ini (Valve KeyValues format), read verbatim.
 *
 * Messaging is done through the engine's network-message system
 * (CUserMessageTextMsg via IGameEventSystem::PostEventAbstract) so it does NOT
 * depend on any hard-coded function signatures/offsets - that is what makes it
 * survive CS2 game updates.
 */

#include "reklama.h"

#include "eiface.h"
#include "engine/igameeventsystem.h"
#include "globalvars.h"
#include "icvar.h"
#include "interface.h"
#include "irecipientfilter.h"
#include "networksystem/inetworkmessages.h"
#include "networksystem/netmessage.h"
#include "playerslot.h"
#include "tier0/dbg.h"
#include "tier1/convar.h"

#include "usermessages.pb.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

// ---------------------------------------------------------------------------
// Globals / interfaces
// ---------------------------------------------------------------------------
Reklama g_Reklama;

IVEngineServer2* g_pEngineServer2 = nullptr;
ISource2Server* g_pSource2Server = nullptr;
IServerGameClients* g_pSource2GameClients = nullptr;
IGameEventSystem* g_gameEventSystem = nullptr;
// g_pCVar (icvar.h) and g_pNetworkMessages (inetworkmessages.h) are SDK-declared.

PLUGIN_EXPOSE(Reklama, g_Reklama);

// SourceHook declarations. GameFrame lives on IServerGameDLL (ISource2Server
// derives from it); the client callbacks live on IServerGameClients.
SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);
SH_DECL_HOOK4_void(IServerGameClients, ClientActive, SH_NOATTRIB, 0, CPlayerSlot, bool, const char*, uint64);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, CPlayerSlot, int, const char*, uint64, const char*);

static CGlobalVars* GetGlobals()
{
	return g_pEngineServer2 ? g_pEngineServer2->GetServerGlobals() : nullptr;
}

// ---------------------------------------------------------------------------
// Recipient filter that targets every currently-connected slot
// ---------------------------------------------------------------------------
class CBroadcastFilter : public IRecipientFilter
{
public:
	CBroadcastFilter()
	{
		for (int i = 0; i < 64; i++)
			if (g_Reklama.m_bConnected[i])
				m_Recipients.Set(i);
	}

	~CBroadcastFilter() override {}

	NetChannelBufType_t GetNetworkBufType() const override { return BUF_RELIABLE; }
	bool IsInitMessage() const override { return false; }
	const CPlayerBitVec& GetRecipients() const override { return m_Recipients; }
	CPlayerSlot GetPredictedPlayerSlot() const override { return -1; }

	bool HasRecipients() const
	{
		for (int i = 0; i < 64; i++)
			if (g_Reklama.m_bConnected[i])
				return true;
		return false;
	}

private:
	CPlayerBitVec m_Recipients;
};

// ---------------------------------------------------------------------------
// Chat colors: translate {TAG} placeholders into CS2 chat control bytes.
// The tag names match the ones used in settings.ini (SourceMod-style names).
// ---------------------------------------------------------------------------
struct ColorTag
{
	const char* name;
	char code;
};

static const ColorTag s_ColorTags[] = {
	{"DEFAULT", '\x01'},
	{"WHITE", '\x01'},
	{"DARKRED", '\x02'},
	{"RED", '\x07'},
	{"LIGHTRED", '\x0F'},
	{"PURPLE", '\x03'},
	{"LIGHTPURPLE", '\x0E'},
	{"GREEN", '\x04'},
	{"LIGHTGREEN", '\x05'},
	{"LIME", '\x06'},
	{"OLIVE", '\x05'},
	{"LIGHTOLIVE", '\x09'},
	{"YELLOW", '\x09'},
	{"GOLD", '\x10'},
	{"SILVER", '\x0A'},
	{"GRAY", '\x08'},
	{"GREY", '\x08'},
	{"BLUE", '\x0B'},
	{"LIGHTBLUE", '\x0B'},
	{"DARKBLUE", '\x0C'},
	{"BLUEGREY", '\x0D'},
	{"GRAYBLUE", '\x0D'},
	{"MAGENTA", '\x0E'},
	{"PINK", '\x0E'},
};

static std::string ApplyChatColors(const std::string& in)
{
	std::string out;
	out.reserve(in.size());

	for (size_t i = 0; i < in.size();)
	{
		if (in[i] == '{')
		{
			size_t end = in.find('}', i);
			if (end != std::string::npos)
			{
				std::string tag = in.substr(i + 1, end - i - 1);
				for (char& c : tag)
					c = (char)toupper((unsigned char)c);

				bool matched = false;
				for (const ColorTag& ct : s_ColorTags)
				{
					if (tag == ct.name)
					{
						out.push_back(ct.code);
						matched = true;
						break;
					}
				}

				if (matched)
				{
					i = end + 1;
					continue;
				}
			}
		}

		out.push_back(in[i]);
		i++;
	}

	return out;
}

// ---------------------------------------------------------------------------
// Minimal Valve KeyValues (KV1) parser - reads the exact settings.ini format.
// ---------------------------------------------------------------------------
struct KVNode
{
	std::string key;
	std::string value;
	bool isSection = false;
	std::vector<KVNode> children;

	const KVNode* Find(const char* name) const
	{
		for (const KVNode& c : children)
			if (c.key == name)
				return &c;
		return nullptr;
	}
};

class KVParser
{
public:
	explicit KVParser(const std::string& text) : m_text(text) {}

	bool Parse(KVNode& root)
	{
		// The file is: "Config" { ... } - read the top key then its section.
		std::string key;
		if (!NextToken(key))
			return false;

		root.key = key;
		root.isSection = true;

		std::string brace;
		if (!NextToken(brace) || brace != "{")
			return false;

		return ParseSection(root);
	}

private:
	const std::string& m_text;
	size_t m_pos = 0;

	bool ParseSection(KVNode& section)
	{
		for (;;)
		{
			std::string token;
			if (!NextToken(token))
				return false; // unexpected EOF

			if (token == "}")
				return true;

			KVNode child;
			child.key = token;

			std::string next;
			if (!NextToken(next))
				return false;

			if (next == "{")
			{
				child.isSection = true;
				if (!ParseSection(child))
					return false;
			}
			else
			{
				child.value = next;
			}

			section.children.push_back(std::move(child));
		}
	}

	// Reads one token: a brace, or a (quoted or bare) string. Skips whitespace
	// and // comments. Returns false at end of input.
	bool NextToken(std::string& out)
	{
		SkipTrivia();
		if (m_pos >= m_text.size())
			return false;

		char c = m_text[m_pos];

		if (c == '{' || c == '}')
		{
			out = std::string(1, c);
			m_pos++;
			return true;
		}

		if (c == '"')
		{
			m_pos++; // opening quote
			out.clear();
			while (m_pos < m_text.size() && m_text[m_pos] != '"')
			{
				out.push_back(m_text[m_pos]);
				m_pos++;
			}
			if (m_pos < m_text.size())
				m_pos++; // closing quote
			return true;
		}

		// bareword
		out.clear();
		while (m_pos < m_text.size())
		{
			char b = m_text[m_pos];
			if (b == ' ' || b == '\t' || b == '\r' || b == '\n' || b == '{' || b == '}' || b == '"')
				break;
			out.push_back(b);
			m_pos++;
		}
		return !out.empty();
	}

	void SkipTrivia()
	{
		for (;;)
		{
			while (m_pos < m_text.size() &&
				   (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\r' || m_text[m_pos] == '\n'))
				m_pos++;

			// line comments
			if (m_pos + 1 < m_text.size() && m_text[m_pos] == '/' && m_text[m_pos + 1] == '/')
			{
				while (m_pos < m_text.size() && m_text[m_pos] != '\n')
					m_pos++;
				continue;
			}
			break;
		}
	}
};

// ---------------------------------------------------------------------------
// Config loading
// ---------------------------------------------------------------------------
void Reklama::LoadConfig()
{
	m_vecChatGroups.clear();
	m_vecCenterMessages.clear();
	m_iChatIndex = 0;
	m_iCenterIndex = 0;
	m_flChatAccum = 0.0f;
	m_flCenterAccum = 0.0f;

	std::string path = std::string(g_SMAPI->GetBaseDir()) + "/addons/configs/Reklama/settings.ini";

	std::ifstream file(path, std::ios::binary);
	if (!file.good())
	{
		Warning("[Reklama] Failed to load config %s\n", path.c_str());
		return;
	}

	std::stringstream ss;
	ss << file.rdbuf();
	std::string text = ss.str();

	KVNode root;
	KVParser parser(text);
	if (!parser.Parse(root))
	{
		Warning("[Reklama] Failed to parse config %s\n", path.c_str());
		return;
	}

	if (const KVNode* n = root.Find("ChatInterval"))
		m_iChatInterval = atoi(n->value.c_str());
	if (const KVNode* n = root.Find("CenterInterval"))
		m_iCenterInterval = atoi(n->value.c_str());

	if (m_iChatInterval <= 0)
		m_iChatInterval = 25;
	if (m_iCenterInterval <= 0)
		m_iCenterInterval = 40;

	if (const KVNode* chat = root.Find("ChatMessages"))
	{
		for (const KVNode& group : chat->children)
		{
			if (!group.isSection)
				continue;

			std::vector<std::string> lines;
			for (const KVNode& line : group.children)
				lines.push_back(ApplyChatColors(line.value));

			if (!lines.empty())
				m_vecChatGroups.push_back(std::move(lines));
		}
	}

	if (const KVNode* center = root.Find("CenterMessages"))
	{
		for (const KVNode& msg : center->children)
		{
			if (!msg.isSection)
				m_vecCenterMessages.push_back(msg.value);
		}
	}

	Msg("[Reklama] Config loaded: %d chat groups, %d center messages (chat=%ds, center=%ds)\n",
		(int)m_vecChatGroups.size(), (int)m_vecCenterMessages.size(), m_iChatInterval, m_iCenterInterval);
}

// ---------------------------------------------------------------------------
// Message sending
// ---------------------------------------------------------------------------
static void SendTextMsg(int hudDest, const char* text)
{
	if (!g_pNetworkMessages || !g_gameEventSystem)
		return;

	CBroadcastFilter filter;
	if (!filter.HasRecipients())
		return;

	INetworkMessageInternal* pNetMsg = g_pNetworkMessages->FindNetworkMessagePartial("TextMsg");
	if (!pNetMsg)
		return;

	CUserMessageTextMsg* pData = pNetMsg->AllocateMessage()->ToPB<CUserMessageTextMsg>();
	pData->set_dest(hudDest);
	pData->add_param(text);

	g_gameEventSystem->PostEventAbstract(-1, false, &filter, pNetMsg, pData, 0);

	delete pData;
}

void Reklama::SendNextChatMessage()
{
	if (m_vecChatGroups.empty())
		return;

	if (m_iChatIndex >= m_vecChatGroups.size())
		m_iChatIndex = 0;

	const std::vector<std::string>& group = m_vecChatGroups[m_iChatIndex];
	for (const std::string& line : group)
		SendTextMsg(3 /*HUD_PRINTTALK*/, line.c_str());

	m_iChatIndex = (m_iChatIndex + 1) % m_vecChatGroups.size();
}

void Reklama::SendNextCenterMessage()
{
	if (m_vecCenterMessages.empty())
		return;

	if (m_iCenterIndex >= m_vecCenterMessages.size())
		m_iCenterIndex = 0;

	SendTextMsg(4 /*HUD_PRINTCENTER*/, m_vecCenterMessages[m_iCenterIndex].c_str());

	m_iCenterIndex = (m_iCenterIndex + 1) % m_vecCenterMessages.size();
}

// ---------------------------------------------------------------------------
// Console command: mm_reklama_reload
// ---------------------------------------------------------------------------
static void Reklama_Reload_Callback(const CCommandContext& context, const CCommand& args)
{
	g_Reklama.LoadConfig();
	Msg("[Reklama] Config reloaded.\n");
}

static ConCommand mm_reklama_reload_command("mm_reklama_reload", Reklama_Reload_Callback,
											"Reload the Reklama advertisement config.", FCVAR_LINKED_CONCOMMAND | FCVAR_GAMEDLL);

// ---------------------------------------------------------------------------
// SourceHook callbacks
// ---------------------------------------------------------------------------
void Reklama::Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick)
{
	CGlobalVars* pGlobals = GetGlobals();
	if (!pGlobals)
		return;

	float curtime = pGlobals->curtime;
	float dt = curtime - m_flLastCurtime;
	m_flLastCurtime = curtime;

	// Guard against map changes / hitches where curtime jumps or resets.
	if (dt < 0.0f || dt > 1.0f)
		dt = 0.0f;

	m_flChatAccum += dt;
	m_flCenterAccum += dt;

	if (m_flChatAccum >= (float)m_iChatInterval)
	{
		m_flChatAccum = 0.0f;
		SendNextChatMessage();
	}

	if (m_flCenterAccum >= (float)m_iCenterInterval)
	{
		m_flCenterAccum = 0.0f;
		SendNextCenterMessage();
	}
}

void Reklama::Hook_ClientActive(CPlayerSlot slot, bool bLoadGame, const char* pszName, uint64 xuid)
{
	int i = slot.Get();
	if (i >= 0 && i < 64)
		m_bConnected[i] = true;
}

void Reklama::Hook_ClientDisconnect(CPlayerSlot slot, int reason, const char* pszName, uint64 xuid, const char* pszNetworkID)
{
	int i = slot.Get();
	if (i >= 0 && i < 64)
		m_bConnected[i] = false;
}

// ---------------------------------------------------------------------------
// Metamod entry points
// ---------------------------------------------------------------------------
bool Reklama::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer2, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_gameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, SOURCE2GAMECLIENTS_INTERFACE_VERSION);

	SH_ADD_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &Reklama::Hook_GameFrame), true);
	SH_ADD_HOOK(IServerGameClients, ClientActive, g_pSource2GameClients, SH_MEMBER(this, &Reklama::Hook_ClientActive), true);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &Reklama::Hook_ClientDisconnect), true);

	// Flush statically-constructed ConCommands (mm_reklama_reload) into the engine.
	ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL);

	LoadConfig();

	Msg("[Reklama] Plugin loaded.\n");
	return true;
}

bool Reklama::Unload(char* error, size_t maxlen)
{
	SH_REMOVE_HOOK(IServerGameDLL, GameFrame, g_pSource2Server, SH_MEMBER(this, &Reklama::Hook_GameFrame), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientActive, g_pSource2GameClients, SH_MEMBER(this, &Reklama::Hook_ClientActive), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, g_pSource2GameClients, SH_MEMBER(this, &Reklama::Hook_ClientDisconnect), true);

	ConVar_Unregister();

	return true;
}
