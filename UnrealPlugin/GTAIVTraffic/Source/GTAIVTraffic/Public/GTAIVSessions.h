#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "OnlineSessionSettings.h"
#include "GTAIVSessions.generated.h"

/** A game found on the network (Steam lobbies, or the LAN when Steam isn't running). */
USTRUCT(BlueprintType)
struct FGTAIVFoundGame
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "GTA IV|Multiplayer") FString Host;
	UPROPERTY(BlueprintReadOnly, Category = "GTA IV|Multiplayer") FString GameType;
	UPROPERTY(BlueprintReadOnly, Category = "GTA IV|Multiplayer") FString Map;
	UPROPERTY(BlueprintReadOnly, Category = "GTA IV|Multiplayer") int32 Players = 0;
	UPROPERTY(BlueprintReadOnly, Category = "GTA IV|Multiplayer") int32 MaxPlayers = 0;
	UPROPERTY(BlueprintReadOnly, Category = "GTA IV|Multiplayer") int32 Ping = 0;
};

/** GTA IV style multiplayer through the online subsystem (Steam lobbies; LAN when Steam isn't
 *  available): host a Free Mode game on the current map, find games, join one, invite Steam
 *  friends, accept invites, leave. The phone drives it. */
UCLASS()
class GTAIVTRAFFIC_API UGTAIVSessions : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	static UGTAIVSessions* Get(const UObject* WorldContext);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Hosts a game on Map ("/Game/...", options allowed after ?), or on the current map when empty;
	 *  it (re)loads as a listen server. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Multiplayer") void HostGame(int32 MaxPlayers = 16, const FString& Map = TEXT(""));
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Multiplayer") void FindGames();
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Multiplayer") void JoinGame(int32 Index);
	/** Leaves (or ends, when hosting) the game and goes back to playing alone. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Multiplayer") void LeaveGame();
	/** Steam's overlay to invite friends to this game. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Multiplayer") void InviteFriends();

	UFUNCTION(BlueprintPure, Category = "GTA IV|Multiplayer") const TArray<FGTAIVFoundGame>& GetFoundGames() const { return Found; }
	UFUNCTION(BlueprintPure, Category = "GTA IV|Multiplayer") bool IsSearching() const { return bSearching; }
	UFUNCTION(BlueprintPure, Category = "GTA IV|Multiplayer") bool IsInGame() const;
	UFUNCTION(BlueprintPure, Category = "GTA IV|Multiplayer") bool IsHosting() const;
	/** What's going on, for the phone ("Searching...", "Joining...", errors). */
	UFUNCTION(BlueprintPure, Category = "GTA IV|Multiplayer") FString GetStatus() const { return Status; }
	/** "Steam" or "LAN". */
	UFUNCTION(BlueprintPure, Category = "GTA IV|Multiplayer") FString GetServiceName() const;
	UFUNCTION(BlueprintPure, Category = "GTA IV|Multiplayer") int32 GetPlayerCount() const;

private:
	IOnlineSessionPtr Sessions() const;
	bool IsLan() const;
	FString SinglePlayerMap;
	FString PendingMap;
	FString PendingOptions;
	TSharedPtr<FOnlineSessionSearch> Search;
	TArray<FGTAIVFoundGame> Found;
	bool bSearching = false;
	FString Status;
	FDelegateHandle CreateHandle, StartHandle, FindHandle, JoinHandle, DestroyHandle, InviteHandle;
	void OnCreate(FName Name, bool bOk);
	void OnFind(bool bOk);
	void OnJoin(FName Name, EOnJoinSessionCompleteResult::Type Result);
	void OnDestroy(FName Name, bool bOk);
	void OnInviteAccepted(bool bOk, int32 User, FUniqueNetIdPtr From, const FOnlineSessionSearchResult& Result);
	void TravelTo(const FOnlineSessionSearchResult& Result);
	void StartHosting(UWorld* W);
};
