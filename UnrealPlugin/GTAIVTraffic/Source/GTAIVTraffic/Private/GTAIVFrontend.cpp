#include "GTAIVFrontend.h"
#include "GTAIVStory.h"

#include "Components/AudioComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "CanvasItem.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	const TCHAR* SaveSlot = TEXT("GTAIVSettings");
	const float FadeMs = 500.f;      // loading screen fades
	const float MenuFadeMs = 200.f;  // FAD_main_fade_time
	const float StartFadeSeconds = 1.f;  // loading theme fade-out on Start New Game
	const double LoadingSeconds = 9.0;   // loading screens shown after Start
	const FLinearColor White(1.f, 1.f, 1.f, 1.f);
	const FLinearColor Grey(0.55f, 0.55f, 0.55f, 1.f);
	const FLinearColor Dim(0.30f, 0.30f, 0.30f, 1.f);

	FString StripTokens(const FString& In)
	{
		// GXT formatting: ~n~ new line, other ~x~ codes are colours/buttons
		FString Out;
		for (int32 i = 0; i < In.Len(); ++i)
		{
			if (In[i] == TEXT('~'))
			{
				const int32 End = In.Find(TEXT("~"), ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 1);
				if (End > i)
				{
					const FString Tok = In.Mid(i + 1, End - i - 1);
					if (Tok == TEXT("n"))
					{
						Out += TEXT("\n");
					}
					else if (Tok.StartsWith(TEXT("PAD_")) || Tok.StartsWith(TEXT("INPUT_")))
					{
						Out += TEXT("Right Stick");
					}
					i = End;
					continue;
				}
			}
			Out.AppendChar(In[i]);
		}
		return Out;
	}

	bool AnyPressed(APlayerController* PC, std::initializer_list<FKey> Keys)
	{
		for (const FKey& K : Keys)
		{
			if (PC->WasInputKeyJustPressed(K))
			{
				return true;
			}
		}
		return false;
	}

	bool AnyDown(APlayerController* PC, std::initializer_list<FKey> Keys)
	{
		for (const FKey& K : Keys)
		{
			if (PC->IsInputKeyDown(K))
			{
				return true;
			}
		}
		return false;
	}
}

// ------------------------------------------------------------------ game mode

AGTAIVFrontendGameMode::AGTAIVFrontendGameMode()
{
	HUDClass = AGTAIVFrontendHUD::StaticClass();
	DefaultPawnClass = nullptr;
}

// ------------------------------------------------------------------ setup

AGTAIVFrontendHUD::AGTAIVFrontendHUD()
{
	static ConstructorHelpers::FObjectFinder<UGTAIVFrontendData> Asset(TEXT("/Game/GTAIV/Frontend/DA_GTAIV_Frontend.DA_GTAIV_Frontend"));
	if (Asset.Succeeded())
	{
		Data = Asset.Object;
	}
}

void AGTAIVFrontendHUD::BeginPlay()
{
	Super::BeginPlay();
	LastTime = FPlatformTime::Seconds();
	BuildFont();
	LoadSettings();
	ApplySettings();
	if (APlayerController* PC = GetOwningPlayerController())
	{
		PC->bShowMouseCursor = false;  // the game's own cursor is drawn
		PC->SetInputMode(FInputModeGameOnly());
	}
	if (Data)
	{
		for (int32 i = 0; i < Data->Screens.Num(); ++i)
		{
			if (Data->Screens[i].Flag == TEXT("INITIAL_MAIN"))
			{
				LoopStart = i;
				break;
			}
		}
		BgIndex = LoopStart;
		if (Data->Music)
		{
			MusicComp = UGameplayStatics::SpawnSound2D(this, Data->Music, Pref(TEXT("PREF_MUSIC_VOLUME"), 8) / 10.f);
		}
		if (Pref(TEXT("PREF_SKIP_INTRO"), 0) == 1)
		{
			State = EState::Title;
		}
	}
	TArray<FIntPoint> Res;
	UKismetSystemLibrary::GetSupportedFullscreenResolutions(Res);
	for (const FIntPoint& R : Res)
	{
		Resolutions.AddUnique(R);
	}
	if (Resolutions.Num() == 0)
	{
		Resolutions.Add(FIntPoint(1920, 1080));
	}
}

void AGTAIVFrontendHUD::EndPlay(const EEndPlayReason::Type Reason)
{
	if (MusicComp)
	{
		MusicComp->Stop();
	}
	Super::EndPlay(Reason);
}

void AGTAIVFrontendHUD::BuildFont()
{
	CharCell.Reset();
	if (!Data)
	{
		return;
	}
	for (int32 Cell = 0; Cell < Data->FontMap.Num(); ++Cell)
	{
		const bool bSub = Cell >= Data->FontMainEnd && Cell < Data->FontCommonStart;  // Pricedown digits
		const int32 Code = Data->FontMap[Cell];
		if (!bSub && Code != 32 && !CharCell.Contains(Code))
		{
			CharCell.Add(Code, Cell);
		}
	}
}

// ------------------------------------------------------------------ settings

int32 AGTAIVFrontendHUD::GetPreference(const FString& Pref, int32 Default)
{
	if (UGTAIVFrontendSettings* S = Cast<UGTAIVFrontendSettings>(UGameplayStatics::LoadGameFromSlot(SaveSlot, 0)))
	{
		if (const int32* V = S->Values.Find(Pref))
		{
			return *V;
		}
	}
	return Default;
}

void AGTAIVFrontendHUD::LoadSettings()
{
	Settings = Cast<UGTAIVFrontendSettings>(UGameplayStatics::LoadGameFromSlot(SaveSlot, 0));
	if (!Settings)
	{
		Settings = Cast<UGTAIVFrontendSettings>(UGameplayStatics::CreateSaveGameObject(UGTAIVFrontendSettings::StaticClass()));
	}
}

void AGTAIVFrontendHUD::SaveSettings()
{
	if (Settings)
	{
		UGameplayStatics::SaveGameToSlot(Settings, SaveSlot, 0);
	}
}

int32 AGTAIVFrontendHUD::Pref(const FString& Name, int32 Default) const
{
	if (Settings)
	{
		if (const int32* V = Settings->Values.Find(Name))
		{
			return *V;
		}
	}
	return Default;
}

void AGTAIVFrontendHUD::SetPref(const FString& Name, int32 V)
{
	if (Settings)
	{
		Settings->Values.Add(Name, V);
	}
}

int32 AGTAIVFrontendHUD::DefaultFor(const FGTAIVFeOption& O) const
{
	const FString& P = O.Value;
	UGameUserSettings* GUS = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	if (P == TEXT("PREF_VIDEO_MODE") && GUS)
	{
		const FIntPoint R = GUS->GetScreenResolution();
		const int32 I = Resolutions.IndexOfByKey(R);
		return I != INDEX_NONE ? I : Resolutions.Num() - 1;
	}
	if (P == TEXT("PREF_VSYNC") && GUS)
	{
		return GUS->IsVSyncEnabled() ? 1 : 0;
	}
	if (O.Display.Contains(TEXT("SLIDERBAR")))
	{
		return O.Scaler > 20 ? O.Scaler / 2 : FMath::Max(0, O.Scaler - 3);
	}
	if (O.Display.Contains(TEXT("QUALITY")) || O.Display.Contains(TEXT("SHADOW")))
	{
		return FMath::Clamp(ValueCount(O) - 2, 0, 3);
	}
	if (O.Display == TEXT("MENU_DISPLAY_ON_OFF"))
	{
		return P.Contains(TEXT("SKIP")) || P.Contains(TEXT("INVERT")) ? 0 : 1;
	}
	return 0;
}

int32 AGTAIVFrontendHUD::ValueCount(const FGTAIVFeOption& O) const
{
	if (O.Value == TEXT("PREF_VIDEO_MODE"))
	{
		return Resolutions.Num();
	}
	if (O.DisplayValues.Num() > 0)
	{
		return O.DisplayValues.Num();
	}
	return FMath::Max(O.Scaler, 0);
}

bool AGTAIVFrontendHUD::IsSlider(const FGTAIVFeOption& O) const
{
	return O.Display.Contains(TEXT("SLIDERBAR")) && O.Value != TEXT("PREF_VIDEO_MODE");
}

FString AGTAIVFrontendHUD::ValueText(const FGTAIVFeOption& O) const
{
	const int32 V = Pref(O.Value, DefaultFor(O));
	if (O.Value == TEXT("PREF_VIDEO_MODE"))
	{
		const FIntPoint R = Resolutions[FMath::Clamp(V, 0, Resolutions.Num() - 1)];
		return FString::Printf(TEXT("%d x %d"), R.X, R.Y);
	}
	if (O.Display.Contains(TEXT("VALUE_SLIDERBAR")))
	{
		return FString::Printf(TEXT("%d"), V);
	}
	if (O.DisplayValues.IsValidIndex(V))
	{
		return O.DisplayValues[V];
	}
	return FString();
}

bool AGTAIVFrontendHUD::Selectable(const FGTAIVFeOption& O) const
{
	if (O.Label.IsEmpty() || O.Label == TEXT("Option Locked") || O.Action == TEXT("MENUOPT_NONE"))
	{
		return false;
	}
	static const TSet<FString> Unsupported = {
		TEXT("MENUOPT_JUMP_GUIDE"), TEXT("MENUOPT_JUMP_MARKETPLACE"), TEXT("MENUOPT_BENCHMARK"), TEXT("LOAD_GAME"),
		TEXT("MENUOPT_QUICK_SCAN"), TEXT("MENUOPT_COMPLETE_SCAN")};
	if (Unsupported.Contains(O.Action) || O.Label == TEXT("Benchmark") || O.Label.Contains(TEXT("Scan")))
	{
		return false;
	}
	return true;
}

void AGTAIVFrontendHUD::ApplySettings()
{
	UGameUserSettings* GUS = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	if (!GUS || !Settings)
	{
		return;
	}
	auto Get = [this](const TCHAR* Name, int32 Def) { return Pref(Name, Def); };
	auto Quality = [](int32 V) { return FMath::Clamp(V, 0, 4); };
	if (Settings->Values.Contains(TEXT("PREF_VIDEO_MODE")) && Resolutions.Num())
	{
		GUS->SetScreenResolution(Resolutions[FMath::Clamp(Get(TEXT("PREF_VIDEO_MODE"), 0), 0, Resolutions.Num() - 1)]);
	}
	if (Settings->Values.Contains(TEXT("PREF_TEXTURE_QUALITY")))
	{
		GUS->SetTextureQuality(Quality(Get(TEXT("PREF_TEXTURE_QUALITY"), 3)));
	}
	if (Settings->Values.Contains(TEXT("PREF_SHADOW_QUALITY")))
	{
		GUS->SetShadowQuality(Quality(FMath::Max(0, Get(TEXT("PREF_SHADOW_QUALITY"), 3) - 1)));
	}
	if (Settings->Values.Contains(TEXT("PREF_REFLECTION_RESOLUTION")))
	{
		GUS->SetReflectionQuality(Quality(FMath::Max(0, Get(TEXT("PREF_REFLECTION_RESOLUTION"), 3) - 1)));
	}
	if (Settings->Values.Contains(TEXT("PREF_WATER_QUALITY")))
	{
		GUS->SetShadingQuality(Quality(Get(TEXT("PREF_WATER_QUALITY"), 3)));
	}
	if (Settings->Values.Contains(TEXT("PREF_VSYNC")))
	{
		GUS->SetVSyncEnabled(Get(TEXT("PREF_VSYNC"), 1) == 1);
	}
	GUS->ApplySettings(false);
	GUS->SaveSettings();

	auto CVar = [](const TCHAR* Name, int32 V) {
		if (IConsoleVariable* C = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			C->Set(V, ECVF_SetByGameSetting);
		}
	};
	if (Settings->Values.Contains(TEXT("PREF_TEXTURE_FILTER_QUALITY")))
	{
		CVar(TEXT("r.MaxAnisotropy"), 1 << FMath::Clamp(Get(TEXT("PREF_TEXTURE_FILTER_QUALITY"), 4), 0, 4));
	}
	FApp::SetVolumeMultiplier(Get(TEXT("PREF_SFX_VOLUME"), 8) / 10.f);
	if (MusicComp)
	{
		MusicComp->SetVolumeMultiplier(Get(TEXT("PREF_MUSIC_VOLUME"), 8) / 10.f);
	}
}

// ------------------------------------------------------------------ menus

const FGTAIVFeMenu* AGTAIVFrontendHUD::CurrentMenu() const
{
	if (!Data || MenuStack.Num() == 0)
	{
		return nullptr;
	}
	return Data->Menus.FindByPredicate([this](const FGTAIVFeMenu& M) { return M.Id == MenuStack.Last(); });
}

TArray<FGTAIVFeOption> AGTAIVFrontendHUD::CurrentItems() const
{
	if (State == EState::Title)
	{
		return Data ? Data->TitleItems : TArray<FGTAIVFeOption>();
	}
	const FGTAIVFeMenu* M = CurrentMenu();
	return M ? M->Options : TArray<FGTAIVFeOption>();
}

void AGTAIVFrontendHUD::MoveSelection(int32 Dir)
{
	const TArray<FGTAIVFeOption> Items = CurrentItems();
	for (int32 n = 0; n < Items.Num(); ++n)
	{
		Selected = (Selected + Dir + Items.Num()) % FMath::Max(1, Items.Num());
		if (Selectable(Items[Selected]))
		{
			return;
		}
	}
}

void AGTAIVFrontendHUD::Activate(int32 Index)
{
	const TArray<FGTAIVFeOption> Items = CurrentItems();
	if (!Items.IsValidIndex(Index) || !Selectable(Items[Index]))
	{
		return;
	}
	const FGTAIVFeOption& O = Items[Index];
	FString Target;
	if (O.Action == TEXT("NEW_GAME"))
	{
		EnterLoading();
		return;
	}
	if (O.Action == TEXT("QUIT"))
	{
		UKismetSystemLibrary::QuitGame(this, GetOwningPlayerController(), EQuitPreference::Quit, false);
		return;
	}
	if (O.Action == TEXT("MENU") || O.Action == TEXT("MENUOPT_JUMP_NEXT_MENU"))
	{
		Target = O.Value;
	}
	else if (O.Action == TEXT("MENUOPT_KEYBOARD_OPTIONS"))
	{
		Target = TEXT("SCR_KEYBOARD_OPTIONS");
	}
	else if (O.Action == TEXT("MENUOPT_CONTROLLER_OPTIONS"))
	{
		Target = TEXT("SCR_CONTROLLER_OPTIONS");
	}
	else if (O.Label == TEXT("Auto Configure"))
	{
		if (UGameUserSettings* GUS = GEngine->GetGameUserSettings())
		{
			GUS->RunHardwareBenchmark();
			GUS->ApplyHardwareBenchmarkResults();
		}
		return;
	}
	else if (O.Label == TEXT("Default"))
	{
		if (const FGTAIVFeMenu* M = CurrentMenu())
		{
			for (const FGTAIVFeOption& X : M->Options)
			{
				Settings->Values.Remove(X.Value);
			}
			SaveSettings();
			ApplySettings();
		}
		return;
	}
	else
	{
		Adjust(Index, 1);
		return;
	}
	if (Data && Data->Menus.ContainsByPredicate([&](const FGTAIVFeMenu& M) { return M.Id == Target; }))
	{
		MenuStack.Push(Target);
		State = EState::Menu;
		Selected = -1;
		MoveSelection(1);
	}
}

void AGTAIVFrontendHUD::Adjust(int32 Index, int32 Dir)
{
	const TArray<FGTAIVFeOption> Items = CurrentItems();
	if (State != EState::Menu || !Items.IsValidIndex(Index) || !Selectable(Items[Index]))
	{
		return;
	}
	const FGTAIVFeOption& O = Items[Index];
	const int32 N = ValueCount(O);
	if (N <= 0 || O.Value.IsEmpty() || O.Value == TEXT("PREF_NULL"))
	{
		return;
	}
	int32 V = Pref(O.Value, DefaultFor(O)) + Dir;
	V = IsSlider(O) || O.Display.Contains(TEXT("VALUE_SLIDERBAR")) ? FMath::Clamp(V, 0, N - 1) : (V + N) % N;
	SetPref(O.Value, V);
	SaveSettings();
	ApplySettings();
}

void AGTAIVFrontendHUD::Back()
{
	if (State == EState::Menu)
	{
		const FString Was = MenuStack.Pop();
		if (MenuStack.Num() == 0)
		{
			State = EState::Title;
		}
		const TArray<FGTAIVFeOption> Items = CurrentItems();
		Selected = FMath::Max(0, Items.IndexOfByPredicate([&](const FGTAIVFeOption& O) { return O.Value == Was; }));
		if (!Items.IsValidIndex(Selected) || !Selectable(Items[Selected]))
		{
			Selected = -1;
			MoveSelection(1);
		}
	}
}

void AGTAIVFrontendHUD::SkipIntro()
{
	if (State == EState::Intro)
	{
		EnterTitle();
	}
}

void AGTAIVFrontendHUD::EnterTitle()
{
	State = EState::Title;
	TitleSel = 0;
	MenuStack.Reset();
}

void AGTAIVFrontendHUD::EnterLoading()
{
	// Start: the loading screens play (with the theme) while the game "loads"
	State = EState::Starting;
	BgIndex = LoopStart;
	BgTime = 0.f;
	StartingFrames = 0.f;
	StartingSince = FPlatformTime::Seconds();
	bSkipLoading = false;
}

void AGTAIVFrontendHUD::ActivateTitle(int32 Index)
{
	switch (Index)
	{
	case 0:
		EnterLoading();
		break;
	case 1:
		OpenMenu(Data && Data->Menus.ContainsByPredicate([](const FGTAIVFeMenu& M) { return M.Id == TEXT("SCR_TOPTIONS"); })
					 ? TEXT("SCR_TOPTIONS") : TEXT("SCR_TGRAPHICS"));
		break;
	case 3:
		UKismetSystemLibrary::QuitGame(this, GetOwningPlayerController(), EQuitPreference::Quit, false);
		break;
	default:
		break;  // Social Club: not available here
	}
}

void AGTAIVFrontendHUD::StartGame()
{
	if (!Data || Data->StartLevel.IsEmpty())
	{
		return;
	}
	if (Data->StartMusic)
	{
		// the loading theme stops under the start sting, which carries on into the level
		if (MusicComp)
		{
			MusicComp->Stop();
		}
		UGameplayStatics::SpawnSound2D(this, Data->StartMusic, Pref(TEXT("PREF_MUSIC_VOLUME"), 8) / 10.f, 1.f, 0.f, nullptr, true);
	}
	State = EState::Intro;  // stops further loads while travelling
	ScreenIndex = Data->Screens.Num();
	// with the story exported, Start New Game is GTA IV's: the first mission from the opening cutscene
	const bool bStory = UGTAIVStoryData::Load() != nullptr;
	UGameplayStatics::OpenLevel(this, FName(*Data->StartLevel), true, bStory ? TEXT("story") : TEXT(""));
}

void AGTAIVFrontendHUD::OpenMenu(const FString& MenuId)
{
	SkipIntro();
	if (Data && Data->Menus.ContainsByPredicate([&](const FGTAIVFeMenu& M) { return M.Id == MenuId; }))
	{
		MenuStack.Reset();
		MenuStack.Push(MenuId);
		State = EState::Menu;
		Selected = -1;
		MoveSelection(1);
	}
}

// ------------------------------------------------------------------ frame

void AGTAIVFrontendHUD::Advance(float DtMs)
{
	if (!Data || Data->Screens.Num() == 0)
	{
		State = State == EState::Intro ? EState::Title : State;
		return;
	}
	if (State == EState::Intro)
	{
		ScreenTime += DtMs;
		while (ScreenIndex < Data->Screens.Num() && ScreenTime >= Data->Screens[ScreenIndex].Duration)
		{
			ScreenTime -= Data->Screens[ScreenIndex].Duration;
			++ScreenIndex;
			if (ScreenIndex >= LoopStart)  // legal screens and logos played: the title screen
			{
				EnterTitle();
				break;
			}
		}
	}
	if (State == EState::Starting)
	{
		BgTime += DtMs;
		if (BgTime >= Data->Screens[BgIndex].Duration)
		{
			BgTime = 0.f;
			BgIndex = BgIndex + 1 < Data->Screens.Num() ? BgIndex + 1 : LoopStart;  // loading screens loop
		}
	}
}

void AGTAIVFrontendHUD::HandleInput(float DtMs)
{
	APlayerController* PC = GetOwningPlayerController();
	if (!PC)
	{
		return;
	}
	const bool bAccept = AnyPressed(PC, {EKeys::Enter, EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom});
	const bool bBack = AnyPressed(PC, {EKeys::Escape, EKeys::BackSpace, EKeys::Gamepad_FaceButton_Right});
	if (State == EState::Intro)
	{
		if (bAccept || bBack || PC->WasInputKeyJustPressed(EKeys::LeftMouseButton))
		{
			// skip the logos, like the game; legal screens can't be skipped there either but a key press is fine
			EnterTitle();
		}
		return;
	}
	if (State == EState::Starting)
	{
		if ((bAccept || PC->WasInputKeyJustPressed(EKeys::LeftMouseButton)) && FPlatformTime::Seconds() - StartingSince > 1.5)
		{
			bSkipLoading = true;
		}
		return;
	}
	if (State == EState::Title)
	{
		// the bar at the bottom: left/right (or up/down) choose, enter or a click picks
		if (AnyPressed(PC, {EKeys::Left, EKeys::A, EKeys::Up, EKeys::W, EKeys::Gamepad_DPad_Left, EKeys::Gamepad_DPad_Up,
							EKeys::Gamepad_LeftStick_Left}))
		{
			TitleSel = (TitleSel + 3) % 4;
		}
		if (AnyPressed(PC, {EKeys::Right, EKeys::D, EKeys::Down, EKeys::S, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_DPad_Down,
							EKeys::Gamepad_LeftStick_Right}))
		{
			TitleSel = (TitleSel + 1) % 4;
		}
		float MX, MY;
		if (PC->GetMousePosition(MX, MY))
		{
			static FVector2D LastMouse(-1, -1);
			const FVector2D M(MX, MY);
			for (int32 i = 0; i < TitleRects.Num(); ++i)
			{
				if (TitleRects[i].IsInside(M))
				{
					if (M != LastMouse)
					{
						TitleSel = i;
					}
					if (PC->WasInputKeyJustPressed(EKeys::LeftMouseButton))
					{
						ActivateTitle(i);
						return;
					}
				}
			}
			LastMouse = M;
		}
		if (bAccept)
		{
			ActivateTitle(TitleSel);
		}
		return;
	}
	if (AnyPressed(PC, {EKeys::Up, EKeys::W, EKeys::Gamepad_DPad_Up, EKeys::Gamepad_LeftStick_Up}))
	{
		MoveSelection(-1);
	}
	if (AnyPressed(PC, {EKeys::Down, EKeys::S, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_LeftStick_Down}))
	{
		MoveSelection(1);
	}
	int32 Dir = 0;
	if (AnyPressed(PC, {EKeys::Left, EKeys::A, EKeys::Gamepad_DPad_Left, EKeys::Gamepad_LeftStick_Left}))
	{
		Dir = -1;
		RepeatTimer = -350.f;
	}
	else if (AnyPressed(PC, {EKeys::Right, EKeys::D, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_LeftStick_Right}))
	{
		Dir = 1;
		RepeatTimer = -350.f;
	}
	else if (AnyDown(PC, {EKeys::Left, EKeys::Right, EKeys::Gamepad_DPad_Left, EKeys::Gamepad_DPad_Right}))
	{
		RepeatTimer += DtMs;
		if (RepeatTimer > 60.f)
		{
			RepeatTimer = 0.f;
			Dir = AnyDown(PC, {EKeys::Left, EKeys::Gamepad_DPad_Left}) ? -1 : 1;
		}
	}
	if (Dir != 0)
	{
		Adjust(Selected, Dir);
	}
	// mouse: hover selects, click activates (or steps the value on its right/left half)
	float MX, MY;
	if (PC->GetMousePosition(MX, MY))
	{
		static FVector2D LastMouse(-1, -1);
		const FVector2D M(MX, MY);
		for (const FRow& R : Rows)
		{
			if (R.Box.IsInside(M))
			{
				if (M != LastMouse)
				{
					Selected = R.Index;
				}
				if (PC->WasInputKeyJustPressed(EKeys::LeftMouseButton))
				{
					const TArray<FGTAIVFeOption> Items = CurrentItems();
					if (State == EState::Menu && Items.IsValidIndex(R.Index) && ValueCount(Items[R.Index]) > 0
						&& Items[R.Index].Action.StartsWith(TEXT("MENUOPT_ADJUST")))
					{
						Adjust(R.Index, M.X < Area.Min.X + Area.GetSize().X * 0.68f ? -1 : 1);
					}
					else
					{
						Activate(R.Index);
					}
				}
			}
		}
		LastMouse = M;
	}
	if (PC->WasInputKeyJustPressed(EKeys::RightMouseButton))
	{
		Back();
	}
	if (bAccept)
	{
		Activate(Selected);
	}
	else if (bBack)
	{
		Back();
	}
}

void AGTAIVFrontendHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	const float DtMs = FMath::Clamp(float(Now - LastTime) * 1000.f, 0.f, 100.f);
	LastTime = Now;

	// the game's coordinates are for a 16:9 screen: fit one in the viewport (pillar/letterbox)
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	float AW = W, AH = W * 9.f / 16.f;
	if (AH > H)
	{
		AH = H;
		AW = H * 16.f / 9.f;
	}
	Area = FBox2D(FVector2D((W - AW) * 0.5f, (H - AH) * 0.5f), FVector2D((W + AW) * 0.5f, (H + AH) * 0.5f));
	FCanvasTileItem Black(FVector2D::ZeroVector, FVector2D(W, H), FLinearColor::Black);
	Canvas->DrawItem(Black);

	Advance(DtMs);
	HandleInput(DtMs);
	if (!Data)
	{
		DrawText2(TEXT("GTA IV front end data not found (DA_GTAIV_Frontend)"), FVector2D(0.5, 0.5), FVector2D(0.3, 0.45), White, 1);
		return;
	}

	if (State == EState::Intro && Data->Screens.IsValidIndex(ScreenIndex))
	{
		const FGTAIVFeScreen& S = Data->Screens[ScreenIndex];
		if (S.Flag == TEXT("LEGAL"))
		{
			DrawLegal(Data->LegalText1);
		}
		else if (S.Flag == TEXT("LEGAL2"))
		{
			DrawLegal(Data->LegalText2);
		}
		else
		{
			DrawScreen(S, ScreenTime, 1.f);
		}
		return;
	}

	if (State == EState::Starting)
	{
		// the loading screens, then the game (a key press after a moment skips ahead)
		if (Data->Screens.IsValidIndex(BgIndex))
		{
			DrawScreen(Data->Screens[BgIndex], BgTime, 1.f);
		}
		StartingFrames += 1.f;
		if (StartingFrames > 2.f && (bSkipLoading || FPlatformTime::Seconds() - StartingSince > LoadingSeconds))
		{
			StartGame();
		}
		return;
	}
	if (State == EState::Title)
	{
		DrawTitleScreen();
	}
	else
	{
		// option pages over the title art
		DrawTitleBackground();
		DrawMenu();
	}

	// cursor
	if (APlayerController* PC = GetOwningPlayerController())
	{
		float MX, MY;
		if (Data->Cursor && PC->GetMousePosition(MX, MY))
		{
			const float S = Area.GetSize().Y * 0.03f;
			FCanvasTileItem C(FVector2D(MX, MY), Data->Cursor->GetResource(), FVector2D(S, S), FLinearColor::White);
			C.BlendMode = SE_BLEND_Translucent;
			Canvas->DrawItem(C);
		}
	}
}

// ------------------------------------------------------------------ drawing

void AGTAIVFrontendHUD::DrawScreen(const FGTAIVFeScreen& S, float T, float Alpha)
{
	float A = Alpha;
	if (S.Fade == TEXT("IN_OUT"))
	{
		A *= FMath::Clamp(T / FadeMs, 0.f, 1.f) * FMath::Clamp((S.Duration - T) / FadeMs, 0.f, 1.f);
	}
	for (const FGTAIVFeObject& O : S.Objects)
	{
		if (!O.Texture || T < O.StartTime || T > O.EndTime)
		{
			continue;
		}
		const float Local = T - O.StartTime;
		const float Moves = FMath::FloorToFloat(Local / FMath::Max(1.f, O.MoveTime));
		const float Scales = FMath::FloorToFloat(Local / FMath::Max(1.f, O.ScaleTime));
		const FVector2D Pos = O.Pos + O.Move * Moves;
		const FVector2D Size = O.Size + O.Scale * Scales;
		float OA = A;
		if (S.Fade == TEXT("IN_OUT") && S.Objects.Num() == 1)  // logos fade with their own window
		{
			OA = Alpha * FMath::Clamp(Local / FadeMs, 0.f, 1.f) * FMath::Clamp((O.EndTime - T) / FadeMs, 0.f, 1.f);
		}
		const FVector2D PxSize = Size * Area.GetSize();
		FCanvasTileItem Item(ToPx(Pos) - PxSize * 0.5f, O.Texture->GetResource(), PxSize, FLinearColor(1, 1, 1, OA));
		Item.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Item);
	}
}

void AGTAIVFrontendHUD::DrawLegal(const FString& Text)
{
	const FVector2D Scale(0.20f, 0.30f);
	const TArray<FString> Lines = Wrap(StripTokens(Text), Scale, 0.80f);
	const float LineH = 0.032f;
	float Y = 0.5f - Lines.Num() * LineH * 0.5f;
	for (const FString& Line : Lines)
	{
		DrawText2(Line, FVector2D(0.5f, Y), Scale, White, 1);
		Y += LineH;
	}
}

void AGTAIVFrontendHUD::Rect(FVector2D Pos, FVector2D Size, FLinearColor Color)
{
	FCanvasTileItem Item(ToPx(Pos), Size * Area.GetSize(), Color);
	Item.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Item);
}

FVector2D AGTAIVFrontendHUD::L(const FString& Name, FVector2D Default) const
{
	if (Data)
	{
		if (const FVector2D* V = Data->Layout.Find(Name))
		{
			return *V;
		}
	}
	return Default;
}

void AGTAIVFrontendHUD::DrawMenu()
{
	Rows.Reset();
	const FGTAIVFeMenu* M = CurrentMenu();
	const FString Header = State == EState::Title ? Data->TitleHeader : (M ? M->Header : FString());
	const TArray<FGTAIVFeOption> Items = CurrentItems();

	// frame: lines and the translucent middle band (frontend_pc.dat)
	const float TopY = L(TEXT("TOP_position_of_top_line"), FVector2D(0.192, 0)).X;
	const float BotY = L(TEXT("BOT_position_of_bottom_line"), FVector2D(0.808, 0.778)).X;
	const float Opacity = L(TEXT("MID_background_opacity"), FVector2D(160, 0)).X / 255.f;
	Rect(FVector2D(0, 0), FVector2D(1, 1), FLinearColor(0, 0, 0, 0.35f));
	Rect(FVector2D(0, TopY), FVector2D(1, BotY - TopY), FLinearColor(0, 0, 0, Opacity));
	Rect(FVector2D(0.044f, TopY), FVector2D(0.912f, 0.0018f), FLinearColor(1, 1, 1, 0.85f));
	Rect(FVector2D(0.044f, BotY), FVector2D(0.912f, 0.0018f), FLinearColor(1, 1, 1, 0.85f));

	// header
	const FVector2D HPos = L(TEXT("TOP_top_text_position"), FVector2D(0.065, 0.112));
	const FVector2D HSize = L(TEXT("TOP_top_text_size"), FVector2D(0.585, 0.955));
	DrawText2(Header, FVector2D(HPos.X, HPos.Y - 0.035f), HSize, White, 0);

	// options
	const FVector2D TPos = L(TEXT("MID_middle_text_position"), FVector2D(0.088, 0.173));
	const FVector2D TSize = L(TEXT("MID_middle_text_size"), FVector2D(0.215, 0.3325));
	const FVector2D Gap = L(TEXT("MID_spacing_between_middle_text"), FVector2D(0.0215, 0.0215));
	const float LineH = TSize.Y * 40.f / 488.f + Gap.Y;
	const int32 MaxRows = FMath::Max(1, int32((BotY - TopY - 0.06f) / LineH));
	const int32 First = FMath::Clamp(Selected - MaxRows + 2, 0, FMath::Max(0, Items.Num() - MaxRows));
	const float ValueX = 0.60f;
	const FVector2D Bar = L(TEXT("MID_slider_bar_size"), FVector2D(0.159, 0.0105));
	float Y = TopY + 0.035f;
	for (int32 i = First; i < Items.Num() && i < First + MaxRows; ++i)
	{
		const FGTAIVFeOption& O = Items[i];
		const bool bSel = i == Selected;
		const bool bOk = Selectable(O);
		const FLinearColor C = !bOk ? Dim : (bSel ? White : Grey);
		if (bSel && bOk)
		{
			Rect(FVector2D(TPos.X - 0.012f, Y - 0.004f), FVector2D(0.85f, LineH - Gap.Y * 0.4f), FLinearColor(1, 1, 1, 0.08f));
		}
		DrawText2(StripTokens(O.Label), FVector2D(TPos.X, Y), TSize, C, 0);
		if (State == EState::Menu && ValueCount(O) > 0 && O.Action.StartsWith(TEXT("MENUOPT_ADJUST")))
		{
			const int32 N = ValueCount(O);
			const int32 V = Pref(O.Value, DefaultFor(O));
			if (IsSlider(O))
			{
				const float By = Y + LineH * 0.3f;
				Rect(FVector2D(ValueX, By), FVector2D(Bar.X, Bar.Y), FLinearColor(1, 1, 1, 0.25f));
				Rect(FVector2D(ValueX, By), FVector2D(Bar.X * float(V) / FMath::Max(1, N - 1), Bar.Y), C);
			}
			else
			{
				const FString VT = (bSel ? TEXT("<  ") : TEXT("")) + ValueText(O) + (bSel ? TEXT("  >") : TEXT(""));
				DrawText2(VT, FVector2D(ValueX + (bSel ? 0.f : TextWidth(TEXT("<  "), TSize)), Y), TSize, C, 0);
			}
		}
		const FVector2D P0 = ToPx(FVector2D(TPos.X - 0.012f, Y - 0.004f));
		const FVector2D P1 = ToPx(FVector2D(TPos.X + 0.84f, Y + LineH - 0.004f));
		if (bOk)
		{
			Rows.Add({FBox2D(P0, P1), i});
		}
		Y += LineH;
	}

	// help (bottom right)
	const FVector2D HelpPos = L(TEXT("HLP_helper_text_position"), FVector2D(0.91, 0.836));
	const FVector2D HelpSize = L(TEXT("HLP_help_text_size"), FVector2D(0.198, 0.3));
	const FString Help = State == EState::Title ? TEXT("Enter: Select") : TEXT("Enter: Select    Esc: Back    Left/Right: Change");
	DrawText2(Help, FVector2D(HelpPos.X, HelpPos.Y), HelpSize, Grey, 2);
}

// ------------------------------------------------------------------ text (GTA IV font1)

float AGTAIVFrontendHUD::TextWidth(const FString& Text, FVector2D Scale) const
{
	float W = 0.f, Best = 0.f;
	for (const TCHAR Ch : Text)
	{
		if (Ch == TEXT('\n'))
		{
			Best = FMath::Max(Best, W);
			W = 0.f;
			continue;
		}
		const int32* Cell = CharCell.Find(int32(Ch));
		const int32 Units = (Ch == TEXT(' ') || !Cell) ? (Data ? Data->FontWhitespace : 8)
			: 32 - (Data->FontProp.IsValidIndex(*Cell) ? Data->FontProp[*Cell] : 0);
		W += Units * Scale.X / 512.f;
	}
	return FMath::Max(Best, W);
}

TArray<FString> AGTAIVFrontendHUD::Wrap(const FString& Text, FVector2D Scale, float MaxWidth) const
{
	TArray<FString> Out;
	TArray<FString> Paras;
	Text.ParseIntoArray(Paras, TEXT("\n"), false);
	for (const FString& P : Paras)
	{
		TArray<FString> Words;
		P.ParseIntoArray(Words, TEXT(" "), true);
		FString Line;
		for (const FString& Wd : Words)
		{
			const FString Try = Line.IsEmpty() ? Wd : Line + TEXT(" ") + Wd;
			if (!Line.IsEmpty() && TextWidth(Try, Scale) > MaxWidth)
			{
				Out.Add(Line);
				Line = Wd;
			}
			else
			{
				Line = Try;
			}
		}
		Out.Add(Line);
	}
	return Out;
}

void AGTAIVFrontendHUD::DrawText2(const FString& Text, FVector2D Pos, FVector2D Scale, FLinearColor Color, int32 Align)
{
	if (!Data || !Data->FontTexture || !Canvas)
	{
		return;
	}
	// font1 cells are 64x80 texels (16 per row, 12.8 rows): 32x40 units in fonts.dat's 512x488 space
	const FVector2D CellN(Scale.X * 32.f / 512.f, Scale.Y * 40.f / 488.f);
	const FVector2D CellPx = CellN * Area.GetSize();
	float X = Pos.X;
	if (Align == 1)
	{
		X -= TextWidth(Text, Scale) * 0.5f;
	}
	else if (Align == 2)
	{
		X -= TextWidth(Text, Scale);
	}
	const FTextureResource* Res = Data->FontTexture->GetResource();
	for (const TCHAR Ch : Text)
	{
		const int32* Cell = CharCell.Find(int32(Ch));
		if (Ch == TEXT(' ') || !Cell)
		{
			X += Data->FontWhitespace * Scale.X / 512.f;
			continue;
		}
		const int32 C = *Cell;
		const float V0 = (C / 16) * 80.f / 1024.f, V1 = V0 + 80.f / 1024.f;
		FCanvasTileItem Item(ToPx(FVector2D(X, Pos.Y)), Res, CellPx, FVector2D((C % 16) / 16.f, V0),
							 FVector2D((C % 16 + 1) / 16.f, V1), Color);
		Item.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Item);
		X += (32 - (Data->FontProp.IsValidIndex(C) ? Data->FontProp[C] : 0)) * Scale.X / 512.f;
	}
}


// ------------------------------------------------------------------ title screen (PC, Complete Edition)

void AGTAIVFrontendHUD::DrawTitleBackground()
{
	if (!Data || !Canvas)
	{
		return;
	}
	// the art comes per aspect ratio, squeezed into a square texture: stretch it over the screen
	const float W = Canvas->ClipX, H = Canvas->ClipY, A = W / FMath::Max(1.f, H);
	const TCHAR* Order[] = {A > 2.05f ? TEXT("21by9") : A > 1.7f ? TEXT("w") : A > 1.55f ? TEXT("16by10") : A > 1.29f ? TEXT("4by3") : TEXT("5by4"),
							TEXT("w"), TEXT("21by9"), TEXT("16by10")};
	for (const TCHAR* Key : Order)
	{
		if (const TObjectPtr<UTexture2D>* T = Data->TitleBackgrounds.Find(Key))
		{
			if (*T)
			{
				FCanvasTileItem Item(FVector2D::ZeroVector, (*T)->GetResource(), FVector2D(W, H), FLinearColor::White);
				Item.BlendMode = SE_BLEND_Opaque;
				Canvas->DrawItem(Item);
				return;
			}
		}
	}
	// no title art exported: the first loading screen instead
	if (Data->Screens.IsValidIndex(LoopStart))
	{
		DrawScreen(Data->Screens[LoopStart], 1000.f, 1.f);
	}
}

void AGTAIVFrontendHUD::DrawTitleScreen()
{
	DrawTitleBackground();
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	// the black bar along the bottom with the choices
	const float BarY = H * 0.894f;
	FCanvasTileItem Bar(FVector2D(0, BarY), FVector2D(W, H - BarY), FLinearColor(0.01f, 0.01f, 0.01f, 1.f));
	Bar.BlendMode = SE_BLEND_Opaque;
	Canvas->DrawItem(Bar);
	static const TCHAR* Defaults[] = {TEXT("Start"), TEXT("Options"), TEXT("Social Club"), TEXT("Back")};
	const float CellH = H * 0.066f;
	const float Y = BarY + (H - BarY) * 0.5f - CellH * 0.5f;
	float X = W * 0.015f;
	TitleRects.Reset();
	for (int32 i = 0; i < 4; ++i)
	{
		const FString Label = Data->TitleBar.IsValidIndex(i) && !Data->TitleBar[i].IsEmpty() ? Data->TitleBar[i] : FString(Defaults[i]);
		const bool bSel = i == TitleSel;
		DrawTextPx(Label, FVector2D(X, Y), CellH, bSel ? FLinearColor(0.95f, 0.62f, 0.10f, 1.f) : FLinearColor(0.84f, 0.84f, 0.84f, 1.f));
		const float Wd = TextWidthPx(Label, CellH);
		TitleRects.Add(FBox2D(FVector2D(X - 4.f, BarY), FVector2D(X + Wd + 4.f, H)));
		X += Wd + W * 0.012f;
	}
}

float AGTAIVFrontendHUD::TextWidthPx(const FString& Text, float CellH) const
{
	const float CellW = CellH * 0.8f;  // 64x80 cells
	float X = 0.f;
	for (const TCHAR Ch : Text)
	{
		const int32* Cell = CharCell.Find(int32(Ch));
		X += (Ch == TEXT(' ') || !Cell) ? CellW * (Data ? Data->FontWhitespace : 8) / 32.f
			: CellW * (32 - (Data->FontProp.IsValidIndex(*Cell) ? Data->FontProp[*Cell] : 0)) / 32.f;
	}
	return X;
}

void AGTAIVFrontendHUD::DrawTextPx(const FString& Text, FVector2D TopLeft, float CellH, FLinearColor Color)
{
	if (!Data || !Data->FontTexture)
	{
		return;
	}
	const float CellW = CellH * 0.8f;
	const FTextureResource* Res = Data->FontTexture->GetResource();
	float X = TopLeft.X;
	for (const TCHAR Ch : Text)
	{
		const int32* Cell = CharCell.Find(int32(Ch));
		if (Ch == TEXT(' ') || !Cell)
		{
			X += CellW * Data->FontWhitespace / 32.f;
			continue;
		}
		const int32 C = *Cell;
		const float V0 = (C / 16) * 80.f / 1024.f;
		FCanvasTileItem Item(FVector2D(X, TopLeft.Y), Res, FVector2D(CellW, CellH), FVector2D((C % 16) / 16.f, V0),
							 FVector2D((C % 16 + 1) / 16.f, V0 + 80.f / 1024.f), Color);
		Item.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Item);
		X += CellW * (32 - (Data->FontProp.IsValidIndex(C) ? Data->FontProp[C] : 0)) / 32.f;
	}
}
