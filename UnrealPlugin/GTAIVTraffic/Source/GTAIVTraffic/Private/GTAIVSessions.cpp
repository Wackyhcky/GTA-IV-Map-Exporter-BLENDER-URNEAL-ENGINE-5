#include "GTAIVSessions.h"

#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Interfaces/OnlineExternalUIInterface.h"
#include "Kismet/GameplayStatics.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"
#include "Online/OnlineSessionNames.h"

namespace
{
	const FName GameSession = NAME_GameSession;
	const FName KeyGameType = TEXT("GTAIV_GAMETYPE");
	const FName KeyMap = TEXT("GTAIV_MAP");
	const FName KeyHost = TEXT("GTAIV_HOST");
}

UGTAIVSessions* UGTAIVSessions::Get(const UObject* WorldContext)
{
	const UWorld* W = WorldContext ? WorldContext->GetWorld() : nullptr;
	UGameInstance* GI = W ? W->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UGTAIVSessions>() : nullptr;
}

void UGTAIVSessions::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (IOnlineSessionPtr S = Sessions())
	{
		// a friend's Steam invite: join their game
		InviteHandle = S->AddOnSessionUserInviteAcceptedDelegate_Handle(
			FOnSessionUserInviteAcceptedDelegate::CreateUObject(this, &UGTAIVSessions::OnInviteAccepted));
	}
}

void UGTAIVSessions::Deinitialize()
{
	if (IOnlineSessionPtr S = Sessions())
	{
		S->ClearOnSessionUserInviteAcceptedDelegate_Handle(InviteHandle);
	}
	Super::Deinitialize();
}

IOnlineSessionPtr UGTAIVSessions::Sessions() const
{
	const IOnlineSubsystem* OSS = Online::GetSubsystem(GetWorld());
	if (!OSS)
	{
		OSS = IOnlineSubsystem::Get();
	}
	return OSS ? OSS->GetSessionInterface() : nullptr;
}

bool UGTAIVSessions::IsLan() const
{
	const IOnlineSubsystem* OSS = IOnlineSubsystem::Get();
	return !OSS || OSS->GetSubsystemName() == NULL_SUBSYSTEM;
}

FString UGTAIVSessions::GetServiceName() const
{
	return IsLan() ? TEXT("LAN") : TEXT("Steam");
}

bool UGTAIVSessions::IsInGame() const
{
	const UWorld* W = GetWorld();
	return W && W->GetNetMode() != NM_Standalone;
}

bool UGTAIVSessions::IsHosting() const
{
	const UWorld* W = GetWorld();
	return W && (W->GetNetMode() == NM_ListenServer || W->GetNetMode() == NM_DedicatedServer);
}

int32 UGTAIVSessions::GetPlayerCount() const
{
	const UWorld* W = GetWorld();
	const AGameStateBase* GS = W ? W->GetGameState() : nullptr;
	return GS ? GS->PlayerArray.Num() : 1;
}

// ------------------------------------------------------------------ hosting

void UGTAIVSessions::HostGame(int32 MaxPlayers, const FString& Map)
{
	UWorld* W = GetWorld();
	IOnlineSessionPtr S = Sessions();
	if (!W)
	{
		return;
	}
	PendingMap = Map.IsEmpty() ? UGameplayStatics::GetCurrentLevelName(W, false) : Map;
	// "/Game/Map?game=..." : the options (the game mode) travel along after ?listen
	PendingOptions.Reset();
	int32 Q = INDEX_NONE;
	if (PendingMap.FindChar(TEXT('?'), Q))
	{
		PendingOptions = PendingMap.Mid(Q);
		PendingMap.LeftInline(Q);
	}
	if (!PendingMap.StartsWith(TEXT("/")))
	{
		PendingMap = W->GetOutermost()->GetName();   // the map's package path
		PendingMap = UWorld::RemovePIEPrefix(PendingMap);
	}
	if (SinglePlayerMap.IsEmpty())
	{
		SinglePlayerMap = PendingMap;
	}
	if (!S)
	{
		// no online subsystem at all: still host (players join by IP: "open <ip>")
		Status = TEXT("Hosting (join by IP)");
		StartHosting(W);
		return;
	}
	if (S->GetNamedSession(GameSession))
	{
		S->DestroySession(GameSession);
	}
	FOnlineSessionSettings Settings;
	Settings.bIsLANMatch = IsLan();
	Settings.NumPublicConnections = FMath::Clamp(MaxPlayers, 2, 32);
	Settings.bShouldAdvertise = true;
	Settings.bAllowJoinInProgress = true;
	Settings.bAllowJoinViaPresence = true;
	Settings.bUsesPresence = !Settings.bIsLANMatch;
	Settings.bUseLobbiesIfAvailable = true;
	Settings.bAllowInvites = true;
	Settings.Set(KeyGameType, FString(TEXT("Free Mode")), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Settings.Set(KeyMap, FString(FPaths::GetBaseFilename(PendingMap)), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	FString HostName = TEXT("Player");
	if (const APlayerController* PC = W->GetFirstPlayerController())
	{
		if (PC->PlayerState)
		{
			HostName = PC->PlayerState->GetPlayerName();
		}
	}
	Settings.Set(KeyHost, HostName, EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	CreateHandle = S->AddOnCreateSessionCompleteDelegate_Handle(FOnCreateSessionCompleteDelegate::CreateUObject(this, &UGTAIVSessions::OnCreate));
	Status = TEXT("Creating the game...");
	const FUniqueNetIdPtr Id = W->GetFirstLocalPlayerFromController() ? W->GetFirstLocalPlayerFromController()->GetPreferredUniqueNetId().GetUniqueNetId() : nullptr;
	const bool bStarted = Id.IsValid() ? S->CreateSession(*Id, GameSession, Settings) : S->CreateSession(0, GameSession, Settings);
	if (!bStarted)
	{
		S->ClearOnCreateSessionCompleteDelegate_Handle(CreateHandle);
		OnCreate(GameSession, false);
	}
}

void UGTAIVSessions::StartHosting(UWorld* W)
{
	// already on that map, playing alone: open it to other players as it is (no reload)
	const FString Here = UWorld::RemovePIEPrefix(W->GetOutermost()->GetName());
	if (W->GetNetMode() == NM_Standalone && Here == UWorld::RemovePIEPrefix(PendingMap))
	{
		FURL URL(nullptr, *(PendingMap + TEXT("?listen") + PendingOptions), TRAVEL_Absolute);
		if (W->Listen(URL))
		{
			return;
		}
	}
	W->ServerTravel(PendingMap + TEXT("?listen") + PendingOptions);
}

void UGTAIVSessions::OnCreate(FName Name, bool bOk)
{
	if (IOnlineSessionPtr S = Sessions())
	{
		S->ClearOnCreateSessionCompleteDelegate_Handle(CreateHandle);
		if (bOk)
		{
			S->StartSession(GameSession);
		}
	}
	UWorld* W = GetWorld();
	if (!W)
	{
		return;
	}
	// even without the online service a listen server can be joined by IP
	Status = bOk ? FString::Printf(TEXT("Hosting Free Mode on %s"), *GetServiceName()) : TEXT("Couldn't create the game: hosting by IP");
	StartHosting(W);
}

// ------------------------------------------------------------------ finding and joining

void UGTAIVSessions::FindGames()
{
	IOnlineSessionPtr S = Sessions();
	if (!S || bSearching)
	{
		Status = S ? Status : TEXT("No online service");
		return;
	}
	Search = MakeShared<FOnlineSessionSearch>();
	Search->bIsLanQuery = IsLan();
	Search->MaxSearchResults = 100;
	Search->QuerySettings.Set(SEARCH_LOBBIES, true, EOnlineComparisonOp::Equals);
	FindHandle = S->AddOnFindSessionsCompleteDelegate_Handle(FOnFindSessionsCompleteDelegate::CreateUObject(this, &UGTAIVSessions::OnFind));
	bSearching = true;
	Found.Reset();
	Status = TEXT("Searching...");
	if (!S->FindSessions(0, Search.ToSharedRef()))
	{
		S->ClearOnFindSessionsCompleteDelegate_Handle(FindHandle);
		OnFind(false);
	}
}

void UGTAIVSessions::OnFind(bool bOk)
{
	bSearching = false;
	if (IOnlineSessionPtr S = Sessions())
	{
		S->ClearOnFindSessionsCompleteDelegate_Handle(FindHandle);
	}
	Found.Reset();
	if (!Search.IsValid())
	{
		return;
	}
	for (const FOnlineSessionSearchResult& R : Search->SearchResults)
	{
		FGTAIVFoundGame G;
		R.Session.SessionSettings.Get(KeyHost, G.Host);
		R.Session.SessionSettings.Get(KeyGameType, G.GameType);
		R.Session.SessionSettings.Get(KeyMap, G.Map);
		if (G.Host.IsEmpty())
		{
			G.Host = R.Session.OwningUserName;
		}
		if (G.GameType.IsEmpty())
		{
			continue;  // not one of ours (Spacewar, App ID 480, is shared by every test game)
		}
		G.MaxPlayers = R.Session.SessionSettings.NumPublicConnections;
		G.Players = G.MaxPlayers - R.Session.NumOpenPublicConnections;
		G.Ping = R.PingInMs;
		Found.Add(G);
	}
	Status = !bOk ? TEXT("Search failed") : Found.Num() ? FString::Printf(TEXT("%d games"), Found.Num()) : TEXT("No games found");
}

void UGTAIVSessions::JoinGame(int32 Index)
{
	IOnlineSessionPtr S = Sessions();
	if (!S || !Search.IsValid())
	{
		return;
	}
	// the found list skips other games: map the phone's index back to the search result
	int32 Seen = -1;
	for (const FOnlineSessionSearchResult& R : Search->SearchResults)
	{
		FString Type;
		R.Session.SessionSettings.Get(KeyGameType, Type);
		if (Type.IsEmpty() || ++Seen != Index)
		{
			continue;
		}
		if (S->GetNamedSession(GameSession))
		{
			S->DestroySession(GameSession);
		}
		JoinHandle = S->AddOnJoinSessionCompleteDelegate_Handle(FOnJoinSessionCompleteDelegate::CreateUObject(this, &UGTAIVSessions::OnJoin));
		Status = TEXT("Joining...");
		if (!S->JoinSession(0, GameSession, R))
		{
			S->ClearOnJoinSessionCompleteDelegate_Handle(JoinHandle);
			Status = TEXT("Couldn't join");
		}
		return;
	}
}

void UGTAIVSessions::OnJoin(FName Name, EOnJoinSessionCompleteResult::Type Result)
{
	IOnlineSessionPtr S = Sessions();
	if (S)
	{
		S->ClearOnJoinSessionCompleteDelegate_Handle(JoinHandle);
	}
	FString Address;
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (Result != EOnJoinSessionCompleteResult::Success || !S || !S->GetResolvedConnectString(GameSession, Address) || !PC)
	{
		Status = Result == EOnJoinSessionCompleteResult::SessionIsFull ? TEXT("That game is full") : TEXT("Couldn't join");
		return;
	}
	if (SinglePlayerMap.IsEmpty() && GetWorld())
	{
		SinglePlayerMap = GetWorld()->GetOutermost()->GetName();
		SinglePlayerMap = UWorld::RemovePIEPrefix(SinglePlayerMap);
	}
	Status = TEXT("Connecting...");
	PC->ClientTravel(Address, TRAVEL_Absolute);
}

void UGTAIVSessions::OnInviteAccepted(bool bOk, int32 User, FUniqueNetIdPtr From, const FOnlineSessionSearchResult& Result)
{
	if (!bOk || !Result.IsValid())
	{
		return;
	}
	IOnlineSessionPtr S = Sessions();
	if (!S)
	{
		return;
	}
	if (S->GetNamedSession(GameSession))
	{
		S->DestroySession(GameSession);
	}
	JoinHandle = S->AddOnJoinSessionCompleteDelegate_Handle(FOnJoinSessionCompleteDelegate::CreateUObject(this, &UGTAIVSessions::OnJoin));
	Status = TEXT("Joining your friend...");
	S->JoinSession(User, GameSession, Result);
}

void UGTAIVSessions::InviteFriends()
{
	const IOnlineSubsystem* OSS = IOnlineSubsystem::Get();
	IOnlineExternalUIPtr UI = OSS ? OSS->GetExternalUIInterface() : nullptr;
	if (UI.IsValid() && IsInGame())
	{
		UI->ShowInviteUI(0, GameSession);
		Status = TEXT("Steam overlay: invite friends");
	}
	else
	{
		Status = IsInGame() ? TEXT("Invites need Steam") : TEXT("Host a game first");
	}
}

void UGTAIVSessions::LeaveGame()
{
	IOnlineSessionPtr S = Sessions();
	if (S && S->GetNamedSession(GameSession))
	{
		DestroyHandle = S->AddOnDestroySessionCompleteDelegate_Handle(FOnDestroySessionCompleteDelegate::CreateUObject(this, &UGTAIVSessions::OnDestroy));
		S->DestroySession(GameSession);
		return;
	}
	OnDestroy(GameSession, true);
}

void UGTAIVSessions::OnDestroy(FName Name, bool bOk)
{
	if (IOnlineSessionPtr S = Sessions())
	{
		S->ClearOnDestroySessionCompleteDelegate_Handle(DestroyHandle);
	}
	Status = TEXT("Left the game");
	UWorld* W = GetWorld();
	if (!W)
	{
		return;
	}
	FString Map = SinglePlayerMap;
	if (Map.IsEmpty())
	{
		Map = W->GetOutermost()->GetName();
		Map = UWorld::RemovePIEPrefix(Map);
	}
	UGameplayStatics::OpenLevel(W, FName(*Map), true);
}
