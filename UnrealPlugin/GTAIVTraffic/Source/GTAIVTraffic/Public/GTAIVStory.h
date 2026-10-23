#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "GTAIVStory.generated.h"

class UAnimSequence;
class UAudioComponent;
class UCameraComponent;
class USkeletalMesh;
class USkeletalMeshComponent;
class USoundBase;
class UStaticMesh;
class UStaticMeshComponent;
class ACameraActor;
class AGTAIVVehicle;
class AGTAIVPlayerHUD;

/** One thing in a cutscene shot: a character (skeletal mesh + its body animation), a prop or a
 *  vehicle, and where it is on every frame. */
USTRUCT(BlueprintType)
struct FGTAIVCutActor
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FName Model;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USkeletalMesh> SkeletalMesh = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<UAnimSequence> Anim = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<UStaticMesh> StaticMesh = nullptr;
	/** A car from the exported vehicle set (body and wheels). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FName Vehicle;
	/** World placement per frame: x y z (cm) qx qy qz qw. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<float> Root;
};

/** One of the scene's lights (the .cut's [LIGHTS]): spots, fills and rims lighting the actors. */
USTRUCT(BlueprintType)
struct FGTAIVCutLight
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FName Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") bool bSpot = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") bool bRim = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FLinearColor Colour = FLinearColor::White;
	/** Per frame: position (cm), direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<float> Track;
};

/** A section of a cutscene (one GTA IV animation dictionary): its camera and actors. */
USTRUCT(BlueprintType)
struct FGTAIVCutShot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") int32 Frames = 0;
	/** Seconds from the start of the cutscene. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float Start = 0.f;
	/** Per frame: position (cm), forward, up, vertical field of view (degrees). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<float> Camera;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<FGTAIVCutActor> Actors;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<FGTAIVCutLight> Lights;
};

USTRUCT(BlueprintType)
struct FGTAIVSubtitle
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float Start = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float Duration = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FString Text;
};

/** A GTA IV cutscene (cuts.img), written by the GTA IV Exporter's story mode. */
UCLASS(BlueprintType)
class GTAIVTRAFFIC_API UGTAIVCutscene : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float Fps = 30.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float Duration = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<FGTAIVCutShot> Shots;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<FGTAIVSubtitle> Subtitles;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> Audio = nullptr;
	/** The time of day it plays at (hours, e.g. 20.6 for the opening's dusk); below 0 leaves the sky alone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float Hour = -1.f;
};

/** "The Cousins Bellic", GTA IV's first mission: its cutscenes, where it starts and ends, its text. */
UCLASS(BlueprintType)
class GTAIVTRAFFIC_API UGTAIVStoryData : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The opening: the Platypus, then Roman meeting Niko at the docks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<UGTAIVCutscene> Intro = nullptr;
	/** Roman's apartment (rom1_b). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<UGTAIVCutscene> End = nullptr;
	/** Where the opening leaves Niko, Roman and Roman's car (world, the mesh/actor placement). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FTransform PlayerStart;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FTransform RomanStart;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FTransform CarStart;
	/** Roman's front door (where rom1_b starts). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FVector Door = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FName CarName = TEXT("rom");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USkeletalMesh> RomanMesh = nullptr;
	/** The mission's lines (get_in, drive, dead, abandoned, passed, failed, ...) with GTA colour tokens. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TMap<FString, FString> Text;

	static UGTAIVStoryData* Load();
};

/** Plays a UGTAIVCutscene: its camera, characters, props, vehicles, audio and subtitles, with the
 *  player hidden and the HUD letterboxed. Enter / Space / A (held a moment in) skips. */
UCLASS(NotBlueprintable)
class GTAIVTRAFFIC_API AGTAIVCutscenePlayer : public AActor
{
	GENERATED_BODY()

public:
	AGTAIVCutscenePlayer();

	void Play(UGTAIVCutscene* InCutscene, TFunction<void()> InOnFinished);
	void Stop();
	/** Ends it now, as the skip button does (the "finished" step still runs). */
	void Skip() { if (Cutscene) Finish(); }
	bool IsPlaying() const { return Cutscene != nullptr; }
	float GetTime() const { return Time; }

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UPROPERTY() TObjectPtr<UGTAIVCutscene> Cutscene;
	UPROPERTY() TObjectPtr<ACameraActor> Camera;
	UPROPERTY() TObjectPtr<UAudioComponent> Audio;
	UPROPERTY() TArray<TObjectPtr<USceneComponent>> ShotComponents;
	struct FActorComps
	{
		int32 Actor = INDEX_NONE;
		TWeakObjectPtr<USceneComponent> Root;
		TWeakObjectPtr<USkeletalMeshComponent> Skel;
	};
	TArray<FActorComps> Live;
	TArray<TPair<int32, TWeakObjectPtr<class ULocalLightComponent>>> LiveLights;
	TFunction<void()> OnFinished;
	int32 Shot = INDEX_NONE;
	float Time = 0.f;
	int32 NextSubtitle = 0;
	TWeakObjectPtr<APawn> HiddenPawn;
	TArray<TWeakObjectPtr<AActor>> HiddenExtra;
	bool bTrafficWas = true;
	bool bPedsWas = true;
	void BuildShot(int32 Index);
	void ClearShot();
	void Finish();
	AGTAIVPlayerHUD* HUD() const;
};

UENUM()
enum class EGTAIVMissionState : uint8
{
	Idle,
	Intro,
	GetIn,
	Drive,
	Arrived,
	End,
	Passed,
	Failed,
	Done
};

/** GTA IV's first mission run in the world: the opening cutscene, "Get into Roman's car", the drive
 *  to Roman's place in Hove Beach with him as a passenger, his apartment cutscene, Mission Passed.
 *  Starts by itself when the level is opened with ?story (the front end's Start New Game). */
UCLASS()
class GTAIVTRAFFIC_API UGTAIVStory : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "GTA IV|Story", meta = (WorldContext = "WorldContext"))
	static UGTAIVStory* GetStory(UObject* WorldContext);

	/** Starts "The Cousins Bellic" from the opening cutscene (SkipIntro: from getting in the car). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Story") void StartMission(bool bSkipIntro = false);
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Story") void SkipCutscene();
	UFUNCTION(BlueprintPure, Category = "GTA IV|Story") FString GetDebugState() const;
	UFUNCTION(BlueprintPure, Category = "GTA IV|Story") bool IsInCutscene() const;
	UFUNCTION(BlueprintPure, Category = "GTA IV|Story") EGTAIVMissionState GetState() const { return State; }
	/** Teleports Niko into Roman's car at the destination (testing). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Story") void DebugWarpToDestination();

	/** Sets the sun, the sky and the city's night glow (windows, street lights) for an hour of the day. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Story", meta = (WorldContext = "WorldContext"))
	static void SetTimeOfDay(UObject* WorldContext, float Hour);

	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UGTAIVStory, STATGROUP_Tickables); }
	virtual bool IsTickable() const override { return State != EGTAIVMissionState::Idle || bPendingStart; }
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override
	{
		return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
	}

private:
	UPROPERTY() TObjectPtr<UGTAIVStoryData> Data;
	UPROPERTY() TObjectPtr<AGTAIVCutscenePlayer> Player;
	UPROPERTY() TObjectPtr<AGTAIVVehicle> Car;
	UPROPERTY() TObjectPtr<AActor> Roman;
	UPROPERTY() TObjectPtr<AActor> Marker;
	EGTAIVMissionState State = EGTAIVMissionState::Idle;
	float StateTime = 0.f;
	float Fade = 0.f;
	bool bPendingStart = false;
	bool bPendingSkipIntro = false;
	float PendingDelay = 0.f;
	bool bRomanInCar = false;
	FVector Destination = FVector::ZeroVector;
	double NextRoute = 0.0;
	float AbandonTime = 0.f;
	void SetState(EGTAIVMissionState S);
	void PlayCutscene(UGTAIVCutscene* C, TFunction<void()> Then);
	void SetupGetIn();
	void SpawnRoman();
	void RomanGetsIn();
	void ClearMission(bool bKeepCar);
	void Fail(const FString& Reason);
	void UpdateRoute();
	APawn* PlayerPawn() const;
	APawn* PlayerOnFoot() const;
	AGTAIVPlayerHUD* HUD() const;
	FString Line(const TCHAR* Key, const TCHAR* Fallback) const;
	FVector FindDestination() const;
};
