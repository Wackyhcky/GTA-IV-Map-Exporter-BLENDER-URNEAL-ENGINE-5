#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "GTAIVRadio.generated.h"

class UAudioComponent;
class USoundWaveProcedural;
class UTexture2D;
class FGTAIVRadioStream;

USTRUCT(BlueprintType)
struct FGTAIVRadioStationInfo
{
	GENERATED_BODY()

	/** radio_<Id>.rpf, and the folder of its audio under Content/GTAIV/RadioData. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FName Id;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FString Name;
	/** radio_hud's logos: colour (the station playing) and grey (the others in the strip). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<UTexture2D> Logo;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<UTexture2D> LogoGrey;
};

/** GTA IV's radio stations, written by the GTA IV Exporter (/Game/GTAIV/Radio/DA_GTAIV_Radio). The
 *  audio itself (the game's ADPCM, decoded while it plays) and the programme of each station are in
 *  Content/GTAIV/RadioData (radio.json). */
UCLASS(BlueprintType)
class GTAIVTRAFFIC_API UGTAIVRadioData : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<FGTAIVRadioStationInfo> Stations;
};

/** GTA IV's car radio. Every station is live: it plays on with its own programme while you aren't
 *  listening (songs, the DJ talking over the intros and at the time of day, idents, DJ monologues,
 *  adverts, news and weather, following the game's rules from game.dat16), so tuning in lands in the
 *  middle of whatever is on. The drivable GTA IV vehicles turn it on for the player; mouse wheel,
 *  comma / period or the D-pad change station, like the game. */
UCLASS()
class GTAIVTRAFFIC_API UGTAIVRadio : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	static UGTAIVRadio* Get(const UObject* WorldContext);
	/** The radio of the world WorldContext is in. */
	UFUNCTION(BlueprintPure, Category = "GTA IV|Radio", meta = (WorldContext = "WorldContext", DisplayName = "Get GTA IV Radio"))
	static UGTAIVRadio* GetRadio(UObject* WorldContext) { return Get(WorldContext); }

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return false; }

	/** Stations and Radio Off (the last entry). */
	UFUNCTION(BlueprintPure, Category = "GTA IV|Radio") int32 GetStationCount() const { return Stations.Num(); }
	UFUNCTION(BlueprintPure, Category = "GTA IV|Radio") bool HasStations() const { return Stations.Num() > 0; }
	/** The station playing (INDEX_NONE: radio off). */
	UFUNCTION(BlueprintPure, Category = "GTA IV|Radio") int32 GetStation() const { return Current; }
	UFUNCTION(BlueprintPure, Category = "GTA IV|Radio") FString GetStationName(int32 Index) const;
	UFUNCTION(BlueprintPure, Category = "GTA IV|Radio") UTexture2D* GetStationLogo(int32 Index, bool bColour) const;
	UFUNCTION(BlueprintPure, Category = "GTA IV|Radio") bool IsListening() const { return bListening; }
	/** Seconds since the station was changed (the HUD shows the logo strip for a moment). */
	UFUNCTION(BlueprintPure, Category = "GTA IV|Radio") float GetTimeSinceTuned() const;
	/** A random station (what a car has on when you get in). */
	UFUNCTION(BlueprintPure, Category = "GTA IV|Radio") int32 RandomStation() const;

	/** Turns the radio on (on Station) or off: the vehicles do this when the player gets in/out. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Radio") void SetListening(bool bOn, int32 Station);
	/** Next (+1) / previous (-1) station; Radio Off sits after the last one. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Radio") void Retune(int32 Step);
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Radio") void SetStation(int32 Index);
	/** Getting out: the radio plays on from the car, softer with distance (and muffled), until the
	 *  player is far away or in another car. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Radio") void LeaveInVehicle(AActor* Vehicle);
	/** The car the radio is playing from (null while the player is in a car, or off). */
	UFUNCTION(BlueprintPure, Category = "GTA IV|Radio") AActor* GetPlayingFrom() const { return Source.Get(); }
	/** How far away the parked car's radio can be heard (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Radio") float HearingDistance = 3000.f;
	/** What's on: station, programme item, seconds into it, whether audio is playing. */
	UFUNCTION(BlueprintPure, Category = "GTA IV|Radio") FString GetDebugState() const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Radio") float Volume = 0.8f;

private:
	struct FClip
	{
		FString File;
		float Len = 0.f;
	};
	enum class EKind : uint8 { Music, Ident, Solo, Advert, News, Weather, ToAd, ToNews, ToWeather, Count };
	struct FSlot
	{
		FClip Clip;
		EKind Kind = EKind::Music;
		FClip Voice;          // the DJ over the song's intro
		float VoiceAt = 0.f;
		double Start = 0.0;   // station clock
	};
	struct FStation
	{
		FName Id;
		FString Name;
		TObjectPtr<UTexture2D> Logo = nullptr;
		TObjectPtr<UTexture2D> LogoGrey = nullptr;
		bool bTalk = false;
		FString AdPool;
		TArray<FClip> Music, Idents, Solo, General, ToAd, ToNews, ToWeather;
		TMap<FString, TArray<FClip>> Time;
		// the live programme
		TArray<FSlot> Queue;        // Queue[0] is on now
		TArray<EKind> History;      // last kinds aired
		TMap<int32, double> LastAired;   // kind -> station clock
		TArray<FString> RecentSongs;
		bool bStarted = false;
	};
	TArray<FStation> Stations;
	/** Holds the logos (FStation is a plain struct, invisible to GC). */
	UPROPERTY() TObjectPtr<UGTAIVRadioData> RadioData;
	TMap<FString, TArray<FClip>> Adverts, Weather;
	TArray<FClip> News;
	float AfterMusic[(int32)EKind::Count] = {};
	float AfterTwoSongs[(int32)EKind::Count] = {};
	float MinRepeat[(int32)EKind::Count] = {};
	FString DataDir;

	bool bListening = false;
	TWeakObjectPtr<AActor> Source;   // playing from a parked car
	void SetPositional(AActor* From);
	int32 Current = INDEX_NONE;
	double TunedAt = -100.0;
	UPROPERTY() TObjectPtr<UAudioComponent> MusicAudio;
	UPROPERTY() TObjectPtr<UAudioComponent> VoiceAudio;
	UPROPERTY() TObjectPtr<UAudioComponent> StaticAudio;
	FString PlayingFile;
	double PlayingStart = 0.0;
	bool bVoicePlayed = false;
	float Duck = 1.f;
	int32 LoadSerial = 0;
	/** A clip playing: its decoder and the wave it feeds a couple of seconds ahead. */
	struct FFeed
	{
		TSharedPtr<FGTAIVRadioStream> Stream;
		TWeakObjectPtr<USoundWaveProcedural> Wave;
	};
	FFeed MusicFeed, VoiceFeed;
	void Feed(FFeed& F);

	void Load();
	double Clock() const;
	void Advance(FStation& S, double Now);
	void Schedule(FStation& S);
	const FClip* Pick(const TArray<FClip>& From, TArray<FString>* Recent = nullptr, int32 Avoid = 0) const;
	void Play(const FSlot& Slot, double Now);
	void PlayVoice(const FClip& Clip);
	void PlayStatic();
	void StopAll();
	UAudioComponent* MakeAudio(const TCHAR* Name);
	/** Opens a .gtr on a worker thread (decoding up to Offset seconds), then plays it on Target. */
	void Stream(UAudioComponent* Target, FFeed& Into, const FClip& Clip, float Offset);
};

/** Streams the exporter's .gtr files (GTA IV's IMA ADPCM, repacked) as interleaved 16-bit PCM:
 *  only the compressed file stays in memory. */
class GTAIVTRAFFIC_API FGTAIVRadioStream
{
public:
	bool Open(TArray<uint8>&& InFile);
	/** Decodes the next frames (all channels) into Out; returns the frames written (0 at the end). */
	int32 Read(int16* Out, int32 Frames);
	/** Decodes and drops frames (to start in the middle). */
	void Skip(int64 Frames);
	int32 Rate = 0;
	int32 Channels = 0;
	int64 Frames = 0;
	int64 Position = 0;

private:
	struct FChannel
	{
		int64 SegTable = 0, SegCount = 0, Data = 0, Bytes = 0;
		int64 Seg = 0, SegByte = 0, Sample = 0;   // current segment, its first byte, sample within it
		int32 Pred = 0, Index = 0;
	};
	TArray<uint8> File;
	TArray<FChannel> Chans;
	int16 Next(FChannel& C);
};
