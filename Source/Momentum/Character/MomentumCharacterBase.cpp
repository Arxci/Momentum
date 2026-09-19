#include "Momentum/Character/MomentumCharacterBase.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Momentum/Character/MomentumCharacterMovementComponent.h"

namespace MomentumCharacter
{
	constexpr float DefaultCapsuleRadius = 34.0f;
	constexpr float DefaultCapsuleHalfHeight = 88.0f;

	/** Skeletal meshes are authored facing +Y, so the body is yawed to face the capsule's +X. */
	constexpr float MeshYawCorrection = -90.0f;
}

AMomentumCharacterBase::AMomentumCharacterBase()
{
	// Ticks to interpolate the eye height. PostInitializeComponents orders this after the movement
	// simulation so the view reflects the position the pawn actually ended the frame at.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	CapsuleComponent = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CapsuleComponent"));
	CapsuleComponent->InitCapsuleSize(MomentumCharacter::DefaultCapsuleRadius, MomentumCharacter::DefaultCapsuleHalfHeight);
	CapsuleComponent->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	CapsuleComponent->SetShouldUpdatePhysicsVolume(true);
	CapsuleComponent->SetCanEverAffectNavigation(false);
	// Other pawns must not use this capsule as a step, or they would climb each other.
	CapsuleComponent->CanCharacterStepUpOn = ECB_No;
	RootComponent = CapsuleComponent;

	ViewPivot = CreateDefaultSubobject<USceneComponent>(TEXT("ViewPivot"));
	ViewPivot->SetupAttachment(CapsuleComponent);
	ViewPivot->SetRelativeLocation(FVector(0.0f, 0.0f, StandingViewHeight - MomentumCharacter::DefaultCapsuleHalfHeight));

	CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("CameraComponent"));
	CameraComponent->SetupAttachment(ViewPivot);
	// The capsule follows the controller's yaw and the camera adds its pitch, which keeps the
	// movement component's forward vector aligned with where the player is looking.
	CameraComponent->bUsePawnControlRotation = true;

	MeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("MeshComponent"));
	MeshComponent->SetupAttachment(CapsuleComponent);
	MeshComponent->SetRelativeLocationAndRotation(
		FVector(0.0f, 0.0f, -MomentumCharacter::DefaultCapsuleHalfHeight),
		FRotator(0.0f, MomentumCharacter::MeshYawCorrection, 0.0f));
	// The capsule is the only collider; the mesh is visual only.
	MeshComponent->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	MeshComponent->SetGenerateOverlapEvents(false);

	MovementComponent = CreateDefaultSubobject<UMomentumMovementComponent>(TEXT("MovementComponent"));
	// Assigned directly rather than through SetUpdatedComponent: the component resolves it properly
	// in OnRegister, once the world exists.
	MovementComponent->UpdatedComponent = CapsuleComponent;

	// First person aiming: yaw turns the body, pitch is camera only.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;

	BaseEyeHeight = StandingViewHeight - MomentumCharacter::DefaultCapsuleHalfHeight;
}

void AMomentumCharacterBase::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// Interpolate the view after this frame's movement has been simulated, not before it.
	if (MovementComponent)
	{
		AddTickPrerequisiteComponent(MovementComponent);
	}
}

void AMomentumCharacterBase::BeginPlay()
{
	Super::BeginPlay();

	CurrentViewHeight = StandingViewHeight;
	UpdateViewHeight(0.0f);
}

void AMomentumCharacterBase::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UpdateViewHeight(DeltaSeconds);
}

void AMomentumCharacterBase::UpdateViewHeight(const float DeltaSeconds)
{
	if (!ViewPivot || !CapsuleComponent)
	{
		return;
	}

	const bool bCrouching = MovementComponent && MovementComponent->IsCrouching();
	const float TargetViewHeight = bCrouching ? CrouchedViewHeight : StandingViewHeight;

	CurrentViewHeight = (ViewHeightInterpSpeed > 0.0f && DeltaSeconds > 0.0f)
		? FMath::FInterpTo(CurrentViewHeight, TargetViewHeight, DeltaSeconds, ViewHeightInterpSpeed)
		: TargetViewHeight;

	// The eye is placed from the feet upward. The capsule half height changes instantly on crouch,
	// and subtracting it here cancels that jump exactly, leaving only this smoothed value to move
	// the view. That is why no separate crouch offset smoothing is needed.
	// Named to avoid shadowing AActor::PivotOffset.
	const float ViewPivotRelativeZ = CurrentViewHeight - CapsuleComponent->GetScaledCapsuleHalfHeight();

	ViewPivot->SetRelativeLocation(FVector(0.0f, 0.0f, ViewPivotRelativeZ));
	BaseEyeHeight = ViewPivotRelativeZ;
}

UPawnMovementComponent* AMomentumCharacterBase::GetMovementComponent() const
{
	return MovementComponent;
}

FVector AMomentumCharacterBase::GetPawnViewLocation() const
{
	// Keep AI perception, aim traces and spawn points on the actual camera rather than on
	// APawn's BaseEyeHeight approximation.
	return CameraComponent ? CameraComponent->GetComponentLocation() : Super::GetPawnViewLocation();
}

// -------------------------------------------------------------------------------------------------
// Intent API
// -------------------------------------------------------------------------------------------------

void AMomentumCharacterBase::AddMoveInput(const FVector2D MoveInput)
{
	if (MoveInput.IsNearlyZero())
	{
		return;
	}

	// Built from the control rotation's yaw only, so looking up or down never shortens or tilts the
	// movement input.
	const FRotationMatrix YawMatrix(FRotator(0.0f, GetControlRotation().Yaw, 0.0f));

	AddMovementInput(YawMatrix.GetUnitAxis(EAxis::X), MoveInput.Y);
	AddMovementInput(YawMatrix.GetUnitAxis(EAxis::Y), MoveInput.X);
}

void AMomentumCharacterBase::AddLookInput(const FVector2D LookInput)
{
	AddControllerYawInput(LookInput.X);
	AddControllerPitchInput(LookInput.Y);
}

void AMomentumCharacterBase::StartJump()
{
	if (MovementComponent)
	{
		MovementComponent->SetJumpInput(true);
	}
}

void AMomentumCharacterBase::StopJump()
{
	if (MovementComponent)
	{
		MovementComponent->SetJumpInput(false);
	}
}

void AMomentumCharacterBase::SetSprinting(const bool bEnabled)
{
	if (MovementComponent)
	{
		MovementComponent->SetSprintInput(bEnabled);
	}
}

void AMomentumCharacterBase::SetCrouching(const bool bEnabled)
{
	if (MovementComponent)
	{
		MovementComponent->SetCrouchInput(bEnabled);
	}
}

void AMomentumCharacterBase::ToggleCrouch()
{
	if (MovementComponent)
	{
		MovementComponent->SetCrouchInput(!MovementComponent->IsCrouching());
	}
}