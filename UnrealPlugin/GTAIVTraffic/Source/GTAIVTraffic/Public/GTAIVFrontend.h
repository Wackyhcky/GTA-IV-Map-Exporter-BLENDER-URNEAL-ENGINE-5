#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/SaveGame.h"
#include "GTAIVFrontend.generated.h"

class UTexture2D;
class USoundBase;
class UAudioComponent;

/** One animated texture of a startup/loading screen (loadingscreens_pc.dat). Times in ms; every
 *  MoveTime ms the position moves by Move, every ScaleTime ms the size grows by Scale. Positions
 *  and sizes are fractions of a 16:9 screen; Pos is the centre. */
USTRUCT(BlueprintType)
struct FGTAIVFeObject
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<UTexture2D> Texture;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float StartTime = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float EndTime = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float MoveTime = 20.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float ScaleTime = 20.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FVector2D Pos = FVector2D(0.5, 0.5);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FVector2D Move = FVector2D::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FVector2D Size = FVector2D(1, 1);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FVector2D Scale = FVector2D::ZeroVector;
};

USTRUCT(BlueprintType)
struct FGTAIVFeScreen
{
	GENERATED_BODY()

	/** LEGAL, LEGAL2, START_INTRO, END_INTRO, INITIAL_MAIN, ... */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FString Flag;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float Duration = 0.f;
	/** NONE or IN_OUT */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FString Fade;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<FGTAIVFeObject> Objects;
};

USTRUCT(BlueprintType)
struct FGTAIVFeOption
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FString Label;
	/** MENUOPT_ADJUST, MENUOPT_JUMP_NEXT_MENU, ... (frontend_menus.xml) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FString Action;
	/** The preference (PREF_*) or target menu (SCR_*) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FString Value;
	/** Number of values (sliders: steps) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") int32 Scaler = 0;
	/** MENU_DISPLAY_* */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FString Display;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<FString> DisplayValues;
};

USTRUCT(BlueprintType)
struct FGTAIVFeMenu
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FString Id;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FString Header;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<FGTAIVFeOption> Options;
};

/** Everything the GTA IV front end needs, written by the GTA IV Exporter from the game's files. */
UCLASS(BlueprintType)
class GTAIVTRAFFIC_API UGTAIVFrontendData : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Startup") TArray<FGTAIVFeScreen> Screens;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Startup") FString LegalText1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Startup") FString LegalText2;
	/** Loops behind the startup and the title menu (loading.rpf: loading_tune). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Startup") TObjectPtr<USoundBase> Music;
	/** Played when Start New Game is chosen, carrying on into the level (starting_tune). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Startup") TObjectPtr<USoundBase> StartMusic;
	/** GTA IV's menu music (menu_streamed), for pause menus. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Startup") TObjectPtr<USoundBase> MenuMusic;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Menu") FString TitleHeader;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Menu") TArray<FGTAIVFeOption> TitleItems;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Menu") TArray<FGTAIVFeMenu> Menus;
	/** frontend_pc.dat [HD]: normalised positions/sizes by name */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Menu") TMap<FString, FVector2D> Layout;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Menu") TObjectPtr<UTexture2D> Cursor;
	/** The level "Start New Game" opens. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Menu") FString StartLevel;

	/** font1 atlas: 16x16 cells, cell -> character code, proportional trim per cell (fonts.dat) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") TObjectPtr<UTexture2D> FontTexture;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") TArray<int32> FontMap;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") TArray<int32> FontProp;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") int32 FontMainEnd = 134;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") int32 FontCommonStart = 150;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") int32 FontWhitespace = 8;

	/** The PC title screen art by aspect ratio (w = 16:9, 21by9, 16by10, 4by3, 5by4). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Title") TMap<FString, TObjectPtr<UTexture2D>> TitleBackgrounds;
	/** The title screen's bar: Start, Options, Social Club, Back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Title") TArray<FString> TitleBar;

	/** font3 (garage style, the in-game loading text): atlas, cell -> char, proportional trims. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") TObjectPtr<UTexture2D> Font3Texture;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") TArray<int32> Font3Map;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") TArray<int32> Font3Prop;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") int32 Font3Start = 92;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Font") int32 Font3End = 164;

	/** "Loading -" and "Wasted" (american.gxt). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Text") FString LoadingText = TEXT("Loading -");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Text") FString WastedText = TEXT("Wasted");
};

/** The player's menu settings (preference -> value index), saved between runs. */
UCLASS()
class GTAIVTRAFFIC_API UGTAIVFrontendSettings : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY() TMap<FString, int32> Values;
};

/** Plays GTA IV's startup (legal screens, Rockstar logos, loading screens) and runs its title
 *  menu and option pages, drawn with the game's textures and font. */
UCLASS(Blueprintable)
class GTAIVTRAFFIC_API AGTAIVFrontendHUD : public AHUD
{
	GENERATED_BODY()

public:
	AGTAIVFrontendHUD();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TObjectPtr<UGTAIVFrontendData> Data;

	/** Current value of a preference (PREF_*), as the menus store it. */
	UFUNCTION(BlueprintPure, Category = "GTA IV")
	static int32 GetPreference(const FString& Pref, int32 Default = 0);

	/** Jump past the startup sequence to the title menu. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void SkipIntro();

	/** Open a menu page by id (SCR_TOPTIONS, SCR_TGRAPHICS, SCR_TAUDIO, ...). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void OpenMenu(const FString& MenuId);

	virtual void DrawHUD() override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	enum class EState { Intro, Title, Menu, Starting };

	struct FRow
	{
		FBox2D Box;
		int32 Index;
	};

	EState State = EState::Intro;
	int32 ScreenIndex = 0;
	float ScreenTime = 0.f;          // ms into the current screen
	int32 LoopStart = 0;             // first loading screen (INITIAL_MAIN)
	int32 BgIndex = 0;               // background loading screen behind the menus
	float BgTime = 0.f;
	double LastTime = 0.0;
	TArray<FString> MenuStack;
	int32 Selected = 0;
	float RepeatTimer = 0.f;
	float StartingFrames = 0.f;
	double StartingSince = 0.0;
	TArray<FRow> Rows;
	int32 TitleSel = 0;
	TArray<FBox2D> TitleRects;
	bool bSkipLoading = false;
	FBox2D Area;                     // 16:9 area of the viewport
	TMap<int32, int32> CharCell;
	UPROPERTY() TObjectPtr<UGTAIVFrontendSettings> Settings;   // (a UPROPERTY, or GC frees it under the menu)
	TArray<FIntPoint> Resolutions;
	UPROPERTY() TObjectPtr<UAudioComponent> MusicComp;

	void BuildFont();
	void LoadSettings();
	void SaveSettings();
	void ApplySettings();
	int32 Pref(const FString& Name, int32 Default) const;
	void SetPref(const FString& Name, int32 V);
	int32 DefaultFor(const FGTAIVFeOption& O) const;
	int32 ValueCount(const FGTAIVFeOption& O) const;
	FString ValueText(const FGTAIVFeOption& O) const;
	bool IsSlider(const FGTAIVFeOption& O) const;
	bool Selectable(const FGTAIVFeOption& O) const;
	const FGTAIVFeMenu* CurrentMenu() const;
	TArray<FGTAIVFeOption> CurrentItems() const;

	void Advance(float DtMs);
	void HandleInput(float DtMs);
	void Activate(int32 Index);
	void Adjust(int32 Index, int32 Dir);
	void Back();
	void MoveSelection(int32 Dir);

	void DrawScreen(const FGTAIVFeScreen& S, float T, float Alpha);
	void DrawLegal(const FString& Text);
	void DrawMenu();
	void EnterTitle();
	void EnterLoading();
	void ActivateTitle(int32 Index);
	void StartGame();
	void DrawTitleBackground();
	void DrawTitleScreen();
	float TextWidthPx(const FString& Text, float CellH) const;
	void DrawTextPx(const FString& Text, FVector2D TopLeft, float CellH, FLinearColor Color);
	float TextWidth(const FString& Text, FVector2D Scale) const;
	void DrawText2(const FString& Text, FVector2D Pos, FVector2D Scale, FLinearColor Color, int32 Align = 0);
	TArray<FString> Wrap(const FString& Text, FVector2D Scale, float MaxWidth) const;
	void Rect(FVector2D Pos, FVector2D Size, FLinearColor Color);
	FVector2D L(const FString& Name, FVector2D Default) const;
	FVector2D ToPx(FVector2D N) const { return Area.Min + N * Area.GetSize(); }
};

UCLASS()
class GTAIVTRAFFIC_API AGTAIVFrontendGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AGTAIVFrontendGameMode();
};
