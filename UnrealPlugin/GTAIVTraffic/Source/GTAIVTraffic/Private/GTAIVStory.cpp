#include "GTAIVStory.h"

#include "Animation/AnimSequence.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GTAIVPedManager.h"
#include "GTAIVPlayerHUD.h"
#include "GTAIVTrafficData.h"
#include "GTAIVTrafficManager.h"
#include "GTAIVVehicle.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Sound/SoundBase.h"

DEFINE_LOG_CATEGORY_STATIC(LogGTAIVStory, Log, All);

namespace
{
	const TCHAR* StoryDataPath = TEXT("/Game/GTAIV/Story/DA_GTAIV_Story.DA_GTAIV_Story");
	const TCHAR* PedClassPath = TEXT("/Game/GTAIV/Characters/BP_GTAIV_Ped.BP_GTAIV_Ped_C");

	FTransform RootAt(const TArray<float>& R, int32 F)
	{
		const int32 N = R.Num() / 7;
		if (N == 0)
		{
			return FTransform::Identity;
		}
		const int32 i = FMath::Clamp(F, 0, N - 1) * 7;
		return FTransform(FQuat(R[i + 3], R[i + 4], R[i + 5], R[i + 6]).GetNormalized(), FVector(R[i], R[i + 1], R[i + 2]));
	}

	FTransform RootLerp(const TArray<float>& R, float Frame)
	{
		const int32 F0 = FMath::FloorToInt(Frame);
		const FTransform A = RootAt(R, F0), B = RootAt(R, F0 + 1);
		if (FVector::DistSquared(A.GetLocation(), B.GetLocation()) > 100.f * 100.f)
		{
			return A;   // a cut: don't slide between the two places
		}
		const float T = Frame - F0;
		return FTransform(FQuat::Slerp(A.GetRotation(), B.GetRotation(), T), FMath::Lerp(A.GetLocation(), B.GetLocation(), T));
	}

	const FGTAIVVehicleDef* FindVehicle(FName Name)
	{
		if (const UGTAIVVehicleSet* Set = AGTAIVVehicle::LoadVehicleSet())
		{
			for (const FGTAIVVehicleDef& D : Set->Vehicles)
			{
				if (D.Name == Name)
				{
					return &D;
				}
			}
		}
		return nullptr;
	}
}

UGTAIVStoryData* UGTAIVStoryData::Load()
{
	return LoadObject<UGTAIVStoryData>(nullptr, StoryDataPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

// ------------------------------------------------------------------ time of day

void UGTAIVStory::SetTimeOfDay(UObject* WorldContext, float Hour)
{
	UWorld* W = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!W)
	{
		return;
	}
	// Liberty City in summer: up at 5:30, down at 20:30
	const float H = FMath::Fmod(FMath::Fmod(Hour, 24.f) + 24.f, 24.f);
	const float Elevation = 65.f * FMath::Sin(PI * (H - 5.5f) / 15.f);   // degrees above the horizon
	for (TActorIterator<ADirectionalLight> It(W); It; ++It)
	{
		UDirectionalLightComponent* L = Cast<UDirectionalLightComponent>(It->GetLightComponent());
		if (!L || L->Mobility != EComponentMobility::Movable)
		{
			continue;
		}
		const FRotator R = It->GetActorRotation();
		// below the horizon the sky goes through twilight to night (the sun lights it from beneath)
		It->SetActorRotation(FRotator(-FMath::Max(Elevation, -12.f), R.Yaw, 0.f));
		const float Day = FMath::Clamp((Elevation + 4.f) / 10.f, 0.f, 1.f);
		L->SetIntensity(FMath::Lerp(0.04f, 8.f, Day * Day));
		break;
	}
	if (UMaterialParameterCollection* MPC = LoadObject<UMaterialParameterCollection>(
			nullptr, TEXT("/Game/GTAIV/Materials/MPC_GTAIV.MPC_GTAIV"), nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		UKismetMaterialLibrary::SetScalarParameterValue(W, MPC, TEXT("Night"), FMath::Clamp((2.f - Elevation) / 6.f, 0.f, 1.f));
	}
}

// ------------------------------------------------------------------ cutscene player

AGTAIVCutscenePlayer::AGTAIVCutscenePlayer()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

AGTAIVPlayerHUD* AGTAIVCutscenePlayer::HUD() const
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	return PC ? Cast<AGTAIVPlayerHUD>(PC->GetHUD()) : nullptr;
}

void AGTAIVCutscenePlayer::Play(UGTAIVCutscene* InCutscene, TFunction<void()> InOnFinished)
{
	Stop();
	if (!InCutscene || InCutscene->Shots.Num() == 0)
	{
		if (InOnFinished)
		{
			InOnFinished();
		}
		return;
	}
	Cutscene = InCutscene;
	OnFinished = MoveTemp(InOnFinished);
	if (Cutscene->Hour >= 0.f)
	{
		UGTAIVStory::SetTimeOfDay(this, Cutscene->Hour);
	}
	Time = 0.f;
	Shot = INDEX_NONE;
	NextSubtitle = 0;
	UWorld* W = GetWorld();
	APlayerController* PC = W->GetFirstPlayerController();
	if (PC)
	{
		// the cutscene has its own Niko: hide ours and keep him still
		if (APawn* P = PC->GetPawn())
		{
			HiddenPawn = P;
			P->SetActorHiddenInGame(true);
			P->DisableInput(PC);
			if (AGTAIVVehicle* V = Cast<AGTAIVVehicle>(P))
			{
				if (APawn* D = V->GetDriver())
				{
					D->SetActorHiddenInGame(true);
					HiddenExtra.Add(D);
				}
			}
		}
		PC->SetIgnoreMoveInput(true);
		PC->SetIgnoreLookInput(true);
	}
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	Camera = W->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform::Identity, SP);
	if (Camera)
	{
		UCameraComponent* CC = Camera->GetCameraComponent();
		CC->bConstrainAspectRatio = false;
		// GTA IV's cutscenes keep night dark: don't let the eye adaptation brighten a dark scene until
		// the lit faces burn out white
		CC->PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
		CC->PostProcessSettings.AutoExposureMinBrightness = 2.5f;
		CC->PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
		CC->PostProcessSettings.AutoExposureMaxBrightness = 14.f;
		CC->PostProcessSettings.bOverride_AutoExposureSpeedUp = true;
		CC->PostProcessSettings.AutoExposureSpeedUp = 20.f;
		CC->PostProcessSettings.bOverride_AutoExposureSpeedDown = true;
		CC->PostProcessSettings.AutoExposureSpeedDown = 20.f;
		CC->PostProcessBlendWeight = 1.f;
		if (PC)
		{
			PC->SetViewTargetWithBlend(Camera, 0.f);
		}
	}
	if (AGTAIVPlayerHUD* H = HUD())
	{
		H->SetCutsceneMode(true);
	}
	// the streets go quiet for the scene (traffic and peds come back after)
	for (TActorIterator<AGTAIVTrafficManager> It(W); It; ++It)
	{
		bTrafficWas = It->bTrafficEnabled;
		It->bTrafficEnabled = false;
		It->ClearTraffic();
	}
	for (TActorIterator<AGTAIVPedManager> It(W); It; ++It)
	{
		bPedsWas = It->bPedsEnabled;
		It->bPedsEnabled = false;
		It->ClearPeds();
	}
	if (Cutscene->Audio)
	{
		Audio = UGameplayStatics::CreateSound2D(this, Cutscene->Audio, 1.f, 1.f, 0.f, nullptr, false, false);
		if (Audio)
		{
			Audio->Play();
		}
	}
	SetActorTickEnabled(true);
	Tick(0.f);
}

void AGTAIVCutscenePlayer::ClearShot()
{
	for (USceneComponent* C : ShotComponents)
	{
		if (C)
		{
			C->DestroyComponent();
		}
	}
	ShotComponents.Reset();
	Live.Reset();
	LiveLights.Reset();
}

void AGTAIVCutscenePlayer::BuildShot(int32 Index)
{
	ClearShot();
	Shot = Index;
	const FGTAIVCutShot& S = Cutscene->Shots[Index];
	for (int32 a = 0; a < S.Actors.Num(); ++a)
	{
		const FGTAIVCutActor& A = S.Actors[a];
		FActorComps L;
		L.Actor = a;
		if (A.SkeletalMesh)
		{
			USkeletalMeshComponent* K = NewObject<USkeletalMeshComponent>(this);
			K->SetSkeletalMeshAsset(A.SkeletalMesh);
			K->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			K->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			K->SetupAttachment(RootComponent);
			K->RegisterComponent();
			if (A.Anim)
			{
				K->SetAnimationMode(EAnimationMode::AnimationSingleNode);
				K->SetAnimation(A.Anim);
				K->Play(false);
				K->SetPlayRate(0.f);
			}
			L.Root = K;
			L.Skel = K;
			ShotComponents.Add(K);
		}
		else if (A.StaticMesh)
		{
			UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(this);
			M->SetMobility(EComponentMobility::Movable);
			M->SetStaticMesh(A.StaticMesh);
			M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			M->SetupAttachment(RootComponent);
			M->RegisterComponent();
			L.Root = M;
			ShotComponents.Add(M);
		}
		else if (const FGTAIVVehicleDef* D = A.Vehicle.IsNone() ? nullptr : FindVehicle(A.Vehicle))
		{
			UStaticMeshComponent* B = NewObject<UStaticMeshComponent>(this);
			B->SetMobility(EComponentMobility::Movable);
			B->SetStaticMesh(D->Body);
			B->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			B->SetupAttachment(RootComponent);
			B->RegisterComponent();
			ShotComponents.Add(B);
			for (const FGTAIVWheel& Wh : D->Wheels)
			{
				UStaticMeshComponent* Wm = NewObject<UStaticMeshComponent>(this);
				Wm->SetMobility(EComponentMobility::Movable);
				Wm->SetStaticMesh(Wh.Mesh);
				Wm->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Wm->SetupAttachment(B);
				Wm->SetRelativeLocation(Wh.Offset);
				Wm->SetRelativeRotation(FRotator(0.f, Wh.bRightSide ? 180.f : 0.f, 0.f));
				Wm->RegisterComponent();
				ShotComponents.Add(Wm);
			}
			L.Root = B;
		}
		if (L.Root.IsValid())
		{
			Live.Add(L);
		}
	}
	// the scene's lights: GTA IV lights its cutscenes itself (spots on the faces, fills, rims)
	for (int32 l = 0; l < S.Lights.Num(); ++l)
	{
		const FGTAIVCutLight& CL = S.Lights[l];
		ULocalLightComponent* LC = nullptr;
		if (CL.bSpot)
		{
			USpotLightComponent* Sp = NewObject<USpotLightComponent>(this);
			Sp->SetInnerConeAngle(18.f);
			Sp->SetOuterConeAngle(42.f);
			Sp->SetIntensity(2200.f);
			LC = Sp;
		}
		else
		{
			UPointLightComponent* Pt = NewObject<UPointLightComponent>(this);
			Pt->SetIntensity(CL.bRim ? 1200.f : 800.f);
			Pt->SetSourceRadius(40.f);
			LC = Pt;
		}
		LC->SetIntensityUnits(ELightUnits::Lumens);
		LC->SetMobility(EComponentMobility::Movable);
		LC->SetLightColor(CL.Colour);
		LC->SetAttenuationRadius(1600.f);
		LC->SetCastShadows(CL.bSpot);
		LC->SetupAttachment(RootComponent);
		LC->RegisterComponent();
		ShotComponents.Add(LC);
		LiveLights.Add(TPair<int32, TWeakObjectPtr<ULocalLightComponent>>(l, LC));
	}
}

void AGTAIVCutscenePlayer::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Cutscene)
	{
		return;
	}
	Time += DeltaSeconds;
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (PC && Time > 1.f && (PC->WasInputKeyJustPressed(EKeys::Enter) || PC->WasInputKeyJustPressed(EKeys::SpaceBar) ||
							 PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Bottom)))
	{
		Finish();
		return;
	}
	if (Time >= Cutscene->Duration)
	{
		Finish();
		return;
	}
	int32 Index = 0;
	for (int32 i = 0; i < Cutscene->Shots.Num(); ++i)
	{
		if (Cutscene->Shots[i].Start <= Time)
		{
			Index = i;
		}
	}
	if (Index != Shot)
	{
		BuildShot(Index);
	}
	const FGTAIVCutShot& S = Cutscene->Shots[Index];
	const float Fps = FMath::Max(1.f, Cutscene->Fps);
	const float Local = Time - S.Start;
	const float Frame = FMath::Clamp(Local * Fps, 0.f, float(FMath::Max(0, S.Frames - 1)));

	// the camera: position, forward, up, vertical field of view per frame
	const int32 NC = S.Camera.Num() / 10;
	if (Camera && NC > 0)
	{
		const int32 F0 = FMath::Clamp(FMath::FloorToInt(Frame), 0, NC - 1), F1 = FMath::Min(F0 + 1, NC - 1);
		auto V = [&](int32 F, int32 o) { return FVector(S.Camera[F * 10 + o], S.Camera[F * 10 + o + 1], S.Camera[F * 10 + o + 2]); };
		float T = Frame - F0;
		if (FVector::DistSquared(V(F0, 0), V(F1, 0)) > 150.f * 150.f)
		{
			T = 0.f;   // a camera cut
		}
		const FVector Pos = FMath::Lerp(V(F0, 0), V(F1, 0), T);
		const FVector Fwd = FMath::Lerp(V(F0, 3), V(F1, 3), T).GetSafeNormal();
		const FVector Up = FMath::Lerp(V(F0, 6), V(F1, 6), T).GetSafeNormal();
		const float VFov = FMath::Lerp(S.Camera[F0 * 10 + 9], S.Camera[F1 * 10 + 9], T);
		Camera->SetActorLocationAndRotation(Pos, FRotationMatrix::MakeFromXZ(Fwd, Up).Rotator());
		float Aspect = 16.f / 9.f;
		if (GEngine && GEngine->GameViewport)
		{
			FVector2D Size;
			GEngine->GameViewport->GetViewportSize(Size);
			if (Size.Y > 1.f)
			{
				Aspect = Size.X / Size.Y;
			}
		}
		const float HFov = FMath::RadiansToDegrees(2.f * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(VFov) * 0.5f) * Aspect));
		Camera->GetCameraComponent()->SetFieldOfView(FMath::Clamp(HFov, 5.f, 170.f));
	}
	for (const FActorComps& L : Live)
	{
		USceneComponent* R = L.Root.Get();
		if (!R)
		{
			continue;
		}
		R->SetWorldTransform(RootLerp(S.Actors[L.Actor].Root, Frame));
		if (USkeletalMeshComponent* K = L.Skel.Get())
		{
			if (S.Actors[L.Actor].Anim)
			{
				K->SetPosition(FMath::Min(Local, S.Actors[L.Actor].Anim->GetPlayLength()), false);
			}
		}
	}
	for (const auto& LL : LiveLights)
	{
		ULocalLightComponent* LC = LL.Value.Get();
		const TArray<float>& Tr = S.Lights[LL.Key].Track;
		const int32 NL = Tr.Num() / 6;
		if (!LC || NL == 0)
		{
			continue;
		}
		const int32 F = FMath::Clamp(FMath::RoundToInt(Frame), 0, NL - 1) * 6;
		const FVector Dir(Tr[F + 3], Tr[F + 4], Tr[F + 5]);
		LC->SetWorldLocationAndRotation(FVector(Tr[F], Tr[F + 1], Tr[F + 2]), Dir.IsNearlyZero() ? FRotator(-90.f, 0.f, 0.f) : Dir.Rotation());
	}
	if (AGTAIVPlayerHUD* H = HUD())
	{
		while (NextSubtitle < Cutscene->Subtitles.Num() && Cutscene->Subtitles[NextSubtitle].Start <= Time)
		{
			const FGTAIVSubtitle& Sub = Cutscene->Subtitles[NextSubtitle++];
			H->ShowSubtitle(Sub.Text, Sub.Duration);
		}
	}
}

void AGTAIVCutscenePlayer::Finish()
{
	TFunction<void()> Then = MoveTemp(OnFinished);
	Stop();
	if (Then)
	{
		Then();
	}
}

void AGTAIVCutscenePlayer::Stop()
{
	if (!Cutscene)
	{
		return;
	}
	Cutscene = nullptr;
	SetActorTickEnabled(false);
	ClearShot();
	if (Audio)
	{
		Audio->Stop();
		Audio->DestroyComponent();
		Audio = nullptr;
	}
	UWorld* W = GetWorld();
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	if (APawn* P = HiddenPawn.Get())
	{
		P->SetActorHiddenInGame(false);
		if (PC)
		{
			P->EnableInput(PC);
		}
	}
	for (const TWeakObjectPtr<AActor>& A : HiddenExtra)
	{
		if (AActor* X = A.Get())
		{
			X->SetActorHiddenInGame(false);
		}
	}
	HiddenExtra.Reset();
	if (PC)
	{
		PC->ResetIgnoreInputFlags();
		if (PC->GetPawn())
		{
			PC->SetViewTargetWithBlend(PC->GetPawn(), 0.f);
		}
	}
	HiddenPawn = nullptr;
	if (Camera)
	{
		Camera->Destroy();
		Camera = nullptr;
	}
	if (AGTAIVPlayerHUD* H = HUD())
	{
		H->SetCutsceneMode(false);
		H->ShowSubtitle(FString(), 0.f);
	}
	if (W)
	{
		for (TActorIterator<AGTAIVTrafficManager> It(W); It; ++It)
		{
			It->bTrafficEnabled = bTrafficWas;
		}
		for (TActorIterator<AGTAIVPedManager> It(W); It; ++It)
		{
			It->bPedsEnabled = bPedsWas;
		}
	}
}

void AGTAIVCutscenePlayer::EndPlay(const EEndPlayReason::Type Reason)
{
	OnFinished = nullptr;
	Stop();
	Super::EndPlay(Reason);
}

// ------------------------------------------------------------------ the mission

UGTAIVStory* UGTAIVStory::GetStory(UObject* WorldContext)
{
	UWorld* W = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	return W ? W->GetSubsystem<UGTAIVStory>() : nullptr;
}

void UGTAIVStory::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// Start New Game opens the level with ?story; not in multiplayer
	if (InWorld.URL.HasOption(TEXT("story")) && InWorld.GetNetMode() == NM_Standalone)
	{
		bPendingStart = true;
		bPendingSkipIntro = false;
		PendingDelay = 1.f;
	}
}

APawn* UGTAIVStory::PlayerPawn() const
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	return PC ? PC->GetPawn() : nullptr;
}

APawn* UGTAIVStory::PlayerOnFoot() const
{
	APawn* P = PlayerPawn();
	if (AGTAIVVehicle* V = Cast<AGTAIVVehicle>(P))
	{
		return V->GetDriver();
	}
	return P;
}

AGTAIVPlayerHUD* UGTAIVStory::HUD() const
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	return PC ? Cast<AGTAIVPlayerHUD>(PC->GetHUD()) : nullptr;
}

FString UGTAIVStory::Line(const TCHAR* Key, const TCHAR* Fallback) const
{
	const FString* S = Data ? Data->Text.Find(Key) : nullptr;
	return S && !S->IsEmpty() ? *S : FString(Fallback);
}

bool UGTAIVStory::IsInCutscene() const
{
	return Player && Player->IsPlaying();
}

void UGTAIVStory::SetState(EGTAIVMissionState S)
{
	State = S;
	StateTime = 0.f;
	UE_LOG(LogGTAIVStory, Log, TEXT("The Cousins Bellic: %s"), *UEnum::GetValueAsString(S));
}

void UGTAIVStory::StartMission(bool bSkipIntro)
{
	Data = UGTAIVStoryData::Load();
	if (!Data)
	{
		UE_LOG(LogGTAIVStory, Warning, TEXT("No story data (%s): export the map with Story mode on"), StoryDataPath);
		return;
	}
	ClearMission(false);
	if (!Player)
	{
		FActorSpawnParameters SP;
		SP.ObjectFlags |= RF_Transient;
		Player = GetWorld()->SpawnActor<AGTAIVCutscenePlayer>(AGTAIVCutscenePlayer::StaticClass(), FTransform::Identity, SP);
	}
	if (!bSkipIntro && Data->Intro)
	{
		SetState(EGTAIVMissionState::Intro);
		PlayCutscene(Data->Intro, [this]() { SetupGetIn(); });
	}
	else
	{
		SetupGetIn();
	}
}

void UGTAIVStory::SkipCutscene()
{
	if (Player && Player->IsPlaying())
	{
		Player->Skip();
	}
}

void UGTAIVStory::PlayCutscene(UGTAIVCutscene* C, TFunction<void()> Then)
{
	Fade = 0.f;
	if (AGTAIVPlayerHUD* H = HUD())
	{
		H->SetFade(0.f);
	}
	if (!Player)
	{
		Then();
		return;
	}
	Player->Play(C, MoveTemp(Then));
}

void UGTAIVStory::SpawnRoman()
{
	if (!Data || !Data->RomanMesh)
	{
		return;
	}
	UClass* Cls = LoadClass<AActor>(nullptr, PedClassPath, nullptr, LOAD_NoWarn | LOAD_Quiet);   // the ped Blueprint (a Character in GASP projects)
	const FTransform T = Data->RomanStart;
	// the ped actor stands at its capsule's centre; the mesh faces its +Y (actor +X)
	const FTransform At(FRotator(0.f, T.Rotator().Yaw + 90.f, 0.f), T.GetLocation() + FVector(0.f, 0.f, 95.f));
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	SP.ObjectFlags |= RF_Transient;
	AActor* R = Cls ? GetWorld()->SpawnActor<AActor>(Cls, At, SP) : nullptr;
	if (R)
	{
		// his own looks instead of a random everyday ped's
		TArray<USkeletalMeshComponent*> Meshes;
		R->GetComponents(Meshes);
		for (USkeletalMeshComponent* M : Meshes)
		{
			if (M->GetSkeletalMeshAsset() && M->GetSkeletalMeshAsset()->GetName().StartsWith(TEXT("SK_PED_")))
			{
				M->SetSkeletalMeshAsset(Data->RomanMesh);
			}
		}
	}
	else
	{
		// no ped Blueprint (not a Game Animation Sample project): Roman as a figure by the car
		USkeletalMeshComponent* K = nullptr;
		AActor* A = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), T, SP);
		if (A)
		{
			K = NewObject<USkeletalMeshComponent>(A);
			K->SetSkeletalMeshAsset(Data->RomanMesh);
			A->SetRootComponent(K);
			K->RegisterComponent();
			K->SetWorldTransform(T);
		}
		R = A;
	}
	Roman = R;
}

void UGTAIVStory::SetupGetIn()
{
	if (!Data)
	{
		return;
	}
	// the evening Niko arrives: dusk at the docks
	SetTimeOfDay(this, 20.8f);
	// everyone where the opening left them: Niko and Roman by Roman's car at the docks
	if (APawn* P = PlayerOnFoot())
	{
		if (AGTAIVVehicle* V = Cast<AGTAIVVehicle>(PlayerPawn()))
		{
			V->ExitDriver(true);
		}
		const FTransform T = Data->PlayerStart;
		P->SetActorLocationAndRotation(T.GetLocation() + FVector(0.f, 0.f, 95.f), FRotator(0.f, T.Rotator().Yaw + 90.f, 0.f),
									   false, nullptr, ETeleportType::TeleportPhysics);
		if (AController* C = P->GetController())
		{
			C->SetControlRotation(FRotator(0.f, T.Rotator().Yaw + 90.f, 0.f));
		}
	}
	if (!Car)
	{
		Car = AGTAIVVehicle::SpawnVehicle(this, Data->CarName, Data->CarStart);
		if (Car)
		{
			Car->PlaceBodyAt(Data->CarStart);
		}
	}
	if (!Roman && !bRomanInCar)
	{
		SpawnRoman();
	}
	if (AGTAIVPlayerHUD* H = HUD())
	{
		H->ShowSubtitle(Line(TEXT("get_in"), TEXT("~s~Get into ~b~Roman's car.")), 7.f);
		H->ClearWaypoint();
		H->SetRoute({}, FLinearColor::White);
		H->RemoveBlip(TEXT("story_dest"));
	}
	SetState(EGTAIVMissionState::GetIn);
}

void UGTAIVStory::RomanGetsIn()
{
	if (Roman)
	{
		Roman->Destroy();
		Roman = nullptr;
	}
	if (Car && Data && !bRomanInCar)
	{
		Car->SetPassenger(Data->RomanMesh);
		bRomanInCar = true;
	}
}

FVector UGTAIVStory::FindDestination() const
{
	// the street outside Roman's building: the nearest road node, towards his door
	const FVector Door = Data ? Data->Door : FVector::ZeroVector;
	const UGTAIVRoadNetwork* Net = nullptr;
	for (TActorIterator<AGTAIVTrafficManager> It(GetWorld()); It && !Net; ++It)
	{
		Net = It->Network;
	}
	FVector Best = Door;
	if (Net && Net->Nodes.Num())
	{
		float D2 = TNumericLimits<float>::Max();
		for (const FVector& N : Net->Nodes)
		{
			const float d = FVector::DistSquared2D(N, Door);
			if (d < D2)
			{
				D2 = d;
				Best = N;
			}
		}
		Best += (Door - Best).GetSafeNormal2D() * 350.f;   // pull in at the kerb
	}
	// onto the street from just above it: Hove Beach's elevated train runs over the road, and a trace
	// from high up lands the marker on its tracks
	FHitResult Hit;
	FCollisionQueryParams Params(TEXT("GTAIVStoryMarker"), false);
	if (GetWorld()->LineTraceSingleByChannel(Hit, Best + FVector(0, 0, 250.f), Best - FVector(0, 0, 600.f), ECC_Visibility, Params))
	{
		Best.Z = Hit.ImpactPoint.Z;
	}
	return Best;
}

void UGTAIVStory::UpdateRoute()
{
	AGTAIVPlayerHUD* H = HUD();
	const UGTAIVRoadNetwork* Net = nullptr;
	for (TActorIterator<AGTAIVTrafficManager> It(GetWorld()); It && !Net; ++It)
	{
		Net = It->Network;
	}
	APawn* P = PlayerPawn();
	if (!H || !Net || !P || Net->Nodes.Num() == 0)
	{
		return;
	}
	auto Nearest = [&](const FVector& X) {
		int32 B = 0;
		float D2 = TNumericLimits<float>::Max();
		for (int32 i = 0; i < Net->Nodes.Num(); ++i)
		{
			const float d = FVector::DistSquared2D(Net->Nodes[i], X);
			if (d < D2)
			{
				D2 = d;
				B = i;
			}
		}
		return B;
	};
	const int32 S = Nearest(P->GetActorLocation()), G = Nearest(Destination);
	// A* over the drivable links (both ways: GTA's one-way streets aside, the GPS shows the way)
	const int32 N = Net->Nodes.Num();
	TArray<float> Cost;
	Cost.Init(TNumericLimits<float>::Max(), N);
	TArray<int32> From;
	From.Init(INDEX_NONE, N);
	TArray<TPair<float, int32>> Open;
	Cost[S] = 0.f;
	Open.HeapPush(TPair<float, int32>(0.f, S), [](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; });
	int32 Steps = 0;
	while (Open.Num() && Steps++ < 20000)
	{
		TPair<float, int32> Top;
		Open.HeapPop(Top, [](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; });
		const int32 U = Top.Value;
		if (U == G)
		{
			break;
		}
		if (!Net->FirstLink.IsValidIndex(U))
		{
			continue;
		}
		for (int32 l = Net->FirstLink[U]; l < Net->FirstLink[U] + Net->LinkCount[U]; ++l)
		{
			const int32 V = Net->LinkTarget.IsValidIndex(l) ? Net->LinkTarget[l] : INDEX_NONE;
			if (!Net->Nodes.IsValidIndex(V))
			{
				continue;
			}
			const float C = Cost[U] + FVector::Dist(Net->Nodes[U], Net->Nodes[V]);
			if (C < Cost[V])
			{
				Cost[V] = C;
				From[V] = U;
				Open.HeapPush(TPair<float, int32>(C + FVector::Dist(Net->Nodes[V], Net->Nodes[G]), V),
							  [](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; });
			}
		}
	}
	TArray<FVector> Points;
	if (From[G] != INDEX_NONE || S == G)
	{
		for (int32 v = G; v != INDEX_NONE && Points.Num() < 4000; v = From[v])
		{
			Points.Insert(Net->Nodes[v], 0);
		}
	}
	Points.Insert(P->GetActorLocation(), 0);
	Points.Add(Destination);
	H->SetRoute(Points, FLinearColor(0.81f, 0.66f, 0.05f));   // the route in the objective's yellow
}

void UGTAIVStory::ClearMission(bool bKeepCar)
{
	if (Roman)
	{
		Roman->Destroy();
		Roman = nullptr;
	}
	if (Marker)
	{
		Marker->Destroy();
		Marker = nullptr;
	}
	if (Car && !bKeepCar)
	{
		if (Car->GetDriver())
		{
			Car->ExitDriver(true);
		}
		Car->Destroy();
		Car = nullptr;
	}
	bRomanInCar = false;
	if (AGTAIVPlayerHUD* H = HUD())
	{
		H->RemoveBlip(TEXT("story_car"));
		H->RemoveBlip(TEXT("story_dest"));
		H->SetRoute({}, FLinearColor::White);
		H->SetFade(0.f);
	}
}

void UGTAIVStory::Fail(const FString& Reason)
{
	ClearMission(false);
	if (AGTAIVPlayerHUD* H = HUD())
	{
		H->ShowBigMessage(Line(TEXT("failed"), TEXT("MISSION FAILED")), 5.f, FLinearColor(0.6f, 0.27f, 0.27f));
		H->ShowSubtitle(Reason, 5.f);
	}
	SetState(EGTAIVMissionState::Failed);
}

void UGTAIVStory::DebugWarpToDestination()
{
	if (!Car || !Data)
	{
		return;
	}
	if (Destination.IsZero())
	{
		Destination = FindDestination();
	}
	Car->SetActorLocation(Destination + FVector(0.f, 0.f, 120.f), false, nullptr, ETeleportType::TeleportPhysics);
}

FString UGTAIVStory::GetDebugState() const
{
	return FString::Printf(TEXT("state=%s t=%.1f cutscene=%d time=%.1f car=%s roman=%s inCar=%d dest=%s"),
						   *UEnum::GetValueAsString(State), StateTime, IsInCutscene(), Player ? Player->GetTime() : 0.f,
						   *GetNameSafe(Car), *GetNameSafe(Roman), bRomanInCar, *Destination.ToString());
}

void UGTAIVStory::Tick(float DeltaTime)
{
	if (bPendingStart)
	{
		PendingDelay -= DeltaTime;
		if (PendingDelay <= 0.f)
		{
			bPendingStart = false;
			StartMission(bPendingSkipIntro);
		}
		return;
	}
	StateTime += DeltaTime;
	AGTAIVPlayerHUD* H = HUD();
	APawn* P = PlayerPawn();
	switch (State)
	{
	case EGTAIVMissionState::GetIn:
	{
		if (!Car)
		{
			Fail(Line(TEXT("trashed"), TEXT("~r~Roman's car has been trashed.~s~")));
			break;
		}
		// got out and walked up to Roman's door (with Roman along): that's arriving too
		if (bRomanInCar && P && Data && !Destination.IsZero() &&
			(FVector::Dist2D(P->GetActorLocation(), Destination) < 1000.f || FVector::Dist2D(P->GetActorLocation(), Data->Door) < 1200.f))
		{
			if (H)
			{
				H->RemoveBlip(TEXT("story_car"));
				H->RemoveBlip(TEXT("story_dest"));
				H->SetRoute({}, FLinearColor::White);
			}
			SetState(EGTAIVMissionState::Arrived);
			break;
		}
		if (H)
		{
			H->SetObjectiveBlip(TEXT("story_car"), Car->GetActorLocation(), FLinearColor(0.29f, 0.58f, 0.63f));
		}
		if (P == Car)
		{
			// Niko's at the wheel: Roman gets in beside him
			RomanGetsIn();
			if (Destination.IsZero())
			{
				Destination = FindDestination();
			}
			if (!Marker)
			{
				FActorSpawnParameters SP;
				SP.ObjectFlags |= RF_Transient;
				AStaticMeshActor* M = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(Destination), SP);
				if (M)
				{
					M->SetMobility(EComponentMobility::Movable);
					UStaticMeshComponent* C = M->GetStaticMeshComponent();
					C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
					C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					C->SetWorldScale3D(FVector(3.f, 3.f, 0.08f));
					if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
					{
						UMaterialInstanceDynamic* MI = UMaterialInstanceDynamic::Create(Base, M);
						MI->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.9f, 0.7f, 0.05f));
						C->SetMaterial(0, MI);
					}
					Marker = M;
				}
			}
			if (H)
			{
				H->RemoveBlip(TEXT("story_car"));
				H->SetObjectiveBlip(TEXT("story_dest"), Destination, FLinearColor(0.81f, 0.66f, 0.05f));
				H->ShowSubtitle(Line(TEXT("drive"), TEXT("~s~Drive to ~y~Roman's place.")), 7.f);
				H->ShowHelpText(Line(TEXT("radar"), TEXT("The yellow icon on the radar shows the location of your next objective.")), 8.f);
			}
			NextRoute = 0.0;
			SetState(EGTAIVMissionState::Drive);
		}
		break;
	}
	case EGTAIVMissionState::Drive:
	{
		if (!Car)
		{
			Fail(Line(TEXT("trashed"), TEXT("~r~Roman's car has been trashed.~s~")));
			break;
		}
		if (P != Car)
		{
			// got out: back in the car (Roman waits in it)
			if (H)
			{
				H->ShowSubtitle(Line(TEXT("get_in"), TEXT("~s~Get into ~b~Roman's car.")), 7.f);
				H->SetRoute({}, FLinearColor::White);
			}
			AbandonTime = 0.f;
			SetState(EGTAIVMissionState::GetIn);
			break;
		}
		const double Now = GetWorld()->GetRealTimeSeconds();
		if (Now >= NextRoute)
		{
			NextRoute = Now + 1.5;
			UpdateRoute();
		}
		if (StateTime > 9.f && StateTime < 9.f + DeltaTime * 1.5f && H)
		{
			H->ShowHelpText(Line(TEXT("route"), TEXT("While in a vehicle the colored line on the radar shows the route to your objective.")), 8.f);
		}
		const bool bAtDoor = Data && FVector::Dist2D(Car->GetActorLocation(), Data->Door) < 1500.f;
		if ((FVector::Dist2D(Car->GetActorLocation(), Destination) < 1000.f || bAtDoor) && Car->GetSpeedKmh() < 25.f)
		{
			// pulled up outside: brake, fade out, Roman's apartment
			Car->bExternalInput = true;
			Car->ThrottleInput = 0.f;
			Car->bHandbrake = true;
			if (H)
			{
				H->RemoveBlip(TEXT("story_dest"));
				H->SetRoute({}, FLinearColor::White);
			}
			SetState(EGTAIVMissionState::Arrived);
		}
		break;
	}
	case EGTAIVMissionState::Arrived:
	{
		Fade = FMath::Clamp(StateTime / 1.f, 0.f, 1.f);
		if (H)
		{
			H->SetFade(Fade);
		}
		if (StateTime > 1.2f)
		{
			if (Marker)
			{
				Marker->Destroy();
				Marker = nullptr;
			}
			if (Car)
			{
				Car->ClearPassenger();
				Car->bExternalInput = false;
				Car->bHandbrake = false;
				if (Car->GetDriver())
				{
					Car->ExitDriver(true);
				}
			}
			bRomanInCar = false;
			SetState(EGTAIVMissionState::End);
			PlayCutscene(Data ? Data->End.Get() : nullptr, [this]() {
				// after the scene Niko is in Roman's apartment
				if (APawn* Me = PlayerOnFoot())
				{
					Me->SetActorLocation(Data->Door + FVector(0.f, 0.f, 95.f), false, nullptr, ETeleportType::TeleportPhysics);
				}
				if (AGTAIVPlayerHUD* H2 = HUD())
				{
					H2->ShowBigMessage(Line(TEXT("passed"), TEXT("MISSION PASSED")), 6.f, FLinearColor(0.85f, 0.68f, 0.22f));
				}
				SetState(EGTAIVMissionState::Passed);
			});
		}
		break;
	}
	case EGTAIVMissionState::Passed:
		if (StateTime > 7.f)
		{
			SetState(EGTAIVMissionState::Done);
		}
		break;
	case EGTAIVMissionState::Failed:
		if (StateTime > 6.f)
		{
			StartMission(true);   // try again from getting in the car
		}
		break;
	case EGTAIVMissionState::Done:
		State = EGTAIVMissionState::Idle;
		break;
	default:
		break;
	}
	// leaving Roman behind
	if ((State == EGTAIVMissionState::GetIn || State == EGTAIVMissionState::Drive) && Car && P)
	{
		const bool bFar = FVector::Dist(P->GetActorLocation(), Car->GetActorLocation()) > 15000.f;
		AbandonTime = bFar ? AbandonTime + DeltaTime : 0.f;
		if (AbandonTime > 4.f)
		{
			Fail(Line(TEXT("abandoned"), TEXT("~r~You have abandoned Roman.~s~")));
		}
	}
}
