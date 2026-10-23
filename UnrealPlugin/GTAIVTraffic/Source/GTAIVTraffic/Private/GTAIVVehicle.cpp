#include "GTAIVVehicle.h"

#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Engine/OverlapResult.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/UnrealType.h"
#include "AIController.h"
#include "Components/ChildActorComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "EngineUtils.h"
#include "GTAIVPedManager.h"
#include "GTAIVPlayerComponent.h"
#include "GTAIVVehicleAnims.h"
#include "GTAIVPlayerHUD.h"
#include "GTAIVRadio.h"
#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "GTAIVTrafficCar.h"
#include "InputCoreTypes.h"

namespace
{
	constexpr float G = 9.81f;  // m/s^2

	/** Exported meshes point their nose along -Y; the actor faces +X. */
	FVector MeshToActor(const FVector& V) { return FVector(-V.Y, V.X, V.Z); }
	FVector AbsMeshToActor(const FVector& V) { return FVector(FMath::Abs(V.Y), FMath::Abs(V.X), FMath::Abs(V.Z)); }

	bool Pressed(APlayerController* PC, std::initializer_list<FKey> Keys)
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

	bool Down(APlayerController* PC, std::initializer_list<FKey> Keys)
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

	/** Stops / restarts the driver's own movement while it rides in the car. Mover characters (GASP)
	 *  keep simulating whatever their tick says, so they're switched to Mover's "Flying" mode (no
	 *  gravity) instead; otherwise the hidden driver falls through the world and is killed. */
	void SetMovementTick(APawn* P, bool bOn)
	{
		for (UActorComponent* C : P->GetComponents())
		{
			const FString N = C ? C->GetClass()->GetName() : FString();
			if (UCharacterMovementComponent* CM = Cast<UCharacterMovementComponent>(C))
			{
				CM->SetMovementMode(bOn ? MOVE_Falling : MOVE_None);
				CM->Velocity = FVector::ZeroVector;
				CM->SetComponentTickEnabled(bOn);
			}
			else if (N == TEXT("MoverComponent") || (N.EndsWith(TEXT("MoverComponent")) && !N.Contains(TEXT("Liaison"))))
			{
				if (UFunction* F = C->FindFunction(TEXT("QueueNextMode")))
				{
					uint8* Parms = static_cast<uint8*>(FMemory_Alloca(FMath::Max<int32>(1, F->ParmsSize)));
					FMemory::Memzero(Parms, FMath::Max<int32>(1, F->ParmsSize));
					for (TFieldIterator<FProperty> It(F); It; ++It)
					{
						if (FNameProperty* NP = CastField<FNameProperty>(*It))
						{
							NP->SetPropertyValue_InContainer(Parms, bOn ? FName(TEXT("Falling")) : FName(TEXT("Flying")));
						}
						else if (FBoolProperty* BP = CastField<FBoolProperty>(*It))
						{
							BP->SetPropertyValue_InContainer(Parms, true);  // re-enter even if already in it
						}
					}
					C->ProcessEvent(F, Parms);
				}
			}
		}
	}
}

AGTAIVVehicle::AGTAIVVehicle()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	Chassis = CreateDefaultSubobject<UBoxComponent>(TEXT("Chassis"));
	Chassis->SetCollisionProfileName(TEXT("Vehicle"));
	Chassis->SetMobility(EComponentMobility::Movable);
	Chassis->BodyInstance.bUseCCD = true;
	RootComponent = Chassis;

	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Chassis);
	Body->SetMobility(EComponentMobility::Movable);
	Body->SetRelativeRotation(FRotator(0.f, 90.f, 0.f));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	CameraArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraArm"));
	CameraArm->SetupAttachment(Chassis);
	CameraArm->bUsePawnControlRotation = true;
	CameraArm->bEnableCameraLag = true;
	CameraArm->CameraLagSpeed = 9.f;
	CameraArm->bEnableCameraRotationLag = true;
	CameraArm->CameraRotationLagSpeed = 9.f;
	CameraArm->ProbeSize = 16.f;
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(CameraArm);
	Camera->SetFieldOfView(70.f);

	AutoPossessPlayer = EAutoReceiveInput::Disabled;
}

UGTAIVVehicleSet* AGTAIVVehicle::LoadVehicleSet()
{
	return LoadObject<UGTAIVVehicleSet>(nullptr, TEXT("/Game/GTAIV/Traffic/DA_GTAIV_Vehicles.DA_GTAIV_Vehicles"));
}

void AGTAIVVehicle::Setup(const FGTAIVVehicleDef& InDef, const FGTAIVPaint& InPaint)
{
	Def = InDef;
	Paint = InPaint;
	const FGTAIVHandling& H = Def.Handling;
	Body->SetStaticMesh(Def.Body);
	AGTAIVTrafficCar::ApplyPaint(Body, Paint);

	// the chassis box is the body's bounds; the actor sits at its centre
	FVector Centre = FVector::ZeroVector, Ext(220.f, 90.f, 70.f);
	if (Def.Body)
	{
		const FBox B = Def.Body->GetBoundingBox();
		Centre = MeshToActor(B.GetCenter());
		Ext = AbsMeshToActor(B.GetExtent());
	}
	Body->SetRelativeLocation(-Centre);
	Chassis->SetBoxExtent(FVector(Ext.X * 0.95f, Ext.Y * 0.9f, Ext.Z * 0.8f));

	// wheels: meshes on the body (mesh space), physics in actor space
	Travel = FMath::Max(0.05f, H.SuspensionUpperLimit - H.SuspensionLowerLimit);
	Wheels.Reset();
	for (UStaticMeshComponent* W : WheelMeshes)
	{
		if (W)
		{
			W->DestroyComponent();
		}
	}
	WheelMeshes.Reset();
	for (int32 i = 0; i < Def.Wheels.Num(); ++i)
	{
		const FGTAIVWheel& W = Def.Wheels[i];
		FWheelState S;
		S.Rest = MeshToActor(W.Offset) - Centre - FVector(0, 0, H.SuspensionRaise * 100.f);
		S.Radius = FMath::Max(W.Radius, 10.f);
		S.bFront = S.Rest.X > 0.f;
		S.bSteers = W.bSteers;
		S.bRight = W.bRightSide;
		S.Compression = S.LastCompression = 1.f / FMath::Max(0.5f, H.SuspensionForce * FMath::Max(1, Def.Wheels.Num()));
		Wheels.Add(S);
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("Wheel%d"), i));
		C->SetMobility(EComponentMobility::Movable);
		C->SetStaticMesh(W.Mesh);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetupAttachment(Body);
		C->SetRelativeLocation(W.Offset);
		C->SetRelativeRotation(FRotator(0.f, W.bRightSide ? 180.f : 0.f, 0.f));
		C->RegisterComponent();
		AGTAIVTrafficCar::ApplyPaint(C, Paint);
		WheelMeshes.Add(C);
	}

	// mass and centre of mass from handling.dat (GTA axes: x right, y forward, z up, metres)
	Chassis->SetSimulatePhysics(true);
	Chassis->SetMassOverrideInKg(NAME_None, FMath::Max(50.f, H.Mass), true);
	Chassis->SetCenterOfMass(FVector(H.CentreOfMass.Y, H.CentreOfMass.X, H.CentreOfMass.Z) * 100.f + MeshToActor(FVector::ZeroVector) - Centre);
	Chassis->SetLinearDamping(0.02f);
	Chassis->SetAngularDamping(0.25f);

	if (DoorMesh)
	{
		DoorMesh->DestroyComponent();
		DoorMesh = nullptr;
	}
	if (Def.Door)
	{
		DoorMesh = NewObject<UStaticMeshComponent>(this, TEXT("Door"));
		DoorMesh->SetMobility(EComponentMobility::Movable);
		DoorMesh->SetStaticMesh(Def.Door);
		DoorMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		DoorMesh->SetupAttachment(Body);
		DoorMesh->SetRelativeLocation(Def.DoorHinge);
		DoorMesh->RegisterComponent();
		AGTAIVTrafficCar::ApplyPaint(DoorMesh, Paint);
	}
	CameraArm->TargetArmLength = FMath::Max(420.f, Def.Length * 1.0f + 230.f);
	CameraArm->SocketOffset = FVector(0.f, 0.f, 35.f + Ext.Z);
	bBuilt = true;
}

namespace
{
	/** A traffic car's driver, pulled out by the player: a pedestrian in the same clothes, thrown
	 *  to the road, who then runs (or fights). */
	void SpawnJackedDriver(UWorld* W, USkeletalMesh* Look, const FVector& Loc, const FRotator& Rot, const FVector& Out, APawn* Jacker)
	{
		TSubclassOf<APawn> Cls;
		for (TActorIterator<AGTAIVPedManager> It(W); It; ++It)
		{
			if (It->PedClass)
			{
				Cls = It->PedClass;
				break;
			}
		}
		if (!Cls || !Look)
		{
			return;
		}
		FActorSpawnParameters SP;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		APawn* Ped = W->SpawnActor<APawn>(Cls, Loc, Rot, SP);
		if (!Ped)
		{
			return;
		}
		if (!Ped->GetController())
		{
			if (AAIController* C = W->SpawnActor<AAIController>(AAIController::StaticClass(), Loc, Rot))
			{
				C->Possess(Ped);
			}
		}
		TArray<UChildActorComponent*> Children;
		Ped->GetComponents(Children);
		for (UChildActorComponent* C : Children)
		{
			AActor* Child = C->GetChildActor();
			if (Child && Child->GetClass()->GetName().Contains(TEXT("GTAIV_Ped")))
			{
				if (USkeletalMeshComponent* M = Child->FindComponentByClass<USkeletalMeshComponent>())
				{
					M->SetSkeletalMeshAsset(Look);
				}
			}
		}
		if (!Out.IsNearlyZero())
		{
			UGTAIVPlayerComponent::RagdollActor(Ped, (Out * 260.f + FVector(0, 0, 120.f)) * 12.f, Loc);
		}
		if (Jacker)
		{
			UGTAIVPedBrain::Threaten(Ped, Jacker, false);
		}
	}
}

AGTAIVVehicle* AGTAIVVehicle::TakeOver(AGTAIVTrafficCar* Traffic, APawn* Jacker)
{
	if (!Traffic || !Traffic->Def.Body)
	{
		return nullptr;
	}
	UWorld* W = Traffic->GetWorld();
	const FTransform T = Traffic->GetActorTransform();
	USkeletalMesh* DriverLook = Traffic->DriverMesh ? Cast<USkeletalMesh>(Traffic->DriverMesh->GetSkinnedAsset()) : nullptr;
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AGTAIVVehicle* V = W->SpawnActor<AGTAIVVehicle>(AGTAIVVehicle::StaticClass(), T, SP);
	if (!V)
	{
		return nullptr;
	}
	const FGTAIVVehicleDef D = Traffic->Def;
	const FGTAIVPaint P = Traffic->Paint;
	Traffic->Destroy();
	V->Setup(D, P);
	// the traffic car's origin is the body mesh's origin: put the body back where it was
	const FBox B = D.Body->GetBoundingBox();
	V->SetActorLocation(T.GetLocation() + T.GetRotation().RotateVector(MeshToActor(B.GetCenter())), false, nullptr, ETeleportType::TeleportPhysics);
	// the driver is still at the wheel: the player pulls them out (jack animations)
	const UGTAIVVehicleAnims* Anims = UGTAIVVehicleAnims::Get();
	const FGTAIVVehicleAnimSet* Set = Anims ? Anims->Find(D.AnimGroup) : nullptr;
	if (DriverLook && Set)
	{
		V->Jacker = Jacker;
		V->VictimMesh = NewObject<UPoseableMeshComponent>(V, TEXT("JackedDriver"));
		V->VictimMesh->SetSkinnedAssetAndUpdate(DriverLook);
		V->VictimMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		V->VictimMesh->RegisterComponent();
		UGTAIVVehicleAnims::PoseDriver(V->VictimMesh, *Set, 0.f, 0.f, 0.f);
		const FQuat Facing = V->GetActorQuat() * FQuat(FVector::UpVector, FMath::DegreesToRadians(-90.f));
		V->VictimMesh->SetWorldLocationAndRotation(V->GetSeatLocation() - Facing.RotateVector(UGTAIVVehicleAnims::PelvisRefLocation(V->VictimMesh)), Facing);
		V->VictimMesh->AttachToComponent(V->Chassis, FAttachmentTransformRules::KeepWorldTransform);
	}
	else if (DriverLook)
	{
		const FVector Left = -T.GetRotation().GetRightVector();
		SpawnJackedDriver(W, DriverLook, T.GetLocation() + Left * (D.Width * 0.5f + 90.f) + FVector(0, 0, 100.f), Left.Rotation(), Left, Jacker);
	}
	return V;
}

AGTAIVVehicle* AGTAIVVehicle::SpawnVehicle(UObject* WorldContext, FName VehicleName, FTransform Transform)
{
	UWorld* W = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGTAIVVehicleSet* Set = LoadVehicleSet();
	if (!W || !Set)
	{
		return nullptr;
	}
	const FGTAIVVehicleDef* D = Set->Vehicles.FindByPredicate([&](const FGTAIVVehicleDef& X) { return X.Name == VehicleName; });
	if (!D || !D->Body)
	{
		return nullptr;
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// Transform is on the ground: lift the body to its ride height
	const FBox B = D->Body->GetBoundingBox();
	Transform.AddToTranslation(Transform.GetRotation().GetUpVector() * (D->RideHeight + MeshToActor(B.GetCenter()).Z + 15.f));
	AGTAIVVehicle* V = W->SpawnActor<AGTAIVVehicle>(AGTAIVVehicle::StaticClass(), Transform, SP);
	if (V)
	{
		const FGTAIVPaint P = D->Paints.Num() ? D->Paints[FMath::RandRange(0, D->Paints.Num() - 1)] : FGTAIVPaint();
		V->Setup(*D, P);
	}
	return V;
}

void AGTAIVVehicle::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UGTAIVRadio* Radio = UGTAIVRadio::Get(this))
	{
		if (Cast<APlayerController>(GetController()) || Radio->GetPlayingFrom() == this)
		{
			Radio->SetListening(false, INDEX_NONE);
		}
	}
	Super::EndPlay(Reason);
}

void AGTAIVVehicle::SetPassenger(USkeletalMesh* Mesh)
{
	ClearPassenger();
	const UGTAIVVehicleAnims* Anims = UGTAIVVehicleAnims::Get();
	const FGTAIVVehicleAnimSet* Set = Anims ? Anims->Find(Def.AnimGroup) : nullptr;
	if (!Mesh || !Set)
	{
		return;
	}
	PassengerMesh = NewObject<UPoseableMeshComponent>(this, TEXT("Passenger"));
	PassengerMesh->SetSkinnedAssetAndUpdate(Mesh);
	PassengerMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PassengerMesh->SetupAttachment(Chassis);
	// GTA peds face the mesh's +Y; the car faces the actor's +X
	PassengerMesh->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	PassengerMesh->RegisterComponent();
	PassengerTime = FMath::FRandRange(0.f, 6.f);
	UGTAIVVehicleAnims::PoseDriver(PassengerMesh, *Set, PassengerTime, 0.f, 0.f, true);
	PassengerMesh->RefreshBoneTransforms();
	FVector SeatBody = Def.PassengerSeat;
	if (SeatBody.IsNearlyZero())
	{
		SeatBody = Def.Seat * FVector(-1.f, 1.f, 1.f);   // mirror the driver's across the car
	}
	const FVector Seat = Body->GetRelativeTransform().TransformPosition(SeatBody) + FVector(0.f, 0.f, UGTAIVVehicleAnims::SeatToPelvis(Def.AnimGroup));
	PassengerMesh->SetRelativeLocation(Seat - FRotator(0.f, -90.f, 0.f).RotateVector(UGTAIVVehicleAnims::PelvisRefLocation(PassengerMesh)));
}

void AGTAIVVehicle::ClearPassenger()
{
	if (PassengerMesh)
	{
		PassengerMesh->DestroyComponent();
		PassengerMesh = nullptr;
	}
}

void AGTAIVVehicle::PlaceBodyAt(const FTransform& BodyWorld)
{
	const FTransform Actor = Body->GetRelativeTransform().Inverse() * BodyWorld;
	SetActorTransform(FTransform(FRotator(0.f, Actor.Rotator().Yaw, 0.f), Actor.GetLocation() + FVector(0.f, 0.f, 20.f)),
					  false, nullptr, ETeleportType::TeleportPhysics);
}

FVector AGTAIVVehicle::GetSeatLocation() const
{
	if (Def.Seat.IsNearlyZero())
	{
		// no seat bone exported: front left, a little above the chassis centre
		return GetActorTransform().TransformPosition(FVector(Def.Length * 0.02f, -Def.Width * 0.22f, 0.f));
	}
	return Body->GetComponentTransform().TransformPosition(Def.Seat) + GetActorUpVector() * UGTAIVVehicleAnims::SeatToPelvis(Def.AnimGroup);
}

float AGTAIVVehicle::GetSteerAmount() const
{
	return FMath::Clamp(SteerAngle / FMath::Max(1.f, Def.Handling.SteeringLock), -1.f, 1.f);
}

FString AGTAIVVehicle::GetDisplayName() const
{
	return Def.DisplayName.IsEmpty() ? Def.Name.ToString().ToUpper() : Def.DisplayName;
}

float AGTAIVVehicle::GetSpeedKmh() const
{
	return GetVelocity().Size() * 0.036f;
}

FString AGTAIVVehicle::GetDebugState() const
{
	int32 Contacts = 0, Skids = 0;
	for (const FWheelState& W : Wheels)
	{
		Contacts += W.bContact;
		Skids += W.bSkid;
	}
	int32 Playing = 0;
	for (const UAudioComponent* A : EngineAudio)
	{
		Playing += A && A->IsPlaying();
	}
	return FString::Printf(TEXT("%s %.0f km/h gear %d revs %.2f steer %.1f contacts %d skids %d throttle %.2f hb %d | engine %s rpm %.2f fade %.2f loops %d door %.2f locked %d victim %s"),
						   *Def.Name.ToString(), GetSpeedKmh(), Gear, Revs, SteerAngle, Contacts, Skids, ThrottleInput, bHandbrake,
						   bEngineOn ? TEXT("on") : TEXT("off"), EngineRpm, EngineFade, Playing, DoorOpen, bInputLocked,
						   VictimMesh ? (VictimTime >= 0.f ? TEXT("pulled") : TEXT("seated")) : TEXT("-"));
}

// ------------------------------------------------------------------ driver

void AGTAIVVehicle::EnterDriver(APawn* Character)
{
	if (!Character || Driver.IsValid())
	{
		return;
	}
	AController* C = Character->GetController();
	Driver = Character;
	SetMovementTick(Character, false);
	Character->SetActorEnableCollision(false);
	Character->SetActorLocation(GetActorLocation() + GetActorUpVector() * 30.f, false, nullptr, ETeleportType::TeleportPhysics);
	Character->AttachToComponent(Chassis, FAttachmentTransformRules::KeepWorldTransform);
	// shown sitting at the wheel (GTA IV's driving animations), otherwise hidden
	UGTAIVPlayerComponent* Seat = Character->FindComponentByClass<UGTAIVPlayerComponent>();
	const bool bShown = Seat && Seat->SetSeat(this, VictimMesh != nullptr);
	Character->SetActorHiddenInGame(!bShown);
	bInputLocked = bShown && Seat->IsBusyInVehicle();
	if (C)
	{
		C->Possess(this);
		C->SetControlRotation(FRotator(-7.f, GetActorRotation().Yaw, 0.f));
		if (APlayerController* PC = Cast<APlayerController>(C))
		{
			if (AGTAIVPlayerHUD* HUD = Cast<AGTAIVPlayerHUD>(PC->GetHUD()))
			{
				HUD->ShowVehicleName(GetDisplayName());
			}
			// the car's radio comes on (a random station the first time, like the game)
			if (UGTAIVRadio* Radio = UGTAIVRadio::Get(this))
			{
				if (RadioStation == -2)
				{
					RadioStation = Radio->RandomStation();
				}
				Radio->SetListening(true, RadioStation);
			}
		}
	}
	bHandbrake = false;
	bExitRequested = false;
	ExitHeld = 0.f;
	NoExitUntil = GetWorld()->GetTimeSeconds() + 0.6;
	if (Cast<APlayerController>(C))
	{
		SetEngine(true);
	}
}

void AGTAIVVehicle::SetDoorOpen(float Amount)
{
	DoorOpen = FMath::Clamp(Amount, 0.f, 1.f);
	if (DoorMesh)
	{
		// the driver's (left) door swings out about its hinge
		DoorMesh->SetRelativeRotation(FRotator(0.f, 65.f * DoorOpen, 0.f));
	}
}

FVector AGTAIVVehicle::GetEntryLocation() const
{
	const FVector SeatLocal = GetActorTransform().InverseTransformPosition(GetSeatLocation());
	const FVector Local(SeatLocal.X + 15.f, -(Def.Width * 0.5f + 40.f), 0.f);
	FVector At = GetActorTransform().TransformPosition(Local);
	FHitResult Hit;
	FCollisionQueryParams Params(TEXT("GTAIVEntry"), false, this);
	if (Driver.IsValid())
	{
		Params.AddIgnoredActor(Driver.Get());
	}
	if (GetWorld()->LineTraceSingleByChannel(Hit, At + FVector(0, 0, 150.f), At - FVector(0, 0, 400.f), ECC_Visibility, Params))
	{
		return Hit.ImpactPoint;
	}
	return At - FVector(0, 0, Def.RideHeight);
}

void AGTAIVVehicle::PlayDoorSound(bool bOpen)
{
	OneShot(bOpen ? Def.Sounds.DoorOpen.Get() : Def.Sounds.DoorClose.Get(), 1.f);
}

void AGTAIVVehicle::OneShot(USoundBase* Sound, float Volume, float Pitch)
{
	if (Sound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, Sound, GetActorLocation(), Volume, Pitch);
	}
}

void AGTAIVVehicle::FinishBail()
{
	APawn* P = Driver.Get();
	if (!P)
	{
		return;
	}
	const FVector Vel = GetVelocity();
	if (ExitDriver(true))
	{
		// thrown clear of the car, rolling along the road at the car's speed
		UGTAIVPlayerComponent::RagdollActor(P, (Vel * 0.6f - GetActorRightVector() * 250.f + FVector(0, 0, 120.f)) * 15.f,
											P->GetActorLocation());
		bHandbrake = false;  // the empty car rolls on, door open
	}
}

void AGTAIVVehicle::StartVictim()
{
	if (!VictimMesh || VictimTime >= 0.f)
	{
		return;
	}
	VictimTime = 0.f;
	VictimFrom = VictimMesh->GetRelativeTransform();
}

void AGTAIVVehicle::UpdateVictim(float Dt)
{
	if (!VictimMesh || VictimTime < 0.f)
	{
		return;
	}
	const UGTAIVVehicleAnims* Anims = UGTAIVVehicleAnims::Get();
	const FGTAIVVehicleAnimSet* Set = Anims ? Anims->Find(Def.AnimGroup) : nullptr;
	const float Dur = Set && Set->Jacked.Frames > 1 ? Set->Jacked.Duration() : 3.f;
	VictimTime += Dt;
	const FVector Bob = Set ? UGTAIVVehicleAnims::PoseFull(VictimMesh, Set->Jacked, VictimTime) : FVector::ZeroVector;
	// dragged from the seat to the road beside the door (behind the jacker), on their feet
	const FQuat Facing = GetActorQuat() * FQuat(FVector::UpVector, FMath::DegreesToRadians(-90.f));
	const FVector Stand = GetEntryLocation() - GetActorForwardVector() * 70.f - GetActorRightVector() * 60.f;
	const FVector Sit = GetSeatLocation() - Facing.RotateVector(UGTAIVVehicleAnims::PelvisRefLocation(VictimMesh));
	const float A = FMath::SmoothStep(0.15f, 0.75f, VictimTime / Dur);
	VictimMesh->SetWorldLocationAndRotation(FMath::Lerp(Sit, Stand, A) + Facing.RotateVector(Bob) * (1.f - A), Facing);
	VictimMesh->RefreshBoneTransforms();
	if (VictimTime >= Dur)
	{
		// on their feet: a pedestrian again, who runs (or fights)
		USkeletalMesh* Look = Cast<USkeletalMesh>(VictimMesh->GetSkinnedAsset());
		const FVector Away = -GetActorRightVector();
		VictimMesh->DestroyComponent();
		VictimMesh = nullptr;
		SpawnJackedDriver(GetWorld(), Look, Stand + FVector(0, 0, 100.f), Away.Rotation(), FVector::ZeroVector, Jacker.Get());
	}
}

void AGTAIVVehicle::SetEngine(bool bOn)
{
	if (bOn == bEngineOn)
	{
		return;
	}
	bEngineOn = bOn;
	const FGTAIVVehicleSounds& S = Def.Sounds;
	if (bOn)
	{
		OneShot(S.Ignition, 0.8f);
		OneShot(S.Start, 1.f);
		StartDelay = 0.6f;
		if (EngineAudio.Num() == 0)
		{
			USoundBase* Loops[6] = {S.EngineLow ? S.EngineLow.Get() : S.EngineHigh.Get(), S.EngineHigh ? S.EngineHigh.Get() : S.EngineLow.Get(),
									S.ExhaustLow ? S.ExhaustLow.Get() : S.ExhaustHigh.Get(), S.ExhaustHigh ? S.ExhaustHigh.Get() : S.ExhaustLow.Get(),
									S.EngineIdle ? S.EngineIdle.Get() : S.ExhaustIdle.Get(), S.ExhaustIdle ? S.ExhaustIdle.Get() : S.EngineIdle.Get()};
			for (int32 i = 0; i < 6; ++i)
			{
				UAudioComponent* A = NewObject<UAudioComponent>(this, *FString::Printf(TEXT("Engine%d"), i));
				A->bAutoActivate = false;
				A->bAllowSpatialization = false;
				A->SetSound(Loops[i]);
				A->SetupAttachment(Chassis);
				A->RegisterComponent();
				A->SetVolumeMultiplier(0.f);
				EngineAudio.Add(A);
			}
		}
		for (UAudioComponent* A : EngineAudio)
		{
			if (A && A->GetSound())
			{
				A->Play(FMath::FRand());
			}
		}
	}
	else
	{
		OneShot(S.Shutdown, 0.8f);
	}
}

void AGTAIVVehicle::UpdateSounds(float Dt)
{
	// revs: from the road speed in the gear (or free revving standing still), like GTA IV's
	const float VFwd = FVector::DotProduct(GetVelocity(), GetActorForwardVector()) / 100.f;
	const float InGear = FMath::Clamp(FMath::Abs(VFwd) / FMath::Max(1.f, GearTopSpeed(Gear)), 0.f, 1.f);
	const float Throttle = bEngineOn ? FMath::Abs(ThrottleInput) : 0.f;
	float Target = 0.12f;
	if (FMath::Abs(VFwd) > 1.5f)
	{
		Target = FMath::Lerp(0.25f, 1.f, InGear) * (Throttle > 0.05f ? 1.f : 0.85f);
	}
	else if (Throttle > 0.05f)
	{
		Target = 0.35f + 0.35f * Throttle;
	}
	if (Gear != LastGear)
	{
		if (Gear > LastGear && bEngineOn)
		{
			if (const UGTAIVVehicleSet* Set = LoadVehicleSet())
			{
				OneShot(Set->GearSound, 0.35f);
			}
		}
		LastGear = Gear;
	}
	EngineRpm = FMath::FInterpTo(EngineRpm, Target, Dt, Target > EngineRpm ? 5.f : 3.f);
	// lifting off at high revs: the revs-off burble
	RevsOffCooldown -= Dt;
	if (bEngineOn && LastThrottle > 0.5f && Throttle < 0.1f && EngineRpm > 0.55f && RevsOffCooldown <= 0.f)
	{
		OneShot(Def.Sounds.RevsOff, 0.7f, 0.8f + EngineRpm * 0.3f);
		RevsOffCooldown = 2.f;
	}
	LastThrottle = Throttle;

	StartDelay -= Dt;
	EngineFade = FMath::FInterpConstantTo(EngineFade, bEngineOn && StartDelay <= 0.f ? 1.f : 0.f, Dt, bEngineOn ? 1.5f : 1.f);
	if (EngineAudio.Num() == 6)
	{
		const float R = EngineRpm;
		const float Idle = 1.f - FMath::SmoothStep(0.15f, 0.35f, R);
		const float Low = FMath::SmoothStep(0.1f, 0.3f, R) * (1.f - FMath::SmoothStep(0.55f, 0.8f, R));
		const float High = FMath::SmoothStep(0.5f, 0.8f, R);
		const float Load = 0.55f + 0.45f * Throttle;
		const float Vol[6] = {Low * 0.8f, High * 0.8f, Low * Load, High * Load, Idle * 0.8f, Idle * 0.7f};
		const float Pitch[6] = {0.7f + R * 0.9f, 0.6f + R * 0.7f, 0.7f + R * 0.9f, 0.6f + R * 0.7f, 1.f + R * 0.5f, 1.f + R * 0.5f};
		for (int32 i = 0; i < 6; ++i)
		{
			if (UAudioComponent* A = EngineAudio[i])
			{
				A->SetVolumeMultiplier(Vol[i] * EngineFade);
				A->SetPitchMultiplier(Pitch[i]);
				if (EngineFade <= 0.f && !bEngineOn && A->IsPlaying())
				{
					A->Stop();
				}
			}
		}
	}

	// tyres: GTA IV's tarmac skid loop under sliding wheels
	int32 Skids = 0;
	for (const FWheelState& W : Wheels)
	{
		Skids += W.bSkid;
	}
	const float Speed = GetVelocity().Size() / 100.f;
	const float SkidVol = Skids ? FMath::Clamp(Skids / 3.f, 0.3f, 1.f) * FMath::Clamp(Speed / 8.f, 0.f, 1.f) : 0.f;
	if (SkidVol > 0.02f && !SkidAudio)
	{
		if (const UGTAIVVehicleSet* Set = LoadVehicleSet())
		{
			if (Set->SkidSound)
			{
				SkidAudio = UGameplayStatics::SpawnSoundAttached(Set->SkidSound, Chassis, NAME_None, FVector::ZeroVector,
																   EAttachLocation::KeepRelativeOffset, false, 0.f);
				if (SkidAudio)
				{
					SkidAudio->bAutoDestroy = false;
				}
			}
		}
	}
	if (SkidAudio)
	{
		SkidAudio->SetVolumeMultiplier(FMath::FInterpTo(SkidAudio->VolumeMultiplier, SkidVol, Dt, 10.f));
		SkidAudio->SetPitchMultiplier(0.9f + FMath::Clamp(Speed / 40.f, 0.f, 0.25f));
	}
}

void AGTAIVVehicle::SetPlayerCooldown(APawn* P) const
{
	if (UGTAIVPlayerComponent* PC = P ? P->FindComponentByClass<UGTAIVPlayerComponent>() : nullptr)
	{
		PC->VehicleCooldownUntil = GetWorld()->GetTimeSeconds() + 0.6;
	}
}

void AGTAIVVehicle::BailOut()
{
	APawn* P = Driver.Get();
	if (!P)
	{
		return;
	}
	UGTAIVPlayerComponent* Seat = P->FindComponentByClass<UGTAIVPlayerComponent>();
	if (Seat && Seat->StartJumpOut())
	{
		bInputLocked = true;  // jump_out_ds plays, then FinishBail throws them clear
		bExitRequested = false;
		return;
	}
	FinishBail();
}

bool AGTAIVVehicle::ExitDriver(bool bForce)
{
	APawn* P = Driver.Get();
	if (!P || (!bForce && GetSpeedKmh() > 15.f))
	{
		return false;
	}
	AController* C = GetController();
	if (Cast<APlayerController>(C))
	{
		if (UGTAIVRadio* Radio = UGTAIVRadio::Get(this))
		{
			RadioStation = Radio->GetStation();
			Radio->LeaveInVehicle(this);  // it plays on from the car, softer as you walk away
		}
		SetEngine(false);
		if (HornAudio)
		{
			HornAudio->Stop();
		}
	}
	bInputLocked = false;
	bExitBraking = false;
	if (C)
	{
		C->UnPossess();
	}
	if (UGTAIVPlayerComponent* Seat = P->FindComponentByClass<UGTAIVPlayerComponent>())
	{
		Seat->SetSeat(nullptr);
	}
	P->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	// standing at the driver's door, where the get-out animation left them
	const FVector Out = GetEntryLocation() + FVector(0, 0, 100.f);
	P->SetActorLocationAndRotation(Out, FRotator(0.f, GetActorRotation().Yaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
	P->SetActorHiddenInGame(false);
	P->SetActorEnableCollision(true);
	SetMovementTick(P, true);
	if (C)
	{
		C->Possess(P);
		C->SetControlRotation(FRotator(0.f, GetActorRotation().Yaw, 0.f));
	}
	SetPlayerCooldown(P);
	bExitRequested = false;
	ExitHeld = 0.f;
	Driver = nullptr;
	ThrottleInput = 0.f;
	SteerInput = 0.f;
	bHandbrake = true;  // parked
	return true;
}

// ------------------------------------------------------------------ frame

float AGTAIVVehicle::GearTopSpeed(int32 Gr) const
{
	const FGTAIVHandling& H = Def.Handling;
	const int32 N = FMath::Max(1, H.Gears);
	return H.MaxSpeed / 3.6f * 1.1f * FMath::Pow(float(FMath::Clamp(Gr, 1, N)) / N, 0.9f) * 1.04f;
}

void AGTAIVVehicle::ReadInput(float Dt)
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return;
	}
	float T = ThrottleInput, S = SteerInput;
	if (!bExternalInput)
	{
		T = 0.f;
		S = 0.f;
	}
	// the phone is up: the arrow keys are its
	const AGTAIVPlayerHUD* PhoneHud = Cast<AGTAIVPlayerHUD>(PC->GetHUD());
	const bool bPhoneUp = PhoneHud && PhoneHud->IsPhoneOpen();
	// the horn: H or the left stick (held)
	const bool bHorn = !bInputLocked && Down(PC, {EKeys::H, EKeys::Gamepad_LeftThumbstick});
	if (bHorn && Def.Sounds.Horn)
	{
		if (!HornAudio)
		{
			HornAudio = UGameplayStatics::SpawnSoundAttached(Def.Sounds.Horn, Chassis, NAME_None, FVector::ZeroVector,
															   EAttachLocation::KeepRelativeOffset, false, 1.f);
			if (HornAudio)
			{
				HornAudio->bAutoDestroy = false;
			}
		}
		else if (!HornAudio->IsPlaying())
		{
			HornAudio->Play();
		}
	}
	else if (HornAudio && HornAudio->IsPlaying())
	{
		HornAudio->Stop();
	}
	if (Down(PC, {EKeys::W}) || (!bPhoneUp && Down(PC, {EKeys::Up})))
	{
		T += 1.f;
	}
	if (Down(PC, {EKeys::S}) || (!bPhoneUp && Down(PC, {EKeys::Down})))
	{
		T -= 1.f;
	}
	T += PC->GetInputAnalogKeyState(EKeys::Gamepad_RightTriggerAxis) - PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftTriggerAxis);
	if (Down(PC, {EKeys::D}) || (!bPhoneUp && Down(PC, {EKeys::Right})))
	{
		S += 1.f;
	}
	if (Down(PC, {EKeys::A}) || (!bPhoneUp && Down(PC, {EKeys::Left})))
	{
		S -= 1.f;
	}
	S += PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX);
	ThrottleInput = bInputLocked ? 0.f : FMath::Clamp(T, -1.f, 1.f);
	SteerInput = bInputLocked ? 0.f : FMath::Clamp(S, -1.f, 1.f);
	bHandbrake = Down(PC, {EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom}) || (bExternalInput && bHandbrake);

	// camera: mouse / right stick look around; after a moment it swings back behind the car
	float DX = 0.f, DY = 0.f;
	PC->GetInputMouseDelta(DX, DY);
	DX += PC->GetInputAnalogKeyState(EKeys::Gamepad_RightX) * 140.f * Dt;
	DY += PC->GetInputAnalogKeyState(EKeys::Gamepad_RightY) * 90.f * Dt;
	FRotator R = PC->GetControlRotation();
	if (FMath::Abs(DX) + FMath::Abs(DY) > 0.01f)
	{
		LookIdle = 0.f;
		R.Yaw += DX * 1.2f;
		R.Pitch = FMath::Clamp(FRotator::NormalizeAxis(R.Pitch + DY * 1.2f), -60.f, 25.f);
	}
	else
	{
		LookIdle += Dt;
		const FVector V = GetVelocity();
		if (LookIdle > 1.5f && V.Size() > 300.f)
		{
			const float Fwd = FVector::DotProduct(V, GetActorForwardVector());
			const float WantYaw = Fwd > 0.f ? V.Rotation().Yaw : GetActorRotation().Yaw;
			R.Yaw = FMath::FixedTurn(FRotator::NormalizeAxis(R.Yaw), FRotator::NormalizeAxis(WantYaw), 120.f * Dt * FMath::Min(1.f, LookIdle - 1.5f));
			R.Pitch = FMath::FInterpTo(FRotator::NormalizeAxis(R.Pitch), -7.f, Dt, 2.f);
		}
	}
	R.Roll = 0.f;
	PC->SetControlRotation(R);

	// the radio: mouse wheel, comma / period, D-pad left / right
	if (UGTAIVRadio* Radio = UGTAIVRadio::Get(this))
	{
		if (Pressed(PC, {EKeys::MouseScrollUp, EKeys::Period, EKeys::Gamepad_DPad_Right}))
		{
			Radio->Retune(1);
		}
		else if (Pressed(PC, {EKeys::MouseScrollDown, EKeys::Comma, EKeys::Gamepad_DPad_Left}))
		{
			Radio->Retune(-1);
		}
	}
	// GTA IV: F stops the car and gets out; held at speed, the driver jumps out
	if (GetWorld()->GetTimeSeconds() >= NoExitUntil && !bInputLocked)
	{
		if (Pressed(PC, {EKeys::F, EKeys::Enter, EKeys::Gamepad_FaceButton_Top}))
		{
			bExitRequested = true;
		}
		ExitHeld = Down(PC, {EKeys::F, EKeys::Enter, EKeys::Gamepad_FaceButton_Top}) ? ExitHeld + Dt : 0.f;
	}
}

void AGTAIVVehicle::Simulate(float Dt)
{
	const FGTAIVHandling& H = Def.Handling;
	const int32 N = Wheels.Num();
	if (N == 0 || !Chassis->IsSimulatingPhysics())
	{
		return;
	}
	const float M = FMath::Max(50.f, H.Mass);
	const FTransform T = GetActorTransform();
	const FVector Up = T.GetRotation().GetUpVector();
	const FVector Fwd = T.GetRotation().GetForwardVector();
	const FVector Vel = Chassis->GetPhysicsLinearVelocity() / 100.f;  // m/s
	const float VFwd = FVector::DotProduct(Vel, Fwd);
	const float Speed = Vel.Size();
	const float MPerWheel = M / N;
	FCollisionQueryParams Params(TEXT("GTAIVWheel"), false, this);
	if (Driver.IsValid())
	{
		Params.AddIgnoredActor(Driver.Get());
	}

	// steering: GTA's lock, less of it at speed, eased in and out
	const float SpeedFactor = FMath::Lerp(1.f, 0.45f, FMath::Clamp(Speed / 45.f, 0.f, 1.f));
	SteerAngle = FMath::FInterpConstantTo(SteerAngle, SteerInput * H.SteeringLock * SpeedFactor, Dt, H.SteeringLock * 3.f);

	// gearbox: forward gears change by road speed
	const float VMax = FMath::Max(10.f, H.MaxSpeed / 3.6f * 1.1f);  // drag keeps the car just under it
	const int32 NGears = FMath::Max(1, H.Gears);
	if (VFwd > GearTopSpeed(Gear) * 0.97f && Gear < NGears)
	{
		++Gear;
	}
	else if (Gear > 1 && VFwd < GearTopSpeed(Gear - 1) * 0.72f)
	{
		--Gear;
	}

	// what the pedals ask for: drive forwards / backwards, or brake
	float Drive = 0.f, Brake = bExitBraking ? 1.f : 0.f;
	bool bReverse = false;
	if (ThrottleInput > 0.05f)
	{
		if (VFwd < -1.f)
		{
			Brake = ThrottleInput;
		}
		else
		{
			Drive = ThrottleInput;
		}
	}
	else if (ThrottleInput < -0.05f)
	{
		if (VFwd > 1.f)
		{
			Brake = -ThrottleInput;
		}
		else
		{
			Drive = ThrottleInput;
			bReverse = true;
		}
	}
	// the engine responds as fast as its drive inertia lets it
	Revs = FMath::FInterpConstantTo(Revs, FMath::Abs(Drive), Dt, 2.5f / FMath::Max(0.2f, H.DriveInertia));
	float DriveForce = 0.f;  // N, total
	if (Revs > 0.f)
	{
		if (bReverse)
		{
			const float Curve = FMath::Clamp(1.f - FMath::Pow(FMath::Abs(VFwd) / (VMax * 0.3f), 3.f), 0.f, 1.f);
			DriveForce = -M * H.DriveForce * G * 2.0f * 1.2f * Curve * Revs;
		}
		else
		{
			const float GearMult = 1.45f - 0.55f * float(Gear - 1) / FMath::Max(1, NGears - 1);
			const float Curve = FMath::Clamp(1.f - FMath::Pow(FMath::Max(0.f, VFwd) / VMax, 3.f), 0.f, 1.f);
			DriveForce = M * H.DriveForce * G * 2.0f * GearMult * Curve * Revs;
		}
	}
	const float BrakeTotal = M * H.BrakeForce * G * 4.f * Brake;
	int32 FrontContacts = 0, RearContacts = 0;
	for (const FWheelState& W : Wheels)
	{
		(W.bFront ? FrontContacts : RearContacts) += W.bContact;
	}
	int32 FrontCount = 0, RearCount = 0;
	for (const FWheelState& W : Wheels)
	{
		(W.bFront ? FrontCount : RearCount)++;
	}

	// suspension: per wheel a ray from the top of its travel; force = SuspensionForce * mass * g per
	// metre of compression (so a car at rest sits 1 / (force * wheels) m into its travel)
	float AxleComp[2][2] = {{0, 0}, {0, 0}};  // [front][right]
	for (FWheelState& W : Wheels)
	{
		const FVector Top = T.TransformPosition(W.Rest + FVector(0, 0, H.SuspensionUpperLimit * 100.f));
		const float RayLen = Travel * 100.f + W.Radius;
		FHitResult Hit;
		W.bContact = GetWorld()->LineTraceSingleByChannel(Hit, Top, Top - Up * RayLen, ECC_Visibility, Params);
		W.LastCompression = W.Compression;
		if (!W.bContact)
		{
			W.Compression = 0.f;
			W.Load = 0.f;
			W.bSkid = false;
			continue;
		}
		const float CentreDist = Hit.Distance - W.Radius;  // cm from the top of travel to the wheel centre
		W.Compression = FMath::Clamp((Travel * 100.f - CentreDist) / 100.f, 0.f, Travel);
		W.Contact = Hit.ImpactPoint;
		W.Normal = Hit.ImpactNormal;
		const float Bias = W.bFront ? 2.f * H.SuspensionBias : 2.f * (1.f - H.SuspensionBias);
		const float K = H.SuspensionForce * M * G * Bias;
		const float CompVel = (W.Compression - W.LastCompression) / Dt;
		const float Damp = (CompVel > 0.f ? H.SuspensionCompDamp : H.SuspensionReboundDamp) * 0.7f * FMath::Sqrt(K * MPerWheel);
		float F = K * W.Compression + Damp * CompVel;
		if (CentreDist < 0.f)
		{
			F += K * 15.f * (-CentreDist / 100.f);  // bottomed out
		}
		W.Load = FMath::Max(0.f, F);
		Chassis->AddForceAtLocation(Up * W.Load * 100.f, Top);
		AxleComp[W.bFront][W.bRight] = W.Compression;
	}
	// a little anti-roll: GTA IV's cars lean a lot in corners, but shouldn't topple at the first one
	for (FWheelState& W : Wheels)
	{
		if (!W.bContact)
		{
			continue;
		}
		const float Diff = AxleComp[W.bFront][W.bRight] - AxleComp[W.bFront][!W.bRight];
		const float K = H.SuspensionForce * M * G * 0.12f;
		Chassis->AddForceAtLocation(Up * K * Diff * 100.f, T.TransformPosition(W.Rest));
	}

	// tyres: GTA's traction curve; grip rises to TractionCurveMax at the peak slip angle
	// (TractionCurveLateral) and falls to TractionCurveMin past it; TractionBias shares it front/rear
	for (FWheelState& W : Wheels)
	{
		W.bSkid = false;
		if (!W.bContact || W.Load <= 0.f)
		{
			continue;
		}
		const FVector Nrm = W.Normal;
		FVector WFwd = Fwd;
		if (W.bSteers)
		{
			WFwd = FQuat(Up, FMath::DegreesToRadians(SteerAngle)).RotateVector(Fwd);
		}
		WFwd = (WFwd - Nrm * FVector::DotProduct(WFwd, Nrm)).GetSafeNormal();
		const FVector WRight = FVector::CrossProduct(Nrm, WFwd);
		const FVector V = Chassis->GetPhysicsLinearVelocityAtPoint(W.Contact) / 100.f;
		const float VLong = FVector::DotProduct(V, WFwd);
		const float VLatRaw = FVector::DotProduct(V, WRight);
		// traction spring: the tyre's sideways deflection builds up over TractionSpringDeltaMax metres of
		// rolling, so grip comes in with a slight lag like GTA IV's instead of snapping on
		const float Delta = FMath::Clamp(H.TractionSpringDeltaMax, 0.03f, 0.5f);
		W.LatDeflect = FMath::Clamp((W.LatDeflect + VLatRaw * Dt) / (1.f + FMath::Abs(VLong) * Dt / Delta), -Delta, Delta);
		const float VLat = FMath::Abs(VLong) > 2.f ? W.LatDeflect * FMath::Abs(VLong) / Delta : VLatRaw;
		const float TBias = W.bFront ? 2.f * H.TractionBias : 2.f * (1.f - H.TractionBias);
		const float MuMax = H.TractionCurveMax * TBias;
		const float MuMin = H.TractionCurveMin * TBias;
		const bool bHB = bHandbrake && !W.bFront;
		const float StopForce = MPerWheel / Dt;  // force that would stop this corner's sliding this step

		// lateral
		float Fy;
		const float Alpha = FMath::RadiansToDegrees(FMath::Atan2(FMath::Abs(VLat), FMath::Max(FMath::Abs(VLong), 0.5f)));
		const float Ratio = Alpha / FMath::Max(1.f, H.TractionCurveLateral);
		float MuLat = Ratio <= 1.f ? MuMax * Ratio : FMath::Lerp(MuMax, MuMin, FMath::Clamp(Ratio - 1.f, 0.f, 1.f));
		if (bHB)
		{
			MuLat = FMath::Min(MuLat, MuMin * 0.55f);
		}
		Fy = -FMath::Sign(VLat) * FMath::Min(MuLat * W.Load, FMath::Abs(VLat) * StopForce * 0.6f);

		// longitudinal: drive, brakes, handbrake, rolling resistance; holds still when parked
		float Fx = 0.f;
		const int32 AxleContacts = W.bFront ? FrontContacts : RearContacts;
		const float AxleShare = W.bFront ? H.DriveBias : 1.f - H.DriveBias;
		if (AxleContacts > 0 && AxleShare > 0.f)
		{
			Fx += DriveForce * AxleShare / AxleContacts;
		}
		const float BrakeShare = W.bFront ? H.BrakeBias / FMath::Max(1, FrontCount) : (1.f - H.BrakeBias) / FMath::Max(1, RearCount);
		float BrakeF = BrakeTotal * BrakeShare;
		if (bHB)
		{
			BrakeF += M * H.HandBrakeForce * G * 0.9f / FMath::Max(1, RearCount);
		}
		if (FMath::Abs(Drive) < 0.05f && Brake < 0.05f && !bHB && Speed < 0.6f)
		{
			BrakeF = FMath::Max(BrakeF, MPerWheel * G);  // parked: don't creep
		}
		Fx -= FMath::Sign(VLong) * FMath::Min(BrakeF, FMath::Abs(VLong) * StopForce * 0.8f);
		// engine braking on the driven wheels when off the throttle
		if (FMath::Abs(Drive) < 0.05f && AxleShare > 0.f && AxleContacts > 0)
		{
			Fx -= FMath::Sign(VLong) * FMath::Min(1.f, FMath::Abs(VLong) / 3.f) * M * G * 0.05f * AxleShare / AxleContacts;
		}
		// rolling resistance: ~1.5% of the load, eased in near standstill
		Fx -= FMath::Sign(VLong) * FMath::Min(1.f, FMath::Abs(VLong)) * W.Load * 0.015f;

		// friction circle: past the peak the tyre slides at the lower grip
		const float Mag = FMath::Sqrt(Fx * Fx + Fy * Fy);
		const float Limit = MuMax * W.Load;
		if (Mag > Limit && Mag > 1.f)
		{
			const float Scale = FMath::Min(Limit, MuMin * W.Load * (bHB ? 0.8f : 1.f)) / Mag;
			Fx *= Scale;
			Fy *= Scale;
			W.bSkid = Speed > 3.f;
		}
		W.bSkid |= Ratio > 1.1f && Speed > 5.f;
		const FVector At = W.Contact + Nrm * (W.Radius * 0.4f);
		Chassis->AddForceAtLocation((WFwd * Fx + WRight * Fy) * 100.f, At);
		W.SpinSpeed = FMath::RadiansToDegrees(VLong / (W.Radius / 100.f));
		if (bHB || (Brake > 0.5f && W.bSkid))
		{
			W.SpinSpeed = 0.f;  // locked
		}
	}

	// in the air: the car keeps its attitude like GTA IV's (some angular damping) and after a
	// moment of real airtime it can be tipped a little (throttle: nose down, steer: roll)
	int32 Grounded = 0;
	for (const FWheelState& W : Wheels)
	{
		Grounded += W.bContact;
	}
	AirTime = Grounded == 0 ? AirTime + Dt : 0.f;
	Chassis->SetAngularDamping(AirTime > 0.1f ? 1.2f : 0.25f);
	if (AirTime > 0.4f && Driver.IsValid())
	{
		const FVector Right = T.GetRotation().GetRightVector();
		Chassis->AddTorqueInRadians(Right * ThrottleInput * 0.5f - Fwd * SteerInput * 0.7f, NAME_None, true);
	}

	// air drag (handling.dat's drag multiplier)
	Chassis->AddForce(-Vel * Speed * H.DragMult * 0.035f * 100.f);
}

void AGTAIVVehicle::HitPeds()
{
	// people the car runs into are knocked over (their ragdoll takes the hit)
	const FVector Vel = GetVelocity();
	if (Vel.Size() < 250.f)
	{
		return;
	}
	TArray<FOverlapResult> Hits;
	FCollisionQueryParams Params(TEXT("GTAIVCarHit"), false, this);
	if (Driver.IsValid())
	{
		Params.AddIgnoredActor(Driver.Get());
	}
	const FVector Ext = Chassis->GetScaledBoxExtent() + FVector(40.f, 25.f, 10.f);
	GetWorld()->OverlapMultiByObjectType(Hits, GetActorLocation() + Vel * 0.03f, GetActorQuat(),
										 FCollisionObjectQueryParams(ECC_Pawn), FCollisionShape::MakeBox(Ext), Params);
	const double Now = GetWorld()->GetTimeSeconds();
	for (const FOverlapResult& O : Hits)
	{
		APawn* Ped = Cast<APawn>(O.GetActor());
		if (!Ped || Ped == Driver.Get() || Ped->IsA<AGTAIVVehicle>())
		{
			continue;
		}
		// once per person per second
		const double* Last = RecentHits.Find(Ped);
		if (Last && Now - *Last < 1.0)
		{
			continue;
		}
		RecentHits.Add(Ped, Now);
		// the push lands on one bone (~12 kg): about the car's speed for that bone
		const FVector Push = (Vel * 0.9f + FVector(0, 0, Vel.Size() * 0.25f)) * 12.f;
		UGTAIVPlayerComponent::RagdollActor(Ped, Push, Ped->GetActorLocation() + FVector(0, 0, -20.f));
		// a fast hit hurts the player too
		if (UGTAIVPlayerComponent* PC = Ped->FindComponentByClass<UGTAIVPlayerComponent>())
		{
			PC->TakeHit(FMath::Max(0.f, Vel.Size() * 0.036f - 20.f) * 1.5f, GetActorLocation());
		}
	}
}

void AGTAIVVehicle::UpdateWheelVisuals(float Dt)
{
	const FGTAIVHandling& H = Def.Handling;
	for (int32 i = 0; i < Wheels.Num() && i < WheelMeshes.Num(); ++i)
	{
		FWheelState& W = Wheels[i];
		const FGTAIVWheel& D = Def.Wheels[i];
		if (!W.bContact)
		{
			W.SpinSpeed *= 0.98f;
		}
		W.Spin = FMath::Fmod(W.Spin + W.SpinSpeed * Dt, 360.f);
		// the wheel sits where its suspension is (rest = static compression)
		const float Static = 1.f / FMath::Max(0.5f, H.SuspensionForce * FMath::Max(1, Wheels.Num()));
		const float Dz = (W.Compression - Static) * 100.f + (W.bContact ? 0.f : 0.f) - H.SuspensionRaise * 100.f;
		WheelMeshes[i]->SetRelativeLocation(D.Offset + FVector(0, 0, W.bContact ? Dz : -(Travel - Static) * 100.f));
		const FQuat Side(FVector::UpVector, FMath::DegreesToRadians(D.bRightSide ? 180.f : 0.f));
		const FQuat Steer(FVector::UpVector, FMath::DegreesToRadians(D.bSteers ? SteerAngle : 0.f));
		const FQuat Roll(FVector::ForwardVector, FMath::DegreesToRadians(D.bRightSide ? -W.Spin : W.Spin));
		WheelMeshes[i]->SetRelativeRotation(Steer * Side * Roll);
	}
}

void AGTAIVVehicle::Tick(float DeltaSeconds)
{
	if (PassengerMesh && PassengerMesh->WasRecentlyRendered(0.3f))
	{
		PassengerTime += DeltaSeconds;
		const UGTAIVVehicleAnims* PA = UGTAIVVehicleAnims::Get();
		if (const FGTAIVVehicleAnimSet* PS = PA ? PA->Find(Def.AnimGroup) : nullptr)
		{
			UGTAIVVehicleAnims::PoseDriver(PassengerMesh, *PS, PassengerTime, 0.f, 0.f, true);
		}
	}
	Super::Tick(DeltaSeconds);
	if (!bBuilt)
	{
		return;
	}
	const float Dt = FMath::Clamp(DeltaSeconds, 0.001f, 1.f / 30.f);
	ReadInput(Dt);
	if (!Driver.IsValid() && !GetController() && !bExternalInput)
	{
		ThrottleInput = 0.f;
		SteerInput = 0.f;
	}
	if (bExitRequested && Driver.IsValid())
	{
		const bool bUpsideDown = GetActorUpVector().Z < 0.3f;
		UGTAIVPlayerComponent* Seat = Driver->FindComponentByClass<UGTAIVPlayerComponent>();
		const bool bAnimated = !bUpsideDown && Seat && Seat->IsSeated();
		if (bAnimated && GetSpeedKmh() < 4.f && Seat->StartExit())
		{
			// stopped: open the door and get out (held still meanwhile)
			bExitRequested = false;
			bInputLocked = true;
			bExitBraking = true;
		}
		else if (!bAnimated && (GetSpeedKmh() < 15.f || bUpsideDown))
		{
			ExitDriver(true);
		}
		else if (bAnimated && GetSpeedKmh() < 15.f)
		{
			ThrottleInput = 0.f;
			SteerInput = 0.f;
			bExitBraking = true;  // nearly stopped: brake the rest of the way
		}
		else if (ExitHeld > 0.35f)
		{
			BailOut();
		}
		else
		{
			// brake to a stop first
			ThrottleInput = FVector::DotProduct(GetVelocity(), GetActorForwardVector()) > 0.f ? -1.f : 1.f;
			SteerInput = 0.f;
		}
	}
	Simulate(Dt);
	HitPeds();
	UpdateWheelVisuals(Dt);
	UpdateVictim(DeltaSeconds);
	if (bEngineOn || EngineFade > 0.f || SkidAudio)
	{
		UpdateSounds(Dt);
	}
}
