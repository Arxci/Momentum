#pragma once

#include "CoreMinimal.h"
#include "Engine/HitResult.h"

#include "MomentumMovementTypes.generated.h"

/**
 * High level movement states driven by UMomentumMovementComponent.
 *
 * Grounded and Airborne cover the whole base game. Traversal abilities added later
 * (wall running, sliding, mantling, zip lines) should use Custom together with a
 * project defined uint8 mode id, so this enum never has to change.
 */
UENUM(BlueprintType)
enum class EMomentumMovementMode : uint8
{
	/** No simulation. The component is inactive or has no updated component. */
	None		UMETA(DisplayName = "None"),

	/** Standing on a walkable surface. Friction and ground acceleration apply. */
	Grounded	UMETA(DisplayName = "Grounded"),

	/** In the air. Gravity and air acceleration apply. */
	Airborne	UMETA(DisplayName = "Airborne"),

	/** Driven by PhysCustom, selected by UMomentumMovementComponent::GetCustomMovementMode(). */
	Custom		UMETA(DisplayName = "Custom")
};

/**
 * Result of a downward capsule sweep used to decide whether the pawn is standing on something.
 */
USTRUCT(BlueprintType)
struct FMomentumFloorResult
{
	GENERATED_BODY()

	/** True if the sweep hit any blocking geometry, walkable or not. */
	UPROPERTY(BlueprintReadOnly, Category = "Momentum|Floor")
	bool bBlockingHit = false;

	/** True if the hit surface is flat enough to stand on. Only then is the pawn considered grounded. */
	UPROPERTY(BlueprintReadOnly, Category = "Momentum|Floor")
	bool bWalkableFloor = false;

	/** Gap between the bottom of the capsule and the floor, in centimetres. Zero when penetrating. */
	UPROPERTY(BlueprintReadOnly, Category = "Momentum|Floor")
	float FloorDistance = 0.0f;

	/** The sweep hit itself. Valid only when bBlockingHit is true. */
	UPROPERTY(BlueprintReadOnly, Category = "Momentum|Floor")
	FHitResult HitResult;

	void Clear()
	{
		bBlockingHit = false;
		bWalkableFloor = false;
		FloorDistance = 0.0f;
		HitResult.Reset(1.0f, false);
	}

	/** Surface normal of the floor, or straight up when there is no floor. */
	FVector GetSurfaceNormal() const
	{
		return bBlockingHit ? HitResult.ImpactNormal : FVector::UpVector;
	}
};

/** Fired after the movement mode changes. Both modes are supplied so listeners can react to specific transitions. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FMomentumMovementModeChangedSignature, EMomentumMovementMode, PreviousMode, EMomentumMovementMode, NewMode);

/** Fired on the transition from Airborne to Grounded. Use GetLastLandingVelocity() for impact strength. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMomentumLandedSignature, const FHitResult&, FloorHit);

/** Fired whenever a jump is performed. JumpIndex is 0 for the ground jump and counts up for each air jump. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMomentumJumpedSignature, int32, JumpIndex);

/** Fired when the capsule finishes resizing into or out of the crouched state. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMomentumCrouchStateChangedSignature, bool, bIsCrouching);