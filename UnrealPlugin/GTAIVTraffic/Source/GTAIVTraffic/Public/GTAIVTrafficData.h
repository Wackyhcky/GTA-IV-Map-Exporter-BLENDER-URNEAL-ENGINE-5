#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
class USoundBase;

#include "GTAIVTrafficData.generated.h"

class UStaticMesh;

/** One wheel of a vehicle. Offsets are in the body mesh's own space (cm). */
USTRUCT(BlueprintType)
struct FGTAIVWheel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TObjectPtr<UStaticMesh> Mesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FVector Offset = FVector::ZeroVector;

	/** Right-side wheels use the same mesh turned 180 degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	bool bRightSide = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	bool bSteers = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	float Radius = 30.f;
};

/** A paint combination from carcols.dat (linear colours). */
USTRUCT(BlueprintType)
struct FGTAIVPaint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FLinearColor Primary = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FLinearColor Secondary = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FLinearColor Tertiary = FLinearColor::White;
};

/** A vehicle's row of GTA IV's handling.dat (units as in the file: metres, kg, km/h, degrees). */
USTRUCT(BlueprintType)
struct FGTAIVHandling
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body") float Mass = 1700.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body") float DragMult = 6.f;
	/** GTA axes: x right, y forward, z up (metres from the model's origin). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body") FVector CentreOfMass = FVector(0, 0, -0.2);
	/** 1 = front wheel drive, 0 = rear wheel drive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transmission") float DriveBias = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transmission") int32 Gears = 5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transmission") float DriveForce = 0.17f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transmission") float DriveInertia = 1.f;
	/** Top speed, km/h. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transmission") float MaxSpeed = 140.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transmission") float BrakeForce = 0.22f;
	/** Front share of the braking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transmission") float BrakeBias = 0.65f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transmission") float HandBrakeForce = 0.7f;
	/** Degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transmission") float SteeringLock = 35.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traction") float TractionCurveMax = 1.2f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traction") float TractionCurveMin = 0.95f;
	/** Slip angle of peak grip, degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traction") float TractionCurveLateral = 14.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traction") float TractionSpringDeltaMax = 0.13f;
	/** Front share of the grip (0.5 = even). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traction") float TractionBias = 0.47f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Suspension") float SuspensionForce = 1.6f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Suspension") float SuspensionCompDamp = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Suspension") float SuspensionReboundDamp = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Suspension") float SuspensionUpperLimit = 0.15f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Suspension") float SuspensionLowerLimit = -0.16f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Suspension") float SuspensionRaise = 0.f;
	/** Front share of the spring strength (0.5 = even). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Suspension") float SuspensionBias = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damage") float CollisionDamageMult = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damage") float EngineDamageMult = 1.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Misc") int32 ModelFlags = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Misc") int32 HandlingFlags = 0;
};

/** A car's GTA IV sounds (game.dat16's settings for it, the waves from streamed_vehicles.rpf and
 *  resident.rpf). The loops (engine, exhaust, idle, horn) loop. */
USTRUCT(BlueprintType)
struct FGTAIVVehicleSounds
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> EngineLow = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> EngineHigh = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> ExhaustLow = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> ExhaustHigh = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> EngineIdle = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> ExhaustIdle = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> RevsOff = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> Start = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> Ignition = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> Shutdown = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> Horn = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> DoorOpen = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TObjectPtr<USoundBase> DoorClose = nullptr;
};

USTRUCT(BlueprintType)
struct FGTAIVVehicleDef
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FName Name;

	/** Body mesh as exported: the car's nose points along the mesh's -Y axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TObjectPtr<UStaticMesh> Body = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TArray<FGTAIVWheel> Wheels;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TArray<FGTAIVPaint> Paints;

	/** Relative spawn chance (vehicles.ide frequency). 0 = never in traffic. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	float Frequency = 100.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	float Length = 450.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	float Width = 190.f;

	/** Height of the body mesh origin above the ground (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	float RideHeight = 60.f;

	/** Name shown in game (vehicles.ide game name, e.g. "Admiral"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FGTAIVHandling Handling;

	/** The driver's seat (seat_dside_f) in the body mesh's space; zero when unknown. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FVector Seat = FVector::ZeroVector;

	/** The front passenger's seat (seat_pside_f). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FVector PassengerSeat = FVector::ZeroVector;

	/** In-car animations (vehicles.ide): std, low, truck, van or bus. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FName AnimGroup = TEXT("std");

	/** The driver's door, cut out of the body (its hinge at the origin) so it can open. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TObjectPtr<UStaticMesh> Door = nullptr;

	/** The door's hinge in the body mesh's space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FVector DoorHinge = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	FGTAIVVehicleSounds Sounds;
};

/** GTA IV's cars, filled in by the exporter. */
UCLASS(BlueprintType)
class GTAIVTRAFFIC_API UGTAIVVehicleSet : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TArray<FGTAIVVehicleDef> Vehicles;

	/** Tyre skids (resident.rpf WHEEL_LOOPS) and the gear change, shared by every car. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Sounds") TObjectPtr<USoundBase> SkidSound;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Sounds") TObjectPtr<USoundBase> SideSkidSound;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Sounds") TObjectPtr<USoundBase> GearSound;
};

/** GTA IV's vehicle road network (paths.img), in world space (cm).
 *  Node i's drivable links are LinkTarget/LinkLanes[FirstLink[i] .. FirstLink[i] + LinkCount[i]). */
UCLASS(BlueprintType)
class GTAIVTRAFFIC_API UGTAIVRoadNetwork : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TArray<FVector> Nodes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TArray<int32> FirstLink;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TArray<int32> LinkCount;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TArray<int32> LinkTarget;

	/** Lanes in the direction node -> target. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TArray<int32> LinkLanes;
};
