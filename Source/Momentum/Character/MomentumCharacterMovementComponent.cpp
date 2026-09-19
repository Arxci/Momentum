#include "Momentum/Character/MomentumCharacterMovementComponent.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"

#if !UE_BUILD_SHIPPING
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#endif

namespace MomentumMovement
{
	/** Gap kept between the capsule and the floor so swept moves never start in penetration. */
	constexpr float GroundContactOffset = 1.5f;

	/** How far below an airborne pawn to look for a floor. Landing is resolved by the move sweep itself. */
	constexpr float AirborneFloorProbeDistance = 2.0f;

	/** Floor sweeps use a slightly narrower capsule so they do not catch on walls being brushed. */
	constexpr float FloorSweepRadiusShrink = 1.0f;

	/** Stand-up clearance tests shrink by this much so the current floor does not read as an obstruction. */
	constexpr float StandUpClearanceTolerance = 1.0f;

	static const FName FindFloorTag(TEXT("MomentumFindFloor"));
	static const FName StandUpClearanceTag(TEXT("MomentumStandUpClearance"));

#if !UE_BUILD_SHIPPING
	static int32 ShowDebug = 0;
	static FAutoConsoleVariableRef CVarShowDebug(
		TEXT("momentum.Movement.ShowDebug"),
		ShowDebug,
		TEXT("Draw diagnostics for UMomentumMovementComponent: state, speed, wish direction and floor. 0 = off, 1 = on."),
		ECVF_Cheat);
#endif
}

UMomentumMovementComponent::UMomentumMovementComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	// Nothing here is replicated: this component is built for a single-player movement sandbox.
	SetIsReplicatedByDefault(false);
}

void UMomentumMovementComponent::BeginPlay()
{
	Super::BeginPlay();

	WalkableFloorZ = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(WalkableFloorAngle, 0.0f, 89.0f)));

	// Start airborne and let the first floor probe settle the pawn onto the ground.
	if (MovementMode == EMomentumMovementMode::None)
	{
		SetMovementMode(EMomentumMovementMode::Airborne);
	}
}

void UMomentumMovementComponent::SetUpdatedComponent(USceneComponent* NewUpdatedComponent)
{
	Super::SetUpdatedComponent(NewUpdatedComponent);

	CurrentFloor.Clear();

	if (const UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		StandingHalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
	}
}

UCapsuleComponent* UMomentumMovementComponent::GetCapsuleComponent() const
{
	return Cast<UCapsuleComponent>(UpdatedComponent);
}

// -------------------------------------------------------------------------------------------------
// Tick and simulation loop
// -------------------------------------------------------------------------------------------------

void UMomentumMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (ShouldSkipUpdate(DeltaTime))
	{
		return;
	}

	if (!PawnOwner || !UpdatedComponent || UpdatedComponent->Mobility != EComponentMobility::Movable || UpdatedComponent->IsSimulatingPhysics())
	{
		return;
	}

	WalkableFloorZ = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(WalkableFloorAngle, 0.0f, 89.0f)));

	// Input accumulated by APawn::AddMovementInput must be consumed exactly once per frame, not per
	// substep, so the wish direction is resolved here and reused by every step below.
	const FVector InputVector = ConsumeInputVector().GetClampedToMaxSize(1.0f);
	WishDirection = FVector(InputVector.X, InputVector.Y, 0.0f).GetSafeNormal();
	WishInputScale = FMath::Min(InputVector.Size2D(), 1.0f);

	// Long frames are split into fixed steps. Time beyond the iteration budget is dropped, which
	// makes a stalling game run in slow motion rather than tunnel through geometry.
	const int32 IterationLimit = bEnableSubstepping ? FMath::Max(MaxSimulationIterations, 1) : 1;
	const float StepLimit = bEnableSubstepping ? FMath::Max(MaxSimulationTimeStep, UE_KINDA_SMALL_NUMBER) : DeltaTime;

	float RemainingTime = DeltaTime;
	int32 Iterations = 0;
	while (RemainingTime > UE_KINDA_SMALL_NUMBER && Iterations < IterationLimit)
	{
		++Iterations;
		const float StepTime = FMath::Min(StepLimit, RemainingTime);
		RemainingTime -= StepTime;

		PerformMovement(StepTime);
	}

	// Push the simulated velocity onto the component so GetVelocity(), anim and audio all agree.
	UpdateComponentVelocity();

	DrawDebugInfo();
}

void UMomentumMovementComponent::PerformMovement(const float DeltaTime)
{
	UpdateTimers(DeltaTime);
	UpdateCrouchState();
	UpdateFloorAndMode();

	// Resolved after the mode update so a jump buffered mid-air fires on the same step as the landing.
	TryJumpFromInput();

	switch (MovementMode)
	{
	case EMomentumMovementMode::Grounded:
		PhysGrounded(DeltaTime);
		break;

	case EMomentumMovementMode::Airborne:
		PhysAirborne(DeltaTime);
		break;

	case EMomentumMovementMode::Custom:
		PhysCustom(DeltaTime, CustomMovementMode);
		break;

	default:
		break;
	}
}

void UMomentumMovementComponent::UpdateTimers(const float DeltaTime)
{
	TimeInCurrentMode += DeltaTime;
	JumpBufferTimeRemaining = FMath::Max(JumpBufferTimeRemaining - DeltaTime, 0.0f);
	CoyoteTimeRemaining = FMath::Max(CoyoteTimeRemaining - DeltaTime, 0.0f);
}

void UMomentumMovementComponent::UpdateFloorAndMode()
{
	// Custom modes own their own transitions; the base loop must not pull them back to the ground.
	if (MovementMode == EMomentumMovementMode::Custom)
	{
		return;
	}

	// While grounded the probe reaches a full step height so stairs and ramps keep contact. While
	// airborne it only looks far enough to confirm a touchdown the move sweep already produced.
	const float ProbeDistance = IsGrounded()
		? GroundSnapDistance + MomentumMovement::GroundContactOffset
		: MomentumMovement::AirborneFloorProbeDistance;

	FindFloor(UpdatedComponent->GetComponentLocation(), ProbeDistance, CurrentFloor);

	// Rising means the pawn has just jumped or been launched, so the floor it is leaving is ignored.
	const bool bGroundedNow = CurrentFloor.bWalkableFloor && Velocity.Z <= UE_KINDA_SMALL_NUMBER;

	if (bGroundedNow && !IsGrounded())
	{
		Land(CurrentFloor.HitResult);
	}
	else if (!bGroundedNow && IsGrounded())
	{
		SetMovementMode(EMomentumMovementMode::Airborne);
	}
}

void UMomentumMovementComponent::Land(const FHitResult& FloorHit)
{
	// Captured before PhysGrounded flattens the velocity, so listeners can scale landing effects
	// and fall damage by the real impact speed.
	LastLandingVelocity = Velocity;

	SetMovementMode(EMomentumMovementMode::Grounded);
	OnLandedDelegate.Broadcast(FloorHit);
}

// -------------------------------------------------------------------------------------------------
// Movement modes
// -------------------------------------------------------------------------------------------------

void UMomentumMovementComponent::PhysGrounded(const float DeltaTime)
{
	// Grounded movement is solved horizontally; MoveAlongFloor reintroduces Z to follow the slope.
	Velocity.Z = 0.0f;

	ApplyFriction(DeltaTime);
	ApplyAcceleration(WishDirection, GetMaxSpeed() * WishInputScale, GroundAcceleration, DeltaTime);

	if (Velocity.IsNearlyZero())
	{
		Velocity = FVector::ZeroVector;
	}
	else
	{
		MoveAlongFloor(Velocity * DeltaTime);
	}

	MaintainGroundContact();
}

void UMomentumMovementComponent::PhysAirborne(const float DeltaTime)
{
	Velocity.Z = FMath::Max(Velocity.Z + GetGravityZ() * GravityScale * DeltaTime, -TerminalVelocity);

	// Capping the wish speed instead of the velocity is what produces air strafing: turning the wish
	// direction away from the current velocity keeps the projected speed under the cap, so thrust
	// keeps being added and total speed grows.
	const float AirWishSpeed = FMath::Min(GetMaxSpeed(), MaxAirWishSpeed) * WishInputScale;
	ApplyAcceleration(WishDirection, AirWishSpeed, AirAcceleration, DeltaTime);

	const FVector Delta = Velocity * DeltaTime;
	if (Delta.IsNearlyZero())
	{
		return;
	}

	FHitResult Hit(1.0f);
	SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);

	if (!Hit.IsValidBlockingHit())
	{
		return;
	}

	if (IsWalkable(Hit))
	{
		// Touchdown. Record the floor here rather than waiting for the next step's probe, so the
		// landing event fires on the same frame the pawn actually hits the ground.
		CurrentFloor.bBlockingHit = true;
		CurrentFloor.bWalkableFloor = true;
		CurrentFloor.FloorDistance = 0.0f;
		CurrentFloor.HitResult = Hit;

		Land(Hit);
		return;
	}

	HandleBlockingImpact(Hit);
	SlideAlongSurface(Delta, 1.0f - Hit.Time, Hit.Normal, Hit, true);
}

void UMomentumMovementComponent::PhysCustom(const float DeltaTime, const uint8 CustomMode)
{
	// Extension point for traversal states such as wall running, sliding and mantling.
	// Subclasses handle their own mode ids here and are responsible for leaving Custom again.
}

// -------------------------------------------------------------------------------------------------
// Acceleration model
// -------------------------------------------------------------------------------------------------

void UMomentumMovementComponent::ApplyAcceleration(const FVector& WishDir, const float WishSpeed, const float Acceleration, const float DeltaTime)
{
	if (WishDir.IsNearlyZero() || WishSpeed <= 0.0f || Acceleration <= 0.0f || DeltaTime <= 0.0f)
	{
		return;
	}

	// Only the velocity already pointing along the wish direction counts against the cap. Speed
	// carried in any other direction is untouched, which is what preserves momentum across jumps.
	const float ProjectedSpeed = FVector::DotProduct(Velocity, WishDir);
	const float SpeedToAdd = WishSpeed - ProjectedSpeed;
	if (SpeedToAdd <= 0.0f)
	{
		return;
	}

	Velocity += WishDir * FMath::Min(Acceleration * WishSpeed * DeltaTime, SpeedToAdd);
}

void UMomentumMovementComponent::ApplyFriction(const float DeltaTime)
{
	const float Speed = Velocity.Size2D();
	if (Speed < UE_KINDA_SMALL_NUMBER)
	{
		Velocity.X = 0.0f;
		Velocity.Y = 0.0f;
		return;
	}

	// Treating anything below StopSpeed as if it were moving at StopSpeed gives a crisp stop
	// instead of an exponential tail that never quite reaches zero.
	const float Control = FMath::Max(Speed, StopSpeed);
	const float NewSpeed = FMath::Max(Speed - Control * GroundFriction * DeltaTime, 0.0f);

	const float Scale = NewSpeed / Speed;
	Velocity.X *= Scale;
	Velocity.Y *= Scale;
}

// -------------------------------------------------------------------------------------------------
// Collision
// -------------------------------------------------------------------------------------------------

bool UMomentumMovementComponent::FindFloor(const FVector& CapsuleLocation, const float SweepDistance, FMomentumFloorResult& OutFloor) const
{
	OutFloor.Clear();

	const UWorld* World = GetWorld();
	if (!World || !UpdatedPrimitive || SweepDistance <= 0.0f)
	{
		return false;
	}

	FCollisionShape SweepShape = UpdatedPrimitive->GetCollisionShape();
	if (SweepShape.IsCapsule())
	{
		SweepShape = FCollisionShape::MakeCapsule(
			FMath::Max(SweepShape.GetCapsuleRadius() - MomentumMovement::FloorSweepRadiusShrink, 1.0f),
			SweepShape.GetCapsuleHalfHeight());
	}

	FCollisionQueryParams QueryParams(MomentumMovement::FindFloorTag, false, GetOwner());
	FCollisionResponseParams ResponseParams;
	UpdatedPrimitive->InitSweepCollisionParams(QueryParams, ResponseParams);

	FHitResult Hit(1.0f);
	const bool bHit = World->SweepSingleByChannel(
		Hit,
		CapsuleLocation,
		CapsuleLocation - FVector(0.0f, 0.0f, SweepDistance),
		FQuat::Identity,
		UpdatedPrimitive->GetCollisionObjectType(),
		SweepShape,
		QueryParams,
		ResponseParams);

	if (!bHit)
	{
		return false;
	}

	OutFloor.bBlockingHit = true;
	OutFloor.HitResult = Hit;
	OutFloor.FloorDistance = Hit.bStartPenetrating ? 0.0f : Hit.Distance;
	OutFloor.bWalkableFloor = IsWalkable(Hit);

	return true;
}

bool UMomentumMovementComponent::IsWalkable(const FHitResult& Hit) const
{
	if (!Hit.bBlockingHit)
	{
		return false;
	}

	// A hit that starts in penetration has an unreliable impact normal, but its depenetration
	// normal still describes the surface.
	const FVector SurfaceNormal = Hit.bStartPenetrating ? Hit.Normal : Hit.ImpactNormal;
	if (SurfaceNormal.Z < WalkableFloorZ)
	{
		return false;
	}

	// Honour geometry that opts out of being stood on, such as other pawns.
	return !Hit.Component.IsValid() || Hit.Component->CanCharacterStepUp(PawnOwner);
}

void UMomentumMovementComponent::MoveAlongFloor(const FVector& Delta)
{
	if (Delta.IsNearlyZero() || !UpdatedComponent)
	{
		return;
	}

	FVector RampDelta = Delta;
	if (CurrentFloor.bWalkableFloor)
	{
		const FVector FloorNormal = CurrentFloor.GetSurfaceNormal();
		if (FloorNormal.Z > UE_KINDA_SMALL_NUMBER && !FMath::IsNearlyEqual(FloorNormal.Z, 1.0f))
		{
			// Keep the horizontal distance and solve for the Z that lies in the floor plane, so
			// ramps are climbed and descended at a constant ground speed instead of launching.
			RampDelta.Z = -(FloorNormal.X * Delta.X + FloorNormal.Y * Delta.Y) / FloorNormal.Z;
		}
	}

	const FQuat Rotation = UpdatedComponent->GetComponentQuat();

	FHitResult Hit(1.0f);
	SafeMoveUpdatedComponent(RampDelta, Rotation, true, Hit);

	if (!Hit.IsValidBlockingHit())
	{
		return;
	}

	// Obstructions too steep to walk onto may still be short enough to step over.
	if (!IsWalkable(Hit) && TryStepUp(Hit, RampDelta * (1.0f - Hit.Time)))
	{
		return;
	}

	HandleBlockingImpact(Hit);
	SlideAlongSurface(RampDelta, 1.0f - Hit.Time, Hit.Normal, Hit, true);
}

bool UMomentumMovementComponent::TryStepUp(const FHitResult& Hit, const FVector& RemainingDelta)
{
	if (MaxStepHeight <= 0.0f || !IsGrounded() || !UpdatedComponent || !Hit.IsValidBlockingHit())
	{
		return false;
	}

	// Walkable surfaces need no step, and overhangs cannot be stepped onto at all.
	if (Hit.ImpactNormal.Z >= WalkableFloorZ || Hit.ImpactNormal.Z < -UE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	if (Hit.Component.IsValid() && !Hit.Component->CanCharacterStepUp(PawnOwner))
	{
		return false;
	}

	const FVector HorizontalDelta(RemainingDelta.X, RemainingDelta.Y, 0.0f);
	if (HorizontalDelta.IsNearlyZero())
	{
		return false;
	}

	const FQuat Rotation = UpdatedComponent->GetComponentQuat();
	const float StartZ = UpdatedComponent->GetComponentLocation().Z;

	// Everything below is provisional: reverting restores the pre-step transform in one go.
	FScopedMovementUpdate ScopedStep(UpdatedComponent, EScopedUpdate::DeferredUpdates);

	// 1. Rise by the step height.
	FHitResult UpHit(1.0f);
	MoveUpdatedComponent(FVector(0.0f, 0.0f, MaxStepHeight), Rotation, true, &UpHit);
	if (UpHit.bStartPenetrating)
	{
		ScopedStep.RevertMove();
		return false;
	}
	const float RisenDistance = MaxStepHeight * (UpHit.bBlockingHit ? UpHit.Time : 1.0f);

	// 2. Travel the rest of the move at the raised height.
	FHitResult ForwardHit(1.0f);
	MoveUpdatedComponent(HorizontalDelta, Rotation, true, &ForwardHit);
	if (ForwardHit.bBlockingHit && ForwardHit.Time <= 0.0f)
	{
		// Still blocked up there: this is a wall, not a step.
		ScopedStep.RevertMove();
		return false;
	}

	// 3. Settle back down onto whatever was stepped onto.
	FHitResult DownHit(1.0f);
	MoveUpdatedComponent(FVector(0.0f, 0.0f, -RisenDistance), Rotation, true, &DownHit);
	if (!DownHit.IsValidBlockingHit() || !IsWalkable(DownHit))
	{
		// Stepped into thin air or onto a slope too steep to stand on.
		ScopedStep.RevertMove();
		return false;
	}

	if (UpdatedComponent->GetComponentLocation().Z - StartZ > MaxStepHeight + UE_KINDA_SMALL_NUMBER)
	{
		ScopedStep.RevertMove();
		return false;
	}

	return true;
}

void UMomentumMovementComponent::MaintainGroundContact()
{
	if (!UpdatedComponent)
	{
		return;
	}

	const float ProbeDistance = GroundSnapDistance + MomentumMovement::GroundContactOffset;

	FMomentumFloorResult Floor;
	FindFloor(UpdatedComponent->GetComponentLocation(), ProbeDistance, Floor);

	// Pull the pawn back onto its floor after a move, so descending stairs and ramps does not throw
	// it into the air every step.
	if (Floor.bWalkableFloor && Floor.FloorDistance > MomentumMovement::GroundContactOffset)
	{
		const float SnapDistance = Floor.FloorDistance - MomentumMovement::GroundContactOffset;

		FHitResult SnapHit(1.0f);
		SafeMoveUpdatedComponent(FVector(0.0f, 0.0f, -SnapDistance), UpdatedComponent->GetComponentQuat(), true, SnapHit);

		FindFloor(UpdatedComponent->GetComponentLocation(), ProbeDistance, Floor);
	}

	CurrentFloor = Floor;
}

void UMomentumMovementComponent::HandleBlockingImpact(const FHitResult& Hit)
{
	// Drop the component of velocity heading into the surface. Without this, speed is banked against
	// a wall and released the moment the pawn turns away from it.
	if (Hit.bBlockingHit && FVector::DotProduct(Velocity, Hit.Normal) < 0.0f)
	{
		Velocity = FVector::VectorPlaneProject(Velocity, Hit.Normal);
	}
}

// -------------------------------------------------------------------------------------------------
// Crouch
// -------------------------------------------------------------------------------------------------

void UMomentumMovementComponent::UpdateCrouchState()
{
	const bool bShouldCrouch = bWantsToCrouch && bCanCrouch;
	if (bShouldCrouch == bIsCrouching)
	{
		return;
	}

	const float TargetHalfHeight = bShouldCrouch ? FMath::Min(CrouchedHalfHeight, StandingHalfHeight) : StandingHalfHeight;

	// Standing up can fail when there is no headroom, in which case the pawn simply stays crouched.
	if (ResizeCapsule(TargetHalfHeight, true))
	{
		bIsCrouching = bShouldCrouch;
		OnCrouchStateChangedDelegate.Broadcast(bIsCrouching);
	}
}

bool UMomentumMovementComponent::ResizeCapsule(const float NewUnscaledHalfHeight, const bool bMaintainBaseLocation)
{
	UCapsuleComponent* Capsule = GetCapsuleComponent();
	if (!Capsule)
	{
		return false;
	}

	const float OldUnscaledHalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
	if (FMath::IsNearlyEqual(OldUnscaledHalfHeight, NewUnscaledHalfHeight))
	{
		return false;
	}

	const float ShapeScale = Capsule->GetShapeScale();
	const float ScaledAdjust = (NewUnscaledHalfHeight - OldUnscaledHalfHeight) * ShapeScale;

	// On the ground the feet stay planted, so the capsule centre moves by the half height change.
	// In the air the capsule shrinks around its centre instead, so the pawn tucks up rather than drops.
	const FVector Translation = (bMaintainBaseLocation && IsGrounded())
		? FVector(0.0f, 0.0f, ScaledAdjust)
		: FVector::ZeroVector;

	// Growing requires clearance at the destination; nothing is committed until that is confirmed.
	if (NewUnscaledHalfHeight > OldUnscaledHalfHeight)
	{
		const UWorld* World = GetWorld();
		if (!World || !UpdatedPrimitive)
		{
			return false;
		}

		const FCollisionShape ClearanceShape = FCollisionShape::MakeCapsule(
			FMath::Max(Capsule->GetScaledCapsuleRadius() - MomentumMovement::StandUpClearanceTolerance, 1.0f),
			FMath::Max(NewUnscaledHalfHeight * ShapeScale - MomentumMovement::StandUpClearanceTolerance, 1.0f));

		FCollisionQueryParams QueryParams(MomentumMovement::StandUpClearanceTag, false, GetOwner());
		FCollisionResponseParams ResponseParams;
		UpdatedPrimitive->InitSweepCollisionParams(QueryParams, ResponseParams);

		const FVector TestLocation = UpdatedComponent->GetComponentLocation() + Translation;
		if (World->OverlapBlockingTestByChannel(
				TestLocation,
				FQuat::Identity,
				UpdatedPrimitive->GetCollisionObjectType(),
				ClearanceShape,
				QueryParams,
				ResponseParams))
		{
			return false;
		}
	}

	Capsule->SetCapsuleHalfHeight(NewUnscaledHalfHeight, true);

	if (!Translation.IsZero())
	{
		UpdatedComponent->MoveComponent(Translation, UpdatedComponent->GetComponentQuat(), false, nullptr, MOVECOMP_NoFlags, ETeleportType::TeleportPhysics);
	}

	return true;
}

// -------------------------------------------------------------------------------------------------
// Jumping
// -------------------------------------------------------------------------------------------------

bool UMomentumMovementComponent::CanJump() const
{
	if (!bJumpEnabled || !UpdatedComponent)
	{
		return false;
	}

	// Coyote time keeps a ground jump available for a moment after walking off a ledge.
	if (IsGrounded() || CoyoteTimeRemaining > 0.0f)
	{
		return true;
	}

	return IsAirborne() && AirJumpsUsed < MaxAirJumps;
}

bool UMomentumMovementComponent::DoJump()
{
	if (!CanJump())
	{
		return false;
	}

	const bool bFromGround = IsGrounded() || CoyoteTimeRemaining > 0.0f;
	if (!bFromGround)
	{
		++AirJumpsUsed;
	}

	// Replaces vertical velocity rather than adding to it, so jump height stays predictable no
	// matter what the pawn was doing beforehand. Horizontal momentum is untouched.
	Velocity.Z = JumpZVelocity;
	JumpBufferTimeRemaining = 0.0f;

	SetMovementMode(EMomentumMovementMode::Airborne);

	// Must follow SetMovementMode, which reopens the coyote window on a Grounded to Airborne change.
	CoyoteTimeRemaining = 0.0f;

	OnJumpedDelegate.Broadcast(bFromGround ? 0 : AirJumpsUsed);
	return true;
}

void UMomentumMovementComponent::TryJumpFromInput()
{
	const bool bBufferedPress = JumpBufferTimeRemaining > 0.0f;
	const bool bAutoHopping = bAutoHop && bJumpInputHeld && IsGrounded();

	if (bBufferedPress || bAutoHopping)
	{
		DoJump();
	}
}

// -------------------------------------------------------------------------------------------------
// Mode changes
// -------------------------------------------------------------------------------------------------

void UMomentumMovementComponent::SetMovementMode(const EMomentumMovementMode NewMode, const uint8 NewCustomMode)
{
	const uint8 ResolvedCustomMode = (NewMode == EMomentumMovementMode::Custom) ? NewCustomMode : 0;
	if (MovementMode == NewMode && CustomMovementMode == ResolvedCustomMode)
	{
		return;
	}

	const EMomentumMovementMode PreviousMode = MovementMode;
	const uint8 PreviousCustomMode = CustomMovementMode;

	MovementMode = NewMode;
	CustomMovementMode = ResolvedCustomMode;
	TimeInCurrentMode = 0.0f;

	OnMovementModeChanged(PreviousMode, PreviousCustomMode);
	OnMovementModeChangedDelegate.Broadcast(PreviousMode, MovementMode);
}

void UMomentumMovementComponent::OnMovementModeChanged(const EMomentumMovementMode PreviousMode, const uint8 PreviousCustomMode)
{
	if (MovementMode == EMomentumMovementMode::Grounded)
	{
		AirJumpsUsed = 0;
		CoyoteTimeRemaining = 0.0f;
	}
	else if (MovementMode == EMomentumMovementMode::Airborne && PreviousMode == EMomentumMovementMode::Grounded)
	{
		// Leaving the ground opens the coyote window. DoJump closes it again immediately.
		CoyoteTimeRemaining = CoyoteTime;
	}
}

// -------------------------------------------------------------------------------------------------
// Input and queries
// -------------------------------------------------------------------------------------------------

void UMomentumMovementComponent::SetJumpInput(const bool bPressed)
{
	if (bPressed && !bJumpInputHeld)
	{
		// Remembering the press lets a jump entered just before touchdown still fire on landing.
		JumpBufferTimeRemaining = JumpInputBufferTime;
	}

	bJumpInputHeld = bPressed;
}

void UMomentumMovementComponent::SetSprintInput(const bool bPressed)
{
	bWantsToSprint = bPressed;
}

void UMomentumMovementComponent::SetCrouchInput(const bool bPressed)
{
	bWantsToCrouch = bPressed;
}

void UMomentumMovementComponent::AddImpulse(const FVector Impulse, const bool bOverrideVerticalVelocity)
{
	if (Impulse.IsNearlyZero())
	{
		return;
	}

	if (bOverrideVerticalVelocity)
	{
		Velocity.X += Impulse.X;
		Velocity.Y += Impulse.Y;
		Velocity.Z = Impulse.Z;
	}
	else
	{
		Velocity += Impulse;
	}

	// An upward impulse has to break ground contact, or the next step would snap the pawn straight back down.
	if (Velocity.Z > 0.0f && IsGrounded())
	{
		SetMovementMode(EMomentumMovementMode::Airborne);
	}
}

float UMomentumMovementComponent::GetMaxSpeed() const
{
	if (bIsCrouching)
	{
		return MaxCrouchSpeed;
	}

	return IsSprinting() ? MaxSprintSpeed : MaxWalkSpeed;
}

bool UMomentumMovementComponent::IsSprinting() const
{
	if (!bWantsToSprint || bIsCrouching || WishDirection.IsNearlyZero())
	{
		return false;
	}

	if (bSprintRequiresForwardInput && UpdatedComponent)
	{
		const FVector Facing = UpdatedComponent->GetForwardVector().GetSafeNormal2D();
		if (FVector::DotProduct(WishDirection, Facing) < SprintForwardInputThreshold)
		{
			return false;
		}
	}

	return true;
}

bool UMomentumMovementComponent::IsMovingOnGround() const
{
	return IsGrounded();
}

bool UMomentumMovementComponent::IsFalling() const
{
	return IsAirborne();
}

bool UMomentumMovementComponent::IsCrouching() const
{
	return bIsCrouching;
}

// -------------------------------------------------------------------------------------------------
// Debug
// -------------------------------------------------------------------------------------------------

void UMomentumMovementComponent::DrawDebugInfo() const
{
#if !UE_BUILD_SHIPPING
	if (MomentumMovement::ShowDebug == 0 || !UpdatedComponent)
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (GEngine)
	{
		const UEnum* ModeEnum = StaticEnum<EMomentumMovementMode>();
		const FString ModeName = ModeEnum ? ModeEnum->GetNameStringByValue(static_cast<int64>(MovementMode)) : TEXT("Unknown");

		GEngine->AddOnScreenDebugMessage(
			static_cast<uint64>(GetUniqueID()),
			0.0f,
			FColor::Cyan,
			FString::Printf(
				TEXT("[Momentum] %s | Ground %.0f cm/s | Vertical %.0f cm/s | Air jumps %d/%d%s%s"),
				*ModeName,
				GetGroundSpeed(),
				Velocity.Z,
				AirJumpsUsed,
				MaxAirJumps,
				IsSprinting() ? TEXT(" | Sprinting") : TEXT(""),
				bIsCrouching ? TEXT(" | Crouched") : TEXT("")));
	}

	const FVector Origin = UpdatedComponent->GetComponentLocation();

	// Velocity scaled down so a 1000 cm/s run draws a readable 250 cm line.
	DrawDebugLine(World, Origin, Origin + Velocity * 0.25f, FColor::Green, false, -1.0f, 0, 2.0f);

	if (!WishDirection.IsNearlyZero())
	{
		DrawDebugLine(World, Origin, Origin + WishDirection * 100.0f, FColor::Cyan, false, -1.0f, 0, 2.0f);
	}

	if (CurrentFloor.bBlockingHit)
	{
		DrawDebugPoint(World, CurrentFloor.HitResult.ImpactPoint, 10.0f, CurrentFloor.bWalkableFloor ? FColor::Green : FColor::Red, false, -1.0f);
	}
#endif
}