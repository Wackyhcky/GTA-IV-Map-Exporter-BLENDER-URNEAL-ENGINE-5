#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GTAIVTrafficData.h"
#include "GTAIVTrafficCar.generated.h"

class UStaticMeshComponent;
class UPoseableMeshComponent;

/** A GTA IV car driven by AGTAIVTrafficManager (moved kinematically along the lanes). */
UCLASS(NotPlaceable)
class GTAIVTRAFFIC_API AGTAIVTrafficCar : public AActor
{
	GENERATED_BODY()

public:
	AGTAIVTrafficCar();

	/** Builds the body and wheels and applies a paint combo (per-car custom primitive data). */
	void Setup(const FGTAIVVehicleDef& InDef, const FGTAIVPaint& InPaint);

	/** Colours a mesh's paint materials (MI_VEH_*_P1/P2/P3) from a carcols.dat combo. */
	static void ApplyPaint(UStaticMeshComponent* C, const FGTAIVPaint& Colours);

	/** Rolls the wheels by DistanceCm travelled and steers the front ones. */
	void UpdateWheels(float DistanceCm, float SteerDegrees);

	UPROPERTY(VisibleAnywhere, Category = "GTA IV")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "GTA IV")
	TObjectPtr<UStaticMeshComponent> Body;

	UPROPERTY(VisibleAnywhere, Category = "GTA IV")
	TArray<TObjectPtr<UStaticMeshComponent>> WheelComponents;

	/** A random GTA IV ped at the wheel, sitting and steering with GTA IV's animations. */
	UPROPERTY(VisibleAnywhere, Category = "GTA IV")
	TObjectPtr<UPoseableMeshComponent> DriverMesh;

	FName VehicleName;

	/** What it was built from (kept so a player can take the car over). */
	FGTAIVVehicleDef Def;
	FGTAIVPaint Paint;

private:
	TArray<FGTAIVWheel> WheelDefs;
	TArray<float> WheelRoll;
	float DriverTime = 0.f;
	float DriverSteer = 0.f;
	void SetupDriver();
};
