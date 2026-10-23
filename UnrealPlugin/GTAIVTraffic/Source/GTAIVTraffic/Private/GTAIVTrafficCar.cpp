#include "GTAIVTrafficCar.h"

#include "Components/PoseableMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GTAIVVehicleAnims.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"

AGTAIVTrafficCar::AGTAIVTrafficCar()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Root);
	Body->SetMobility(EComponentMobility::Movable);
	// exported meshes point their nose along -Y; turn them to face the actor's +X
	Body->SetRelativeRotation(FRotator(0.f, 90.f, 0.f));
	Body->SetCollisionProfileName(TEXT("BlockAllDynamic"));
	// ground traces from other cars (and this one) shouldn't hit car bodies
	Body->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	Body->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
}

void AGTAIVTrafficCar::Setup(const FGTAIVVehicleDef& InDef, const FGTAIVPaint& InPaint)
{
	Def = InDef;
	Paint = InPaint;
	VehicleName = Def.Name;
	Body->SetStaticMesh(Def.Body);
	WheelDefs = Def.Wheels;
	WheelRoll.Init(0.f, WheelDefs.Num());

	ApplyPaint(Body, Paint);

	for (int32 i = 0; i < WheelDefs.Num(); ++i)
	{
		const FGTAIVWheel& W = WheelDefs[i];
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("Wheel%d"), i));
		C->SetMobility(EComponentMobility::Movable);
		C->SetStaticMesh(W.Mesh);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetupAttachment(Body);
		C->SetRelativeLocation(W.Offset);
		C->SetRelativeRotation(FRotator(0.f, W.bRightSide ? 180.f : 0.f, 0.f));
		C->RegisterComponent();
		ApplyPaint(C, Paint);
		WheelComponents.Add(C);
	}
	if (Def.Door)
	{
		UStaticMeshComponent* D = NewObject<UStaticMeshComponent>(this, TEXT("Door"));
		D->SetMobility(EComponentMobility::Movable);
		D->SetStaticMesh(Def.Door);
		D->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		D->SetupAttachment(Body);
		D->SetRelativeLocation(Def.DoorHinge);
		D->RegisterComponent();
		ApplyPaint(D, Paint);
	}
	SetupDriver();
}

void AGTAIVTrafficCar::SetupDriver()
{
	const UGTAIVVehicleAnims* Anims = UGTAIVVehicleAnims::Get();
	const FGTAIVVehicleAnimSet* Set = Anims ? Anims->Find(Def.AnimGroup) : nullptr;
	USkeletalMesh* Ped = Set && !Def.Seat.IsNearlyZero() ? UGTAIVVehicleAnims::RandomPedMesh() : nullptr;
	if (!Ped)
	{
		return;
	}
	DriverMesh = NewObject<UPoseableMeshComponent>(this, TEXT("Driver"));
	DriverMesh->SetSkinnedAssetAndUpdate(Ped);
	DriverMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DriverMesh->SetCastShadow(false);
	DriverMesh->SetupAttachment(Root);
	// GTA peds face the mesh's +Y; the car faces the actor's +X
	DriverMesh->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	DriverMesh->RegisterComponent();
	DriverTime = FMath::FRandRange(0.f, 8.f);
	UGTAIVVehicleAnims::PoseDriver(DriverMesh, *Set, DriverTime, 0.f, 0.f);
	DriverMesh->RefreshBoneTransforms();
	// the pelvis on the seat
	const FVector Seat = Body->GetRelativeTransform().TransformPosition(Def.Seat) + FVector(0.f, 0.f, UGTAIVVehicleAnims::SeatToPelvis(Def.AnimGroup));
	DriverMesh->SetRelativeLocation(Seat - FRotator(0.f, -90.f, 0.f).RotateVector(UGTAIVVehicleAnims::PelvisRefLocation(DriverMesh)));
}

void AGTAIVTrafficCar::UpdateWheels(float DistanceCm, float SteerDegrees)
{
	// the driver turns the wheel with the car (only when someone can see it)
	if (DriverMesh && DriverMesh->WasRecentlyRendered(0.3f))
	{
		const float Dt = GetWorld()->GetDeltaSeconds();
		DriverTime += Dt;
		DriverSteer = FMath::FInterpTo(DriverSteer, FMath::Clamp(-SteerDegrees / 30.f, -1.f, 1.f), Dt, 6.f);
		const UGTAIVVehicleAnims* Anims = UGTAIVVehicleAnims::Get();
		if (const FGTAIVVehicleAnimSet* Set = Anims ? Anims->Find(Def.AnimGroup) : nullptr)
		{
			UGTAIVVehicleAnims::PoseDriver(DriverMesh, *Set, DriverTime, DriverSteer, 0.f);
		}
	}
	for (int32 i = 0; i < WheelComponents.Num(); ++i)
	{
		const FGTAIVWheel& W = WheelDefs[i];
		const float Radius = FMath::Max(W.Radius, 5.f);
		// rolling: the axle is the body mesh's X axis
		WheelRoll[i] = FMath::Fmod(WheelRoll[i] + FMath::RadiansToDegrees(DistanceCm / Radius), 360.f);
		const FQuat Side(FVector::UpVector, FMath::DegreesToRadians(W.bRightSide ? 180.f : 0.f));
		const FQuat Steer(FVector::UpVector, FMath::DegreesToRadians(W.bSteers ? -SteerDegrees : 0.f));
		const FQuat Roll(FVector::ForwardVector, FMath::DegreesToRadians(W.bRightSide ? -WheelRoll[i] : WheelRoll[i]));
		WheelComponents[i]->SetRelativeRotation(Steer * Side * Roll);
	}
}

void AGTAIVTrafficCar::ApplyPaint(UStaticMeshComponent* C, const FGTAIVPaint& Colours)
{
	// paint materials are named MI_VEH_<texture>_P<slot>: give each its colour from the combo
	for (int32 m = 0; m < C->GetNumMaterials(); ++m)
	{
		const UMaterialInterface* Mat = C->GetMaterial(m);
		if (!Mat)
		{
			continue;
		}
		const FString N = Mat->GetName();
		const int32 At = N.Find(TEXT("_P"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (At == INDEX_NONE || At + 2 >= N.Len() || !FChar::IsDigit(N[At + 2]))
		{
			continue;
		}
		const int32 Slot = N[At + 2] - TEXT('0');
		const FLinearColor& Col = Slot == 2 ? Colours.Secondary : Slot == 3 ? Colours.Tertiary : Colours.Primary;
		if (UMaterialInstanceDynamic* MID = C->CreateDynamicMaterialInstance(m))
		{
			MID->SetVectorParameterValue(TEXT("PaintColor"), Col);
		}
	}
}
