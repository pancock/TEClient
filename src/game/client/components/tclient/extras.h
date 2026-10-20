#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_EXTRAS_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_EXTRAS_H

#include <engine/client/enums.h>
#include <engine/console.h>
#include <engine/shared/protocol.h>

#include <game/client/component.h>

#include <map>
#include <string>
#include <vector>

// Personal extras: auto strong hook for dummy play, auto kill on freeze, friend tracker.
class CExtras : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnConsoleInit() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnMapLoad() override;
	void OnRender() override;

	enum EStrongState
	{
		STRONG_INACTIVE = 0, // feature off / no dummy
		STRONG_UNKNOWN, // tees not both visible or server has no extended data
		STRONG_OK,
		STRONG_WRONG, // wrong tee is strong (fixing, waiting or blocked by a guard)
	};
	EStrongState StrongState() const { return m_StrongState; }

	// Live status for the settings page, so it is visible what the client "sees"
	struct SAutoKillInfo
	{
		bool m_Present = false; // tee exists
		bool m_Applies = false; // selected by "applies to"
		bool m_Frozen = false;
		float m_FrozenFor = 0.0f; // seconds
		int m_KillsSent = 0;
	};
	SAutoKillInfo AutoKillInfo(int Conn) const;

	struct SFriendInfo
	{
		int m_ListSize = 0; // friends in the friends list
		int m_InServer = 0; // friends currently seen on this server
		bool m_Scanning = false;
		float m_SinceScan = -1.0f; // seconds since last finished scan, <0 if none yet
		int m_ScanServers = 0;
		int m_ScanFriends = 0;
	};
	SFriendInfo FriendInfo() const;
	void TestToast();

private:
	// ---- auto strong hook ----
	EStrongState m_StrongState = STRONG_INACTIVE;
	bool m_aWasAlive[NUM_DUMMIES] = {false, false};
	float m_aSpawnTime[NUM_DUMMIES] = {-1000.0f, -1000.0f};
	float m_OrderChangeTime = -1000.0f;
	bool m_PrevOrderOk = true;
	float m_LastFixTime = -1000.0f;
	int m_FixTries = 0;
	void UpdateTeeLife();
	void UpdateStrongHook();
	void SendKillTo(int Conn);

	// ---- auto kill ----
	bool m_aFrozen[NUM_DUMMIES] = {false, false};
	float m_aFrozenSince[NUM_DUMMIES] = {0.0f, 0.0f};
	float m_aLastAutoKill[NUM_DUMMIES] = {-1000.0f, -1000.0f};
	void UpdateAutoKill();
	bool IsFrozen(int Conn) const;
	bool m_aPresent[NUM_DUMMIES] = {false, false};
	bool m_aApplies[NUM_DUMMIES] = {false, false};
	int m_aKillsSent[NUM_DUMMIES] = {0, 0};

	// ---- friend tracker ----
	bool m_aFriendPresent[MAX_CLIENTS] = {};
	char m_aaFriendName[MAX_CLIENTS][MAX_NAME_LENGTH] = {};
	float m_BaselineUntil = 0.0f;
	bool m_BaselineDone = true;
	std::vector<std::string> m_vExistingFriends;
	void ResetFriendTracking();
	void UpdateFriendsInServer();

	enum EScanState
	{
		SCAN_IDLE = 0,
		SCAN_WAITING,
	};
	struct SFriendLocation
	{
		std::string m_Server;
		std::string m_Map;
	};
	EScanState m_ScanState = SCAN_IDLE;
	float m_ScanStart = 0.0f;
	float m_LastScan = -1000.0f;
	bool m_GlobalBaselineDone = false;
	int m_LastScanServers = 0;
	int m_LastScanFriends = 0;
	std::map<std::string, SFriendLocation> m_GlobalOnline;
	std::map<std::string, float> m_GlobalNotifiedAt;
	void UpdateGlobalFriends();
	void EvaluateServerList();
	bool IsNameOnCurrentServer(const char *pName) const;

	// ---- toasts ----
	struct SToast
	{
		std::string m_Title;
		std::string m_Text;
		float m_Start;
	};
	std::vector<SToast> m_vToasts;
	void AddFriendToast(const char *pTitle, const char *pText);
	void RenderToasts();

	static void ConSwapStrong(IConsole::IResult *pResult, void *pUserData);
	static void ConToggleAutoKill(IConsole::IResult *pResult, void *pUserData);
	static void ConToggleAutoStrong(IConsole::IResult *pResult, void *pUserData);
};

#endif
