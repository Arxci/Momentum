#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PawnMovementComponent.h"
#include "Momentum/Character/MomentumMovementTypes.h"

#include "MomentumCharacterMovementComponent.generated.h"

class UCapsuleComponent;

/**
 * Momentum preserving movement for first person movement shooters.
 *
 * Deliberately built on UPawnMovementComponent rather than UCharacterMovementComponent: it keeps the
 * useful primitives (UpdatedComponent, swept moves, surface sliding, input accumulation) and drops
 * root motion, networked prediction, swimming and flying.
 *
 * Acceleration uses the Quake/Source model: velocity is only accelerated up to the wish speed along
 * the wish direction, and ground friction is what brings it back down. Speed above the walk limit is
 * therefore never hard clamped, so momentum carried into a jump survives the jump, and steering in the
 * air while at the wish-speed cap produces air strafing.
 *
 * Usage:
 *   1. Give the owning pawn a UCapsuleComponent root and point UpdatedComponent at it.
 *   2. Feed direction with APawn::AddMovementInput (world space, magnitude 0..1).
 *   3. Feed state with SetJumpInput / SetSprintInput / SetCrouchInput.
 *
 * Extending: add a traversal state by calling SetMovementMode(Custom, MyModeId) and handling MyModeId
 * in an override of PhysCustom. Nothing in the base loop needs to change.
 *
 * This component is intentionally not replicated.
 */
UCLASS(ClassGroup = (Momentum), meta = (BlueprintSpawnableComponent, DisplayName = "Momentum Movement Component"))
class MOMENTUM_API UMomentumMovementComponent : public UPawnMovementComponent
{
	GENERATED_BODY()

public:
	UMomentumMovementComponent();

	//~ Begin UActorComponent interface
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	//~ End UActorComponent interface

	//~ Begin UMovementComponent interface
	virtual void SetUpdatedComponent(USceneComponent* NewUpdatedComponent) override;
	virtual float GetMaxSpeed() const override;
	virtual bool IsMovingOnGround() const override;
	virtual bool IsFalling() const override;
	virtual bool IsCrouching() const override;
	//~ End UMovementComponent interface

	// ---------------------------------------------------------------------------------------------
	// Input API. Call these from a pawn, a controller or an AI task. The component never reads input
	// devices itself and never looks at the owning pawn's type.
	// ---------------------------------------------------------------------------------------------

	/** Hold state of the jump button. Pressing opens the jump input buffer; releasing closes auto hop. */
	UFUNCTION(BlueprintCallable, Category = "Momentum|Movement|Input")
	void SetJumpInput(bool bPressed);

	/** Hold state of the sprint button. Sprint is suppressed while crouched. */
	UFUNCTION(BlueprintCallable, Category = "Momentum|Movement|Input")
	void SetSprintInput(bool bPressed);

	/** Hold state of the crouch button. The capsule resizes on the next simulation step. */
	UFUNCTION(BlueprintCallable, Category = "Momentum|Movement|Input")
	void SetCrouchInput(bool bPressed);

	// ---------------------------------------------------------------------------------------------
	// State queries
	// ---------------------------------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Momentum|Movement|State")
	EMomentumMovementMode GetMovementMode() const { return MovementMode; }

	UFUNCTION(BlueprintPure, Category = "Momentum|Movement|State")
	uint8 GetCustomMovementMode() const { return CustomMovementMode; }

	UFUNCTION(BlueprintPure, Category = "Momentum|Movement|State")
	bool IsGrounded() const { return MovementMode == EMomentumMovementMode::Grounded; }

	UFUNCTION(BlueprintPure, Category = "Momentum|Movement|State")
	bool IsAirborne() const { return MovementMode == EMomentumMovementMode::Airborne; }

	/** True when the sprint button is held and the current input actually qualifies for sprinting. */
	UFUNCTION(BlueprintPure, Category = "Momentum|Movement|State")
	bool IsSprinting() const;

	/** Horizontal speed in cm/s. This is the number to drive a speedometer or FOV kick with. */
	UFUNCTION(BlueprintPure, Category = "Momentum|Movement|State")
	float GetGroundSpeed() const { return Velocity.Size2D(); }

	/** Normalised world space direction the pawn is asking to move in. Zero when there is no input. */
	UFUNCTION(BlueprintPure, Category = "Momentum|Movement|State")
	FVector GetWishDirection() const { return WishDirection; }

	/** Velocity recorded on the frame the pawn last touched down. Use for landing effects and fall damage. */
	UFUNCTION(BlueprintPure, Category = "Momentum|Movement|State")
	FVector GetLastLandingVelocity() const { return LastLandingVelocity; }

	/** Seconds spent in the current movement mode. */
	UFUNCTION(BlueprintPure, Category = "Momentum|Movement|State")
	float GetTimeInCurrentMode() const { return TimeInCurrentMode; }

	/** Result of the most recent floor sweep. */
	const FMomentumFloorResult& GetCurrentFloor() const { return CurrentFloor; }

	/** Cosine of WalkableFloorAngle, pre-computed. A surface is walkable when its normal Z is at least this. */
	UFUNCTION(BlueprintPure, Category = "Momentum|Movement|State")
	float GetWalkableFloorZ() const { return WalkableFloorZ; }

	/** True when a jump would succeed right now, accounting for coyote time and remaining air jumps. */
	UFUNCTION(BlueprintPure, Category = "Momentum|Movement|Jump")
	bool CanJump() const;

	// ---------------------------------------------------------------------------------------------
	// Commands
	// ---------------------------------------------------------------------------------------------

	/** Jump immediately, bypassing the input buffer. Returns false if CanJump() was false. */
	UFUNCTION(BlueprintCallable, Category = "Momentum|Movement|Jump")
	bool DoJump();

	/** Add an instantaneous velocity change, for launch pads, explosions and grapples. */
	UFUNCTION(BlueprintCallable, Category = "Momentum|Movement")
	void AddImpulse(FVector Impulse, bool bOverrideVerticalVelocity = false);

	/** Change movement mode. CustomMode is only meaningful when NewMode is Custom. */
	UFUNCTION(BlueprintCallable, Category = "Momentum|Movement")
	void SetMovementMode(EMomentumMovementMode NewMode, uint8 NewCustomMode = 0);

	// ---------------------------------------------------------------------------------------------
	// Events
	// ---------------------------------------------------------------------------------------------

	UPROPERTY(BlueprintAssignable, Category = "Momentum|Movement|Events")
	FMomentumMovementModeChangedSignature OnMovementModeChangedDelegate;

	UPROPERTY(BlueprintAssignable, Category = "Momentum|Movement|Events")
	FMomentumLandedSignature OnLandedDelegate;

	UPROPERTY(BlueprintAssignable, Category = "Momentum|Movement|Events")
	FMomentumJumpedSignature OnJumpedDelegate;

	UPROPERTY(BlueprintAssignable, Category = "Momentum|Movement|Events")
	FMomentumCrouchStateChangedSignature OnCrouchStateChangedDelegate;

	// ---------------------------------------------------------------------------------------------
	// Tuning: ground
	// ---------------------------------------------------------------------------------------------

	/** Target speed with no modifiers held. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Ground", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "CentimetersPerSecond"))
	float MaxWalkSpeed = 600.0f;

	/** Target speed while sprinting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Ground", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "CentimetersPerSecond"))
	float MaxSprintSpeed = 1000.0f;

	/** Target speed while crouched. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Ground", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "CentimetersPerSecond"))
	float MaxCrouchSpeed = 300.0f;

	/** How hard the pawn accelerates toward the wish speed on the ground. Higher is snappier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Ground", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float GroundAcceleration = 15.0f;

	/** How hard the ground bleeds off speed. This, not a hard clamp, is what limits top speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Ground", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float GroundFriction = 8.0f;

	/** Below this speed, friction is applied as if the pawn were moving at this speed, so it stops crisply. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Ground", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "CentimetersPerSecond"))
	float StopSpeed = 200.0f;

	/** Sprinting requires the wish direction to be roughly forward. Turn off for omnidirectional sprint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Ground")
	bool bSprintRequiresForwardInput = true;

	/** Minimum dot product between wish direction and facing for bSprintRequiresForwardInput. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Ground", meta = (ClampMin = "-1.0", ClampMax = "1.0", EditCondition = "bSprintRequiresForwardInput"))
	float SprintForwardInputThreshold = 0.5f;

	// ---------------------------------------------------------------------------------------------
	// Tuning: air
	// ---------------------------------------------------------------------------------------------

	/** How hard the pawn accelerates in the air. Higher values make air strafing more responsive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Air", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float AirAcceleration = 25.0f;

	/**
	 * Wish speed cap while airborne, before analogue input scaling. This is the heart of the air
	 * strafe: acceleration is only added while the speed along the wish direction is below this
	 * value, so steering the wish direction sideways keeps adding speed without ever raising the
	 * projected speed past the cap.
	 * Small values (50-100) give classic strafe gain; large values turn it into plain air control.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Air", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "CentimetersPerSecond"))
	float MaxAirWishSpeed = 80.0f;

	/** Multiplier on world gravity. Movement shooters usually want more than 1 for a snappy arc. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Air", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float GravityScale = 2.0f;

	/** Maximum downward speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Air", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "CentimetersPerSecond"))
	float TerminalVelocity = 4000.0f;

	// ---------------------------------------------------------------------------------------------
	// Tuning: jump
	// ---------------------------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Jump")
	bool bJumpEnabled = true;

	/** Upward speed applied on jump. Replaces vertical velocity rather than adding to it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Jump", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "CentimetersPerSecond"))
	float JumpZVelocity = 700.0f;

	/** Extra jumps allowed after leaving the ground. 0 is a single jump, 1 is a double jump. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Jump", meta = (ClampMin = "0", UIMin = "0"))
	int32 MaxAirJumps = 0;

	/** Grace period after walking off a ledge during which a ground jump still works. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Jump", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "Seconds"))
	float CoyoteTime = 0.12f;

	/** How long a jump press is remembered, so a press just before landing still fires on touchdown. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Jump", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "Seconds"))
	float JumpInputBufferTime = 0.15f;

	/** Holding jump re-jumps the instant the pawn lands, which makes chaining hops far easier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Jump")
	bool bAutoHop = false;

	// ---------------------------------------------------------------------------------------------
	// Tuning: crouch
	// ---------------------------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Crouch")
	bool bCanCrouch = true;

	/** Unscaled capsule half height while crouched. The standing value is read from the capsule itself. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Crouch", meta = (ClampMin = "1.0", UIMin = "1.0", Units = "Centimeters"))
	float CrouchedHalfHeight = 44.0f;

	// ---------------------------------------------------------------------------------------------
	// Tuning: collision
	// ---------------------------------------------------------------------------------------------

	/** Steepest surface, in degrees from horizontal, that still counts as walkable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Collision", meta = (ClampMin = "0.0", ClampMax = "89.0", UIMin = "0.0", UIMax = "89.0", Units = "Degrees"))
	float WalkableFloorAngle = 46.0f;

	/** Tallest obstruction the pawn will step over instead of colliding with. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Collision", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "Centimeters"))
	float MaxStepHeight = 45.0f;

	/** How far below the capsule a grounded pawn will snap to stay attached, e.g. walking down stairs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Collision", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "Centimeters"))
	float GroundSnapDistance = 45.0f;

	// ---------------------------------------------------------------------------------------------
	// Tuning: simulation
	// ---------------------------------------------------------------------------------------------

	/** Split long frames into fixed steps so behaviour stays stable when the frame rate drops. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Simulation", AdvancedDisplay)
	bool bEnableSubstepping = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Simulation", AdvancedDisplay, meta = (ClampMin = "0.002", UIMin = "0.002", Units = "Seconds", EditCondition = "bEnableSubstepping"))
	float MaxSimulationTimeStep = 0.02f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Momentum|Simulation", AdvancedDisplay, meta = (ClampMin = "1", UIMin = "1", EditCondition = "bEnableSubstepping"))
	int32 MaxSimulationIterations = 8;

protected:
	//~ Begin UActorComponent interface
	virtual void BeginPlay() override;
	//~ End UActorComponent interface

	// --- Simulation. Override these to add behaviour. ---

	/** One simulation step: advance timers, resolve state, then run the mode's physics. */
	virtual void PerformMovement(float DeltaTime);

	/** Ground physics: friction, acceleration, slope-projected movement, ground snapping. */
	virtual void PhysGrounded(float DeltaTime);

	/** Air physics: gravity, air acceleration, swept movement with surface sliding. */
	virtual void PhysAirborne(float DeltaTime);

	/** Physics for EMomentumMovementMode::Custom. Empty by default; this is the traversal hook. */
	virtual void PhysCustom(float DeltaTime, uint8 CustomMode);

	/** Called after MovementMode changes, before the delegate is broadcast. */
	virtual void OnMovementModeChanged(EMomentumMovementMode PreviousMode, uint8 PreviousCustomMode);

	/** Called when a swept move is blocked. Default behaviour clips velocity into the surface plane. */
	virtual void HandleBlockingImpact(const FHitResult& Hit);

	// --- Shared helpers ---

	/**
	 * Quake/Source acceleration. Adds speed along WishDir only while the velocity already projected
	 * onto WishDir is below WishSpeed, which is what allows momentum above WishSpeed to survive.
	 */
	void ApplyAcceleration(const FVector& WishDir, float WishSpeed, float Acceleration, float DeltaTime);

	/** Horizontal friction, with StopSpeed applied as a floor so low speeds decay quickly. */
	void ApplyFriction(float DeltaTime);

	/** Sweep the capsule straight down and classify what it finds. */
	bool FindFloor(const FVector& CapsuleLocation, float SweepDistance, FMomentumFloorResult& OutFloor) const;

	/** True if the hit surface is flat enough to stand on and is not flagged as un-steppable. */
	bool IsWalkable(const FHitResult& Hit) const;

	/** Move along the current floor plane so slopes neither launch the pawn nor drag it into the ground. */
	void MoveAlongFloor(const FVector& Delta);

	/** Up, forward, down. Returns true if the pawn ended on a walkable surface above where it started. */
	bool TryStepUp(const FHitResult& Hit, const FVector& RemainingDelta);

	/** Pull the pawn back down onto its floor after a grounded move, so stairs and ramps do not launch it. */
	void MaintainGroundContact();

	/** Resize the capsule to match the crouch input, refusing to stand up when there is no headroom. */
	void UpdateCrouchState();

	/** Consume buffered jump input if a jump is currently possible. */
	void TryJumpFromInput();

	/** The capsule this component moves, or null if the updated component is not a capsule. */
	UCapsuleComponent* GetCapsuleComponent() const;

private:
	void UpdateTimers(float DeltaTime);
	void UpdateFloorAndMode();
	void Land(const FHitResult& FloorHit);
	bool ResizeCapsule(float NewUnscaledHalfHeight, bool bMaintainBaseLocation);

	/** Draws state, speed and floor diagnostics. Compiled out in shipping. */
	void DrawDebugInfo() const;

	/** Current high level state. */
	EMomentumMovementMode MovementMode = EMomentumMovementMode::None;

	/** Project defined sub-mode, only meaningful while MovementMode is Custom. */
	uint8 CustomMovementMode = 0;

	/** World space direction requested this frame, normalised. */
	FVector WishDirection = FVector::ZeroVector;

	/** Magnitude of this frame's input, 0..1, so analogue sticks scale the wish speed. */
	float WishInputScale = 0.0f;

	/** Most recent floor sweep. */
	FMomentumFloorResult CurrentFloor;

	/** Velocity captured on the frame the pawn last landed. */
	FVector LastLandingVelocity = FVector::ZeroVector;

	/** Standing half height, cached from the capsule when the updated component is assigned. */
	float StandingHalfHeight = 88.0f;

	/** Cosine of WalkableFloorAngle, refreshed whenever the angle changes. */
	float WalkableFloorZ = 0.695f;

	float TimeInCurrentMode = 0.0f;
	float JumpBufferTimeRemaining = 0.0f;
	float CoyoteTimeRemaining = 0.0f;
	int32 AirJumpsUsed = 0;

	bool bJumpInputHeld = false;
	bool bWantsToSprint = false;
	bool bWantsToCrouch = false;
	bool bIsCrouching = false;
};