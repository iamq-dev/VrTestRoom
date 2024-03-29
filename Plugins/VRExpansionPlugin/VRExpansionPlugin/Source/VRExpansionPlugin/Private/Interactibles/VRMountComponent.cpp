// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "VRMountComponent.h"
#include "Net/UnrealNetwork.h"

//=============================================================================
UVRMountComponent::UVRMountComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	this->bGenerateOverlapEvents = true;
	this->PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = true;

	bRepGameplayTags = false;
	bReplicateMovement = false;

	MovementReplicationSetting = EGripMovementReplicationSettings::ForceClientSideMovement;
	BreakDistance = 100.0f;
	Stiffness = 1500.0f;
	Damping = 200.0f;

	ParentComponent = nullptr;
	MountRotationAxis = EVRInteractibleMountAxis::Axis_XZ;


	InitialRelativeTransform = FTransform::Identity;
	InitialInteractorLocation = FVector::ZeroVector;
	InitialGripRot = 0.0f;
	qRotAtGrab = FQuat::Identity;

	bDenyGripping = false;

	FlipingZone = 0.4;
	FlipReajustYawSpeed = 7.7;

	// Set to only overlap with things so that its not ruined by touching over actors
	this->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
}

//=============================================================================
UVRMountComponent::~UVRMountComponent()
{
}


void UVRMountComponent::GetLifetimeReplicatedProps(TArray< class FLifetimeProperty > & OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UVRMountComponent, bRepGameplayTags);
	DOREPLIFETIME(UVRMountComponent, bReplicateMovement);
	DOREPLIFETIME_CONDITION(UVRMountComponent, GameplayTags, COND_Custom);
}

void UVRMountComponent::PreReplication(IRepChangedPropertyTracker & ChangedPropertyTracker)
{
	Super::PreReplication(ChangedPropertyTracker);

	// Don't replicate if set to not do it
	DOREPLIFETIME_ACTIVE_OVERRIDE(UVRMountComponent, GameplayTags, bRepGameplayTags);

	DOREPLIFETIME_ACTIVE_OVERRIDE(USceneComponent, RelativeLocation, bReplicateMovement);
	DOREPLIFETIME_ACTIVE_OVERRIDE(USceneComponent, RelativeRotation, bReplicateMovement);
	DOREPLIFETIME_ACTIVE_OVERRIDE(USceneComponent, RelativeScale3D, bReplicateMovement);
}

void UVRMountComponent::BeginPlay()
{
	// Call the base class 
	Super::BeginPlay();

	ResetInitialMountLocation();
}

void UVRMountComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	// Call supers tick (though I don't think any of the base classes to this actually implement it)
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

}

void UVRMountComponent::OnUnregister()
{
	Super::OnUnregister();
}

void UVRMountComponent::TickGrip_Implementation(UGripMotionControllerComponent * GrippingController, const FBPActorGripInformation & GripInformation, float DeltaTime)
{
	// Handle manual tracking here

	FTransform CurrentRelativeTransform = InitialRelativeTransform * GetCurrentParentTransform();
	FVector CurInteractorLocation = CurrentRelativeTransform.InverseTransformPosition(GrippingController->GetComponentLocation());

	switch (MountRotationAxis)
	{
	case EVRInteractibleMountAxis::Axis_XZ:
	{
		//The current Mount to target location vector. 
		FVector MountToTarget;

		//In case the Mount is initially gripped on the back (negative of relative x axis)
		if (GrippedOnBack)
		{
			CurInteractorLocation = CurInteractorLocation *-1;
		}

		//Rotate the Initial Grip relative to the Forward Axis so it represents the current correct vector after Mount is rotated. 
		FVector CurToForwardAxisVec = FRotator(RelativeRotation.Pitch, RelativeRotation.Yaw, TwistDiff).RotateVector(InitialGripToForwardVec);

		//The Current InteractorLocation based on current interactor location to intersection point on sphere with forward axis. 
		CurInteractorLocation = (CurInteractorLocation.GetSafeNormal() * InitialInteractorLocation.Size() + CurToForwardAxisVec).GetSafeNormal()*CurInteractorLocation.Size();

		FRotator Rot;

		//If we are inside the fliping zone defined from Cone Area on top or bottom. ToDo: This probably can be combined with later FlipZone.
		if (bIsInsideFrontFlipingZone || bIsInsideBackFlipZone)
		{
			// Entering the flip zone for the first time, a initial forward plane is made to ajust currentinteractorlocation relative x and y. This immitates a mechanical pull/push
			if (!bFirstEntryToHalfFlipZone)
			{
				//Unmodified Right vector can still be used here to create the plane, before it will be changed later.
				ForwardPullPlane = FPlane(FVector::ZeroVector, FVector(EntryRightVec.X, EntryRightVec.Y, 0).GetSafeNormal());

				bFirstEntryToHalfFlipZone = true;
				bLerpingOutOfFlipZone = false;

				CurInterpGripLoc = CurInteractorLocation;

				CurPointOnForwardPlane = FPlane::PointPlaneProject(CurInteractorLocation, ForwardPullPlane);
			}

			LastPointOnForwardPlane = CurPointOnForwardPlane;

			CurPointOnForwardPlane = FPlane::PointPlaneProject(CurInteractorLocation, ForwardPullPlane);

			FVector ForwardPlainDiffVec = CurPointOnForwardPlane - LastPointOnForwardPlane;

			//Add the difference to how much we moved trough the forward plane since the last frame.
			CurInterpGripLoc += ForwardPlainDiffVec;

			// InterpTo the current projected point on forward plane over time when inside the FlipZone. 
			CurInterpGripLoc = FMath::VInterpConstantTo(CurInterpGripLoc, CurPointOnForwardPlane, GetWorld()->GetDeltaSeconds(), 50);

			//The current location of the motion controller projected on to the forward plane. Only x and y is used from interpolation.
			MountToTarget = FVector(CurInterpGripLoc.X, CurInterpGripLoc.Y, CurInteractorLocation.Z).GetSafeNormal();

			//Save the CurInteractorLocation once to modify roll with it later
			CurInteractorLocation = FVector(CurInterpGripLoc.X, CurInterpGripLoc.Y, CurInteractorLocation.Z);
		}
		else
		{
			//When going out of whole FlipZone Lerp yaw back to curinteractorlocation in case relative entry xy deviates to much from when leaving area
			if (bLerpingOutOfFlipZone)
			{
				//If projected point on plane is not close enough to the "real" motion controller location lerp
				if ((CurInterpGripLoc - CurInteractorLocation).Size() >= 1.0f)
				{
					//How fast yaw should rotate back when leaving flipzone. ToDo: For LerpOutSpeed maybe better us distance deviation
					LerpOutAlpha = FMath::Clamp(LerpOutAlpha + FlipReajustYawSpeed / 100.0f, 0.0f, 1.0f);

					//Lerp
					CurInterpGripLoc = CurInterpGripLoc + LerpOutAlpha * (CurInteractorLocation - CurInterpGripLoc);

					//The new vector to make rotation from.
					MountToTarget = CurInterpGripLoc.GetSafeNormal();

					//We left the flipzone completly, set everything back to normal.
					if (LerpOutAlpha >= 0.97f)
					{
						bLerpingOutOfFlipZone = false;
						bIsInsideBackFlipZone = false;
					}
				}
				else
				{
					//If we are already near the real controller location just snap back
					bLerpingOutOfFlipZone = false;
					bIsInsideBackFlipZone = false;
					MountToTarget = CurInteractorLocation.GetSafeNormal();
				}
			}
			else
			{
				//There is still a possibility here maybe to be inside backflipzone, but Mount is actually already outside. So just use unmodified rotation.
				MountToTarget = CurInteractorLocation.GetSafeNormal();
				bIsInsideBackFlipZone = false;
			}

		}

		//Setting the relative rotation once before using "accumulated" relative rotation to calculate roll onto it.
		Rot = MountToTarget.Rotation();
		this->SetRelativeRotation((FTransform(FRotator(Rot.Pitch, Rot.Yaw, 0))*InitialRelativeTransform).Rotator());


		FVector nAxis;
		float FlipAngle;
		FQuat::FindBetweenVectors(FVector(0, 0, 1), CurInteractorLocation.GetSafeNormal()).ToAxisAndAngle(nAxis, FlipAngle);

		// This part takes care of the roll rotation ff Mount is inside flipping zone on top or on bottom.
		if (FlipAngle < FlipingZone || FlipAngle > PI - FlipingZone)
		{
			//When entering FlipZone for the first time setup initial properties
			if (!bIsInsideFrontFlipingZone && !bIsInsideBackFlipZone)
			{
				//We entered the FrontFlipzone
				bIsInsideFrontFlipingZone = true;

				//Up and Right Vector when entering the FlipZone. Parent Rotation is accounted for.
				EntryUpVec = ParentComponent->GetComponentRotation().UnrotateVector(GetUpVector());
				EntryRightVec = ParentComponent->GetComponentRotation().UnrotateVector(GetRightVector());

				//Only relative x and y is important for a Mount which has XY rotation limitation for the flip plane.
				EntryUpXYNeg = FVector(EntryUpVec.X, EntryUpVec.Y, 0).GetSafeNormal()*-1;
				//If flipping over the bottom
				if (FlipAngle > PI - FlipingZone)
				{
					EntryUpXYNeg *= -1;
				}

				//A Plane perpendicular to the FlipZone relative EntryPoint XY UpVector. This plane determines when the roll has to be turned by 180 degree
				FlipPlane = FPlane(FVector::ZeroVector, EntryUpXYNeg);
			}

			//CurInteractor vector to its projected point on flipplane
			FVector CurInteractorToFlipPlaneVec = CurInteractorLocation - FPlane::PointPlaneProject(CurInteractorLocation, FlipPlane);

			if (bIsInsideFrontFlipingZone)
			{
				//If Mount rotation is  on or over flipplane but still inside frontflipzone flip the roll.
				if (FVector::DotProduct(CurInteractorToFlipPlaneVec, EntryUpXYNeg) <= 0)
				{
					bIsInsideFrontFlipingZone = false;
					bIsInsideBackFlipZone = true;

					bIsFlipped = !bIsFlipped;

					if (bIsFlipped)
					{
						TwistDiff = 180;
					}
					else
					{
						TwistDiff = 0;
					}
				}
				else
				{
					//If Mount Rotation is still inside FrontFlipZone ajust the roll so it looks naturally when moving the Mount against the FlipPlane

					FVector RelativeUpVec = ParentComponent->GetComponentRotation().UnrotateVector(GetUpVector());
					FVector CurrentUpVec = FVector(RelativeUpVec.X, RelativeUpVec.Y, 0).GetSafeNormal();

					//If rotating over the top ajust relative UpVector
					if (FlipAngle < FlipingZone)
					{
						CurrentUpVec *= -1;
					}

					float EntryTwist = FMath::Atan2(EntryUpXYNeg.Y, EntryUpXYNeg.X);
					float CurTwist = FMath::Atan2(CurrentUpVec.Y, CurrentUpVec.X);

					//Rotate the roll so relative up vector x y looks at the flip plane
					if (bIsFlipped)
					{
						TwistDiff = FMath::RadiansToDegrees(EntryTwist - CurTwist - PI);
					}
					else
					{
						TwistDiff = FMath::RadiansToDegrees(EntryTwist - CurTwist);
					}
				}
			}
			else
			{
				//If Inside Back Flip Zone just flip the roll. ToDo: Dont just ajust roll to 0 or 180. Calculate Twist diff according to up vector to FlipPlane. 
				if (bIsInsideBackFlipZone)
				{
					if (FVector::DotProduct(CurInteractorToFlipPlaneVec, EntryUpXYNeg) >= 0)
					{
						bIsInsideFrontFlipingZone = true;
						bIsInsideBackFlipZone = false;

						bIsFlipped = !bIsFlipped;

						if (bIsFlipped)
						{
							TwistDiff = 180;
						}
						else
						{
							TwistDiff = 0;
						}
					}
				}
			}

		}
		else
		{
			//If the Mount went into the flipping zone and back out without going over flip plane reset roll
			bIsInsideFrontFlipingZone = false;
			bIsInsideBackFlipZone = false;

			if (bIsFlipped)
			{
				TwistDiff = 180;
			}
			else
			{
				TwistDiff = 0;
			}

			if (bFirstEntryToHalfFlipZone)
			{
				//If never left FlipZone before but doing now it now, reset first time entry bool. ToDo: Maybe better to do this elsewhere.
				bFirstEntryToHalfFlipZone = false;
				LerpOutAlpha = 0;

				//We left the flipzone so rotate yaw back from interpolated controller position on forward pull plane back to "real" one.
				bLerpingOutOfFlipZone = true;
			}

		}

		//Add Roll modifications to accumulated LocalRotation.
		this->AddLocalRotation(FRotator(0, 0, -TwistDiff));

	}break;
	default:break;
	}

	// #TODO: This drop code is incorrect, it is based off of the initial point and not the location at grip - revise it at some point
	// Also set it to after rotation
	if (GrippingController->HasGripAuthority(GripInformation) && FVector::DistSquared(InitialInteractorDropLocation, this->GetComponentTransform().InverseTransformPosition(GrippingController->GetComponentLocation())) >= FMath::Square(BreakDistance))
	{
		GrippingController->DropObjectByInterface(this);
		return;
	}
}

void UVRMountComponent::OnGrip_Implementation(UGripMotionControllerComponent * GrippingController, const FBPActorGripInformation & GripInformation)
{
	ParentComponent = this->GetAttachParent();

	
	
	FTransform CurrentRelativeTransform = InitialRelativeTransform * GetCurrentParentTransform();

	// This lets me use the correct original location over the network without changes
	FTransform RelativeToGripTransform = (GripInformation.RelativeTransform.Inverse() * this->GetComponentTransform());

	//continue here CurToForwardAxis is based on last gripped location ---> change this
	InitialInteractorLocation = CurrentRelativeTransform.InverseTransformPosition(RelativeToGripTransform.GetTranslation());
	InitialInteractorDropLocation = this->GetComponentTransform().InverseTransformPosition(RelativeToGripTransform.GetTranslation());

	switch (MountRotationAxis)
	{
	case EVRInteractibleMountAxis::Axis_XZ:
	{
		//spaceharry

		qRotAtGrab = this->GetComponentTransform().GetRelativeTransform(CurrentRelativeTransform).GetRotation();

		InitialForwardVector = InitialInteractorLocation.Size() * ParentComponent->GetComponentRotation().UnrotateVector(GetForwardVector());

		if (FVector::DotProduct(InitialInteractorLocation, ParentComponent->GetComponentRotation().UnrotateVector(GetForwardVector())) <= 0)
		{
			GrippedOnBack = true;
			InitialGripToForwardVec = (InitialForwardVector + InitialInteractorLocation);
		}
		else
		{
			InitialGripToForwardVec = InitialForwardVector - InitialInteractorLocation;
			GrippedOnBack = false;
		}


		InitialGripToForwardVec = FRotator(RelativeRotation.Pitch, RelativeRotation.Yaw, TwistDiff).UnrotateVector(InitialGripToForwardVec);

	}break;
	default:break;
	}

		


	this->SetComponentTickEnabled(true);
}

void UVRMountComponent::OnGripRelease_Implementation(UGripMotionControllerComponent * ReleasingController, const FBPActorGripInformation & GripInformation)
{
		this->SetComponentTickEnabled(false);
}

void UVRMountComponent::OnChildGrip_Implementation(UGripMotionControllerComponent * GrippingController, const FBPActorGripInformation & GripInformation) {}
void UVRMountComponent::OnChildGripRelease_Implementation(UGripMotionControllerComponent * ReleasingController, const FBPActorGripInformation & GripInformation) {}
void UVRMountComponent::OnSecondaryGrip_Implementation(USceneComponent * SecondaryGripComponent, const FBPActorGripInformation & GripInformation) {}
void UVRMountComponent::OnSecondaryGripRelease_Implementation(USceneComponent * ReleasingSecondaryGripComponent, const FBPActorGripInformation & GripInformation) {}
void UVRMountComponent::OnUsed_Implementation() {}
void UVRMountComponent::OnEndUsed_Implementation() {}
void UVRMountComponent::OnSecondaryUsed_Implementation() {}
void UVRMountComponent::OnEndSecondaryUsed_Implementation() {}
void UVRMountComponent::OnInput_Implementation(FKey Key, EInputEvent KeyEvent) {}

bool UVRMountComponent::DenyGripping_Implementation()
{
	return bDenyGripping;
}

EGripInterfaceTeleportBehavior UVRMountComponent::TeleportBehavior_Implementation()
{
	return EGripInterfaceTeleportBehavior::DropOnTeleport;
}

bool UVRMountComponent::SimulateOnDrop_Implementation()
{
	return false;
}


EGripCollisionType UVRMountComponent::GetPrimaryGripType_Implementation(bool bIsSlot)
{
		return EGripCollisionType::CustomGrip;
}

ESecondaryGripType UVRMountComponent::SecondaryGripType_Implementation()
{
	return ESecondaryGripType::SG_None;
}


EGripMovementReplicationSettings UVRMountComponent::GripMovementReplicationType_Implementation()
{
	return MovementReplicationSetting;
}

EGripLateUpdateSettings UVRMountComponent::GripLateUpdateSetting_Implementation()
{
	return EGripLateUpdateSettings::LateUpdatesAlwaysOff;
}

/*float UVRMountComponent::GripStiffness_Implementation()
{
return Stiffness;
}

float UVRMountComponent::GripDamping_Implementation()
{
return Damping;
}*/
void UVRMountComponent::GetGripStiffnessAndDamping_Implementation(float &GripStiffnessOut, float &GripDampingOut)
{
	GripStiffnessOut = Stiffness;
	GripDampingOut = Damping;
}

FBPAdvGripSettings UVRMountComponent::AdvancedGripSettings_Implementation()
{
	return FBPAdvGripSettings();
}

float UVRMountComponent::GripBreakDistance_Implementation()
{
	return BreakDistance;
}

/*void UVRMountComponent::ClosestSecondarySlotInRange_Implementation(FVector WorldLocation, bool & bHadSlotInRange, FTransform & SlotWorldTransform, UGripMotionControllerComponent * CallingController, FName OverridePrefix)
{
bHadSlotInRange = false;
}

void UVRMountComponent::ClosestPrimarySlotInRange_Implementation(FVector WorldLocation, bool & bHadSlotInRange, FTransform & SlotWorldTransform, UGripMotionControllerComponent * CallingController, FName OverridePrefix)
{
bHadSlotInRange = false;
}*/

void UVRMountComponent::ClosestGripSlotInRange_Implementation(FVector WorldLocation, bool bSecondarySlot, bool & bHadSlotInRange, FTransform & SlotWorldTransform, UGripMotionControllerComponent * CallingController, FName OverridePrefix)
{
	bHadSlotInRange = false;
}

bool UVRMountComponent::IsInteractible_Implementation()
{
	return false;
}

void UVRMountComponent::IsHeld_Implementation(UGripMotionControllerComponent *& CurHoldingController, bool & bCurIsHeld)
{
	CurHoldingController = HoldingController;
	bCurIsHeld = bIsHeld;
}

void UVRMountComponent::SetHeld_Implementation(UGripMotionControllerComponent * NewHoldingController, bool bNewIsHeld)
{
	bIsHeld = bNewIsHeld;

	if (bIsHeld)
		HoldingController = NewHoldingController;
	else
		HoldingController = nullptr;
}

FBPInteractionSettings UVRMountComponent::GetInteractionSettings_Implementation()
{
	return FBPInteractionSettings();
}


