#include "GTAIVVehicleAnims.h"

#include "Components/PoseableMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

UGTAIVVehicleAnims* UGTAIVVehicleAnims::Get()
{
	static TWeakObjectPtr<UGTAIVVehicleAnims> Cached;
	static double LastTry = -100.0;
	if (!Cached.IsValid())
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - LastTry < 5.0)
		{
			return nullptr;  // not exported: don't search on every spawn
		}
		LastTry = Now;
		Cached = LoadObject<UGTAIVVehicleAnims>(nullptr, TEXT("/Game/GTAIV/Traffic/DA_GTAIV_VehicleAnims.DA_GTAIV_VehicleAnims"),
												nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	return Cached.Get();
}

const FGTAIVVehicleAnimSet* UGTAIVVehicleAnims::Find(FName Group) const
{
	if (const FGTAIVVehicleAnimSet* S = Sets.Find(Group))
	{
		return S;
	}
	return Sets.Find(TEXT("std"));
}

void UGTAIVVehicleAnims::PoseDriver(UPoseableMeshComponent* Mesh, const FGTAIVVehicleAnimSet& Set, float Time, float Steer,
									float Reverse, bool bPassenger)
{
	if (!Mesh || !Mesh->GetSkinnedAsset())
	{
		return;
	}
	const TArray<FTransform>& RefPose = Mesh->GetSkinnedAsset()->GetRefSkeleton().GetRefBonePose();
	if (Mesh->BoneSpaceTransforms.Num() != RefPose.Num())
	{
		return;
	}
	// from the reference pose (pelvis on the seat), every joint turned by the clips
	Mesh->BoneSpaceTransforms = RefPose;
	const FGTAIVAnimClip& Sit = bPassenger && Set.SitPassenger.Frames > 0 ? Set.SitPassenger : Set.Sit;
	UGTAIVPlayerComponent::PoseClip(Mesh, Sit, FMath::Fmod(Time, FMath::Max(0.1f, Sit.Duration())), 1.f, nullptr);
	if (!bPassenger)
	{
		const FGTAIVAnimClip& Turn = Steer < 0.f ? Set.SteerLeft : Set.SteerRight;
		const float A = FMath::Abs(Steer);
		UGTAIVPlayerComponent::PoseClip(Mesh, Turn, A * 0.8f, FMath::Clamp(A * 2.5f, 0.f, 1.f), nullptr);
		if (Reverse > 0.f)
		{
			UGTAIVPlayerComponent::PoseClip(Mesh, Set.Reverse, 0.5f, Reverse, nullptr);
		}
	}
	Mesh->MarkRefreshTransformDirty();
}

FVector UGTAIVVehicleAnims::PoseFull(UPoseableMeshComponent* Mesh, const FGTAIVAnimClip& Clip, float Time)
{
	if (!Mesh || !Mesh->GetSkinnedAsset())
	{
		return FVector::ZeroVector;
	}
	const TArray<FTransform>& RefPose = Mesh->GetSkinnedAsset()->GetRefSkeleton().GetRefBonePose();
	if (Mesh->BoneSpaceTransforms.Num() == RefPose.Num())
	{
		Mesh->BoneSpaceTransforms = RefPose;
	}
	UGTAIVPlayerComponent::PoseClip(Mesh, Clip, Time, 1.f, nullptr);
	Mesh->MarkRefreshTransformDirty();
	if (Clip.PelvisOffset.Num() == 0 || Clip.Frames == 0)
	{
		return FVector::ZeroVector;
	}
	const float F = FMath::Clamp(Time * Clip.Fps, 0.f, float(FMath::Min(Clip.Frames, Clip.PelvisOffset.Num()) - 1));
	const int32 F0 = FMath::FloorToInt(F);
	const int32 F1 = FMath::Min(F0 + 1, Clip.PelvisOffset.Num() - 1);
	return FMath::Lerp(Clip.PelvisOffset[F0], Clip.PelvisOffset[F1], F - F0);
}

FVector UGTAIVVehicleAnims::PelvisRefLocation(const UPoseableMeshComponent* Mesh)
{
	if (!Mesh || !Mesh->GetSkinnedAsset())
	{
		return FVector::ZeroVector;
	}
	const FReferenceSkeleton& Ref = Mesh->GetSkinnedAsset()->GetRefSkeleton();
	int32 B = Ref.FindBoneIndex(TEXT("Char_Pelvis"));
	FTransform T = FTransform::Identity;
	while (B != INDEX_NONE)
	{
		T = T * Ref.GetRefBonePose()[B];
		B = Ref.GetParentIndex(B);
	}
	return T.GetLocation();
}

USkeletalMesh* UGTAIVVehicleAnims::RandomPedMesh()
{
	static TArray<TWeakObjectPtr<USkeletalMesh>> Pool;
	static bool bLoaded = false;
	if (!bLoaded)
	{
		bLoaded = true;
		UClass* Cls = LoadClass<UObject>(nullptr, TEXT("/Game/GTAIV/Characters/BP_GTAIV_Ped.BP_GTAIV_Ped_C"), nullptr,
										 LOAD_NoWarn | LOAD_Quiet);
		UObject* CDO = Cls ? Cls->GetDefaultObject() : nullptr;
		FArrayProperty* AP = Cls ? FindFProperty<FArrayProperty>(Cls, TEXT("PedMeshes")) : nullptr;
		FObjectPropertyBase* Inner = AP ? CastField<FObjectPropertyBase>(AP->Inner) : nullptr;
		if (CDO && Inner)
		{
			FScriptArrayHelper H(AP, AP->ContainerPtrToValuePtr<void>(CDO));
			for (int32 i = 0; i < H.Num(); ++i)
			{
				USkeletalMesh* M = Cast<USkeletalMesh>(Inner->GetObjectPropertyValue(H.GetRawPtr(i)));
				const FString N = M ? M->GetName() : FString();
				if (M && !N.Contains(TEXT("niko"), ESearchCase::IgnoreCase) && !N.Contains(TEXT("_ig_"), ESearchCase::IgnoreCase) &&
					!N.Contains(TEXT("_cs_"), ESearchCase::IgnoreCase))
				{
					Pool.Add(M);
				}
			}
		}
	}
	for (int32 Tries = 0; Tries < 4 && Pool.Num(); ++Tries)
	{
		if (USkeletalMesh* M = Pool[FMath::RandRange(0, Pool.Num() - 1)].Get())
		{
			return M;
		}
	}
	return nullptr;
}
