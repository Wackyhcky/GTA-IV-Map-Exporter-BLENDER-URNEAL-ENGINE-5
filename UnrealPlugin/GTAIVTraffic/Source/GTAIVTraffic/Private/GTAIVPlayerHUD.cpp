#include "GTAIVPlayerHUD.h"
#include "GTAIVRadio.h"
#include "GTAIVSessions.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Engine/Texture2D.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GTAIVFrontend.h"
#include "GTAIVPlayerComponent.h"
#include "GTAIVVehicle.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/GameModeBase.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TextureResource.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UnrealType.h"

namespace
{
	const int32 StarChar = 0xE001;   // private code for the star glyph
	const float RingInner = 0.86f;   // radarringback: the map disc is 86% of the ring
	const float FadeSeconds = 0.5f;
}

AGTAIVPlayerHUD::AGTAIVPlayerHUD()
{
	static ConstructorHelpers::FObjectFinder<UGTAIVHudData> Asset(TEXT("/Game/GTAIV/HUD/DA_GTAIV_Hud.DA_GTAIV_Hud"));
	if (Asset.Succeeded())
	{
		Data = Asset.Object;
	}
}

void AGTAIVPlayerHUD::BeginPlay()
{
	Super::BeginPlay();
	if (bRespawnFade)
	{
		bRespawnFade = false;
		RespawnFadeStart = FPlatformTime::Seconds();
	}
	BuildFont();
	if (Data && Data->RadarMaterial)
	{
		RadarMID = UMaterialInstanceDynamic::Create(Data->RadarMaterial, this);
		RadarMID->SetTextureParameterValue(TEXT("MapTex"), Data->RadarMap);
	}
	if (Data && Data->RingMaterial)
	{
		RingMID = UMaterialInstanceDynamic::Create(Data->RingMaterial, this);
	}
}

void AGTAIVPlayerHUD::BuildFont()
{
	MainCells.Reset();
	SubCells.Reset();
	const UGTAIVFrontendData* F = Data ? Data->Font.Get() : nullptr;
	if (!F)
	{
		return;
	}
	for (int32 Cell = 0; Cell < F->FontMap.Num(); ++Cell)
	{
		const int32 Code = F->FontMap[Cell];
		const bool bSub = Cell >= F->FontMainEnd && Cell < F->FontCommonStart;  // Pricedown digits
		TMap<int32, int32>& M = bSub ? SubCells : MainCells;
		if (Code != 32 && !M.Contains(Code))
		{
			M.Add(Code, Cell);
		}
		if (Code == 252 && Cell > 0 && F->FontMap[Cell - 1] == 251 && Cell >= F->FontCommonStart)
		{
			MainCells.Add(StarChar, Cell);   // the wanted star
		}
	}
}

// ------------------------------------------------------------------ API

static FString GxtTokens(const FString& In)
{
	static const TMap<FString, FString> Keys = {
		{TEXT("INPUT_PICKUP"), TEXT("E")}, {TEXT("INPUT_ENTER"), TEXT("F")}, {TEXT("INPUT_ACCEPT"), TEXT("Enter")},
		{TEXT("INPUT_JUMP"), TEXT("Space")}, {TEXT("INPUT_SPRINT"), TEXT("Shift")}, {TEXT("INPUT_PHONE"), TEXT("Up")},
		{TEXT("INPUT_ATTACK"), TEXT("Left Mouse")}, {TEXT("INPUT_AIM"), TEXT("Right Mouse")}};
	FString Out;
	for (int32 i = 0; i < In.Len(); ++i)
	{
		if (In[i] == TEXT('~'))
		{
			const int32 End = In.Find(TEXT("~"), ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 1);
			if (End > i)
			{
				const FString Tok = In.Mid(i + 1, End - i - 1);
				if (const FString* K = Keys.Find(Tok))
				{
					Out += *K;
				}
				else if (Tok.StartsWith(TEXT("INPUT_")) || Tok.StartsWith(TEXT("PAD_")))
				{
					Out += Tok.RightChop(Tok.Find(TEXT("_")) + 1).Replace(TEXT("_"), TEXT(" "));
				}
				i = End;
				continue;
			}
		}
		Out.AppendChar(In[i]);
	}
	return Out;
}

void AGTAIVPlayerHUD::ShowHelpText(const FString& InText, float Seconds)
{
	HelpText = GxtTokens(InText);
	HelpUntil = GetWorld()->GetRealTimeSeconds() + Seconds;
}

void AGTAIVPlayerHUD::ShowSubtitle(const FString& InText, float Seconds)
{
	Subtitle = InText;   // drawn with its colour tokens
	SubtitleUntil = GetWorld()->GetRealTimeSeconds() + Seconds;
}

void AGTAIVPlayerHUD::SetWaypoint(FVector Location)
{
	bWaypoint = true;
	Waypoint = Location;
}

void AGTAIVPlayerHUD::ClearWaypoint()
{
	bWaypoint = false;
}

void AGTAIVPlayerHUD::SetBlip(FName Id, FVector Location, const FString& Sprite)
{
	Blips.Add(Id, {Location, Sprite});
}

void AGTAIVPlayerHUD::SetObjectiveBlip(FName Id, FVector Location, FLinearColor Colour)
{
	FBlip B;
	B.Location = Location;
	B.Sprite = TEXT("radar_objective");
	B.Colour = Colour;
	B.bKeep = true;
	Blips.Add(Id, B);
}

void AGTAIVPlayerHUD::SetRoute(const TArray<FVector>& Points, FLinearColor Colour)
{
	Route = Points;
	RouteColour = Colour;
}

void AGTAIVPlayerHUD::ShowBigMessage(const FString& InText, float Seconds, FLinearColor Colour)
{
	BigMessage = InText;
	BigColour = Colour;
	BigSince = GetWorld()->GetRealTimeSeconds();
	BigUntil = BigSince + Seconds;
}

void AGTAIVPlayerHUD::DrawColouredLine(const FString& Raw, FVector2D Pos, FVector2D Scale, int32 Align)
{
	// GTA's colour tokens: ~b~ blue, ~y~ yellow, ~r~ red, ~g~ green, ~s~ / ~w~ back to white
	struct FSeg { FString Text; FLinearColor Colour; };
	TArray<FSeg> Segs;
	FLinearColor Cur = Col(TEXT("HUD_COLOUR_WHITE"));
	FString Acc;
	for (int32 i = 0; i < Raw.Len(); ++i)
	{
		if (Raw[i] == TEXT('~'))
		{
			const int32 End = Raw.Find(TEXT("~"), ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 1);
			if (End > i)
			{
				const FString Tok = Raw.Mid(i + 1, End - i - 1).ToLower();
				FLinearColor Next = Cur;
				bool bColour = true;
				if (Tok == TEXT("b")) Next = Col(TEXT("HUD_COLOUR_BLUE"));
				else if (Tok == TEXT("y")) Next = Col(TEXT("HUD_COLOUR_YELLOW"));
				else if (Tok == TEXT("r")) Next = Col(TEXT("HUD_COLOUR_RED"));
				else if (Tok == TEXT("g")) Next = Col(TEXT("HUD_COLOUR_GREEN"));
				else if (Tok == TEXT("s") || Tok == TEXT("w") || Tok == TEXT("z")) Next = Col(TEXT("HUD_COLOUR_WHITE"));
				else bColour = false;
				if (bColour)
				{
					if (!Acc.IsEmpty())
					{
						Segs.Add({Acc, Cur});
						Acc.Reset();
					}
					Cur = Next;
				}
				else
				{
					Acc += GxtTokens(Raw.Mid(i, End - i + 1));
				}
				i = End;
				continue;
			}
		}
		Acc += Raw[i];
	}
	if (!Acc.IsEmpty())
	{
		Segs.Add({Acc, Cur});
	}
	float Total = 0.f;
	for (const FSeg& S : Segs)
	{
		Total += TextWidth(S.Text, Scale, false);
	}
	float X = Pos.X - (Align == 1 ? Total * 0.5f : Align == 2 ? Total : 0.f);
	for (const FSeg& S : Segs)
	{
		Text(S.Text, FVector2D(X, Pos.Y), Scale, S.Colour, 0);
		X += TextWidth(S.Text, Scale, false);
	}
}

void AGTAIVPlayerHUD::DrawStoryOverlay(double Now)
{
	if (bCutsceneMode)
	{
		// GTA IV's cutscenes are letterboxed
		const float Bar = Canvas->ClipY * 0.11f;
		Box(FVector2D::ZeroVector, FVector2D(Canvas->ClipX, Bar), FLinearColor::Black);
		Box(FVector2D(0.f, Canvas->ClipY - Bar), FVector2D(Canvas->ClipX, Bar), FLinearColor::Black);
	}
	if (Now < BigUntil && !BigMessage.IsEmpty())
	{
		const float Age = float(Now - BigSince), Left = float(BigUntil - Now);
		FLinearColor C = BigColour;
		C.A = FMath::Clamp(Age / 0.3f, 0.f, 1.f) * FMath::Clamp(Left / 0.6f, 0.f, 1.f);
		Text(BigMessage, FVector2D(0.5f, 0.38f), FVector2D(0.62f, 0.9f), C, 1, false, true);
	}
	if (FadeAlpha > 0.f)
	{
		Box(FVector2D::ZeroVector, FVector2D(Canvas->ClipX, Canvas->ClipY), FLinearColor(0, 0, 0, FMath::Clamp(FadeAlpha, 0.f, 1.f)));
	}
}

void AGTAIVPlayerHUD::RemoveBlip(FName Id)
{
	Blips.Remove(Id);
}

FString AGTAIVPlayerHUD::GetAreaName(FVector Location) const
{
	if (!Data)
	{
		return FString();
	}
	const FVector2D P(Location.X / 100.0, -Location.Y / 100.0);  // Unreal -> GTA IV metres
	const FGTAIVZone* Best = nullptr;
	double BestArea = TNumericLimits<double>::Max();
	for (const FGTAIVZone& Z : Data->Zones)
	{
		if (P.X >= Z.Min.X && P.X <= Z.Max.X && P.Y >= Z.Min.Y && P.Y <= Z.Max.Y)
		{
			const double A = (Z.Max.X - Z.Min.X) * (Z.Max.Y - Z.Min.Y);
			if (A < BestArea)
			{
				BestArea = A;
				Best = &Z;
			}
		}
	}
	return Best ? Best->Name : FString();
}

// ------------------------------------------------------------------ helpers

const FGTAIVHudElement* AGTAIVPlayerHUD::El(const TCHAR* Name) const
{
	return Data ? Data->Layout.Find(Name) : nullptr;
}

FLinearColor AGTAIVPlayerHUD::Col(const TCHAR* Name, float Alpha) const
{
	FLinearColor C = FLinearColor::White;
	if (Data)
	{
		if (const FLinearColor* Found = Data->Colours.Find(Name))
		{
			C = *Found;
		}
	}
	C.A = Alpha;
	return C;
}

void AGTAIVPlayerHUD::Tex(const FString& Name, FVector2D CentrePx, FVector2D SizePx, FLinearColor Color, float Rotation)
{
	const TObjectPtr<UTexture2D>* T = Data ? Data->Textures.Find(Name) : nullptr;
	if (!T || !*T)
	{
		return;
	}
	FCanvasTileItem Item(CentrePx - SizePx * 0.5f, (*T)->GetResource(), SizePx, Color);
	Item.BlendMode = SE_BLEND_Translucent;
	Item.Rotation = FRotator(0.f, Rotation, 0.f);
	Item.PivotPoint = FVector2D(0.5f, 0.5f);
	Canvas->DrawItem(Item);
}

float AGTAIVPlayerHUD::TextWidth(const FString& Str, FVector2D Scale, bool bSub) const
{
	const UGTAIVFrontendData* F = Data ? Data->Font.Get() : nullptr;
	if (!F)
	{
		return 0.f;
	}
	float W = 0.f;
	for (const TCHAR Ch : Str)
	{
		const int32* Cell = (bSub ? SubCells : MainCells).Find(int32(Ch));
		if (!Cell && bSub)
		{
			Cell = MainCells.Find(int32(Ch));
		}
		const int32 Units = (Ch == TEXT(' ') || !Cell) ? F->FontWhitespace : 32 - (F->FontProp.IsValidIndex(*Cell) ? F->FontProp[*Cell] : 0);
		W += Units * Scale.X / 512.f;
	}
	return W;
}

void AGTAIVPlayerHUD::Text(const FString& Str, FVector2D Pos, FVector2D Scale, FLinearColor Color, int32 Align, bool bSub, bool bShadow)
{
	const UGTAIVFrontendData* F = Data ? Data->Font.Get() : nullptr;
	if (!F || !F->FontTexture || Str.IsEmpty())
	{
		return;
	}
	if (bShadow)
	{
		const FVector2D Off(0.0012f, 0.0018f);
		Text(Str, Pos + Off, Scale, FLinearColor(0, 0, 0, Color.A * 0.8f), Align, bSub, false);
	}
	const FVector2D CellPx = FVector2D(Scale.X * 32.f / 512.f, Scale.Y * 40.f / 488.f) * Area.GetSize();
	float X = Pos.X - (Align == 1 ? TextWidth(Str, Scale, bSub) * 0.5f : Align == 2 ? TextWidth(Str, Scale, bSub) : 0.f);
	const FTextureResource* Res = F->FontTexture->GetResource();
	for (const TCHAR Ch : Str)
	{
		const int32* Cell = (bSub ? SubCells : MainCells).Find(int32(Ch));
		if (!Cell && bSub)
		{
			Cell = MainCells.Find(int32(Ch));
		}
		if (Ch == TEXT(' ') || !Cell)
		{
			X += F->FontWhitespace * Scale.X / 512.f;
			continue;
		}
		const int32 C = *Cell;
		const float V0 = (C / 16) * 80.f / 1024.f;
		FCanvasTileItem Item(ToPx(FVector2D(X, Pos.Y)), Res, CellPx, FVector2D((C % 16) / 16.f, V0),
							 FVector2D((C % 16 + 1) / 16.f, V0 + 80.f / 1024.f), Color);
		Item.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Item);
		X += (32 - (F->FontProp.IsValidIndex(C) ? F->FontProp[C] : 0)) * Scale.X / 512.f;
	}
}

// ------------------------------------------------------------------ frame

void AGTAIVPlayerHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas || !Data || !bShowGTAHud)
	{
		return;
	}
	// hud.dat positions are fractions of the whole screen
	Area = FBox2D(FVector2D::ZeroVector, FVector2D(Canvas->ClipX, Canvas->ClipY));
	const double Now = GetWorld()->GetRealTimeSeconds();

	APlayerController* PC = GetOwningPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	FVector Loc = Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
	float Speed = Pawn ? Pawn->GetVelocity().Size2D() : 0.f;
	if (Pawn)
	{
		// a Health property on the pawn (0..MaxHealth) drives the ring when there is one
		if (const FFloatProperty* HP = FindFProperty<FFloatProperty>(Pawn->GetClass(), TEXT("Health")))
		{
			float MaxH = 100.f;
			if (const FFloatProperty* MP = FindFProperty<FFloatProperty>(Pawn->GetClass(), TEXT("MaxHealth")))
			{
				MaxH = FMath::Max(1.f, MP->GetPropertyValue_InContainer(Pawn));
			}
			Health = FMath::Clamp(HP->GetPropertyValue_InContainer(Pawn) / MaxH, 0.f, 1.f);
		}
	}
	// the GTA IV player component's weapon: icon, ammo (clip-rest) and the crosshair while aiming
	bool bCrosshair = false;
	bool bWastedNow = false;
	// in a car: the driver's weapon and health
	APawn* Person = Pawn;
	if (const AGTAIVVehicle* Car = Cast<AGTAIVVehicle>(Pawn))
	{
		Person = Car->GetDriver();
	}
	if (const UGTAIVPlayerComponent* G = Person ? Person->FindComponentByClass<UGTAIVPlayerComponent>() : nullptr)
	{
		if (G->IsArmed() && G->Weapon)
		{
			WeaponIconTexture = G->Weapon->Icon;
			AmmoText = FString::Printf(TEXT("%d-%d"), G->GetClipAmmo(), G->GetSpareAmmo());
		}
		else
		{
			WeaponIconTexture = nullptr;
			AmmoText.Reset();
		}
		bCrosshair = G->IsAiming();
		Health = FMath::Clamp(G->Health / 100.f, 0.f, 1.f);
		bWastedNow = G->IsWasted();
	}
	if (bWastedNow || bDeath)
	{
		UpdateDeath(FPlatformTime::Seconds());  // the rest of the HUD is hidden while wasted
		return;
	}
	if (bCutsceneMode)
	{
		DrawStoryOverlay(Now);
		DrawMessages(Now);   // the subtitles (between the bars)
		return;
	}
	FRotator CamRot = PC ? PC->GetControlRotation() : FRotator::ZeroRotator;
	if (PC && PC->PlayerCameraManager)
	{
		CamRot = PC->PlayerCameraManager->GetCameraRotation();
	}

	// area name when entering a new zone
	const FString AreaNow = GetAreaName(Loc);
	if (AreaNow != CurrentArea)
	{
		CurrentArea = AreaNow;
		if (!AreaNow.IsEmpty())
		{
			AreaShown = AreaNow;
			AreaSince = Now;
		}
	}

	DrawRadar(Loc, CamRot.Yaw, Speed);
	DrawTopRight();
	DrawMessages(Now);
	DrawRadio();
	DrawNameTags();
	DrawPlayerList(Now);
	PhoneInput();
	DrawPhone();
	if (bCrosshair)
	{
		// hud_crosshair is a quarter of the ring (centre at its bottom-right): drawn four times
		const TObjectPtr<UTexture2D>* T = Data->Textures.Find(TEXT("hud_crosshair"));
		if (T && *T)
		{
			const float S = Area.GetSize().Y * 0.022f;
			const FVector2D C = Area.GetCenter();
			for (int32 q = 0; q < 4; ++q)
			{
				FCanvasTileItem Item(C - FVector2D(S, S), (*T)->GetResource(), FVector2D(S, S), FLinearColor(1, 1, 1, 0.9f));
				Item.BlendMode = SE_BLEND_Translucent;
				Item.Rotation = FRotator(0.f, 90.f * q, 0.f);
				Item.PivotPoint = FVector2D(1.f, 1.f);
				Canvas->DrawItem(Item);
			}
		}
	}
	// back from being wasted: fade in from black
	const double SinceRespawn = FPlatformTime::Seconds() - RespawnFadeStart;
	if (SinceRespawn < 2.4)
	{
		const float A = SinceRespawn < 0.6 ? 1.f : 1.f - float(SinceRespawn - 0.6) / 1.8f;
		FCanvasTileItem Black(FVector2D::ZeroVector, FVector2D(Canvas->ClipX, Canvas->ClipY), FLinearColor(0, 0, 0, A));
		Black.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Black);
	}
	DrawStoryOverlay(Now);   // mission passed / failed, the story's fades
}

void AGTAIVPlayerHUD::DrawRadar(const FVector& PlayerLoc, float CameraYaw, float Speed)
{
	const FGTAIVHudElement* R = El(TEXT("HUD_RADAR"));
	const FVector2D TopLeft = ToPx(R ? R->Pos : FVector2D(0.064, 0.745));
	const float D = (R ? R->Size.Y : 0.211f) * Area.GetSize().Y;   // round: sized by height
	const FVector2D Centre = TopLeft + FVector2D(D, D) * 0.5f;
	const float MapD = D * RingInner;
	const float Range = FMath::Lerp(RadarRangeOnFoot, RadarRangeFast, FMath::Clamp(Speed / 3000.f, 0.f, 1.f));
	const FVector2D Fwd(FMath::Cos(FMath::DegreesToRadians(CameraYaw)), FMath::Sin(FMath::DegreesToRadians(CameraYaw)));
	const FVector2D Right(-Fwd.Y, Fwd.X);

	Tex(TEXT("radarringback"), Centre, FVector2D(D, D), FLinearColor(1, 1, 1, 0.9f));
	if (RadarMID)
	{
		const FVector2D MapSize(Data->RadarCols * Data->RadarTile, Data->RadarRows * Data->RadarTile);
		const FVector2D G(PlayerLoc.X / 100.0, -PlayerLoc.Y / 100.0);
		const FVector2D UV((G.X - Data->RadarOrigin.X) / MapSize.X, (Data->RadarOrigin.Y - G.Y) / MapSize.Y);
		RadarMID->SetVectorParameterValue(TEXT("Center"), FLinearColor(UV.X, UV.Y, 0, 0));
		RadarMID->SetVectorParameterValue(TEXT("Basis"), FLinearColor(Fwd.X, Fwd.Y, Right.X, Right.Y));
		RadarMID->SetVectorParameterValue(TEXT("Scale"), FLinearColor(2.f * Range / MapSize.X, 2.f * Range / MapSize.Y, 0, 0));
		Canvas->K2_DrawMaterial(RadarMID, Centre - FVector2D(MapD, MapD) * 0.5f, FVector2D(MapD, MapD), FVector2D(0, 0), FVector2D(1, 1));
	}
	// health (left half) and armour (right half) in the ring band
	DisplayedHealth = FMath::FInterpTo(DisplayedHealth, Health, GetWorld()->GetDeltaSeconds(), 6.f);
	DisplayedArmour = FMath::FInterpTo(DisplayedArmour, Armour, GetWorld()->GetDeltaSeconds(), 6.f);
	if (RingMID)
	{
		const bool bLow = DisplayedHealth < 0.2f && FMath::Fmod(GetWorld()->GetRealTimeSeconds(), 0.6) < 0.3;
		RingMID->SetScalarParameterValue(TEXT("Health"), DisplayedHealth);
		RingMID->SetScalarParameterValue(TEXT("Armour"), DisplayedArmour);
		RingMID->SetVectorParameterValue(TEXT("HealthColour"), bLow ? Col(TEXT("HUD_COLOUR_RED")) : Col(TEXT("HUD_COLOUR_GREEN")));
		RingMID->SetVectorParameterValue(TEXT("ArmourColour"), Col(TEXT("HUD_COLOUR_BLUE")));
		Canvas->K2_DrawMaterial(RingMID, Centre - FVector2D(D, D) * 0.5f, FVector2D(D, D), FVector2D(0, 0), FVector2D(1, 1));
	}
	Tex(TEXT("radarringfront"), Centre, FVector2D(D, D), FLinearColor::White);

	// world offset (Unreal cm) -> radar pixels; Keep = stay on the rim when outside
	auto Project = [&](const FVector& W, bool bKeep, FVector2D& Out) {
		const FVector2D Dlt((W.X - PlayerLoc.X) / 100.0, (W.Y - PlayerLoc.Y) / 100.0);
		FVector2D P(FVector2D::DotProduct(Dlt, Right), -FVector2D::DotProduct(Dlt, Fwd));
		P /= (2.f * Range);                           // -0.5..0.5 across the map disc
		const float Len = P.Size();
		if (Len > 0.47f)
		{
			if (!bKeep)
			{
				return false;
			}
			P *= 0.47f / Len;
		}
		Out = Centre + P * MapD;
		return true;
	};
	const float BlipPx = D * 0.12f;
	if (Route.Num() > 1)
	{
		// the GPS: the route as a line on the radar disc
		FVector2D Prev;
		bool bPrev = Project(Route[0], false, Prev);
		for (int32 i = 1; i < Route.Num(); ++i)
		{
			FVector2D Cur;
			const bool bCur = Project(Route[i], false, Cur);
			if (bPrev && bCur)
			{
				FCanvasLineItem L(Prev, Cur);
				L.SetColor(RouteColour);
				L.LineThickness = FMath::Max(2.f, D * 0.018f);
				Canvas->DrawItem(L);
			}
			Prev = Cur;
			bPrev = bCur;
		}
	}
	for (const auto& KV : Blips)
	{
		FVector2D P;
		if (Project(KV.Value.Location, KV.Value.bKeep, P))
		{
			Tex(KV.Value.Sprite, P, FVector2D(BlipPx, BlipPx), KV.Value.Colour);
		}
	}
	FVector2D P;
	if (bWaypoint && Project(Waypoint, true, P))
	{
		Tex(TEXT("radar_waypoint"), P, FVector2D(BlipPx, BlipPx), FLinearColor::White);
	}
	// north: GTA +Y is Unreal -Y
	const FVector2D North(FVector2D::DotProduct(FVector2D(0, -1), Right), -FVector2D::DotProduct(FVector2D(0, -1), Fwd));
	Tex(TEXT("radar_north"), Centre + North.GetSafeNormal() * D * 0.5f * 0.93f, FVector2D(BlipPx, BlipPx), FLinearColor::White);
	// the player: arrow turned by the pawn's heading relative to the camera
	APlayerController* PC = GetOwningPlayerController();
	const float PawnYaw = PC && PC->GetPawn() ? PC->GetPawn()->GetActorRotation().Yaw : CameraYaw;
	Tex(TEXT("radar_centre"), Centre, FVector2D(BlipPx, BlipPx) * 0.75f, FLinearColor::White, PawnYaw - CameraYaw);
}

void AGTAIVPlayerHUD::DrawTopRight()
{
	// cash in the game's Pricedown digits
	if (const FGTAIVHudElement* C = El(TEXT("HUD_CASH")))
	{
		Text(FString::Printf(TEXT("$%d"), Money), C->Pos, C->Size, Col(TEXT("HUD_COLOUR_WHITE")), 2, true, false);
	}
	// wanted stars: six back stars, the level in front
	if (WantedLevel > 0)
	{
		const FGTAIVHudElement* WF = El(TEXT("HUD_WANTED_FRONT"));
		const FVector2D Pos = WF ? WF->Pos : FVector2D(0.929, 0.032);
		const FVector2D Scale = WF ? WF->Size : FVector2D(0.294, 0.466);
		const TCHAR Star = TCHAR(StarChar);
		const float Step = TextWidth(FString(1, &Star), Scale, false) + 0.002f;
		const float Y = Pos.Y - 0.035f;
		for (int32 i = 0; i < 6; ++i)
		{
			const FVector2D S(Pos.X - (i + 1) * Step, Y);
			const bool bOn = i < WantedLevel;
			Text(FString(1, &Star), S, Scale, bOn ? Col(TEXT("HUD_COLOUR_STAR")) : Col(TEXT("HUD_COLOUR_BLACK"), 100.f / 255.f), 0, false, bOn);
		}
	}
	// weapon icon
	if (!WeaponIcon.IsEmpty())
	{
		if (const FGTAIVHudElement* W = El(TEXT("HUD_WEAPON_ICON")))
		{
			const FVector2D SizePx = FVector2D(W->Size.X * 0.42f, W->Size.Y * 0.42f) * Area.GetSize();
			const FVector2D Right = ToPx(FVector2D(W->Pos.X, W->Pos.Y + 0.09f));
			if (WeaponIconTexture)
			{
				FCanvasTileItem Item(Right - FVector2D(SizePx.X, 0.f), WeaponIconTexture->GetResource(), SizePx, FLinearColor(1, 1, 1, 0.9f));
				Item.BlendMode = SE_BLEND_Translucent;
				Canvas->DrawItem(Item);
			}
			else
			{
				Tex(WeaponIcon, Right - FVector2D(SizePx.X * 0.5f, -SizePx.Y * 0.5f), SizePx, FLinearColor(1, 1, 1, 0.9f));
			}
			if (!AmmoText.IsEmpty())
			{
				if (const FGTAIVHudElement* A = El(TEXT("HUD_AMMO")))
				{
					Text(AmmoText, FVector2D(W->Pos.X - W->Size.X * 0.42f - 0.004f, W->Pos.Y + 0.1f), A->Size * 0.8f, Col(TEXT("HUD_COLOUR_WHITE")), 2);
				}
			}
		}
	}
}

void AGTAIVPlayerHUD::DrawMessages(double Now)
{
	// area name, bottom right, fades out after a few seconds
	const float AreaAge = float(Now - AreaSince);
	if (AreaAge < AreaNameSeconds && !AreaShown.IsEmpty())
	{
		const float A = FMath::Clamp(AreaAge / FadeSeconds, 0.f, 1.f) * FMath::Clamp((AreaNameSeconds - AreaAge) / FadeSeconds, 0.f, 1.f);
		if (const FGTAIVHudElement* E = El(TEXT("HUD_AREA_NAME")))
		{
			Text(AreaShown, E->Pos, E->Size, Col(TEXT("HUD_COLOUR_WHITE"), A), 2);
		}
	}
	// vehicle name, above the area name, grey
	const float VehicleAge = float(Now - VehicleSince);
	if (VehicleAge < AreaNameSeconds && !VehicleShown.IsEmpty())
	{
		const float A = FMath::Clamp(VehicleAge / FadeSeconds, 0.f, 1.f) * FMath::Clamp((AreaNameSeconds - VehicleAge) / FadeSeconds, 0.f, 1.f);
		if (const FGTAIVHudElement* E = El(TEXT("HUD_VEHICLE_NAME")))
		{
			Text(VehicleShown, E->Pos, E->Size, Col(TEXT("HUD_COLOUR_GREY"), A), 2);
		}
	}
	// help box, top left
	if (Now < HelpUntil && !HelpText.IsEmpty())
	{
		if (const FGTAIVHudElement* E = El(TEXT("HUD_HELP_MESSAGE")))
		{
			const FVector2D Scale = E->Size;
			const float LineH = Scale.Y * 40.f / 488.f;
			TArray<FString> Words;
			HelpText.ParseIntoArray(Words, TEXT(" "), true);
			TArray<FString> Lines;
			FString Line;
			for (const FString& W : Words)
			{
				const FString Try = Line.IsEmpty() ? W : Line + TEXT(" ") + W;
				if (!Line.IsEmpty() && TextWidth(Try, Scale, false) > 0.24f)
				{
					Lines.Add(Line);
					Line = W;
				}
				else
				{
					Line = Try;
				}
			}
			Lines.Add(Line);
			const FVector2D BoxPos = ToPx(E->Pos - FVector2D(0.008f, 0.008f));
			const FVector2D BoxSize = FVector2D(0.256f, Lines.Num() * LineH + 0.016f) * Area.GetSize();
			FCanvasTileItem Box(BoxPos, BoxSize, FLinearColor(0, 0, 0, 0.6f));
			Box.BlendMode = SE_BLEND_Translucent;
			Canvas->DrawItem(Box);
			for (int32 i = 0; i < Lines.Num(); ++i)
			{
				Text(Lines[i], E->Pos + FVector2D(0, i * LineH), Scale, Col(TEXT("HUD_COLOUR_WHITE")), 0, false, false);
			}
		}
	}
	// subtitles, bottom centre
	if (Now < SubtitleUntil && !Subtitle.IsEmpty())
	{
		if (const FGTAIVHudElement* E = El(TEXT("HUD_SUBITILES")))
		{
			TArray<FString> Lines;
			Subtitle.ParseIntoArray(Lines, TEXT("\n"), true);
			const float LineH = E->Size.Y * 40.f / 488.f;
			for (int32 i = 0; i < Lines.Num(); ++i)
			{
				DrawColouredLine(Lines[i], FVector2D(E->Pos.X, E->Pos.Y - LineH * (Lines.Num() - i)), E->Size, 1);
			}
		}
	}
}


// ------------------------------------------------------------------ wasted

bool AGTAIVPlayerHUD::bRespawnFade = false;

FVector AGTAIVPlayerHUD::BodyLocation(const APawn* Pawn) const
{
	if (!Pawn)
	{
		return FVector::ZeroVector;
	}
	TArray<USkeletalMeshComponent*> Meshes;
	Pawn->GetComponents(Meshes, false);
	for (const USkeletalMeshComponent* M : Meshes)
	{
		if (M->GetOwner() == Pawn && M->GetBoneIndex(TEXT("pelvis")) != INDEX_NONE)
		{
			return M->GetBoneLocation(TEXT("pelvis"));  // the ragdoll, wherever it fell
		}
	}
	return Pawn->GetActorLocation();
}

void AGTAIVPlayerHUD::UpdateDeath(double Real)
{
	APlayerController* PC = GetOwningPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!bDeath)
	{
		bDeath = true;
		bReloaded = false;
		DeathStart = Real;
		if (GetWorld()->GetNetMode() == NM_Standalone)  // (slow motion would slow everyone in a shared game)
		{
			UGameplayStatics::SetGlobalTimeDilation(this, 0.25f);
			UGameplayStatics::SetGlobalPitchModulation(this, 0.6f, 1.f);
		}
		if (PC && PC->PlayerCameraManager)
		{
			const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
			const FRotator CamRot = PC->PlayerCameraManager->GetCameraRotation();
			FActorSpawnParameters SP;
			SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (ACameraActor* Cam = GetWorld()->SpawnActor<ACameraActor>(CamLoc, CamRot, SP))
			{
				UCameraComponent* CC = Cam->GetCameraComponent();
				CC->bConstrainAspectRatio = false;
				CC->SetFieldOfView(PC->PlayerCameraManager->GetFOVAngle());
				CC->PostProcessBlendWeight = 1.f;
				PC->SetViewTargetWithBlend(Cam, 0.f);
				DeathCam = Cam;
				const FVector Off = CamLoc - BodyLocation(Pawn);
				CamDist0 = FMath::Max(150.f, Off.Size2D());
				CamHeight0 = Off.Z;
				CamYaw = Off.Rotation().Yaw;
			}
		}
	}
	const float T = float(Real - DeathStart);
	if (ACameraActor* Cam = DeathCam.Get())
	{
		// slowly up and away from the body, drifting round it, the colour draining out
		const FVector Body = BodyLocation(Pawn);
		const float E = 1.f - FMath::Square(1.f - FMath::Clamp(T / 7.f, 0.f, 1.f));
		const float Dist = CamDist0 + 550.f * E;
		const float Height = CamHeight0 + 420.f * E;
		const FVector Loc = Body + FRotator(0.f, CamYaw + 12.f * T, 0.f).Vector() * Dist + FVector(0, 0, Height);
		Cam->SetActorLocationAndRotation(Loc, (Body - Loc).Rotation());
		const float Sat = 1.f - FMath::Clamp(T / 1.2f, 0.f, 1.f);
		FPostProcessSettings& PP = Cam->GetCameraComponent()->PostProcessSettings;
		PP.bOverride_ColorSaturation = true;
		PP.ColorSaturation = FVector4(Sat, Sat, Sat, 1.f);
		PP.bOverride_ColorContrast = true;
		PP.ColorContrast = FVector4(1.f + 0.15f * (1.f - Sat), 1.f + 0.15f * (1.f - Sat), 1.f + 0.15f * (1.f - Sat), 1.f);
		PP.bOverride_VignetteIntensity = true;
		PP.VignetteIntensity = 0.4f + 0.5f * (1.f - Sat);
	}
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	// WASTED
	if (T > 0.8f && T < 6.2f && Data)
	{
		const float A = FMath::Clamp((T - 0.8f) / 0.35f, 0.f, 1.f);
		const UGTAIVFrontendData* F = Data->Font.Get();
		const FString Word = (F && !F->WastedText.IsEmpty() ? F->WastedText : FString(TEXT("Wasted"))).ToUpper();
		const FGTAIVHudElement* AreaEl = El(TEXT("HUD_AREA_NAME"));
		const FVector2D S = (AreaEl ? AreaEl->Size : FVector2D(0.5f, 0.5f)) * 2.6f;
		Text(Word, FVector2D(0.5f, 0.44f), S, FLinearColor(0.78f, 0.08f, 0.08f, A), 1);
	}
	// fade to black, then the loading screen
	if (T > 5.f)
	{
		FCanvasTileItem Black(FVector2D::ZeroVector, FVector2D(W, H), FLinearColor(0, 0, 0, FMath::Clamp(T - 5.f, 0.f, 1.f)));
		Black.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Black);
	}
	if (T > 6.f)
	{
		DrawLoadingText(T - 6.f);
	}
	if (T > WastedSeconds && !bReloaded && GetWorld()->GetNetMode() != NM_Standalone)
	{
		// multiplayer: the server respawns the player; the new body ends the sequence
		const UGTAIVPlayerComponent* Now = Pawn ? Pawn->FindComponentByClass<UGTAIVPlayerComponent>() : nullptr;
		if (Now && !Now->IsWasted())
		{
			bReloaded = true;
			bDeath = false;
			if (ACameraActor* Cam = DeathCam.Get())
			{
				Cam->Destroy();
			}
			RespawnFadeStart = FPlatformTime::Seconds();
			if (PC)
			{
				PC->SetViewTargetWithBlend(Pawn, 0.f);
			}
		}
		return;
	}
	if (T > WastedSeconds && !bReloaded)
	{
		bReloaded = true;
		UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
		UGameplayStatics::SetGlobalPitchModulation(this, 1.f, 0.f);
		bRespawnFade = true;
		FString Options;
		if (const AGameModeBase* GM = GetWorld()->GetAuthGameMode())
		{
			Options = GM->OptionsString;  // keeps e.g. ?game=GM_GTAIV
			Options.RemoveFromStart(TEXT("?"));
		}
		UGameplayStatics::OpenLevel(this, FName(*UGameplayStatics::GetCurrentLevelName(this, true)), true, Options);
	}
}

void AGTAIVPlayerHUD::DrawLoadingText(float T)
{
	const UGTAIVFrontendData* F = Data ? Data->Font.Get() : nullptr;
	if (!F || !F->Font3Texture)
	{
		return;
	}
	if (Font3Cells.Num() == 0)
	{
		for (int32 Cell = F->Font3Start; Cell < F->Font3End && Cell < F->Font3Map.Num(); ++Cell)
		{
			Font3Cells.FindOrAdd(F->Font3Map[Cell], Cell);
		}
	}
	// "LOADING - <mission or area>" in the garage font, bottom right, with a highlight sweeping along
	FString Word = F->LoadingText;
	Word.TrimEndInline();
	Word.RemoveFromEnd(TEXT("-"));
	Word.TrimEndInline();
	const FString Sub = !LoadingSubtitle.IsEmpty() ? LoadingSubtitle : CurrentArea;
	const FString Line = (Sub.IsEmpty() ? Word : Word + TEXT(" \u2014 ") + Sub).ToUpper();
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	const float CellH = H * 0.075f, CellW = CellH * 0.8f;
	auto Advance = [&](TCHAR Ch) {
		if (Ch == TEXT(' '))
		{
			return CellW * 8.f / 32.f;
		}
		if (Ch == 0x2014)
		{
			return CellW * 0.62f;
		}
		const int32* Cell = Font3Cells.Find(int32(Ch));
		return Cell ? CellW * (32 - (F->Font3Prop.IsValidIndex(*Cell) ? F->Font3Prop[*Cell] : 0)) / 32.f : CellW * 0.4f;
	};
	float Total = 0.f;
	for (const TCHAR Ch : Line)
	{
		Total += Advance(Ch);
	}
	float X = W * 0.962f - Total;
	const float Y = H * 0.885f;
	const float Sweep = FMath::Fmod(T * 6.f, float(Line.Len() + 8)) - 4.f;
	const FTextureResource* Res = F->Font3Texture->GetResource();
	for (int32 i = 0; i < Line.Len(); ++i)
	{
		const TCHAR Ch = Line[i];
		const float B = 0.52f + 0.48f * FMath::Exp(-FMath::Square((i - Sweep) / 1.3f));
		const FLinearColor C(B, B, B, 1.f);
		if (Ch == 0x2014)
		{
			FCanvasTileItem Dash(FVector2D(X + CellW * 0.06f, Y + CellH * 0.47f), FVector2D(CellW * 0.5f, CellH * 0.035f), C);
			Canvas->DrawItem(Dash);
		}
		else if (const int32* Cell = Font3Cells.Find(int32(Ch)))
		{
			const int32 Cl = *Cell;
			const float V0 = (Cl / 16) * 80.f / 1024.f;
			FCanvasTileItem Glyph(FVector2D(X, Y), Res, FVector2D(CellW, CellH), FVector2D((Cl % 16) / 16.f, V0),
								  FVector2D((Cl % 16 + 1) / 16.f, V0 + 80.f / 1024.f), C);
			Glyph.BlendMode = SE_BLEND_Translucent;
			Canvas->DrawItem(Glyph);
		}
		X += Advance(Ch);
	}
}


void AGTAIVPlayerHUD::ShowAreaText(const FString& Text)
{
	AreaShown = Text;
	AreaSince = GetWorld()->GetRealTimeSeconds();
}

void AGTAIVPlayerHUD::ShowVehicleName(const FString& Text)
{
	VehicleShown = Text;
	VehicleSince = GetWorld()->GetRealTimeSeconds();
}

// ------------------------------------------------------------------ multiplayer players

FLinearColor AGTAIVPlayerHUD::PlayerColour(const APlayerState* P) const
{
	// GTA IV's multiplayer colours, handed out in joining order (the same on every machine)
	static const FLinearColor Colours[] = {
		FLinearColor(0.36f, 0.86f, 0.18f), FLinearColor(0.92f, 0.12f, 0.10f), FLinearColor(0.96f, 0.30f, 0.42f),
		FLinearColor(0.42f, 0.62f, 0.98f), FLinearColor(0.84f, 0.30f, 0.86f), FLinearColor(0.95f, 0.95f, 0.95f),
		FLinearColor(0.22f, 0.52f, 0.22f), FLinearColor(0.78f, 0.78f, 0.80f), FLinearColor(1.00f, 0.56f, 0.10f),
		FLinearColor(0.98f, 0.88f, 0.20f), FLinearColor(0.20f, 0.85f, 0.85f), FLinearColor(0.56f, 0.36f, 0.92f),
		FLinearColor(0.66f, 0.44f, 0.24f), FLinearColor(0.16f, 0.60f, 0.62f), FLinearColor(0.98f, 0.58f, 0.50f),
		FLinearColor(0.70f, 0.70f, 0.98f)};
	const AGameStateBase* GS = GetWorld() ? GetWorld()->GetGameState() : nullptr;
	if (!P || !GS)
	{
		return Colours[5];
	}
	int32 Rank = 0;
	for (const APlayerState* O : GS->PlayerArray)
	{
		if (O && O != P && O->GetPlayerId() < P->GetPlayerId())
		{
			++Rank;
		}
	}
	return Colours[Rank % UE_ARRAY_COUNT(Colours)];
}

void AGTAIVPlayerHUD::ShowPlayerList()
{
	ListSince = GetWorld()->GetRealTimeSeconds();
}

void AGTAIVPlayerHUD::DrawPlayerList(double Now)
{
	const AGameStateBase* GS = GetWorld()->GetGameState();
	APlayerController* PC = GetOwningPlayerController();
	if (!GS || !PC || GetWorld()->GetNetMode() == NM_Standalone)
	{
		return;
	}
	TArray<const APlayerState*> Players;
	for (const APlayerState* P : GS->PlayerArray)
	{
		if (P && !P->IsInactive())
		{
			Players.Add(P);
		}
	}
	Players.Sort([](const APlayerState& A, const APlayerState& B) { return A.GetPlayerId() < B.GetPlayerId(); });
	// shown for a while whenever someone joins or leaves, and while Tab / Back is held
	if (Players.Num() != ListedPlayers)
	{
		ListedPlayers = Players.Num();
		ListSince = Now;
	}
	const bool bHeld = PC->IsInputKeyDown(EKeys::Tab) || PC->IsInputKeyDown(EKeys::Gamepad_Special_Left);
	const float Alpha = bHeld ? 1.f : FMath::Clamp(float(6.0 - (Now - ListSince)), 0.f, 1.f);
	if (Alpha <= 0.f || bPhone)
	{
		return;
	}
	const FVector2D Scale(0.24f, 0.34f);
	const float Row = 0.031f;
	float Wide = TextWidth(TEXT("Free Mode"), Scale, false);
	for (const APlayerState* P : Players)
	{
		Wide = FMath::Max(Wide, TextWidth(P->GetPlayerName(), Scale, false));
	}
	const FVector2D Origin(0.025f, 0.04f);
	const FVector2D Pad(0.008f, 0.008f);
	Box(ToPx(Origin), FVector2D(Wide + Pad.X * 2.f, Row * (Players.Num() + 1) + Pad.Y * 2.f) * Area.GetSize(),
		FLinearColor(0.f, 0.f, 0.f, 0.62f * Alpha));
	Text(TEXT("Free Mode"), Origin + Pad, Scale, FLinearColor(1.f, 1.f, 1.f, Alpha), 0, false, false);
	for (int32 i = 0; i < Players.Num(); ++i)
	{
		FLinearColor C = PlayerColour(Players[i]);
		C.A = Alpha;
		Text(Players[i]->GetPlayerName(), Origin + Pad + FVector2D(0.f, Row * (i + 1)), Scale, C, 0, false, true);
	}
}

void AGTAIVPlayerHUD::DrawNameTags()
{
	const AGameStateBase* GS = GetWorld()->GetGameState();
	APlayerController* PC = GetOwningPlayerController();
	if (!GS || !PC || !PC->PlayerCameraManager || GetWorld()->GetNetMode() == NM_Standalone)
	{
		return;
	}
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	const FVector Fwd = PC->PlayerCameraManager->GetCameraRotation().Vector();
	const FVector2D Scale(0.25f, 0.36f);
	for (const APlayerState* P : GS->PlayerArray)
	{
		APawn* Other = P ? P->GetPawn() : nullptr;
		if (!Other || P == PC->PlayerState || Other == PC->GetPawn())
		{
			continue;
		}
		// over their head (over the car when driving), out to 120 m, never behind the camera
		const FVector Top = Other->GetActorLocation() + FVector(0, 0, Other->IsA<AGTAIVVehicle>() ? 170.f : 98.f);
		const float Dist = FVector::Dist(Cam, Top);
		if (Dist > 12000.f || FVector::DotProduct(Top - Cam, Fwd) <= 0.f)
		{
			continue;
		}
		const FVector S = Project(Top);
		if (S.Z <= 0.f)
		{
			continue;
		}
		const float Alpha = FMath::Clamp((12000.f - Dist) / 2000.f, 0.f, 1.f);
		FLinearColor C = PlayerColour(P);
		C.A = Alpha;
		const FString Name = P->GetPlayerName();
		const float W = TextWidth(Name, Scale, false) * Area.GetSize().X;
		const float Dot = Canvas->ClipY * 0.0075f;
		const FVector2D Left(S.X - (W + Dot * 3.f) * 0.5f, S.Y);
		// the coloured dot, outlined, then the name
		FCanvasNGonItem Ring(Left + FVector2D(Dot, Dot * 1.6f), FVector2D(Dot * 1.25f), 16, FLinearColor(0.f, 0.f, 0.f, 0.8f * Alpha));
		Ring.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Ring);
		FCanvasNGonItem Fill(Left + FVector2D(Dot, Dot * 1.6f), FVector2D(Dot), 16, C);
		Fill.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Fill);
		Text(Name, FVector2D((Left.X + Dot * 3.f) / Area.GetSize().X, Left.Y / Area.GetSize().Y), Scale, C, 0, false, true);
	}
}

void AGTAIVPlayerHUD::Box(FVector2D Min, FVector2D Size, FLinearColor Color)
{
	FCanvasTileItem Item(Min, Size, Color);
	Item.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Item);
}

void AGTAIVPlayerHUD::SetPhoneOpen(bool bOpen)
{
	if (bOpen != bPhone)
	{
		bPhone = bOpen;
		Page = EPhonePage::Home;
		PhoneSel = 4;  // Multiplayer
	}
}

TArray<FString> AGTAIVPlayerHUD::PhoneItems() const
{
	const UGTAIVSessions* S = UGTAIVSessions::Get(this);
	TArray<FString> Items;
	switch (Page)
	{
	case EPhonePage::Home:
		Items = {TEXT("Phonebook"), TEXT("Messages"), TEXT("Organiser"), TEXT("Camera"), TEXT("Multiplayer"), TEXT("Options")};
		break;
	case EPhonePage::Multiplayer:
		if (S && S->IsInGame())
		{
			Items = {TEXT("Invite Friends"), TEXT("Players"), S->IsHosting() ? TEXT("End Game") : TEXT("Leave Game")};
		}
		else
		{
			Items = {TEXT("Host Free Mode"), TEXT("Find Games"), TEXT("Invite Friends")};
		}
		break;
	case EPhonePage::Find:
		Items.Add(S && S->IsSearching() ? TEXT("Searching...") : TEXT("Refresh"));
		if (S)
		{
			for (const FGTAIVFoundGame& G : S->GetFoundGames())
			{
				Items.Add(FString::Printf(TEXT("%s  %d/%d  %dms"), *G.Host, G.Players, G.MaxPlayers, G.Ping));
			}
		}
		break;
	case EPhonePage::Players:
		if (const AGameStateBase* GS = GetWorld()->GetGameState())
		{
			for (const APlayerState* P : GS->PlayerArray)
			{
				if (P)
				{
					Items.Add(FString::Printf(TEXT("%s  %dms"), *P->GetPlayerName(), int32(P->GetPingInMilliseconds())));
				}
			}
		}
		break;
	default:
		break;
	}
	return Items;
}

void AGTAIVPlayerHUD::PhoneInput()
{
	APlayerController* PC = GetOwningPlayerController();
	if (!PC)
	{
		return;
	}
	const double Now = GetWorld()->GetRealTimeSeconds();
	const float Dt = LastPhoneTick > 0.0 ? float(FMath::Min(0.1, Now - LastPhoneTick)) : 0.f;
	LastPhoneTick = Now;
	PhoneUp = FMath::FInterpConstantTo(PhoneUp, bPhone ? 1.f : 0.f, Dt, 5.f);
	if (!bPhone)
	{
		if (PC->WasInputKeyJustPressed(EKeys::Up) || PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Up) && !PC->GetPawn())
		{
			SetPhoneOpen(true);
		}
		return;
	}
	const int32 N = FMath::Max(1, PhoneItems().Num());
	const bool bGrid = Page == EPhonePage::Home;
	if (PC->WasInputKeyJustPressed(EKeys::Up))
	{
		PhoneSel = bGrid ? (PhoneSel + N - 3) % N : (PhoneSel + N - 1) % N;
	}
	if (PC->WasInputKeyJustPressed(EKeys::Down))
	{
		PhoneSel = bGrid ? (PhoneSel + 3) % N : (PhoneSel + 1) % N;
	}
	if (bGrid && PC->WasInputKeyJustPressed(EKeys::Left))
	{
		PhoneSel = (PhoneSel + N - 1) % N;
	}
	if (bGrid && PC->WasInputKeyJustPressed(EKeys::Right))
	{
		PhoneSel = (PhoneSel + 1) % N;
	}
	if (PC->WasInputKeyJustPressed(EKeys::Enter) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Bottom))
	{
		PhoneSelect();
	}
	if (PC->WasInputKeyJustPressed(EKeys::BackSpace) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Right) ||
		PC->WasInputKeyJustPressed(EKeys::RightMouseButton))
	{
		PhoneBack();
	}
}

void AGTAIVPlayerHUD::PhoneBack()
{
	switch (Page)
	{
	case EPhonePage::Home:
		bPhone = false;  // put the phone away
		break;
	case EPhonePage::Find:
	case EPhonePage::Players:
		Page = EPhonePage::Multiplayer;
		PhoneSel = 0;
		break;
	default:
		Page = EPhonePage::Home;
		PhoneSel = 4;
		break;
	}
}

void AGTAIVPlayerHUD::PhoneSelect()
{
	UGTAIVSessions* S = UGTAIVSessions::Get(this);
	const TArray<FString> Items = PhoneItems();
	if (!Items.IsValidIndex(PhoneSel))
	{
		return;
	}
	const FString Item = Items[PhoneSel];
	switch (Page)
	{
	case EPhonePage::Home:
		if (Item == TEXT("Multiplayer"))
		{
			Page = EPhonePage::Multiplayer;
			PhoneSel = 0;
		}
		else
		{
			Page = EPhonePage::Info;
			InfoTitle = Item;
			InfoText = Item == TEXT("Phonebook") ? TEXT("Your Steam friends: use Multiplayer > Invite Friends.")
					 : Item == TEXT("Messages") ? TEXT("No new messages.")
					 : Item == TEXT("Organiser") ? TEXT("No appointments.")
					 : Item == TEXT("Camera") ? TEXT("Camera unavailable.")
					 : TEXT("Ringtone: Badger default");
		}
		break;
	case EPhonePage::Multiplayer:
		if (!S)
		{
			break;
		}
		if (Item == TEXT("Host Free Mode"))
		{
			// Free Mode is played on the game's map (the front end's Start New Game level, Liberty City)
			const UGTAIVFrontendData* F = Data ? Data->Font.Get() : nullptr;
			S->HostGame(16, F ? F->StartLevel : FString());
		}
		else if (Item == TEXT("Find Games"))
		{
			Page = EPhonePage::Find;
			PhoneSel = 0;
			S->FindGames();
		}
		else if (Item == TEXT("Invite Friends"))
		{
			S->InviteFriends();
		}
		else if (Item == TEXT("Players"))
		{
			Page = EPhonePage::Players;
			PhoneSel = 0;
		}
		else
		{
			S->LeaveGame();
		}
		break;
	case EPhonePage::Find:
		if (S && PhoneSel == 0)
		{
			S->FindGames();
		}
		else if (S)
		{
			S->JoinGame(PhoneSel - 1);
		}
		break;
	default:
		break;
	}
}

void AGTAIVPlayerHUD::DrawPhone()
{
	if (PhoneUp <= 0.f || !Data)
	{
		return;
	}
	const TObjectPtr<UTexture2D>* Body = Data->Textures.Find(TEXT("phone_body"));
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	// the handset (the left half of amb_mobile_1's texture: its face), bottom right, sliding up
	const float PH = H * 0.62f, PW = PH * 0.5f;
	const float Ease = 1.f - FMath::Square(1.f - PhoneUp);
	const FVector2D Pos(W * 0.80f - PW * 0.5f, H - PH * 0.86f * Ease + (1.f - Ease) * H * 0.15f);
	if (Body && *Body)
	{
		FCanvasTileItem Item(Pos, (*Body)->GetResource(), FVector2D(PW, PH), FVector2D(0.f, 0.f), FVector2D(0.5f, 1.f), FLinearColor::White);
		Item.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Item);
	}
	else
	{
		Box(Pos, FVector2D(PW, PH), FLinearColor(0.08f, 0.08f, 0.09f, 1.f));
	}
	// the screen (where the texture shows its LCD)
	const FVector2D S0 = Pos + FVector2D(PW * 0.06f, PH * 0.035f);
	const FVector2D SS(PW * 0.88f, PH * 0.37f);
	Box(S0, SS, FLinearColor(0.05f, 0.09f, 0.16f, 1.f));
	Box(S0 + FVector2D(0, SS.Y * 0.45f), FVector2D(SS.X, SS.Y * 0.55f), FLinearColor(0.06f, 0.16f, 0.30f, 0.9f));
	// status bar: signal, clock
	Box(S0, FVector2D(SS.X, SS.Y * 0.1f), FLinearColor(0.f, 0.f, 0.f, 0.55f));
	const FDateTime T = FDateTime::Now();
	auto Px = [&](FVector2D P) { return FVector2D(P.X / W, P.Y / H); };
	const FVector2D Small = FVector2D(0.16f, 0.24f) * (PH / (H * 0.62f));
	const FVector2D Big = FVector2D(0.2f, 0.3f) * (PH / (H * 0.62f));
	Text(FString::Printf(TEXT("%02d:%02d"), T.GetHour(), T.GetMinute()), Px(S0 + FVector2D(SS.X * 0.96f, SS.Y * 0.012f)), Small,
		 FLinearColor::White, 2, false, false);
	for (int32 B = 0; B < 4; ++B)
	{
		Box(S0 + FVector2D(SS.X * (0.04f + B * 0.03f), SS.Y * (0.075f - B * 0.015f)), FVector2D(SS.X * 0.02f, SS.Y * (0.015f + B * 0.015f)),
			FLinearColor::White);
	}
	const UGTAIVSessions* Sess = UGTAIVSessions::Get(this);
	const TArray<FString> Items = PhoneItems();
	const FLinearColor Sel(1.f, 0.62f, 0.f, 1.f);
	const TCHAR* Title = Page == EPhonePage::Home ? TEXT("") : Page == EPhonePage::Multiplayer ? TEXT("Multiplayer")
					   : Page == EPhonePage::Find ? TEXT("Free Mode") : Page == EPhonePage::Players ? TEXT("Players") : *InfoTitle;
	if (Page == EPhonePage::Home)
	{
		// the icon grid; the selected one's name above it
		const TCHAR* Icons[6] = {TEXT("phone_icon_invite"), TEXT("phone_icon_envelope"), TEXT("phone_icon_appointment"),
								 TEXT("phone_icon_picture"), TEXT("icon_gta"), TEXT("phone_icon_bell")};
		if (Items.IsValidIndex(PhoneSel))
		{
			Text(Items[PhoneSel], Px(S0 + FVector2D(SS.X * 0.5f, SS.Y * 0.14f)), Big, FLinearColor::White, 1);
		}
		const float Cell = SS.X / 3.f;
		for (int32 i = 0; i < 6; ++i)
		{
			const FVector2D C = S0 + FVector2D(Cell * (i % 3 + 0.5f), SS.Y * (0.48f + 0.3f * (i / 3)));
			if (i == PhoneSel)
			{
				Box(C - FVector2D(Cell * 0.42f, SS.Y * 0.13f), FVector2D(Cell * 0.84f, SS.Y * 0.26f), FLinearColor(1.f, 0.62f, 0.f, 0.45f));
			}
			Tex(Icons[i], C, FVector2D(Cell * 0.55f, Cell * 0.55f), FLinearColor::White);
		}
	}
	else if (Page == EPhonePage::Info)
	{
		Text(Title, Px(S0 + FVector2D(SS.X * 0.5f, SS.Y * 0.14f)), Big, FLinearColor::White, 1);
		Text(InfoText, Px(S0 + FVector2D(SS.X * 0.06f, SS.Y * 0.4f)), Small, FLinearColor::White, 0);
	}
	else
	{
		Text(Title, Px(S0 + FVector2D(SS.X * 0.5f, SS.Y * 0.14f)), Big, FLinearColor::White, 1);
		const int32 Rows = 5;
		const int32 First = FMath::Clamp(PhoneSel - Rows + 1, 0, FMath::Max(0, Items.Num() - Rows));
		for (int32 i = First; i < Items.Num() && i < First + Rows; ++i)
		{
			const FVector2D R = S0 + FVector2D(SS.X * 0.04f, SS.Y * (0.3f + 0.11f * (i - First)));
			if (i == PhoneSel)
			{
				Box(R - FVector2D(0, SS.Y * 0.01f), FVector2D(SS.X * 0.92f, SS.Y * 0.1f), FLinearColor(1.f, 0.62f, 0.f, 0.85f));
			}
			Text(Items[i], Px(R + FVector2D(SS.X * 0.03f, 0)), Small, i == PhoneSel ? FLinearColor::Black : FLinearColor::White, 0, false, false);
		}
		if (Items.Num() == 0)
		{
			Text(TEXT("Nobody here"), Px(S0 + FVector2D(SS.X * 0.06f, SS.Y * 0.32f)), Small, FLinearColor::White, 0);
		}
		// what the online service is doing
		if (Sess)
		{
			const FString Line = Sess->GetStatus().IsEmpty() ? Sess->GetServiceName() : Sess->GetStatus();
			Text(Line, Px(S0 + FVector2D(SS.X * 0.5f, SS.Y * 0.88f)), Small, Sel, 1, false, false);
		}
	}
}

void AGTAIVPlayerHUD::DrawRadio()
{
	// GTA IV's station strip: the station playing in colour in the middle, its neighbours grey on
	// either side, for a few seconds after retuning
	const UGTAIVRadio* Radio = UGTAIVRadio::Get(this);
	if (!Radio || !Radio->HasStations() || !Radio->IsListening())
	{
		return;
	}
	const float Age = Radio->GetTimeSinceTuned();
	const float Show = 3.f;
	if (Age > Show)
	{
		return;
	}
	const float A = FMath::Clamp((Show - Age) / 0.5f, 0.f, 1.f);
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	const FVector2D Centre(W * 0.5f, H * 0.075f);
	const int32 N = Radio->GetStationCount() + 1;  // + Radio Off
	const int32 Cur = Radio->GetStation() == INDEX_NONE ? Radio->GetStationCount() : Radio->GetStation();
	auto Logo = [&](int32 Index, FVector2D At, float Height, float Alpha, bool bColour)
	{
		if (UTexture2D* T = Radio->GetStationLogo(Index, bColour))
		{
			const FVector2D Size(Height * T->GetSizeX() / FMath::Max(1, T->GetSizeY()), Height);
			FCanvasTileItem Item(At - Size * 0.5f, T->GetResource(), Size, FLinearColor(1.f, 1.f, 1.f, Alpha));
			Item.BlendMode = SE_BLEND_Translucent;
			Canvas->DrawItem(Item);
		}
		else if (Index >= Radio->GetStationCount() || Index == INDEX_NONE)
		{
			Text(TEXT("Radio Off"), FVector2D(At.X / W, (At.Y - Height * 0.25f) / H), FVector2D(0.35f, 0.5f) * (Height / (H * 0.09f)),
				 FLinearColor(1.f, 1.f, 1.f, Alpha), 1);
		}
	};
	for (int32 D = 3; D >= 1; --D)
	{
		for (int32 Side : {-1, 1})
		{
			const int32 I = ((Cur + Side * D) % N + N) % N;
			// clear of the middle logo (2:1, 0.09 H tall), then a logo width apart
			Logo(I, Centre + FVector2D(Side * H * (0.17f + 0.13f * (D - 1)), 0.f), H * 0.055f, A * (0.8f - 0.2f * (D - 1)), false);
		}
	}
	Logo(Cur, Centre, H * 0.09f, A, true);
}
