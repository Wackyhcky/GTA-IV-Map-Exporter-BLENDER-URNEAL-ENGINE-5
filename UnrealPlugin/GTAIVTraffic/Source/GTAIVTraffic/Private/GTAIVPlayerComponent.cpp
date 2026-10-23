#include "GTAIVPlayerComponent.h"

#include "Components/CapsuleComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/DamageType.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "UObject/UnrealType.h"
#include "EngineUtils.h"
#include "GTAIVPlayerHUD.h"
#include "GTAIVVehicle.h"
#include "GTAIVVehicleAnims.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
#include "EngineUtils.h"
#include "GTAIVTrafficCar.h"
#include "Components/BoxComponent.h"
#include "Sound/SoundAttenuation.h"

TArray<UGTAIVPlayerComponent::FPendingPush> UGTAIVPlayerComponent::Pending;

namespace
{
	const FName DeadTag(TEXT("GTAIV_Dead"));

	/** Ragdolling = the hips are simulated (GASP's ragdoll character keeps some limbs on
	 *  physics control all the time). */
	bool IsRagdolling(const USkeletalMeshComponent* Mesh)
	{
		if (!Mesh)
		{
			return false;
		}
		for (const TCHAR* Bone : {TEXT("pelvis"), TEXT("Char_Pelvis"), TEXT("Pelvis"), TEXT("hips")})
		{
			if (Mesh->GetBoneIndex(Bone) != INDEX_NONE)
			{
				return Mesh->IsSimulatingPhysics(Bone);
			}
		}
		return Mesh->IsSimulatingPhysics(NAME_None);
	}

	bool IsTwistOrFace(const FString& Bone)
	{
		return Bone.Contains(TEXT("Roll")) || Bone.Contains(TEXT("Twist")) || Bone.StartsWith(TEXT("FB_")) ||
			   Bone.StartsWith(TEXT("PointFB_"));
	}
}

UGTAIVPlayerComponent::UGTAIVPlayerComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;  // after the characters have animated
	SetIsReplicatedByDefault(true);
	PlayerMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TEXT("/Game/GTAIV/Characters/SK_PED_Niko.SK_PED_Niko")));
	PedVisualClass = TSoftClassPtr<AActor>(FSoftObjectPath(TEXT("/Game/GTAIV/Characters/BP_GTAIV_Ped.BP_GTAIV_Ped_C")));
}

void UGTAIVPlayerComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UGTAIVPlayerComponent, ModelMesh);
	DOREPLIFETIME_CONDITION(UGTAIVPlayerComponent, bArmed, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(UGTAIVPlayerComponent, bAimNet, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(UGTAIVPlayerComponent, AimPointNet, COND_SkipOwner);
	DOREPLIFETIME(UGTAIVPlayerComponent, Health);
	DOREPLIFETIME(UGTAIVPlayerComponent, bWastedNet);
	DOREPLIFETIME(UGTAIVPlayerComponent, SeatVehicleNet);
	DOREPLIFETIME(UGTAIVPlayerComponent, SeatPhaseNet);
	DOREPLIFETIME(UGTAIVPlayerComponent, bJackNet);
}

void UGTAIVPlayerComponent::OnRep_Model()
{
	DressTimer = 0.f;  // dress right away
}

void UGTAIVPlayerComponent::OnRep_Armed()
{
	// someone else drew / put away their gun: play it here too
	if (bArmed)
	{
		Action = EAction::Draw;
		ActionTime = 0.f;
	}
	else if (Action != EAction::Holster)
	{
		bArmed = true;  // holster plays, then puts it away
		Action = EAction::Holster;
		ActionTime = 0.f;
	}
}

void UGTAIVPlayerComponent::OnRep_Wasted()
{
	if (bWastedNet && !bWasted)
	{
		bWasted = true;
		WastedTime = 0.f;
		bAiming = false;
		DropWeapon();
	}
	else if (!bWastedNet)
	{
		bWasted = false;
	}
}

void UGTAIVPlayerComponent::OnRep_Seat()
{
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		return;
	}
	AGTAIVVehicle* Car = SeatVehicleNet;
	if (Car != SeatVehicle.Get())
	{
		SetSeat(Car, bJackNet);
	}
	if (Car && SeatPhaseNet != uint8(SeatPhase))
	{
		SeatPhase = ESeatPhase(SeatPhaseNet);
		PhaseTime = 0.f;
		bDoorSound = false;
		if (SeatPhase == ESeatPhase::Seated)
		{
			SeatTime = 0.f;
		}
	}
}

void UGTAIVPlayerComponent::PublishSeat()
{
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		SeatVehicleNet = SeatVehicle.Get();
		SeatPhaseNet = uint8(SeatPhase);
		bJackNet = bJackPending;
	}
}

void UGTAIVPlayerComponent::AssignModel()
{
	// the server picks how each player looks, and keeps it across respawns
	APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Pawn || !Pawn->HasAuthority() || ModelMesh || !Pawn->IsPlayerControlled())
	{
		return;
	}
	static TMap<int32, TWeakObjectPtr<USkeletalMesh>> Chosen;
	const APlayerState* PS = Pawn->GetPlayerState();
	const int32 Id = PS ? PS->GetPlayerId() : 0;
	if (const TWeakObjectPtr<USkeletalMesh>* Was = Chosen.Find(Id))
	{
		ModelMesh = Was->Get();
	}
	if (!ModelMesh)
	{
		const bool bHost = Pawn->IsLocallyControlled() || GetWorld()->GetNetMode() == NM_Standalone;
		ModelMesh = bHost ? PlayerMesh.LoadSynchronous() : UGTAIVVehicleAnims::RandomPedMesh();
		if (!ModelMesh)
		{
			ModelMesh = PlayerMesh.LoadSynchronous();
		}
		Chosen.Add(Id, ModelMesh);
	}
	DressTimer = 0.f;
}

bool UGTAIVPlayerComponent::IsOwningClient() const
{
	// an RPC to the server: only from the client that owns this character (never on the server,
	// where an unreplicated component would run it locally and loop)
	return GetOwner() && GetOwner()->GetLocalRole() == ROLE_AutonomousProxy && GetIsReplicated();
}

void UGTAIVPlayerComponent::ServerDraw_Implementation()
{
	if (GetOwner()->HasAuthority())
	{
		DrawWeapon();
	}
}
void UGTAIVPlayerComponent::ServerHolster_Implementation() { if (GetOwner()->HasAuthority()) HolsterWeapon(); }
void UGTAIVPlayerComponent::ServerReload_Implementation() { if (GetOwner()->HasAuthority()) Reload(); }

void UGTAIVPlayerComponent::ServerSetAim_Implementation(bool bAim, FVector_NetQuantize Point)
{
	bAimNet = bAim && bArmed;
	AimPointNet = Point;
	if (!IsLocalPlayer())
	{
		bAiming = bAimNet;
	}
}

void UGTAIVPlayerComponent::ServerFire_Implementation(FVector_NetQuantize Start, FVector_NetQuantizeNormal Dir)
{
	if (GetOwner()->HasAuthority() && !bWasted && Weapon)
	{
		ShotTrace(Start, Dir);
	}
}

void UGTAIVPlayerComponent::ServerEnterVehicle_Implementation()
{
	if (GetOwner()->HasAuthority() && GetWorld()->GetTimeSeconds() >= VehicleCooldownUntil)
	{
		EnterNearestVehicle();
	}
}

void UGTAIVPlayerComponent::ServerSpawnVehicle_Implementation(int32 Step)
{
	if (GetOwner()->HasAuthority())
	{
		SpawnVehicleInFront(Step);
	}
}

void UGTAIVPlayerComponent::MulticastShot_Implementation(FVector_NetQuantize Impact, bool bHit)
{
	if (IsLocalPlayer() || !Weapon)
	{
		return;  // the shooter played it already
	}
	Recoil = 1.f;
	FlashTime = 0.05f;
	PlaySound(Weapon->ShotSound);
	PlaySound(Weapon->ShotLowSound, 0.8f);
	PlaySound(Weapon->EchoSound, 0.6f);
	if (bHit && Weapon->ImpactSounds.Num() > 0)
	{
		UGameplayStatics::PlaySoundAtLocation(this, Weapon->ImpactSounds[FMath::RandRange(0, Weapon->ImpactSounds.Num() - 1)], Impact, 0.7f);
	}
}

void UGTAIVPlayerComponent::MulticastRagdoll_Implementation(AActor* Actor, FVector_NetQuantize Impulse, FVector_NetQuantize Location)
{
	if (GetWorld()->GetNetMode() != NM_Client)
	{
		return;  // the server did it already
	}
	const ENetMode Mode = GetWorld()->GetNetMode();
	(void)Mode;
	if (Actor)
	{
		bLocalRagdoll = true;
		RagdollActor(Actor, Impulse, Location);
		bLocalRagdoll = false;
	}
}

void UGTAIVPlayerComponent::MulticastPain_Implementation(bool bDeath)
{
	if (const UGTAIVVoiceData* V = LoadObject<UGTAIVVoiceData>(nullptr, TEXT("/Game/GTAIV/Characters/Voices/DA_GTAIV_Voices.DA_GTAIV_Voices")))
	{
		const TArray<TObjectPtr<USoundBase>>& Lines = bDeath ? V->PainMale.DeathHigh : V->PainMale.PainHigh;
		if (Lines.Num())
		{
			UGameplayStatics::PlaySoundAtLocation(this, Lines[FMath::RandRange(0, Lines.Num() - 1)], GetOwner()->GetActorLocation());
		}
	}
}

void UGTAIVPlayerComponent::BeginPlay()
{
	Super::BeginPlay();
	// added to the character Blueprint before multiplayer: its template doesn't replicate
	if (!GetIsReplicated())
	{
		SetIsReplicated(true);
	}
	if (!Weapon)
	{
		Weapon = LoadObject<UGTAIVWeaponData>(nullptr, TEXT("/Game/GTAIV/Characters/Weapons/DA_GTAIV_Pistol.DA_GTAIV_Pistol"));
	}
	if (Weapon)
	{
		ClipAmmo = Weapon->ClipSize;
		SpareAmmo = FMath::Max(0, Weapon->StartAmmo - Weapon->ClipSize);
	}
	if (bStartArmed)
	{
		bArmed = true;
		ArmedAlpha = 1.f;
	}
	DressTimer = 0.f;
}

void UGTAIVPlayerComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	TeardownVisual();
	Pending.Reset();
	Super::EndPlay(Reason);
}

// ------------------------------------------------------------------ helpers

USkeletalMeshComponent* UGTAIVPlayerComponent::OwnerBodyMesh() const
{
	// the character's own (mannequin) mesh: the visual override copies its pose
	AActor* Owner = GetOwner();
	if (const ACharacter* C = Cast<ACharacter>(Owner))
	{
		return C->GetMesh();
	}
	TArray<USkeletalMeshComponent*> Meshes;
	Owner->GetComponents(Meshes, false);
	for (USkeletalMeshComponent* M : Meshes)
	{
		if (M->GetOwner() == Owner && M->GetSkeletalMeshAsset())
		{
			return M;
		}
	}
	return nullptr;
}

bool UGTAIVPlayerComponent::IsLocalPlayer() const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	return Pawn && Pawn->IsLocallyControlled() && Cast<APlayerController>(Pawn->GetController());
}

bool UGTAIVPlayerComponent::OwnerRagdolling() const
{
	// Mover characters (GASP): the movement mode says it; their bodies are partly simulated
	// all the time (physics control), so the bodies alone can't tell
	for (UActorComponent* C : GetOwner()->GetComponents())
	{
		if (!C || !C->GetClass()->GetName().Contains(TEXT("MoverComponent")))
		{
			continue;
		}
		if (UFunction* F = C->FindFunction(TEXT("GetMovementModeName")))
		{
			if (F->ParmsSize >= sizeof(FName) && F->NumParms == 1)
			{
				uint8* Parms = static_cast<uint8*>(FMemory_Alloca(F->ParmsSize));
				FMemory::Memzero(Parms, F->ParmsSize);
				C->ProcessEvent(F, Parms);
				if (const FNameProperty* Ret = CastField<FNameProperty>(F->GetReturnProperty()))
				{
					return Ret->GetPropertyValue_InContainer(Parms).ToString().Contains(TEXT("Ragdoll"));
				}
			}
		}
	}
	return IsRagdolling(OwnerBodyMesh());
}

FString UGTAIVPlayerComponent::GetDebugState() const
{
	FString Mode;
	for (UActorComponent* C : GetOwner()->GetComponents())
	{
		if (C && C->GetClass()->GetName().Contains(TEXT("MoverComponent")))
		{
			if (UFunction* F = C->FindFunction(TEXT("GetMovementModeName")))
			{
				uint8* Parms = static_cast<uint8*>(FMemory_Alloca(F->ParmsSize));
				FMemory::Memzero(Parms, F->ParmsSize);
				C->ProcessEvent(F, Parms);
				if (const FNameProperty* Ret = CastField<FNameProperty>(F->GetReturnProperty()))
				{
					Mode = Ret->GetPropertyValue_InContainer(Parms).ToString();
				}
			}
		}
	}
	FString Hand;
	if (Poseable && bHandTarget)
	{
		const FTransform AT = GetOwner()->GetActorTransform();
		const FVector T = AT.InverseTransformPosition(HandTarget);
		const FVector H = AT.InverseTransformPosition(Poseable->GetBoneLocation(bHandLeft ? FName(TEXT("Char_L_Hand")) : FName(TEXT("Char_R_Hand"))));
		Hand = FString::Printf(TEXT(" hand=%s a=%.2f target(%.0f,%.0f,%.0f) at(%.0f,%.0f,%.0f)"), bHandLeft ? TEXT("L") : TEXT("R"), HandAlpha,
							   T.X, T.Y, T.Z, H.X, H.Y, H.Z);
	}
	const TCHAR* Phases[] = {TEXT("none"), TEXT("open_door"), TEXT("jack"), TEXT("get_in"), TEXT("close_in"), TEXT("seated"),
							 TEXT("get_out"), TEXT("close_out"), TEXT("jump_out")};
	return FString::Printf(TEXT("armed=%d aiming=%d aim=%.2f action=%d ragdoll=%d mode=%s pose=%d clip=%d hit=%s%s seat=%s %.2fs net=role%d/rep%d/local%d/owning%d wasted=%d hp=%.0f respawn=%.1f now=%.1f ctrl=%s"),
						   bArmed, bAiming, AimAlpha, int32(Action), OwnerRagdolling(), *Mode, Poseable != nullptr,
						   ClipAmmo, *LastHit, *Hand, Phases[int32(SeatPhase)], PhaseTime, GetOwner() ? int32(GetOwner()->GetLocalRole()) : -1,
						   GetIsReplicated(), IsLocalPlayer(), IsOwningClient(), bWasted, Health, RespawnAt, GetWorld()->GetTimeSeconds(),
						   *GetNameSafe(Cast<APawn>(GetOwner()) ? Cast<APawn>(GetOwner())->GetController() : nullptr));
}

bool UGTAIVPlayerComponent::ReadGaspAim(bool& bOutAim) const
{
	// GASP keeps WantsToAim in PlayerInputState (Mover) / CharacterInputState (CMC): Blueprint
	// structs whose member names carry GUID suffixes, so match them by prefix
	const AActor* Owner = GetOwner();
	for (TFieldIterator<FStructProperty> It(Owner->GetClass()); It; ++It)
	{
		const FString N = It->GetName();
		if (!N.StartsWith(TEXT("PlayerInputState")) && !N.StartsWith(TEXT("CharacterInputState")))
		{
			continue;
		}
		const void* Data = It->ContainerPtrToValuePtr<void>(Owner);
		for (TFieldIterator<FBoolProperty> B(It->Struct); B; ++B)
		{
			if (B->GetName().StartsWith(TEXT("WantsToAim")))
			{
				bOutAim = B->GetPropertyValue_InContainer(Data);
				return true;
			}
		}
	}
	return false;
}

void UGTAIVPlayerComponent::PlaySound(USoundBase* Sound, float Volume) const
{
	if (Sound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, Sound, MuzzleTransform().GetLocation(), Volume);
	}
}

FTransform UGTAIVPlayerComponent::MuzzleTransform() const
{
	if (Gun && Weapon)
	{
		return FTransform(Weapon->Muzzle) * Gun->GetComponentTransform();
	}
	return GetOwner()->GetActorTransform();
}

// ------------------------------------------------------------------ Niko

void UGTAIVPlayerComponent::Dress()
{
	AActor* Owner = GetOwner();
	UClass* PedClass = PedVisualClass.LoadSynchronous();
	const bool bNiko = bDressAsNiko && ModelMesh != nullptr;  // players (their model); NPCs keep their own look
	if (bNiko && PedClass)
	{
		// GASP's visual override is a child actor; its manager component swaps it. Point the
		// manager at the GTA IV ped and let it apply the override (hiding the mannequin).
		TArray<UChildActorComponent*> ChildComps;
		Owner->GetComponents(ChildComps);
		UChildActorComponent* Override = nullptr;
		for (UChildActorComponent* C : ChildComps)
		{
			if (C->GetName().Contains(TEXT("VisualOverride")) || C->ComponentHasTag(TEXT("VisualOverride")))
			{
				Override = C;
				break;
			}
		}
		if (Override && Override->GetChildActorClass() != PedClass)
		{
			bool bApplied = false;
			for (UActorComponent* C : Owner->GetComponents())
			{
				if (!C || !C->GetClass()->GetName().StartsWith(TEXT("AC_VisualOverrideManager")))
				{
					continue;
				}
				if (FSoftClassProperty* P = FindFProperty<FSoftClassProperty>(C->GetClass(), TEXT("VisualOverride")))
				{
					P->SetPropertyValue_InContainer(C, FSoftObjectPtr(PedClass));
					if (UFunction* F = C->FindFunction(TEXT("ApplyVisualOverride")))
					{
						if (F->NumParms == 0)
						{
							C->ProcessEvent(F, nullptr);
							bApplied = Override->GetChildActorClass() == PedClass;
						}
					}
				}
			}
			if (!bApplied)
			{
				Override->SetChildActorClass(PedClass);
				if (USkeletalMeshComponent* Body = OwnerBodyMesh())
				{
					Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
					Body->SetVisibility(false, false);
				}
			}
		}
	}
	// the GTA ped visual (ours or a GASP override using it): Niko's mesh, and our pose layer
	AActor* Ped = nullptr;
	TArray<UChildActorComponent*> ChildComps;
	Owner->GetComponents(ChildComps);
	for (UChildActorComponent* C : ChildComps)
	{
		AActor* Child = C->GetChildActor();
		if (Child && Child->GetClass()->GetName().Contains(TEXT("GTAIV_Ped")))
		{
			Ped = Child;
			break;
		}
	}
	if (Ped != PedActor.Get())
	{
		TeardownVisual();
		if (Ped)
		{
			SetupVisual(Ped);
		}
	}
	if (Ped && bNiko && Source.IsValid())
	{
		if (USkeletalMesh* Niko = ModelMesh.Get())
		{
			if (Source->GetSkeletalMeshAsset() != Niko)
			{
				Source->SetSkeletalMeshAsset(Niko);
			}
			if (Poseable && Poseable->GetSkinnedAsset() != Niko)
			{
				Poseable->SetSkinnedAssetAndUpdate(Niko);
				Poseable->EmptyOverrideMaterials();
			}
		}
	}
}

void UGTAIVPlayerComponent::SetupVisual(AActor* Ped)
{
	USkeletalMeshComponent* Mesh = Ped->FindComponentByClass<USkeletalMeshComponent>();
	if (!Mesh)
	{
		return;
	}
	PedActor = Ped;
	Source = Mesh;
	// The ped's mesh keeps animating (retargeted from GASP) but is drawn by a poseable copy, so
	// GTA IV's weapon animations can be layered on its upper body afterwards.
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Poseable = NewObject<UPoseableMeshComponent>(Ped, TEXT("GTAIV_PlayerPose"));
	Poseable->SetSkinnedAssetAndUpdate(Mesh->GetSkeletalMeshAsset());
	for (int32 i = 0; i < Mesh->GetNumOverrideMaterials(); ++i)
	{
		Poseable->SetMaterial(i, Mesh->OverrideMaterials[i]);
	}
	Poseable->SetupAttachment(Mesh);
	Poseable->RegisterComponent();
	Poseable->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Poseable->SetCastShadow(true);
	Mesh->SetHiddenInGame(true, false);
	AddTickPrerequisiteComponent(Mesh);

	Gun = NewObject<UStaticMeshComponent>(Ped, TEXT("GTAIV_Weapon"));
	Gun->SetStaticMesh(Weapon ? Weapon->Mesh.Get() : nullptr);
	Gun->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Gun->RegisterComponent();
	Gun->AttachToComponent(Poseable, FAttachmentTransformRules::SnapToTargetNotIncludingScale,
						   Weapon ? Weapon->HandBone : FName(TEXT("Char_R_Hand")));
	if (Weapon)
	{
		Gun->SetRelativeTransform(Weapon->Grip);
	}
	Gun->SetVisibility(false);

	Flash = NewObject<UPointLightComponent>(Ped, TEXT("GTAIV_MuzzleFlash"));
	Flash->RegisterComponent();
	Flash->AttachToComponent(Gun, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	Flash->SetRelativeLocation(Weapon ? Weapon->Muzzle : FVector::ZeroVector);
	Flash->SetLightColor(FLinearColor(1.f, 0.75f, 0.4f));
	Flash->SetAttenuationRadius(600.f);
	Flash->SetIntensity(0.f);
	Flash->SetCastShadows(false);
}

void UGTAIVPlayerComponent::TeardownVisual()
{
	if (Source.IsValid())
	{
		Source->SetHiddenInGame(false, false);
		RemoveTickPrerequisiteComponent(Source.Get());
	}
	for (USceneComponent* C : {static_cast<USceneComponent*>(Flash.Get()), static_cast<USceneComponent*>(Gun.Get()),
							   static_cast<USceneComponent*>(Poseable.Get())})
	{
		if (C)
		{
			C->DestroyComponent();
		}
	}
	Flash = nullptr;
	Gun = nullptr;
	Poseable = nullptr;
	Source = nullptr;
	PedActor = nullptr;
}

// ------------------------------------------------------------------ weapon

void UGTAIVPlayerComponent::DrawWeapon()
{
	if (IsOwningClient())
	{
		ServerDraw();
	}
	if (!bArmed && Weapon)
	{
		bArmed = true;
		Action = EAction::Draw;
		ActionTime = 0.f;
	}
}

void UGTAIVPlayerComponent::HolsterWeapon()
{
	if (IsOwningClient())
	{
		ServerHolster();
	}
	if (bArmed && Action != EAction::Holster)
	{
		Action = EAction::Holster;
		ActionTime = 0.f;
	}
}

void UGTAIVPlayerComponent::Reload()
{
	if (!bArmed || !Weapon || Action != EAction::None || SpareAmmo <= 0 || ClipAmmo >= Weapon->ClipSize)
	{
		return;
	}
	if (IsOwningClient())
	{
		ServerReload();
	}
	Action = EAction::Reload;
	ActionTime = 0.f;
	bReloadSoundPlayed = false;
	bCockSoundPlayed = false;
}

void UGTAIVPlayerComponent::Fire()
{
	if (!bArmed || !Weapon || Action != EAction::None || !bAiming || AimAlpha < 0.6f || FireCooldown > 0.f)
	{
		return;
	}
	FireCooldown = Weapon->FireInterval;
	if (ClipAmmo <= 0)
	{
		PlaySound(Weapon->DrySound);
		Reload();
		return;
	}
	--ClipAmmo;
	Recoil = 1.f;
	FlashTime = 0.05f;
	PlaySound(Weapon->ShotSound);
	PlaySound(Weapon->ShotLowSound, 0.8f);
	PlaySound(Weapon->EchoSound, 0.6f);

	// the shot goes where the camera looks (the crosshair), from the camera
	APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	FVector Start;
	FRotator Rot;
	if (PC)
	{
		PC->GetPlayerViewPoint(Start, Rot);
	}
	else
	{
		Start = MuzzleTransform().GetLocation();
		Rot = GetOwner()->GetActorRotation();
	}
	const FVector Dir = Rot.Vector();
	// skip past whatever is between the camera and the character
	Start += Dir * FMath::Max(0.f, FVector::DotProduct(GetOwner()->GetActorLocation() - Start, Dir));
	if (IsOwningClient())
	{
		ServerFire(Start, Dir);  // the server decides what it hit
	}
	else if (GetOwner()->HasAuthority())
	{
		ShotTrace(Start, Dir);
	}
}

void UGTAIVPlayerComponent::ShotTrace(const FVector& Start, const FVector& Dir)
{
	ScanThreats(true);  // gunshots scare everyone around
	APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	FCollisionQueryParams Params(TEXT("GTAIVShot"), false, GetOwner());
	if (PedActor.IsValid())
	{
		Params.AddIgnoredActor(PedActor.Get());
	}
	FCollisionObjectQueryParams Objects;
	for (ECollisionChannel C : {ECC_WorldStatic, ECC_WorldDynamic, ECC_Pawn, ECC_PhysicsBody, ECC_Vehicle, ECC_Destructible})
	{
		Objects.AddObjectTypesToQuery(C);
	}
	FHitResult Hit;
	if (!GetWorld()->LineTraceSingleByObjectType(Hit, Start, Start + Dir * Weapon->Range, Objects, Params))
	{
		LastHit = TEXT("nothing");
		MulticastShot(Start + Dir * Weapon->Range, false);
		return;
	}
	MulticastShot(Hit.ImpactPoint, true);
	LastHit = FString::Printf(TEXT("%s/%s/%s"), *GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetComponent()), *Hit.BoneName.ToString());
	if (Weapon->ImpactSounds.Num() > 0 && IsLocalPlayer())
	{
		UGameplayStatics::PlaySoundAtLocation(this, Weapon->ImpactSounds[FMath::RandRange(0, Weapon->ImpactSounds.Num() - 1)],
											  Hit.ImpactPoint, 0.7f);
	}
	AActor* Target = Hit.GetActor();
	if (!Target)
	{
		return;
	}
	UGameplayStatics::ApplyPointDamage(Target, Weapon->Damage, Dir, Hit, PC, GetOwner(), UDamageType::StaticClass());
	// another player: loses health (players don't get the NPCs' wounds)
	UGTAIVPlayerComponent* Other = Target->FindComponentByClass<UGTAIVPlayerComponent>();
	if (Other && Cast<APawn>(Target) && Cast<APawn>(Target)->IsPlayerControlled())
	{
		Other->TakeHit(Weapon->Damage * 1.4f, Start);
		return;
	}
	if (Cast<APawn>(Target))
	{
		// bleeds, struggles to get up, dies (and ragdolls)
		UGTAIVWoundComponent::ApplyHit(Target, Hit, Dir, Weapon, GetOwner());
	}
	else if (UPrimitiveComponent* Prim = Hit.GetComponent())
	{
		if (Prim->IsSimulatingPhysics(Hit.BoneName))
		{
			Prim->AddImpulseAtLocation(Dir * Weapon->Impulse * 0.5f, Hit.ImpactPoint, Hit.BoneName);
		}
	}
}

bool UGTAIVPlayerComponent::bLocalRagdoll = false;

void UGTAIVPlayerComponent::RagdollActor(AActor* Actor, FVector Impulse, FVector Location)
{
	if (!Actor)
	{
		return;
	}
	// a networked game: the server tells every client (through any player's component)
	UWorld* W = Actor->GetWorld();
	if (!bLocalRagdoll && W && W->GetNetMode() != NM_Standalone && W->GetNetMode() != NM_Client)
	{
		for (TActorIterator<APawn> It(W); It; ++It)
		{
			if (UGTAIVPlayerComponent* C = It->FindComponentByClass<UGTAIVPlayerComponent>())
			{
				if (It->IsPlayerControlled())
				{
					C->MulticastRagdoll(Actor, Impulse, Location);
					break;
				}
			}
		}
	}
	// SandboxCharacter_Mover_Ragdoll (and others like it) have their own ragdoll with get-up
	if (UFunction* F = Actor->FindFunction(TEXT("TriggerRagdoll")))
	{
		// GASP's takes a flag (stop the current montage first): set any flags it has
		uint8* Parms = static_cast<uint8*>(FMemory_Alloca(FMath::Max<int32>(1, F->ParmsSize)));
		FMemory::Memzero(Parms, FMath::Max<int32>(1, F->ParmsSize));
		for (TFieldIterator<FBoolProperty> It(F); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				It->SetPropertyValue_InContainer(Parms, true);
			}
		}
		Actor->ProcessEvent(F, F->ParmsSize > 0 ? Parms : nullptr);
		Pending.Add({Actor, Impulse, Location, 0.5f});
		return;
	}
	// anything else with a skeletal mesh and a physics asset: plain physics ragdoll
	USkeletalMeshComponent* Mesh = nullptr;
	if (ACharacter* C = Cast<ACharacter>(Actor))
	{
		Mesh = C->GetMesh();
		if (C->GetCharacterMovement())
		{
			C->GetCharacterMovement()->DisableMovement();
		}
		C->GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (!Mesh)
	{
		Mesh = Actor->FindComponentByClass<USkeletalMeshComponent>();
	}
	if (!Mesh || !Mesh->GetPhysicsAsset())
	{
		return;
	}
	if (APawn* P = Cast<APawn>(Actor))
	{
		if (AController* Ctrl = P->GetController())
		{
			Ctrl->StopMovement();
		}
	}
	Actor->Tags.AddUnique(DeadTag);
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Mesh->SetCollisionProfileName(TEXT("Ragdoll"));
	Mesh->SetAllBodiesSimulatePhysics(true);
	Mesh->SetSimulatePhysics(true);
	Mesh->WakeAllRigidBodies();
	Mesh->AddImpulseAtLocation(Impulse, Location, Mesh->FindClosestBone(Location));
}

void UGTAIVPlayerComponent::ProcessPending(float Dt)
{
	for (int32 i = Pending.Num() - 1; i >= 0; --i)
	{
		FPendingPush& P = Pending[i];
		P.TimeLeft -= Dt;
		AActor* A = P.Actor.Get();
		USkeletalMeshComponent* Mesh = A ? A->FindComponentByClass<USkeletalMeshComponent>() : nullptr;
		if (const ACharacter* C = Cast<ACharacter>(A))
		{
			Mesh = C->GetMesh();
		}
		if (Mesh && IsRagdolling(Mesh))
		{
			Mesh->AddImpulseAtLocation(P.Impulse, P.Location, Mesh->FindClosestBone(P.Location));
			Pending.RemoveAtSwap(i);
		}
		else if (!A || P.TimeLeft <= 0.f)
		{
			Pending.RemoveAtSwap(i);
		}
	}
}

// ------------------------------------------------------------------ frame

void UGTAIVPlayerComponent::HandleInput(float Dt)
{
	APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (!PC)
	{
		return;
	}
	if (PC->WasInputKeyJustPressed(DrawKey) || PC->WasInputKeyJustPressed(EKeys::MouseScrollUp) ||
		PC->WasInputKeyJustPressed(EKeys::MouseScrollDown) || PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Up))
	{
		if (bArmed && !PC->WasInputKeyJustPressed(DrawKey))
		{
			HolsterWeapon();  // the wheel cycles fists <-> pistol like the game
		}
		else
		{
			DrawWeapon();
		}
	}
	if (PC->WasInputKeyJustPressed(HolsterKey))
	{
		HolsterWeapon();
	}
	// get in the nearest car (traffic cars are taken over)
	if ((PC->WasInputKeyJustPressed(EKeys::F) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Top)) &&
		GetWorld()->GetTimeSeconds() >= VehicleCooldownUntil)
	{
		if (IsOwningClient())
		{
			ServerEnterVehicle();
			VehicleCooldownUntil = GetWorld()->GetTimeSeconds() + 0.6;
		}
		else if (EnterNearestVehicle())
		{
			return;
		}
	}
	// Page Up / Page Down: put the next / previous GTA IV vehicle in front of you
	const bool bNext = PC->WasInputKeyJustPressed(EKeys::PageUp);
	if (bNext || PC->WasInputKeyJustPressed(EKeys::PageDown))
	{
		if (IsOwningClient())
		{
			ServerSpawnVehicle(bNext ? 1 : -1);
		}
		else
		{
			SpawnVehicleInFront(bNext ? 1 : -1);
		}
	}
	if (PC->WasInputKeyJustPressed(ReloadKey) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Left))
	{
		Reload();
	}
	bool bGaspAim = false;
	if (!ReadGaspAim(bGaspAim))
	{
		bGaspAim = PC->IsInputKeyDown(AimKey) || PC->IsInputKeyDown(EKeys::Gamepad_LeftTrigger);
	}
	bAiming = bArmed && (bGaspAim || bAimOverride) && Action != EAction::Holster && !bWasted;
	ThreatTimer -= Dt;
	if (bAiming && AimAlpha > 0.5f && ThreatTimer <= 0.f && GetOwner()->HasAuthority())
	{
		ThreatTimer = 0.25f;
		ScanThreats(false);
	}
	if (PC->WasInputKeyJustPressed(FireKey) || PC->WasInputKeyJustPressed(EKeys::Gamepad_RightTrigger))
	{
		Fire();
	}
}

void UGTAIVPlayerComponent::ApplyClip(const FGTAIVAnimClip& Clip, float Time, float Weight, const TSet<FName>* Mask)
{
	PoseClip(Poseable, Clip, Time, Weight, Mask);
}

void UGTAIVPlayerComponent::PoseClip(UPoseableMeshComponent* Poseable, const FGTAIVAnimClip& Clip, float Time, float Weight,
									 const TSet<FName>* Mask)
{
	const int32 NB = Clip.Bones.Num();
	if (!Poseable || NB == 0 || Clip.Frames == 0 || Weight <= 0.f || Clip.Keys.Num() < NB * Clip.Frames)
	{
		return;
	}
	const float F = FMath::Clamp(Time * Clip.Fps, 0.f, float(Clip.Frames - 1));
	const int32 F0 = FMath::FloorToInt(F);
	const int32 F1 = FMath::Min(F0 + 1, Clip.Frames - 1);
	const float A = F - F0;
	for (int32 b = 0; b < NB; ++b)
	{
		if (Mask && !Mask->Contains(Clip.Bones[b]))
		{
			continue;
		}
		const int32 Index = Poseable->GetBoneIndex(Clip.Bones[b]);
		if (!Poseable->BoneSpaceTransforms.IsValidIndex(Index))
		{
			continue;
		}
		const FQuat Q = FQuat::Slerp(Clip.Keys[F0 * NB + b].GetRotation(), Clip.Keys[F1 * NB + b].GetRotation(), A);
		FTransform& T = Poseable->BoneSpaceTransforms[Index];
		// rotations only: the clips were made for another body than Niko's
		T.SetRotation(FQuat::Slerp(T.GetRotation(), Q, Weight).GetNormalized());
	}
}

void UGTAIVPlayerComponent::UpdatePose(float Dt)
{
	if (!Poseable || !Source.IsValid())
	{
		return;
	}
	Poseable->CopyPoseFromSkeletalComponent(Source.Get());
	// at the wheel: GTA IV's driving animations instead of GASP's
	if (AGTAIVVehicle* Car = SeatVehicle.Get())
	{
		SeatTime += Dt;
		const UGTAIVVehicleAnims* Anims = UGTAIVVehicleAnims::Get();
		const FGTAIVVehicleAnimSet* Set = Anims ? Anims->Find(Car->Def.AnimGroup) : nullptr;
		if (Set && SeatPhase != ESeatPhase::Seated && SeatPhase != ESeatPhase::None)
		{
			UpdateSeatPhase(Dt, Car, *Set);  // getting in / out: GTA IV's door animations
		}
		else
		{
			if (Set)
			{
				const float Fwd = FVector::DotProduct(Car->GetVelocity(), Car->GetActorForwardVector());
				ReverseAlpha = FMath::FInterpTo(ReverseAlpha, Fwd < -80.f && Car->ThrottleInput < -0.1f ? 1.f : 0.f, Dt, 4.f);
				UGTAIVVehicleAnims::PoseDriver(Poseable, *Set, SeatTime, Car->GetSteerAmount(), ReverseAlpha);
			}
			// the body facing the car's way with its pelvis on the seat, whatever way the character
			// faced when it got in (GASP's movement keeps the actor's own facing)
			const FQuat Facing = Car->GetActorQuat() * FQuat(FVector::UpVector, FMath::DegreesToRadians(-90.f));
			Poseable->SetWorldLocationAndRotation(
				Car->GetSeatLocation() - Facing.RotateVector(UGTAIVVehicleAnims::PelvisRefLocation(Poseable)), Facing);
		}
		if (Gun)
		{
			Gun->SetVisibility(false);
		}
		Poseable->RefreshBoneTransforms();
		return;
	}
	const bool bRagdoll = OwnerRagdolling();
	if (Gun)
	{
		Gun->SetVisibility(bArmed && Action != EAction::Draw ? true : (Action == EAction::Draw && ActionTime > 0.35f));
	}
	if (bRagdoll)
	{
		AimAlpha = 0.f;
		HandAlpha = 0.f;
		Poseable->MarkRefreshTransformDirty();
		return;
	}
	if (!Weapon)
	{
		Poseable->RefreshBoneTransforms();
		ApplyHandIK(Dt);
		return;
	}

	// which bones each layer drives
	TSet<FName>& Arms = ArmBones;
	TSet<FName>& Upper = UpperBones;
	if (!Poseable->GetSkinnedAsset())
	{
		return;
	}
	if (MaskAsset.Get() != Poseable->GetSkinnedAsset())
	{
		MaskAsset = Poseable->GetSkinnedAsset();
		Arms.Reset();
		Upper.Reset();
		const FReferenceSkeleton& Ref = Poseable->GetSkinnedAsset()->GetRefSkeleton();
		auto Below = [&Ref](int32 i, const TCHAR* Root) {
			for (int32 p = i; p != INDEX_NONE; p = Ref.GetParentIndex(p))
			{
				if (Ref.GetBoneName(p) == FName(Root))
				{
					return true;
				}
			}
			return false;
		};
		for (int32 i = 0; i < Ref.GetNum(); ++i)
		{
			const FName N = Ref.GetBoneName(i);
			if (IsTwistOrFace(N.ToString()))
			{
				continue;
			}
			if (Below(i, TEXT("Char_L_Clavicle")) || Below(i, TEXT("Char_R_Clavicle")))
			{
				Arms.Add(N);
			}
			if (Below(i, TEXT("Char_Spine1")))
			{
				Upper.Add(N);
			}
		}
	}

	auto Clip = [this](const TCHAR* Name) { return Weapon->Clips.Find(FName(Name)); };
	const FGTAIVAnimClip* Fire = Clip(TEXT("fire"));
	const FGTAIVAnimClip* Draw = Clip(TEXT("unholster"));
	const FGTAIVAnimClip* Put = Clip(TEXT("holster"));
	const FGTAIVAnimClip* Rel = Clip(TEXT("reload"));

	AimAlpha = FMath::FInterpConstantTo(AimAlpha, bAiming && Action != EAction::Draw ? 1.f : 0.f, Dt, 7.f);
	ArmedAlpha = FMath::FInterpConstantTo(ArmedAlpha, bArmed ? 1.f : 0.f, Dt, 6.f);

	// armed and not aiming: the gun held low (end of the draw)
	if (bArmed && Draw && Action == EAction::None)
	{
		ApplyClip(*Draw, Draw->Duration(), ArmedAlpha * (1.f - AimAlpha), &Arms);
	}
	switch (Action)
	{
	case EAction::Draw:
		if (Draw)
		{
			ApplyClip(*Draw, ActionTime, FMath::Min(1.f, ActionTime * 8.f), &Upper);
		}
		if (!Draw || ActionTime >= Draw->Duration())
		{
			Action = EAction::None;
		}
		break;
	case EAction::Holster:
		if (Put)
		{
			ApplyClip(*Put, ActionTime, FMath::Min(1.f, (Put->Duration() - ActionTime) * 6.f), &Upper);
		}
		if (!Put || ActionTime >= Put->Duration())
		{
			Action = EAction::None;
			bArmed = false;
			ArmedAlpha = 0.f;
		}
		break;
	case EAction::Reload:
		if (Rel)
		{
			ApplyClip(*Rel, ActionTime, FMath::Min(1.f, FMath::Min(ActionTime, Rel->Duration() - ActionTime) * 6.f), &Upper);
			if (!bReloadSoundPlayed && ActionTime > Rel->Duration() * 0.45f)
			{
				bReloadSoundPlayed = true;
				PlaySound(Weapon->ClipInSound);
			}
			if (!bCockSoundPlayed && ActionTime > Rel->Duration() * 0.7f)
			{
				bCockSoundPlayed = true;
				PlaySound(Weapon->CockSound);
			}
		}
		if (!Rel || ActionTime >= Rel->Duration())
		{
			const int32 Take = FMath::Min(Weapon->ClipSize - ClipAmmo, SpareAmmo);
			ClipAmmo += Take;
			SpareAmmo -= Take;
			Action = EAction::None;
		}
		break;
	default:
		break;
	}
	// aiming: the fire clip's held pose, then turned to where the camera looks
	if (Fire && AimAlpha > 0.f && Action == EAction::None)
	{
		ApplyClip(*Fire, Weapon->AimFrame / Fire->Fps, AimAlpha, &Upper);
	}
	Poseable->MarkRefreshTransformDirty();
	Poseable->RefreshBoneTransforms();

	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (AimAlpha > 0.f && Pawn)
	{
		const FRotator Control = bHasAimTarget
			? (AimTarget - Poseable->GetBoneLocation(TEXT("Char_Spine3"))).Rotation()
			: Pawn->GetControlRotation();
		const FRotator Facing = GetOwner()->GetActorRotation();
		const float Pitch = FMath::Clamp(FRotator::NormalizeAxis(Control.Pitch), -70.f, 70.f) * AimAlpha;
		const float Yaw = FMath::Clamp(FRotator::NormalizeAxis(Control.Yaw - Facing.Yaw), -60.f, 60.f) * AimAlpha;
		const FVector Right = FRotationMatrix(FRotator(0.f, Control.Yaw, 0.f)).GetUnitAxis(EAxis::Y);
		const FQuat Turn = FQuat(FVector::UpVector, FMath::DegreesToRadians(Yaw / 3.f)) *
						   FQuat(Right, FMath::DegreesToRadians(-Pitch / 3.f));
		for (const TCHAR* Bone : {TEXT("Char_Spine1"), TEXT("Char_Spine2"), TEXT("Char_Spine3")})
		{
			FTransform T = Poseable->GetBoneTransformByName(Bone, EBoneSpaces::WorldSpace);
			T.SetRotation(Turn * T.GetRotation());
			Poseable->SetBoneTransformByName(Bone, T, EBoneSpaces::WorldSpace);
		}
	}
	if (Recoil > 0.f)
	{
		const FVector Right = GetOwner()->GetActorRightVector();
		FTransform T = Poseable->GetBoneTransformByName(TEXT("Char_R_Forearm"), EBoneSpaces::WorldSpace);
		T.SetRotation(FQuat(Right, FMath::DegreesToRadians(-14.f * Recoil)) * T.GetRotation());
		Poseable->SetBoneTransformByName(TEXT("Char_R_Forearm"), T, EBoneSpaces::WorldSpace);
		Recoil = FMath::Max(0.f, Recoil - Dt * 9.f);
	}
	// the turns above change bones after the gun was placed: refresh the pose, then the gun
	Poseable->RefreshBoneTransforms();
	ApplyHandIK(Dt);
	if (Gun)
	{
		Gun->UpdateComponentToWorld();
	}
	if (Flash)
	{
		Flash->UpdateComponentToWorld();
	}
}

void UGTAIVPlayerComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!DebugAction.IsEmpty())
	{
		const FString A = DebugAction;
		DebugAction.Reset();
		if (A == TEXT("draw")) DrawWeapon();
		else if (A == TEXT("holster")) HolsterWeapon();
		else if (A == TEXT("fire")) Fire();
		else if (A == TEXT("aim1")) bAimOverride = true;
		else if (A == TEXT("aim0")) bAimOverride = false;
		else if (A == TEXT("enter"))
		{
			if (IsOwningClient()) ServerEnterVehicle(); else EnterNearestVehicle();
		}
		else if (A == TEXT("spawn"))
		{
			if (IsOwningClient()) ServerSpawnVehicle(1); else SpawnVehicleInFront(1);
		}
	}
	DressTimer -= DeltaTime;
	if (DressTimer <= 0.f)
	{
		DressTimer = 0.5f;
		Dress();
	}
	if (Action != EAction::None)
	{
		ActionTime += DeltaTime;
	}
	FireCooldown = FMath::Max(0.f, FireCooldown - DeltaTime);
	if (bWasted)
	{
		WastedTime += DeltaTime;  // game time: slowed down while wasted
		// stays down (GASP's ragdoll character would get up)
		const double Now = GetWorld()->GetRealTimeSeconds();
		if (!OwnerRagdolling() && Now - LastDownCheck > 0.5)
		{
			LastDownCheck = Now;
			RagdollActor(GetOwner(), FVector::ZeroVector, GetOwner()->GetActorLocation());
		}
		APawn* Pawn = Cast<APawn>(GetOwner());
		APlayerController* PC = Cast<APlayerController>(Pawn ? Pawn->GetController() : nullptr);
		if (GetWorld()->GetNetMode() != NM_Standalone)
		{
			// multiplayer: the server brings the player back at a spawn point (the body stays a while)
			if (!PC)
			{
				PC = DeadController.Get();  // in case the ragdoll lost its controller
			}
			if (Pawn && Pawn->HasAuthority() && RespawnAt > 0.0 && GetWorld()->GetTimeSeconds() >= RespawnAt && PC)
			{
				RespawnAt = 0.0;
				if (AGameModeBase* GM = GetWorld()->GetAuthGameMode())
				{
					if (PC->GetPawn() == Pawn)
					{
						PC->UnPossess();
					}
					Pawn->SetLifeSpan(30.f);
					GM->RestartPlayer(PC);
				}
			}
		}
		// the GTA IV HUD plays the wasted sequence and reloads; without it, reload here
		else if (IsLocalPlayer() && !(PC && Cast<AGTAIVPlayerHUD>(PC->GetHUD())) && WastedTime > 6.f)
		{
			bWasted = false;  // once
			UGameplayStatics::OpenLevel(this, FName(*UGameplayStatics::GetCurrentLevelName(this)));
		}
	}
	else if (IsLocalPlayer())
	{
		HandleInput(DeltaTime);
		// the server (and so everyone) needs to know where this player aims
		if (IsOwningClient())
		{
			AimSendTimer -= DeltaTime;
			FVector Eye;
			FRotator Rot;
			if (APlayerController* PC = Cast<APlayerController>(Cast<APawn>(GetOwner())->GetController()))
			{
				PC->GetPlayerViewPoint(Eye, Rot);
				if (bAiming != bLastAimSent || (bAiming && AimSendTimer <= 0.f))
				{
					AimSendTimer = 0.1f;
					bLastAimSent = bAiming;
					ServerSetAim(bAiming, Eye + Rot.Vector() * 2000.f);
				}
			}
		}
		else if (GetOwner())
		{
			bAimNet = bAiming;
			FVector Eye;
			FRotator Rot;
			if (APlayerController* PC = Cast<APlayerController>(Cast<APawn>(GetOwner())->GetController()))
			{
				PC->GetPlayerViewPoint(Eye, Rot);
				AimPointNet = Eye + Rot.Vector() * 2000.f;
			}
		}
	}
	else if (Cast<APawn>(GetOwner()) && Cast<APawn>(GetOwner())->IsPlayerControlled())
	{
		// another player: aims where they aim
		bAiming = bAimNet && bArmed && Action != EAction::Holster;
		bHasAimTarget = bAiming;
		AimTarget = AimPointNet;
		// the server scares the peds they aim at
		if (GetOwner()->HasAuthority() && bAiming)
		{
			ThreatTimer -= DeltaTime;
			if (ThreatTimer <= 0.f)
			{
				ThreatTimer = 0.25f;
				ScanThreats(false);
			}
		}
	}
	else
	{
		bAiming = bArmed && bAimOverride && Action != EAction::Holster;
	}
	AssignModel();
	UpdatePose(DeltaTime);
	if (Flash)
	{
		FlashTime -= DeltaTime;
		Flash->SetIntensity(FlashTime > 0.f ? 20000.f : 0.f);
	}
	ProcessPending(DeltaTime);
}


// ------------------------------------------------------------------ fights

void UGTAIVPlayerComponent::ScanThreats(bool bGunshot)
{
	APawn* Me = Cast<APawn>(GetOwner());
	APlayerController* PC = Me ? Cast<APlayerController>(Me->GetController()) : nullptr;
	if (!PC)
	{
		return;
	}
	FVector Eye;
	FRotator Rot;
	PC->GetPlayerViewPoint(Eye, Rot);
	const FVector Fwd = Rot.Vector();
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		APawn* P = *It;
		if (P == Me || P->Tags.Contains(TEXT("GTAIV_Dead")) || !UGTAIVPedBrain::IsPed(P))
		{
			continue;  // cars (and anything else that isn't a person) don't react
		}
		const FVector Chest = P->GetActorLocation() + FVector(0, 0, 40.f);
		const float Dist = FVector::Dist(Chest, Me->GetActorLocation());
		if (bGunshot)
		{
			if (Dist < 2500.f)
			{
				UGTAIVPedBrain::Threaten(P, Me, false);
			}
			continue;
		}
		if (Dist > 4000.f)
		{
			continue;
		}
		const FVector To = (Chest - Eye).GetSafeNormal();
		const float Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(To, Fwd), -1.f, 1.f)));
		if (Angle > (Dist < 1000.f ? 20.f : 9.f))
		{
			continue;
		}
		FHitResult Block;
		FCollisionQueryParams Params(TEXT("GTAIVThreat"), false, Me);
		Params.AddIgnoredActor(P);
		if (PedActor.IsValid())
		{
			Params.AddIgnoredActor(PedActor.Get());
		}
		if (!GetWorld()->LineTraceSingleByChannel(Block, Eye, Chest, ECC_Visibility, Params) ||
			FVector::Dist(Block.ImpactPoint, Chest) < 60.f)
		{
			UGTAIVPedBrain::Threaten(P, Me, true);
		}
	}
}

bool UGTAIVPlayerComponent::FireAtTarget(float Spread)
{
	if (!bArmed || !Weapon || Action != EAction::None || AimAlpha < 0.6f || FireCooldown > 0.f || !bHasAimTarget)
	{
		return false;
	}
	FireCooldown = Weapon->FireInterval;
	if (ClipAmmo <= 0)
	{
		SpareAmmo = FMath::Max(SpareAmmo, Weapon->ClipSize);  // peds don't run dry
		Reload();
		return false;
	}
	--ClipAmmo;
	Recoil = 1.f;
	FlashTime = 0.05f;
	PlaySound(Weapon->ShotSound);
	PlaySound(Weapon->ShotLowSound, 0.8f);
	const FVector Start = MuzzleTransform().GetLocation();
	const FVector Dir = FMath::VRandCone((AimTarget - Start).GetSafeNormal(), FMath::DegreesToRadians(Spread));
	FCollisionQueryParams Params(TEXT("GTAIVPedShot"), false, GetOwner());
	if (PedActor.IsValid())
	{
		Params.AddIgnoredActor(PedActor.Get());
	}
	FCollisionObjectQueryParams Objects;
	for (ECollisionChannel C : {ECC_WorldStatic, ECC_WorldDynamic, ECC_Pawn, ECC_PhysicsBody, ECC_Vehicle})
	{
		Objects.AddObjectTypesToQuery(C);
	}
	FHitResult Hit;
	if (!GetWorld()->LineTraceSingleByObjectType(Hit, Start, Start + Dir * Weapon->Range, Objects, Params))
	{
		return true;
	}
	if (Weapon->ImpactSounds.Num() > 0)
	{
		UGameplayStatics::PlaySoundAtLocation(this, Weapon->ImpactSounds[FMath::RandRange(0, Weapon->ImpactSounds.Num() - 1)],
											  Hit.ImpactPoint, 0.6f);
	}
	AActor* A = Hit.GetActor();
	// the visual override is a child actor: a hit on it is a hit on its character
	if (A && A->GetParentActor())
	{
		A = A->GetParentActor();
	}
	if (UGTAIVPlayerComponent* Victim = A ? A->FindComponentByClass<UGTAIVPlayerComponent>() : nullptr)
	{
		const APawn* VictimPawn = Cast<APawn>(A);
		if (Victim != this && VictimPawn && VictimPawn->IsPlayerControlled())
		{
			Victim->TakeHit(Weapon->Damage * 0.35f, Start);
			return true;
		}
	}
	if (Cast<APawn>(A))
	{
		UGTAIVWoundComponent::ApplyHit(A, Hit, Dir, Weapon, GetOwner());
	}
	return true;
}

void UGTAIVPlayerComponent::TakeHit(float Damage, FVector From)
{
	if (bWasted || (GetOwner() && !GetOwner()->HasAuthority()))
	{
		return;  // the server keeps the score
	}
	Health = FMath::Max(0.f, Health - Damage);
	MulticastPain(Health <= 0.f);
	if (Health <= 0.f)
	{
		bWasted = true;
		bWastedNet = true;
		RespawnAt = GetWorld()->GetTimeSeconds() + 10.0;
		DeadController = Cast<APlayerController>(Cast<APawn>(GetOwner()) ? Cast<APawn>(GetOwner())->GetController() : nullptr);
		WastedTime = 0.f;
		bAiming = false;
		DropWeapon();
		const FVector Dir = (GetOwner()->GetActorLocation() - From).GetSafeNormal();
		RagdollActor(GetOwner(), Dir * 6000.f, GetOwner()->GetActorLocation() + FVector(0, 0, 40.f));
	}
}

void UGTAIVPlayerComponent::DropWeapon()
{
	bArmed = false;
	bAiming = false;
	Action = EAction::None;
	if (Gun && Gun->IsVisible() && Gun->GetStaticMesh())
	{
		// a loose copy falls to the ground (a physics box around the model: the weapon has no
		// simple collision of its own); ours stays hidden for the next draw
		const FTransform GT = Gun->GetComponentTransform();
		const FBoxSphereBounds B = Gun->GetStaticMesh()->GetBounds();
		FActorSpawnParameters SP;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Loose = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), GT, SP);
		if (Loose)
		{
			UBoxComponent* Box = NewObject<UBoxComponent>(Loose, TEXT("Box"));
			Box->SetMobility(EComponentMobility::Movable);
			Box->SetBoxExtent(B.BoxExtent.ComponentMax(FVector(1.5f)));
			Loose->SetRootComponent(Box);
			Box->RegisterComponent();
			Box->SetWorldLocationAndRotation(GT.TransformPosition(B.Origin), GT.GetRotation());
			UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(Loose, TEXT("Mesh"));
			M->SetMobility(EComponentMobility::Movable);
			M->SetStaticMesh(Gun->GetStaticMesh());
			M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			M->SetupAttachment(Box);
			M->RegisterComponent();
			M->SetRelativeLocation(-B.Origin);
			Box->SetCollisionProfileName(TEXT("PhysicsActor"));
			Box->SetSimulatePhysics(true);
			Box->AddImpulse(FVector(FMath::FRandRange(-60.f, 60.f), FMath::FRandRange(-60.f, 60.f), 60.f), NAME_None, true);
			Loose->SetLifeSpan(180.f);
		}
		Gun->SetVisibility(false);
	}
}


// ------------------------------------------------------------------ hand IK

void UGTAIVPlayerComponent::ApplyHandIK(float Dt)
{
	HandAlpha = FMath::FInterpConstantTo(HandAlpha, bHandTarget ? 1.f : 0.f, Dt, 2.5f);
	if (!Poseable || HandAlpha <= 0.f)
	{
		return;
	}
	const FName S = bHandLeft ? FName(TEXT("Char_L_UpperArm")) : FName(TEXT("Char_R_UpperArm"));
	const FName E = bHandLeft ? FName(TEXT("Char_L_Forearm")) : FName(TEXT("Char_R_Forearm"));
	const FName H = bHandLeft ? FName(TEXT("Char_L_Hand")) : FName(TEXT("Char_R_Hand"));
	if (Poseable->GetBoneIndex(S) == INDEX_NONE || Poseable->GetBoneIndex(H) == INDEX_NONE)
	{
		return;
	}
	// two-bone IK: shoulder stays, elbow bends on the side it already points to
	const FVector PS = Poseable->GetBoneLocation(S);
	const FVector PE = Poseable->GetBoneLocation(E);
	const FVector PH = Poseable->GetBoneLocation(H);
	const float A = FVector::Dist(PS, PE);
	const float B = FVector::Dist(PE, PH);
	const FVector Goal = FMath::Lerp(PH, HandTarget, FMath::SmoothStep(0.f, 1.f, HandAlpha));
	FVector ToGoal = Goal - PS;
	const float D = FMath::Clamp(ToGoal.Size(), FMath::Abs(A - B) + 1.f, A + B - 0.5f);
	const FVector Dir = ToGoal.GetSafeNormal();
	FVector Pole = (PE - PS) - Dir * FVector::DotProduct(PE - PS, Dir);
	if (Pole.SizeSquared() < 1.f)
	{
		Pole = -FVector::UpVector - Dir * FVector::DotProduct(-FVector::UpVector, Dir);
	}
	Pole.Normalize();
	const float CosA = FMath::Clamp((A * A + D * D - B * B) / (2.f * A * D), -1.f, 1.f);
	const float Ang = FMath::Acos(CosA);
	const FVector NewE = PS + Dir * (FMath::Cos(Ang) * A) + Pole * (FMath::Sin(Ang) * A);
	const FVector NewH = PS + Dir * D;

	FTransform TS = Poseable->GetBoneTransformByName(S, EBoneSpaces::WorldSpace);
	TS.SetRotation(FQuat::FindBetweenNormals((PE - PS).GetSafeNormal(), (NewE - PS).GetSafeNormal()) * TS.GetRotation());
	Poseable->SetBoneTransformByName(S, TS, EBoneSpaces::WorldSpace);
	Poseable->RefreshBoneTransforms();
	const FVector PE2 = Poseable->GetBoneLocation(E);
	const FVector PH2 = Poseable->GetBoneLocation(H);
	FTransform TE = Poseable->GetBoneTransformByName(E, EBoneSpaces::WorldSpace);
	TE.SetRotation(FQuat::FindBetweenNormals((PH2 - PE2).GetSafeNormal(), (NewH - PE2).GetSafeNormal()) * TE.GetRotation());
	Poseable->SetBoneTransformByName(E, TE, EBoneSpaces::WorldSpace);
	Poseable->RefreshBoneTransforms();
}


// ------------------------------------------------------------------ vehicles

bool UGTAIVPlayerComponent::StartExit()
{
	if (!SeatVehicle.IsValid() || SeatPhase != ESeatPhase::Seated || !Poseable || !UGTAIVVehicleAnims::Get())
	{
		return false;
	}
	SeatPhase = ESeatPhase::GetOut;
	PhaseTime = 0.f;
	bDoorSound = false;
	PublishSeat();
	return true;
}

bool UGTAIVPlayerComponent::StartJumpOut()
{
	if (!SeatVehicle.IsValid() || SeatPhase != ESeatPhase::Seated || !Poseable || !UGTAIVVehicleAnims::Get())
	{
		return false;
	}
	SeatPhase = ESeatPhase::JumpOut;
	PhaseTime = 0.f;
	bDoorSound = false;
	PublishSeat();
	return true;
}

void UGTAIVPlayerComponent::UpdateSeatPhase(float Dt, AGTAIVVehicle* Car, const FGTAIVVehicleAnimSet& Set)
{
	// where the body is: standing at the door (its feet on the ground) or with its pelvis on the seat
	const FQuat Facing = Car->GetActorQuat() * FQuat(FVector::UpVector, FMath::DegreesToRadians(-90.f));
	const FVector PelvisRef = UGTAIVVehicleAnims::PelvisRefLocation(Poseable);
	const FVector Sit = Car->GetSeatLocation() - Facing.RotateVector(PelvisRef);
	const FVector Stand = Car->GetEntryLocation();
	auto Len = [](const FGTAIVAnimClip& C, float Fallback) { return C.Frames > 1 ? C.Duration() : Fallback; };
	PhaseTime += Dt;
	const FGTAIVAnimClip* Clip = nullptr;
	float Dur = 0.5f, Alpha = 0.f;   // Alpha: 0 standing at the door .. 1 seated
	switch (SeatPhase)
	{
	case ESeatPhase::OpenDoor:
		Clip = &Set.OpenDoor;
		Dur = Len(*Clip, 0.6f);
		Car->SetDoorOpen(FMath::SmoothStep(0.35f, 1.f, PhaseTime / Dur));
		if (!bDoorSound && PhaseTime > Dur * 0.35f)
		{
			bDoorSound = true;
			Car->PlayDoorSound(true);
		}
		break;
	case ESeatPhase::Jack:
		Clip = &Set.Jack;
		Dur = Len(*Clip, 2.5f);
		Car->SetDoorOpen(1.f);
		break;
	case ESeatPhase::GetIn:
		Clip = &Set.GetIn;
		Dur = Len(*Clip, 0.9f);
		Alpha = FMath::SmoothStep(0.05f, 0.95f, PhaseTime / Dur);
		Car->SetDoorOpen(1.f);
		break;
	case ESeatPhase::CloseIn:
		Clip = &Set.CloseDoorInside;
		Dur = Len(*Clip, 1.f);
		Alpha = 1.f;
		Car->SetDoorOpen(1.f - FMath::SmoothStep(0.3f, 0.75f, PhaseTime / Dur));
		if (!bDoorSound && PhaseTime > Dur * 0.72f)
		{
			bDoorSound = true;
			Car->PlayDoorSound(false);
		}
		break;
	case ESeatPhase::GetOut:
		Clip = &Set.GetOut;
		Dur = Len(*Clip, 1.5f);
		Alpha = 1.f - FMath::SmoothStep(0.3f, 0.9f, PhaseTime / Dur);
		Car->SetDoorOpen(FMath::SmoothStep(0.02f, 0.3f, PhaseTime / Dur));
		if (!bDoorSound && PhaseTime > Dur * 0.04f)
		{
			bDoorSound = true;
			Car->PlayDoorSound(true);
		}
		break;
	case ESeatPhase::CloseOut:
		Clip = &Set.CloseDoorOutside;
		Dur = Len(*Clip, 1.f);
		Car->SetDoorOpen(1.f - FMath::SmoothStep(0.25f, 0.7f, PhaseTime / Dur));
		if (!bDoorSound && PhaseTime > Dur * 0.68f)
		{
			bDoorSound = true;
			Car->PlayDoorSound(false);
		}
		break;
	case ESeatPhase::JumpOut:
		Clip = &Set.JumpOut;
		Dur = 0.55f;   // the start of jump_out_ds, then the ragdoll takes over
		Alpha = 1.f - FMath::SmoothStep(0.1f, 1.f, PhaseTime / Dur);
		Car->SetDoorOpen(FMath::Min(1.f, PhaseTime / 0.15f));
		break;
	default:
		break;
	}
	const FVector Bob = Clip ? UGTAIVVehicleAnims::PoseFull(Poseable, *Clip, PhaseTime) : FVector::ZeroVector;
	Poseable->SetWorldLocationAndRotation(FMath::Lerp(Stand, Sit, Alpha) + Facing.RotateVector(Bob) * (1.f - Alpha), Facing);
	if (PhaseTime < Dur)
	{
		return;
	}
	if (GetOwner() && !GetOwner()->HasAuthority())
	{
		PhaseTime = Dur;  // a client holds the end of the step until the server's next one arrives
		return;
	}
	// next step
	PhaseTime = 0.f;
	bDoorSound = false;
	switch (SeatPhase)
	{
	case ESeatPhase::OpenDoor:
		SeatPhase = bJackPending ? ESeatPhase::Jack : ESeatPhase::GetIn;
		if (bJackPending)
		{
			Car->StartVictim();
		}
		break;
	case ESeatPhase::Jack:
		SeatPhase = ESeatPhase::GetIn;
		break;
	case ESeatPhase::GetIn:
		SeatPhase = ESeatPhase::CloseIn;
		Car->SetInputLocked(false);  // drive off while the door shuts
		break;
	case ESeatPhase::CloseIn:
		SeatPhase = ESeatPhase::Seated;
		SeatTime = 0.f;
		break;
	case ESeatPhase::GetOut:
		SeatPhase = ESeatPhase::CloseOut;
		break;
	case ESeatPhase::CloseOut:
		SeatPhase = ESeatPhase::None;
		Car->FinishExit();
		break;
	case ESeatPhase::JumpOut:
		SeatPhase = ESeatPhase::None;
		Car->FinishBail();
		break;
	default:
		break;
	}
	PublishSeat();
}

bool UGTAIVPlayerComponent::SetSeat(AGTAIVVehicle* Vehicle, bool bJack)
{
	SeatVehicle = Vehicle;
	SeatTime = 0.f;
	ReverseAlpha = 0.f;
	SeatPhase = Vehicle ? ESeatPhase::OpenDoor : ESeatPhase::None;
	PhaseTime = 0.f;
	bJackPending = bJack;
	bDoorSound = false;
	PublishSeat();
	if (!Vehicle)
	{
		if (Poseable)
		{
			Poseable->SetRelativeTransform(FTransform::Identity);  // back on the walking body
		}
		return true;
	}
	bArmed = false;
	bAiming = false;
	Action = EAction::None;
	if (Gun)
	{
		Gun->SetVisibility(false);
	}
	return PlaceInSeat();
}

bool UGTAIVPlayerComponent::PlaceInSeat()
{
	AGTAIVVehicle* Car = SeatVehicle.Get();
	AActor* Owner = GetOwner();
	if (!Car || !Owner || !Poseable || !Poseable->GetSkinnedAsset() || !UGTAIVVehicleAnims::Get())
	{
		return false;
	}
	// facing forward, the reference pose's pelvis on the seat (where GTA IV puts it)
	Owner->SetActorRotation(Car->GetActorRotation(), ETeleportType::TeleportPhysics);
	Poseable->UpdateComponentToWorld();
	const FVector Pelvis = Poseable->GetComponentTransform().TransformPosition(UGTAIVVehicleAnims::PelvisRefLocation(Poseable));
	Owner->AddActorWorldOffset(Car->GetSeatLocation() - Pelvis, false, nullptr, ETeleportType::TeleportPhysics);
	return true;
}

bool UGTAIVPlayerComponent::EnterNearestVehicle()
{
	APawn* Me = Cast<APawn>(GetOwner());
	if (!Me || bWasted)
	{
		return false;
	}
	// how far the player is from the car's side (ground distance minus half the car's size)
	const FVector MyLoc = Me->GetActorLocation();
	auto Gap = [&](const AActor* A, const FGTAIVVehicleDef& D)
	{
		const FVector L = A->GetActorLocation();
		if (FMath::Abs(L.Z - MyLoc.Z) > 400.f)
		{
			return 1e9f;
		}
		return float(FVector::Dist2D(L, MyLoc)) - FMath::Max(D.Length, D.Width) * 0.4f;
	};
	AActor* Best = nullptr;
	float BestDist = 320.f;
	for (TActorIterator<AGTAIVVehicle> It(GetWorld()); It; ++It)
	{
		const float D = Gap(*It, It->Def);
		if (!It->GetDriver() && D < BestDist)
		{
			Best = *It;
			BestDist = D;
		}
	}
	for (TActorIterator<AGTAIVTrafficCar> It(GetWorld()); It; ++It)
	{
		const float D = Gap(*It, It->Def);
		if (D < BestDist)
		{
			Best = *It;
			BestDist = D;
		}
	}
	AGTAIVVehicle* Car = Cast<AGTAIVVehicle>(Best);
	if (!Car)
	{
		Car = AGTAIVVehicle::TakeOver(Cast<AGTAIVTrafficCar>(Best), Me);
	}
	if (!Car)
	{
		return false;
	}
	bArmed = false;
	bAiming = false;
	Action = EAction::None;
	Car->EnterDriver(Me);
	return true;
}

void UGTAIVPlayerComponent::SpawnVehicleInFront(int32 Step)
{
	const UGTAIVVehicleSet* Set = AGTAIVVehicle::LoadVehicleSet();
	APawn* Me = Cast<APawn>(GetOwner());
	if (!Set || Set->Vehicles.Num() == 0 || !Me)
	{
		if (APlayerController* PC = Cast<APlayerController>(Me ? Me->GetController() : nullptr))
		{
			if (AGTAIVPlayerHUD* HUD = Cast<AGTAIVPlayerHUD>(PC->GetHUD()))
			{
				HUD->ShowHelpText(TEXT("No GTA IV vehicles in this project yet: export them from the Vehicles tab."), 5.f);
			}
		}
		return;
	}
	static int32 Index = -1;
	Index = (Index + Step + Set->Vehicles.Num()) % Set->Vehicles.Num();
	if (AGTAIVVehicle* Old = LastSpawned.Get())
	{
		if (!Old->GetDriver())
		{
			Old->Destroy();
		}
	}
	const FRotator Yaw(0.f, Me->GetActorRotation().Yaw, 0.f);
	FVector At = Me->GetActorLocation() + Yaw.Vector() * 650.f;
	FHitResult Hit;
	FCollisionQueryParams Params(TEXT("GTAIVSpawnCar"), false, Me);
	if (GetWorld()->LineTraceSingleByChannel(Hit, At + FVector(0, 0, 300.f), At - FVector(0, 0, 1000.f), ECC_Visibility, Params))
	{
		At = Hit.ImpactPoint;
	}
	const FGTAIVVehicleDef& D = Set->Vehicles[Index];
	AGTAIVVehicle* Car = AGTAIVVehicle::SpawnVehicle(this, D.Name, FTransform(Yaw + FRotator(0.f, 90.f, 0.f), At));
	LastSpawned = Car;
	if (APlayerController* PC = Cast<APlayerController>(Me->GetController()))
	{
		if (AGTAIVPlayerHUD* HUD = Cast<AGTAIVPlayerHUD>(PC->GetHUD()))
		{
			HUD->ShowHelpText(FString::Printf(TEXT("%s (%d/%d) - press F to get in. Page Up/Down: other vehicles."),
											  Car ? *Car->GetDisplayName() : *D.Name.ToString(), Index + 1, Set->Vehicles.Num()), 6.f);
		}
	}
}
