#include "Modules/ModuleManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GTAIVTrafficData.h"
#include "GTAIVTrafficManager.h"

namespace
{
	/** A map from the GTA IV Map Exporter has no PlayerStart: the game would put everyone at the
	 *  world origin. Like GTA IV, players start on the pavement in Hove Beach (Niko's first
	 *  neighbourhood), spread over the nearby streets so several players don't land on top of
	 *  each other. Runs before anyone spawns; nothing is saved into the map. */
	void AddGTAIVPlayerStarts(const FActorsInitializedParams& Params)
	{
		UWorld* W = Params.World;
		if (!W || !W->IsGameWorld() || W->GetNetMode() == NM_Client)
		{
			return;
		}
		for (TActorIterator<APlayerStart> It(W); It; ++It)
		{
			return;  // the map has its own
		}
		const UGTAIVRoadNetwork* Net = nullptr;
		for (TActorIterator<AGTAIVTrafficManager> It(W); It && !Net; ++It)
		{
			Net = It->Network;
		}
		if (!Net || Net->Nodes.Num() == 0)
		{
			return;
		}
		// Hove Beach, by Niko's apartment (GTA IV (893, -500) in metres; Unreal flips Y)
		const FVector Home(89300.f, 50000.f, 1400.f);
		TArray<int32> Order;
		Order.Reserve(Net->Nodes.Num());
		for (int32 i = 0; i < Net->Nodes.Num(); ++i)
		{
			Order.Add(i);
		}
		Order.Sort([&](int32 A, int32 B) { return FVector::DistSquared2D(Net->Nodes[A], Home) < FVector::DistSquared2D(Net->Nodes[B], Home); });
		TArray<FVector> Spots;
		for (int32 i : Order)
		{
			if (Spots.Num() >= 12 || (Spots.Num() > 0 && FVector::Dist2D(Net->Nodes[i], Net->Nodes[Order[0]]) > 25000.f))
			{
				break;
			}
			// to the side of the road (the nodes run down its middle), facing along it
			FVector P = Net->Nodes[i];
			FVector Along = FVector::ForwardVector;
			if (Net->LinkCount.IsValidIndex(i) && Net->LinkCount[i] > 0 && Net->LinkTarget.IsValidIndex(Net->FirstLink[i]))
			{
				Along = (Net->Nodes[Net->LinkTarget[Net->FirstLink[i]]] - P).GetSafeNormal2D();
			}
			const FVector Side = FVector::CrossProduct(FVector::UpVector, Along).GetSafeNormal();
			P += Side * 650.f;
			bool bFar = true;
			for (const FVector& S : Spots)
			{
				bFar &= FVector::Dist2D(S, P) > 1500.f;
			}
			if (!bFar)
			{
				continue;
			}
			// onto the ground there when its collision is loaded (it is the node's height otherwise)
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, P + FVector(0, 0, 300.f), P - FVector(0, 0, 500.f), ECC_Visibility))
			{
				P.Z = Hit.ImpactPoint.Z;
			}
			Spots.Add(P);
			FActorSpawnParameters SP;
			SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SP.ObjectFlags |= RF_Transient;
			W->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), P + FVector(0, 0, 100.f), Along.Rotation(), SP);
		}
		UE_LOG(LogTemp, Log, TEXT("GTA IV: %d player starts in Hove Beach (the map has none), first at %s"), Spots.Num(), Spots.Num() ? *Spots[0].ToString() : TEXT("-"));
	}
}

class FGTAIVTrafficModule : public FDefaultModuleImpl
{
public:
	virtual void StartupModule() override
	{
		Handle = FWorldDelegates::OnWorldInitializedActors.AddStatic(&AddGTAIVPlayerStarts);
	}
	virtual void ShutdownModule() override
	{
		FWorldDelegates::OnWorldInitializedActors.Remove(Handle);
	}

private:
	FDelegateHandle Handle;
};

IMPLEMENT_MODULE(FGTAIVTrafficModule, GTAIVTraffic)
