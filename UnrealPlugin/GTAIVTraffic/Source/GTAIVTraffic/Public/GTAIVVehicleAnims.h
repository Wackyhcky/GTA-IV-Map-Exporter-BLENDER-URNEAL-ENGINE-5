#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GTAIVPlayerComponent.h"
#include "GTAIVVehicleAnims.generated.h"

class UPoseableMeshComponent;
class USkeletalMesh;

/** One vehicle type's in-car animations (GTA IV's veh@std, veh@low, veh@truck, veh@van, veh@bus). */
USTRUCT(BlueprintType)
struct FGTAIVVehicleAnimSet
{
	GENERATED_BODY()

	/** sit_drive: the driver's idle loop. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip Sit;
	/** steer_l / steer_r: hand over hand to full lock (about 0.8 s in). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip SteerLeft;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip SteerRight;
	/** reverse: looking back over the shoulder. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip Reverse;
	/** sit_pass: a front passenger. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip SitPassenger;
	/** get_in_ds / get_out_ds: climbing in and out of the driver's seat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip GetIn;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip GetOut;
	/** jump_out_ds: diving out of a moving car. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip JumpOut;
	/** d_open_out_ds, d_close_in_ds, d_close_out_ds: the door. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip OpenDoor;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip CloseDoorInside;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip CloseDoorOutside;
	/** Pulling the driver out (the jacker's clip) and being pulled out (jack_driver_ds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip Jack;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") FGTAIVAnimClip Jacked;
};

/** GTA IV's driver animations, written by the GTA IV Exporter (/Game/GTAIV/Traffic/DA_GTAIV_VehicleAnims).
 *  Used to seat the player in the car and to show drivers in traffic. */
UCLASS(BlueprintType)
class GTAIVTRAFFIC_API UGTAIVVehicleAnims : public UDataAsset
{
	GENERATED_BODY()

public:
	/** By vehicles.ide animation group: std, low, truck, van, bus. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TMap<FName, FGTAIVVehicleAnimSet> Sets;

	static UGTAIVVehicleAnims* Get();

	/** How far above the seat bone (the cushion) the pelvis rides, cm: sports cars (low) sit lower. */
	static float SeatToPelvis(FName Group) { return Group == FName(TEXT("low")) ? -8.f : 3.f; }

	/** The group's set, or the standard car's. */
	const FGTAIVVehicleAnimSet* Find(FName Group) const;

	/** Poses a GTA ped mesh sitting at the wheel. Steer: -1 (left) .. 1, Reverse: 0..1 looking back.
	 *  The pelvis stays where the reference pose has it (GTA IV puts it on the seat). */
	static void PoseDriver(UPoseableMeshComponent* Mesh, const FGTAIVVehicleAnimSet& Set, float Time, float Steer,
						   float Reverse, bool bPassenger = false);

	/** Poses the whole body with one clip (from the reference pose); returns the clip's pelvis
	 *  movement at that time (component space, cm). */
	static FVector PoseFull(UPoseableMeshComponent* Mesh, const FGTAIVAnimClip& Clip, float Time);

	/** The pelvis of the mesh's reference pose, in component space. */
	static FVector PelvisRefLocation(const UPoseableMeshComponent* Mesh);

	/** A random everyday GTA IV ped (the ped Blueprint's PedMeshes, story characters left out). */
	static USkeletalMesh* RandomPedMesh();
};
