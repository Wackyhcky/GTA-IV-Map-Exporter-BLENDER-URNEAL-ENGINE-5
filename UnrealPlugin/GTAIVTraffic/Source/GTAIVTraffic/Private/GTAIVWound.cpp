#include "GTAIVPlayerComponent.h"

#include "Components/ChildActorComponent.h"
#include "Components/DecalComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/UnrealType.h"

namespace
{
	const int32 MaxDecals = 160;
	TArray<TWeakObjectPtr<UDecalComponent>> AllDecals;  // oldest first, shared by every wound

	/** The Mover movement mode of a Game Animation Sample character ("Ragdoll", "Walking", ...). */
	FString MoverMode(const AActor* Actor)
	{
		for (UActorComponent* C : Actor->GetComponents())
		{
			if (!C || !C->GetClass()->GetName().Contains(TEXT("MoverComponent")))
			{
				continue;
			}
			if (UFunction* F = C->FindFunction(TEXT("GetMovementModeName")))
			{
				uint8* Parms = static_cast<uint8*>(FMemory_Alloca(FMath::Max<int32>(1, F->ParmsSize)));
				FMemory::Memzero(Parms, FMath::Max<int32>(1, F->ParmsSize));
				C->ProcessEvent(F, Parms);
				if (const FNameProperty* Ret = CastField<FNameProperty>(F->GetReturnProperty()))
				{
					return Ret->GetPropertyValue_InContainer(Parms).ToString();
				}
			}
		}
		return FString();
	}

	bool IsHead(FName Bone)
	{
		const FString B = Bone.ToString().ToLower();
		return B.Contains(TEXT("head")) || B.Contains(TEXT("neck"));
	}
}

UGTAIVWoundComponent::UGTAIVWoundComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

FString UGTAIVWoundComponent::GetStateName() const
{
	switch (State)
	{
	case EState::Alive: return TEXT("Alive");
	case EState::Down: return TEXT("Down");
	case EState::Struggling: return TEXT("Struggling");
	case EState::Staggering: return TEXT("Staggering");
	case EState::Dying: return TEXT("Dying");
	default: return TEXT("Dead");
	}
}

void UGTAIVWoundComponent::ApplyHit(AActor* Target, const FHitResult& Hit, FVector Dir, UGTAIVWeaponData* Weapon, AActor* Shooter)
{
	if (!Target || Target == Shooter || !UGTAIVPedBrain::IsPed(Target))
	{
		return;  // shots into cars and props don't wound them
	}
	UGTAIVWoundComponent* W = Target->FindComponentByClass<UGTAIVWoundComponent>();
	if (!W)
	{
		W = NewObject<UGTAIVWoundComponent>(Target, TEXT("GTAIV_Wound"));
		W->RegisterComponent();
	}
	W->Data = Weapon;
	W->OnHit(Hit, Dir);
}

USkeletalMeshComponent* UGTAIVWoundComponent::Body() const
{
	// the character's own (simulated) mesh; a visual override copies its pose
	TArray<USkeletalMeshComponent*> Meshes;
	GetOwner()->GetComponents(Meshes, false);
	for (USkeletalMeshComponent* M : Meshes)
	{
		if (M->GetOwner() == GetOwner() && M->GetPhysicsAsset())
		{
			return M;
		}
	}
	return Meshes.Num() ? Meshes[0] : nullptr;
}

FVector UGTAIVWoundComponent::WoundLocation(const FWound& W) const
{
	const USkeletalMeshComponent* M = Body();
	if (!M || M->GetBoneIndex(W.Bone) == INDEX_NONE)
	{
		return GetOwner()->GetActorLocation();
	}
	return M->GetBoneTransform(M->GetBoneIndex(W.Bone)).TransformPosition(W.Local);
}

void UGTAIVWoundComponent::OnHit(const FHitResult& Hit, FVector Dir)
{
	USkeletalMeshComponent* M = Body();
	const FVector P = Hit.ImpactPoint;
	FName Bone = Hit.BoneName;
	if (M && (Bone.IsNone() || M->GetBoneIndex(Bone) == INDEX_NONE))
	{
		Bone = M->FindClosestBone(P);
	}
	if (M && !Bone.IsNone() && Wounds.Num() < 4)
	{
		const FTransform BT = M->GetBoneTransform(M->GetBoneIndex(Bone));
		FWound W{Bone, BT.InverseTransformPosition(P), 0.f, NAME_None, FVector::ZeroVector};
		// the same wound on the visible GTA IV ped, on its body's surface (the shot hits the
		// capsule, a little in front of the body)
		if (USkeletalMeshComponent* V = Visual())
		{
			W.VisualBone = V->FindClosestBone(P);
			if (!W.VisualBone.IsNone())
			{
				const FTransform VT = V->GetBoneTransform(V->GetBoneIndex(W.VisualBone));
				FVector L = VT.InverseTransformPosition(P);
				W.VisualLocal = L.GetClampedToMaxSize(13.f);
			}
		}
		Wounds.Add(W);
	}
	// keep the blood off the bodies themselves (the pool would paint them)
	TArray<UPrimitiveComponent*> Prims;
	GetOwner()->GetComponents(Prims, true);
	for (UPrimitiveComponent* C : Prims)
	{
		C->SetReceivesDecals(false);
	}

	Spray(P, Dir, 16, 380.f);
	// splatter on whatever is behind, and on the ground below
	FCollisionQueryParams Params(TEXT("GTAIVBlood"), false, GetOwner());
	FHitResult Behind;
	if (GetWorld()->LineTraceSingleByChannel(Behind, P, P + Dir * 260.f, ECC_Visibility, Params))
	{
		Splat(Behind.ImpactPoint, Behind.ImpactNormal, FMath::RandRange(45.f, 90.f), Data ? Data->BloodSplat : nullptr,
			  FMath::RandRange(0, 3));
	}
	FHitResult Below;
	if (GetWorld()->LineTraceSingleByChannel(Below, P, P - FVector(0, 0, 300.f), ECC_Visibility, Params))
	{
		Splat(Below.ImpactPoint, Below.ImpactNormal, FMath::RandRange(30.f, 55.f), Data ? Data->BloodSplat : nullptr,
			  FMath::RandRange(0, 3), 0.85f);
	}

	if (UGTAIVPedBrain* Brain = UGTAIVPedBrain::Get(GetOwner()))
	{
		Brain->OnShot();
		Brain->SayPain(true);
	}
	Health -= Data ? Data->Damage : 25.f;
	const bool bKill = Health <= 0.f || IsHead(Bone);
	switch (State)
	{
	case EState::Alive:
		StrugglesLeft = bKill ? 0 : FMath::RandRange(MinStruggles, MaxStruggles);
		bFailGetUp = FMath::FRand() < 0.35f;  // sometimes the first try to stand fails
		State = bKill ? EState::Dying : EState::Down;
		StateTime = 0.f;
		break;
	case EState::Down:
	case EState::Struggling:
	case EState::Staggering:
		// shot again while trying to get up / walking it off: that's it
		HoldWound(false);
		StrugglesLeft = 0;
		State = EState::Dying;
		StateTime = 0.f;
		break;
	default:
		break;
	}
	if (State != EState::Dead)
	{
		bSettled = false;
	}
	UGTAIVPlayerComponent::RagdollActor(GetOwner(), Dir * (Data ? Data->Impulse : 9000.f), P);
}

void UGTAIVWoundComponent::Collapse(float Push)
{
	const double Now = GetWorld()->GetTimeSeconds();
	if (Now - LastCollapse < 0.4)
	{
		return;
	}
	LastCollapse = Now;
	if (USkeletalMeshComponent* M = Body())
	{
		M->SetAllBodiesPhysicsBlendWeight(1.f);  // full ragdoll again
	}
	const FVector Dir = FRotator(0.f, FMath::FRandRange(0.f, 360.f), 0.f).Vector();
	UGTAIVPlayerComponent::RagdollActor(GetOwner(), Dir * Push, GetOwner()->GetActorLocation() + FVector(0, 0, 40.f));
}

void UGTAIVWoundComponent::Spray(FVector From, FVector Dir, int32 Count, float Speed)
{
	for (int32 i = 0; i < Count; ++i)
	{
		const FVector V = (Dir + FMath::VRand() * 0.6f + FVector(0, 0, 0.25f)).GetSafeNormal() * Speed * FMath::FRandRange(0.3f, 1.f);
		Drops.Add({From, V, 3.f, FMath::FRandRange(0.8f, 1.6f)});
	}
}

UDecalComponent* UGTAIVWoundComponent::Splat(FVector Location, FVector Normal, float Size, UTexture2D* Tex, int32 Cell, float Opacity)
{
	if (!Data || !Data->BloodDecal || !Tex)
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Data->BloodDecal, GetOwner());
	MID->SetTextureParameterValue(TEXT("Tex"), Tex);
	// blood_01 is a 2x2 atlas; Cell < 0 uses the whole texture
	const FLinearColor Atlas = Cell < 0 ? FLinearColor(1, 1, 0, 0) : FLinearColor(0.5f, 0.5f, 0.5f * (Cell % 2), 0.5f * (Cell / 2));
	MID->SetVectorParameterValue(TEXT("Atlas"), Atlas);
	MID->SetScalarParameterValue(TEXT("Opacity"), Opacity);
	FRotator Rot = (-Normal).Rotation();
	Rot.Roll = FMath::FRandRange(-180.f, 180.f);
	UDecalComponent* D = UGameplayStatics::SpawnDecalAtLocation(this, MID, FVector(12.f, Size * 0.5f, Size * 0.5f), Location, Rot, 0.f);
	if (D)
	{
		D->SetFadeScreenSize(0.f);
		AllDecals.Add(D);
		while (AllDecals.Num() > MaxDecals)
		{
			if (UDecalComponent* Old = AllDecals[0].Get())
			{
				Old->DestroyComponent();
			}
			AllDecals.RemoveAt(0);
		}
	}
	return D;
}

void UGTAIVWoundComponent::UpdateDrops(float Dt)
{
	if (!DropMeshes && Data && Data->BloodDropMesh)
	{
		// drops are simple spheres in world space, one instanced mesh per character
		DropMeshes = NewObject<UInstancedStaticMeshComponent>(GetOwner(), TEXT("GTAIV_BloodDrops"));
		DropMeshes->SetUsingAbsoluteLocation(true);
		DropMeshes->SetUsingAbsoluteRotation(true);
		DropMeshes->SetUsingAbsoluteScale(true);
		DropMeshes->SetStaticMesh(Data->BloodDropMesh);
		if (Data->BloodDrop)
		{
			DropMeshes->SetMaterial(0, Data->BloodDrop);
		}
		DropMeshes->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		DropMeshes->SetCastShadow(false);
		DropMeshes->RegisterComponent();
		DropMeshes->SetWorldTransform(FTransform::Identity);
	}
	FCollisionQueryParams Params(TEXT("GTAIVBloodDrop"), false, GetOwner());
	for (int32 i = Drops.Num() - 1; i >= 0; --i)
	{
		FDrop& D = Drops[i];
		D.Life -= Dt;
		D.Vel.Z -= 980.f * Dt;
		const FVector Next = D.Pos + D.Vel * Dt;
		FHitResult H;
		if (GetWorld()->LineTraceSingleByChannel(H, D.Pos, Next, ECC_Visibility, Params))
		{
			Splat(H.ImpactPoint, H.ImpactNormal, FMath::RandRange(9.f, 20.f) * D.Size, Data ? Data->BloodSplat : nullptr,
				  FMath::RandRange(0, 3), 0.9f);
			Drops.RemoveAtSwap(i);
			continue;
		}
		D.Pos = Next;
		if (D.Life <= 0.f)
		{
			Drops.RemoveAtSwap(i);
		}
	}
	if (DropMeshes)
	{
		DropMeshes->ClearInstances();
		for (const FDrop& D : Drops)
		{
			const float S = 0.012f * D.Size;
			DropMeshes->AddInstance(FTransform(D.Vel.Rotation(), D.Pos, FVector(S * 1.8f, S, S)), true);
		}
	}
}

void UGTAIVWoundComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	StateTime += DeltaTime;
	const FString Mode = MoverMode(GetOwner());
	const bool bHasMover = !Mode.IsEmpty();
	const bool bRagdoll = Mode.Contains(TEXT("Ragdoll"));

	switch (State)
	{
	case EState::Down:
		// GASP's ragdoll character gets up by itself once it lies still
		if (bHasMover && !bRagdoll && StateTime > 0.5f)
		{
			State = EState::Struggling;
			StateTime = 0.f;
			StruggleTime = bFailGetUp ? FMath::FRandRange(1.1f, 1.6f) : 1.7f;
			if (UGTAIVPedBrain* Brain = UGTAIVPedBrain::Get(GetOwner()))
			{
				Brain->SayPain(false);  // moaning while trying to stand
			}
		}
		else if (!bHasMover && StateTime > 2.5f)
		{
			State = EState::Dying;  // plain physics ragdolls can't get up
			StateTime = 0.f;
		}
		break;
	case EState::Struggling:
		if (bRagdoll)
		{
			State = EState::Down;  // knocked over again
			StateTime = 0.f;
		}
		else
		{
			// heave up: the ragdoll blends towards the character's standing animation (only half
			// way when this attempt fails)
			USkeletalMeshComponent* M = Body();
			const float Up = FMath::Clamp((StateTime - 0.2f) / 1.4f, 0.f, 1.f);
			if (M)
			{
				M->SetAllBodiesPhysicsBlendWeight(1.f - Up * (bFailGetUp ? 0.55f : 1.f));
				if (!bFailGetUp && Up >= 1.f)
				{
					M->SetAllBodiesSimulatePhysics(false);  // up: the animation carries it now
				}
			}
		}
		if (State == EState::Struggling && StateTime > StruggleTime && (bFailGetUp || StateTime > 1.65f))
		{
			if (bFailGetUp)
			{
				// the legs give way
				bFailGetUp = false;
				Collapse(2500.f);
				if (UGTAIVPedBrain* Brain = UGTAIVPedBrain::Get(GetOwner()))
				{
					Brain->SayPain(true);
				}
				State = EState::Down;
			}
			else
			{
				// up: walks a few steps holding the wound
				State = EState::Staggering;
				StaggerTime = FMath::FRandRange(4.f, 8.f);
				MoanTimer = 0.5f;
				FVector Away = GetOwner()->GetActorForwardVector();
				if (const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0))
				{
					Away = GetOwner()->GetActorLocation() - Player->GetActorLocation();
				}
				Away.Z = 0.f;
				StaggerDir = Away.GetSafeNormal().RotateAngleAxis(FMath::FRandRange(-50.f, 50.f), FVector::UpVector);
				HoldWound(true);
			}
			StateTime = 0.f;
		}
		break;
	case EState::Staggering:
	{
		if (bRagdoll)
		{
			HoldWound(false);
			State = EState::Down;
			StateTime = 0.f;
			break;
		}
		HoldWound(true);
		// a slow, unsteady walk
		StaggerDir = StaggerDir.RotateAngleAxis(FMath::FRandRange(-25.f, 25.f) * DeltaTime, FVector::UpVector);
		const float Speed = 95.f + 35.f * FMath::Sin(StateTime * 5.f);
		if (UGTAIVPedBrain* Brain = UGTAIVPedBrain::Get(GetOwner()))
		{
			Brain->MoveInDirection(StaggerDir, Speed);
			MoanTimer -= DeltaTime;
			if (MoanTimer <= 0.f)
			{
				MoanTimer = FMath::FRandRange(1.6f, 2.8f);
				Brain->SayPain(false);
			}
		}
		if (StateTime > StaggerTime)
		{
			HoldWound(false);
			StrugglesLeft = 0;
			Collapse(1800.f);
			if (UGTAIVPedBrain* Brain = UGTAIVPedBrain::Get(GetOwner()))
			{
				Brain->SayPain(true);
			}
			State = EState::Dying;
			StateTime = 0.f;
		}
		break;
	}
	case EState::Dying:
		if (bHasMover && !bRagdoll)
		{
			Collapse(1500.f);
		}
		if (!bSaidDeath && StateTime > 0.3f)
		{
			bSaidDeath = true;
			if (UGTAIVPedBrain* Brain = UGTAIVPedBrain::Get(GetOwner()))
			{
				Brain->SayDeath();
			}
		}
		if (StateTime > 1.5f)
		{
			State = EState::Dead;
			StateTime = 0.f;
			GetOwner()->Tags.AddUnique(TEXT("GTAIV_Dead"));
		}
		break;
	case EState::Dead:
		DeadTime += DeltaTime;
		if (bHasMover && !bRagdoll)
		{
			Collapse(500.f);  // stays down
		}
		if (!bSettled && StateTime > 2.f)
		{
			// no more getting up: GASP's ragdoll character decides that in its tick
			bSettled = true;
			if (bHasMover)
			{
				GetOwner()->SetActorTickEnabled(false);
			}
		}
		if (!Pool && StateTime > 0.8f && Data && Data->BloodPools.Num() > 0)
		{
			const USkeletalMeshComponent* M = Body();
			FVector At = GetOwner()->GetActorLocation();
			for (const TCHAR* B : {TEXT("spine_02"), TEXT("pelvis"), TEXT("Char_Spine"), TEXT("Char_Pelvis")})
			{
				if (M && M->GetBoneIndex(B) != INDEX_NONE)
				{
					At = M->GetBoneLocation(B);
					break;
				}
			}
			FCollisionQueryParams Params(TEXT("GTAIVBloodPool"), false, GetOwner());
			FHitResult Floor;
			if (GetWorld()->LineTraceSingleByChannel(Floor, At + FVector(0, 0, 50.f), At - FVector(0, 0, 250.f), ECC_Visibility, Params))
			{
				PoolSize = FMath::FRandRange(120.f, 180.f);
				Pool = Splat(Floor.ImpactPoint, Floor.ImpactNormal, 10.f,
							 Data->BloodPools[FMath::RandRange(0, Data->BloodPools.Num() - 1)], -1, 1.f);
			}
		}
		if (Pool)
		{
			// the pool spreads, quickly at first
			const float T = FMath::Clamp(StateTime / 16.f, 0.f, 1.f);
			const float S = FMath::Lerp(10.f, PoolSize, 1.f - FMath::Pow(1.f - T, 2.5f));
			Pool->DecalSize = FVector(12.f, S * 0.5f, S * 0.5f);
			Pool->MarkRenderStateDirty();
		}
		break;
	default:
		break;
	}

	// the wounds drip, faster while alive, stopping a while after death
	const float Rate = State == EState::Dead ? FMath::Max(0.f, 6.f - DeadTime * 0.4f) : 10.f;
	if (Rate > 0.f && Wounds.Num() > 0)
	{
		DripTimer -= DeltaTime;
		while (DripTimer <= 0.f)
		{
			DripTimer += 1.f / Rate * FMath::FRandRange(0.6f, 1.4f);
			const FWound& W = Wounds[FMath::RandRange(0, Wounds.Num() - 1)];
			Drops.Add({WoundLocation(W) + FMath::VRand() * 2.f, FMath::VRand() * 25.f, 3.f, FMath::FRandRange(0.6f, 1.2f)});
		}
	}
	UpdateDrops(DeltaTime);
}

void UGTAIVWoundComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	if (DropMeshes)
	{
		DropMeshes->DestroyComponent();
	}
	if (Reason == EEndPlayReason::EndPlayInEditor || Reason == EEndPlayReason::Quit)
	{
		AllDecals.Reset();
	}
	Super::EndPlay(Reason);
}


FString UGTAIVWoundComponent::GetDebugState() const
{
	const USkeletalMeshComponent* M = Body();
	FString Out = FString::Printf(TEXT("%s mode=%s body=%s"), *GetStateName(), *MoverMode(GetOwner()), M ? *M->GetName() : TEXT("-"));
	if (M && M->GetBoneIndex(TEXT("pelvis")) != INDEX_NONE)
	{
		const FVector P = M->GetBoneLocation(TEXT("pelvis"));
		const FVector A = GetOwner()->GetActorLocation();
		Out += FString::Printf(TEXT(" pelvisdz=%.0f d2=%.0f sim=%d"), P.Z - A.Z, FVector::Dist2D(P, A),
							   M->IsSimulatingPhysics(TEXT("pelvis")));
		if (Wounds.Num())
		{
			const FTransform AT = GetOwner()->GetActorTransform();
			const FVector W = AT.InverseTransformPosition(WoundLocation(Wounds[0]));
			Out += FString::Printf(TEXT(" wound=%s local(%.0f,%.0f,%.0f) off(%.0f,%.0f,%.0f)"), *Wounds[0].Bone.ToString(), W.X, W.Y, W.Z,
								   Wounds[0].Local.X, Wounds[0].Local.Y, Wounds[0].Local.Z);
		}
	}
	return Out;
}

USkeletalMeshComponent* UGTAIVWoundComponent::Visual() const
{
	// a GTA IV ped visual override (child actor with an SK_PED_ mesh)
	TArray<UChildActorComponent*> Kids;
	GetOwner()->GetComponents(Kids);
	for (UChildActorComponent* C : Kids)
	{
		if (AActor* Child = C->GetChildActor())
		{
			if (USkeletalMeshComponent* M = Child->FindComponentByClass<USkeletalMeshComponent>())
			{
				if (M->GetSkeletalMeshAsset() && M->GetSkeletalMeshAsset()->GetName().StartsWith(TEXT("SK_PED_")))
				{
					return M;
				}
			}
		}
	}
	return nullptr;
}

bool UGTAIVWoundComponent::IsStanding() const
{
	// up when the hips are back at the capsule (lying, they're near the floor)
	const USkeletalMeshComponent* M = Body();
	if (!M)
	{
		return true;
	}
	for (const TCHAR* B : {TEXT("pelvis"), TEXT("Char_Pelvis")})
	{
		if (M->GetBoneIndex(B) != INDEX_NONE)
		{
			const FVector P = M->GetBoneLocation(B);
			const FVector A = GetOwner()->GetActorLocation();
			return P.Z > A.Z - 40.f && FVector::Dist2D(P, A) < 60.f;
		}
	}
	return true;
}

void UGTAIVWoundComponent::HoldWound(bool bHold)
{
	// the pose layer of the ped's GTA IV visual (the same one the player's weapon uses)
	UGTAIVPlayerComponent* Pose = GetOwner()->FindComponentByClass<UGTAIVPlayerComponent>();
	if (!bHold)
	{
		if (Pose)
		{
			Pose->ClearHandTarget();
		}
		HandSide = -1;
		return;
	}
	if (!Pose)
	{
		Pose = NewObject<UGTAIVPlayerComponent>(GetOwner(), TEXT("GTAIV_PedPose"));
		Pose->bDressAsNiko = false;
		Pose->RegisterComponent();
	}
	if (Wounds.Num() == 0)
	{
		return;
	}
	const FWound& W = Wounds[0];
	const USkeletalMeshComponent* M = Body();
	FVector At = WoundLocation(W);
	const USkeletalMeshComponent* V = Visual();
	if (V && !W.VisualBone.IsNone() && V->GetBoneIndex(W.VisualBone) != INDEX_NONE)
	{
		At = V->GetBoneTransform(V->GetBoneIndex(W.VisualBone)).TransformPosition(W.VisualLocal);
		M = V;
	}
	// press on the body's surface, from the side the wound is on
	FVector Centre = GetOwner()->GetActorLocation();
	if (M)
	{
		for (const TCHAR* B : {TEXT("Char_Spine1"), TEXT("spine_02")})
		{
			if (M->GetBoneIndex(B) != INDEX_NONE)
			{
				Centre = M->GetBoneLocation(B);
				break;
			}
		}
	}
	At += (At - Centre).GetSafeNormal2D() * 4.f;
	// the hand on the wound's side; the other one when that arm is the one hit
	const FString Bone = W.Bone.ToString().ToLower();
	const bool bRightSide = FVector::DotProduct(At - Centre, GetOwner()->GetActorRightVector()) > 0.f;
	bool bLeft = !bRightSide;
	if (Bone.Contains(TEXT("_l")) && (Bone.Contains(TEXT("arm")) || Bone.Contains(TEXT("hand")) || Bone.Contains(TEXT("clavicle"))))
	{
		bLeft = false;
	}
	else if (Bone.Contains(TEXT("_r")) && (Bone.Contains(TEXT("arm")) || Bone.Contains(TEXT("hand")) || Bone.Contains(TEXT("clavicle"))))
	{
		bLeft = true;
	}
	if (HandSide < 0)
	{
		HandSide = bLeft ? 1 : 0;  // chosen once, so the hands don't swap while walking
	}
	Pose->SetHandTarget(HandSide == 1, At);
}
