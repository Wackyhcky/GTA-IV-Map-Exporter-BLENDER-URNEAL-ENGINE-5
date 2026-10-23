#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameFramework/HUD.h"
#include "GTAIVPlayerHUD.generated.h"

class UTexture2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UGTAIVFrontendData;

/** One hud.dat entry: position, size (or text scale), colour and alpha. */
USTRUCT(BlueprintType)
struct FGTAIVHudElement
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FVector2D Pos = FVector2D::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FVector2D Size = FVector2D::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FLinearColor Colour = FLinearColor::White;
};

/** An info.zon area box (GTA IV world coordinates, metres). */
USTRUCT(BlueprintType)
struct FGTAIVZone
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FString Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FVector2D Min = FVector2D::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FVector2D Max = FVector2D::ZeroVector;
};

/** The in-game HUD's data, written by the GTA IV Exporter from the game's files. */
UCLASS(BlueprintType)
class GTAIVTRAFFIC_API UGTAIVHudData : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout") TMap<FString, FGTAIVHudElement> Layout;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout") TMap<FString, FLinearColor> Colours;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Areas") TArray<FGTAIVZone> Zones;

	/** All radar tiles in one texture: RadarCols x RadarRows tiles of RadarTile metres, top-left at RadarOrigin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radar") TObjectPtr<UTexture2D> RadarMap;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radar") int32 RadarCols = 12;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radar") int32 RadarRows = 9;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radar") float RadarTile = 500.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radar") FVector2D RadarOrigin = FVector2D(-3000, 3000);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radar") TObjectPtr<UMaterialInterface> RadarMaterial;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radar") TObjectPtr<UMaterialInterface> RingMaterial;

	/** hud.wtd / blips.wtd textures by name (radarringback, radar_centre, radar_north, ...) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Textures") TMap<FString, TObjectPtr<UTexture2D>> Textures;

	/** Font (the front end's data holds the GTA IV font atlas and metrics). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") TObjectPtr<UGTAIVFrontendData> Font;
};

/** GTA IV's in-game HUD: radar with health/armour ring and blips, cash, wanted stars, weapon,
 *  area names, help text and subtitles, laid out from the game's hud.dat. */
UCLASS(Blueprintable)
class GTAIVTRAFFIC_API AGTAIVPlayerHUD : public AHUD
{
	GENERATED_BODY()

public:
	AGTAIVPlayerHUD();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TObjectPtr<UGTAIVHudData> Data;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	bool bShowGTAHud = true;

	/** 0..1 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	float Health = 1.f;

	/** 0..1 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	float Armour = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	int32 Money = 0;

	/** 0..6 stars */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	int32 WantedLevel = 0;

	/** Weapon icon texture name (hud.wtd), e.g. fist_icon. Empty hides it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FString WeaponIcon = TEXT("fist_icon");

	/** Ammo text shown next to the weapon icon (empty for melee). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FString AmmoText;

	/** Weapon icon texture (overrides WeaponIcon); set by the GTA IV Player component. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TObjectPtr<UTexture2D> WeaponIconTexture;

	/** Radar radius on foot and at full speed in a vehicle (metres). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	float RadarRangeOnFoot = 120.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	float RadarRangeFast = 260.f;

	/** Shown after "LOADING -" on the loading screen after you're wasted (e.g. the mission's
	 *  name); empty shows the area you died in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FString LoadingSubtitle;

	/** Seconds of the wasted sequence before the level reloads (slow motion, grey, the camera
	 *  pulling back, then the loading screen). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	float WastedSeconds = 10.f;

	/** How long an area name stays up after entering the area (seconds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	float AreaNameSeconds = 5.f;

	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void ShowHelpText(const FString& Text, float Seconds = 6.f);

	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void ShowSubtitle(const FString& Text, float Seconds = 4.f);

	/** Shows a name bottom right like an area name (e.g. the vehicle just entered). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void ShowAreaText(const FString& Text);
	/** The vehicle's name (grey, above the area name) when getting in. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|HUD")
	void ShowVehicleName(const FString& Text);

	/** Waypoint on the radar (world location); call ClearWaypoint to remove. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void SetWaypoint(FVector Location);

	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void ClearWaypoint();

	/** Radar blip using a blips.wtd sprite (radar_safehouse, radar_police, ...). Same Id replaces it. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void SetBlip(FName Id, FVector Location, const FString& Sprite);

	/** A mission blip (radar_objective, tinted): stays on the radar's rim when it's far away. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Story")
	void SetObjectiveBlip(FName Id, FVector Location, FLinearColor Colour);

	/** The GPS line on the radar (world points, cm); empty clears it. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Story")
	void SetRoute(const TArray<FVector>& Points, FLinearColor Colour);

	/** Cutscenes: black bars and subtitles, nothing else. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Story") void SetCutsceneMode(bool bOn) { bCutsceneMode = bOn; }
	UFUNCTION(BlueprintPure, Category = "GTA IV|Story") bool IsCutsceneMode() const { return bCutsceneMode; }
	/** Black over the screen, 0..1 (the story's fades). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Story") void SetFade(float Alpha) { FadeAlpha = Alpha; }
	/** MISSION PASSED / MISSION FAILED in the middle of the screen. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Story") void ShowBigMessage(const FString& Text, float Seconds, FLinearColor Colour);

	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void RemoveBlip(FName Id);

	/** Area name (info.zon) at a world location. */
	UFUNCTION(BlueprintPure, Category = "GTA IV")
	FString GetAreaName(FVector Location) const;

	/** The area the player is in (as last shown on the HUD). */
	UFUNCTION(BlueprintPure, Category = "GTA IV")
	FString GetCurrentArea() const { return CurrentArea; }

	virtual void DrawHUD() override;

	/** GTA IV's phone (Up arrow): multiplayer (host, find and join games, invite friends, players,
	 *  leave), and the rest of the Badger's home screen. */
	UFUNCTION(BlueprintPure, Category = "GTA IV|Phone") bool IsPhoneOpen() const { return bPhone; }
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Phone") void SetPhoneOpen(bool bOpen);
	/** Shows the multiplayer player list for a few seconds (it also shows while Tab / Back is held). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV|Multiplayer") void ShowPlayerList();

protected:
	virtual void BeginPlay() override;

private:
	struct FBlip { FVector Location; FString Sprite; FLinearColor Colour = FLinearColor::White; bool bKeep = false; };
	TArray<FVector> Route;
	FLinearColor RouteColour = FLinearColor::White;
	bool bCutsceneMode = false;
	float FadeAlpha = 0.f;
	FString BigMessage;
	FLinearColor BigColour = FLinearColor::White;
	double BigSince = -100, BigUntil = 0;
	void DrawColouredLine(const FString& Raw, FVector2D Pos, FVector2D Scale, int32 Align);
	void DrawStoryOverlay(double Now);
	TMap<FName, FBlip> Blips;
	bool bWaypoint = false;
	FVector Waypoint = FVector::ZeroVector;
	FString HelpText, Subtitle, AreaShown, CurrentArea, VehicleShown;
	double HelpUntil = 0, SubtitleUntil = 0, AreaSince = -100, VehicleSince = -100;
	void DrawRadio();

	// multiplayer: the player list (top left, "Free Mode" and everyone's name in their colour; hold
	// Tab / Back, and for a few seconds when someone joins or leaves) and names over other players
	int32 ListedPlayers = -1;
	double ListSince = -100.0;
	void DrawPlayerList(double Now);
	void DrawNameTags();
	FLinearColor PlayerColour(const class APlayerState* P) const;

	// the phone
	enum class EPhonePage : uint8 { Home, Multiplayer, Find, Players, Info };
	bool bPhone = false;
	float PhoneUp = 0.f;               // 0 hidden .. 1 up
	EPhonePage Page = EPhonePage::Home;
	int32 PhoneSel = 0;
	FString InfoTitle, InfoText;
	double LastPhoneTick = 0.0;
	void DrawPhone();
	void PhoneInput();
	void PhoneSelect();
	void PhoneBack();
	TArray<FString> PhoneItems() const;
	void Box(FVector2D Min, FVector2D Size, FLinearColor Color);
	FBox2D Area;
	float DisplayedHealth = 1.f, DisplayedArmour = 0.f;
	TMap<int32, int32> MainCells, SubCells;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> RadarMID;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> RingMID;

	void BuildFont();
	const FGTAIVHudElement* El(const TCHAR* Name) const;
	FLinearColor Col(const TCHAR* Name, float Alpha = 1.f) const;
	FVector2D ToPx(FVector2D N) const { return Area.Min + N * Area.GetSize(); }
	void Tex(const FString& Name, FVector2D CentrePx, FVector2D SizePx, FLinearColor Color, float Rotation = 0.f);
	float TextWidth(const FString& Text, FVector2D Scale, bool bSub) const;
	void Text(const FString& Text, FVector2D Pos, FVector2D Scale, FLinearColor Color, int32 Align, bool bSub = false,
			  bool bShadow = true);
	void DrawRadar(const FVector& PlayerLoc, float CameraYaw, float Speed);
	void DrawTopRight();
	void DrawMessages(double Now);

	// wasted: slow motion, grey, the camera pulling back, "WASTED", then the loading screen
	bool bDeath = false;
	bool bReloaded = false;
	double DeathStart = 0.0;
	double RespawnFadeStart = -100.0;
	float CamYaw = 0.f, CamDist0 = 300.f, CamHeight0 = 100.f;
	TWeakObjectPtr<class ACameraActor> DeathCam;
	TMap<int32, int32> Font3Cells;
	static bool bRespawnFade;
	void UpdateDeath(double Real);
	void DrawLoadingText(float T);
	FVector BodyLocation(const APawn* Pawn) const;
};
