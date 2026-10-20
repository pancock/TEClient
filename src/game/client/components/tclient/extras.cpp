#include "extras.h"

#include <base/str.h>

#include <engine/client.h>
#include <engine/friends.h>
#include <engine/graphics.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/protocol.h>

#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/sounds.h>
#include <game/client/gameclient.h>
#include <game/localization.h>

#include <algorithm>

// ---------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------

void CExtras::OnConsoleInit()
{
	Console()->Register("tc_ah_swap", "", CFGFLAG_CLIENT, ConSwapStrong, this, "Swap which tee (main/dummy) must have strong hook");
	Console()->Register("tc_ah_toggle", "", CFGFLAG_CLIENT, ConToggleAutoStrong, this, "Toggle auto strong hook");
	Console()->Register("tc_ak_toggle", "", CFGFLAG_CLIENT, ConToggleAutoKill, this, "Toggle auto kill on freeze");
}

void CExtras::ConSwapStrong(IConsole::IResult *pResult, void *pUserData)
{
	CExtras *pSelf = (CExtras *)pUserData;
	g_Config.m_TcAhStrong ^= 1;
	pSelf->m_FixTries = 0;
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), TCLocalize("Strong hook: %s"), g_Config.m_TcAhStrong ? TCLocalize("dummy") : TCLocalize("main tee"));
	pSelf->GameClient()->m_Chat.Echo(aBuf);
}

void CExtras::ConToggleAutoStrong(IConsole::IResult *pResult, void *pUserData)
{
	CExtras *pSelf = (CExtras *)pUserData;
	g_Config.m_TcAhEnable ^= 1;
	pSelf->m_FixTries = 0;
	pSelf->GameClient()->m_Chat.Echo(g_Config.m_TcAhEnable ? TCLocalize("Auto strong hook: on") : TCLocalize("Auto strong hook: off"));
}

void CExtras::ConToggleAutoKill(IConsole::IResult *pResult, void *pUserData)
{
	CExtras *pSelf = (CExtras *)pUserData;
	g_Config.m_TcAkEnable ^= 1;
	pSelf->GameClient()->m_Chat.Echo(g_Config.m_TcAkEnable ? TCLocalize("Auto kill: on") : TCLocalize("Auto kill: off"));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void CExtras::OnStateChange(int NewState, int OldState)
{
	if(NewState == IClient::STATE_ONLINE && OldState != IClient::STATE_ONLINE)
		ResetFriendTracking();
	if(NewState != IClient::STATE_ONLINE)
	{
		m_vToasts.clear();
		for(int Conn = 0; Conn < NUM_DUMMIES; Conn++)
		{
			m_aFrozen[Conn] = false;
			m_aWasAlive[Conn] = false;
		}
		m_FixTries = 0;
		m_StrongState = STRONG_INACTIVE;
	}
}

void CExtras::OnMapLoad()
{
	ResetFriendTracking();
}

void CExtras::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	UpdateTeeLife();
	UpdateStrongHook();
	UpdateAutoKill();

	if(g_Config.m_TcFtEnable)
	{
		if(g_Config.m_TcFtCurrent)
			UpdateFriendsInServer();
		if(g_Config.m_TcFtGlobal)
			UpdateGlobalFriends();
	}

	RenderToasts();
}

void CExtras::SendKillTo(int Conn)
{
	CMsgPacker Msg(NETMSGTYPE_CL_KILL, false);
	Client()->SendMsg(Conn, &Msg, MSGFLAG_VITAL);
}

bool CExtras::IsFrozen(int Conn) const
{
	const int Id = GameClient()->m_aLocalIds[Conn];
	if(Id < 0)
		return false;
	const auto &Data = GameClient()->m_aClients[Id];
	const bool Predicted = g_Config.m_ClPredict && (Conn == g_Config.m_ClDummy || GameClient()->PredictDummy());
	if(Predicted)
		return (g_Config.m_TcFastInput ? Data.m_RegularPredicted.m_FreezeEnd : Data.m_Predicted.m_FreezeEnd) != 0;
	return Data.m_FreezeEnd != 0;
}

// ---------------------------------------------------------------------------
// Auto strong hook
//
// Strong/weak is decided by spawn order: the tee that spawned last is the weakest.
// So to make tee A strong we make sure tee B (the one that must be weak) respawns
// after A, which is done by killing B.
// ---------------------------------------------------------------------------

void CExtras::UpdateTeeLife()
{
	const float Now = Client()->LocalTime();
	for(int Conn = 0; Conn < NUM_DUMMIES; Conn++)
	{
		const int Id = GameClient()->m_aLocalIds[Conn];
		const bool Alive = Id >= 0 && GameClient()->m_Snap.m_aCharacters[Id].m_Active;
		if(Alive && !m_aWasAlive[Conn])
			m_aSpawnTime[Conn] = Now;
		m_aWasAlive[Conn] = Alive;
	}
}

void CExtras::UpdateStrongHook()
{
	m_StrongState = STRONG_INACTIVE;
	if(!g_Config.m_TcAhEnable || !Client()->DummyConnected())
	{
		m_FixTries = 0;
		return;
	}

	const int aIds[NUM_DUMMIES] = {GameClient()->m_aLocalIds[0], GameClient()->m_aLocalIds[1]};
	if(aIds[0] < 0 || aIds[1] < 0)
		return;

	const auto &Main = GameClient()->m_Snap.m_aCharacters[aIds[0]];
	const auto &Dummy = GameClient()->m_Snap.m_aCharacters[aIds[1]];
	if(!Main.m_Active || !Dummy.m_Active || !Main.m_HasExtendedData || !Dummy.m_HasExtendedData)
	{
		m_StrongState = STRONG_UNKNOWN;
		return;
	}

	const int StrongConn = g_Config.m_TcAhStrong ? 1 : 0;
	const int WeakConn = 1 - StrongConn;
	const int StrongValue = (StrongConn == 0 ? Main : Dummy).m_ExtendedData.m_StrongWeakId;
	const int WeakValue = (WeakConn == 0 ? Main : Dummy).m_ExtendedData.m_StrongWeakId;
	const float Now = Client()->LocalTime();

	// The higher id is the stronger one
	const bool Ok = StrongValue > WeakValue;
	if(m_PrevOrderOk && !Ok)
		m_OrderChangeTime = Now; // order just flipped, treat as a respawn event
	m_PrevOrderOk = Ok;

	if(Ok)
	{
		m_StrongState = STRONG_OK;
		m_FixTries = 0;
		return;
	}
	m_StrongState = STRONG_WRONG;

	// Guards: only act shortly after a respawn, never spam
	if(g_Config.m_TcAhWindow > 0)
	{
		const float LastEvent = std::max({m_aSpawnTime[0], m_aSpawnTime[1], m_OrderChangeTime});
		if(Now - LastEvent > (float)g_Config.m_TcAhWindow)
			return;
	}
	if(g_Config.m_TcAhSkipTeam && (GameClient()->m_Teams.Team(aIds[0]) != 0 || GameClient()->m_Teams.Team(aIds[1]) != 0))
		return;
	if(m_FixTries >= g_Config.m_TcAhMaxTries)
		return;
	if(Now - m_LastFixTime < (float)g_Config.m_TcAhCooldown / 1000.0f)
		return;

	SendKillTo(WeakConn);
	m_LastFixTime = Now;
	m_FixTries++;
}

// ---------------------------------------------------------------------------
// Auto kill on freeze
// ---------------------------------------------------------------------------

void CExtras::UpdateAutoKill()
{
	const float Now = Client()->LocalTime();
	for(int Conn = 0; Conn < NUM_DUMMIES; Conn++)
	{
		const int Id = GameClient()->m_aLocalIds[Conn];
		const bool Connected = Conn == 0 || Client()->DummyConnected();
		bool Applies = false;
		switch(g_Config.m_TcAkTarget)
		{
		case 0: Applies = Conn == g_Config.m_ClDummy; break;
		case 1: Applies = Conn == 0; break;
		case 2: Applies = Conn == 1; break;
		default: Applies = true; break;
		}

		const bool Alive = Connected && Id >= 0 && GameClient()->m_Snap.m_aCharacters[Id].m_Active;
		const bool Frozen = Alive && IsFrozen(Conn);

		if(Frozen && !m_aFrozen[Conn])
			m_aFrozenSince[Conn] = Now;
		m_aFrozen[Conn] = Frozen;

		if(!g_Config.m_TcAkEnable || !Applies || !Frozen)
			continue;

		if(Now - m_aFrozenSince[Conn] < (float)g_Config.m_TcAkDelay / 1000.0f)
			continue;
		if(Now - m_aLastAutoKill[Conn] < 1.0f) // resend only if the kill did not go through
			continue;

		SendKillTo(Conn);
		m_aLastAutoKill[Conn] = Now;
	}
}

// ---------------------------------------------------------------------------
// Friend tracker
// ---------------------------------------------------------------------------

void CExtras::ResetFriendTracking()
{
	std::fill(std::begin(m_aFriendPresent), std::end(m_aFriendPresent), false);
	for(auto &aName : m_aaFriendName)
		aName[0] = '\0';
	m_vExistingFriends.clear();
	m_BaselineDone = false;
	m_BaselineUntil = Client()->LocalTime() + 3.0f;
}

bool CExtras::IsNameOnCurrentServer(const char *pName) const
{
	for(int i = 0; i < MAX_CLIENTS; i++)
		if(GameClient()->m_aClients[i].m_Active && str_comp(GameClient()->m_aClients[i].m_aName, pName) == 0)
			return true;
	return false;
}

void CExtras::UpdateFriendsInServer()
{
	const float Now = Client()->LocalTime();
	const bool Baseline = !m_BaselineDone && Now < m_BaselineUntil;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		const auto &Data = GameClient()->m_aClients[i];
		const bool Local = i == GameClient()->m_aLocalIds[0] || i == GameClient()->m_aLocalIds[1];
		const bool Friend = Data.m_Active && !Local && GameClient()->Friends()->IsFriend(Data.m_aName, Data.m_aClan, true);

		const bool Known = m_aFriendPresent[i] && str_comp(m_aaFriendName[i], Data.m_aName) == 0;
		if(Friend && !Known)
		{
			if(Baseline)
			{
				m_vExistingFriends.emplace_back(Data.m_aName);
			}
			else
			{
				char aText[128];
				str_format(aText, sizeof(aText), "%s  -  %s", Data.m_aName, GameClient()->Map()->BaseName());
				AddFriendToast(TCLocalize("Friend joined"), aText);
			}
		}
		m_aFriendPresent[i] = Friend;
		if(Friend)
			str_copy(m_aaFriendName[i], Data.m_aName);
		else
			m_aaFriendName[i][0] = '\0';
	}

	if(!m_BaselineDone && !Baseline)
	{
		m_BaselineDone = true;
		if(g_Config.m_TcFtExisting && !m_vExistingFriends.empty())
		{
			std::string Names;
			for(const auto &Name : m_vExistingFriends)
			{
				if(!Names.empty())
					Names += ", ";
				Names += Name;
			}
			AddFriendToast(TCLocalize("Friends on this server"), Names.c_str());
		}
		m_vExistingFriends.clear();
	}
}

void CExtras::UpdateGlobalFriends()
{
	IServerBrowser *pBrowser = GameClient()->ServerBrowser();
	const float Now = Client()->LocalTime();

	if(m_ScanState == SCAN_IDLE)
	{
		// Do not touch the server list while the menu (and its server browser) is open
		if(GameClient()->m_Menus.IsActive() || pBrowser->IsRefreshing() || pBrowser->IsGettingServerlist())
			return;
		if(Now - m_LastScan < (float)g_Config.m_TcFtInterval)
			return;
		pBrowser->Refresh(IServerBrowser::TYPE_INTERNET, true);
		m_ScanState = SCAN_WAITING;
		m_ScanStart = Now;
		return;
	}

	// Wait for the list from the master servers; player names are part of it
	const float Waited = Now - m_ScanStart;
	if((Waited > 4.0f && !pBrowser->IsGettingServerlist()) || Waited > 30.0f)
	{
		EvaluateServerList();
		m_ScanState = SCAN_IDLE;
		m_LastScan = Now;
	}
}

void CExtras::EvaluateServerList()
{
	IServerBrowser *pBrowser = GameClient()->ServerBrowser();
	const float Now = Client()->LocalTime();

	std::map<std::string, SFriendLocation> Current;
	for(int i = 0; i < pBrowser->NumServers(); i++)
	{
		const CServerInfo *pInfo = pBrowser->Get(i);
		if(pInfo->m_FriendState == IFriends::FRIEND_NO)
			continue;
		for(const CServerInfo::CClient &Client : pInfo->m_vClients)
		{
			if(Client.m_FriendState != IFriends::FRIEND_PLAYER)
				continue;
			Current[Client.m_aName] = SFriendLocation{pInfo->m_aName, pInfo->m_aMap};
		}
	}

	// Never announce from an empty list (list not loaded yet)
	if(pBrowser->NumServers() == 0)
		return;

	if(m_GlobalBaselineDone)
	{
		for(const auto &[Name, Location] : Current)
		{
			if(m_GlobalOnline.count(Name))
				continue;
			if(IsNameOnCurrentServer(Name.c_str())) // already handled by the in-server tracker
				continue;
			auto It = m_GlobalNotifiedAt.find(Name);
			if(It != m_GlobalNotifiedAt.end() && Now - It->second < 300.0f) // list flicker protection
				continue;
			m_GlobalNotifiedAt[Name] = Now;

			char aText[256];
			str_format(aText, sizeof(aText), "%s  -  %s (%s)", Name.c_str(), Location.m_Server.c_str(), Location.m_Map.c_str());
			AddFriendToast(TCLocalize("Friend online"), aText);
		}
	}
	m_GlobalOnline = std::move(Current);
	m_GlobalBaselineDone = true;
}

// ---------------------------------------------------------------------------
// Toasts
// ---------------------------------------------------------------------------

void CExtras::AddFriendToast(const char *pTitle, const char *pText)
{
	if(m_vToasts.size() >= 4)
		m_vToasts.erase(m_vToasts.begin());
	m_vToasts.push_back(SToast{pTitle, pText, Client()->LocalTime()});

	if(g_Config.m_TcFtSound)
		GameClient()->m_Sounds.Play(CSounds::CHN_GUI, SOUND_CHAT_HIGHLIGHT, 1.0f);
	if(g_Config.m_TcFtChat)
	{
		char aBuf[320];
		str_format(aBuf, sizeof(aBuf), "[%s] %s", pTitle, pText);
		GameClient()->m_Chat.Echo(aBuf);
	}
}

void CExtras::RenderToasts()
{
	if(m_vToasts.empty())
		return;

	const float Now = Client()->LocalTime();
	const float Duration = (float)g_Config.m_TcFtDuration;
	m_vToasts.erase(std::remove_if(m_vToasts.begin(), m_vToasts.end(), [&](const SToast &Toast) { return Now - Toast.m_Start > Duration; }), m_vToasts.end());

	const float Height = 300.0f;
	const float Width = Height * Graphics()->ScreenAspect();
	Graphics()->MapScreen({0.0f, 0.0f, Width, Height});

	const float TitleSize = 6.0f;
	const float TextSize = 8.0f;
	const float Pad = 5.0f;
	const float BoxHeight = Pad * 2.0f + TitleSize + 2.0f + TextSize;
	float Y = 40.0f;

	for(const SToast &Toast : m_vToasts)
	{
		const float Age = Now - Toast.m_Start;
		const float SlideIn = std::min(1.0f, Age / 0.25f);
		const float FadeOut = std::min(1.0f, (Duration - Age) / 0.4f);
		const float Alpha = std::clamp(std::min(SlideIn, FadeOut), 0.0f, 1.0f);
		const float Ease = 1.0f - (1.0f - SlideIn) * (1.0f - SlideIn);

		const float TextWidth = std::max(TextRender()->TextWidth(TextSize, Toast.m_Text.c_str()), TextRender()->TextWidth(TitleSize, Toast.m_Title.c_str()));
		const float BoxWidth = TextWidth + Pad * 2.0f + 4.0f;
		const float X = Width - (BoxWidth + 8.0f) * Ease;

		Graphics()->TextureClear();
		Graphics()->DrawRect(X, Y, BoxWidth, BoxHeight, ColorRGBA(0.05f, 0.06f, 0.08f, 0.82f * Alpha), IGraphics::CORNER_L, 4.0f);
		Graphics()->DrawRect(X, Y, 2.5f, BoxHeight, ColorRGBA(0.30f, 0.85f, 0.45f, Alpha), IGraphics::CORNER_L, 4.0f);

		TextRender()->TextColor(0.30f, 0.85f, 0.45f, Alpha);
		TextRender()->Text(X + Pad + 3.0f, Y + Pad, TitleSize, Toast.m_Title.c_str(), -1.0f);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, Alpha);
		TextRender()->Text(X + Pad + 3.0f, Y + Pad + TitleSize + 2.0f, TextSize, Toast.m_Text.c_str(), -1.0f);

		Y += BoxHeight + 4.0f;
	}
	TextRender()->TextColor(TextRender()->DefaultTextColor());
}
